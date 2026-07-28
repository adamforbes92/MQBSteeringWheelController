#include "tasks.h"

#include <Arduino.h>
#include <driver/twai.h>

#include "globals.h"
#include "io.h"
#include "CAN.h"
#include "LIN.h"

#if ENABLE_IO_TEST
struct IoTestState {
  bool highSide;
  bool lin1ToLin2;
  bool lin2ToLin1;
  bool canReceived;
  uint16_t resistanceOhm;
};

static bool testLin(HardwareSerial& sender, HardwareSerial& receiver, uint8_t marker) {
  while (receiver.available()) receiver.read();

  const uint8_t message[] = {0x55, marker, 0xAA};
  sender.write(message, sizeof(message));
  sender.flush();

  for (uint8_t index = 0; index < sizeof(message); index++) {
    const uint32_t timeout = millis() + 20;
    while (!receiver.available() && (int32_t)(millis() - timeout) < 0) {
      vTaskDelay(1);
    }
    if (!receiver.available() || receiver.read() != message[index]) return false;
  }
  return true;
}

static void showLinStatus(bool healthy) {
  if (healthy) {
    digitalWrite(pinOnboardLed, HIGH);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return;
  }

  for (uint8_t flash = 0; flash < 8; flash++) {
    digitalWrite(pinOnboardLed, !digitalRead(pinOnboardLed));
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void ioTestTask(void* parameter) {
  (void)parameter;

  IoTestState test = {};
  pinMode(pinOnboardLed, OUTPUT);
  digitalWrite(pinOnboardLed, LOW);
  digitalWrite(pinPNP, LOW);
  radioResistor.setPosition(radioResistor.Ohm2Position(0), true);
  DEBUG("[IO TEST] ENABLED - link LIN1 and LIN2");

  while (true) {
    test.highSide = !test.highSide;
    digitalWrite(pinPNP, test.highSide);
    radioResistor.setPosition(radioResistor.Ohm2Position(test.resistanceOhm), true);

    test.lin1ToLin2 = testLin(Serial1, Serial2, 0x12);
    test.lin2ToLin1 = testLin(Serial2, Serial1, 0x21);
    pollCanRx();
    test.canReceived = canHealthy();

    DEBUG("[IO TEST] HS=%s LIN1>2=%s LIN2>1=%s CAN=%s R=%uK",
          test.highSide ? "ON" : "OFF",
          test.lin1ToLin2 ? "GOOD" : "FAIL",
          test.lin2ToLin1 ? "GOOD" : "FAIL",
          test.canReceived ? "GOOD" : "WAIT",
          test.resistanceOhm / 1000);

    showLinStatus(test.lin1ToLin2 && test.lin2ToLin1);
    test.resistanceOhm = test.resistanceOhm >= digipotMaxOhm ? 0 : test.resistanceOhm + 1000;
  }
}
#endif

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
      total_Time = avgPeriodUs;
      on_Time    = avgOnUs;
      dutyCycle  = avgPeriodUs;  // period in µs — used for mapping and learn capture

    } else {
      // No edges in the last second — signal absent or frequency < 1 Hz.
      total_Time = 0;
      on_Time    = 0;
      dutyCycle  = 0;
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
  }
}

#if enableDebug
static void debugTask(void* parameter) {
  (void)parameter;

  while (true) {
    const uint32_t periodUs = total_Time;
    const uint32_t onUs = on_Time;
    const uint32_t auxHz = periodUs > 0 ? 1000000UL / periodUs : 0;
    const uint32_t auxDutyPct10 = periodUs > 0 ? onUs * 1000UL / periodUs : 0;
    const uint32_t nowMs = millis();
    const bool lin1Healthy = swLinLastOkMs != 0 && nowMs - swLinLastOkMs < 1000UL;
    const bool lin2Healthy = chassisLinLastOkMs != 0 && nowMs - chassisLinLastOkMs < 1000UL;

  #if debugIO
    DEBUG_IO("resistive=%lu/%u ohm high-side=%s aux=%lu.%lu%% @ %lu Hz",
             static_cast<unsigned long>(radioResistor.getOhm()), digipotMaxOhm,
             digitalRead(pinPNP) == HIGH ? "ON" : "OFF",
             auxDutyPct10 / 10, auxDutyPct10 % 10, auxHz);
  #endif
  #if debugLIN
    DEBUG_LIN("LIN1=%s LIN2=%s RX=0x%02lX TX=0x%02lX",
              lin1Healthy ? "HEALTHY" : "NO DATA",
              lin2Healthy ? "HEALTHY" : "NO DATA",
              static_cast<unsigned long>(lastLinInId),
              static_cast<unsigned long>(lastLinOutId));
  #endif
  #if debugCAN
    DEBUG_CAN("state=%s RX=%s TX=%s ID=0x%03X",
          canBroadcastEnabled || paddlesEnabled ? "ENABLED" : "DISABLED",
          canHealthy() ? "HEALTHY" : "NO DATA",
          canBroadcastEnabled ? "ENABLED" : "DISABLED",
          canBroadcastId);
  #endif

    vTaskDelay(pdMS_TO_TICKS(serialMonitorRefresh));
  }
}
#endif

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
    sendLatchedButtonOutputs();

    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(linPause));
  }
}

static void debounceOutputTask(void* parameter) {
  (void)parameter;

  while (1) {
    broadcastButtonsCAN();
    serviceOpenHaldex();

    // PNP output: diagnostic override → latch state → momentary hold.
    {
      bool pnpActive = diagPnpActive;

      // Latched buttons: PNP stays active while the latch is engaged.
      if (!pnpActive) {
        for (size_t i = 0; i < buttonMappingCount; i++) {
          if (buttonLatched[i] &&
              (buttonMappings[i].flags & FLAG_ACTIVATES_PNP) &&
              !(buttonMappings[i].flags & FLAG_OPENHALDEX_CONTROL)) {
            pnpActive = true;
            break;
          }
        }
      }

      // Momentary (non-latch): active while within the hold window.
      if (!pnpActive && latestLinButtonId != 0 &&
          ((uint32_t)millis() - latestLinButtonTimestamp) < (uint32_t)canHoldMs) {
        for (size_t i = 0; i < buttonMappingCount; i++) {
          if ((buttonMappings[i].flags & FLAG_ACTIVATES_PNP) &&
              !(buttonMappings[i].flags & FLAG_LATCH) &&
              !(buttonMappings[i].flags & FLAG_OPENHALDEX_CONTROL) &&
              buttonMappings[i].oldButtonId == latestLinButtonId) {
            pnpActive = true;
            break;
          }
        }
      }

      digitalWrite(pinPNP, pnpActive ? HIGH : LOW);
    }

    // Resistive output: idle HIGH; commanded resistances toggle DOWN then back.
    {
      const uint8_t idlePos = radioResistor.Ohm2Position(digipotMaxOhm);  // idle = max resistance (HIGH)
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
#if ENABLE_IO_TEST
  xTaskCreatePinnedToCore(ioTestTask, "ioTestTask", 4096, nullptr, 3, nullptr, 1);
  return;
#endif

  if (steeringWheelLinMutex == nullptr) {
    steeringWheelLinMutex = xSemaphoreCreateMutex();
  }

  if (chassisLinMutex == nullptr) {
    chassisLinMutex = xSemaphoreCreateMutex();
  }

#if enableDebug
  xTaskCreatePinnedToCore(debugTask, "debugTask", 3072, nullptr, 1, nullptr, 0);
#endif


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
