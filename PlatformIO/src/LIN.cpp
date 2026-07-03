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
  chassisLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, buf);

  const LIN_Master_Base::error_t linErr = chassisLIN.getError();

  unlockLinBus(chassisLinMutex);

  if (linErr == LIN_Master_Base::NO_ERROR)
  {
    // Only update on a clean receive — preserves last known brightness on missed frames.
    memcpy(gatewayLightData, buf, sizeof(gatewayLightData));
    chassisLinLastOkMs = millis();  // LIN 2 healthy
  }
  else
  {
    static uint32_t lastErrMs = 0;
    const uint32_t nowMs = millis();
    if (nowMs - lastErrMs >= 1000)
    {
      lastErrMs = nowMs;
      DEBUG("Chassis LIN light RX error 0x%02X — keeping last value", static_cast<uint8_t>(linErr));
    }
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
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linButtonID, 8, recvButtonData);

  // Discard the frame if the library flagged any error (timeout, checksum,
  // echo mismatch, etc.).  Without this, partial bytes written by a failed
  // receive leave garbage in recvButtonData[1] which matches a button mapping
  // and makes a button appear pressed when nothing was touched.
  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();
  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    static uint32_t linErrLast = 0;
    const uint32_t nowMs = millis();
    if (nowMs - linErrLast >= 1000)
    {
      linErrLast = nowMs;
      DEBUG("SW LIN RX error 0x%02X — frame discarded", static_cast<uint8_t>(linErr));
    }
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
  lastLinInId = linButtonID;
  portEXIT_CRITICAL(&stateMux);

  if (recvButtonData[1] != 0)
  {
    latestLinButtonId = recvButtonData[1];
    latestLinButtonTimestamp = millis();
    captureLearnedButton(recvButtonData[1]);
  }
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
  steeringWheelLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightID, 4, steeringWheelLightData);

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
    DEBUG("Button ID: %u", recvButtonData[1]);

    for (size_t i = 0; i < buttonMappingCount; i++)
    {
      if (recvButtonData[1] == buttonMappings[i].oldButtonId)
      {
        DEBUG("Button: %s", buttonMappings[i].name);

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

  if (!buttonFound && recvButtonData[1] != 0)
  {
    DEBUG("Button not found, program it?");
  }
}
