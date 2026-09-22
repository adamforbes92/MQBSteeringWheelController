#include "CAN.h"

#include <driver/twai.h>

#include "globals.h"
#include "savvycan.h"

// Frames handed to the driver, and frames it refused. "Accepted" means queued
// for transmission, not acknowledged on the wire — but a refusal is a hard
// fact, and a count that doesn't move while a mapped button is held tells you
// the problem is configuration, not the bus.
volatile uint32_t canTxAccepted = 0;
volatile uint32_t canTxRefused  = 0;

static bool twaiSendStandardFrame(uint32_t id, const uint8_t* data, uint8_t length) {
  twai_message_t frame = {};
  frame.identifier = id;
  frame.extd = 0;
  frame.rtr = 0;
  frame.data_length_code = length;
  memcpy(frame.data, data, length);

  const bool ok = twai_transmit(&frame, pdMS_TO_TICKS(10)) == ESP_OK;
  if (ok) {
    canTxAccepted++;
  } else {
    canTxRefused++;
  }
  return ok;
}

// VW runs more than one CAN speed: powertrain at 500 kbit/s, but the comfort /
// infotainment bus — where a radio listens for steering-wheel buttons on a PQ
// car — at 100 kbit/s. A node at the wrong speed cannot exchange a single
// frame with that bus, and the error frames it provokes drive it straight into
// bus-off. Selected in Setup; applied here at boot only, like the LIN pins.
static twai_timing_config_t timingForKbit(uint16_t kbit) {
  switch (kbit) {
    case 100: return TWAI_TIMING_CONFIG_100KBITS();
    case 125: return TWAI_TIMING_CONFIG_125KBITS();
    case 250: return TWAI_TIMING_CONFIG_250KBITS();
    case 500:
    default:  return TWAI_TIMING_CONFIG_500KBITS();
  }
}

void canInit() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(pinCAN_TX), static_cast<gpio_num_t>(pinCAN_RX), TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = timingForKbit(canBitrateKbit);
  DEBUG("TWAI bitrate %u kbit/s", (unsigned)canBitrateKbit);
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

// Bus-off recovery. The TWAI driver never leaves bus-off by itself: once the
// transmit error counter overflows — which transmitting into a bus with no
// other node to ACK does within a fraction of a second — both TX and RX stop,
// and stay stopped until twai_initiate_recovery() is called. Recovery then
// lands the controller in STOPPED, which needs twai_start() again. Without
// this, one bad moment (car off, loose wire, bench with only a listen-only
// sniffer attached) silently kills CAN until the next reboot, which reads to a
// user as "broadcasting doesn't work" with no indication why.
static void serviceCanBusState() {
  static bool wasBusOff = false;

  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) {
    return;
  }

  if (st.state == TWAI_STATE_BUS_OFF) {
    if (!wasBusOff) {
      logLine("CAN bus-off (tx_err=%u): recovering", (unsigned)st.tx_error_counter);
      wasBusOff = true;
    }
    twai_initiate_recovery();
  } else if (st.state == TWAI_STATE_STOPPED) {
    if (twai_start() == ESP_OK && wasBusOff) {
      logLine("CAN restarted after bus-off");
    }
    wasBusOff = false;
  }
}

const char* canBusStateName() {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) {
    return "not installed";
  }
  switch (st.state) {
    case TWAI_STATE_RUNNING:    return "running";
    case TWAI_STATE_BUS_OFF:    return "bus-off";
    case TWAI_STATE_RECOVERING: return "recovering";
    case TWAI_STATE_STOPPED:    return "stopped";
    default:                    return "?";
  }
}

void pollCanRx() {
  twai_message_t frame = {};

  serviceCanBusState();

  while (twai_receive(&frame, 0) == ESP_OK) {
    lastCanRxMs = millis();  // record valid frame arrival for CAN health

    // CAN monitor (off unless enabled) — standard 11-bit frames only, which is
    // everything this device deals with.
    if (!frame.extd) {
      logCanFrame((uint16_t)frame.identifier, frame.data, frame.data_length_code);
    }

    // SavvyCAN analyzer stream (no-op unless a mode is active). Unlike the
    // monitor above this forwards everything, extended frames included.
    analyzerQueueFrame(frame, 0);

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

// Resend the target mode until the OpenHaldex
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

// Write a 4-bit target-program field at a DBC start bit. Every field selected
// in kCharismaParticipants is nibble-aligned, so none straddles a byte.
static void setProgramField(uint8_t* data, uint8_t startBit, uint8_t program) {
  const uint8_t idx   = (uint8_t)(startBit / 8);
  const uint8_t shift = (uint8_t)(startBit % 8);
  data[idx] = (uint8_t)((data[idx] & ~(0x0F << shift)) | ((program & 0x0F) << shift));
}

static void setBit(uint8_t* data, uint8_t startBit) {
  data[startBit / 8] |= (uint8_t)(1U << (startBit % 8));
}

// Charisma (Drive Select) output. Transmitted cyclically and again immediately
// whenever the state changes, which is what the real nodes do — a purely
// cyclic sender would drop a short press between frames.
//
// 1 Hz is the MQB matrix's GenMsgCycleTime for all three MQB messages. The PQ
// matrix carries no cycle-time attribute at all, so the same rate is used there
// for want of a measured one; Gate_Komf_1 on a real PQ comfort bus is likely
// faster, and that is worth checking against a capture before trusting it.
//
// Nothing is transmitted at all while charismaMode is OFF, so this stays
// entirely inert until it is deliberately turned on. See defs.h for why the
// button/coordinator choice is the user's rather than ours.
void serviceCharisma() {
  if (charismaMode == CHARISMA_MODE_OFF) {
    return;
  }

  const uint32_t now = millis();
  const bool pressActive =
      charismaPressMs != 0 && (now - charismaPressMs) < kCharismaPressHoldMs;

  static uint32_t lastSendMs = 0;
  static bool     lastPress  = false;
  static uint8_t  lastProg   = 0;
  static uint8_t  lastMode   = CHARISMA_MODE_OFF;

  const bool changed = (pressActive != lastPress) || (charismaProgram != lastProg) ||
                       (charismaMode != lastMode);
  if (!changed && (now - lastSendMs) < 1000UL) {
    return;
  }
  lastPress  = pressActive;
  lastProg   = charismaProgram;
  lastMode   = charismaMode;
  lastSendMs = now;

  if (charismaMode == CHARISMA_MODE_MQB_BUTTON) {
    // Emulate the dash button: the car's own coordinator does the cycling.
    uint8_t bcm[8] = {0};
    if (pressActive) {
      setBit(bcm, charismaButtonBit);
    }
    twaiSendStandardFrame(BCM_01_ID, bcm, sizeof(bcm));
    return;
  }

  if (charismaMode == CHARISMA_MODE_PQ_COORD) {
    // PQ: one mode value for the whole car, in Gate_Komf_1's multiplexed
    // sub-frame 1. Every other signal in this message goes out as zero, which
    // is why this is the riskiest option here — see defs.h.
    uint8_t gk[8] = {0};
    setProgramField(gk, PQ_CHARISMA_MODE_BIT, charismaProgram);
    setProgramField(gk, PQ_SAMFKTNR_BIT, PQ_SAMFKT_CHARISMA);
    twaiSendStandardFrame(GATE_KOMF_1_ID, gk, sizeof(gk));
    return;
  }

  // MQB coordinator: name the target program for each selected participant, and
  // report it as the current mode. CHA_Fahrer_Umschaltung marks the change as
  // a manual one by the driver so a participant can tell it from a retry.
  uint8_t cha01[8] = {0};
  uint8_t cha07[8] = {0};
  bool send01 = false;

  for (uint8_t i = 0; i < kCharismaParticipantCount; i++) {
    if (!(charismaParticipants & (1U << i))) {
      continue;
    }
    setProgramField(cha01, kCharismaParticipants[i].startBit, charismaProgram);
    send01 = true;
  }

  setProgramField(cha07, CHARISMA_CURRENT_MODE_BIT, charismaProgram);
  if (pressActive) {
    setBit(cha01, CHARISMA_MANUAL_FLAG_BIT);
    setBit(cha07, CHARISMA_MANUAL_FLAG_BIT);
  }

  if (send01) {
    twaiSendStandardFrame(CHARISMA_01_ID, cha01, sizeof(cha01));
  }
  twaiSendStandardFrame(CHARISMA_07_ID, cha07, sizeof(cha07));
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
    // Both simultaneously → send command 0x03 (combined paddle request).
    if (paddlesEnabled && dsgPaddleUp && dsgPaddleDown) {
      dsgPaddleUp    = false;
      dsgPaddleDown  = false;
      activeCommand  = 0x03;
      activeUntilMs  = millis() + kGraPulseMs;
      DEBUG("GRA: simultaneous up+down — command 0x03");
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
