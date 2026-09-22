/* A Multi-function VAG Based Steering Wheel adapter - used to convert old steering 
wheel inputs into new.  

Will accept PWM-type auxiliary light input for dimming, and up to 24 button
mappings with resistive and/or CAN output.  

Also has a PNP output for activating a relay or similar when certain buttons are pressed.

Created by Forbes Automotive.com
*/

#include <Arduino.h>

#include "defs.h"
#include "io.h"
#include "tasks.h"
#include "API.h"
#include "power_manager.h"
#include "wifi_manager.h"
#include "ota_manager.h"

void setup() {
  basicInit(); // basic init for IO
#if ENABLE_IO_TEST
  startTasks(); // manufacturing IO test owns the outputs and bus interfaces
#else
  // Universal WiFi front-end: SoftAP at 192.168.1.1, reachable as mfsw.local,
  // LittleFS mounted, firmware-versioned (cache-busted) UI serving.
  wifimgr_config_t wcfg = wifiDefaultConfig();
  wcfg.hostName = wifiHostName; // SoftAP SSID + hostname
  wcfg.mdnsName = "mfsw";       // http://mfsw.local
  wcfg.fwVersion = FW_VERSION;  // injected into index.html for cache-busting
  wifiManagerInit(&wcfg);

  // Universal OTA module: firmware (U_FLASH) + filesystem (U_SPIFFS) updates,
  // reachable on the "OTA" tab. Must be initialised before setupApiServer(),
  // which registers the OTA routes via otaManagerAttach(server).
  ota_config_t ocfg = otaDefaultConfig();
  ocfg.fwVersion = FW_VERSION;
  ocfg.product = "MFSW Controller";
  ocfg.githubRepo = "adamforbes92/MQBSteeringWheelController"; // Releases/ + releases.json for "Check for updates"
  otaManagerInit(&ocfg);

  setupApiServer(); // register API routes + static serving, then start server

  // Universal reduced-power module: turns WiFi off 1 min after the last client
  // disconnects, scales CPU 240->80 MHz, releases Bluetooth and kills the
  // onboard LED to cut current draw (and therefore linear-regulator heat).
  power_config_t pcfg = powerDefaultConfig();
  powerInit(&pcfg);

  startTasks(); // begin FreeRTOS tasks for LIN handling, CAN handling, and output control
#endif
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000)); // purely idling in the main loop — all work is done in tasks and ISRs
  wifiManagerTick();               // Home WiFi (bridge mode): connection tracking + retry back-off
}
