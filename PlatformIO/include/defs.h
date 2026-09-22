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

#define FW_VERSION "1.05"

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
constexpr uint8_t FLAG_CHARISMA_CONTROL   = 0x08;  // bit 3: button advances the Charisma (Drive Select) program (exclusive, as above)
// Rotary direction filter: a scroll wheel reports the SAME button code whichever
// way it turns, with the direction carried by a separate byte, so a row must be
// able to say which way it wants. Neither bit (or both) = fires either way,
// which is what every existing row does. Kept in spare flag bits rather than a
// new struct field on purpose: ButtonMapping is persisted to NVS as a raw blob
// sized by sizeof(), so growing it would make every saved mapping fail to load.
constexpr uint8_t FLAG_ROTARY_UP          = 0x10;  // bit 4: only when the rotary byte steps positive
constexpr uint8_t FLAG_ROTARY_DOWN        = 0x20;  // bit 5: only when it steps negative
// Press-stage filter. The same byte that carries roller movement is, for every
// non-roller code, a hold counter the wheel maintains itself — measured across
// all 13 buttons of the MQB wheel in docs/ (14/09 captures): 1 on press, then 4
// at ~0.8 s, 5 at ~2 s, 6 at ~3 s held. So "long press" is not something we
// have to time; the wheel reports it. Short = stage 1..3, long = stage >= 4.
// Neither bit (every pre-existing row) or both = fires at any stage. A short
// row and a long row on the same code are disjoint, so both can coexist:
// e.g. momentary on a tap, latch toggle on a hold (long + Latch).
constexpr uint8_t FLAG_PRESS_SHORT        = 0x40;  // bit 6: only while stage < kLongPressStage
constexpr uint8_t FLAG_PRESS_LONG         = 0x80;  // bit 7: only once stage >= kLongPressStage
constexpr uint8_t kLongPressStage         = 4;

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

// --- Charisma (VW's internal name for Drive Select) -------------------------
// Decoded from MQB_ACAN_KMatrix_EN.dbc. Every message here is 1 Hz cyclic in
// the factory schedule, and every target-program field is 4 bits encoded
// 0 = no function, 1..15 = Program_1..Program_15.
//
// There are two ways to inject a drive-select change, and which is correct
// depends on the car — hence charismaMode rather than a hardcoded choice:
//   BUTTON      the dash button press inside BCM_01; the car's own Charisma
//               coordinator sees it and does the mode cycling itself.
//   COORDINATOR the coordinator's own output, naming a target program per
//               participant. For a car with no factory drive select.
// Both IDs are normally transmitted by the gateway, so on a car that already
// has drive select this device becomes a second transmitter of an existing ID.
constexpr uint16_t CHARISMA_01_ID = 0x385;  // per-participant target programs
constexpr uint16_t CHARISMA_07_ID = 0x3E8;  // carries CHA_Current_Mode
constexpr uint16_t BCM_01_ID      = 0x65A;  // carries the Charisma button bits

// PQ does Charisma completely differently (vw_pq.dbc, in the OpenHaldex repo
// alongside the MQB matrix). There are no dedicated Charisma messages and no
// per-participant target programs: the whole car's mode is a single 4-bit
// value riding in a MULTIPLEXED slot of the comfort gateway's Gate_Komf_1,
// and each participant reports its own state inside its own message instead
// (e.g. LH2_Sta_Charisma in Lenkhilfe_2 0x3D2).
//
//   GK1_SamFktNr      bit 12, 4 bits — the multiplexor
//   GK1_CharismaModus bit  8, 4 bits — only present when SamFktNr == 1
//
// Both sit in byte 1, so that byte is (SamFktNr << 4) | CharismaModus.
//
// There is NO PQ equivalent of the MQB Charisma button: the only buttons in
// the PQ matrix are Status_CDC_Taster (a damper-ECU status, not a request)
// and Taster_Niveau. So on PQ the coordinator route is the only one available.
constexpr uint16_t GATE_KOMF_1_ID      = 0x390;
constexpr uint8_t  PQ_SAMFKTNR_BIT     = 12;
constexpr uint8_t  PQ_CHARISMA_MODE_BIT = 8;
constexpr uint8_t  PQ_SAMFKT_CHARISMA  = 1;

constexpr uint8_t CHARISMA_MODE_OFF        = 0;
constexpr uint8_t CHARISMA_MODE_MQB_BUTTON = 1;
constexpr uint8_t CHARISMA_MODE_MQB_COORD  = 2;
constexpr uint8_t CHARISMA_MODE_PQ_COORD   = 3;
constexpr uint8_t CHARISMA_MODE_MAX        = CHARISMA_MODE_PQ_COORD;

// Selectable button bits within BCM_01, as DBC start bits (little-endian).
constexpr uint8_t CHARISMA_BTN_TASTE2  = 22;  // BCM_Charisma_Taste2
constexpr uint8_t CHARISMA_BTN_ECO     = 33;  // BCM_Eco_Charisma_Taste
constexpr uint8_t CHARISMA_BTN_OFFROAD = 21;  // BCM_Offroad_Taste

// CHA_Current_Mode in Charisma_07, and the "this was a manual change by the
// driver, not an automatic retry" flag carried by both Charisma messages.
constexpr uint8_t CHARISMA_CURRENT_MODE_BIT = 8;
constexpr uint8_t CHARISMA_MANUAL_FLAG_BIT  = 14;

// MQB coordinator participants the user can choose to drive (PQ has no
// equivalent — its mode is one value for the whole car). Selected by bitmask,
// so adding a row here extends the UI's checklist without touching anything
// else. Deliberately a short list of drivetrain/chassis participants rather
// than all 30-odd in the matrix: commanding ESP or AEB by accident is not
// something a steering-wheel button should be able to do by default.
// All of these live in Charisma_01; startBit is the DBC start bit of that
// participant's 4-bit target-program field.
struct CharismaParticipant {
  const char* name;
  uint8_t     startBit;
};
constexpr CharismaParticipant kCharismaParticipants[] = {
    {"AWD (ALR)",      24},
    {"VAQ",            36},
    {"Engine (MO)",    16},
    {"Gearbox (GE)",   20},
    {"Steering (EPS)", 48},
    {"ESP",             4},
};
constexpr uint8_t kCharismaParticipantCount =
    (uint8_t)(sizeof(kCharismaParticipants) / sizeof(kCharismaParticipants[0]));

// How long a press keeps the button bit asserted / the manual-change flag set.
constexpr uint32_t kCharismaPressHoldMs = 300;

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
  uint8_t sourceByte;    // which button-frame byte carries this button (0 = use default byte); set by Learn (e.g. 6 paddle, 7 horn)
};

enum LearnTarget : uint8_t {
  LEARN_NONE = 0,
  LEARN_OLD_LIN = 1,
  LEARN_NEW_LIN = 2,
  LEARN_NEW_CAN = 3,
};
