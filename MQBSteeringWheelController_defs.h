#include <ESP32_CAN.h>    // for CAN
#include <Preferences.h>  // stores settings - mainly for last interval speed
#include "LIN_master_HardwareSerial_ESP32.h"

// defines
// debug / diag / testing
#define stateDebug 1                                             // State Debug - set to 0 on release ** CAN CHANGE THIS **
#define ChassisCANDebug 0                                        // if 1, will print CAN 1 (Chassis) messages ** CAN CHANGE THIS **
#define hasResistiveStereo 1                                     // for outputting resistive values for Sony/etc stereos ** CAN CHANGE THIS **
#define hasAuxLight 1                                            // to disable getting aux input - just saves some CPU if it's not being used ** CAN CHANGE THIS **
#define hasCAN 1                                                 // to broadcast over CAN ** CAN CHANGE THIS **

#define arraySize(array) (sizeof((array)) / sizeof((array)[0]))  // calc. for size of arrays

// define pins
#define pinTX_LINSteeringWheel 16                                // transmit pin for LIN TJA1020
#define pinRX_LINSteeringWheel 17                                // receive pin for LIN TJA1020
#define pinTX_LINchassis 22                                      // transmit pin for LIN TJA1020
#define pinRX_LINchassis 23                                      // receive pin for LIN TJA1020
#define pinWake_LIN 18                                           // wake for BOTH LIN TJA1020
#define pinCS_LIN 19                                             // chip select for BOTH LIN TJA1020

#define linLightID 0x0D                                          // for sending over light data - this is required whatever we're doing otherwise the controller doesn't feedback(!)
#define linButtonID 0x0E                                         // for retrieving button presses - they are returned over this ID
#define steeringWheel_ID 0x1E0                                   // broadcast to CAN ID
#define linBaud 19200                                            // LIN 2.x > 19.2kBaud
#define linPause 100                                             // Send packets every x ms ** CAN CHANGE THIS **

#define pinCAN_RX 14                                             // RX pin for SN65HVD230 (CAN_RX)
#define pinCAN_TX 13                                             // TX pin for SN65HVD230 (CAN_TX)

#define pinAuxLight 35                                           // for aux PWM input - like MK4 chassis etc.

// CAN IDs
#define MOTOR1_ID 0x280
#define MOTOR2_ID 0x288
#define MOTOR3_ID 0x380
#define MOTOR5_ID 0x480
#define MOTOR6_ID 0x488
#define MOTOR7_ID 0x588
#define MOTOR_FLEX_ID 0x580
#define GRA_ID 0x38A
#define BRAKES1_ID 0x1A0
#define BRAKES2_ID 0x2A0
#define BRAKES3_ID 0x4A0
#define BRAKES5_ID 0x5A0
#define HALDEX_ID 0x2C0

uint32_t lastMillis = 0;                                                     // Counter for sending frames x ms
uint8_t gatewayLightData[4] = { 0x00, 0x00, 0x00, 0x00 };                    // mqb may require 4 bytes (the last two being 0xFF).  First byte is brightness
uint8_t steeringWheelLightData[4] = { 0x00, 0xF9, 0xFF, 0xFF };                           // mqb may require 4 bytes (the last two being 0xFF).  First byte is brightness
uint8_t recvButtonData[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  // button data return, set to 0x00 for first run
uint8_t transButtonDataLIN[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  // button data return, set to 0x00 for first run
uint8_t transButtonDataCAN[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  // button data return, set to 0x00 for first run

extern unsigned long fall_Time = 0;   // Placeholder for microsecond time when last falling edge occured.
extern unsigned long rise_Time = 0;   // Placeholder for microsecond time when last rising edge occured.
extern unsigned long dutyCycle = 0;   // Duty Cycle %
extern unsigned long lastRead = 0;    // Last interrupt time (needed to determine interrupt lockup due to 0% and 100% duty cycle)
extern unsigned long total_Time = 0;  // Last interrupt time (needed to determine interrupt lockup due to 0% and 100% duty cycle)
extern unsigned long on_Time = 0;

extern unsigned long upperLightsAux = 100;
extern byte upperLightsLIN = 0x7F;

bool buttonFound = false;

struct {
  uint8_t fromID;  // button 'id' FROM steering wheel
  uint8_t toID;    // convert TO 'id' for gateway
  uint8_t canID;   // convert TO 'id' for CAN
  String comment;  // generic English comment - mostly for Serial.  Yes, it hogs memory but it's an ESP sooo...
} buttonTranspose[] = {
  // from > to
  { 0x00, 0x00, 0x00, "NULL" },
  { 0x03, 0x16, 0x01, "Previous" },   // prev
  { 0x02, 0x15, 0x02, "Next" },       // next
  { 0x1A, 0x19, 0x03, "Voice/Mic" },  // phone <- voice/mic
  { 0x1A, 0x1C, 0x04, "Phone" },      // phone
  { 0x29, 0x23, 0x05, "Return" },     // return <- view (on wheels with "view" button)
  { 0x22, 0x04, 0x06, "Up" },         // up
  { 0x23, 0x05, 0x07, "Down" },       // down
  { 0x09, 0x03, 0x08, "Source-" },    // src-
  { 0x0A, 0x02, 0x09, "Source+" },    // src+
  { 0x28, 0x07, 0x10, "OK" },         // ok
  { 0x06, 0x10, 0xA, "Volume+" },     // vol+
  { 0x07, 0x11, 0x00, "Volume-" },    // vol-
  { 0x2B, 0x0C, 0x00, "Voice/Mic" },  // voice/mic <- ACC mode (on wheels with "view" button)
  { 0x42, 0x0C, 0x00, "Voice/Mic" },  // voice/mic <- ACC mode (on wheels with "view" button)
};