#include "skr_pico_mount.h"

#include <cstring>
#include <cmath>
#include <memory>
#include <termios.h>

#include <indicom.h>   // tty_read, tty_write_string, etc.

static std::unique_ptr<SKRPicoMount> mount(new SKRPicoMount());

SKRPicoMount::SKRPicoMount()
{
    // TELESCOPE_HAS_PULSE_GUIDING: required for PHD2's INDI mount guiding
    // TELESCOPE_CAN_ABORT / CAN_SYNC / CAN_GOTO: basic mount capabilities
    // TELESCOPE_HAS_TRACK_MODE / TRACK_RATE: sidereal + custom rate support
    SetTelescopeCapability(
        TELESCOPE_CAN_GOTO |
        TELESCOPE_CAN_SYNC |
        TELESCOPE_CAN_ABORT |
        TELESCOPE_HAS_TRACK_MODE |
        TELESCOPE_HAS_TRACK_RATE |
        TELESCOPE_HAS_PULSE_GUIDING,
        4 /* slew rate count, unused meaningfully here but required */
    );

    setTelescopeConnection(CONNECTION_SERIAL);
}

const char *SKRPicoMount::getDefaultName()
{
    return "SKR Pico Cycloidal Mount";
}

bool SKRPicoMount::initProperties()
{
    INDI::Telescope::initProperties();
    INDI::GuiderInterface::initGuiderProperties(getDeviceName(), MOTION_TAB);

    addAuxControls();

    // Reasonable default baud for the serial connection plugin
    serialConnection->setDefaultBaudRate(Connection::Serial::B_115200);

    return true;
}

bool SKRPicoMount::updateProperties()
{
    INDI::Telescope::updateProperties();

    if (isConnected())
    {
        defineProperty(GuideNSNP);
        defineProperty(GuideWENP);
    }
    else
    {
        deleteProperty(GuideNSNP.name);
        deleteProperty(GuideWENP.name);
    }

    return true;
}

// ---------------------------------------------------------------------
// Connection handshake - confirm the SKR Pico firmware is alive and
// responding, using the PING command from its serial protocol.
// ---------------------------------------------------------------------
bool SKRPicoMount::Handshake()
{
    PortFD = serialConnection->getPortFD();

    char response[32] = {0};
    if (!sendCommand("PING\n", response, sizeof(response)))
    {
        LOG_ERROR("No response from SKR Pico firmware (PING failed).");
        return false;
    }

    if (strncmp(response, "OK", 2) != 0)
    {
        LOGF_ERROR("Unexpected handshake response: %s", response);
        return false;
    }

    LOG_INFO("SKR Pico mount firmware responded to PING - connected.");

    // Enable both drivers and set microstepping to match our constants
    sendAxisCommand("RA", "EN", 1);
    sendAxisCommand("DEC", "EN", 1);
    sendAxisCommand("RA", "MS", MICROSTEPS);
    sendAxisCommand("DEC", "MS", MICROSTEPS);

    return true;
}

// ---------------------------------------------------------------------
// Low-level serial helpers
// ---------------------------------------------------------------------
bool SKRPicoMount::sendCommand(const char *cmd, char *response, size_t responseLen)
{
    int nbytes_written = 0, nbytes_read = 0, rc = 0;

    tcflush(PortFD, TCIOFLUSH);

    rc = tty_write_string(PortFD, cmd, &nbytes_written);
    if (rc != TTY_OK)
    {
        char errmsg[MAXRBUF];
        tty_error_msg(rc, errmsg, MAXRBUF);
        LOGF_ERROR("Serial write error: %s", errmsg);
        return false;
    }

    rc = tty_nread_section(PortFD, response, responseLen - 1, '\n', 2 /*timeout sec*/, &nbytes_read);
    if (rc != TTY_OK)
    {
        char errmsg[MAXRBUF];
        tty_error_msg(rc, errmsg, MAXRBUF);
        LOGF_ERROR("Serial read error: %s", errmsg);
        return false;
    }
    response[nbytes_read] = '\0';

    return true;
}

bool SKRPicoMount::sendAxisCommand(const char *axis, const char *action, double param)
{
    char cmd[64];
    // integer-friendly formatting for step counts/rates; RATE also
    // accepts fractional steps/sec if you want finer than integer rates
    snprintf(cmd, sizeof(cmd), "%s:%s:%.3f\n", axis, action, param);

    char response[32] = {0};
    if (!sendCommand(cmd, response, sizeof(response)))
        return false;

    if (strncmp(response, "OK", 2) != 0)
    {
        LOGF_WARN("Axis command '%s' got non-OK response: %s", cmd, response);
        return false;
    }
    return true;
}

bool SKRPicoMount::sendAxisCommandStr(const char *axis, const char *action, const char *param)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s:%s:%s\n", axis, action, param);

    char response[32] = {0};
    if (!sendCommand(cmd, response, sizeof(response)))
        return false;

    return strncmp(response, "OK", 2) == 0;
}

// ---------------------------------------------------------------------
// Coordinate / rate math
// ---------------------------------------------------------------------
double SKRPicoMount::arcsecPerSecToStepsPerSec(double arcsecPerSec, double gearRatio, int stepsPerRev, int microsteps)
{
    // 1 full output-shaft revolution = 360*3600 arcsec
    double stepsPerOutputRev = stepsPerRev * microsteps * gearRatio;
    double arcsecPerStep = (360.0 * 3600.0) / stepsPerOutputRev;
    return arcsecPerSec / arcsecPerStep;
}

long SKRPicoMount::degreesToSteps(double degrees, double gearRatio, int stepsPerRev, int microsteps)
{
    double stepsPerOutputRev = stepsPerRev * microsteps * gearRatio;
    return (long)lround((degrees / 360.0) * stepsPerOutputRev);
}

// ---------------------------------------------------------------------
// ReadScopeStatus - this firmware has NO encoders, so "status" here is
// really just re-reporting our commanded/integrated position, not a
// true measured one. See README for why this matters and what to do
// about it (a real build wants at least a rough position sanity check
// via plate-solving/sync, not just dead-reckoning forever).
// ---------------------------------------------------------------------
bool SKRPicoMount::ReadScopeStatus()
{
    // TODO: if tracking, currentRA should be advancing with sidereal
    // time between polls. A more complete driver computes elapsed time
    // since last ReadScopeStatus() call and advances currentRA
    // accordingly here, rather than treating it as static during
    // tracking. Left as a stub - see README "Known gaps".

    NewRaDec(currentRA, currentDEC);
    return true;
}

// ---------------------------------------------------------------------
// Goto - move to a target RA/Dec via relative step MOVE commands.
// NOTE: since there's no absolute position feedback, this is only as
// accurate as our running dead-reckoned currentRA/currentDEC - drift
// will accumulate without periodic Sync() calls (e.g. after a
// plate-solve).
// ---------------------------------------------------------------------
bool SKRPicoMount::Goto(double ra, double dec)
{
    double deltaRAHours = ra - currentRA;
    double deltaDECDeg  = dec - currentDEC;

    double deltaRADeg = deltaRAHours * 15.0; // hours -> degrees

    long raSteps  = degreesToSteps(deltaRADeg, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    long decSteps = degreesToSteps(deltaDECDeg, DEC_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);

    LOGF_INFO("Goto: dRA=%.4fdeg (%ld steps) dDEC=%.4fdeg (%ld steps)", deltaRADeg, raSteps, deltaDECDeg, decSteps);

    if (raSteps != 0)
        sendAxisCommand("RA", "MOVE", (double)raSteps);
    if (decSteps != 0)
        sendAxisCommand("DEC", "MOVE", (double)decSteps);

    // Optimistically assume the move will complete; a more complete
    // driver would track "is this axis still moving" (e.g. by having
    // the firmware report remaining steps, or by timing the expected
    // move duration) rather than assuming instant completion here.
    currentRA = ra;
    currentDEC = dec;

    TrackState = SCOPE_SLEWING;
    return true;
}

// ---------------------------------------------------------------------
// Sync - tell the driver "you are actually pointed at this RA/Dec now"
// without moving. Since we have no real position sensor, this just
// resets our dead-reckoning baseline - this is the main way you correct
// accumulated drift (e.g. after a polar-alignment plate-solve, or after
// manually centering a star).
// ---------------------------------------------------------------------
bool SKRPicoMount::Sync(double ra, double dec)
{
    currentRA = ra;
    currentDEC = dec;
    LOGF_INFO("Sync: position reset to RA=%.4f DEC=%.4f", ra, dec);
    return true;
}

bool SKRPicoMount::Abort()
{
    sendAxisCommandStr("RA", "STOP", "");
    sendAxisCommandStr("DEC", "STOP", "");
    TrackState = SCOPE_IDLE;
    return true;
}

// ---------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------
bool SKRPicoMount::SetTrackEnabled(bool enabled)
{
    if (enabled)
    {
        double raRateStepsPerSec = arcsecPerSecToStepsPerSec(
            SIDEREAL_ARCSEC_PER_SEC, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
        sendAxisCommand("RA", "RATE", raRateStepsPerSec);
        isTracking = true;
        LOGF_INFO("Tracking enabled at %.3f steps/sec on RA", raRateStepsPerSec);
    }
    else
    {
        sendAxisCommandStr("RA", "STOP", "");
        isTracking = false;
        LOG_INFO("Tracking disabled.");
    }
    return true;
}

bool SKRPicoMount::SetTrackRate(double raRate, double deRate)
{
    // raRate/deRate arrive in arcsec/sec from INDI's track-rate property
    double raStepsPerSec = arcsecPerSecToStepsPerSec(raRate, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    sendAxisCommand("RA", "RATE", raStepsPerSec);

    if (fabs(deRate) > 0.001)
    {
        double decStepsPerSec = arcsecPerSecToStepsPerSec(deRate, DEC_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
        sendAxisCommand("DEC", "RATE", decStepsPerSec);
    }
    return true;
}

bool SKRPicoMount::SetTrackMode(uint8_t mode)
{
    // Only sidereal is really meaningful for this mount right now;
    // lunar/solar rates would need different constants substituted in
    // for SIDEREAL_ARCSEC_PER_SEC - left as a future improvement.
    INDI_UNUSED(mode);
    return true;
}

// ---------------------------------------------------------------------
// Manual N/S/E/W slew buttons (not guiding - these are the "nudge"
// buttons a client shows for manual centering)
// ---------------------------------------------------------------------
bool SKRPicoMount::MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command)
{
    if (command == MOTION_START)
    {
        double rate = arcsecPerSecToStepsPerSec(600, DEC_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS); // ~ moderate manual slew speed
        sendAxisCommand("DEC", "RATE", (dir == DIRECTION_NORTH) ? rate : -rate);
    }
    else
    {
        sendAxisCommandStr("DEC", "STOP", "");
    }
    return true;
}

bool SKRPicoMount::MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command)
{
    if (command == MOTION_START)
    {
        double rate = arcsecPerSecToStepsPerSec(600, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
        sendAxisCommand("RA", "RATE", (dir == DIRECTION_WEST) ? rate : -rate);
    }
    else
    {
        // resume normal sidereal tracking rather than a hard stop, if
        // tracking was active before the manual nudge
        if (isTracking)
            SetTrackEnabled(true);
        else
            sendAxisCommandStr("RA", "STOP", "");
    }
    return true;
}

// ---------------------------------------------------------------------
// Pulse guiding (PHD2) - short, small corrections layered on top of
// whatever the RA axis is already doing (normally sidereal tracking).
// NOTE: this naive implementation stops tracking, nudges, then restarts
// tracking - a smoother approach would briefly adjust the RATE up/down
// rather than stopping/restarting, to avoid a jerky guide correction.
// Flagged as a known rough edge, not a hidden one.
// ---------------------------------------------------------------------
IPState SKRPicoMount::GuideNorth(uint32_t ms)
{
    double rate = arcsecPerSecToStepsPerSec(200, DEC_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    sendAxisCommand("DEC", "RATE", rate);
    usleep(ms * 1000);
    sendAxisCommandStr("DEC", "STOP", "");
    return IPS_OK;
}

IPState SKRPicoMount::GuideSouth(uint32_t ms)
{
    double rate = arcsecPerSecToStepsPerSec(200, DEC_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    sendAxisCommand("DEC", "RATE", -rate);
    usleep(ms * 1000);
    sendAxisCommandStr("DEC", "STOP", "");
    return IPS_OK;
}

IPState SKRPicoMount::GuideEast(uint32_t ms)
{
    double sidereal = arcsecPerSecToStepsPerSec(SIDEREAL_ARCSEC_PER_SEC, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    double bump = arcsecPerSecToStepsPerSec(200, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    sendAxisCommand("RA", "RATE", sidereal + bump);
    usleep(ms * 1000);
    if (isTracking) SetTrackEnabled(true); else sendAxisCommandStr("RA", "STOP", "");
    return IPS_OK;
}

IPState SKRPicoMount::GuideWest(uint32_t ms)
{
    double sidereal = arcsecPerSecToStepsPerSec(SIDEREAL_ARCSEC_PER_SEC, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    double bump = arcsecPerSecToStepsPerSec(200, RA_GEAR_RATIO, MOTOR_STEPS_PER_REV, MICROSTEPS);
    sendAxisCommand("RA", "RATE", sidereal - bump);
    usleep(ms * 1000);
    if (isTracking) SetTrackEnabled(true); else sendAxisCommandStr("RA", "STOP", "");
    return IPS_OK;
}
