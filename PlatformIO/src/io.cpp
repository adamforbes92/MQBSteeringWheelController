#include "io.h"

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
//   duty-cycle error and apparent frequency jitter.  Averaging hundreds of
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

  radioResistor.begin(resistorInc, resistorUD, resistorCS);
  // Idle HIGH: hold the wiper at maximum resistance so the stereo stays
  // responsive. Commanded resistances momentarily toggle DOWN from here.
  radioResistor.setPosition(radioResistor.Ohm2Position(baseResistance), true);
}

void basicInit() {
#ifdef ENABLE_DEBUG
  Serial.begin(115200);
#endif

  DEBUG("VW Steering Wheel LIN Controller Initialising...");

  DEBUG("Preferences Initialising...");
  preferences.begin("settings", false);
  loadPreferences();
  DEBUG("Preferences Initialised!");

  DEBUG("IO Pins Initialising...");
  setupPins();
  DEBUG("IO Pins Initialised!");

  DEBUG("LIN Initialising...");
  steeringWheelLIN.begin(linBaud);
  chassisLIN.begin(linBaud);
  DEBUG("LIN Initialised!");

  DEBUG("CAN/TWAI Initialising...");
  canInit();
  DEBUG("CAN/TWAI Initialised!");

  DEBUG("VW Steering Wheel LIN Controller Initialised!");
}
