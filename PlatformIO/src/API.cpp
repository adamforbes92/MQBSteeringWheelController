#include "API.h"
#include "power_manager.h"
#include "wifi_manager.h"
#include "ota_manager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "CAN.h"
#include "globals.h"
#include "savvycan.h"

void clearLearnState() {
  learnActive = false;
  learnBaselineReady = false;
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
  // WiFi bring-up is handled by the universal wifi_manager (SoftAP + mDNS).
  wifiManagerStartAP();
}

void setupApiServer() {
  // Shared OTA + Home WiFi routes first: ota_manager's first route carries the
  // filter that notes web activity for every request (otaWebClientActive()),
  // and /api/wifi/sta must precede any /api/wifi... route of our own.
  otaManagerAttach(server);
  wifiManagerAttachSta(server);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    expireLearnState();

    JsonDocument doc;

    doc["FW_VERSION"] = FW_VERSION;

    // Match each mapping at its own source byte (paddles = 6, horn = 7) so
    // any learned button surfaces on the dashboard, not just byte-1 presses.
    const uint8_t defaultByte = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
    uint8_t buttonIncoming = 0;
    const char* pressedButtonName = "";

    // Prefer the row the LIN task actually matched, held briefly so a scroll
    // detent (which is over in a single poll) is still readable here. Matching
    // by code alone would pick whichever of a scroll wheel's two direction rows
    // came first in the table, since both carry the same code.
    const int8_t matchedRow = latestMatchedRow;
    const bool matchFresh =
        latestLinButtonTimestamp != 0 &&
        ((uint32_t)millis() - latestLinButtonTimestamp) < (uint32_t)canHoldMs;
    if (matchFresh && matchedRow >= 0 && (size_t)matchedRow < buttonMappingCount) {
      buttonIncoming = buttonMappings[matchedRow].oldButtonId;
      pressedButtonName = buttonMappings[matchedRow].name;
    } else {
      for (size_t j = 0; j < buttonMappingCount; j++) {
        const ButtonMapping& m = buttonMappings[j];
        if (m.oldButtonId == 0) continue;
        uint8_t sb = (m.sourceByte >= 1 && m.sourceByte < 8) ? m.sourceByte : defaultByte;
        if (lastLinInLen > sb && lastLinInFrame[sb] == m.oldButtonId) {
          buttonIncoming = m.oldButtonId;
          pressedButtonName = m.name;
          break;
        }
      }
    }
    if (buttonIncoming == 0 && lastLinInLen > defaultByte) {
      buttonIncoming = lastLinInFrame[defaultByte];  // unmapped press: raw code
    }

    // Fall back to the last matched button while the CAN hold window is still
    // open, so the dashboard appears/clears in lockstep with the CAN output on
    // quick taps instead of flickering the instant the frame changes.
    if (buttonIncoming == 0 && latestLinButtonId != 0 &&
        (uint32_t)(millis() - latestLinButtonTimestamp) < (uint32_t)canHoldMs) {
      buttonIncoming = latestLinButtonId;
      for (size_t j = 0; j < buttonMappingCount; j++) {
        if (buttonMappings[j].oldButtonId != 0 &&
            buttonMappings[j].oldButtonId == latestLinButtonId) {
          pressedButtonName = buttonMappings[j].name;
          break;
        }
      }
    }

    uint8_t buttonOutgoing = lastLinOutLen > 1 ? lastLinOutFrame[1] : 0;
    uint8_t backlightRaw = steeringWheelLightData[0];
    if (backlightRaw > upperLightsLIN) backlightRaw = upperLightsLIN;
    uint8_t backlightPercent = upperLightsLIN == 0 ? 0 : (uint8_t)((backlightRaw * 100U) / upperLightsLIN);

    doc["incomingLinId"] = lastLinInId;
    doc["incomingLinData"] = frameToHex(lastLinInFrame, lastLinInLen);
    doc["incomingBcmId"] = linLightInId;
    doc["incomingBcmData"] = frameToHex(gatewayLightData, sizeof(gatewayLightData));
    doc["outgoingLinId"] = lastLinOutId;
    doc["outgoingLinData"] = frameToHex(lastLinOutFrame, lastLinOutLen);
    doc["outgoingCanId"] = lastCanOutId;
    doc["outgoingCanData"] = frameToHex(lastCanOutFrame, lastCanOutLen);
    doc["incomingAccData"] = frameToHex(lastAccInFrame, lastAccInLen);
    doc["incomingTempData"] = frameToHex(lastTempInFrame, lastTempInLen);
    doc["latestButtonId"] = latestLinButtonId;
    doc["buttonIncoming"] = buttonIncoming;
    doc["buttonOutgoing"] = buttonOutgoing;

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
    doc["mappingsRevision"] = mappingsRevision;
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
    doc["passthruEnabled"] = passthruEnabled;

    // ---- Bus health ----------------------------------------------------
    // A bus is "healthy" if it had an error-free transaction within the
    // staleness window. LIN 1 (steering wheel) and LIN 2's light frame are
    // both polled every cycle regardless of mode; LIN 2's button-output send
    // only happens when a button is actually pressed.
    //
    // LIN 2 is split into two independently-tracked channels because they can
    // fail for different reasons and a single combined timestamp hides that:
    // "lin2Healthy" is true if EITHER channel is active, so a BCM that never
    // answers the light poll can still show "Healthy" purely from button
    // presses driving output. "lin2LightHealthy" isolates the BCM light-frame
    // read so that failure mode is visible on its own.
    const uint32_t nowMs = millis();
    const uint32_t kLinStaleMs = 1000UL;
    const bool lin1Healthy = swLinLastOkMs != 0 && (nowMs - swLinLastOkMs) < kLinStaleMs;
    const bool lin2Active = (!useAuxLightSource && !forceBacklight) || linOutputEnabled;
    const bool lin2Healthy = chassisLinLastOkMs != 0 && (nowMs - chassisLinLastOkMs) < kLinStaleMs;
    // The BCM light frame is now polled every cycle regardless of the active
    // backlight source, so this is always true — kept as a field in case that
    // ever changes again, rather than hardcoding "Healthy"/"No Data" in the UI.
    const bool lin2LightActive = true;
    const bool lin2LightHealthy = chassisLightLastOkMs != 0 && (nowMs - chassisLightLastOkMs) < kLinStaleMs;
    doc["canHealthy"]  = canHealthy();
    doc["canBusState"] = canBusStateName();
    doc["canTxAccepted"] = canTxAccepted;
    doc["canTxRefused"]  = canTxRefused;
    doc["canBitrateKbit"] = canBitrateKbit;
    doc["canEnabled"]  = canBroadcastEnabled || paddlesEnabled;
    doc["lin1Healthy"] = lin1Healthy;
    doc["lin2Healthy"] = lin2Healthy;
    doc["lin2Active"]  = lin2Active;
    doc["lin2LightHealthy"] = lin2LightHealthy;
    doc["lin2LightActive"]  = lin2LightActive;

    // BCM light status for the dashboard — independent of which source is
    // actively driving the wheel's backlight (AUX/FORCED/LIN), so it's always
    // visible whether real BCM light data is present on the chassis bus.
    doc["bcmLightAvailable"] = lin2LightHealthy;
    if (lin2LightHealthy) {
      uint8_t bcmRaw = gatewayLightData[0];
      if (bcmRaw > upperLightsLIN) bcmRaw = upperLightsLIN;
      doc["bcmLightPercent"] = upperLightsLIN == 0 ? 0 : (uint8_t)((bcmRaw * 100U) / upperLightsLIN);
    }
    doc["resistiveOhmNow"] = radioResistor.getOhm();
    doc["digipotMaxOhm"] = digipotMaxOhm;

    // ---- OpenHaldex live state -----------------------------------------
    // Current mode last seen on the OpenHaldex broadcast (0x6B0), and whether
    // a commanded change is still awaiting confirmation.
    {
      const uint8_t ohMode = openHaldexCurrentMode;
      const bool ohFresh = openHaldexLastRxMs != 0 && (nowMs - openHaldexLastRxMs) < 2000UL;
      static const char* const kOhNames[OPENHALDEX_MODE_COUNT] = {
          "Stock", "FWD", "50:50", "60:40", "75:25", "Expert"};
      doc["openHaldexPresent"] = ohFresh;
      doc["openHaldexMode"] = ohMode;
      doc["openHaldexModeStr"] =
          (ohFresh && ohMode < OPENHALDEX_MODE_COUNT) ? kOhNames[ohMode] : "--";
      doc["openHaldexPending"] = (openHaldexTargetMode != OPENHALDEX_MODE_UNKNOWN);
    }

    doc["charismaMode"] = charismaMode;
    doc["charismaProgram"] = charismaProgram;
    doc["rotaryDelta"] = wheelRotaryDelta;

    // Rows whose latch is currently engaged, by index into the mapping table.
    JsonArray latched = doc["latched"].to<JsonArray>();
    for (size_t i = 0; i < buttonMappingCount; i++) {
      if (buttonLatched[i]) {
        latched.add((int)i);
      }
    }

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  server.on("/api/setup", HTTP_GET, [](AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["canBroadcastEnabled"] = canBroadcastEnabled;
    doc["canBroadcastId"] = canBroadcastId;
    doc["canBitrateKbit"] = canBitrateKbit;
    doc["paddlesEnabled"] = paddlesEnabled;
    doc["hasAuxLight"] = useAuxLightSource;
    doc["auxDimDuty"]    = auxDimDutyPct10;
    doc["auxBrightDuty"] = auxBrightDutyPct10;
    doc["forceBacklight"] = forceBacklight;
    doc["forceBacklightPercent"] = forceBacklightPercent;
    doc["canHoldMs"] = canHoldMs;
    doc["linOutputEnabled"] = linOutputEnabled;
    doc["linOutputId"] = linOutputId;
    doc["digipot20kEnabled"] = digipot20kEnabled;
    doc["linLegacyPins"] = linLegacyPins;
    doc["linButtonInId"] = linButtonInId;
    doc["linLightInId"] = linLightInId;
    doc["linTempInId"] = linTempInId;
    doc["linAccInId"] = linAccInId;
    doc["linButtonByteIndex"] = linButtonByteIndex;
    doc["linRotaryByteIndex"] = linRotaryByteIndex;
    doc["wheelProtocol"] = wheelProtocol;
    doc["mqbActByte1"] = mqbActByte1;
    doc["mqbActByte2"] = mqbActByte2;
    doc["mqbActByte3"] = mqbActByte3;
    doc["chassisProtocol"] = chassisProtocol;
    doc["charismaMode"] = charismaMode;
    doc["charismaButtonBit"] = charismaButtonBit;
    doc["charismaProgramCount"] = charismaProgramCount;
    doc["charismaParticipants"] = charismaParticipants;

    // Participant list is defined in firmware (defs.h) so the UI's checklist
    // follows it without needing to duplicate the bit offsets.
    JsonArray parts = doc["charismaParticipantNames"].to<JsonArray>();
    for (uint8_t i = 0; i < kCharismaParticipantCount; i++) {
      parts.add(kCharismaParticipants[i].name);
    }

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
      row["openHaldexMode"] = buttonMappings[i].openHaldexMode;  // 0-5 or 255 (push-to-next)
      row["sourceByte"] = buttonMappings[i].sourceByte;  // 0 = default byte; set by Learn (e.g. 6/7)
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

        // The client posts its whole copy of the mapping table. If a Learn has
        // landed since that copy was taken, applying it would silently
        // overwrite the learned code — refuse before touching anything, and
        // let the client pull the change in first. Clients that don't send a
        // revision (older UI) are not checked.
        if (!doc["mappingsRevision"].isNull() &&
            doc["mappingsRevision"].as<uint32_t>() != mappingsRevision) {
          request->send(409, "application/json",
                        "{\"ok\":false,\"error\":\"mappings changed on device since last load\"}");
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
        digipot20kEnabled = doc["digipot20kEnabled"] | (bool)digipot20kEnabled;
        const bool oldLegacyPins = linLegacyPins;
        const uint16_t oldBitrate = canBitrateKbit;
        {
          int kb = doc["canBitrateKbit"] | (int)canBitrateKbit;
          canBitrateKbit = (uint16_t)((kb == 100 || kb == 125 || kb == 250 || kb == 500) ? kb : 500);
        }
        linLegacyPins    = doc["linLegacyPins"]    | (bool)linLegacyPins;
        linButtonInId    = doc["linButtonInId"]    | (uint8_t)linButtonInId;
        linLightInId     = doc["linLightInId"]     | (uint8_t)linLightInId;
        linTempInId      = doc["linTempInId"]      | (uint8_t)linTempInId;
        linAccInId       = doc["linAccInId"]       | (uint8_t)linAccInId;
        {
          int bi = doc["linButtonByteIndex"] | (int)linButtonByteIndex;
          linButtonByteIndex = (uint8_t)((bi >= 0 && bi < 8) ? bi : 1);
          int rb = doc["linRotaryByteIndex"] | (int)linRotaryByteIndex;
          linRotaryByteIndex = (uint8_t)((rb >= 0 && rb <= 8) ? rb : 8);
        }
        {
          int wp = doc["wheelProtocol"] | (int)wheelProtocol;
          wheelProtocol = (uint8_t)(wp == WHEEL_PROTOCOL_MQB ? WHEEL_PROTOCOL_MQB : WHEEL_PROTOCOL_PQ);
        }
        mqbActByte1 = doc["mqbActByte1"] | (uint8_t)mqbActByte1;
        mqbActByte2 = doc["mqbActByte2"] | (uint8_t)mqbActByte2;
        mqbActByte3 = doc["mqbActByte3"] | (uint8_t)mqbActByte3;
        {
          int cp = doc["chassisProtocol"] | (int)chassisProtocol;
          chassisProtocol = (uint8_t)(cp == CHASSIS_PROTOCOL_PQ ? CHASSIS_PROTOCOL_PQ : CHASSIS_PROTOCOL_MQB);
        }
        {
          int cm = doc["charismaMode"] | (int)charismaMode;
          charismaMode = (uint8_t)(cm >= CHARISMA_MODE_OFF && cm <= CHARISMA_MODE_MAX
                                       ? cm
                                       : CHARISMA_MODE_OFF);
          int cb = doc["charismaButtonBit"] | (int)charismaButtonBit;
          charismaButtonBit = (uint8_t)(cb == CHARISMA_BTN_ECO || cb == CHARISMA_BTN_OFFROAD
                                            ? cb
                                            : CHARISMA_BTN_TASTE2);
          int cn = doc["charismaProgramCount"] | (int)charismaProgramCount;
          charismaProgramCount = (uint8_t)(cn >= 2 && cn <= 15 ? cn : 4);
          charismaParticipants = doc["charismaParticipants"] | (uint8_t)charismaParticipants;
          if (charismaProgram > charismaProgramCount) {
            charismaProgram = 1;
          }
        }

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
            buttonMappings[i].openHaldexMode = row["openHaldexMode"] | (uint8_t)0;
            {
              int sb = row["sourceByte"] | 0;
              buttonMappings[i].sourceByte = (uint8_t)((sb >= 0 && sb < 8) ? sb : 0);
            }
          }
          buttonMappingCount = newCount;
        }

        savePreferences();
        // The RX/TX pin mapping is only applied during LIN init at boot, so a
        // change to it requires a reboot to take effect.
        if (linLegacyPins != oldLegacyPins) {
          request->send(200, "application/json",
                        "{\"ok\":true,\"reboot\":true,\"message\":\"LIN pin mapping changed. Rebooting...\"}");
          delay(100);
          ESP.restart();
          return;
        }
        // Likewise the TWAI timing is fixed at driver install.
        if (canBitrateKbit != oldBitrate) {
          request->send(200, "application/json",
                        "{\"ok\":true,\"reboot\":true,\"message\":\"CAN bitrate changed. Rebooting...\"}");
          delay(100);
          ESP.restart();
          return;
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
        // The idle baseline is taken by getButtonState() from the next frame
        // that arrives without error, not from here — lastLinInFrame is zeroed
        // on any LIN error, and a zero baseline makes every constant byte in
        // the following frame look like a press.
        learnBaselineReady = false;

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
          diagResistiveOhm = (uint16_t)(ohm < 0 ? 0 : (ohm > digipotMaxOhm ? digipotMaxOhm : ohm));
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
          if (v > digipotMaxOhm) v = digipotMaxOhm;
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

  // Passthru diagnostic mode: bypass mapping/protocol-shaping and relay LIN
  // frames unmodified between the wheel and chassis buses, logging every
  // poll/send attempt (success or failure) to the LIN Monitor.
  server.on(
      "/api/passthru", HTTP_POST,
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
        passthruEnabled = body["enabled"] | (bool)passthruEnabled;
        request->send(200, "application/json", "{\"ok\":true}");
      });

  // ----- LIN diagnostic log -----
  // `since` = last seen writeIndex; only newer entries are returned.
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t since = 0;
    if (req->hasParam("since")) {
      since = strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
    }
    const uint32_t writeIdx = logWriteIndex;
    uint32_t start = since;
    if (writeIdx > kLogLineCount && start < writeIdx - kLogLineCount) {
      start = writeIdx - kLogLineCount;
    }
    if (start > writeIdx) start = writeIdx;

    JsonDocument doc;
    doc["writeIndex"] = writeIdx;
    JsonArray arr = doc["entries"].to<JsonArray>();
    for (uint32_t i = start; i < writeIdx; i++) {
      const LogEntry& e = logBuffer[i % kLogLineCount];
      JsonObject row = arr.add<JsonObject>();
      row["i"] = i;
      row["ms"] = e.ms;
      row["t"] = e.text;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    portENTER_CRITICAL(&stateMux);
    logWriteIndex = 0;
    memset(logBuffer, 0, sizeof(logBuffer));
    portEXIT_CRITICAL(&stateMux);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Full LIN monitor log (up to the last kLogLineCount lines held) as a downloadable CSV.
  server.on("/api/log/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    const uint32_t writeIdx = logWriteIndex;
    const uint32_t start = (writeIdx > kLogLineCount) ? (writeIdx - kLogLineCount) : 0;

    String csv;
    csv.reserve((writeIdx - start) * 48 + 32);
    csv += "Index,TimeMs,Message\n";
    for (uint32_t i = start; i < writeIdx; i++) {
      const LogEntry& e = logBuffer[i % kLogLineCount];
      csv += String(i);
      csv += ',';
      csv += String(e.ms);
      csv += ",\"";
      for (const char* p = e.text; *p; p++) {
        if (*p == '"') csv += "\"\"";
        else csv += *p;
      }
      csv += "\"\n";
    }

    AsyncWebServerResponse* response = req->beginResponse(200, "text/csv", csv);
    response->addHeader("Content-Disposition", "attachment; filename=\"lin_monitor_log.csv\"");
    req->send(response);
  });

  // ----- CAN monitor -----
  // Separate from the LIN log: its own buffer, its own endpoints, and an
  // export in SavvyCAN's CSV column order so a capture opens directly there.
  server.on("/api/canlog", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t since = 0;
    if (req->hasParam("since")) {
      since = strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
    }
    const uint32_t writeIdx = canLogWriteIndex;
    uint32_t start = since;
    if (writeIdx > kCanLogCount && start < writeIdx - kCanLogCount) {
      start = writeIdx - kCanLogCount;
    }
    if (start > writeIdx) start = writeIdx;

    JsonDocument doc;
    doc["writeIndex"] = writeIdx;
    doc["enabled"] = canLogEnabled;
    doc["filterId"] = canLogId;
    doc["changedOnly"] = canLogChangedOnly;
    JsonArray arr = doc["entries"].to<JsonArray>();
    for (uint32_t i = start; i < writeIdx; i++) {
      const CanLogEntry& e = canLogBuffer[i % kCanLogCount];
      JsonObject row = arr.add<JsonObject>();
      row["i"] = i;
      row["ms"] = e.ms;
      row["id"] = e.id;
      row["d"] = frameToHex(e.data, e.len);
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on(
      "/api/canlog/config", HTTP_POST,
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
        canLogEnabled = body["enabled"] | (bool)canLogEnabled;
        canLogChangedOnly = body["changedOnly"] | (bool)canLogChangedOnly;
        {
          int id = body["filterId"] | (int)canLogId;
          canLogId = (uint16_t)((id > 0 && id <= 0x7FF) ? id : 0);
        }
        request->send(200, "application/json", "{\"ok\":true}");
      });

  server.on("/api/canlog/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    portENTER_CRITICAL(&stateMux);
    canLogWriteIndex = 0;
    memset(canLogBuffer, 0, sizeof(canLogBuffer));
    portEXIT_CRITICAL(&stateMux);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // SavvyCAN generic-CSV column order, so this file can be opened straight in
  // SavvyCAN without conversion. Timestamps are microseconds, as it expects.
  server.on("/api/canlog/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    const uint32_t writeIdx = canLogWriteIndex;
    const uint32_t start = (writeIdx > kCanLogCount) ? (writeIdx - kCanLogCount) : 0;

    String csv;
    csv.reserve((writeIdx - start) * 64 + 64);
    csv += "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n";
    for (uint32_t i = start; i < writeIdx; i++) {
      const CanLogEntry& e = canLogBuffer[i % kCanLogCount];
      char line[96];
      snprintf(line, sizeof(line), "%llu,%08X,false,Rx,0,%u",
               (unsigned long long)e.ms * 1000ULL, (unsigned)e.id, (unsigned)e.len);
      csv += line;
      for (uint8_t b = 0; b < e.len && b < 8; b++) {
        snprintf(line, sizeof(line), ",%02X", e.data[b]);
        csv += line;
      }
      csv += '\n';
    }

    AsyncWebServerResponse* response = req->beginResponse(200, "text/csv", csv);
    response->addHeader("Content-Disposition", "attachment; filename=\"can_monitor_log.csv\"");
    req->send(response);
  });

  // SavvyCAN analyzer: WiFi (TCP:23) and Serial (USB 1 Mbaud) are mutually
  // exclusive, which setAnalyzerMode()/setAnalyzerSerialMode() enforce.
  server.on(
      "/api/analyzer", HTTP_POST,
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
        if (!body["protocol"].isNull()) {
          const int p = body["protocol"].as<int>();
          analyzerProtocol = (uint8_t)(p == ANALYZER_PROTOCOL_LAWICEL ? ANALYZER_PROTOCOL_LAWICEL
                                                                     : ANALYZER_PROTOCOL_GVRET);
        }
        if (!body["wifi"].isNull()) {
          setAnalyzerMode(body["wifi"].as<bool>());
        }
        if (!body["serial"].isNull()) {
          setAnalyzerSerialMode(body["serial"].as<bool>());
        }
        JsonDocument out;
        out["ok"] = true;
        out["wifi"] = analyzerMode;
        out["serial"] = analyzerSerial;
        out["protocol"] = analyzerProtocol;
        String payload;
        serializeJson(out, payload);
        request->send(200, "application/json", payload);
      });

  // "/" (the UI, or the recovery page when the filesystem holds no usable UI)
  // + static files with no-cache revalidation.
  wifiManagerAttachStatic(server);
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
  // Stay awake while a client is connected OR an OTA upload is streaming, so
  // the radio is never dropped mid-update.
  // ... or a browser has hit us in the last 30 s (a phone on the home router
  // in bridge mode is not an AP station).
  return WiFi.softAPgetStationNum() > 0 || otaInProgress() || otaWebClientActive();
}

// ACTIVE -> REDUCED: close the web server cleanly, then drop AP + mDNS.
void powerOnEnterReduced()
{
  server.end();
  wifiManagerStopAP();
}

// REDUCED -> ACTIVE: bring the AP + mDNS and web server back. Routes are already
// registered (no need to re-run setupApiServer()), so we only restart the
// radio and the listener.
void powerOnExitReduced()
{
  wifiManagerStartAP();
  server.begin();
}
