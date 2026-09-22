/*
  ota_manager — universal ESP32 over-the-air update module (implementation, v2)
  See ota_manager.h for the rationale and usage notes.

  Upload results
  --------------
  The upload callbacks never call request->send(). The body is still streaming
  in while they run; a response written mid-body makes the browser treat the
  upload as finished and drop the connection under the parser - AsyncTCP then
  touches the freed pcb (Guru Meditation in tcp_output) and a half-written
  partition is left behind. Each handler records an outcome (the first one
  wins, later chunks are drained) and the request handler - which
  ESPAsyncWebServer runs once the whole body has been parsed - sends it.

  Filesystem partition guards
  ---------------------------
  LittleFS is built with CONFIG_LITTLEFS_ASSERTS=y and the panic handler
  reboots. Feed lfs a partition that is half one image and half another and it
  can assert while walking the directory tree - at boot, every boot. So the
  partition is only handed to lfs after a look at the superblock pair, a
  failed upload erases that pair, and wifi_manager serves a recovery page
  when there is no usable UI.
*/

#include "ota_manager.h"

#include <Update.h>
#include <LittleFS.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ---- Module state -----------------------------------------------------------
static ota_config_t g_cfg;
static bool g_initialised = false;
static volatile bool g_inProgress = false;
static bool g_fsMounted = false;
static volatile uint32_t g_lastWebActivityMs = 0;
#define OTA_WEB_ACTIVE_WINDOW_MS 30000UL

void otaNoteWebActivity(void) { g_lastWebActivityMs = millis(); }

bool otaWebClientActive(void)
{
  if (g_inProgress)
    return true;
  return g_lastWebActivityMs != 0 && (millis() - g_lastWebActivityMs) < OTA_WEB_ACTIVE_WINDOW_MS;
}

// ---- Weak default hooks (override in your project) --------------------------
__attribute__((weak)) bool otaIsSafe(String &reason)
{
  reason = "Ready";
  return true;
}
__attribute__((weak)) void otaOnStart(bool filesystem) { (void)filesystem; }
__attribute__((weak)) void otaOnEnd(bool filesystem, bool success)
{
  (void)filesystem;
  (void)success;
}

// ---- Helpers ----------------------------------------------------------------
static void otaLog(const char *fmt, ...)
{
  if (!g_cfg.verbose)
    return;
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[OTA] %s\n", buf);
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
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static String hex32(const uint8_t *b)
{
  char s[65];
  for (int i = 0; i < 32; i++)
    sprintf(s + i * 2, "%02x", b[i]);
  s[64] = '\0';
  return String(s);
}

// Reboot on a short-lived task so the HTTP response can flush to the browser
// before the radio drops. Runs once then deletes itself.
static void otaScheduleReboot(void)
{
  static uint32_t delayMs;
  delayMs = g_cfg.rebootDelayMs;
  xTaskCreate(
      [](void *arg)
      {
        uint32_t d = *static_cast<uint32_t *>(arg);
        vTaskDelay(pdMS_TO_TICKS(d));
        ESP.restart();
        vTaskDelete(nullptr);
      },
      "ota_reboot", 2048, &delayMs, 1, nullptr);
}

// ---- Filesystem partition guards --------------------------------------------
static const esp_partition_t *fsPartition(void)
{
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
}

// LittleFS superblock: revision(4) tag(4) "littlefs"(8) tag(4) version(4)
// block_size(4) block_count(4)... The pair lives in blocks 0 and 1 and either
// may be the current copy.
static bool fsSuperblockLooksValid(const esp_partition_t *p)
{
  if (!p)
    return false;
  const uint32_t blockSize = 4096;
  for (int b = 0; b < 2; b++)
  {
    uint8_t head[32];
    if (esp_partition_read(p, (size_t)b * blockSize, head, sizeof(head)) != ESP_OK)
      continue;
    uint32_t bs, bc;
    memcpy(&bs, head + 24, 4);
    memcpy(&bc, head + 28, 4);
    otaLog("FS block %d: %s | magic %s block_size %lu block_count %lu (partition %lu bytes)", b, hex32(head).c_str(),
           memcmp(head + 8, "littlefs", 8) == 0 ? "ok" : "MISSING", (unsigned long)bs, (unsigned long)bc, (unsigned long)p->size);
    if (memcmp(head + 8, "littlefs", 8) != 0)
      continue;
    if (bs == blockSize && bc > 0 && (uint64_t)bc * bs <= p->size)
      return true;
  }
  return false;
}

bool otaFsMounted(void) { return g_fsMounted; }

bool otaFsMountSafe(void)
{
  if (g_fsMounted)
    return true;
  const esp_partition_t *p = fsPartition();
  if (!p)
  {
    otaLog("FS: no data/spiffs partition in the table");
    return false;
  }
  if (!fsSuperblockLooksValid(p))
  {
    otaLog("FS: no LittleFS superblock on '%s' - not mounting (upload littlefs.bin from the recovery page)", p->label);
    return false;
  }
  g_fsMounted = LittleFS.begin(false);
  otaLog("FS: LittleFS.begin -> %s; index.html %s, app.js %s, version.json %s", g_fsMounted ? "mounted" : "FAILED",
         g_fsMounted && LittleFS.exists("/index.html") ? "present" : "missing",
         g_fsMounted && LittleFS.exists("/app.js") ? "present" : "missing",
         g_fsMounted && LittleFS.exists("/version.json") ? "present" : "missing");
  return g_fsMounted;
}

static void fsUnmount(void)
{
  if (g_fsMounted)
    LittleFS.end();
  g_fsMounted = false;
}

bool otaFsUiAvailable(void)
{
  return g_fsMounted && LittleFS.exists("/index.html") && LittleFS.exists("/app.js");
}

void otaFsInvalidate(void)
{
  fsUnmount();
  const esp_partition_t *p = fsPartition();
  if (p)
    esp_partition_erase_range(p, 0, 2 * 4096);
  otaLog("FS: superblock pair erased - filesystem partition marked empty");
}

// "fs" from /version.json ("--" if unmounted or the file is missing).
String otaFsVersion(void)
{
  if (!g_fsMounted)
    return "--";
  File f = LittleFS.open("/version.json", "r");
  if (!f)
    return "--";
  String body = f.readString();
  f.close();
  int k = body.indexOf("\"fs\"");
  if (k < 0)
    return "--";
  int q1 = body.indexOf('"', k + 4);
  int q2 = q1 >= 0 ? body.indexOf('"', q1 + 1) : -1;
  if (q1 < 0 || q2 < 0)
    return "--";
  return body.substring(q1 + 1, q2);
}

// SHA-256 of the first `len` bytes of a partition as read back from flash
// (diagnostics only: what actually got written, not what was received).
#include <mbedtls/sha256.h>
static String partitionSha256(const esp_partition_t *p, size_t len)
{
  if (!p)
    return "";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  static uint8_t buf[1024];
  for (size_t off = 0; off < len; off += sizeof(buf))
  {
    size_t n = min(sizeof(buf), len - off);
    if (esp_partition_read(p, off, buf, n) != ESP_OK)
    {
      mbedtls_sha256_free(&ctx);
      return "";
    }
    mbedtls_sha256_update(&ctx, buf, n);
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  return hex32(digest);
}

// ---- Upload results ---------------------------------------------------------
struct OtaResult
{
  bool set = false;
  int code = 0;
  String msg;
};
static OtaResult fwResult, fsResult;
static bool fwRebootPending = false;

static void otaSetResult(OtaResult &r, int code, const String &msg)
{
  if (r.set)
    return;
  r.set = true;
  r.code = code;
  r.msg = msg;
  otaLog("result %d: %s", code, msg.c_str());
}

static void otaSendResult(AsyncWebServerRequest *request, OtaResult &r, const char *noFileMsg)
{
  if (!r.set)
  {
    r.code = 400;
    r.msg = noFileMsg;
  }
  String json = "{\"ok\":";
  json += (r.code == 200) ? "true" : "false";
  json += ",\"success\":";
  json += (r.code == 200) ? "true" : "false";
  json += ",\"message\":\"" + jsonEscape(r.msg) + "\"}";
  request->send(r.code, "application/json", json);
  r.set = false;
}

static size_t expectedSize(AsyncWebServerRequest *request)
{
  if (!request->hasParam("size"))
    return 0;
  long l = request->getParam("size")->value().toInt();
  return l > 0 ? (size_t)l : 0;
}

// ---- Firmware upload --------------------------------------------------------
static size_t fwBytes = 0;
static bool fwStarted = false;

static void fwAbort(void)
{
  if (fwStarted)
    Update.abort();
  fwStarted = false;
  g_inProgress = false;
}

static void handleFirmwareUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                                 uint8_t *data, size_t len, bool final)
{
  if (index == 0)
  {
    fwResult.set = false;
    fwBytes = 0;
    fwStarted = false;
  }
  if (fwResult.set)
    return; // outcome already decided - drain the rest of the body

  String reason;
  if (!otaIsSafe(reason))
  {
    fwAbort();
    otaSetResult(fwResult, 403, "OTA BLOCKED: " + reason);
    return;
  }

  if (index == 0)
  {
    if (Update.isRunning())
      Update.abort();
    otaLog("Starting firmware update: %s (%u bytes expected)", filename.c_str(), (unsigned)expectedSize(request));
    otaOnStart(false);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
    {
      otaSetResult(fwResult, 500, "OTA ERROR: failed to begin firmware update (no free OTA slot?)");
      return;
    }
    fwStarted = true;
    g_inProgress = true;
  }

  fwBytes += len;
  if (len && Update.write(data, len) != len)
  {
    fwAbort();
    otaSetResult(fwResult, 500, "OTA ERROR: firmware write failed - is that a firmware.bin?");
    return;
  }

  if (final)
  {
    size_t expect = expectedSize(request);
    if (expect && fwBytes != expect)
    {
      fwAbort();
      otaSetResult(fwResult, 400, "OTA ERROR: firmware upload was short (" + String(fwBytes) + " of " + String(expect) + " bytes) - retry");
      return;
    }
    // end() validates the image (magic, checksum, app descriptor) and sets the
    // boot partition; it refuses anything that is not a bootable app image.
    if (!Update.end(true))
    {
      fwStarted = false;
      g_inProgress = false;
      otaSetResult(fwResult, 400, "OTA ERROR: image rejected (" + String(Update.errorString()) + ") - is that a firmware.bin for this board?");
      otaOnEnd(false, false);
      return;
    }
    fwStarted = false;
    otaLog("Firmware written (%u bytes). Rebooting once the response is out.", (unsigned)fwBytes);
    otaOnEnd(false, true);
    otaSetResult(fwResult, 200, "Firmware update complete. Rebooting...");
    fwRebootPending = true; // g_inProgress stays set until the reboot
  }
}

static void finishFirmwareRequest(AsyncWebServerRequest *request)
{
  otaSendResult(request, fwResult, "No file received - pick firmware.bin first.");
  if (fwRebootPending)
    otaScheduleReboot();
}

// ---- Filesystem upload ------------------------------------------------------
static size_t fsBytes = 0;
static uint8_t fsHeadRx[32];
static size_t fsHeadRxLen = 0;

// `wipe` = false only when nothing has been written yet (the old image is
// intact, so put it back rather than erase it).
static void fsUpdateFail(int code, const String &msg, bool wipe = true)
{
  Update.abort();
  if (wipe)
    otaFsInvalidate();
  else
    otaFsMountSafe();
  g_inProgress = false;
  otaSetResult(fsResult, code, msg);
  otaOnEnd(true, false);
}

static void handleFilesystemUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                                   uint8_t *data, size_t len, bool final)
{
  if (index == 0)
  {
    fsResult.set = false;
    fsBytes = 0;
    fsHeadRxLen = 0;
  }
  if (fsResult.set)
    return; // outcome already decided - drain the rest of the body

  String reason;
  if (!otaIsSafe(reason))
  {
    if (index == 0)
      otaSetResult(fsResult, 403, "OTA BLOCKED: " + reason); // nothing written
    else
      fsUpdateFail(403, "OTA BLOCKED: " + reason + " (mid-upload) - re-upload the filesystem");
    return;
  }

  if (index == 0)
  {
    if (Update.isRunning())
      Update.abort(); // an earlier upload that never finished
    g_inProgress = true;
    otaLog("Starting filesystem update: %s (%u bytes expected)", filename.c_str(), (unsigned)expectedSize(request));
    otaOnStart(true);
    fsUnmount(); // nothing may read the partition while it is being rewritten
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS))
    {
      fsUpdateFail(500, "OTA ERROR: failed to begin filesystem update (no spiffs partition?)", false);
      return;
    }
    // Phone wandered off mid-upload: don't leave half an image behind.
    request->onDisconnect([]()
                          {
      if (g_inProgress && Update.isRunning()) fsUpdateFail(0, "client disconnected mid-upload"); });
  }

  if (fsHeadRxLen < sizeof(fsHeadRx) && len)
  {
    size_t n = min(len, sizeof(fsHeadRx) - fsHeadRxLen);
    memcpy(fsHeadRx + fsHeadRxLen, data, n);
    fsHeadRxLen += n;
  }
  fsBytes += len;
  if (len && Update.write(data, len) != len)
  {
    fsUpdateFail(500, "OTA ERROR: filesystem write failed (" + String(Update.errorString()) + ")");
    return;
  }

  if (final)
  {
    // A short body still arrives with final=true, and Update.end(true) would
    // happily accept it - so the browser tells us how much to expect.
    size_t expect = expectedSize(request);
    if (expect && fsBytes != expect)
    {
      fsUpdateFail(400, "OTA ERROR: filesystem upload was short (" + String(fsBytes) + " of " + String(expect) + " bytes) - re-upload");
      return;
    }
    if (!Update.end(true))
    {
      fsUpdateFail(500, "OTA ERROR: filesystem update failed (" + String(Update.errorString()) + ")");
      return;
    }
    // Mount what was written. If it doesn't come up as a filesystem holding
    // the web UI there's no point keeping it.
    if (!otaFsMountSafe() || !otaFsUiAvailable())
    {
      uint8_t flashHead[32] = {0};
      esp_partition_read(fsPartition(), 0, flashHead, sizeof(flashHead));
      String msg = "OTA ERROR: filesystem written but does not mount (";
      msg += g_fsMounted ? "mounted, web UI files missing" : "mount failed";
      msg += "). Received " + String(fsBytes) + " bytes. First 32 bytes received: " + hex32(fsHeadRx) +
             " | in flash: " + hex32(flashHead) + " | read-back sha256: " + partitionSha256(fsPartition(), fsBytes);
      fsUpdateFail(500, msg);
      return;
    }
    otaLog("Filesystem update complete: %u bytes, web UI v%s. No reboot.", (unsigned)fsBytes, otaFsVersion().c_str());
    g_inProgress = false;
    otaOnEnd(true, true);
    otaSetResult(fsResult, 200, "Filesystem update complete.");
  }
}

// ---- Public API -------------------------------------------------------------
ota_config_t otaDefaultConfig(void)
{
  ota_config_t c = {};
  c.fwVersion = nullptr;
  c.product = nullptr;
  c.githubRepo = nullptr;
  c.githubBranch = "main";
  c.releasesDir = "Releases";
  c.rebootDelayMs = 1500;
  c.verbose = false;
  return c;
}

void otaManagerInit(const ota_config_t *cfg)
{
  g_cfg = cfg ? *cfg : otaDefaultConfig();
  if (!strHasContent(g_cfg.githubBranch))
    g_cfg.githubBranch = "main";
  if (!strHasContent(g_cfg.releasesDir))
    g_cfg.releasesDir = "Releases";
  g_initialised = true;
}

void otaManagerAttach(AsyncWebServer &server)
{
  if (!g_initialised)
    otaManagerInit(nullptr);

  // Device/firmware info for the OTA tab (and the guided updater's config).
  // The filter on this first route is evaluated for EVERY request (the server
  // asks each handler `filter && canHandle` in registration order), which is
  // how otaWebClientActive() sees traffic to the project's own routes too.
  // Register otaManagerAttach() before your own routes for that to hold.
  server.on("/api/ota/info", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_partition_t *running = esp_ota_get_running_partition();
    char board[64];
    snprintf(board, sizeof(board), "%s (%d core%s, rev %d)", ESP.getChipModel(), chip.cores, chip.cores == 1 ? "" : "s", chip.revision);
    String reason;
    bool safe = otaIsSafe(reason);

    String json = "{";
    json += "\"version\":\"" + jsonEscape(otaManagerFwVersion()) + "\",";
    json += "\"fsVersion\":\"" + jsonEscape(otaFsVersion()) + "\",";
    json += "\"product\":\"" + jsonEscape(otaManagerProduct()) + "\",";
    json += "\"board\":\"" + jsonEscape(board) + "\",";
    json += "\"chipModel\":\"" + jsonEscape(ESP.getChipModel()) + "\",";
    json += "\"chipRevision\":" + String(chip.revision) + ",";
    json += "\"partition\":\"" + String(running ? running->label : "?") + "\",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"flashSize\":" + String(ESP.getFlashChipSize()) + ",";
    json += "\"repo\":\"" + jsonEscape(strHasContent(g_cfg.githubRepo) ? g_cfg.githubRepo : "") + "\",";
    json += "\"branch\":\"" + jsonEscape(g_cfg.githubBranch) + "\",";
    json += "\"releasesDir\":\"" + jsonEscape(g_cfg.releasesDir) + "\",";
    json += "\"allowed\":" + String(safe ? "true" : "false") + ",";
    json += "\"reason\":\"" + jsonEscape(reason) + "\"";
    json += "}";
    AsyncWebServerResponse *res = request->beginResponse(200, "application/json", json);
    res->addHeader("Cache-Control", "no-store");
    request->send(res); })
      .setFilter([](AsyncWebServerRequest *request)
                 { (void)request; otaNoteWebActivity(); return true; });

  // Safety gate status (polled by the OTA tab).
  server.on("/api/ota/check", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String reason;
    bool safe = otaIsSafe(reason);
    String json = "{\"allowed\":" + String(safe ? "true" : "false") + ",\"reason\":\"" + jsonEscape(reason) + "\",\"busy\":" + String(g_inProgress ? "true" : "false") + "}";
    request->send(200, "application/json", json); });

  // After a filesystem upload: is the web UI really there, and which version?
  server.on("/api/ota/fsinfo", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (g_inProgress) { request->send(409, "application/json", "{\"ok\":false,\"error\":\"update in progress\"}"); return; }
    bool mounted = otaFsMounted() || otaFsMountSafe();
    bool ui = mounted && otaFsUiAvailable();
    String json = "{\"ok\":" + String(ui ? "true" : "false") + ",\"mounted\":" + String(mounted ? "true" : "false") +
                  ",\"fsVersion\":\"" + jsonEscape(otaFsVersion()) + "\",\"fwVersion\":\"" + jsonEscape(otaManagerFwVersion()) + "\"}";
    request->send(200, "application/json", json); });

  // What is physically in the filesystem partition (?len=N hashes the first N
  // bytes; default the whole partition). Compare with `sha256sum littlefs.bin`.
  server.on("/api/ota/fsdiag", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    const esp_partition_t *p = fsPartition();
    if (!p) { request->send(500, "application/json", "{\"error\":\"no spiffs partition\"}"); return; }
    size_t len = p->size;
    if (request->hasParam("len")) { long l = request->getParam("len")->value().toInt(); if (l > 0 && (size_t)l <= p->size) len = (size_t)l; }
    uint8_t head[32] = {0};
    esp_partition_read(p, 0, head, sizeof(head));
    uint32_t bs = 0, bc = 0;
    memcpy(&bs, head + 24, 4);
    memcpy(&bc, head + 28, 4);
    String json = "{";
    json += "\"partition\":\"" + String(p->label) + "\",\"address\":" + String(p->address) + ",\"size\":" + String(p->size) + ",";
    json += "\"head\":\"" + hex32(head) + "\",\"magic\":" + String(memcmp(head + 8, "littlefs", 8) == 0 ? "true" : "false") + ",";
    json += "\"blockSize\":" + String(bs) + ",\"blockCount\":" + String(bc) + ",";
    json += "\"mounted\":" + String(otaFsMounted() ? "true" : "false") + ",\"ui\":" + String(otaFsUiAvailable() ? "true" : "false") + ",";
    json += "\"fsVersion\":\"" + jsonEscape(otaFsVersion()) + "\",";
    json += "\"hashLen\":" + String(len) + ",\"sha256\":\"" + partitionSha256(p, len) + "\"}";
    request->send(200, "application/json", json); });

  // NOTE: /api/ota/fs must be registered BEFORE /api/ota - the handler for
  // "<uri>" also matches "<uri>/..." and would swallow filesystem uploads.
  server.on(
      "/api/ota/fs", HTTP_POST,
      [](AsyncWebServerRequest *request) { otaSendResult(request, fsResult, "No file received - pick littlefs.bin first."); },
      [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
      { handleFilesystemUpload(request, filename, index, data, len, final); });

  server.on(
      "/api/ota", HTTP_POST, finishFirmwareRequest,
      [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
      { handleFirmwareUpload(request, filename, index, data, len, final); });

  otaLog("routes attached (/api/ota, /api/ota/fs, /api/ota/info, /api/ota/check, /api/ota/fsinfo, /api/ota/fsdiag)");
}

bool otaInProgress(void) { return g_inProgress; }

const char *otaManagerFwVersion(void)
{
  return strHasContent(g_cfg.fwVersion) ? g_cfg.fwVersion : "";
}

const char *otaManagerProduct(void)
{
  return strHasContent(g_cfg.product) ? g_cfg.product : "ESP32";
}
