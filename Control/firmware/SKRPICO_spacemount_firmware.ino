/*
 * SKR Pico Telescope Mount Firmware (RA/Dec/Az/Alt step generator)
 * ------------------------------------------------------------------
 * Target: BIGTREETECH SKR Pico V1.0 (RP2040), Arduino-Pico core
 *         (https://github.com/earlephilhower/arduino-pico)
 *
 * Role in the stack: this firmware does NOT know about sidereal rates,
 * coordinates, or GoTo targets. It only does two things:
 *   1. Generate precise step/dir pulses to the 4 onboard TMC2209s
 *   2. Accept simple serial commands from the Pi 4B telling it what
 *      rate to run at (continuous tracking) or how far to move (GoTo)
 *
 * All astronomy math (sidereal rate, RA/Dec targeting, INDI protocol
 * translation) lives in Python on the Pi 4B. This board is deliberately
 * "dumb" so it can be small and reliable.
 *
 * ------------------------------------------------------------------
 * PIN MAP - sourced from BIGTREETECH's own Klipper config
 * (github.com/bigtreetech/SKR-Pico and Klipper3d/klipper generic cfg)
 * ------------------------------------------------------------------
 *  Axis   Step   Dir    Enable   (active-low dir/enable per BTT cfg)
 *  X      GP11   GP10   GP12
 *  Y      GP6    GP5    GP7
 *  Z      GP19   GP28   GP2
 *  E0     GP14   GP13   GP15
 *
 *  Shared TMC2209 UART (single-wire, all 4 drivers on one bus):
 *  TX: GP8   RX: GP9
 *  UART addresses (per BTT's own default cfg - VERIFY against your
 *  board's MS1/MS2 jumper/address pins before trusting this, some
 *  SKR Pico revisions number them 0,2,1,3 rather than 0,1,2,3):
 *    X = 0, Y = 1(or 2), Z = 2(or 1), E0 = 3
 *
 * In this firmware, axis 0=X is mapped to RA, 1=Y to DEC,
 * 2=Z to AZ (polar align), 3=E0 to ALT (polar align).
 * Re-map AXIS_* below if you wire it differently.
 * ------------------------------------------------------------------
 *
 * SERIAL COMMAND PROTOCOL (USB CDC, 115200 baud, newline-terminated)
 * ------------------------------------------------------------------
 *   <AXIS>:RATE:<signed_steps_per_sec>      continuous run (tracking)
 *   <AXIS>:MOVE:<signed_steps>              relative move at MOVE_SPEED
 *   <AXIS>:STOP                             stop this axis now
 *   <AXIS>:EN:<0|1>                         driver enable/disable
 *   <AXIS>:MS:<1|2|4|8|16|32|64|128|256>    set microstepping
 *   PING                                    -> replies "OK"
 *
 *   <AXIS> is one of: RA DEC AZ ALT
 *
 *   Examples:
 *     RA:RATE:1350       (start continuous stepping for sidereal track)
 *     RA:STOP
 *     DEC:MOVE:-4000      (relative GoTo-style move)
 *     RA:MS:16
 *
 * Every command gets a single-line reply: "OK" or "ERR <reason>"
 * ------------------------------------------------------------------
 */

#include <Arduino.h>
#include <TMCStepper.h>          // https://github.com/teemuatlut/TMCStepper

// ---------------- User-tunable constants ----------------
#define SERIAL_BAUD        115200
#define R_SENSE            0.11f   // BTT TMC2209 default sense resistor
#define TMC_UART_ADDR_RA    0
#define TMC_UART_ADDR_DEC   1
#define TMC_UART_ADDR_AZ    2
#define TMC_UART_ADDR_ALT   3
#define DEFAULT_RUN_CURRENT_MA  600   // set to your motor's rated current
#define DEFAULT_MICROSTEPS      16
#define MOVE_SPEED_STEPS_PER_SEC 2000  // speed used for relative MOVE commands
#define MAX_RATE_STEPS_PER_SEC   8000  // safety clamp

// ---------------- Pin map (see header comment) ----------------
struct AxisPins {
  uint8_t step;
  uint8_t dir;
  uint8_t enable;
  uint8_t uartAddr;
  const char* name;
};

// index: 0=RA 1=DEC 2=AZ 3=ALT
AxisPins axisPins[4] = {
  { 11, 10, 12, TMC_UART_ADDR_RA,  "RA"  },  // X
  { 6,  5,  7,  TMC_UART_ADDR_DEC, "DEC" },  // Y
  { 19, 28, 2,  TMC_UART_ADDR_AZ,  "AZ"  },  // Z
  { 14, 13, 15, TMC_UART_ADDR_ALT, "ALT" },  // E0
};

#define UART_TX_PIN 8
#define UART_RX_PIN 9

// ---------------- Per-axis runtime state ----------------
struct AxisState {
  volatile bool running = false;
  volatile bool dir = true;          // true = positive direction
  volatile uint32_t intervalUs = 0;  // microseconds between step pulses
  volatile int32_t stepsRemaining = 0; // for MOVE commands; <0 = infinite (RATE)
  repeating_timer_t timer;
  bool timerActive = false;
};

AxisState axisState[4];

// One shared UART for all 4 TMC2209s (single-wire PDN_UART bus)
// Use a hardware UART on the Pico; SoftwareSerial also works if needed.
HardwareSerial &tmcSerial = Serial2; // wire GP8/GP9 to Serial2 in variant, see note below

TMC2209Stepper driver[4] = {
  TMC2209Stepper(&tmcSerial, R_SENSE, TMC_UART_ADDR_RA),
  TMC2209Stepper(&tmcSerial, R_SENSE, TMC_UART_ADDR_DEC),
  TMC2209Stepper(&tmcSerial, R_SENSE, TMC_UART_ADDR_AZ),
  TMC2209Stepper(&tmcSerial, R_SENSE, TMC_UART_ADDR_ALT),
};

// ---------------- Step ISR (one repeating timer per axis) ----------------
// Runs in interrupt context: keep it minimal - just toggle STEP pin and
// decrement step count. All the "what rate should this axis run at" logic
// happens outside the ISR, in the command handler.
bool stepISR(struct repeating_timer *rt) {
  int axisIdx = (int)(intptr_t)rt->user_data;
  AxisState &st = axisState[axisIdx];
  AxisPins &pins = axisPins[axisIdx];

  if (!st.running) return true; // keep timer alive but idle, cheap check

  // pulse STEP high then low (TMC2209 needs only a short high pulse,
  // ~a few hundred ns minimum - a couple of NOPs/digitalWrite round trip
  // is plenty at these step rates)
  digitalWrite(pins.step, HIGH);
  delayMicroseconds(2);
  digitalWrite(pins.step, LOW);

  if (st.stepsRemaining > 0) {
    st.stepsRemaining--;
    if (st.stepsRemaining == 0) {
      st.running = false;
    }
  }
  // stepsRemaining < 0 means "run forever" (RATE mode / tracking) - never decremented

  return true; // true = keep repeating
}

// ---------------- Axis control helpers ----------------
void setAxisDirection(int idx, bool positive) {
  AxisPins &pins = axisPins[idx];
  axisState[idx].dir = positive;
  digitalWrite(pins.dir, positive ? HIGH : LOW);
}

void stopAxis(int idx) {
  axisState[idx].running = false;
  if (axisState[idx].timerActive) {
    cancel_repeating_timer(&axisState[idx].timer);
    axisState[idx].timerActive = false;
  }
}

// Start (or retarget) an axis at a given signed step rate.
// stepCount < 0 means "run forever" (continuous tracking / RATE command).
// stepCount >= 0 means "run exactly this many steps then stop" (MOVE command).
void startAxis(int idx, float stepsPerSec, int32_t stepCount) {
  AxisState &st = axisState[idx];

  // stop any existing timer on this axis before reconfiguring
  stopAxis(idx);

  bool positive = stepsPerSec >= 0;
  float absRate = fabs(stepsPerSec);
  if (absRate < 0.01f) return; // essentially zero, nothing to do
  if (absRate > MAX_RATE_STEPS_PER_SEC) absRate = MAX_RATE_STEPS_PER_SEC;

  setAxisDirection(idx, positive);

  st.stepsRemaining = stepCount; // -1 => infinite, run() decrements only if >0
  st.intervalUs = (uint32_t)(1000000.0f / absRate);
  st.running = true;

  // negative delay = "delay from start of previous callback" -> stable rate
  add_repeating_timer_us(-(int32_t)st.intervalUs, stepISR, (void*)(intptr_t)idx, &st.timer);
  st.timerActive = true;
}

int axisIndexFromName(const String &name) {
  for (int i = 0; i < 4; i++) {
    if (name.equalsIgnoreCase(axisPins[i].name)) return i;
  }
  return -1;
}

void setMicrosteps(int idx, uint16_t ms) {
  driver[idx].microsteps(ms);
}

void setEnable(int idx, bool en) {
  // BTT SKR Pico enable pins are active-low per Klipper cfg (!gpioX)
  digitalWrite(axisPins[idx].enable, en ? LOW : HIGH);
}

// ---------------- Serial command parsing ----------------
String inputLine;

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line.equalsIgnoreCase("PING")) {
    Serial.println("OK");
    return;
  }

  int firstColon = line.indexOf(':');
  if (firstColon < 0) {
    Serial.println("ERR malformed");
    return;
  }

  String axisName = line.substring(0, firstColon);
  String rest = line.substring(firstColon + 1);
  int idx = axisIndexFromName(axisName);
  if (idx < 0) {
    Serial.println("ERR unknown_axis");
    return;
  }

  int secondColon = rest.indexOf(':');
  String action = (secondColon >= 0) ? rest.substring(0, secondColon) : rest;
  String param   = (secondColon >= 0) ? rest.substring(secondColon + 1) : "";

  if (action.equalsIgnoreCase("RATE")) {
    float rate = param.toFloat();
    startAxis(idx, rate, -1); // -1 = run forever, this is tracking mode
    Serial.println("OK");

  } else if (action.equalsIgnoreCase("MOVE")) {
    long steps = param.toInt();
    if (steps == 0) { Serial.println("OK"); return; }
    float rate = (steps > 0) ? MOVE_SPEED_STEPS_PER_SEC : -MOVE_SPEED_STEPS_PER_SEC;
    startAxis(idx, rate, labs(steps));
    Serial.println("OK");

  } else if (action.equalsIgnoreCase("STOP")) {
    stopAxis(idx);
    Serial.println("OK");

  } else if (action.equalsIgnoreCase("EN")) {
    setEnable(idx, param.toInt() != 0);
    Serial.println("OK");

  } else if (action.equalsIgnoreCase("MS")) {
    int ms = param.toInt();
    setMicrosteps(idx, (uint16_t)ms);
    Serial.println("OK");

  } else {
    Serial.println("ERR unknown_action");
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  // Arduino-Pico (earlephilhower core) remaps UART pins like this,
  // BEFORE calling begin() - NOT the ESP32-style 4-argument begin().
  tmcSerial.setRX(UART_RX_PIN);
  tmcSerial.setTX(UART_TX_PIN);
  tmcSerial.begin(115200);

  for (int i = 0; i < 4; i++) {
    pinMode(axisPins[i].step, OUTPUT);
    pinMode(axisPins[i].dir, OUTPUT);
    pinMode(axisPins[i].enable, OUTPUT);
    digitalWrite(axisPins[i].step, LOW);
    setEnable(i, false); // start disabled until Pi explicitly enables

    driver[i].begin();
    driver[i].toff(4);
    driver[i].rms_current(DEFAULT_RUN_CURRENT_MA);
    driver[i].microsteps(DEFAULT_MICROSTEPS);
    driver[i].pwm_autoscale(true);
    driver[i].en_spreadCycle(false); // stealthChop by default; disable for
                                      // higher-torque spreadCycle if tracking
                                      // smoothness matters more than noise
  }

  Serial.println("READY mount_firmware skr_pico v0.1");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputLine.length() > 0) {
        handleCommand(inputLine);
        inputLine = "";
      }
    } else {
      inputLine += c;
    }
  }
}
