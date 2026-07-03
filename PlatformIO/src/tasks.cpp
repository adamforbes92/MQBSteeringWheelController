#include "tasks.h"

#include <Arduino.h>

#include "globals.h"
#include "io.h"
#include "CAN.h"
#include "LIN.h"

static void updateBacklightState() {
  // -----------------------------------------------------------------------
  // Aux PWM measurement — 1-second averaging window.
  //
  // Runs every loop iteration but only does real work once per second.
  // Accumulating over ~1 s captures hundreds of cycles at typical automotive
  // dimmer frequencies (100 Hz – 2 kHz), which averages out the per-edge
  // jitter introduced by the optocoupler and gives a stable reading.
  // -----------------------------------------------------------------------
  static uint32_t auxWindowMs = 0;
  const uint32_t nowMs = (uint32_t)millis();

  if (nowMs - auxWindowMs >= 1000UL) {
    auxWindowMs = nowMs;
    const AuxPwmSnapshot snap = takeAuxPwmSnapshot();

    if (snap.riseCount > 0) {
      const uint32_t avgPeriodUs = snap.sumPeriodUs / snap.riseCount;
      const uint32_t avgOnUs     = snap.sumOnUs     / snap.riseCount;
      const uint32_t freqHz      = avgPeriodUs > 0 ? 1000000UL / avgPeriodUs : 0;
      const uint32_t dutyPct10   = avgPeriodUs > 0
                                   ? (avgOnUs * 1000UL / avgPeriodUs)  // ×10 → 0.1 % precision
                                   : 0;
      total_Time = avgPeriodUs;
      on_Time    = avgOnUs;
      dutyCycle  = avgPeriodUs;  // period in µs — used for mapping and learn capture

      DEBUG("AUX [%lu edges/s]: %lu Hz  period=%lu us  on=%lu us  duty=%lu.%lu%%",
            snap.riseCount, freqHz, avgPeriodUs, avgOnUs,
            dutyPct10 / 10, dutyPct10 % 10);
    } else {
      // No edges in the last second — signal absent or frequency < 1 Hz.
      total_Time = 0;
      on_Time    = 0;
      dutyCycle  = 0;
      DEBUG("AUX: no signal (0 edges in 1 s window)");
    }
  }

  // -----------------------------------------------------------------------
  // Apply the measured (or forced) value to the steering wheel LIN frame.
  // -----------------------------------------------------------------------
  if (!useAuxLightSource) {
    if (forceBacklight) {
      steeringWheelLightData[0] = (uint8_t)((forceBacklightPercent * upperLightsLIN) / 100U);
    } else {
      // Forward chassis LIN brightness — gatewayLightData[0] populated by getLightLINFrame().
      const uint8_t linRaw = gatewayLightData[0];
      steeringWheelLightData[0] = linRaw < upperLightsLIN ? linRaw : upperLightsLIN;
    }
    return;
  }

  if (total_Time > 0) {
    // Map duty cycle (tenths of %) to LIN brightness.
    // High duty (brightDuty) → full brightness; low duty (dimDuty) → zero brightness.
    // Clamp output so signals outside the calibrated range still yield a valid level.
    const uint32_t dutyPct10 = (on_Time * 1000UL) / total_Time;  // 0–1000 (tenths of %)
    uint16_t dimDuty    = auxDimDutyPct10;    // duty at zero-brightness end  (e.g. 197 = 19.7 %)
    uint16_t brightDuty = auxBrightDutyPct10; // duty at full-brightness end  (e.g. 980 = 98.0 %)
    if (brightDuty <= dimDuty) { dimDuty = 197; brightDuty = 980; }
    const long mapped = map((long)dutyPct10, (long)dimDuty, (long)brightDuty, 0L, (long)upperLightsLIN);
    steeringWheelLightData[0] = (uint8_t)constrain(mapped, 0L, (long)upperLightsLIN);
    lastDutyCycle = dutyPct10;
  } else {
    // No edges in the last window — could be DC high (lights full on) or signal absent.
    const bool pinHigh = digitalRead(pinAuxLight) == HIGH;
    steeringWheelLightData[0] = pinHigh ? upperLightsLIN : 0;
    if (pinHigh) {
      DEBUG("AUX: DC high → full brightness");
    } else {
      DEBUG("AUX: no signal (0 edges in 1 s window)");
    }
  }
}

// Single sequential task for the steering wheel LIN bus.
// Previously split into two tasks (lightLinTask + buttonLinTask), which caused
// a race: both called steeringWheelLIN.handler() without mutex protection and
// both used independent vTaskDelayUntil timers initialised at the same tick,
// so they woke simultaneously every 100 ms and corrupted the LIN state machine.
static void steeringWheelLinTask(void* parameter) {
  (void)parameter;

  TickType_t lastWakeTime = xTaskGetTickCount();

  while (1) {
    // --- Light frame (0x0D) ---
    // In LIN mode: read brightness from chassis bus, then forward to steering wheel.
    // In AUX/FORCED mode: skip the chassis read to avoid unnecessary blocking.
    steeringWheelLIN.handler();
    if (!useAuxLightSource && !forceBacklight) {
      getLightLINFrame();
    }
    updateBacklightState();
    sendLightLINFrame();

    // --- Button frame (0x0E) — wheel sends, chassis listens ---
    steeringWheelLIN.handler();
    getButtonState();
    sendButtonLINFrame();

    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(linPause));
  }
}

static void debounceOutputTask(void* parameter) {
  (void)parameter;

  while (1) {
    if (hasCAN) {
      broadcastButtonsCAN();
    }

    // PNP output: diagnostic override → latch state → momentary hold.
    {
      static uint32_t lastLatchTimestamp = 0;
      static bool pnpLatchState = false;

      // Rising-edge detection: new timestamp means a new button press event.
      if (latestLinButtonId != 0 && latestLinButtonTimestamp != lastLatchTimestamp) {
        lastLatchTimestamp = latestLinButtonTimestamp;
        for (size_t i = 0; i < buttonMappingCount; i++) {
          if ((buttonMappings[i].flags & FLAG_ACTIVATES_PNP) &&
              (buttonMappings[i].flags & FLAG_PNP_LATCH) &&
              buttonMappings[i].oldButtonId == latestLinButtonId) {
            pnpLatchState = !pnpLatchState;  // toggle on each press
            break;
          }
        }
      }

      bool pnpActive = diagPnpActive || pnpLatchState;

      // Momentary (non-latch): active while within the hold window.
      if (!pnpActive && latestLinButtonId != 0 &&
          ((uint32_t)millis() - latestLinButtonTimestamp) < (uint32_t)canHoldMs) {
        for (size_t i = 0; i < buttonMappingCount; i++) {
          if ((buttonMappings[i].flags & FLAG_ACTIVATES_PNP) &&
              !(buttonMappings[i].flags & FLAG_PNP_LATCH) &&
              buttonMappings[i].oldButtonId == latestLinButtonId) {
            pnpActive = true;
            break;
          }
        }
      }

      digitalWrite(pinPNP, pnpActive ? HIGH : LOW);
    }

    // Resistive output: idle HIGH; commanded resistances toggle DOWN then back.
    if (hasResistiveStereo) {
      const uint8_t idlePos = radioResistor.Ohm2Position(baseResistance);  // idle = max resistance (HIGH)
      static bool prevDiagResistive = false;
      static uint8_t lastDiagPos = 0xFF;  // 0xFF = unset sentinel; reset on disable
      if (diagResistiveEnabled) {
        const uint8_t targetPos = radioResistor.Ohm2Position(diagResistiveOhm);
        if (targetPos != lastDiagPos) {
          radioResistor.setPosition(targetPos, true);  // forced only on change
          lastDiagPos = targetPos;
        }
        prevDiagResistive = true;
      } else {
        if (prevDiagResistive) {
          radioResistor.setPosition(idlePos, true);  // return to idle HIGH when diagnostic is disabled
          prevDiagResistive = false;
          lastDiagPos = 0xFF;  // force reposition next time diagnostic is re-enabled
        }

        // Test resistance: pulse DOWN to the requested value for 0.5 s, then idle HIGH.
        if (testResistancePulse) {
          testResistancePulse = false;
          radioResistor.setPosition(radioResistor.Ohm2Position(testResistanceOhm), true);
          vTaskDelay(pdMS_TO_TICKS(500));
          radioResistor.setPosition(idlePos, true);
        }

        // Momentary button press: hold the mapped resistance only while the
        // button is actively pressed on the wheel, then snap back to idle HIGH
        // as soon as it is released — mirroring the real button duration.
        // radioResistanceMs is refreshed on every LIN poll the button is held;
        // the short window tolerates one missed poll (linPause = 100 ms).
        static uint8_t lastBtnPos = 0xFF;  // 0xFF = currently idle HIGH
        const uint32_t kResistiveHoldMs = 200;
        const bool btnHeld = radioResistance != 0 &&
                             (uint32_t)(millis() - radioResistanceMs) < kResistiveHoldMs;
        if (btnHeld) {
          const uint8_t pos = radioResistor.Ohm2Position(radioResistance);
          if (pos != lastBtnPos) {
            radioResistor.setPosition(pos, true);  // toggle DOWN to the mapped value
            lastBtnPos = pos;
          }
        } else if (lastBtnPos != 0xFF) {
          radioResistor.setPosition(idlePos, true);  // released → reset HIGH
          lastBtnPos = 0xFF;
          radioResistance = 0;
        }
      }
    }

    // Run at ~20 Hz so the hold window clears promptly after button release
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void startTasks() {
  if (steeringWheelLinMutex == nullptr) {
    steeringWheelLinMutex = xSemaphoreCreateMutex();
  }

  if (chassisLinMutex == nullptr) {
    chassisLinMutex = xSemaphoreCreateMutex();
  }


  // GRA (paddle shift) task — broadcasts continuously at 20 ms, matching DSG keepalive expectation
  xTaskCreate(broadcastGRATask, "broadcastGRATask", 2048, nullptr, 2, nullptr);

  // CAN RX polling task — drains the TWAI receive queue continuously
  xTaskCreate([](void*) {
    while (true) {
      pollCanRx();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }, "canRxTask", 2048, nullptr, 2, nullptr);

  // Preferences save task — persists all settings to NVS every 5 s
  xTaskCreate([](void*) {
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      savePreferences();
    }
  }, "prefsTask", 3072, nullptr, 1, nullptr);

  // create the steering wheel LIN task pinned to core 1 (leaving core 0 free for WiFi and CAN tasks, which use ISRs that must run on core 0)
  BaseType_t linResult = xTaskCreatePinnedToCore(steeringWheelLinTask, "steeringWheelLinTask", 8192, nullptr, 3, nullptr, 1);
  if (linResult != pdPASS) {
    DEBUG("steeringWheelLinTask create pinned failed, falling back to xTaskCreate");
    xTaskCreate(steeringWheelLinTask, "steeringWheelLinTask", 8192, nullptr, 3, nullptr);
  }

  // create the debounce and output control task on core 0 (CAN and output updates must run on the same core)
  BaseType_t debounceResult = xTaskCreatePinnedToCore(debounceOutputTask, "debounceOutputTask", 4096, nullptr, 1, nullptr, 1);
  if (debounceResult != pdPASS) {
    DEBUG("debounceOutputTask create pinned failed, falling back to xTaskCreate");
    xTaskCreate(debounceOutputTask, "debounceOutputTask", 4096, nullptr, 1, nullptr);
  }
}
