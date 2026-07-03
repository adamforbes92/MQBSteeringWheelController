#include "API.h"
#include "power_manager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>

#include "CAN.h"
#include "globals.h"

void clearLearnState() {
  learnActive = false;
  learnTarget = LEARN_NONE;
  learnRowIndex = 0;
  learnStartTimestamp = 0;
}

void expireLearnState() {
  if (learnActive && (millis() - learnStartTimestamp) > 5000UL) {
    clearLearnState();
  }
}

static String frameToHex(const uint8_t* data, uint8_t len) {
  String out;
  for (uint8_t i = 0; i < len; i++) {
    if (i > 0) {
      out += ' ';
    }
    if (data[i] < 16) {
      out += '0';
    }
    out += String(data[i], HEX);
  }
  out.toUpperCase();
  return out;
}

void setupWiFi() {
  WiFi.hostname(wifiHostName);

#if detailedDebugWiFi
  DEBUG("Beginning WiFi...");
  DEBUG("Creating Access Point...");
#endif

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(wifiHostName);
  WiFi.setSleep(false);
}

void setupApiServer() {
  LittleFS.begin(true);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    expireLearnState();

    JsonDocument doc;

    doc["FW_VERSION"] = FW_VERSION;

    uint8_t buttonIncoming = lastLinInLen > 1 ? lastLinInFrame[1] : 0;
    uint8_t buttonOutgoing = lastLinOutLen > 1 ? lastLinOutFrame[1] : 0;
    uint8_t backlightRaw = steeringWheelLightData[0];
    if (backlightRaw > upperLightsLIN) backlightRaw = upperLightsLIN;
    uint8_t backlightPercent = upperLightsLIN == 0 ? 0 : (uint8_t)((backlightRaw * 100U) / upperLightsLIN);

    doc["incomingLinId"] = lastLinInId;
    doc["incomingLinData"] = frameToHex(lastLinInFrame, lastLinInLen);
    doc["outgoingLinId"] = lastLinOutId;
    doc["outgoingLinData"] = frameToHex(lastLinOutFrame, lastLinOutLen);
    doc["outgoingCanId"] = lastCanOutId;
    doc["outgoingCanData"] = frameToHex(lastCanOutFrame, lastCanOutLen);
    doc["latestButtonId"] = latestLinButtonId;
    doc["buttonIncoming"] = buttonIncoming;
    doc["buttonOutgoing"] = buttonOutgoing;

    // Resolve display name for the currently active incoming button
    const char* pressedButtonName = "";
    if (buttonIncoming != 0) {
      for (size_t j = 0; j < buttonMappingCount; j++) {
        if (buttonMappings[j].oldButtonId == buttonIncoming) {
          pressedButtonName = buttonMappings[j].name;
          break;
        }
      }
    }
    doc["pressedButtonName"] = pressedButtonName;

    // CAN frame bytes as an array so the frontend can render each bit
    JsonArray canBytesArr = doc["canBytes"].to<JsonArray>();
    for (uint8_t i = 0; i < 8; i++) {
      canBytesArr.add(lastCanOutFrame[i]);
    }

    doc["canBroadcastEnabled"] = canBroadcastEnabled;
    doc["canBroadcastId"] = canBroadcastId;
    doc["hasAuxLight"] = useAuxLightSource;
    doc["forceBacklight"] = forceBacklight;
    // Priority matches updateBacklightState(): aux → force → lin.
    doc["backlightSource"] = useAuxLightSource ? "AUX"
                           : forceBacklight    ? "FORCED"
                           :                    "LIN";
    doc["backlightOn"] = backlightRaw > 0;
    doc["backlightState"] = (backlightRaw == 0)              ? "OFF"
                          : (backlightRaw >= upperLightsLIN) ? "ON"
                          :                                   "DIM";
    doc["backlightPercent"] = backlightPercent;
    doc["auxDutyRaw"]  = dutyCycle;
    doc["auxOnTimeUs"]  = on_Time;
    doc["auxPeriodUs"]  = total_Time;
    doc["auxFreqHz"]   = (total_Time > 0) ? (1000000UL / total_Time) : 0UL;
    doc["auxDimDuty"]    = auxDimDutyPct10;
    doc["auxBrightDuty"] = auxBrightDutyPct10;
    doc["learnActive"] = learnActive;
    doc["learnTarget"] = learnTarget;
    doc["learnRowIndex"] = learnRowIndex;

    uint32_t elapsed = learnActive ? (millis() - learnStartTimestamp) : 0;
    uint32_t remaining = 0;
    if (elapsed < 5000UL) {
      remaining = 5000UL - elapsed;
    }
    doc["learnMsRemaining"] = remaining;
    doc["diagPnpActive"]        = diagPnpActive;
    doc["diagResistiveEnabled"] = diagResistiveEnabled;
    doc["diagResistiveOhm"]     = diagResistiveOhm;
    doc["testResistanceEnabled"] = testResistanceEnabled;
    doc["testResistanceOhm"]     = testResistanceOhm;

    // ---- Bus health ----------------------------------------------------
    // A bus is "healthy" if it had an error-free transaction within the
    // staleness window. LIN 1 (steering wheel) is polled continuously; LIN 2
    // (chassis) is only polled when reading gateway lights or sending output,
    // so report whether it is currently being exercised at all.
    const uint32_t nowMs = millis();
    const uint32_t kLinStaleMs = 1000UL;
    const bool lin1Healthy = swLinLastOkMs != 0 && (nowMs - swLinLastOkMs) < kLinStaleMs;
    const bool lin2Active = (!useAuxLightSource && !forceBacklight) || linOutputEnabled;
    const bool lin2Healthy = chassisLinLastOkMs != 0 && (nowMs - chassisLinLastOkMs) < kLinStaleMs;
    doc["canHealthy"]  = (hasCAN != 0) && canHealthy();
    doc["canEnabled"]  = (hasCAN != 0);
    doc["lin1Healthy"] = lin1Healthy;
    doc["lin2Healthy"] = lin2Healthy;
    doc["lin2Active"]  = lin2Active;
    doc["resistiveOhmNow"] = (hasResistiveStereo != 0) ? radioResistor.getOhm() : 0;
    doc["hasResistiveStereo"] = (hasResistiveStereo != 0);

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  server.on("/api/setup", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["canBroadcastEnabled"] = canBroadcastEnabled;
    doc["canBroadcastId"] = canBroadcastId;
    doc["paddlesEnabled"] = paddlesEnabled;
    doc["hasAuxLight"] = useAuxLightSource;
    doc["auxDimDuty"]    = auxDimDutyPct10;
    doc["auxBrightDuty"] = auxBrightDutyPct10;
    doc["forceBacklight"] = forceBacklight;
    doc["forceBacklightPercent"] = forceBacklightPercent;
    doc["canHoldMs"] = canHoldMs;
    doc["linOutputEnabled"] = linOutputEnabled;
    doc["linOutputId"] = linOutputId;

    JsonArray mappings = doc["mappings"].to<JsonArray>();
    for (size_t i = 0; i < buttonMappingCount; i++) {
      JsonObject row = mappings.add<JsonObject>();
      row["name"] = buttonMappings[i].name;
      row["oldButtonId"] = buttonMappings[i].oldButtonId;
      row["newLinButtonId"] = buttonMappings[i].newLinButtonId;
      row["canByteIndex"] = buttonMappings[i].canByteIndex;  // 0-7 or 255 (no CAN)
      row["canBitIndex"] = buttonMappings[i].canBitIndex;    // 0-7
      row["resistiveOhm"] = buttonMappings[i].resistiveOhm;
      row["flags"] = buttonMappings[i].flags;
    }

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  server.on(
      "/api/setup/save", HTTP_POST,
      // onRequest — fires after ALL body chunks have been received.
      [](AsyncWebServerRequest* request) {
        String* bodyStr = request->_tempObject
                          ? static_cast<String*>(request->_tempObject)
                          : nullptr;
        if (!bodyStr) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
          return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, *bodyStr);
        delete bodyStr;
        request->_tempObject = nullptr;

        if (err) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }

        canBroadcastEnabled = doc["canBroadcastEnabled"] | (bool)canBroadcastEnabled;
        canBroadcastId      = doc["canBroadcastId"]      | (uint16_t)canBroadcastId;
        paddlesEnabled      = doc["paddlesEnabled"]      | (bool)paddlesEnabled;
        useAuxLightSource   = doc["hasAuxLight"]         | (bool)useAuxLightSource;
        auxDimDutyPct10     = doc["auxDimDuty"]          | (uint16_t)auxDimDutyPct10;
        auxBrightDutyPct10  = doc["auxBrightDuty"]       | (uint16_t)auxBrightDutyPct10;
        forceBacklight      = doc["forceBacklight"]      | (bool)forceBacklight;

        {
          int pct = doc["forceBacklightPercent"] | (int)forceBacklightPercent;
          forceBacklightPercent = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
        }

        {
          int holdMs = doc["canHoldMs"] | (int)canHoldMs;
          canHoldMs = (uint16_t)(holdMs < 50 ? 50 : (holdMs > 5000 ? 5000 : holdMs));
        }
        linOutputEnabled = doc["linOutputEnabled"] | (bool)linOutputEnabled;
        linOutputId      = doc["linOutputId"]      | (uint8_t)linOutputId;

        if (auxBrightDutyPct10 <= auxDimDutyPct10) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"bright duty must be greater than dim duty\"}");
          return;
        }

        JsonArray mappings = doc["mappings"].as<JsonArray>();
        if (!mappings.isNull()) {
          size_t newCount = mappings.size();
          if (newCount > kMaxButtonMappings) {
            newCount = kMaxButtonMappings;
          }

          for (size_t i = 0; i < newCount; i++) {
            JsonObject row = mappings[i].as<JsonObject>();
            const char* name = row["name"] | "Unnamed";
            strlcpy(buttonMappings[i].name, name, sizeof(buttonMappings[i].name));
            buttonMappings[i].oldButtonId    = row["oldButtonId"]    | (uint8_t)0;
            buttonMappings[i].newLinButtonId = row["newLinButtonId"] | (uint8_t)0;
            {
              int byteRaw = row["canByteIndex"] | 255;
              int bitRaw  = row["canBitIndex"]  | 0;
              buttonMappings[i].canByteIndex = (byteRaw >= 0 && byteRaw < 8) ? (uint8_t)byteRaw : 0xFF;
              buttonMappings[i].canBitIndex  = (uint8_t)(bitRaw >= 0 && bitRaw < 8 ? bitRaw : 0);
            }
            buttonMappings[i].resistiveOhm = row["resistiveOhm"] | (uint16_t)0;
            buttonMappings[i].flags        = row["flags"]        | (uint8_t)0;
          }
          buttonMappingCount = newCount;
        }

        request->send(200, "application/json", "{\"ok\":true}");
      },
      nullptr,
      // onBody — accumulate every chunk into a heap String via _tempObject.
      // The full body may arrive across multiple TCP segments (e.g. when the
      // mappings table makes the JSON > ~1460 bytes), so we cannot process it
      // until onRequest signals that all chunks have been received.
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index == 0) {
          request->_tempObject = new String();
          static_cast<String*>(request->_tempObject)->reserve(total);
        }
        if (request->_tempObject) {
          static_cast<String*>(request->_tempObject)->concat(
              reinterpret_cast<const char*>(data), len);
        }
      });

  server.on(
      "/api/backlight/learn", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        (void)request;
      },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"chunked request not supported\"}");
          return;
        }

        JsonDocument body;
        DeserializationError err = deserializeJson(body, data, len);
        if (err) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }

        const char* target = body["target"] | "";
        // Capture the current measured frequency in Hz.
        // Capture current duty cycle in tenths of percent (0–1000).
        const uint16_t captured = (total_Time > 0)
            ? (uint16_t)constrain(on_Time * 1000UL / total_Time, 0UL, 1000UL)
            : 0;
        if (total_Time == 0) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"no signal detected\"}" );
          return;
        }

        if (strcmp(target, "dim") == 0) {
          auxDimDutyPct10 = captured;
        } else if (strcmp(target, "bright") == 0) {
          auxBrightDutyPct10 = captured;
        } else {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"target must be dim or bright\"}");
          return;
        }

        if (auxBrightDutyPct10 <= auxDimDutyPct10) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"captured range invalid; bright duty must be greater than dim duty\"}");
          return;
        }

        request->send(200, "application/json", "{\"ok\":true}");
      });

  server.on(
      "/api/learn/start", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        (void)request;
      },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"chunked request not supported\"}");
          return;
        }

        JsonDocument body;
        DeserializationError err = deserializeJson(body, data, len);
        if (err) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }

        uint8_t rowIndex = body["rowIndex"] | (uint8_t)0;
        uint8_t target   = body["target"]   | (uint8_t)0;

        if (rowIndex >= buttonMappingCount || target < LEARN_OLD_LIN || target > LEARN_NEW_CAN) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid learn request\"}");
          return;
        }

        learnActive = true;
        learnTarget = target;
        learnRowIndex = rowIndex;
        learnStartTimestamp = millis();

        request->send(200, "application/json", "{\"ok\":true}");
      });

  server.on("/api/learn/cancel", HTTP_POST, [](AsyncWebServerRequest* request) {
    clearLearnState();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // Diagnostic: toggle PNP transistor output (Pin 21).
  server.on(
      "/api/diag/pnp", HTTP_POST,
      [](AsyncWebServerRequest* request) { (void)request; },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"chunked not supported\"}");
          return;
        }
        JsonDocument body;
        if (deserializeJson(body, data, len)) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }
        diagPnpActive = body["active"] | (bool)diagPnpActive;
        request->send(200, "application/json", "{\"ok\":true}");
      });

  // Diagnostic: hold resistive output at a fixed position for testing.
  server.on(
      "/api/diag/resistive", HTTP_POST,
      [](AsyncWebServerRequest* request) { (void)request; },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"chunked not supported\"}");
          return;
        }
        JsonDocument body;
        if (deserializeJson(body, data, len)) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }
        diagResistiveEnabled = body["enabled"] | (bool)diagResistiveEnabled;
        {
          int ohm = body["ohm"] | (int)diagResistiveOhm;
          diagResistiveOhm = (uint16_t)(ohm < 0 ? 0 : (ohm > 10000 ? 10000 : ohm));
        }
        request->send(200, "application/json", "{\"ok\":true}");
      });

  // Diagnostic: test resistance — pulse DOWN by 200 ohm steps then return to idle high.
  server.on(
      "/api/diag/testresistance", HTTP_POST,
      [](AsyncWebServerRequest* request) { (void)request; },
      nullptr,
      [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index != 0 || len != total) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"chunked not supported\"}");
          return;
        }
        JsonDocument body;
        if (deserializeJson(body, data, len)) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
          return;
        }
        testResistanceEnabled = body["enabled"] | (bool)testResistanceEnabled;
        int dir = body["dir"] | 0;  // +1 = up, -1 = down, 0 = no step
        if (dir != 0) {
          int v = (int)testResistanceOhm + (dir > 0 ? 200 : -200);
          if (v < 0) v = 0;
          if (v > 10000) v = 10000;
          testResistanceOhm = (uint16_t)v;
          if (testResistanceEnabled) {
            testResistancePulse = true;  // task pulses the new value for 0.5 s
          }
        }
        JsonDocument res;
        res["ok"] = true;
        res["enabled"] = testResistanceEnabled;
        res["ohm"] = testResistanceOhm;
        String payload;
        serializeJson(res, payload);
        request->send(200, "application/json", payload);
      });

  server.on(
      "/api/ota", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        const bool ok = !Update.hasError();
        if (ok) {
          request->send(200, "application/json", "{\"ok\":true,\"message\":\"Update complete. Rebooting...\"}");
          delay(100);
          ESP.restart();
        } else {
          request->send(500, "application/json", "{\"ok\":false,\"message\":\"Update failed\"}");
        }
      },
      [](AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        (void)request;
        if (index == 0) {
          DEBUG("OTA upload started: %s", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
          }
        }

        if (len > 0 && Update.write(data, len) != len) {
          Update.printError(Serial);
        }

        if (final) {
          if (!Update.end(true)) {
            Update.printError(Serial);
          }
          DEBUG("OTA upload completed, %u bytes", static_cast<unsigned>(index + len));
        }
      });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();
}

// ----------------------------------------------------------------------------
// power_manager integration (universal reduced-power module)
// ----------------------------------------------------------------------------
// These override the weak hooks in power_manager.cpp. The device stays fully
// awake while ANY client is associated to the AP. Once the last client leaves,
// the manager's idle timer runs, then turns the radio off and drops the CPU
// clock. A power-cycle (ignition off/on) brings WiFi back automatically.

bool powerIsBusy()
{
  return WiFi.softAPgetStationNum() > 0;
}

// ACTIVE -> REDUCED: close the web server cleanly before the radio drops.
void powerOnEnterReduced()
{
  server.end();
}

// REDUCED -> ACTIVE: bring the AP and web server back. Routes are already
// registered (no need to re-run setupApiServer()), so we only restart the
// radio and the listener.
void powerOnExitReduced()
{
  setupWiFi();
  server.begin();
}
