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

extern volatile bool dsgPaddleUp;
extern volatile bool dsgPaddleDown;
extern bool buttonFound;

extern ButtonMapping buttonMappings[kMaxButtonMappings];
extern size_t buttonMappingCount;

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

void loadPreferences();
void savePreferences();
