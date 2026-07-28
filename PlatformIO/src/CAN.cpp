#include "CAN.h"

#include <driver/twai.h>

#include "globals.h"

static bool twaiSendStandardFrame(uint32_t id, const uint8_t* data, uint8_t length) {
  twai_message_t frame = {};
  frame.identifier = id;
  frame.extd = 0;
  frame.rtr = 0;
  frame.data_length_code = length;
  memcpy(frame.data, data, length);

  return twai_transmit(&frame, pdMS_TO_TICKS(10)) == ESP_OK;
}

void canInit() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(pinCAN_TX), static_cast<gpio_num_t>(pinCAN_RX), TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t installResult = twai_driver_install(&g_config, &t_config, &f_config);
  if (installResult != ESP_OK) {
    DEBUG("TWAI install failed: %d", installResult);
    return;
  }

  esp_err_t startResult = twai_start();
  if (startResult != ESP_OK) {
    DEBUG("TWAI start failed: %d", startResult);
  }
}

bool canHealthy() {
  // Healthy = a valid CAN frame was received recently (frames coming in).
  return lastCanRxMs != 0 && (millis() - lastCanRxMs) < 1000UL;
}

void pollCanRx() {
  twai_message_t frame = {};

  while (twai_receive(&frame, 0) == ESP_OK) {
    lastCanRxMs = millis();  // record valid frame arrival for CAN health

    // OpenHaldex live-state broadcast: data[6] carries the current mode.
    // Used to confirm commanded mode changes and to seed "push-to-next".
    if (frame.identifier == OPENHALDEX_BROADCAST_ID && frame.data_length_code >= 7) {
      openHaldexCurrentMode = frame.data[6];
      openHaldexLastRxMs = millis();
    }

    DEBUG_CHASSIS_CAN_("RX length=%u ID=0x%03X data=", frame.data_length_code, frame.identifier);
    for (uint8_t i = 0; i < frame.data_length_code; i++) {
      DEBUG_CHASSIS_CAN_("%02X ", frame.data[i]);
    }
    DEBUG_CHASSIS_CAN("");
  }
}

// Send a single OpenHaldex external-control frame requesting the given mode.
void sendOpenHaldexMode(uint8_t mode) {
  uint8_t data[8] = {0};
  data[0] = mode;
  if (!twaiSendStandardFrame(OPENHALDEX_EXTERNAL_CONTROL_ID, data, sizeof(data))) {
    DEBUG("OpenHaldex TX Fail!");
  }
}

// Closed-loop mode control: resend the target mode until the OpenHaldex
// broadcast confirms it changed, then stop. Times out to avoid endless
// retransmission if the OpenHaldex unit is absent or CAN-broadcast disabled.
void serviceOpenHaldex() {
  if (openHaldexTargetMode == OPENHALDEX_MODE_UNKNOWN) {
    return;  // no command active
  }

  const uint32_t now = millis();

  // Confirmed by a recent broadcast matching the target -> done.
  if (openHaldexCurrentMode == openHaldexTargetMode &&
      openHaldexLastRxMs != 0 && (now - openHaldexLastRxMs) < 1000UL) {
    openHaldexTargetMode = OPENHALDEX_MODE_UNKNOWN;
    return;
  }

  // Give up after 3 s so a missing OpenHaldex doesn't spam the bus forever.
  if ((now - openHaldexCmdStartMs) > 3000UL) {
    openHaldexTargetMode = OPENHALDEX_MODE_UNKNOWN;
    return;
  }

  // Resend at ~10 Hz until confirmed.
  if (openHaldexLastSendMs == 0 || (now - openHaldexLastSendMs) >= 100UL) {
    sendOpenHaldexMode(openHaldexTargetMode);
    openHaldexLastSendMs = now;
  }
}

void broadcastButtonsCAN() {
  static bool activeFrameSent = false;
  uint8_t payload[8] = {0};
  const uint32_t now = millis();
  bool holdActive = false;
  portENTER_CRITICAL(&stateMux);
  if (now < canHoldUntil) {
    memcpy(payload, canHoldFrame, sizeof(payload));
    holdActive = true;
  }
  portEXIT_CRITICAL(&stateMux);

  // OR in the bits for any latched buttons so their CAN state persists until
  // the button is pressed again (unlatched). OpenHaldex buttons are exclusive
  // and never contribute CAN button bits here.
  for (size_t i = 0; i < buttonMappingCount; i++) {
    if (!buttonLatched[i]) {
      continue;
    }
    const ButtonMapping& m = buttonMappings[i];
    if (m.flags & FLAG_OPENHALDEX_CONTROL) {
      continue;
    }
    if (m.canByteIndex < 8 && m.canBitIndex < 8) {
      payload[m.canByteIndex] |= static_cast<uint8_t>(1U << m.canBitIndex);
      holdActive = true;
    }
  }

  // Always update the display state (dashboard) regardless of whether CAN TX is enabled.
  // The hold-window expiry here is what clears the bits after button release.
  portENTER_CRITICAL(&stateMux);
  memcpy(lastCanOutFrame, payload, sizeof(lastCanOutFrame));
  lastCanOutLen = sizeof(lastCanOutFrame);
  lastCanOutId = canBroadcastId;
  portEXIT_CRITICAL(&stateMux);

  // Send each held state continuously, then one all-zero release frame when
  // the hold window ends. Staying silent after an active frame makes a
  // stateful receiver retain the prior button state indefinitely.
  if (!canBroadcastEnabled || (!holdActive && !activeFrameSent)) {
    return;
  }

  if (!twaiSendStandardFrame(canBroadcastId, payload, sizeof(payload))) {
    DEBUG("Chassis TWAI Write TX Fail!");
    return;
  }

  activeFrameSent = holdActive;
}

void broadcastGRATask(void* parameter) {
  (void)parameter;

  static const uint32_t kGraPulseMs   = 80;  // how long to hold the paddle command active
  static const uint32_t kGraRefreshMs = 20;  // GRA frame broadcast interval

  uint8_t  counter       = 0;
  uint8_t  activeCommand = 0x00;
  uint32_t activeUntilMs = 0;

  while (true) {
    // Latch pending paddle inputs.
    // Both simultaneously → cancel each other (same as Can2Cluster behaviour).
    if (dsgPaddleUp && dsgPaddleDown) {
      dsgPaddleUp   = false;
      dsgPaddleDown = false;
      DEBUG("GRA: simultaneous up+down — cancelled");
    } else if (paddlesEnabled && dsgPaddleUp) {
      dsgPaddleUp    = false;
      activeCommand  = 0x02;
      activeUntilMs  = millis() + kGraPulseMs;
      DEBUG("GRA: paddle up");
    } else if (paddlesEnabled && dsgPaddleDown) {
      dsgPaddleDown  = false;
      activeCommand  = 0x01;
      activeUntilMs  = millis() + kGraPulseMs;
      DEBUG("GRA: paddle down");
    } else {
      // Paddles disabled — discard any pending requests silently.
      dsgPaddleUp   = false;
      dsgPaddleDown = false;
    }

    // Build the GRA frame.
    // Layout (matches Can2Cluster broadcastGRA):
    //   [0] CRC   = data[2] ^ data[3]  (XOR of counter and command)
    //   [1] 0x00  always
    //   [2] counter — free-running 0x00 → 0xFF
    //   [3] command — 0x02 up / 0x01 down / 0x00 idle
    twai_message_t frame = {};
    frame.identifier       = GRA_ID;
    frame.data_length_code = 4;
    frame.data[1]          = 0x00;
    frame.data[2]          = counter;

    if (activeCommand != 0x00 && (int32_t)(millis() - activeUntilMs) < 0) {
      frame.data[3] = activeCommand;
    } else {
      frame.data[3]  = 0x00;
      activeCommand  = 0x00;
    }

    frame.data[0] = frame.data[2] ^ frame.data[3];  // CRC

    // Only put frames on the bus when paddles are enabled.
    if (paddlesEnabled) {
      if (twai_transmit(&frame, pdMS_TO_TICKS(10)) != ESP_OK) {
        DEBUG("GRA: transmit failed");
      }
    }

    counter++;
    vTaskDelay(pdMS_TO_TICKS(kGraRefreshMs));
  }
}
