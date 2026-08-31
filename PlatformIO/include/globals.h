#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>

#include "defs.h"

extern LIN_Master_HardwareSerial_ESP32 steeringWheelLIN;
extern LIN_Master_HardwareSerial_ESP32 chassisLIN;
extern X9C103 radioResistor;
extern Preferences preferences;
extern AsyncWebServer server;

extern uint8_t gatewayLightData[4];
extern uint8_t steeringWheelLightData[4];
extern uint8_t recvButtonData[8];
extern uint8_t transButtonDataLIN[8];
extern uint8_t transButtonDataCAN[8];

extern volatile unsigned long fall_Time;
extern volatile unsigned long rise_Time;
extern volatile unsigned long dutyCycle;
extern volatile unsigned long lastRead;
extern volatile unsigned long total_Time;
extern volatile unsigned long on_Time;
extern volatile unsigned long lastDutyCycle;

extern unsigned long upperLightsAux;
extern uint8_t upperLightsLIN;
extern volatile uint16_t auxDimDutyPct10;    // dim end calibration  (duty % × 10, e.g. 197 = 19.7 %)
extern volatile uint16_t auxBrightDutyPct10; // bright end calibration (duty % × 10, e.g. 980 = 98.0 %)
extern volatile uint16_t radioResistance;
extern volatile uint32_t radioResistanceMs;  // millis() of last LIN poll that saw the button held

extern volatile bool diagPnpActive;       // test-toggle: holds pinPNP high
extern volatile bool diagResistiveEnabled; // test-toggle: holds resistive output at diagResistiveOhm
extern volatile uint16_t diagResistiveOhm; // diagnostic resistance value (ohms)
extern volatile bool testResistanceEnabled; // test mode: pulse resistance down then return to idle high
extern volatile uint16_t testResistanceOhm;  // current test resistance value (ohms)
extern volatile bool testResistancePulse;    // set by API to request a single 0.5 s pulse
extern volatile bool digipot20kEnabled;      // persisted selection; applied on next boot
extern uint16_t digipotMaxOhm;                // maximum resistance active for this boot

extern volatile bool dsgPaddleUp;
extern volatile bool dsgPaddleDown;
extern bool buttonFound;

extern ButtonMapping buttonMappings[kMaxButtonMappings];
extern size_t buttonMappingCount;

// Latch state per mapping: toggled on each press when FLAG_LATCH is set.
// While true, the button's PNP/CAN/LIN/resistive outputs are held active.
extern volatile bool buttonLatched[kMaxButtonMappings];

// --- OpenHaldex control (closed loop) --------------------------------------
extern volatile uint8_t  openHaldexCurrentMode; // last mode from 0x6B0 broadcast (0xFF = unknown)
extern volatile uint32_t openHaldexLastRxMs;    // millis() of last 0x6B0 broadcast
extern volatile uint8_t  openHaldexTargetMode;  // pending commanded mode (0xFF = no command active)
extern volatile uint32_t openHaldexCmdStartMs;  // millis() when the command was issued (timeout)
extern volatile uint32_t openHaldexLastSendMs;  // millis() of last 0x6A0 send (resend cadence)

extern volatile bool canBroadcastEnabled;
extern volatile uint16_t canBroadcastId;
extern volatile bool paddlesEnabled;
extern volatile bool useAuxLightSource;
extern volatile bool forceBacklight;
extern volatile uint8_t forceBacklightPercent;

extern volatile uint16_t canHoldMs;    // how long to hold CAN frame active after last button detect
extern volatile bool linOutputEnabled; // send translated button via chassis LIN
extern volatile uint8_t linOutputId;  // LIN frame ID for chassis LIN button output
extern uint8_t canHoldFrame[8];        // CAN payload held during the window (stateMux protected)
extern volatile uint32_t canHoldUntil; // millis() deadline; 0 = inactive

// Legacy PCB support: reverses RX/TX on both LIN channels. Applied at boot only.
extern volatile bool linLegacyPins;

// Configurable incoming LIN frame IDs (polled as master). Default to the
// constants in defs.h; overridable so wheels using other IDs are supported.
extern volatile uint8_t linButtonInId; // steering-wheel button frame
extern volatile uint8_t linLightInId;  // chassis light/brightness frame
extern volatile uint8_t linTempInId;   // steering-wheel temperature frame
extern volatile uint8_t linAccInId;    // steering-wheel accessory-button frame

extern volatile uint8_t latestLinButtonId;
extern volatile uint32_t latestLinButtonTimestamp;

extern uint8_t lastLinInFrame[8];
extern uint8_t lastLinOutFrame[8];
extern uint8_t lastCanOutFrame[8];
extern uint8_t lastLinInLen;
extern uint8_t lastLinOutLen;
extern uint8_t lastCanOutLen;
extern uint32_t lastLinInId;
extern uint32_t lastLinOutId;
extern uint32_t lastCanOutId;

// Latest accessory-button and temperature frames (for the LIN monitor/status).
extern uint8_t lastAccInFrame[8];
extern uint8_t lastTempInFrame[8];
extern uint8_t lastAccInLen;
extern uint8_t lastTempInLen;

extern volatile bool learnActive;
extern volatile uint8_t learnTarget;
extern volatile uint8_t learnRowIndex;
extern volatile uint32_t learnStartTimestamp;

extern portMUX_TYPE stateMux;
extern SemaphoreHandle_t steeringWheelLinMutex;
extern SemaphoreHandle_t chassisLinMutex;

// Bus-health tracking: millis() timestamp of the last error-free transaction.
// 0 = no successful transaction yet since boot.
extern volatile uint32_t swLinLastOkMs;      // steering-wheel LIN (LIN 1)
extern volatile uint32_t chassisLinLastOkMs; // chassis LIN (LIN 2)
extern volatile uint32_t lastCanRxMs;        // last valid CAN frame received (CAN)

// ---------------------------------------------------------------------------
// Diagnostic log — fixed-size line ring buffer. Producers call logLine();
// the /api/log consumer walks writeIndex.
// ---------------------------------------------------------------------------
constexpr size_t kLogLineLen   = 96;
constexpr size_t kLogLineCount = 64;

struct LogEntry {
  uint32_t ms;
  char     text[kLogLineLen];
};

extern LogEntry          logBuffer[kLogLineCount];
extern volatile uint32_t logWriteIndex;  // monotonically increasing

void logLine(const char* fmt, ...);

// ---------------------------------------------------------------------------
// LIN ID scanner — enumerates protected IDs 0x00..0x3F on both buses and
// records which respond. Driven from the steering-wheel LIN task.
// ---------------------------------------------------------------------------
struct LinScanResult {
  uint8_t bus;        // 1 = steering wheel (LIN 1), 2 = chassis (LIN 2)
  uint8_t id;         // protected frame ID (0x00..0x3F)
  uint8_t len;        // response length captured
  uint8_t data[8];    // last response bytes
  bool    responded;  // true if a clean response was received
};

constexpr size_t kMaxLinScanResults = 128;  // 64 IDs x 2 buses

extern volatile bool     linScanRequested;  // set by API, cleared by task
extern volatile bool     linScanActive;     // true while a scan is running
extern volatile uint32_t linScanDoneMs;     // millis() of last completed scan
extern LinScanResult     linScanResults[kMaxLinScanResults];
extern volatile size_t   linScanResultCount;
extern volatile bool     linScanWatchActive; // re-poll responders live for discovery
extern volatile uint32_t linScanWatchUntil;  // millis() deadline; auto-stops watch

void loadPreferences();
void savePreferences();
