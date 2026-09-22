#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

/*
  ota_manager — universal ESP32 over-the-air update module (v2, 2026-09-20)
  -------------------------------------------------------------------------
  Drop-in, project-agnostic OTA back-end for the Forbes Automotive ESP32
  projects so every product updates the same way, driven by the shared
  data/ota.js front-end. Companion to wifi_manager and power_manager.

  v2 is the port of what was learned getting OpenHaldex's guided update to
  work end to end. The rules it encodes (see also the notes in the .cpp):

    1. Upload callbacks NEVER send a response. The body is still streaming
       while they run; a response mid-body makes the browser close the socket
       under the parser -> AsyncTCP use-after-free (Guru Meditation in
       tcp_output) and a half-written partition. The outcome is recorded and
       sent by the request handler once the whole body has arrived. (v1 got
       this right; the per-project hand-written handlers did not.)
    2. Filesystem uploads unmount LittleFS first, check the body length the
       browser announced (?size=), remount and check the web UI is really
       there before answering OK, and on ANY failure erase the superblock pair
       so a half-written image can never be mounted. LittleFS is built with
       asserts on and the panic handler reboots: a hybrid of two images can
       boot-loop a unit with no web server left to recover from.
    3. The filesystem is only ever mounted after a look at its superblock
       (otaFsMountSafe). wifi_manager uses it at boot and always starts the
       web server; with no usable UI, "/" is a built-in recovery page.
    4. A filesystem update does NOT reboot (the guided flow goes on to flash
       the firmware). A firmware update reboots after the response is out.
    5. No hash gate on the upload stream: esp_ota_end validates a firmware
       image and mounting validates a filesystem image.

  Two independent update targets
  ------------------------------
    FIRMWARE   (application) -> firmware.bin -> inactive OTA app slot (U_FLASH)
    FILESYSTEM (web UI)      -> littlefs.bin -> "spiffs" data partition (U_SPIFFS)

  HTTP endpoints registered by otaManagerAttach()
  -----------------------------------------------
    GET  /api/ota/info    -> version, fsVersion, chip, partition, product, repo…
    GET  /api/ota/check   -> { allowed, reason } from the otaIsSafe() hook
    GET  /api/ota/fsinfo  -> { ok, mounted, fsVersion, fwVersion } after a FS upload
    GET  /api/ota/fsdiag  -> what is physically in the filesystem partition
    POST /api/ota         -> multipart firmware.bin  (?size=<bytes>)
    POST /api/ota/fs      -> multipart littlefs.bin  (?size=<bytes>)

  How to use (any project)
  ------------------------
    #include "ota_manager.h"

    ota_config_t ocfg = otaDefaultConfig();
    ocfg.fwVersion  = FW_VERSION;                 // reported by /api/ota/info
    ocfg.product    = "SpeedPulser";              // shown on the recovery page + OTA tab
    ocfg.githubRepo = "Forbes-Automotive/speedPulser"; // Releases/ + releases.json live here
    otaManagerInit(&ocfg);
    otaManagerAttach(server);                     // BEFORE server.begin()

  The guided "Check for updates" in data/ota.js reads repo/branch/releasesDir
  from /api/ota/info, fetches Releases/releases.json (written by
  tools/make_release.py) and pushes each image through the two POST routes.

  Hooks (weak, override in your project)
  --------------------------------------
    bool otaIsSafe(String &reason)   — false blocks both uploads (vehicle moving,
                                       motor running…). Default: always safe.
    void otaOnStart(bool filesystem) — an upload is starting (stop motors, LEDs…)
    void otaOnEnd(bool filesystem, bool success)

  power_manager integration: keep the radio up while an upload runs:
      bool powerIsBusy() { return WiFi.softAPgetStationNum() > 0 || otaInProgress(); }

  Plain C++/Arduino; depends only on Update.h, LittleFS and ESPAsyncWebServer.
*/

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// ---- Configuration ----------------------------------------------------------
typedef struct
{
  const char *fwVersion;    // version string reported by /api/ota/info (NULL/"" = "")
  const char *product;      // product name for the UI / recovery page (e.g. "SpeedPulser")
  const char *githubRepo;   // "Owner/repo" holding Releases/ (NULL/"" = no GitHub route offered)
  const char *githubBranch; // default "main"
  const char *releasesDir;  // default "Releases"
  uint32_t rebootDelayMs;   // wait after a successful firmware update before ESP.restart() (default 1500)
  bool verbose;             // Serial.printf status lines
} ota_config_t;

ota_config_t otaDefaultConfig(void);

// Store the config. Safe to call once from setup(). Does not touch the server.
void otaManagerInit(const ota_config_t *cfg);

// Register the /api/ota routes on the given server. Call BEFORE server.begin().
void otaManagerAttach(AsyncWebServer &server);

// True while a firmware or filesystem upload is in progress (also true from a
// successful firmware upload until the reboot).
bool otaInProgress(void);

// True while a browser has hit ANY route in the last 30 s (every request is
// noted through a filter on the first route). A phone working through the
// home router in bridge mode is not an AP station, so a powerIsBusy() that
// only counts stations would switch WiFi off under it:
//   bool powerIsBusy() { return WiFi.softAPgetStationNum() > 0 || otaInProgress() || otaWebClientActive(); }
bool otaWebClientActive(void);
void otaNoteWebActivity(void);

const char *otaManagerFwVersion(void);
const char *otaManagerProduct(void);

// ---- Web UI filesystem guards (used by wifi_manager at boot) -----------------
bool otaFsMountSafe(void);   // mount only if the superblock looks like a LittleFS image that fits the partition
bool otaFsMounted(void);
bool otaFsUiAvailable(void); // mounted and /index.html + /app.js present
void otaFsInvalidate(void);  // erase the superblock pair so a half-written image can never be mounted
String otaFsVersion(void);   // "fs" from /version.json, or "--"

// ---- Weak hooks: override in YOUR project ------------------------------------
bool otaIsSafe(String &reason);
void otaOnStart(bool filesystem);
void otaOnEnd(bool filesystem, bool success);

#endif // OTA_MANAGER_H
