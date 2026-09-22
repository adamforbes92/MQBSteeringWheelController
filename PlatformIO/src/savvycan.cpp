#include "savvycan.h"

#include "globals.h"
#include <WiFi.h>
#include <driver/twai.h>

// ============================================================================
// SavvyCAN / CAN Analyzer — GVRET (WiFi + Serial) and Lawicel/SLCAN (WiFi)
// ============================================================================
// Ported from Can2Cluster (originally OpenHaldex / SpeedPulserPro).
// Single TWAI bus at 500 kbit/s; standard twai_transmit() API.
//
// Frame flow:
//   pollCanRx() (CAN.cpp)
//     -> analyzerQueueFrame(frame, 0)  [called for every received frame]
//       -> analyzerTask dequeues + sends to SavvyCAN via WiFi or Serial

namespace {

static const uint16_t kAnalyzerPort           = 23;
static const uint32_t kAnalyzerPollDelayMs    = 1;
static const size_t   kAnalyzerQueueDepth     = 8;
static const uint32_t kGvretControlWriteTimeoutMs = 250;

struct AnalyzerFrame {
  twai_message_t frame;
  uint8_t  bus;        // always 0 for Can2Cluster (single bus)
  uint32_t timestamp;
};

static QueueHandle_t analyzerQueue       = nullptr;
static WiFiServer    analyzerServer(kAnalyzerPort);
static WiFiClient    analyzerClient;
static uint8_t       analyzerActiveProtocol = ANALYZER_PROTOCOL_GVRET;
static bool          analyzerServerStarted  = false;

// ============================================================================
// GVRET binary protocol (SavvyCAN)
// ============================================================================
enum GvretState {
  GVRET_WAIT,
  GVRET_CMD,
  GVRET_PAYLOAD
};

static GvretState gvretState       = GVRET_WAIT;
static uint8_t    gvretCommand     = 0;
static uint8_t    gvretPayload[32];
static size_t     gvretIndex       = 0;
static size_t     gvretExpected    = 0;
static bool       gvretBinaryEnabled = false;
static uint8_t    gvretE7Count     = 0;

static void resetGvretParser() {
  gvretState        = GVRET_WAIT;
  gvretCommand      = 0;
  gvretIndex        = 0;
  gvretExpected     = 0;
  gvretBinaryEnabled = false;
  gvretE7Count      = 0;
}

// Control replies are part of the GVRET handshake; deliver them reliably.
static bool gvretWriteBlocking(const uint8_t *data, size_t len, uint32_t timeoutMs) {
  if (analyzerSerial) {
    Serial.write(data, len);
    return true;
  }

  if (!analyzerClient || !analyzerClient.connected()) {
    return false;
  }

  uint32_t start = millis();
  size_t sent = 0;
  while (sent < len) {
    if (!analyzerClient.connected()) {
      return false;
    }
    size_t written = analyzerClient.write(data + sent, len - sent);
    if (written > 0) {
      sent += written;
      continue;
    }
    if (millis() - start > timeoutMs) {
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}

// Frame streaming is best-effort; drop if TCP can't keep up.
static bool gvretWriteNonBlocking(const uint8_t *data, size_t len) {
  if (analyzerSerial) {
    Serial.write(data, len);
    return true;
  }

  if (!analyzerClient || !analyzerClient.connected()) {
    return false;
  }

  size_t written = analyzerClient.write(data, len);
  return written == len;
}

static void gvretSendNumBuses() {
  // This device has a single CAN bus.
  const uint8_t payload[] = { 0xF1, 0x0C, 0x01 };
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendBusInfo() {
  // Single bus at 500 kbit/s.
  const uint16_t speedKbit = 500;
  const uint8_t payload[] = {
    0xF1, 0x06,
    0x01, 0x00,                                                   // can0: enabled, not listen-only
    (uint8_t)(speedKbit & 0xFF), (uint8_t)((speedKbit >> 8) & 0xFF),
    0x00, 0x00, 0x00,                                             // can1: disabled
    0x00, 0x00,
    0x00                                                          // reserved
  };
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendExtendedBusInfo() {
  uint8_t payload[17] = { 0 };
  payload[0] = 0xF1;
  payload[1] = 0x0D;
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendDeviceInfo() {
  const uint8_t payload[] = { 0xF1, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendValidation() {
  const uint8_t payload[] = { 0xF1, 0x09 };
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendTimeSync() {
  uint32_t now = micros();
  uint8_t payload[6];
  payload[0] = 0xF1;
  payload[1] = 0x01;
  payload[2] = (uint8_t)(now & 0xFF);
  payload[3] = (uint8_t)((now >> 8) & 0xFF);
  payload[4] = (uint8_t)((now >> 16) & 0xFF);
  payload[5] = (uint8_t)((now >> 24) & 0xFF);
  gvretWriteBlocking(payload, sizeof(payload), kGvretControlWriteTimeoutMs);
}

static void gvretSendFrame(const AnalyzerFrame &entry) {
  uint8_t payload[22];
  uint32_t id = entry.frame.identifier & 0x1FFFFFFF;
  if (entry.frame.extd) {
    id |= 0x80000000;
  }

  payload[0]  = 0xF1;
  payload[1]  = 0x00;
  payload[2]  = (uint8_t)(entry.timestamp & 0xFF);
  payload[3]  = (uint8_t)((entry.timestamp >> 8) & 0xFF);
  payload[4]  = (uint8_t)((entry.timestamp >> 16) & 0xFF);
  payload[5]  = (uint8_t)((entry.timestamp >> 24) & 0xFF);
  payload[6]  = (uint8_t)(id & 0xFF);
  payload[7]  = (uint8_t)((id >> 8) & 0xFF);
  payload[8]  = (uint8_t)((id >> 16) & 0xFF);
  payload[9]  = (uint8_t)((id >> 24) & 0xFF);
  payload[10] = (uint8_t)(((entry.bus & 0x0F) << 4) | (entry.frame.data_length_code & 0x0F));

  for (uint8_t i = 0; i < entry.frame.data_length_code && i < 8; ++i) {
    payload[11 + i] = entry.frame.data[i];
  }

  gvretWriteNonBlocking(payload, 11 + entry.frame.data_length_code);
}

static void gvretTransmitFrameFromHost() {
  if (gvretIndex < 7) {
    return;
  }

  uint32_t rawId = (uint32_t)gvretPayload[0]
                 | ((uint32_t)gvretPayload[1] << 8)
                 | ((uint32_t)gvretPayload[2] << 16)
                 | ((uint32_t)gvretPayload[3] << 24);

  uint8_t dlc = gvretPayload[5];

  twai_message_t msg = { 0 };
  msg.extd               = (rawId & 0x80000000UL) != 0;
  msg.rtr                = 0;
  msg.data_length_code   = dlc > 8 ? 8 : dlc;
  msg.identifier         = msg.extd ? (rawId & 0x1FFFFFFF) : (rawId & 0x7FF);

  for (uint8_t i = 0; i < msg.data_length_code; ++i) {
    msg.data[i] = gvretPayload[6 + i];
  }

  twai_transmit(&msg, pdMS_TO_TICKS(5));
}

static void gvretProcessCommand() {
  switch (gvretCommand) {
    case 0x00: gvretTransmitFrameFromHost(); break;
    case 0x01: gvretSendTimeSync();          break;
    case 0x06: gvretSendBusInfo();           break;
    case 0x07: gvretSendDeviceInfo();        break;
    case 0x09: gvretSendValidation();        break;
    case 0x0C: gvretSendNumBuses();          break;
    case 0x0D: gvretSendExtendedBusInfo();   break;
    case 0x05:  // set CAN params (ignored)
    case 0x0E:  // set extended buses (ignored)
    default:   break;
  }
}

static void gvretHandleByte(uint8_t byteIn) {
  if (!gvretBinaryEnabled) {
    // SavvyCAN sends 0xE7 0xE7 to enter binary GVRET mode.
    if (byteIn == 0xE7) {
      if (++gvretE7Count >= 2) {
        gvretBinaryEnabled = true;
        gvretE7Count = 0;
      }
    } else {
      gvretE7Count = 0;
    }
    return;
  }

  switch (gvretState) {
    case GVRET_WAIT:
      if (byteIn == 0xF1) {
        gvretState = GVRET_CMD;
      }
      break;

    case GVRET_CMD:
      gvretCommand  = byteIn;
      gvretIndex    = 0;
      gvretExpected = 0;
      if (gvretCommand == 0x00) {
        gvretExpected = 6;   // id(4) + bus + len, data follows
        gvretState    = GVRET_PAYLOAD;
      } else if (gvretCommand == 0x05) {
        gvretExpected = 9;
        gvretState    = GVRET_PAYLOAD;
      } else if (gvretCommand == 0x0E) {
        gvretExpected = 13;
        gvretState    = GVRET_PAYLOAD;
      } else {
        gvretProcessCommand();
        gvretState = GVRET_WAIT;
      }
      break;

    case GVRET_PAYLOAD:
      if (gvretIndex < sizeof(gvretPayload)) {
        gvretPayload[gvretIndex++] = byteIn;
      } else {
        gvretState = GVRET_WAIT;
        break;
      }
      if (gvretCommand == 0x00 && gvretIndex == 6) {
        uint8_t dlc   = gvretPayload[5];
        gvretExpected = 6 + dlc + 1;  // include trailing 0 byte
      }
      if (gvretIndex >= gvretExpected) {
        gvretProcessCommand();
        gvretState = GVRET_WAIT;
      }
      break;
  }
}

// ============================================================================
// Lawicel/SLCAN protocol (CANHacker, WiFi only)
// ============================================================================
static char  slcanLine[64];
static size_t slcanLen = 0;

static void resetSlcanParser() {
  slcanLen = 0;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool parseHexByte(const char *text, uint8_t &out) {
  int hi = hexNibble(text[0]);
  int lo = hexNibble(text[1]);
  if (hi < 0 || lo < 0) return false;
  out = (uint8_t)((hi << 4) | lo);
  return true;
}

static uint32_t parseHexId(const char *text, size_t len, bool &ok) {
  ok = true;
  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    int nibble = hexNibble(text[i]);
    if (nibble < 0) { ok = false; return 0; }
    value = (value << 4) | (uint32_t)nibble;
  }
  return value;
}

static void slcanSendAck() {
  if (analyzerSerial) {
    Serial.write('\r');
    return;
  }
  if (analyzerClient && analyzerClient.connected()) {
    analyzerClient.write("\r");
  }
}

static void slcanHandleLine() {
  if (slcanLen == 0) return;

  char cmd = slcanLine[0];

  if (cmd == 'O' || cmd == 'C' || cmd == 'S') { slcanSendAck(); return; }
  if (cmd == 'V') {
    if (analyzerClient && analyzerClient.connected()) analyzerClient.print("V0101\r");
    return;
  }
  if (cmd == 'N') {
    if (analyzerClient && analyzerClient.connected()) analyzerClient.print("N0000\r");
    return;
  }

  bool extended = (cmd == 'T' || cmd == 'R');
  bool remote   = (cmd == 'r' || cmd == 'R');
  if (cmd != 't' && cmd != 'T' && cmd != 'r' && cmd != 'R') { slcanSendAck(); return; }

  size_t idLen = extended ? 8 : 3;
  if (slcanLen < 1 + idLen + 1) { slcanSendAck(); return; }

  bool ok = false;
  uint32_t id = parseHexId(&slcanLine[1], idLen, ok);
  if (!ok) { slcanSendAck(); return; }

  int dlcNibble = hexNibble(slcanLine[1 + idLen]);
  if (dlcNibble < 0) { slcanSendAck(); return; }
  uint8_t dlc = (uint8_t)dlcNibble;
  if (dlc > 8) dlc = 8;

  size_t dataIndex = 1 + idLen + 1;
  if (!remote && slcanLen < dataIndex + (dlc * 2)) { slcanSendAck(); return; }

  twai_message_t msg = { 0 };
  msg.extd             = extended;
  msg.rtr              = remote ? 1 : 0;
  msg.identifier       = extended ? (id & 0x1FFFFFFF) : (id & 0x7FF);
  msg.data_length_code = dlc;

  if (!remote) {
    for (uint8_t i = 0; i < dlc; ++i) {
      if (!parseHexByte(&slcanLine[dataIndex + (i * 2)], msg.data[i])) {
        slcanSendAck();
        return;
      }
    }
  }

  twai_transmit(&msg, pdMS_TO_TICKS(5));
  slcanSendAck();
}

static void slcanHandleByte(uint8_t byteIn) {
  if (byteIn == '\r') {
    slcanHandleLine();
    resetSlcanParser();
    return;
  }
  if (slcanLen + 1 < sizeof(slcanLine)) {
    slcanLine[slcanLen++] = (char)byteIn;
    slcanLine[slcanLen]   = '\0';
  } else {
    resetSlcanParser();
  }
}

static void slcanSendFrame(const AnalyzerFrame &entry) {
  char buffer[40];
  char *ptr = buffer;

  if (entry.frame.extd) {
    ptr += sprintf(ptr, "T%08X%1X", entry.frame.identifier, entry.frame.data_length_code & 0xF);
  } else {
    ptr += sprintf(ptr, "t%03X%1X", entry.frame.identifier, entry.frame.data_length_code & 0xF);
  }

  for (uint8_t i = 0; i < entry.frame.data_length_code; ++i) {
    ptr += sprintf(ptr, "%02X", entry.frame.data[i]);
  }
  *ptr++ = '\r';
  *ptr   = '\0';

  size_t len = (size_t)(ptr - buffer);
  if (analyzerSerial) {
    Serial.write((const uint8_t *)buffer, len);
  } else if (analyzerClient && analyzerClient.connected()) {
    analyzerClient.write((const uint8_t *)buffer, len);
  }
}

// ============================================================================
// Analyzer task
// ============================================================================
static void resetAnalyzerClientState() {
  resetGvretParser();
  resetSlcanParser();
}

static void analyzerCloseClient() {
  if (analyzerClient) {
    analyzerClient.stop();
  }
  resetAnalyzerClientState();
}

static void analyzerTask(void *arg) {
  (void)arg;

  bool     serialStarted  = false;
  uint32_t framesDequeued = 0;

  while (1) {
    if (!analyzerMode && !analyzerSerial) {
      // Neither mode active: shut down and idle.
      analyzerCloseClient();
      analyzerServerStarted = false;
      if (serialStarted) {
        serialStarted = false;
        resetGvretParser();
      }
      vTaskDelay(kAnalyzerPollDelayMs / portTICK_PERIOD_MS);
      continue;
    }

    if (!analyzerQueue) {
      vTaskDelay(kAnalyzerPollDelayMs / portTICK_PERIOD_MS);
      continue;
    }

    // ---- Serial GVRET mode (SavvyCAN over USB at 1 Mbaud) ----
    if (analyzerSerial) {
      if (!serialStarted) {
        Serial.begin(1000000);
        serialStarted = true;
        resetGvretParser();
      }
      // Always GVRET for serial.
      while (Serial.available()) {
        gvretHandleByte((uint8_t)Serial.read());
      }
      AnalyzerFrame entry;
      while (xQueueReceive(analyzerQueue, &entry, 0) == pdTRUE) {
        framesDequeued++;
        gvretSendFrame(entry);
      }
      vTaskDelay(kAnalyzerPollDelayMs / portTICK_PERIOD_MS);
      continue;
    }

    // ---- WiFi GVRET / SLCAN mode ----
    serialStarted = false;

    if (analyzerActiveProtocol != analyzerProtocol) {
      analyzerActiveProtocol = analyzerProtocol;
      analyzerCloseClient();
    }

    if (WiFi.getMode() == WIFI_OFF) {
      analyzerCloseClient();
      analyzerServerStarted = false;
      vTaskDelay(kAnalyzerPollDelayMs / portTICK_PERIOD_MS);
      continue;
    }

    if (!analyzerServerStarted) {
      analyzerServer.begin();
      analyzerServer.setNoDelay(true);
      analyzerServerStarted = true;
      DEBUG("[Analyzer] TCP server started on port %u", kAnalyzerPort);
    }

    // Accept a new connection (only one client at a time).
    if (!analyzerClient || !analyzerClient.connected()) {
      analyzerCloseClient();
      WiFiClient candidate = analyzerServer.available();
      if (candidate) {
        analyzerClient = candidate;
        resetAnalyzerClientState();
        DEBUG("[Analyzer] Client connected");
      }
    }

    // Read incoming bytes from the connected client.
    if (analyzerClient && analyzerClient.connected()) {
      while (analyzerClient.available()) {
        uint8_t b = (uint8_t)analyzerClient.read();
        if (analyzerProtocol == ANALYZER_PROTOCOL_LAWICEL) {
          slcanHandleByte(b);
        } else {
          gvretHandleByte(b);
        }
      }
    }

    // Drain the frame queue and send to client.
    AnalyzerFrame entry;
    while (xQueueReceive(analyzerQueue, &entry, 0) == pdTRUE) {
      framesDequeued++;
      if (analyzerProtocol == ANALYZER_PROTOCOL_LAWICEL) {
        slcanSendFrame(entry);
      } else {
        gvretSendFrame(entry);
      }
    }

    vTaskDelay(kAnalyzerPollDelayMs / portTICK_PERIOD_MS);
  }
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

void setupAnalyzer() {
  if (!analyzerQueue) {
    analyzerQueue = xQueueCreate(kAnalyzerQueueDepth, sizeof(AnalyzerFrame));
  }
  // Pinned to core 0 at a modest priority, unlike the source project's
  // unpinned priority 13. Core 1 carries chassisLinListenerTask, which has
  // only a few byte-times to answer the BCM's LIN header; a higher-priority
  // analyzer task sharing that core could preempt it mid-response and break
  // the button relay whenever someone opens SavvyCAN.
  xTaskCreatePinnedToCore(analyzerTask, "analyzer", 4096, NULL, 3, NULL, 0);
}

void setAnalyzerMode(bool enable) {
  analyzerMode = enable;
  if (analyzerMode) {
    analyzerSerial = false;  // mutually exclusive with Serial mode
  }
  if (!analyzerMode && analyzerQueue) {
    xQueueReset(analyzerQueue);
  }
}

void setAnalyzerSerialMode(bool enable) {
  analyzerSerial = enable;
  if (analyzerSerial) {
    analyzerMode = false;  // mutually exclusive with WiFi mode
  }
  if (!analyzerSerial && analyzerQueue) {
    xQueueReset(analyzerQueue);
  }
}

void analyzerQueueFrame(const twai_message_t &frame, uint8_t bus) {
  static uint32_t framesEnqueued = 0;
  static uint32_t framesDropped  = 0;

  if ((!analyzerMode && !analyzerSerial) || !analyzerQueue) {
    return;
  }

  // Lawicel/SLCAN only supports a single bus; this device only has one anyway.
  if (analyzerProtocol == ANALYZER_PROTOCOL_LAWICEL && bus != 0) {
    return;
  }

  AnalyzerFrame entry;
  entry.frame     = frame;
  entry.bus       = bus;
  entry.timestamp = micros();

  if (xQueueSend(analyzerQueue, &entry, 0) == pdTRUE) {
    framesEnqueued++;
  } else {
    framesDropped++;
  }
}
