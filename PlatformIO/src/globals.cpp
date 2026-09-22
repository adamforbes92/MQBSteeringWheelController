#include "globals.h"

#include <stdarg.h>

#include "savvycan.h"  // ANALYZER_PROTOCOL_* for the analyzer defaults below

LIN_Master_HardwareSerial_ESP32 steeringWheelLIN(Serial1, pinRX_LINSteeringWheel, pinTX_LINSteeringWheel, "LIN_SteeringWheel");
LIN_Master_HardwareSerial_ESP32 chassisLIN(Serial2, pinRX_LINchassis, pinTX_LINchassis, "LIN_chassis");

X9C103 radioResistor;
Preferences preferences;
AsyncWebServer server(80);

uint8_t gatewayLightData[4] = {0x00, 0x00, 0x00, 0x00};
uint8_t steeringWheelLightData[4] = {0x00, 0xF9, 0xFF, 0xFF};
uint8_t recvButtonData[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t transButtonDataLIN[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t transButtonDataCAN[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

volatile unsigned long fall_Time = 0;
volatile unsigned long rise_Time = 0;
volatile unsigned long dutyCycle = 0;
volatile unsigned long lastRead = 0;
volatile unsigned long total_Time = 0;
volatile unsigned long on_Time = 0;
volatile unsigned long lastDutyCycle = 0;

unsigned long upperLightsAux = 100;
uint8_t upperLightsLIN = 0x7F;
volatile uint16_t auxDimDutyPct10    = 197;   // 19.7 % duty — dim end
volatile uint16_t auxBrightDutyPct10 = 980;   // 98.0 % duty — bright end
volatile uint16_t radioResistance = 0; 
volatile uint32_t radioResistanceMs = 0;

volatile bool diagPnpActive = false;
volatile bool diagResistiveEnabled = false;
volatile uint16_t diagResistiveOhm = 0;
volatile bool testResistanceEnabled = false;
volatile uint16_t testResistanceOhm = 0;
volatile bool testResistancePulse = false;
volatile bool digipot20kEnabled = false;
uint16_t digipotMaxOhm = 10000;

volatile bool passthruEnabled = false;

volatile bool dsgPaddleUp = false;
volatile bool dsgPaddleDown = false;
bool buttonFound = false;

volatile uint32_t swLinLastOkMs = 0;
volatile uint32_t chassisLinLastOkMs = 0;
volatile uint32_t chassisLightLastOkMs = 0;
volatile uint32_t lastCanRxMs = 0;

// Default build target: PQ wheel -> PQ chassis (see chassisProtocol above),
// so oldButtonId == newLinButtonId throughout -- PQ<->PQ needs no
// translation at all. This used to be a PQ->MQB table (wheelProtocol PQ,
// chassisProtocol MQB); those MQB codes are kept in the trailing comment on
// each row instead of being deleted, so switching chassisProtocol back to
// MQB and re-running Setup's "update button codes" prompt (app.js,
// KNOWN_BUTTON_CODE_PAIRS) still has real values to restore, not guesses.
// Only the 11 rows commented "verified" are independently confirmed against
// the README's documented MQB code table; the rest were never verified even
// when this table targeted MQB and are commented "unverified" accordingly.
ButtonMapping buttonMappings[kMaxButtonMappings] = {
    // {name, oldButtonId, newLinButtonId, canByteIndex, canBitIndex, resistiveOhm}
    // canByteIndex 0xFF = no CAN output for that button
    {"Previous",           0x03, 0x03, 0,    0, 0},   // byte 0 bit 0   (MQB was 0x16, verified)
    {"Next",               0x02, 0x02, 0,    1, 0},   // byte 0 bit 1   (MQB was 0x15, verified)
    {"Voice/Mic",          0x1A, 0x1A, 0,    2, 0},   // byte 0 bit 2   (MQB was 0x19, verified)
    {"Phone",              0x1A, 0x1A, 0,    3, 0},   // byte 0 bit 3   (MQB was 0x1C, unverified)
    {"Return",             0x29, 0x29, 0,    4, 0},   // byte 0 bit 4   (MQB was 0x23, verified)
    {"Up",                 0x22, 0x22, 0,    5, 30},  // byte 0 bit 5   (MQB was 0x04, verified)
    {"Down",               0x23, 0x23, 0,    6, 60},  // byte 0 bit 6   (MQB was 0x05, verified)
    {"Source -",            0x09, 0x09, 0,    7, 0},   // byte 0 bit 7   (MQB was 0x03, verified)
    {"Source +",            0x0A, 0x0A, 1,    0, 0},   // byte 1 bit 0   (MQB was 0x02, verified)
    {"OK",                 0x28, 0x28, 1,    1, 0},   // byte 1 bit 1   (MQB was 0x07, verified)
    {"Volume +",            0x06, 0x06, 1,    2, 20},  // byte 1 bit 2   (MQB was 0x10, verified)
    {"Volume -",            0x07, 0x07, 1,    3, 10},  // no CAN output  (MQB was 0x11, verified)
    {"Voice/Mic ACC",      0x2B, 0x2B, 1,    4, 0},   // no CAN output  (MQB was 0x0C, unverified)
    {"Voice/Mic ACC2",     0x42, 0x42, 1,    5, 0},   // no CAN output  (MQB was 0x0C, unverified)
    {"Paddle +",  0x1E, 0x1E, 1,    6, 0},   // no CAN output  (MQB was 0x00 -- never had a code at all)
    {"Paddle -", 0x1F, 0x1F, 1,    7, 0},   // no CAN output  (MQB was 0x00 -- never had a code at all)
    {"Paddles (both)", 0x03, 0x03, 0xFF, 0, 0, 0, 0, 6},   // byte 6 = up+down pressed together (MQB was 0x00)
    {"Horn",           0x01, 0x01, 0xFF, 0, 0, 0, 0, 7},   // byte 7 = horn line grounded (MQB was 0x00)
};
size_t buttonMappingCount = 18;

volatile bool buttonLatched[kMaxButtonMappings] = {false};

volatile uint8_t  openHaldexCurrentMode = OPENHALDEX_MODE_UNKNOWN;
volatile uint32_t openHaldexLastRxMs    = 0;
volatile uint8_t  openHaldexTargetMode  = OPENHALDEX_MODE_UNKNOWN;
volatile uint32_t openHaldexCmdStartMs  = 0;
volatile uint32_t openHaldexLastSendMs  = 0;

// Charisma defaults: off, so nothing new reaches the bus until it is chosen.
// AWD + VAQ preselected as the drivetrain participants this device exists for.
volatile uint8_t  charismaMode         = CHARISMA_MODE_OFF;
volatile uint8_t  charismaButtonBit    = CHARISMA_BTN_TASTE2;
volatile uint8_t  charismaProgramCount = 4;
volatile uint8_t  charismaParticipants = 0x03;
volatile uint8_t  charismaProgram      = 1;
volatile uint32_t charismaPressMs      = 0;

volatile uint16_t canBitrateKbit = 500;
volatile uint16_t canBroadcastId = canButtonID;
volatile bool canBroadcastEnabled = true;
volatile bool paddlesEnabled = false;
volatile bool useAuxLightSource = true;
volatile bool forceBacklight = false;
volatile uint8_t forceBacklightPercent = 100;

volatile uint16_t canHoldMs = 250;
volatile bool linOutputEnabled = true;
volatile uint8_t linOutputId = linButtonID;
uint8_t canHoldFrame[8] = {0};
volatile uint32_t canHoldUntil = 0;

volatile bool linLegacyPins = false;

volatile uint8_t linButtonInId = linButtonID;
volatile uint8_t linLightInId  = linLightID;
volatile uint8_t linTempInId   = linTemperatureID;
volatile uint8_t linAccInId    = linAccButtonsID;
volatile uint8_t linButtonByteIndex = 1;
volatile int8_t  latestMatchedRow   = -1;
volatile uint8_t linRotaryByteIndex = 3;
volatile int8_t  wheelRotaryDelta   = 0;
volatile uint8_t wheelPressStage    = 0;

volatile uint8_t latestLinButtonId = 0;
volatile uint32_t latestLinButtonTimestamp = 0;

uint8_t lastLinInFrame[8] = {0};
uint8_t lastLinOutFrame[8] = {0};
uint8_t lastCanOutFrame[8] = {0};
uint8_t lastLinInLen = 0;
uint8_t lastLinOutLen = 0;
uint8_t lastCanOutLen = 0;
uint32_t lastLinInId = linButtonID;
uint32_t lastLinOutId = linButtonID;
uint32_t lastCanOutId = canButtonID;

uint8_t lastAccInFrame[8] = {0};
uint8_t lastTempInFrame[8] = {0};
uint8_t lastAccInLen = 0;
uint8_t lastTempInLen = 0;

volatile bool learnActive = false;
volatile uint8_t learnTarget = LEARN_NONE;
volatile uint8_t learnRowIndex = 0;
volatile uint32_t learnStartTimestamp = 0;
volatile uint32_t mappingsRevision = 0;
uint8_t learnBaseline[8] = {0};
volatile bool learnBaselineReady = false;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t steeringWheelLinMutex = nullptr;
SemaphoreHandle_t chassisLinMutex = nullptr;

LogEntry          logBuffer[kLogLineCount] = {};
volatile uint32_t logWriteIndex           = 0;
static portMUX_TYPE logMux                = portMUX_INITIALIZER_UNLOCKED;

void logLine(const char* fmt, ...) {
  char tmp[kLogLineLen];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  portENTER_CRITICAL(&logMux);
  const uint32_t i = logWriteIndex % kLogLineCount;
  logBuffer[i].ms = millis();
  strlcpy(logBuffer[i].text, tmp, sizeof(logBuffer[i].text));
  logWriteIndex++;
  portEXIT_CRITICAL(&logMux);

  // Mirror to the serial monitor so the same lines show in the PlatformIO
  // terminal, not just the web LIN Monitor page.
  DEBUG("[LOG] %s", tmp);
}

volatile bool     analyzerMode               = false;
volatile bool     analyzerSerial             = false;
volatile uint8_t  analyzerProtocol           = ANALYZER_PROTOCOL_GVRET;

CanLogEntry       canLogBuffer[kCanLogCount] = {};
volatile uint32_t canLogWriteIndex            = 0;
volatile bool     canLogEnabled               = false;
volatile uint16_t canLogId                    = 0;
volatile bool     canLogChangedOnly           = true;
static portMUX_TYPE canLogMux                 = portMUX_INITIALIZER_UNLOCKED;

// Called for every received frame while the monitor is on. Two behaviours:
//   canLogId == 0  discovery — each distinct ID recorded ONCE, with the
//                  payload it first arrived with. Answers "what is on this
//                  bus", which is the question worth asking first and the one
//                  a 300-entry buffer can actually answer on a busy bus.
//   canLogId != 0  focus — that ID only, every frame or only on change.
void logCanFrame(uint16_t id, const uint8_t* data, uint8_t len) {
  if (!canLogEnabled) {
    return;
  }
  if (len > 8) {
    len = 8;
  }

  if (canLogId == 0) {
    // Standard IDs are 11-bit, so 2048 bits of "already seen" covers them all.
    static uint32_t seen[64] = {};
    const uint16_t slot = (uint16_t)(id >> 5);
    if (slot >= 64) {
      return;
    }
    const uint32_t bit = 1UL << (id & 0x1F);
    if (seen[slot] & bit) {
      return;
    }
    seen[slot] |= bit;
  } else {
    if (id != canLogId) {
      return;
    }
    if (canLogChangedOnly) {
      static uint8_t lastData[8] = {};
      static uint8_t lastLen     = 0xFF;
      if (lastLen == len && memcmp(lastData, data, len) == 0) {
        return;
      }
      memcpy(lastData, data, len);
      lastLen = len;
    }
  }

  portENTER_CRITICAL(&canLogMux);
  CanLogEntry& e = canLogBuffer[canLogWriteIndex % kCanLogCount];
  e.ms  = millis();
  e.id  = id;
  e.len = len;
  memset(e.data, 0, sizeof(e.data));
  memcpy(e.data, data, len);
  canLogWriteIndex++;
  portEXIT_CRITICAL(&canLogMux);
}

volatile uint8_t  wheelProtocol = WHEEL_PROTOCOL_PQ;
volatile uint8_t  mqbActByte1   = 0xFF;
volatile uint8_t  mqbActByte2   = 0x00;
volatile uint8_t  mqbActByte3   = 0x00;

volatile uint8_t  chassisProtocol = CHASSIS_PROTOCOL_PQ;

void loadPreferences() {
  size_t mapCount = preferences.getUInt("mapCount", buttonMappingCount);
  if (mapCount > kMaxButtonMappings) mapCount = kMaxButtonMappings;
  if (preferences.isKey("mapBlob")) {
    size_t bytesExpected = mapCount * sizeof(ButtonMapping);
    size_t bytesStored = preferences.getBytesLength("mapBlob");
    if (bytesExpected > 0 && bytesStored >= bytesExpected) {
      preferences.getBytes("mapBlob", buttonMappings, bytesExpected);
      buttonMappingCount = mapCount;
    }
  }
  // Migration: maps saved before these rows existed won't contain them, so
  // append any missing built-ins without disturbing learned entries.
  static const ButtonMapping kSpecialRows[] = {
    {"Paddles (both)", 0x03, 0x00, 0xFF, 0, 0, 0, 0, 6},
    {"Horn",           0x01, 0x00, 0xFF, 0, 0, 0, 0, 7},
  };
  for (const ButtonMapping& row : kSpecialRows) {
    bool present = false;
    for (size_t i = 0; i < buttonMappingCount; i++) {
      if (strcmp(buttonMappings[i].name, row.name) == 0) { present = true; break; }
    }
    if (!present && buttonMappingCount < kMaxButtonMappings) {
      buttonMappings[buttonMappingCount++] = row;
    }
  }
  canBroadcastEnabled   = preferences.getBool("canBc",        canBroadcastEnabled);
  canBroadcastId        = preferences.getUShort("canId",      canBroadcastId);
  canBitrateKbit        = preferences.getUShort("canKbit",    canBitrateKbit);
  if (canBitrateKbit != 100 && canBitrateKbit != 125 &&
      canBitrateKbit != 250 && canBitrateKbit != 500) {
    canBitrateKbit = 500;
  }
  paddlesEnabled        = preferences.getBool("paddles",      paddlesEnabled);
  useAuxLightSource     = preferences.getBool("auxLight",     useAuxLightSource);
  auxDimDutyPct10       = preferences.getUShort("auxDimDuty",    auxDimDutyPct10);
  auxBrightDutyPct10    = preferences.getUShort("auxBrightDuty", auxBrightDutyPct10);
  forceBacklight        = preferences.getBool("forceBk",      forceBacklight);
  forceBacklightPercent = preferences.getUChar("forceBkPct",  forceBacklightPercent);
  canHoldMs             = preferences.getUShort("canHoldMs",  canHoldMs);
  linOutputEnabled      = preferences.getBool("linOut",       linOutputEnabled);
  linOutputId           = preferences.getUChar("linOutId",    linOutputId);
  linLegacyPins         = preferences.getBool("linLegacy",    linLegacyPins);
  linButtonInId         = preferences.getUChar("linBtnIn",    linButtonInId);
  linLightInId          = preferences.getUChar("linLgtIn",    linLightInId);
  linTempInId           = preferences.getUChar("linTmpIn",    linTempInId);
  linAccInId            = preferences.getUChar("linAccIn",    linAccInId);
  linButtonByteIndex    = preferences.getUChar("linBtnByte",  linButtonByteIndex);
  if (linButtonByteIndex >= 8) linButtonByteIndex = 1;
  linRotaryByteIndex    = preferences.getUChar("linRotByte",  linRotaryByteIndex);
  if (linRotaryByteIndex > 8) linRotaryByteIndex = 8;  // 8 = rotary disabled
  wheelProtocol         = preferences.getUChar("wheelProto",  wheelProtocol);
  if (wheelProtocol > WHEEL_PROTOCOL_MQB) wheelProtocol = WHEEL_PROTOCOL_PQ;
  mqbActByte1           = preferences.getUChar("mqbAct1",     mqbActByte1);
  mqbActByte2           = preferences.getUChar("mqbAct2",     mqbActByte2);
  mqbActByte3           = preferences.getUChar("mqbAct3",     mqbActByte3);
  chassisProtocol       = preferences.getUChar("chassisProto", chassisProtocol);
  if (chassisProtocol > CHASSIS_PROTOCOL_MQB) chassisProtocol = CHASSIS_PROTOCOL_MQB;
  charismaMode          = preferences.getUChar("chaMode",     charismaMode);
  if (charismaMode > CHARISMA_MODE_MAX) charismaMode = CHARISMA_MODE_OFF;
  charismaButtonBit     = preferences.getUChar("chaBtnBit",   charismaButtonBit);
  if (charismaButtonBit != CHARISMA_BTN_TASTE2 &&
      charismaButtonBit != CHARISMA_BTN_ECO &&
      charismaButtonBit != CHARISMA_BTN_OFFROAD) {
    charismaButtonBit = CHARISMA_BTN_TASTE2;
  }
  charismaProgramCount  = preferences.getUChar("chaProgN",    charismaProgramCount);
  if (charismaProgramCount < 2 || charismaProgramCount > 15) charismaProgramCount = 4;
  charismaParticipants  = preferences.getUChar("chaParts",    charismaParticipants);
  if (charismaProgram > charismaProgramCount) charismaProgram = 1;
  digipot20kEnabled     = preferences.getBool("digipot20k",   false);
  digipotMaxOhm         = digipot20kEnabled ? 20000 : 10000;
  if (auxBrightDutyPct10 <= auxDimDutyPct10) {
    auxDimDutyPct10    = 197;
    auxBrightDutyPct10 = 980;
  }
}

void savePreferences() {
  preferences.putUInt("mapCount",      (uint32_t)buttonMappingCount);
  preferences.putBytes("mapBlob",      buttonMappings, buttonMappingCount * sizeof(ButtonMapping));
  preferences.putBool("canBc",         canBroadcastEnabled);
  preferences.putUShort("canId",       canBroadcastId);
  preferences.putUShort("canKbit",     canBitrateKbit);
  preferences.putBool("paddles",       paddlesEnabled);
  preferences.putBool("auxLight",      useAuxLightSource);
  preferences.putUShort("auxDimDuty",  auxDimDutyPct10);
  preferences.putUShort("auxBrightDuty", auxBrightDutyPct10);
  preferences.putBool("forceBk",       forceBacklight);
  preferences.putUChar("forceBkPct",   forceBacklightPercent);
  preferences.putUShort("canHoldMs",   canHoldMs);
  preferences.putBool("linOut",        linOutputEnabled);
  preferences.putUChar("linOutId",     linOutputId);
  preferences.putBool("linLegacy",     linLegacyPins);
  preferences.putUChar("linBtnIn",     linButtonInId);
  preferences.putUChar("linLgtIn",     linLightInId);
  preferences.putUChar("linTmpIn",     linTempInId);
  preferences.putUChar("linAccIn",     linAccInId);
  preferences.putUChar("linBtnByte",   linButtonByteIndex);
  preferences.putUChar("linRotByte",   linRotaryByteIndex);
  preferences.putUChar("wheelProto",   wheelProtocol);
  preferences.putUChar("mqbAct1",      mqbActByte1);
  preferences.putUChar("mqbAct2",      mqbActByte2);
  preferences.putUChar("mqbAct3",      mqbActByte3);
  preferences.putUChar("chassisProto", chassisProtocol);
  preferences.putUChar("chaMode",      charismaMode);
  preferences.putUChar("chaBtnBit",    charismaButtonBit);
  preferences.putUChar("chaProgN",     charismaProgramCount);
  preferences.putUChar("chaParts",     charismaParticipants);
  preferences.putBool("digipot20k",    digipot20kEnabled);
}
