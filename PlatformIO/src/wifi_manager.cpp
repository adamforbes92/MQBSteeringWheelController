/*
  wifi_manager — universal ESP32 SoftAP + mDNS + web-serving module (impl, v2)
  See wifi_manager.h for the full rationale and usage notes.
*/

#include "wifi_manager.h"
#include "ota_manager.h"

// mDNS is optional: a project built as arduino+espidf (Ignitron) does not ship
// the ESPmDNS library, and one that never set mdnsName does not need it.
// ESPmDNS.h is itself empty unless the IDF mdns component is configured
// (CONFIG_MDNS_MAX_INTERFACES from sdkconfig.h), so key on that.
#include "sdkconfig.h"
#if defined(CONFIG_MDNS_MAX_INTERFACES) && defined(__has_include)
#if __has_include(<ESPmDNS.h>)
#include <ESPmDNS.h>
#define WIFI_MGR_HAS_MDNS 1
#endif
#endif
#ifndef WIFI_MGR_HAS_MDNS
#define WIFI_MGR_HAS_MDNS 0
#endif
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

// Optional serial logging. Define WIFI_MGR_LOG=1 (e.g. in platformio.ini build
// flags) to enable; off by default so the module drops into any project.
#ifndef WIFI_MGR_LOG
#define WIFI_MGR_LOG 0
#endif

// ---- Module state -----------------------------------------------------------
static wifimgr_config_t g_wcfg;
static bool g_initialised = false;
static bool g_mdnsUp = false;

// Bridge mode (home-network STA alongside the AP)
static char g_staSsid[33] = "";
static char g_staPass[65] = "";
static bool g_staConnected = false;
static String g_staIp = "";
static const uint32_t STA_INITIAL_WINDOW_MS = 20000;       // keep the core's auto-reconnect for this long after (re)start
static const uint32_t STA_RETRY_INTERVAL_MS = 5UL * 60000; // then one fresh attempt this often
static uint32_t g_staStartedMs = 0;
static uint32_t g_staLastAttemptMs = 0;
static bool g_staBackedOff = false;
static volatile bool g_staRestartPending = false; // set by the POST handler, acted on in wifiManagerTick()

// ---- Helpers ----------------------------------------------------------------
static void wifiLog(const char *fmt, ...)
{
#if WIFI_MGR_LOG
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[WIFI] %s\n", buf);
#else
  (void)fmt;
#endif
}

static bool strHasContent(const char *s) { return s != nullptr && s[0] != '\0'; }

static String jsonEscape(const String &s)
{
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// ---- Bridge-mode credentials (NVS) -------------------------------------------
static void staLoad(void)
{
  Preferences p;
  // Read-write, not read-only: opening a namespace read-only before it has
  // ever been written fails with NOT_FOUND, and Preferences logs that at
  // ERROR level on every cold boot. Opening read-write creates the namespace
  // on first use and writes nothing, so the load is silent and harmless.
  if (!p.begin("wifimgr", false))
    return;
  String s = p.getString("staSsid", "");
  String w = p.getString("staPass", "");
  p.end();
  strncpy(g_staSsid, s.c_str(), sizeof(g_staSsid) - 1);
  g_staSsid[sizeof(g_staSsid) - 1] = '\0';
  strncpy(g_staPass, w.c_str(), sizeof(g_staPass) - 1);
  g_staPass[sizeof(g_staPass) - 1] = '\0';
}

static void staSave(void)
{
  Preferences p;
  if (!p.begin("wifimgr", false))
    return;
  p.putString("staSsid", g_staSsid);
  p.putString("staPass", g_staPass);
  p.end();
}

// ---- mDNS -------------------------------------------------------------------
static void wifiStopMdns(void)
{
#if WIFI_MGR_HAS_MDNS
  if (g_mdnsUp)
    MDNS.end();
#endif
  g_mdnsUp = false;
}

static void wifiStartMdns(void)
{
  if (!strHasContent(g_wcfg.mdnsName))
    return;
  wifiStopMdns();
#if WIFI_MGR_HAS_MDNS
  if (MDNS.begin(g_wcfg.mdnsName))
  {
    MDNS.addService("http", "tcp", 80);
    g_mdnsUp = true;
    wifiLog("mDNS up: http://%s.local", g_wcfg.mdnsName);
  }
  else
    wifiLog("mDNS begin failed");
#else
  wifiLog("mDNS not available in this build - %s.local will not resolve", g_wcfg.mdnsName);
#endif
}

// ---- STA (bridge mode) --------------------------------------------------------
// One radio is shared between AP and STA, and every STA connect attempt scans
// all channels, pulling the AP off its own channel for a second or two. Fine
// when the network is there (one attempt, done); with the home SSID saved and
// the car parked anywhere else the core's auto-reconnect would re-scan forever
// and the AP would stutter for whoever is in the car. So: try hard for a short
// window after (re)start, then back off to one attempt every few minutes.
static void staBegin(void)
{
  if (strlen(g_staPass) >= 8)
    WiFi.begin(g_staSsid, g_staPass);
  else
    WiFi.begin(g_staSsid); // open network
  g_staLastAttemptMs = millis();
}

static void staStart(void)
{
  g_staConnected = false;
  g_staIp = "";
  g_staBackedOff = false;
  if (!g_wcfg.enableSta || g_staSsid[0] == '\0')
    return; // bridge mode off - stay in plain WIFI_AP
  if (WiFi.getMode() == WIFI_OFF)
    return; // no AP up (a project-managed AP is off) - nothing to ride alongside
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false); // credentials live in our own NVS namespace, not the core's blob
  WiFi.setAutoReconnect(true);
  WiFi.setMinSecurity(strlen(g_staPass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN); // core default refuses open networks
  g_staStartedMs = millis();
  staBegin();
  wifiLog("bridge mode: joining \"%s\"...", g_staSsid);
}

void wifiManagerStaStart(void)
{
  if (!g_initialised)
    return;
  staStart();
}

void wifiManagerStaStop(void)
{
  if (!g_initialised || !g_wcfg.enableSta)
    return;
  if (g_staSsid[0] != '\0')
    WiFi.disconnect(false, false);
  g_staConnected = false;
  g_staIp = "";
  if (WiFi.getMode() == WIFI_AP_STA)
    WiFi.mode(WIFI_AP);
}

void wifiManagerTick(void)
{
  if (g_staRestartPending)
  {
    g_staRestartPending = false;
    if (g_wcfg.manageAp)
      wifiManagerStartAP(); // apply new credentials (AP restarts too - clients re-join)
    else
    {
      wifiManagerStaStop(); // project-managed AP stays up; only the STA side changes
      wifiManagerStaStart();
    }
    return;
  }
  if (!g_wcfg.enableSta || g_staSsid[0] == '\0')
    return;
  const uint32_t now = millis();
  const bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (nowConnected && !g_staConnected)
  {
    g_staIp = WiFi.localIP().toString();
    wifiLog("bridge mode: connected to \"%s\" as %s", g_staSsid, g_staIp.c_str());
    if (g_staBackedOff)
    {
      WiFi.setAutoReconnect(true); // connected: let the core handle brief drop-outs again
      g_staBackedOff = false;
    }
    g_staStartedMs = now; // a later drop gets a fresh fast window
  }
  else if (!nowConnected && g_staConnected)
  {
    g_staIp = "";
    wifiLog("bridge mode: lost \"%s\"", g_staSsid);
  }
  g_staConnected = nowConnected;

  if (!nowConnected)
  {
    if (!g_staBackedOff && (now - g_staStartedMs) > STA_INITIAL_WINDOW_MS)
    {
      WiFi.setAutoReconnect(false); // not there: stop the continuous re-scan so the AP is left alone
      WiFi.disconnect(false, false);
      g_staBackedOff = true;
      wifiLog("bridge mode: \"%s\" not found - retrying every %lu min", g_staSsid, (unsigned long)(STA_RETRY_INTERVAL_MS / 60000UL));
    }
    else if (g_staBackedOff && (now - g_staLastAttemptMs) > STA_RETRY_INTERVAL_MS)
    {
      staBegin(); // one shot; the core gives up on its own without auto-reconnect
    }
  }
}

bool wifiManagerStaConfigured(void) { return g_wcfg.enableSta && g_staSsid[0] != '\0'; }
bool wifiManagerStaConnected(void) { return g_staConnected; }
const char *wifiManagerStaSsid(void) { return g_staSsid; }
String wifiManagerStaIp(void) { return g_staIp; }

// ---- Public API -------------------------------------------------------------
wifimgr_config_t wifiDefaultConfig(void)
{
  wifimgr_config_t c = {};
  c.hostName = "ESP32";
  c.mdnsName = nullptr;
  c.fwVersion = nullptr;
  c.apPassword = nullptr;

  c.apChannel = 1;
  c.apHidden = false;
  c.apMaxClients = 4;

  c.apIp = IPAddress(192, 168, 1, 1);
  c.apGateway = IPAddress(192, 168, 1, 1);
  c.apSubnet = IPAddress(255, 255, 255, 0);

  c.disableWifiSleep = true;
  c.mountLittleFS = true;
  c.enableSta = true;
  c.manageAp = true;
  c.indexPath = "/index.html";
  return c;
}

void wifiManagerStartAP(void)
{
  if (!g_initialised)
    return;
  if (!g_wcfg.manageAp)
  {
    staStart();      // the project has its own AP up (or will call wifiManagerStaStart() when it does)
    wifiStartMdns(); // still ours to start - the project's AP is up, the name is not
    return;
  }

  WiFi.mode(WIFI_AP);
  if (strHasContent(g_wcfg.hostName))
    WiFi.softAPsetHostname(g_wcfg.hostName);

  WiFi.softAPConfig(g_wcfg.apIp, g_wcfg.apGateway, g_wcfg.apSubnet);

  // Open network by default; no captive-portal DNS so the phone keeps mobile
  // data and simply sees a hotspot with no internet.
  const char *pwd = strHasContent(g_wcfg.apPassword) ? g_wcfg.apPassword : nullptr;
  WiFi.softAP(g_wcfg.hostName, pwd, g_wcfg.apChannel, g_wcfg.apHidden ? 1 : 0, g_wcfg.apMaxClients);

  if (g_wcfg.disableWifiSleep)
    WiFi.setSleep(false);

  wifiLog("SoftAP \"%s\" @ %s", g_wcfg.hostName, WiFi.softAPIP().toString().c_str());

  staStart(); // switches to WIFI_AP_STA and starts joining when a home network is saved
  wifiStartMdns();
}

void wifiManagerStopAP(void)
{
  wifiStopMdns();
  if (wifiManagerStaConfigured())
    WiFi.disconnect(false, false);
  g_staConnected = false;
  g_staIp = "";
  if (!g_wcfg.manageAp)
    return; // the project takes its own AP down
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiLog("SoftAP stopped");
}

void wifiManagerInit(const wifimgr_config_t *cfg)
{
  g_wcfg = cfg ? *cfg : wifiDefaultConfig();
  g_initialised = true;

  // Guarded mount: a half-written image never reaches lfs (see ota_manager).
  // A failure here is not fatal - the web server still starts and "/" serves
  // the recovery page.
  if (g_wcfg.mountLittleFS)
  {
    if (!otaFsMountSafe())
      wifiLog("web UI filesystem not usable - recovery page will be served");
  }

  if (g_wcfg.enableSta)
    staLoad();

  wifiManagerStartAP();
}

// ---- Recovery page ----------------------------------------------------------
// Served at "/" when the web UI filesystem is missing, broken or empty. Needs
// nothing from LittleFS: two uploads straight to the OTA endpoints, web UI
// first. Deliberately plain - it has to work from any phone browser.
static const char RECOVERY_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>%PRODUCT% recovery</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:520px}
h1{font-size:20px}p{line-height:1.5;color:#bbb}code{color:#fff}section{border:1px solid #333;border-radius:10px;padding:14px;margin:14px 0}
input[type=file]{display:block;margin:10px 0;max-width:100%}button{background:#2a6df4;color:#fff;border:0;border-radius:8px;padding:10px 16px;font-size:15px}
button:disabled{opacity:.5}.s{margin-top:8px;font-size:14px;color:#9c9}.e{color:#f77}.bar{height:6px;background:#333;border-radius:3px;margin-top:8px}.bar i{display:block;height:100%;width:0;background:#2a6df4;border-radius:3px}
pre{white-space:pre-wrap;word-break:break-all;font-size:12px;color:#bbb;margin:10px 0 0}</style></head>
<body><h1>%PRODUCT% &middot; web UI missing</h1>
<p>The controller is running (firmware <code>%FW%</code>) but its web-interface partition holds no usable filesystem - usually a filesystem update that stopped part-way. Nothing else is affected. Upload the two files from the release folder (<code>Releases/V&hellip;/</code>): the web UI first, then the firmware if you were mid-update.</p>
<section><strong>1. Web UI</strong> &mdash; <code>littlefs.bin</code><input type="file" id="fs" accept=".bin"><button id="fsb">Upload web UI</button><div class="bar"><i id="fsp"></i></div><div class="s" id="fss"></div></section>
<section><strong>2. Firmware</strong> &mdash; <code>firmware.bin</code> (optional; reboots when done)<input type="file" id="fw" accept=".bin"><button id="fwb">Upload firmware</button><div class="bar"><i id="fwp"></i></div><div class="s" id="fws"></div></section>
<section><strong>Partition diagnostics</strong> <button id="dgb" style="float:right;padding:6px 10px;font-size:13px">Refresh</button><pre id="dg">loading&hellip;</pre></section>
<script>
function up(k,url,field,done,then){var f=document.getElementById(k).files[0],b=document.getElementById(k+'b'),s=document.getElementById(k+'s'),p=document.getElementById(k+'p');
if(!f){s.textContent='Pick the file first.';s.className='s e';return}b.disabled=true;s.className='s';s.textContent='Uploading…';
var d=new FormData();d.append(field,f,f.name);var x=new XMLHttpRequest();x.open('POST',url+'?size='+f.size);
x.upload.onprogress=function(e){if(e.lengthComputable)p.style.width=Math.round(e.loaded/e.total*100)+'%'};
x.onload=function(){var m='';try{m=JSON.parse(x.responseText).message}catch(e){m=x.responseText}if(x.status===200){s.textContent=done;if(then)then()}else{s.className='s e';s.textContent=m||('Failed ('+x.status+')');b.disabled=false}};
x.onerror=function(){s.className='s e';s.textContent='Upload failed - check the connection and retry.';b.disabled=false};x.send(d)}
document.getElementById('fsb').onclick=function(){up('fs','/api/ota/fs','filesystem','Web UI installed - opening it…',function(){setTimeout(function(){location.reload()},1500)})};
document.getElementById('fwb').onclick=function(){up('fw','/api/ota','firmware','Firmware installed - rebooting. Reload this page in ~20 s.')};
function diag(){var x=new XMLHttpRequest();x.open('GET','/api/ota/fsdiag');x.onload=function(){try{var d=JSON.parse(x.responseText),o='';for(var k in d)o+=k+': '+d[k]+'\n';document.getElementById('dg').textContent=o}catch(e){document.getElementById('dg').textContent=x.responseText}};x.send()}
document.getElementById('dgb').onclick=diag;diag();
</script></body></html>)HTML";

void wifiManagerAttachStatic(AsyncWebServer &server)
{
  // "/" decides per request, so a filesystem upload from the recovery page
  // switches straight over to the real UI without a reboot.
  auto serveIndex = [](AsyncWebServerRequest *request)
  {
    AsyncWebServerResponse *res;
    if (otaFsUiAvailable())
    {
      const char *path = strHasContent(g_wcfg.indexPath) ? g_wcfg.indexPath : "/index.html";
      if (!LittleFS.exists(path))
      {
        request->send(404, "text/plain", "index not found");
        return;
      }
      // Streamed straight off the filesystem - no large allocation. The old
      // path read the whole 40 KB file into a String and let beginResponse()
      // copy it again; readString() returns a SHORT string on a failed alloc
      // rather than an error, so a heap squeeze served a valid 200 with an
      // empty body (a blank page, nothing in the log).
      //
      // Asset cache-busting (?v=) is baked into index.html when FW_VERSION is
      // bumped - NOT substituted at runtime. tools/make_release.py refuses to
      // cut a release if they disagree.
      res = request->beginResponse(LittleFS, path, "text/html");
    }
    else
    {
      String html = FPSTR(RECOVERY_HTML);
      html.replace("%PRODUCT%", otaManagerProduct());
      html.replace("%FW%", strHasContent(g_wcfg.fwVersion) ? g_wcfg.fwVersion : "?");
      res = request->beginResponse(200, "text/html", html);
    }
    res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    res->addHeader("Pragma", "no-cache");
    res->addHeader("Expires", "0");
    request->send(res);
  };

  server.on("/", HTTP_GET, serveIndex);
  server.on("/index.html", HTTP_GET, serveIndex);

  // app.js / style.css / images: the browser keeps a copy but asks every time
  // (If-None-Match against the ETag the handler derives from the file's mtime
  // or size) and gets a 304 unless the file changed. The filter keeps the
  // handler out of the way while nothing is mounted.
  server.serveStatic("/", LittleFS, "/").setCacheControl("no-cache").setFilter([](AsyncWebServerRequest *request)
                                                                                { (void)request; return otaFsMounted(); });
}

void wifiManagerAttachSta(AsyncWebServer &server)
{
  // GET /api/wifi/scan - async: the first call starts it and answers
  // {"scanning":true}; the page polls until the list is back. A blocking scan
  // would sit inside the async_tcp task for 2-3 s. Manual action only - the
  // single radio leaves the AP's channel for the scan's duration.
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { request->send(200, "application/json", "{\"scanning\":true}"); return; }
    if (n == WIFI_SCAN_FAILED || n < 0)
    {
      WiFi.scanNetworks(true /*async*/, false /*hidden*/);
      request->send(200, "application/json", "{\"scanning\":true}");
      return;
    }
    struct Net { String ssid; int32_t rssi; bool secure; };
    std::vector<Net> list;
    for (int16_t i = 0; i < n; i++)
    {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue; // hidden - type it in instead
      int32_t rssi = WiFi.RSSI(i);
      bool merged = false;
      for (auto &e : list) if (e.ssid == ssid) { if (rssi > e.rssi) e.rssi = rssi; merged = true; break; }
      if (!merged) list.push_back({ssid, rssi, WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
    }
    std::sort(list.begin(), list.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
    String json = "{\"scanning\":false,\"networks\":[";
    for (size_t i = 0; i < list.size(); i++)
    {
      if (i) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(list[i].ssid) + "\",\"rssi\":" + String(list[i].rssi) + ",\"secure\":" + String(list[i].secure ? "true" : "false") + "}";
    }
    json += "]}";
    WiFi.scanDelete(); // next GET starts a fresh scan
    request->send(200, "application/json", json); });

  // POST /api/wifi/sta/reset - forget the home network, back to AP only
  server.on("/api/wifi/sta/reset", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    g_staSsid[0] = '\0';
    g_staPass[0] = '\0';
    staSave();
    g_staRestartPending = true;
    request->send(200, "application/json", "{\"ok\":true}"); });

  // GET /api/wifi/sta - status. Password is write-only, never returned.
  server.on("/api/wifi/sta", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String json = "{";
    json += "\"enabled\":" + String(g_wcfg.enableSta ? "true" : "false") + ",";
    json += "\"ssid\":\"" + jsonEscape(g_staSsid) + "\",";
    json += "\"passwordSet\":" + String(strlen(g_staPass) >= 8 ? "true" : "false") + ",";
    json += "\"connected\":" + String(g_staConnected ? "true" : "false") + ",";
    json += "\"ip\":\"" + g_staIp + "\",";
    json += "\"rssi\":" + (g_staConnected ? String(WiFi.RSSI()) : String("null")) + ",";
    json += "\"mdns\":\"" + String(wifiManagerMdnsName()) + "\"}";
    request->send(200, "application/json", json); });

  // POST /api/wifi/sta  (application/x-www-form-urlencoded: ssid, password)
  // Empty ssid = disable. Applied from wifiManagerTick() so the response gets out first.
  server.on("/api/wifi/sta", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!g_wcfg.enableSta) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"Bridge mode is not available on this device\"}"); return; }
    String ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : "";
    String pwd = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
    ssid.trim();
    if (ssid.length() > 32) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"SSID too long (max 32)\"}"); return; }
    for (size_t i = 0; i < ssid.length(); ++i) { unsigned char c = ssid[i]; if (c < 0x20 || c > 0x7E) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"SSID must be printable ASCII\"}"); return; } }
    if (pwd.length() > 0 && pwd.length() < 8) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"Password must be at least 8 characters, or blank for an open network\"}"); return; }
    if (pwd.length() > 64) { request->send(400, "application/json", "{\"ok\":false,\"error\":\"Password too long (max 64)\"}"); return; }
    memset(g_staSsid, 0, sizeof(g_staSsid));
    strncpy(g_staSsid, ssid.c_str(), sizeof(g_staSsid) - 1);
    memset(g_staPass, 0, sizeof(g_staPass));
    if (pwd.length()) strncpy(g_staPass, pwd.c_str(), sizeof(g_staPass) - 1);
    staSave();
    g_staRestartPending = true;
    request->send(200, "application/json", "{\"ok\":true,\"ssid\":\"" + jsonEscape(g_staSsid) + "\"}"); });
}

const char *wifiManagerFwVersion(void)
{
  return strHasContent(g_wcfg.fwVersion) ? g_wcfg.fwVersion : "";
}

const char *wifiManagerMdnsName(void)
{
  return strHasContent(g_wcfg.mdnsName) ? g_wcfg.mdnsName : "";
}
