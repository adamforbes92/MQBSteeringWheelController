/* A Multi-function VAG Based Steering Wheel adapter - used to convert old steering 
wheel inputs into new.  

Will accept PWM-type auxiliary light input for dimming, and up to 24 button
mappings with resistive and/or CAN output.  

Also has a PNP output for activating a relay or similar when certain buttons are pressed.

Created by Forbes Automotive.com
*/

#include <Arduino.h>

#include "io.h"
#include "tasks.h"
#include "API.h"
#include "power_manager.h"

void setup() {
  basicInit(); // basic init for IO
  setupWiFi(); // setup WiFi connection and mDNS
  setupApiServer(); // setup API server

  // Universal reduced-power module: turns WiFi off 1 min after the last client
  // disconnects, scales CPU 240->80 MHz, releases Bluetooth and kills the
  // onboard LED to cut current draw (and therefore linear-regulator heat).
  power_config_t pcfg = powerDefaultConfig();
  powerInit(&pcfg);

  startTasks(); // begin FreeRTOS tasks for LIN handling, CAN handling, and output control
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000)); // purely idling in the main loop — all work is done in tasks and ISRs
}
