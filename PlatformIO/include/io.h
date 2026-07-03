#pragma once

#include <stdint.h>

// Snapshot of ISR accumulators captured over a measurement window.
struct AuxPwmSnapshot {
  uint32_t riseCount;    // number of valid rising-to-rising periods captured
  uint32_t sumPeriodUs;  // sum of those periods (µs)  → avgPeriod = sum / count
  uint32_t sumOnUs;      // sum of rising-to-falling intervals (µs) → avgOn = sum / count
};

// Atomically snapshots and resets the ISR accumulators.
// Safe to call from any FreeRTOS task on either core.
AuxPwmSnapshot takeAuxPwmSnapshot();

void basicInit();
void setupPins();
void auxLightPWM();
