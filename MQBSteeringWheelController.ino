/*********************
PQ & MQB Based LIN Controller for VW based steering wheels.
This example is based on the 6R GTi Wheel but all models will be similar.

Code has a structure to get wheel button 'numbers' and translate them into new numbers.  Use function 'buttonTranspose' in _defs to change the outputs from the buttons

The PCB has 2x LIN chips for 'changing' into different bits.  Possible solution for other marques - although this is based for use on the MK4 platform.  Mostly untested(!)  Would need examples of other steering wheels!

You HAVE to send a light frame to 'wake' the board up and receive data back - hence the constant light data...  The 'grey' wire needs 12v to keep the board awake - tie these two together at the steering wheel side to save
on pins coming back through the squib

Forbes-Automotive.com; 2024
**********************/

#include "MQBSteeringWheelController_defs.h"

// setup LIN libraries for Serial1 & 2, define pins.  SteeringWheelLIN goes to the SW, chassis goes to the gateway (for >MK4)
LIN_Master_HardwareSerial_ESP32 steeringWheelLIN(Serial1, pinRX_LINSteeringWheel, pinTX_LINSteeringWheel, "LIN_SteeringWheel");  // parameters: interface, Rx, Tx, name
LIN_Master_HardwareSerial_ESP32 chassisLIN(Serial2, pinRX_LINchassis, pinTX_LINchassis, "LIN_chassis");                          // parameters: interface, Rx, Tx, name

// for sending data over CAN, if req.
ESP32_CAN<RX_SIZE_256, TX_SIZE_16> chassisCAN;

// digipot for radio control
DigiPot radioResistor(resistorInc, resistorUD, resistorCS);

// EEPROM - for remembering settings
Preferences preferences;
#define ENABLE_DEBUG  // State Debug - set to 0 on release ** CAN CHANGE THIS **

void auxLightPWM() {    // Interrupt 0 service routine
  lastRead = micros();  // Get current time in micros
  if (rise_Time == 0) {
    rise_Time = lastRead;
  }

  if (digitalRead(pinAuxLight) == HIGH) {
    // Falling edge
    fall_Time = lastRead;  // Just store falling edge and calculate on rising edge
  } else {
    // Rising edge
    total_Time = rise_Time - lastRead;   // Get total cycle time
    on_Time = fall_Time - rise_Time;     // Get on time during this cycle
    total_Time = total_Time / on_Time;   // Divide it down
    dutyCycle = (total_Time / on_Time);  // Convert to a percentage
    rise_Time = lastRead;                // Store rise time
  }
}

void setup() {
  basicInit();  // basic init in '_io'.  Keeps this page clean because of all of the 'serial_debugs'
}

void loop() {
  steeringWheelLIN.handler();  // tick over the LIN handler
  chassisLIN.handler();        // tick over the LIN handler

  if (hasAuxLight) {                   // check to see if 'aux light' is enabled - this will capture using the input/interrupt the freq. of the PWM signal from the ~MK4 based rheostat
    if (dutyCycle > upperLightsAux) {  // overwrite the new highest duty cycle so that the upper bounds of the rheostat is maintained.  *** REVIEW ***
      upperLightsAux = dutyCycle;
    }

    DEBUG(map(dutyCycle, 0, upperLightsAux, 0, upperLightsLIN), HEX);

    if (dutyCycle == 0) {
      steeringWheelLightData[0] = 0x00;  // force light data regardless so that the steering wheel feeds back... 0x64 is full brightness
    } else {
      steeringWheelLightData[0] = map(dutyCycle, 0, upperLightsAux, 0, upperLightsLIN);  // convert the rheostat PWM into a useable (0-0x7F) output.  Assumed linear(!) *** REVIEW ***
    }
  } else {                              // if !MK4, capture the light signal from LIN2 chip (>Mk4 platform)
    getLightLINFrame();                 // if has a gateway installed, check for LIN data for lights
    if (gatewayLightData[0] != 0x00) {  // if found LIN data for lights, let's pass it on...
      steeringWheelLightData[0] = gatewayLightData[0];
    }
  }

  if ((millis() - lastMillis) > linPause) {  // check to see if x ms (linPause) has elapsed - slow down the frames!
    lastMillis = millis();
    sendLightLINFrame();  // send the LIN frame
  }

  // now that light data has been processed check for button states
  getButtonState();
  sendButtonLINFrame();

  if (hasCAN) {
    broadcastButtonsCAN();
  }

  if (hasCAN && dsgPaddleUp) {
    sendPaddleUpFrame();
  }
  if (hasCAN && dsgPaddleDown) {
    sendPaddleDownFrame();
  }

  if (hasResistiveStereo && radioResistance != 0) {
    radioResistor.set(radioResistance);
    radioResistance = 0;
    delay(100);
    radioResistor.set(0);
    delay(100);
  }
}