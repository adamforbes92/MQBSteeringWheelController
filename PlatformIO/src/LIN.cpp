#include "LIN.h"

#include "API.h"
#include "globals.h"

static bool lockLinBus(SemaphoreHandle_t mutex)
{
  if (mutex == nullptr)
  {
    return true;
  }

  return xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlockLinBus(SemaphoreHandle_t mutex)
{
  if (mutex != nullptr)
  {
    xSemaphoreGive(mutex);
  }
}

static void captureLearnedButton(uint8_t buttonId)
{
  expireLearnState();
  if (!learnActive || learnRowIndex >= buttonMappingCount)
  {
    return;
  }

  if (learnTarget == LEARN_OLD_LIN)
  {
    buttonMappings[learnRowIndex].oldButtonId = buttonId;
  }
  else if (learnTarget == LEARN_NEW_LIN)
  {
    buttonMappings[learnRowIndex].newLinButtonId = buttonId;
  }
  // LEARN_NEW_CAN removed: CAN assignment is now explicit byte+bit selection, not learned

  clearLearnState();
}

void getLightLINFrame()
{
  if (!lockLinBus(chassisLinMutex))
  {
    return;  // keep last known value; bus busy
  }

  uint8_t buf[4] = {};
  chassisLIN.resetStateMachine();
  chassisLIN.resetError();
  chassisLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linLightInId, 4, buf);

  const LIN_Master_Base::error_t linErr = chassisLIN.getError();

  unlockLinBus(chassisLinMutex);

  if (linErr == LIN_Master_Base::NO_ERROR)
  {
    // Only update on a clean receive — preserves last known brightness on missed frames.
    memcpy(gatewayLightData, buf, sizeof(gatewayLightData));
    chassisLinLastOkMs = millis();  // LIN 2 healthy
  }
}

// Handle a fresh button-press event (rising edge only): toggle latch state for
// latching buttons and issue OpenHaldex mode commands. Fires exactly once per
// physical press; momentary CAN/LIN output is handled per-poll elsewhere.
static void handleButtonPressEvent(uint8_t buttonId)
{
  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (buttonMappings[i].oldButtonId != buttonId)
    {
      continue;
    }

    if (buttonMappings[i].flags & FLAG_OPENHALDEX_CONTROL)
    {
      // Exclusive: this press only commands an OpenHaldex mode change.
      uint8_t target;
      if (buttonMappings[i].openHaldexMode == OPENHALDEX_MODE_PUSH_NEXT)
      {
        // Advance from the pending target if one is still in flight (so rapid
        // presses chain), else from the last broadcast mode; unknown -> Stock.
        uint8_t base = (openHaldexTargetMode != OPENHALDEX_MODE_UNKNOWN)
                           ? openHaldexTargetMode
                           : openHaldexCurrentMode;
        if (base >= OPENHALDEX_MODE_COUNT)
        {
          base = 0;
        }
        target = static_cast<uint8_t>((base + 1) % OPENHALDEX_MODE_COUNT);
      }
      else
      {
        target = (buttonMappings[i].openHaldexMode < OPENHALDEX_MODE_COUNT)
                     ? buttonMappings[i].openHaldexMode
                     : 0;
      }
      openHaldexTargetMode = target;
      openHaldexCmdStartMs = millis();
      openHaldexLastSendMs = 0;  // force an immediate send in serviceOpenHaldex()
      return;
    }

    if (buttonMappings[i].flags & FLAG_LATCH)
    {
      buttonLatched[i] = !buttonLatched[i];  // toggle on each press
      return;
    }

    return;  // plain button: momentary output handled in sendButtonLINFrame()
  }
}

void getButtonState()
{
  memset(recvButtonData, 0, sizeof(recvButtonData));
  memset(transButtonDataLIN, 0, sizeof(transButtonDataLIN));
  memset(transButtonDataCAN, 0, sizeof(transButtonDataCAN));

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linButtonInId, 8, recvButtonData);

  // Discard the frame if the library flagged any error (timeout, checksum,
  // echo mismatch, etc.).  Without this, partial bytes written by a failed
  // receive leave garbage in recvButtonData[1] which matches a button mapping
  // and makes a button appear pressed when nothing was touched.
  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();
  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    memset(recvButtonData, 0, sizeof(recvButtonData));
  }
  else
  {
    swLinLastOkMs = millis();  // LIN 1 healthy
  }

  unlockLinBus(steeringWheelLinMutex);

  portENTER_CRITICAL(&stateMux);
  memcpy(lastLinInFrame, recvButtonData, sizeof(lastLinInFrame));
  lastLinInLen = sizeof(lastLinInFrame);
  lastLinInId = linButtonInId;
  portEXIT_CRITICAL(&stateMux);

  if (recvButtonData[1] != 0)
  {
    latestLinButtonId = recvButtonData[1];
    latestLinButtonTimestamp = millis();
    captureLearnedButton(recvButtonData[1]);
  }

  // Rising-edge press detection (0 -> non-zero): drives one-shot actions
  // (latch toggling, OpenHaldex commands) exactly once per physical press.
  static uint8_t prevButtonRaw = 0;
  const uint8_t rawButton = recvButtonData[1];
  if (rawButton != 0 && prevButtonRaw == 0)
  {
    logLine("[SW] btn id=0x%02X on 0x%02X", rawButton, linButtonInId);
    handleButtonPressEvent(rawButton);
  }
  prevButtonRaw = rawButton;
}

void sendLightLINFrame()
{
  // Light frame goes ONLY to the steering wheel LIN bus.
  // Chassis LIN is a source (read FROM), not a destination for light data.
  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightInId, 4, steeringWheelLightData);

  unlockLinBus(steeringWheelLinMutex);
}

void sendButtonLINFrame()
{
  buttonFound = false;

  switch (recvButtonData[6])
  {
  case 1:
    dsgPaddleDown = true;
    break;

  case 2:
    dsgPaddleUp = true;
    break;

  default:
    break;
  }

  if (recvButtonData[1] != 0)
  {
    for (size_t i = 0; i < buttonMappingCount; i++)
    {
      if (recvButtonData[1] == buttonMappings[i].oldButtonId)
      {
        // OpenHaldex buttons are exclusive; latch buttons are driven by the
        // persistent latched-output path (sendLatchedButtonOutputs). Neither
        // emits momentary LIN/CAN/resistive output from here.
        if (buttonMappings[i].flags & (FLAG_OPENHALDEX_CONTROL | FLAG_LATCH))
        {
          buttonFound = true;
          break;
        }

        transButtonDataLIN[1] = buttonMappings[i].newLinButtonId;

        // Extend the CAN hold window on every LIN poll cycle that sees this button active
        if (buttonMappings[i].canByteIndex < 8 && buttonMappings[i].canBitIndex < 8)
        {
          const uint32_t holdUntil = millis() + static_cast<uint32_t>(canHoldMs);
          portENTER_CRITICAL(&stateMux);
          memset(canHoldFrame, 0, sizeof(canHoldFrame));
          canHoldFrame[buttonMappings[i].canByteIndex] = static_cast<uint8_t>(1U << buttonMappings[i].canBitIndex);
          canHoldUntil = holdUntil;
          // Mirror immediately so the API status reflects CAN data in the same poll as the button name.
          memcpy(lastCanOutFrame, canHoldFrame, sizeof(lastCanOutFrame));
          lastCanOutLen = sizeof(lastCanOutFrame);
          lastCanOutId = canBroadcastId;
          portEXIT_CRITICAL(&stateMux);
        }

        radioResistance = buttonMappings[i].resistiveOhm;
        radioResistanceMs = millis();  // refresh each poll the button is held

        // LIN output to chassis bus — only when enabled by user
        if (linOutputEnabled)
        {
          if (!lockLinBus(chassisLinMutex))
          {
            buttonFound = true;
            break;
          }

          chassisLIN.resetStateMachine();
          chassisLIN.resetError();
          chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linOutputId, 8, transButtonDataLIN);

          if (chassisLIN.getError() == LIN_Master_Base::NO_ERROR)
          {
            chassisLinLastOkMs = millis();  // LIN 2 healthy
          }

          unlockLinBus(chassisLinMutex);
        }

        buttonFound = true;
        break;
      }
    }
  }

  portENTER_CRITICAL(&stateMux);
  memcpy(lastLinOutFrame, transButtonDataLIN, sizeof(lastLinOutFrame));
  lastLinOutLen = sizeof(lastLinOutFrame);
  lastLinOutId = linButtonID;
  portEXIT_CRITICAL(&stateMux);

}

// Persistent output for latched buttons: while a button's latch is engaged its
// chassis-LIN frame is resent (and its resistance refreshed) every poll, so the
// chassis sees the button held until it is pressed again. The CAN side is kept
// active in broadcastButtonsCAN(). Called each LIN cycle after sendButtonLINFrame().
void sendLatchedButtonOutputs()
{
  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (!buttonLatched[i])
    {
      continue;
    }

    const ButtonMapping& m = buttonMappings[i];
    if (m.flags & FLAG_OPENHALDEX_CONTROL)
    {
      continue;  // OpenHaldex buttons are exclusive and never latch outputs
    }

    // Keep the mapped resistance asserted while latched.
    if (m.resistiveOhm != 0)
    {
      radioResistance = m.resistiveOhm;
      radioResistanceMs = millis();
    }

    // Resend the translated button ID on the chassis LIN bus each cycle.
    if (linOutputEnabled && m.newLinButtonId != 0)
    {
      uint8_t latchedFrame[8] = {0};
      latchedFrame[1] = m.newLinButtonId;

      if (lockLinBus(chassisLinMutex))
      {
        chassisLIN.resetStateMachine();
        chassisLIN.resetError();
        chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linOutputId, 8, latchedFrame);
        if (chassisLIN.getError() == LIN_Master_Base::NO_ERROR)
        {
          chassisLinLastOkMs = millis();
        }
        unlockLinBus(chassisLinMutex);
      }

      portENTER_CRITICAL(&stateMux);
      memcpy(lastLinOutFrame, latchedFrame, sizeof(lastLinOutFrame));
      lastLinOutLen = sizeof(lastLinOutFrame);
      lastLinOutId = linButtonID;
      portEXIT_CRITICAL(&stateMux);
    }
  }
}

// Poll the accessory-button frame on the steering-wheel bus. Its button byte is
// fed into the same mapping pipeline as the main button frame, so acc buttons
// behave identically. Default layout mirrors the main frame (button in byte 1).
void getAccButtonState()
{
  uint8_t buf[8] = {0};

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linAccInId, 8, buf);

  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();
  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    memset(buf, 0, sizeof(buf));
  }
  else
  {
    swLinLastOkMs = millis();
  }

  unlockLinBus(steeringWheelLinMutex);

  portENTER_CRITICAL(&stateMux);
  memcpy(lastAccInFrame, buf, sizeof(lastAccInFrame));
  lastAccInLen = sizeof(lastAccInFrame);
  portEXIT_CRITICAL(&stateMux);

  const uint8_t accButton = buf[1];
  if (accButton != 0)
  {
    latestLinButtonId = accButton;
    latestLinButtonTimestamp = millis();
    captureLearnedButton(accButton);
    // Merge into the momentary output pipeline only when the main frame is idle.
    if (recvButtonData[1] == 0)
    {
      recvButtonData[1] = accButton;
    }
  }

  static uint8_t prevAccRaw = 0;
  if (accButton != 0 && prevAccRaw == 0)
  {
    logLine("[SW] acc id=0x%02X on 0x%02X", accButton, linAccInId);
    handleButtonPressEvent(accButton);
  }
  prevAccRaw = accButton;
}

// Poll the temperature frame on the steering-wheel bus. Captured for the LIN
// monitor / status only — no control action is taken on it yet.
void getTemperatureState()
{
  uint8_t buf[8] = {0};

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linTempInId, 8, buf);

  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();

  unlockLinBus(steeringWheelLinMutex);

  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    return;  // keep last known value
  }
  swLinLastOkMs = millis();

  portENTER_CRITICAL(&stateMux);
  memcpy(lastTempInFrame, buf, sizeof(lastTempInFrame));
  lastTempInLen = sizeof(lastTempInFrame);
  portEXIT_CRITICAL(&stateMux);
}

// Poll a single protected ID on the given bus; returns true on a clean response.
static bool probeLinId(LIN_Master_HardwareSerial_ESP32& bus, SemaphoreHandle_t mutex,
                       uint8_t id, uint8_t* out)
{
  bool responded = false;
  if (!lockLinBus(mutex))
  {
    return false;
  }
  bus.resetStateMachine();
  bus.resetError();
  bus.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, id, 8, out);
  responded = (bus.getError() == LIN_Master_Base::NO_ERROR);
  unlockLinBus(mutex);
  return responded;
}

// Enumerate protected IDs 0x00..0x3F on both LIN buses, recording responders.
// Runs on the steering-wheel LIN task so it owns the bus state machine.
void runLinScan()
{
  linScanActive = true;
  logLine("[SCAN] start");

  LinScanResult tmp[kMaxLinScanResults];
  size_t count = 0;

  struct { LIN_Master_HardwareSerial_ESP32* bus; SemaphoreHandle_t mutex; uint8_t tag; }
  buses[2] = {
      {&steeringWheelLIN, steeringWheelLinMutex, 1},
      {&chassisLIN,       chassisLinMutex,       2},
  };

  for (uint8_t b = 0; b < 2; b++)
  {
    for (uint8_t id = 0x00; id <= 0x3F && count < kMaxLinScanResults; id++)
    {
      uint8_t buf[8] = {0};
      if (probeLinId(*buses[b].bus, buses[b].mutex, id, buf))
      {
        tmp[count].bus = buses[b].tag;
        tmp[count].id = id;
        tmp[count].len = 8;
        memcpy(tmp[count].data, buf, 8);
        tmp[count].responded = true;
        count++;
        if (buses[b].tag == 1) { swLinLastOkMs = millis(); }
        else { chassisLinLastOkMs = millis(); }
      }
    }
  }

  portENTER_CRITICAL(&stateMux);
  memcpy(linScanResults, tmp, count * sizeof(LinScanResult));
  linScanResultCount = count;
  portEXIT_CRITICAL(&stateMux);

  linScanDoneMs = millis();
  linScanActive = false;
  logLine("[SCAN] done: %u responders", (unsigned)count);
}

// Re-poll only the known responder IDs to refresh their live bytes, so the UI
// can spot which frame changes when a button is pressed during discovery.
void refreshLinScanLiveData()
{
  const size_t n = linScanResultCount;
  for (size_t i = 0; i < n && i < kMaxLinScanResults; i++)
  {
    uint8_t buf[8] = {0};
    LIN_Master_HardwareSerial_ESP32& bus =
        (linScanResults[i].bus == 2) ? chassisLIN : steeringWheelLIN;
    SemaphoreHandle_t mutex =
        (linScanResults[i].bus == 2) ? chassisLinMutex : steeringWheelLinMutex;
    if (probeLinId(bus, mutex, linScanResults[i].id, buf))
    {
      portENTER_CRITICAL(&stateMux);
      memcpy(linScanResults[i].data, buf, 8);
      portEXIT_CRITICAL(&stateMux);
    }
  }
}
