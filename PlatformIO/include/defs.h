#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "LIN_master_HardwareSerial_ESP32.h"
#include "X9C10X.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "ver.h"

#define FW_VERSION "1.01"

#define enableDebug 0
#define debugIO 0
#define debugCAN 0
#define debugLIN 0
#define ChassisCANDebug 0
#define detailedDebugWiFi 0
#define ENABLE_IO_TEST 0
#define serialMonitorRefresh 1000

#if enableDebug
#define DEBUG(x, ...) Serial.printf(x "\n", ##__VA_ARGS__)
#define DEBUG_(x, ...) Serial.printf(x, ##__VA_ARGS__)
#else
#define DEBUG(x, ...)
#define DEBUG_(x, ...)
#endif

#if enableDebug && debugIO
#define DEBUG_IO(x, ...) Serial.printf("[IO] " x "\n", ##__VA_ARGS__)
#else
#define DEBUG_IO(x, ...)
#endif

#if enableDebug && debugCAN
#define DEBUG_CAN(x, ...) Serial.printf("[CAN] " x "\n", ##__VA_ARGS__)
#else
#define DEBUG_CAN(x, ...)
#endif

#if enableDebug && debugLIN
#define DEBUG_LIN(x, ...) Serial.printf("[LIN] " x "\n", ##__VA_ARGS__)
#define DEBUG_LIN_(x, ...) Serial.printf("[LIN] " x, ##__VA_ARGS__)
#else
#define DEBUG_LIN(x, ...)
#define DEBUG_LIN_(x, ...)
#endif

#if enableDebug && detailedDebugWiFi
#define DEBUG_WIFI(x, ...) Serial.printf("[WiFi] " x "\n", ##__VA_ARGS__)
#else
#define DEBUG_WIFI(x, ...)
#endif

#if enableDebug && ChassisCANDebug
#define DEBUG_CHASSIS_CAN(x, ...) Serial.printf("[CAN] " x "\n", ##__VA_ARGS__)
#define DEBUG_CHASSIS_CAN_(x, ...) Serial.printf("[CAN] " x, ##__VA_ARGS__)
#else
#define DEBUG_CHASSIS_CAN(x, ...)
#define DEBUG_CHASSIS_CAN_(x, ...)
#endif

constexpr int pinTX_LINSteeringWheel = 17;
constexpr int pinRX_LINSteeringWheel = 16;
constexpr int pinTX_LINchassis = 23;
constexpr int pinRX_LINchassis = 22;
constexpr int pinWake_LIN = 18;
constexpr int pinCS_LIN = 19;

constexpr uint8_t linLightID = 0x0D;
constexpr uint8_t linButtonID = 0x0E;
constexpr uint8_t linTemperatureID = 0x3A;
constexpr uint8_t linAccButtonsID = 0x0F;
constexpr uint16_t canButtonID = 0x1E0;
constexpr uint32_t linBaud = 19200;
constexpr uint32_t linPause = 100;
constexpr uint32_t btnDebounce = 1000;

constexpr int pinCAN_RX = 13;
constexpr int pinCAN_TX = 14;
constexpr int pinOnboardLed = 2;

constexpr int pinAuxLight = 39;

constexpr int resistorUD = 25;
constexpr int resistorInc = 26;
constexpr int resistorCS = 27;

constexpr int pinPNP = 21;
constexpr uint8_t FLAG_ACTIVATES_PNP      = 0x01;  // bit 0: trigger PNP output when pressed
constexpr uint8_t FLAG_LATCH              = 0x02;  // bit 1: latch on press, clear on next press — applies to PNP, CAN and LIN outputs
constexpr uint8_t FLAG_OPENHALDEX_CONTROL = 0x04;  // bit 2: button commands an OpenHaldex mode change (exclusive: no PNP/CAN/LIN/resistive output)

constexpr uint16_t MOTOR1_ID = 0x280;
constexpr uint16_t MOTOR2_ID = 0x288;
constexpr uint16_t MOTOR3_ID = 0x380;
constexpr uint16_t MOTOR5_ID = 0x480;
constexpr uint16_t MOTOR6_ID = 0x488;
constexpr uint16_t MOTOR7_ID = 0x588;
constexpr uint16_t MOTOR_FLEX_ID = 0x580;
constexpr uint16_t GRA_ID = 0x38A;
constexpr uint16_t BRAKES1_ID = 0x1A0;
constexpr uint16_t BRAKES2_ID = 0x2A0;
constexpr uint16_t BRAKES3_ID = 0x4A0;
constexpr uint16_t BRAKES5_ID = 0x5A0;
constexpr uint16_t HALDEX_ID = 0x2C0;

constexpr uint16_t steering_ID = 0x5C1;
constexpr uint16_t light_ID = 0x470;

// --- OpenHaldex external control (see OpenHaldex firmware) -------------------
// Send the desired mode to the OpenHaldex unit: data[0] = mode (0..5), rest 0.
constexpr uint16_t OPENHALDEX_EXTERNAL_CONTROL_ID = 0x6A0;
// OpenHaldex broadcasts its live state here; data[6] = current mode.
constexpr uint16_t OPENHALDEX_BROADCAST_ID        = 0x6B0;
// Modes: 0=Stock 1=FWD 2=50:50 3=60:40 4=75:25 5=Expert
constexpr uint8_t  OPENHALDEX_MODE_COUNT     = 6;
constexpr uint8_t  OPENHALDEX_MODE_PUSH_NEXT = 0xFF;  // sentinel: advance to next mode each press
constexpr uint8_t  OPENHALDEX_MODE_UNKNOWN   = 0xFF;  // no broadcast received yet

constexpr const char* wifiHostName = "MFSWController";

constexpr size_t kMaxButtonMappings = 24;

struct ButtonMapping {
  char name[24];
  uint8_t oldButtonId; // original steering wheel button ID
  uint8_t newLinButtonId; // new steering wheel button ID
  uint8_t canByteIndex;  // 0-7: byte in 8-byte CAN frame; 0xFF = no CAN output
  uint8_t canBitIndex;   // 0-7: bit within that byte
  uint16_t resistiveOhm; // resistive output (in ohms)
  uint8_t flags;         // bit 0: PNP, bit 1: latch, bit 2: OpenHaldex control
  uint8_t openHaldexMode; // 0-5 fixed OpenHaldex mode, 0xFF = push-to-next (used when FLAG_OPENHALDEX_CONTROL set)
};

enum LearnTarget : uint8_t {
  LEARN_NONE = 0,
  LEARN_OLD_LIN = 1,
  LEARN_NEW_LIN = 2,
  LEARN_NEW_CAN = 3,
};
