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

// Passthru diagnostic mode: not persisted (always off at boot, like the other
// diag test toggles). Bypasses all button-mapping/protocol-shaping logic and
// relays LIN frames unmodified between the wheel and chassis buses, and logs
// EVERY poll/send attempt on every polled ID to the LIN Monitor — success or
// failure, any length — instead of only successful, changed frames. See
// logLinAttempt() in LIN.cpp.
extern volatile bool passthruEnabled;

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

// --- Charisma / Drive Select output (see defs.h for the decoded matrix) -----
extern volatile uint8_t  charismaMode;          // CHARISMA_MODE_OFF / _BUTTON / _COORDINATOR
extern volatile uint8_t  charismaButtonBit;     // which BCM_01 bit to assert in BUTTON mode
extern volatile uint8_t  charismaProgramCount;  // programs cycled through (2..15)
extern volatile uint8_t  charismaParticipants;  // bitmask over kCharismaParticipants
extern volatile uint8_t  charismaProgram;       // current program, 1..charismaProgramCount
extern volatile uint32_t charismaPressMs;       // millis() of the last press (0 = none)

// CAN bus speed in kbit/s (100/125/250/500). Applied at boot only.
extern volatile uint16_t canBitrateKbit;
// TX statistics from twaiSendStandardFrame(): queued vs refused by the driver.
extern volatile uint32_t canTxAccepted;
extern volatile uint32_t canTxRefused;

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

// Which byte of the button/accessory frame carries the button code (0-7).
// Default 1; overridable for wheels that place the code in another byte.
extern volatile uint8_t linButtonByteIndex;

// Which byte carries scroll-wheel movement (0-7, or 8+ to disable). Byte 3 on
// both wheels captured so far. How it is READ depends on wheelProtocol: an MQB
// wheel puts that frame's movement there directly, a PQ wheel an absolute
// position whose movement is the difference between polls.
extern volatile uint8_t linRotaryByteIndex;

// Signed movement reported by the current poll: + one way, - the other, 0 if
// the wheel did not turn. Recomputed once per button-frame read.
extern volatile int8_t  wheelRotaryDelta;

// Raw value of the same byte this poll — the press-stage counter when a button
// (not a roller) is held. 0 when idle. See FLAG_PRESS_SHORT / _LONG in defs.h.
extern volatile uint8_t wheelPressStage;

// Steering-wheel protocol family. MQB wheels only populate their button bytes
// once the master publishes a valid 0x0D backlight frame with "activate" bytes;
// PQ wheels don't require this. See mqbActByte1..3.
constexpr uint8_t WHEEL_PROTOCOL_PQ  = 0;
constexpr uint8_t WHEEL_PROTOCOL_MQB = 1;
extern volatile uint8_t wheelProtocol;

// MQB 0x0D backlight activation bytes 1..3 (byte 0 is live brightness). Known
// working sets: older MQB = 0xFF/0x00/0x00; MQB Evo = 0x81/0x64/0x40.
extern volatile uint8_t mqbActByte1;
extern volatile uint8_t mqbActByte2;
extern volatile uint8_t mqbActByte3;

// Chassis-side protocol: shape of the translated button frame this device
// sends AS MASTER to the chassis/BCM on linOutputId. Independent of
// wheelProtocol so any wheel <-> chassis combination can be bridged (PQ>PQ,
// PQ>MQB, MQB>PQ, MQB>MQB) — byte 1 always carries the mapped button code
// (already protocol-specific via the button table); these only control the
// filler/marker bytes around it. MQB is today's existing, already-validated
// output (bytes 0/4 left at 0) and is the default so nobody's working setup
// changes; PQ is new, additive behaviour — see buildChassisButtonFrame() in
// LIN.cpp, modelled on github.com/Dimka8901/MQB-MFSW-PQ25's mqbToPq().
constexpr uint8_t CHASSIS_PROTOCOL_PQ  = 0;
constexpr uint8_t CHASSIS_PROTOCOL_MQB = 1;
extern volatile uint8_t chassisProtocol;

extern volatile uint8_t latestLinButtonId;
extern volatile uint32_t latestLinButtonTimestamp;

// Which mapping row actually matched, -1 for none. The button code alone is no
// longer enough to identify a row: a scroll wheel's two directions share one
// code and differ only in flags, so anything reporting "what was pressed" has
// to be told the row rather than re-deriving it from the code.
extern volatile int8_t latestMatchedRow;

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
// Bumped whenever the FIRMWARE changes a mapping (a Learn landing). The UI
// holds its own copy of the table and posts the whole thing back on Save; it
// watches this so it can pull the learned value in before that, instead of
// overwriting it with the stale copy it was showing.
extern volatile uint32_t mappingsRevision;

extern uint8_t learnBaseline[8];  // idle button-frame snapshot, taken lazily — see learnBaselineReady
// False until the baseline has been taken from a frame that actually arrived
// error-free. Arming Learn cannot snapshot immediately: getButtonState() zeroes
// lastLinInFrame on any LIN error, so arming during one gave an all-zero
// baseline, against which every constant byte in the next good frame looks like
// a press — byte 4 (0x31 on MQB) being the one that got captured, producing a
// row that then matched on every single frame.
extern volatile bool learnBaselineReady;

extern portMUX_TYPE stateMux;
extern SemaphoreHandle_t steeringWheelLinMutex;
extern SemaphoreHandle_t chassisLinMutex;

// Bus-health tracking: millis() timestamp of the last error-free transaction.
// 0 = no successful transaction yet since boot.
extern volatile uint32_t swLinLastOkMs;      // steering-wheel LIN (LIN 1)
extern volatile uint32_t chassisLinLastOkMs; // chassis LIN (LIN 2) — any successful transaction (light RX or button TX)
extern volatile uint32_t chassisLightLastOkMs; // chassis LIN (LIN 2) — successful BCM light-frame RX specifically
extern volatile uint32_t lastCanRxMs;        // last valid CAN frame received (CAN)

// ---------------------------------------------------------------------------
// Diagnostic log — fixed-size line ring buffer. Producers call logLine();
// the /api/log consumer walks writeIndex.
// ---------------------------------------------------------------------------
constexpr size_t kLogLineLen   = 96;
constexpr size_t kLogLineCount = 200;  // large enough to hold a full verbose ID sweep

struct LogEntry {
  uint32_t ms;
  char     text[kLogLineLen];
};

extern LogEntry          logBuffer[kLogLineCount];
extern volatile uint32_t logWriteIndex;  // monotonically increasing

void logLine(const char* fmt, ...);

// ---------------------------------------------------------------------------
// CAN monitor — its own ring buffer, deliberately NOT the logLine() one. A
// 500 kbit/s chassis bus can carry over a thousand frames a second; sharing
// would flush every LIN entry within a second of enabling it.
// ---------------------------------------------------------------------------
constexpr size_t kCanLogCount = 300;

struct CanLogEntry {
  uint32_t ms;
  uint16_t id;
  uint8_t  len;
  uint8_t  data[8];
};

extern CanLogEntry       canLogBuffer[kCanLogCount];
extern volatile uint32_t canLogWriteIndex;  // monotonically increasing

// Runtime-only (never persisted, always off at boot) like the passthru toggle:
// a logger left running across a reboot is a surprise nobody wants.
extern volatile bool     canLogEnabled;
extern volatile uint16_t canLogId;         // 0 = every ID, first sighting only
extern volatile bool     canLogChangedOnly;  // filtered ID: log only on change

void logCanFrame(uint16_t id, const uint8_t* data, uint8_t len);

// SavvyCAN analyzer (see savvycan.h). Runtime-only, off at boot, and WiFi vs
// Serial are mutually exclusive — setAnalyzerMode()/setAnalyzerSerialMode()
// enforce that, so set these through those calls rather than directly.
extern volatile bool    analyzerMode;      // GVRET/SLCAN over TCP:23
extern volatile bool    analyzerSerial;    // GVRET over USB at 1 Mbaud
extern volatile uint8_t analyzerProtocol;  // ANALYZER_PROTOCOL_GVRET / _LAWICEL

void loadPreferences();
void savePreferences();
