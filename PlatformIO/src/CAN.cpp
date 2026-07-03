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
#if ChassisCANDebug
    DEBUG_("Length Recv: %u CAN ID: 0x%03X Buffer: ", frame.data_length_code, frame.identifier);
    for (uint8_t i = 0; i < frame.data_length_code; i++) {
      DEBUG_("%02X ", frame.data[i]);
    }
    DEBUG("");
#endif
  }
}

void broadcastButtonsCAN() {
  uint8_t payload[8] = {0};
  const uint32_t now = millis();
  bool holdActive = false;
  portENTER_CRITICAL(&stateMux);
  if (now < canHoldUntil) {
    memcpy(payload, canHoldFrame, sizeof(payload));
    holdActive = true;
  }
  portEXIT_CRITICAL(&stateMux);

  // Always update the display state (dashboard) regardless of whether CAN TX is enabled.
  // The hold-window expiry here is what clears the bits after button release.
  portENTER_CRITICAL(&stateMux);
  memcpy(lastCanOutFrame, payload, sizeof(lastCanOutFrame));
  lastCanOutLen = sizeof(lastCanOutFrame);
  lastCanOutId = canBroadcastId;
  portEXIT_CRITICAL(&stateMux);

  // Only transmit when there is actually a button held — avoids flooding the
  // bus (and the debug log) with zero frames every 50 ms when idle.
  if (!canBroadcastEnabled || !holdActive) {
    return;
  }

  if (!twaiSendStandardFrame(canBroadcastId, payload, sizeof(payload))) {
    DEBUG("Chassis TWAI Write TX Fail!");
    return;
  }
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
