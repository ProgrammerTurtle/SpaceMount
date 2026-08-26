#pragma once

#include <inditelescope.h>
#include <indiguiderinterface.h>
#include <connectionplugins/connectionserial.h>

/*
 * SKRPicoMount - INDI::Telescope driver for the custom RA/Dec/Az/Alt
 * cycloidal-drive mount, talking to the SKR Pico firmware over USB
 * serial using the simple text protocol documented in the firmware's
 * own README (RATE / MOVE / STOP / EN / MS commands).
 *
 * NOTE: written against the documented INDI::Telescope API from
 * memory/docs - I do not have INDI dev headers available to
 * test-compile this in the sandbox that produced it. Treat method
 * signatures and constant names as "should be right" rather than
 * "verified against the real headers" - build it against your actual
 * INDI version and fix any signature mismatches the compiler flags.
 */
class SKRPicoMount : public INDI::Telescope, public INDI::GuiderInterface
{
    public:
        SKRPicoMount();
        virtual ~SKRPicoMount() = default;

        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;

    protected:
        // ---- lifecycle ----
        virtual bool Handshake() override;

        // ---- core telescope behaviors ----
        virtual bool ReadScopeStatus() override;
        virtual bool Goto(double ra, double dec) override;
        virtual bool Sync(double ra, double dec) override;
        virtual bool Abort() override;

        // ---- tracking ----
        virtual bool SetTrackEnabled(bool enabled) override;
        virtual bool SetTrackRate(double raRate, double deRate) override;
        virtual bool SetTrackMode(uint8_t mode) override;

        // ---- manual slew (N/S/E/W buttons in clients) ----
        virtual bool MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command) override;
        virtual bool MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command) override;

        // ---- pulse guiding (PHD2) ----
        virtual IPState GuideNorth(uint32_t ms) override;
        virtual IPState GuideSouth(uint32_t ms) override;
        virtual IPState GuideEast(uint32_t ms) override;
        virtual IPState GuideWest(uint32_t ms) override;

    private:
        // ---- serial helpers to the SKR Pico ----
        bool sendCommand(const char *cmd, char *response, size_t responseLen);
        bool sendAxisCommand(const char *axis, const char *action, double param);
        bool sendAxisCommandStr(const char *axis, const char *action, const char *param);

        // ---- coordinate / rate math ----
        // Converts a desired angular rate (arcsec/sec) on a given axis into
        // the step-rate value the firmware's RATE command expects, given
        // this axis's gear ratio and motor step angle.
        double arcsecPerSecToStepsPerSec(double arcsecPerSec, double gearRatio, int stepsPerRev, int microsteps);

        // Converts an angular distance (degrees) into a signed step count
        // for a MOVE command, given axis gear ratio and steps/rev.
        long degreesToSteps(double degrees, double gearRatio, int stepsPerRev, int microsteps);

        // Tracks our best estimate of current RA/Dec since this firmware
        // has no encoders - position is purely commanded/integrated, not
        // measured. This is a real limitation, see README.
        double currentRA { 0 };
        double currentDEC { 0 };
        bool isTracking { false };

        // ---- mechanical constants (SET THESE TO YOUR ACTUAL BUILD) ----
        static constexpr double RA_GEAR_RATIO   = 81.0;  // your two-stage cycloidal ratio
        static constexpr double DEC_GEAR_RATIO  = 81.0;  // update if Dec uses a different ratio
        static constexpr int    MOTOR_STEPS_PER_REV = 200; // 1.8 deg/step motors
        static constexpr int    MICROSTEPS      = 16;      // must match firmware's MS setting

        static constexpr double SIDEREAL_ARCSEC_PER_SEC = 15.041; // sidereal rate

        int PortFD { -1 };
};
