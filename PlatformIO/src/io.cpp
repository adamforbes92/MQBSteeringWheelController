#include "io.h"

#include <driver/gpio.h>    // gpio_reset_pin() — detach UART from old pins
#include <soc/gpio_reg.h>   // GPIO_IN_REG / GPIO_IN1_REG for fast pin reads

#include "globals.h"
#include "CAN.h"

// ---------------------------------------------------------------------------
// ISR accumulators — written only by auxLightPWM(), read+reset by
// takeAuxPwmSnapshot().  Protected by a portMUX so both cores are
// safe even though the ISR and snapshot run concurrently.
//
// Why accumulate instead of single-cycle measurement?
//   An optocoupler introduces asymmetric propagation delay (rise ≠ fall time,
//   typically 3–4 µs each for a PC817).  On a single cycle this creates direct
//   duty-cycle error and frequency errors.  Averaging hundreds of
//   cycles over 1 s cancels the per-edge noise and gives a stable result.
//
// Why GPIO register read instead of digitalRead()?
//   digitalRead() in ESP32 Arduino adds ~200 ns of overhead and passes through
//   the GPIO abstraction layer.  On a fast signal the pin may have already
//   changed state again by the time the read completes, giving the wrong edge
//   direction.  A direct REG_READ() resolves in a single load instruction.
// ---------------------------------------------------------------------------
static portMUX_TYPE    sAccumMux    = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t sRiseCount   = 0;  // valid rising-to-rising measurements
static volatile uint32_t sSumPeriodUs = 0;  // sum of rising-to-rising intervals
static volatile uint32_t sSumOnUs     = 0;  // sum of rising-to-falling intervals
static volatile uint32_t sLastRise    = 0;  // µs timestamp of most recent rising edge

AuxPwmSnapshot takeAuxPwmSnapshot() {
  portENTER_CRITICAL(&sAccumMux);
  const AuxPwmSnapshot snap = { sRiseCount, sSumPeriodUs, sSumOnUs };
  sRiseCount    = 0;
  sSumPeriodUs  = 0;
  sSumOnUs      = 0;
  sLastRise     = 0;  // force re-sync; next rising edge re-establishes baseline
  portEXIT_CRITICAL(&sAccumMux);
  return snap;
}

void IRAM_ATTR auxLightPWM() {
  const uint32_t now = (uint32_t)micros();
  lastRead = now;

  // Direct GPIO register read — single load instruction, no OS overhead.
  // pinAuxLight = 39 (≥ 32) → GPIO_IN1 register, bit offset (39 − 32) = 7.
  const bool high = (pinAuxLight < 32)
      ? ((REG_READ(GPIO_IN_REG)  >>  pinAuxLight)        & 1U)
      : ((REG_READ(GPIO_IN1_REG) >> (pinAuxLight - 32))  & 1U);

  if (high) {
    // Rising edge: measure period since the previous rising edge.
    const uint32_t last = sLastRise;
    sLastRise = now;
    if (last != 0) {
      const uint32_t period = now - last;
      // Accept 10 Hz (100 000 µs period) … 200 kHz (5 µs period).
      if (period >= 5UL && period <= 100000UL) {
        portENTER_CRITICAL_ISR(&sAccumMux);
        sSumPeriodUs += period;
        sRiseCount = sRiseCount + 1;
        portEXIT_CRITICAL_ISR(&sAccumMux);
      }
    }
  } else {
    // Falling edge: measure on-time since the last rising edge.
    const uint32_t lastRise = sLastRise;
    if (lastRise != 0) {
      const uint32_t onUs = now - lastRise;
      if (onUs > 0 && onUs < 100000UL) {
        portENTER_CRITICAL_ISR(&sAccumMux);
        sSumOnUs += onUs;
        portEXIT_CRITICAL_ISR(&sAccumMux);
      }
    }
  }
}

void setupPins() {
  pinMode(pinCS_LIN, OUTPUT);
  digitalWrite(pinCS_LIN, HIGH);

  pinMode(pinWake_LIN, OUTPUT);
  digitalWrite(pinWake_LIN, HIGH);

  pinMode(pinAuxLight, INPUT);
  attachInterrupt(pinAuxLight, auxLightPWM, CHANGE);

  pinMode(pinPNP, OUTPUT);
  digitalWrite(pinPNP, LOW);

  pinMode(resistorUD, OUTPUT);
  pinMode(resistorInc, OUTPUT);
  pinMode(resistorCS, OUTPUT);
  digitalWrite(resistorUD, HIGH);
  digitalWrite(resistorInc, HIGH);
  digitalWrite(resistorCS, HIGH);

  radioResistor.begin(resistorInc, resistorUD, resistorCS);
  // Idle HIGH: hold the wiper at maximum resistance so the stereo stays
  // responsive. Commanded resistances momentarily toggle DOWN from here.
  radioResistor.setPosition(radioResistor.Ohm2Position(digipotMaxOhm), true);
}

// Reopen the steering-wheel UART (Serial1) with a new frame format and speed —
// LIN is 8N1, the BMW K-bus 8E1 — on the same pins basicInit() chose,
// including the Legacy-PCB RX/TX swap. Fully releases the pads first, for the
// reason given in basicInit(). Caller holds steeringWheelLinMutex.
void reopenWheelUart(uint32_t baud, uint32_t config) {
  Serial1.end();
  gpio_reset_pin(static_cast<gpio_num_t>(pinRX_LINSteeringWheel));
  gpio_reset_pin(static_cast<gpio_num_t>(pinTX_LINSteeringWheel));
  if (linLegacyPins) {
    Serial1.begin(baud, config, pinTX_LINSteeringWheel, pinRX_LINSteeringWheel);
  } else {
    Serial1.begin(baud, config, pinRX_LINSteeringWheel, pinTX_LINSteeringWheel);
  }
}

void basicInit() {
  // Serial console is always on (115200): logLine() mirrors every LIN Monitor
  // line here. Up to V1.09 this was only started with enableDebug, so a release
  // build printed nothing over USB.
#if MFSW_HAS_NATIVE_USB
  // S3 native USB (already started by the core, CDC on boot): writes never
  // block with no host attached; with one attached, cap the wait at 10 ms so a
  // terminal that's open but not reading can't stall the LIN tasks. (Don't
  // resize its TX ring here — that frees the buffer under a running USB ISR.)
  Serial.setTxTimeoutMs(10);
#else
  // DevKit UART0: a TX ring buffer (must be set before begin()) so log lines
  // queue instead of blocking the LIN tasks on the 128-byte hardware FIFO.
  Serial.setTxBufferSize(4096);
#endif
  Serial.begin(115200);
  Serial.printf("\r\n[BOOT] MFSW Controller fw %s (%s)\r\n", FW_VERSION, MFSW_BOARD_NAME);

  DEBUG("VW Steering Wheel LIN Controller Initialising...");

  DEBUG("Preferences Initialising...");
  preferences.begin("settings", false);
  loadPreferences();
  radioResistor = X9C103(digipotMaxOhm);
  DEBUG("Preferences Initialised!");

  DEBUG("IO Pins Initialising...");
  setupPins();
  DEBUG("IO Pins Initialised!");

  DEBUG("LIN Initialising...");
  steeringWheelLIN.begin(linBaud);
  chassisLIN.begin(linBaud);
  // NOTE (TEMPORARY — inverted for on-board testing): default to the LEGACY
  // swapped RX/TX layout so a legacy board runs without any configuration, then
  // re-define to the normal layout only when legacy mode is OFF. Revert to the
  // original `if (linLegacyPins)` swap once non-legacy boards are confirmed.
  //
  // A bare end()+begin() with swapped pins is not enough: ESP32's HardwareSerial
  // leaves the previous GPIO-matrix routing attached to the original pins, so
  // both the old and new UART signals stay live and the swap silently fails
  // (RX never reads). Fully release the pads with gpio_reset_pin() between
  // end() and begin() so only the intended RX/TX routing remains.

  // --- default: legacy (swapped) RX/TX layout ---
  Serial1.end();
  Serial2.end();
  gpio_reset_pin(static_cast<gpio_num_t>(pinRX_LINSteeringWheel));
  gpio_reset_pin(static_cast<gpio_num_t>(pinTX_LINSteeringWheel));
  gpio_reset_pin(static_cast<gpio_num_t>(pinRX_LINchassis));
  gpio_reset_pin(static_cast<gpio_num_t>(pinTX_LINchassis));
  Serial1.begin(linBaud, SERIAL_8N1, pinTX_LINSteeringWheel, pinRX_LINSteeringWheel);
  Serial2.begin(linBaud, SERIAL_8N1, pinTX_LINchassis, pinRX_LINchassis);

  if (!linLegacyPins) {
    // --- not legacy: re-define to the normal RX/TX layout ---
    Serial1.end();
    Serial2.end();
    gpio_reset_pin(static_cast<gpio_num_t>(pinRX_LINSteeringWheel));
    gpio_reset_pin(static_cast<gpio_num_t>(pinTX_LINSteeringWheel));
    gpio_reset_pin(static_cast<gpio_num_t>(pinRX_LINchassis));
    gpio_reset_pin(static_cast<gpio_num_t>(pinTX_LINchassis));
    Serial1.begin(linBaud, SERIAL_8N1, pinRX_LINSteeringWheel, pinTX_LINSteeringWheel);
    Serial2.begin(linBaud, SERIAL_8N1, pinRX_LINchassis, pinTX_LINchassis);
  }
  DEBUG("LIN Initialised!");

  DEBUG("CAN/TWAI Initialising...");
  canInit();
  DEBUG("CAN/TWAI Initialised!");

  DEBUG("VW Steering Wheel LIN Controller Initialised!");
}
