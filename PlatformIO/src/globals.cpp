#include "globals.h"

LIN_Master_HardwareSerial_ESP32 steeringWheelLIN(Serial1, pinRX_LINSteeringWheel, pinTX_LINSteeringWheel, "LIN_SteeringWheel");
LIN_Master_HardwareSerial_ESP32 chassisLIN(Serial2, pinRX_LINchassis, pinTX_LINchassis, "LIN_chassis");

X9C103 radioResistor(baseResistance);
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

volatile bool dsgPaddleUp = false;
volatile bool dsgPaddleDown = false;
bool buttonFound = false;

volatile uint32_t swLinLastOkMs = 0;
volatile uint32_t chassisLinLastOkMs = 0;
volatile uint32_t lastCanRxMs = 0;

ButtonMapping buttonMappings[kMaxButtonMappings] = {
    // {name, oldButtonId, newLinButtonId, canByteIndex, canBitIndex, resistiveOhm}
    // canByteIndex 0xFF = no CAN output for that button
    {"Previous",           0x03, 0x16, 0,    0, 0},   // byte 0 bit 0
    {"Next",               0x02, 0x15, 0,    1, 0},   // byte 0 bit 1
    {"Voice/Mic",          0x1A, 0x19, 0,    2, 0},   // byte 0 bit 2
    {"Phone",              0x1A, 0x1C, 0,    3, 0},   // byte 0 bit 3
    {"Return",             0x29, 0x23, 0,    4, 0},   // byte 0 bit 4
    {"Up",                 0x22, 0x04, 0,    5, 30},  // byte 0 bit 5
    {"Down",               0x23, 0x05, 0,    6, 60},  // byte 0 bit 6
    {"Source -",            0x09, 0x03, 0,    7, 0},   // byte 0 bit 7
    {"Source +",            0x0A, 0x02, 1,    0, 0},   // byte 1 bit 0
    {"OK",                 0x28, 0x07, 1,    1, 0},   // byte 1 bit 1
    {"Volume +",            0x06, 0x10, 1,    2, 20},  // byte 1 bit 2
    {"Volume -",            0x07, 0x11, 1,    3, 10},  // no CAN output
    {"Voice/Mic ACC",      0x2B, 0x0C, 1,    4, 0},   // no CAN output
    {"Voice/Mic ACC2",     0x42, 0x0C, 1,    5, 0},   // no CAN output
    {"Paddle +",  0x1E, 0x00, 1,    6, 0},   // no CAN output
    {"Paddle -", 0x1F, 0x00, 1,    7, 0},   // no CAN output
};
size_t buttonMappingCount = 16;

volatile uint16_t canBroadcastId = canButtonID;
volatile bool canBroadcastEnabled = true;
volatile bool paddlesEnabled = false;
volatile bool useAuxLightSource = hasAuxLight;
volatile bool forceBacklight = false;
volatile uint8_t forceBacklightPercent = 100;

volatile uint16_t canHoldMs = 250;
volatile bool linOutputEnabled = true;
volatile uint8_t linOutputId = linButtonID;
uint8_t canHoldFrame[8] = {0};
volatile uint32_t canHoldUntil = 0;

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

volatile bool learnActive = false;
volatile uint8_t learnTarget = LEARN_NONE;
volatile uint8_t learnRowIndex = 0;
volatile uint32_t learnStartTimestamp = 0;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t steeringWheelLinMutex = nullptr;
SemaphoreHandle_t chassisLinMutex = nullptr;

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
  canBroadcastEnabled   = preferences.getBool("canBc",        canBroadcastEnabled);
  canBroadcastId        = preferences.getUShort("canId",      canBroadcastId);
  paddlesEnabled        = preferences.getBool("paddles",      paddlesEnabled);
  useAuxLightSource     = preferences.getBool("auxLight",     useAuxLightSource);
  auxDimDutyPct10       = preferences.getUShort("auxDimDuty",    auxDimDutyPct10);
  auxBrightDutyPct10    = preferences.getUShort("auxBrightDuty", auxBrightDutyPct10);
  forceBacklight        = preferences.getBool("forceBk",      forceBacklight);
  forceBacklightPercent = preferences.getUChar("forceBkPct",  forceBacklightPercent);
  canHoldMs             = preferences.getUShort("canHoldMs",  canHoldMs);
  linOutputEnabled      = preferences.getBool("linOut",       linOutputEnabled);
  linOutputId           = preferences.getUChar("linOutId",    linOutputId);
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
  preferences.putBool("paddles",       paddlesEnabled);
  preferences.putBool("auxLight",      useAuxLightSource);
  preferences.putUShort("auxDimDuty",  auxDimDutyPct10);
  preferences.putUShort("auxBrightDuty", auxBrightDutyPct10);
  preferences.putBool("forceBk",       forceBacklight);
  preferences.putUChar("forceBkPct",   forceBacklightPercent);
  preferences.putUShort("canHoldMs",   canHoldMs);
  preferences.putBool("linOut",        linOutputEnabled);
  preferences.putUChar("linOutId",     linOutputId);
}
