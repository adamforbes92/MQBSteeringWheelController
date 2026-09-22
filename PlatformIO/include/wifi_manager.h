#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

/*
  wifi_manager — universal ESP32 SoftAP + mDNS + web-serving module (v2, 2026-09-20)
  ---------------------------------------------------------------------------------
  Drop-in, project-agnostic WiFi front-end for the Forbes Automotive ESP32
  projects so every product brings its network up the same way. Companion to
  ota_manager and power_manager.

  What it standardises
  --------------------
    1. SoftAP on a fixed IP (default 192.168.1.1), open SSID named after the
       project, no captive portal (so phones keep mobile data - see below).
    2. mDNS: http://<mdnsName>.local as well as the IP.
    3. The web UI filesystem, mounted through ota_manager's guard
       (otaFsMountSafe - never hands lfs a half-written image), and served
       with the right caching. The web server always starts: with no usable
       UI, "/" is a built-in RECOVERY PAGE with the two upload forms, so a
       failed filesystem update is never a USB job.
    4. Optional "Home WiFi" bridge mode: join a home/garage router as a
       station alongside the AP so a phone on that network reaches the
       device AND the internet at once - which is what the guided GitHub
       update in data/ota.js needs. Credentials live in NVS (Preferences).

  Why it appears as a normal hotspot (phone data stays available)
  ---------------------------------------------------------------
  No captive-portal DNS. A captive portal makes the phone believe it must
  "sign in" and kills mobile data while connected. As a plain hotspot with no
  internet, most phones keep using cellular - though Android needs the user to
  accept the "no internet" network once, and some phones still drop data.
  Bridge mode is the reliable route.

  Caching (v2)
  ------------
  v1 cached app.js/style.css for a year and relied on a %FW_VERSION% query
  string in index.html to bust it. Every same-version rebuild during
  development then ran a stale app.js against new HTML (dead buttons). Now
  index.html is served no-store and the other assets `no-cache`: the browser
  keeps a copy but revalidates each load (ETag from LittleFS mtime/size, 304
  when unchanged). %FW_VERSION% is still substituted for compatibility.

  How to use (any project)
  ------------------------
    #include "wifi_manager.h"
    #include "ota_manager.h"

    // setup(), before your API routes:
    wifimgr_config_t wcfg = wifiDefaultConfig();
    wcfg.hostName  = "SpeedPulser";  // SoftAP SSID + hostname
    wcfg.mdnsName  = "speedpulser";  // -> http://speedpulser.local
    wcfg.fwVersion = FW_VERSION;
    wifiManagerInit(&wcfg);          // mounts FS (guarded), AP up, STA if saved

    ota_config_t ocfg = otaDefaultConfig(); ... otaManagerInit(&ocfg);
    otaManagerAttach(server);        // /api/ota/*
    wifiManagerAttachSta(server);    // /api/wifi/sta, /api/wifi/sta/reset, /api/wifi/scan
    // ... your own /api routes ...
    wifiManagerAttachStatic(server); // "/" (UI or recovery page) + static files
    server.begin();

    // loop():
    wifiManagerTick();               // bridge-mode connection tracking / back-off

  power_manager integration: on wake  -> wifiManagerStartAP(); server.begin();
                             on sleep -> server.end(); wifiManagerStopAP();

  Depends on WiFi, ESPmDNS, LittleFS, Preferences, ESPAsyncWebServer and
  ota_manager (for the filesystem guards).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// ---- Configuration ----------------------------------------------------------
typedef struct
{
  const char *hostName;   // SoftAP SSID + WiFi.hostname (e.g. "MFSWController")
  const char *mdnsName;   // http://<mdnsName>.local (e.g. "mfsw"). NULL/"" = skip mDNS.
  const char *fwVersion;  // firmware version string (substituted for %FW_VERSION% in index.html)
  const char *apPassword; // NULL or "" = open network (recommended for in-car use)

  uint8_t apChannel;      // SoftAP channel (1-13, default 1)
  bool apHidden;          // hide the SSID (default false)
  uint8_t apMaxClients;   // max associated stations (default 4)

  IPAddress apIp;         // SoftAP IP        (default 192.168.1.1)
  IPAddress apGateway;    // SoftAP gateway   (default 192.168.1.1)
  IPAddress apSubnet;     // SoftAP subnet    (default 255.255.255.0)

  bool disableWifiSleep;  // WiFi.setSleep(false) for a snappier server.

  bool mountLittleFS;     // mount the web UI filesystem at init (default true)
  bool enableSta;         // bridge mode available (default true); false = AP only
  bool manageAp;          // default true. false = the project brings its own soft-AP up/down
                          // (Ignitron: channel/country/USB-IP tuning) and calls
                          // wifiManagerStaStart()/Stop() around it; this module then only
                          // does the filesystem guard, static serving and the STA side.

  const char *indexPath;  // web root index file (default "/index.html")
} wifimgr_config_t;

wifimgr_config_t wifiDefaultConfig(void);

// Store the config, mount the filesystem (guarded) and bring the SoftAP + mDNS
// up, plus the STA side if a home network is saved. Does NOT start the web
// server.
void wifiManagerInit(const wifimgr_config_t *cfg);

// (Re)start AP (+ STA when configured) and mDNS from the stored config.
void wifiManagerStartAP(void);

// Stop mDNS, drop STA and AP, radio off. Stop your web server first.
void wifiManagerStopAP(void);

// "/" and "/index.html" -> the UI (index.html, no-store, %FW_VERSION% substituted)
// or the recovery page when the filesystem holds no usable UI; everything else
// from LittleFS with Cache-Control: no-cache. Call AFTER your own API routes
// and BEFORE server.begin().
void wifiManagerAttachStatic(AsyncWebServer &server);

// Bridge-mode routes. Call BEFORE any of your own "/api/wifi..." routes (the
// server matches "<uri>/..." prefixes too):
//   GET  /api/wifi/sta        -> { ssid, passwordSet, connected, ip, rssi, mdns }
//   POST /api/wifi/sta        -> form fields ssid (empty = disable), password (blank = open)
//   POST /api/wifi/sta/reset  -> forget the home network
//   GET  /api/wifi/scan       -> async scan: {scanning:true} until {networks:[{ssid,rssi,secure}]}
void wifiManagerAttachSta(AsyncWebServer &server);

// Bridge-mode STA on its own (for manageAp = false projects): start joining the
// saved home network alongside the AP the project already has up (switches to
// WIFI_AP_STA), or drop it. No-ops when nothing is saved / bridge mode is off.
void wifiManagerStaStart(void);
void wifiManagerStaStop(void);

// Call from loop(). Tracks the STA link and runs the retry back-off (20 s of
// the core's auto-reconnect after (re)start, then one attempt every 5 min so a
// saved home SSID can't keep pulling the single radio off the AP's channel
// while the car is away from home). No-op when bridge mode is off.
void wifiManagerTick(void);

// Bridge-mode state for your own UI / logs.
bool wifiManagerStaConfigured(void);
bool wifiManagerStaConnected(void);
const char *wifiManagerStaSsid(void);
String wifiManagerStaIp(void);

const char *wifiManagerFwVersion(void);
const char *wifiManagerMdnsName(void);

#endif // WIFI_MANAGER_H
