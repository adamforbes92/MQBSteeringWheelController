document.addEventListener("DOMContentLoaded", initApp);

let mappings = [];
let latchedRows = new Set();  // indices of rows whose latch is engaged, from /api/status
let seenMappingsRevision = null;  // firmware's counter of Learn results; see mergeLearnedCodes()

// Unsaved-changes tracking for the button table. Settings controls save
// themselves on change; the table does not — it needs Save Setup, which sits
// below a wide table and is easy to walk away from. So: mark the table dirty on
// any edit, show it, and ask before leaving the tab or the page.
let mappingsDirty = false;

function setMappingsDirty(dirty) {
  mappingsDirty = dirty;
  const btn = document.getElementById("saveSetupBtn");
  if (btn) {
    btn.classList.toggle("dirty", dirty);
    btn.textContent = dirty ? "Save Setup • unsaved changes" : "Save Setup";
  }
  const banner = document.getElementById("unsavedBanner");
  if (banner) banner.style.display = dirty ? "" : "none";
}

function confirmLeaveUnsaved() {
  if (!mappingsDirty) return true;
  return confirm("You have unsaved button changes. Leave without saving?");
}

// A Learn lands in the firmware whenever the user gets round to pressing the
// button, which can be any time inside the 5 s window — fixed-delay reloads
// after arming were missing it, leaving the table showing the old code. The
// next Save Setup then posted that stale table back and wiped the learned
// value. So: whenever the firmware says a mapping changed, fetch the table and
// copy in JUST the learnable fields, leaving any unsaved edits elsewhere alone.
async function mergeLearnedCodes() {
  try {
    const res = await fetch("/api/setup");
    if (!res.ok) return;
    const setup = await res.json();
    const fresh = Array.isArray(setup.mappings) ? setup.mappings : [];
    let changed = false;
    fresh.forEach((f, i) => {
      const m = mappings[i];
      if (!m) return;
      for (const k of ["oldButtonId", "newLinButtonId", "sourceByte"]) {
        if (num(m[k]) !== num(f[k])) { m[k] = f[k]; changed = true; }
      }
    });
    if (changed) {
      // Not marked dirty: the learned value already lives in the firmware,
      // which persists it on its own 5 s cycle. Only browser-side edits need
      // Save Setup, and the Trigger change that usually follows will flag it.
      renderMappings();
      setStatus("Learned code applied.");
    }
  } catch (e) {}
}
let pollTimer = null;
let localLearnUntil = 0;
let localLearnRow = -1;
let localLearnTarget = 0;

// OpenHaldex mode names, indexed 0-5; 255 = "Push-to-Next".
const OH_MODE_NAMES = ["Stock", "FWD", "50:50", "60:40", "75:25", "Expert"];
const OH_MODE_PUSH_NEXT = 255;

// [pqCode, mqbCode] pairs for buttons independently verified against the
// README's documented MQB code table (see "MQB button codes" section) — the
// only rows we're confident enough about to auto-derive. Everything else
// (Phone, Voice/Mic ACC x2, Paddle +/-, Paddles-both, Horn) isn't in that
// list, so "Overwrite Existing Layout" leaves those rows untouched.
const KNOWN_BUTTON_CODE_PAIRS = [
  [0x03, 0x16], // Previous
  [0x02, 0x15], // Next
  [0x1A, 0x19], // Voice/Mic
  [0x29, 0x23], // Return
  [0x22, 0x04], // Up
  [0x23, 0x05], // Down
  [0x09, 0x03], // Source -
  [0x0A, 0x02], // Source +
  [0x28, 0x07], // OK
  [0x06, 0x10], // Volume +
  [0x07, 0x11], // Volume -
];

// Rewrite oldButtonId/newLinButtonId for rows whose current (old, new) pair
// still matches one of the known PQ/MQB pairs above (in either order), using
// whichever code each protocol selector now calls for. A row a user has
// already re-learned to a real-world value no longer matches either side of
// a known pair, so it's left alone automatically — no separate "custom" flag
// needed. Pass {dryRun: true} to just count how many rows WOULD change,
// without touching the mappings array. Returns the number of rows affected.
function applyProtocolDefaultsToMappings({ dryRun = false } = {}) {
  const wheelIsMqb = document.getElementById("wheelProtocol").value === "1";
  const chassisIsMqb = document.getElementById("chassisProtocol").value === "1";
  // Wheel and chassis on the SAME protocol (PQ<->PQ or MQB<->MQB) means no
  // translation is possible by definition -- every configured row's new code
  // must equal its old code. This has to apply to every row, not just the
  // ones in KNOWN_BUTTON_CODE_PAIRS below: Phone, Voice/Mic ACC2, Paddle +/-,
  // Paddles-both and Horn were never in that list (no verified MQB code for
  // them), so under the old pair-only logic they stayed stuck at whatever
  // "new" code they shipped with (0 in the default table) even in a
  // same-protocol setup where they trivially should have matched "old".
  const sameProtocolBothSides = wheelIsMqb === chassisIsMqb;
  let updated = 0;

  mappings.forEach((m) => {
    const old = num(m.oldButtonId);
    const nw = num(m.newLinButtonId);

    if (sameProtocolBothSides) {
      if (old !== 0 && nw !== old) {
        updated++;
        if (!dryRun) {
          m.newLinButtonId = old;
        }
      }
      return;
    }

    const pair = KNOWN_BUTTON_CODE_PAIRS.find(
      ([pq, mqb]) => (old === pq && nw === mqb) || (old === mqb && nw === pq)
    );
    if (!pair) return;

    const [pq, mqb] = pair;
    const newOld = wheelIsMqb ? mqb : pq;
    const newNew = chassisIsMqb ? mqb : pq;
    if (m.oldButtonId !== newOld || m.newLinButtonId !== newNew) {
      updated++;
      if (!dryRun) {
        m.oldButtonId = newOld;
        m.newLinButtonId = newNew;
      }
    }
  });

  return updated;
}

// Fired when Wheel Protocol or Chassis Protocol changes: offer to update the
// button table's known PQ/MQB codes to match, since we do know the right
// values for a subset of rows (see KNOWN_BUTTON_CODE_PAIRS above). If none
// of the current rows are on a recognised code, say so plainly instead of
// showing a confirm dialog with nothing to actually do.
function promptApplyProtocolDefaults() {
  const candidateCount = applyProtocolDefaultsToMappings({ dryRun: true });
  if (candidateCount === 0) {
    setStatus("No button rows are on a recognised PQ/MQB code, so there's nothing to update automatically.");
    return;
  }

  const wheelLabel = document.getElementById("wheelProtocol").selectedOptions[0].text;
  const chassisLabel = document.getElementById("chassisProtocol").selectedOptions[0].text;
  const ok = confirm(
    `Update ${candidateCount} button code(s) to match Wheel Protocol (${wheelLabel}) / Chassis Protocol (${chassisLabel})?\n\n` +
    `Rows you've already learned or customised away from the standard PQ/MQB codes are left alone.\n\n` +
    `This saves immediately.`
  );
  if (!ok) return;

  const updated = applyProtocolDefaultsToMappings();
  renderMappings();
  saveSetup();
  setStatus(`Updated ${updated} button code(s) to match the selected protocols.`);
}

function ohModeOptions(selected) {
  const opts = OH_MODE_NAMES.map(
    (n, i) => `<option value="${i}"${selected === i ? " selected" : ""}>${n}</option>`
  );
  opts.push(
    `<option value="${OH_MODE_PUSH_NEXT}"${selected === OH_MODE_PUSH_NEXT ? " selected" : ""}>Push-to-Next</option>`
  );
  return opts.join("");
}

// Trigger qualifier, in flag bits 4-7: scroll direction (4 up, 5 down) and
// press stage (6 short, 7 long). None set — every row that predates this —
// means the row fires on any press, which is the original behaviour. The
// wheel reports press stage itself (byte 3: 1 pressed, 4 once held ~0.8 s),
// so "long press" needs no timing on our side.
const FLAG_ROTARY_UP = 0x10;
const FLAG_ROTARY_DOWN = 0x20;
const FLAG_PRESS_SHORT = 0x40;
const FLAG_PRESS_LONG = 0x80;
const TRIGGER_MASK = FLAG_ROTARY_UP | FLAG_ROTARY_DOWN | FLAG_PRESS_SHORT | FLAG_PRESS_LONG;

const TRIGGERS = [
  [0, "Any press", 0],
  [1, "Scroll up", FLAG_ROTARY_UP],
  [2, "Scroll down", FLAG_ROTARY_DOWN],
  [3, "Short press", FLAG_PRESS_SHORT],
  [4, "Long press", FLAG_PRESS_LONG],
];

// Which frame byte a row matches on is otherwise invisible, which made a
// mis-learned row (matching a constant byte, so permanently "pressed") very
// hard to spot. Bytes 6 and 7 are the legitimate paddle/horn positions.
function srcByteHint(row) {
  const sb = num(row.sourceByte);
  if (!sb) return "Matches the default button byte";
  return `Matches frame byte ${sb}` +
    (srcByteSuspect(row) ? " — unusual; if this row is always active, re-learn or delete it" : "");
}

function srcByteSuspect(row) {
  const sb = num(row.sourceByte);
  return sb !== 0 && sb !== 6 && sb !== 7;
}

function triggerOptions(flags) {
  const bits = flags & TRIGGER_MASK;
  let sel = 0;
  for (const [v, , bit] of TRIGGERS) if (bit && bits === bit) sel = v;
  return TRIGGERS
    .map(([v, n]) => `<option value="${v}"${sel === v ? " selected" : ""}>${n}</option>`)
    .join("");
}

function initApp() {
  initTabs();
  // Browser-level guard: refresh, close, or navigating away from the page.
  window.addEventListener("beforeunload", (e) => {
    if (!mappingsDirty) return;
    e.preventDefault();
    e.returnValue = "";
  });
  initCollapsibleCards();
  wireControls();
  loadSetup();
  loadStatus();
  pollTimer = setInterval(loadStatus, 200);
  setInterval(pollLog, 400);
  setInterval(pollCanLog, 400);
}

function initTabs() {
  const tabs = document.querySelectorAll(".tab");
  tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      const current = document.querySelector(".tab.active");
      const leavingButtons = current && current.dataset.tab === "buttons"
        && tab.dataset.tab !== "buttons";
      if (leavingButtons && !confirmLeaveUnsaved()) return;

      tabs.forEach((x) => x.classList.remove("active"));
      tab.classList.add("active");

      const target = tab.dataset.tab;
      document.querySelectorAll(".panel").forEach((panel) => {
        panel.classList.remove("active");
      });
      document.getElementById(`tab-${target}`).classList.add("active");
    });
  });
}

function wireControls() {
  document.getElementById("addRowBtn").addEventListener("click", () => {
    mappings.push({
      name: "New Button",
      oldButtonId: 0,
      newLinButtonId: 0,
      canByteIndex: 255,
      canBitIndex: 0,
      resistiveOhm: 0,
      flags: 0,
      openHaldexMode: 0,
    });
    renderMappings();
    // Persist so the firmware knows the row exists — Learn is rejected for
    // rows beyond the saved mapping count.
    saveSetup();
  });

  document.getElementById("saveSetupBtn").addEventListener("click", saveSetup);
  document.getElementById("learnAuxDimBtn").addEventListener("click", () => learnAuxLimit("dim"));
  document.getElementById("learnAuxBrightBtn").addEventListener("click", () => learnAuxLimit("bright"));

  // Auto-save toggles with mutual exclusion between aux and force.
  function wireExclusive(id, otherId) {
    document.getElementById(id).addEventListener("change", function () {
      if (this.checked) {
        document.getElementById(otherId).checked = false;
      }
      saveSetup();
    });
  }
  wireExclusive("hasAuxLight", "forceBacklight");
  wireExclusive("forceBacklight", "hasAuxLight");

  // Other auto-save toggles (no mutual exclusion).
  ["canBroadcastEnabled", "paddlesEnabled", "linOutputEnabled", "digipot20kEnabled", "linLegacyPins"].forEach((id) => {
    document.getElementById(id).addEventListener("change", saveSetup);
  });

  // Typed fields save on change too (blur / Enter), like the toggles above.
  // They didn't, and the Setup tab has no Save button, so a typed CAN ID or
  // LIN ID only reached the device when some other control happened to save —
  // which looked like the old value being "held in memory".
  ["canBroadcastId", "canHoldMs", "linOutputId", "linButtonInId", "linLightInId",
   "linTempInId", "linAccInId", "linButtonByteIndex", "linRotaryByteIndex",
   "auxDimDuty", "auxBrightDuty"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("change", saveSetup);
  });
  const canBitrate = document.getElementById("canBitrateKbit");
  if (canBitrate) {
    canBitrate.addEventListener("change", () => {
      if (confirm("Changing the CAN bus speed reboots the device. Continue?")) {
        saveSetup();
      } else {
        loadSetup();  // put the select back to the saved value
      }
    });
  }

  // Force brightness slider.
  const pctSlider = document.getElementById("forceBacklightPercent");
  const pctDisplay = document.getElementById("forceBacklightPctDisplay");
  pctSlider.addEventListener("input", () => { pctDisplay.textContent = pctSlider.value; });
  pctSlider.addEventListener("change", saveSetup);

  // Wheel protocol selector + MQB activation preset.
  const wheelProtocol = document.getElementById("wheelProtocol");
  if (wheelProtocol) {
    wheelProtocol.addEventListener("change", () => {
      updateWheelProtocolUi();
      saveSetup();
      promptApplyProtocolDefaults();
    });
  }
  const mqbActPreset = document.getElementById("mqbActPreset");
  if (mqbActPreset) {
    mqbActPreset.addEventListener("change", () => {
      const map = { ff0000: ["FF", "00", "00"], "816440": ["81", "64", "40"] };
      const v = map[mqbActPreset.value];
      updateActByteVisibility();
      if (v) {
        document.getElementById("mqbActByte1").value = v[0];
        document.getElementById("mqbActByte2").value = v[1];
        document.getElementById("mqbActByte3").value = v[2];
        saveSetup();
      }
    });
  }
  ["mqbActByte1", "mqbActByte2", "mqbActByte3"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("change", saveSetup);
  });

  // Charisma: mode switch reveals the fields that mode actually uses.
  const charismaMode = document.getElementById("charismaMode");
  if (charismaMode) {
    charismaMode.addEventListener("change", () => {
      updateCharismaUi();
      saveSetup();
    });
  }
  ["charismaButtonBit", "charismaProgramCount"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("change", saveSetup);
  });
  const charismaParts = document.getElementById("charismaParticipants");
  if (charismaParts) charismaParts.addEventListener("change", saveSetup);

  // Chassis protocol selector — independent of wheelProtocol.
  const chassisProtocol = document.getElementById("chassisProtocol");
  if (chassisProtocol) {
    chassisProtocol.addEventListener("change", () => {
      saveSetup();
      promptApplyProtocolDefaults();
    });
  }

  // Diagnostic: PNP toggle.
  document.getElementById("diagPnpActive").addEventListener("change", async function () {
    await fetch("/api/diag/pnp", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ active: this.checked }),
    });
  });

  // Diagnostic: passthru mode toggle.
  document.getElementById("passthruEnabled").addEventListener("change", async function () {
    await fetch("/api/passthru", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ enabled: this.checked }),
    });
  });

  // Diagnostic: resistive output enable + slider.
  const diagResSlider = document.getElementById("diagResistiveOhm");
  const diagResDisplay = document.getElementById("diagResistiveOhmDisplay");
  diagResSlider.addEventListener("input", () => { diagResDisplay.textContent = diagResSlider.value; });

  async function sendDiagResistive() {
    await fetch("/api/diag/resistive", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        enabled: document.getElementById("diagResistiveEnabled").checked,
        ohm: Number(diagResSlider.value),
      }),
    });
  }
  document.getElementById("diagResistiveEnabled").addEventListener("change", sendDiagResistive);
  diagResSlider.addEventListener("change", sendDiagResistive);

  // Diagnostic: test resistance enable + up/down step buttons.
  async function sendTestResistance(dir) {
    const res = await fetch("/api/diag/testresistance", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        enabled: document.getElementById("testResistanceEnabled").checked,
        dir: dir,
      }),
    });
    if (res.ok) {
      const data = await res.json();
      const disp = document.getElementById("testResistanceOhmDisplay");
      if (disp && data.ohm != null) disp.textContent = data.ohm;
    }
  }
  document.getElementById("testResistanceEnabled").addEventListener("change", () => sendTestResistance(0));
  document.getElementById("testResistanceUpBtn").addEventListener("click", () => sendTestResistance(1));
  document.getElementById("testResistanceDownBtn").addEventListener("click", () => sendTestResistance(-1));

  // Diagnostic: LIN monitor clear.
  const logClearBtn = document.getElementById("logClearBtn");
  if (logClearBtn) {
    logClearBtn.addEventListener("click", async () => {
      await fetch("/api/log/clear", { method: "POST" });
      const view = document.getElementById("logView");
      if (view) view.textContent = "";
      logSince = 0;
    });
  }

  // CAN monitor: config controls, clear and export.
  ["canLogEnabled", "canLogId", "canLogChangedOnly"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("change", pushCanLogConfig);
  });
  const canLogClearBtn = document.getElementById("canLogClearBtn");
  if (canLogClearBtn) {
    canLogClearBtn.addEventListener("click", async () => {
      await fetch("/api/canlog/clear", { method: "POST" });
      const view = document.getElementById("canLogView");
      if (view) view.textContent = "";
      canLogSince = 0;
    });
  }
  const canLogExportBtn = document.getElementById("canLogExportBtn");
  if (canLogExportBtn) {
    canLogExportBtn.addEventListener("click", async () => {
      try {
        const res = await fetch("/api/canlog/export");
        if (!res.ok) throw new Error("Export failed");
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "can_monitor_log.csv";
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        setStatus("CAN monitor log exported.");
      } catch (err) {
        setStatus(`CAN export failed: ${err.message}`);
      }
    });
  }

  // SavvyCAN analyzer. WiFi and Serial are mutually exclusive in firmware, so
  // reflect whatever it reports back rather than assuming the click won.
  const pushAnalyzer = async (body) => {
    try {
      const res = await fetch("/api/analyzer", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const r = await res.json();
      document.getElementById("analyzerWifi").checked = !!r.wifi;
      document.getElementById("analyzerSerial").checked = !!r.serial;
      document.getElementById("analyzerProtocol").value = String(r.protocol || 0);
    } catch (err) {
      setStatus(`Analyzer failed: ${err.message}`);
    }
  };
  const analyzerWifi = document.getElementById("analyzerWifi");
  if (analyzerWifi) {
    analyzerWifi.addEventListener("change", () => pushAnalyzer({ wifi: analyzerWifi.checked }));
  }
  const analyzerSerialEl = document.getElementById("analyzerSerial");
  if (analyzerSerialEl) {
    analyzerSerialEl.addEventListener("change", () => pushAnalyzer({ serial: analyzerSerialEl.checked }));
  }
  const analyzerProtocolEl = document.getElementById("analyzerProtocol");
  if (analyzerProtocolEl) {
    analyzerProtocolEl.addEventListener("change", () =>
      pushAnalyzer({ protocol: Number(analyzerProtocolEl.value) }));
  }

  // Diagnostic: LIN monitor export (CSV download).
  const logExportBtn = document.getElementById("logExportBtn");
  if (logExportBtn) {
    logExportBtn.addEventListener("click", async () => {
      try {
        const res = await fetch("/api/log/export");
        if (!res.ok) throw new Error("Export failed");
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = "lin_monitor_log.csv";
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
        setStatus("LIN monitor log exported.");
      } catch (err) {
        setStatus(`Export failed: ${err.message}`);
      }
    });
  }

  // Settings import/export.
  const exportBtn = document.getElementById("exportSettingsBtn");
  if (exportBtn) exportBtn.addEventListener("click", exportSettings);
  const importBtn = document.getElementById("importSettingsBtn");
  const importFile = document.getElementById("importSettingsFile");
  if (importBtn && importFile) {
    importBtn.addEventListener("click", () => importFile.click());
    importFile.addEventListener("change", importSettings);
  }
}

// Show the MQB activation controls only when the MQB protocol is selected,
// and reveal the raw activation bytes only for the Custom preset (otherwise
// the preset defines them and the fields are just noise).
function updateWheelProtocolUi() {
  const sel = document.getElementById("wheelProtocol");
  const row = document.getElementById("mqbActRow");
  if (sel && row) row.style.display = sel.value === "1" ? "" : "none";
  syncActPresetToBytes();
  updateActByteVisibility();
}

// Pick the preset dropdown value that matches the current activation bytes.
function syncActPresetToBytes() {
  const preset = document.getElementById("mqbActPreset");
  if (!preset) return;
  const b1 = (document.getElementById("mqbActByte1").value || "").toUpperCase();
  const b2 = (document.getElementById("mqbActByte2").value || "").toUpperCase();
  const b3 = (document.getElementById("mqbActByte3").value || "").toUpperCase();
  const key = `${b1}${b2}${b3}`;
  if (key === "FF0000") preset.value = "ff0000";
  else if (key === "816440") preset.value = "816440";
  else preset.value = "custom";
}

// Hide the three activation-byte fields unless the Custom preset is active.
function updateActByteVisibility() {
  const preset = document.getElementById("mqbActPreset");
  const custom = preset && preset.value === "custom";
  ["mqbActByte1", "mqbActByte2", "mqbActByte3"].forEach((id) => {
    const el = document.getElementById(id);
    if (el && el.parentElement) el.parentElement.style.display = custom ? "" : "none";
  });
}

async function exportSettings() {
  try {
    const res = await fetch("/api/setup");
    if (!res.ok) throw new Error("fetch failed");
    const data = await res.text();
    const blob = new Blob([data], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "mfsw-settings.json";
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    setStatus("Settings exported.");
  } catch (err) {
    setStatus(`Export failed: ${err.message}`);
  }
}

async function importSettings(event) {
  const file = event.target.files && event.target.files[0];
  event.target.value = "";  // allow re-importing the same file
  if (!file) return;
  try {
    const text = await file.text();
    const parsed = JSON.parse(text);  // validate before sending
    const res = await fetch("/api/setup/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(parsed),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    setStatus("Settings imported.");
    await loadSetup();
  } catch (err) {
    setStatus(`Import failed: ${err.message}`);
  }
}

// Participant checklist is built from the names the firmware reports, so the
// bit order here always matches kCharismaParticipants in defs.h.
function renderCharismaParticipants(names, mask) {
  const host = document.getElementById("charismaParticipants");
  if (!host) return;
  const list = Array.isArray(names) ? names : [];
  host.innerHTML = list
    .map(
      (name, i) =>
        `<label><input type="checkbox" data-charisma-part="${i}"${(mask & (1 << i)) ? " checked" : ""}><span>${name}</span></label>`
    )
    .join("");
}

function charismaParticipantMask() {
  let mask = 0;
  document.querySelectorAll("[data-charisma-part]").forEach((el) => {
    if (el.checked) mask |= 1 << Number(el.dataset.charismaPart);
  });
  return mask;
}

// Each mode uses a different subset: the button signal is MQB-button only, the
// participant list is MQB-coordinator only (PQ has a single car-wide mode), and
// both coordinator modes need the program count.
function updateCharismaUi() {
  const mode = Number(document.getElementById("charismaMode").value);
  const show = (id, on) => {
    const el = document.getElementById(id);
    if (el) el.style.display = on ? "" : "none";
  };
  show("charismaButtonRow", mode === 1);
  show("charismaProgramRow", mode === 2 || mode === 3);
  show("charismaParticipantsRow", mode === 2);
}

async function loadSetup() {
  try {
    const res = await fetch("/api/setup");
    if (!res.ok) {
      throw new Error("Setup fetch failed");
    }

    const setup = await res.json();
    document.getElementById("canBroadcastEnabled").checked = !!setup.canBroadcastEnabled;
    document.getElementById("canBroadcastId").value = hex3(setup.canBroadcastId || 0x1E0);
    document.getElementById("paddlesEnabled").checked = !!setup.paddlesEnabled;
    document.getElementById("hasAuxLight").checked = !!setup.hasAuxLight;
    document.getElementById("forceBacklight").checked = !!setup.forceBacklight;
    const pct = Number(setup.forceBacklightPercent != null ? setup.forceBacklightPercent : 100);
    document.getElementById("forceBacklightPercent").value = pct;
    document.getElementById("forceBacklightPctDisplay").textContent = pct;
    document.getElementById("canHoldMs").value = Number(setup.canHoldMs || 250);
    document.getElementById("canBitrateKbit").value = String(setup.canBitrateKbit || 500);
    document.getElementById("linOutputEnabled").checked = !!setup.linOutputEnabled;
    document.getElementById("linOutputId").value = hex2(setup.linOutputId != null ? setup.linOutputId : 0x0E);
    document.getElementById("digipot20kEnabled").checked = !!setup.digipot20kEnabled;
    document.getElementById("linLegacyPins").checked = !!setup.linLegacyPins;
    document.getElementById("linButtonInId").value = hex2(setup.linButtonInId != null ? setup.linButtonInId : 0x0E);
    document.getElementById("linAccInId").value = hex2(setup.linAccInId != null ? setup.linAccInId : 0x0F);
    document.getElementById("linTempInId").value = hex2(setup.linTempInId != null ? setup.linTempInId : 0x3A);
    document.getElementById("linLightInId").value = hex2(setup.linLightInId != null ? setup.linLightInId : 0x0D);
    document.getElementById("linButtonByteIndex").value = (setup.linButtonByteIndex != null ? setup.linButtonByteIndex : 1);
    document.getElementById("linRotaryByteIndex").value = (setup.linRotaryByteIndex != null ? setup.linRotaryByteIndex : 3);
    document.getElementById("wheelProtocol").value = String(setup.wheelProtocol != null ? setup.wheelProtocol : 0);
    document.getElementById("mqbActByte1").value = hex2(setup.mqbActByte1 != null ? setup.mqbActByte1 : 0xFF);
    document.getElementById("mqbActByte2").value = hex2(setup.mqbActByte2 != null ? setup.mqbActByte2 : 0x00);
    document.getElementById("mqbActByte3").value = hex2(setup.mqbActByte3 != null ? setup.mqbActByte3 : 0x00);
    document.getElementById("chassisProtocol").value = String(setup.chassisProtocol != null ? setup.chassisProtocol : 1);
    document.getElementById("charismaMode").value = String(setup.charismaMode != null ? setup.charismaMode : 0);
    document.getElementById("charismaButtonBit").value = String(setup.charismaButtonBit != null ? setup.charismaButtonBit : 22);
    document.getElementById("charismaProgramCount").value = Number(setup.charismaProgramCount || 4);
    renderCharismaParticipants(setup.charismaParticipantNames, num(setup.charismaParticipants));
    updateWheelProtocolUi();
    updateCharismaUi();
    document.getElementById("auxDimDuty").value    = Number((setup.auxDimDuty    != null ? setup.auxDimDuty    : 197) / 10).toFixed(1);
    document.getElementById("auxBrightDuty").value = Number((setup.auxBrightDuty != null ? setup.auxBrightDuty : 980) / 10).toFixed(1);

    mappings = Array.isArray(setup.mappings) ? setup.mappings : [];
    setMappingsDirty(false);
    renderMappings();
  } catch (err) {
    setStatus(`Setup load failed: ${err.message}`);
  }
}

async function loadStatus() {
  try {
    const res = await fetch("/api/status");
    if (!res.ok) {
      throw new Error("Status fetch failed");
    }

    const s = await res.json();

    // A controller that isn't running trumps everything else: nothing is going
    // out or coming in regardless of settings, and that is the first thing to
    // know when "CAN doesn't work".
    const canBadge = document.getElementById("canStatusBadge");
    if (s.canBusState && s.canBusState !== "running") {
      setText("canStatusBadge", `CAN: ${s.canBusState}`);
      if (canBadge) canBadge.classList.add("error");
    } else {
      setText("canStatusBadge", s.canEnabled ? (s.canHealthy ? "CAN: Healthy" : "CAN: No Data") : "CAN: Off");
      if (canBadge) canBadge.classList.remove("error");
    }
    // Frames handed to the driver vs refused. If "accepted" doesn't climb while
    // a mapped button is held, nothing is being asked to transmit — that's
    // configuration (row's CAN Byte, flags, broadcast toggle), not the bus.
    if (typeof s.canTxAccepted === "number") {
      setText("canTxStats",
        `TX: ${s.canTxAccepted} accepted, ${s.canTxRefused || 0} refused @ ${s.canBitrateKbit || 500} kbit/s`);
    }

    setText("buttonIncoming", s.buttonIncoming ? toHexId(s.buttonIncoming) : "--");
    setText("pressedButtonName", s.pressedButtonName || "--");
    setText("incomingLinData", s.incomingLinData || "--");
    setText("incomingBcmData", s.incomingBcmData || "--");
    setText("outgoingLinData", s.outgoingLinData || "--");
    setText("outgoingCanId", toHexId(s.outgoingCanId));
    setText("outgoingCanData", s.outgoingCanData || "--");
    renderCanBytes(Array.isArray(s.canBytes) ? s.canBytes : [], s.canBroadcastId);
    setText("backlightSource", s.backlightSource || "--");
    setText("backlightState", s.backlightState || (s.backlightOn ? "ON" : "OFF"));
    setText("backlightPercent", `${Number(s.backlightPercent || 0)}%`);
    setText("bcmLightPercent", s.bcmLightAvailable ? `${Number(s.bcmLightPercent || 0)}%` : "N/A");
    const auxOnUs  = Number(s.auxOnTimeUs || 0);
    const auxPerUs = Number(s.auxPeriodUs  || 0);
    const auxDutyPc = auxPerUs > 0 ? ((auxOnUs / auxPerUs) * 100).toFixed(1) : "--";
    const auxFreqHz = auxPerUs > 0 ? (1e6 / auxPerUs).toFixed(1) : "--";
    setText("auxDutyLive",
      `on: ${auxOnUs}\u00b5s  period: ${auxPerUs}\u00b5s  (${auxDutyPc}% @ ${auxFreqHz} Hz)  ` +
      `cal dim/bright duty: ${(Number(s.auxDimDuty != null ? s.auxDimDuty : 197) / 10).toFixed(1)}% / ${(Number(s.auxBrightDuty != null ? s.auxBrightDuty : 980) / 10).toFixed(1)}%` +
      (s.forceBacklight ? "  [forced on]" : ""));

    // Update diagnostic panel controls from live state
    const pnpEl = document.getElementById("diagPnpActive");
    if (pnpEl) pnpEl.checked = !!s.diagPnpActive;
    const passthruEl = document.getElementById("passthruEnabled");
    if (passthruEl) passthruEl.checked = !!s.passthruEnabled;
    const resEnEl = document.getElementById("diagResistiveEnabled");
    if (resEnEl) resEnEl.checked = !!s.diagResistiveEnabled;
    const resSlider = document.getElementById("diagResistiveOhm");
    const resDisplay = document.getElementById("diagResistiveOhmDisplay");
    if (resSlider && !resSlider.matches(":active")) {
      resSlider.value = Number(s.diagResistiveOhm || 0);
      if (resDisplay) resDisplay.textContent = resSlider.value;
    }

    const testResEnEl = document.getElementById("testResistanceEnabled");
    if (testResEnEl) testResEnEl.checked = !!s.testResistanceEnabled;
    const testResDisplay = document.getElementById("testResistanceOhmDisplay");
    if (testResDisplay && s.testResistanceOhm != null) {
      testResDisplay.textContent = Number(s.testResistanceOhm);
    }

    // System health (understated text values; muted when inactive)
    setStatusValue("canHealthState",
      s.canEnabled ? (s.canHealthy ? "Healthy" : "No Data") : "Disabled",
      s.canEnabled && s.canHealthy);
    setStatusValue("lin1HealthState",
      s.lin1Healthy ? "Healthy" : "No Data",
      s.lin1Healthy);
    setStatusValue("lin2HealthState",
      s.lin2Healthy ? "Healthy" : (s.lin2Active ? "No Data" : "Idle"),
      s.lin2Healthy);
    setStatusValue("lin2LightHealthState",
      s.lin2LightHealthy ? "Healthy" : (s.lin2LightActive ? "No Data" : "Idle"),
      s.lin2LightHealthy);
    setStatusValue("resistiveHealthState", `${Number(s.resistiveOhmNow || 0)} \u03A9`, true);
    if (resSlider && s.digipotMaxOhm) resSlider.max = Number(s.digipotMaxOhm);

    const btnText = s.pressedButtonName ? String(s.pressedButtonName).slice(0, 10) : "--";
    setText("liveButtonBadge", `Btn: ${btnText}`);

    if (s.FW_VERSION) {
      const otaEl = document.getElementById("otaFwVersion");
      if (otaEl) otaEl.textContent = s.FW_VERSION;
    }

    // A Learn landed since we last looked: pull the learned code into the
    // table before the user can Save over it.
    if (typeof s.mappingsRevision === "number") {
      if (seenMappingsRevision !== null && s.mappingsRevision !== seenMappingsRevision) {
        mergeLearnedCodes();
      }
      seenMappingsRevision = s.mappingsRevision;
    }

    // Latched rows: named on the dashboard, highlighted in the builder table.
    {
      const next = new Set(Array.isArray(s.latched) ? s.latched.map(Number) : []);
      const changed = next.size !== latchedRows.size || [...next].some((i) => !latchedRows.has(i));
      latchedRows = next;
      const latchEl = document.getElementById("latchedStatus");
      if (latchEl) {
        const names = [...next].map((i) => (mappings[i] && mappings[i].name) || `row ${i + 1}`);
        latchEl.textContent = names.length ? `Latched: ${names.join(", ")}` : "Latched: none";
        latchEl.classList.toggle("active", names.length > 0);
      }
      if (changed) {
        document.querySelectorAll("#mappingRows tr[data-idx]").forEach((tr) => {
          tr.classList.toggle("latched", next.has(Number(tr.dataset.idx)));
        });
      }
    }

    // Sampled at the status-poll rate, which is slower than the LIN poll, so
    // this shows the last movement it happened to catch rather than every
    // detent — enough to confirm which way the wheel is reporting.
    const scrollEl = document.getElementById("scrollStatus");
    if (scrollEl && typeof s.rotaryDelta === "number" && s.rotaryDelta !== 0) {
      const dir = s.rotaryDelta > 0 ? "up" : "down";
      scrollEl.textContent =
        `Scroll: last movement ${s.rotaryDelta > 0 ? "+" : ""}${s.rotaryDelta} (${dir})`;
    }

    const chaEl = document.getElementById("charismaStatus");
    if (chaEl) {
      const mode = Number(s.charismaMode || 0);
      chaEl.style.display = mode === 0 ? "none" : "";
      if (mode !== 0) {
        const how = mode === 1 ? "MQB button press on 0x65A"
                  : mode === 3 ? "PQ coordinator on 0x390"
                  : "MQB coordinator on 0x385/0x3E8";
        chaEl.textContent = `Charisma program: ${Number(s.charismaProgram || 0)} (${how})`;
      }
    }

    const ohEl = document.getElementById("openHaldexStatus");
    if (ohEl) {
      if (s.openHaldexPresent) {
        const pend = s.openHaldexPending ? " (changing…)" : "";
        ohEl.textContent = `OpenHaldex mode: ${s.openHaldexModeStr || "--"}${pend}`;
      } else {
        ohEl.textContent = "OpenHaldex: not detected";
      }
    }

    updateLearnBannerFromStatus(s);
  } catch (err) {
    updateLearnBannerFromStatus(null);
  }
}

function updateLearnBannerFromStatus(status) {
  const el = document.getElementById("learnBuilderBanner");
  if (!el) {
    return;
  }

  const now = Date.now();

  if (status && status.learnActive) {
    const sec = Math.max(0, Math.ceil((status.learnMsRemaining || 0) / 1000));
    const rowText = Number(status.learnRowIndex || 0) + 1;
    const targetText = learnTargetText(status.learnTarget || 0);

    el.classList.add("active");
    el.textContent = `Learn active for row ${rowText} (${targetText}) - ${sec}s remaining`;
    return;
  }

  if (localLearnUntil > now) {
    const sec = Math.ceil((localLearnUntil - now) / 1000);
    const rowText = localLearnRow + 1;
    const targetText = learnTargetText(localLearnTarget);
    el.classList.add("active");
    el.textContent = `Learn active for row ${rowText} (${targetText}) - ${sec}s remaining`;
    return;
  }

  el.classList.remove("active");
  el.textContent = "Learn mode idle.";
}

function learnTargetText(target) {
  if (target === 1) {
    return "Button ID_LIN (original)";
  }
  if (target === 2) {
    return "Button ID_LIN (new)";
  }
  if (target === 3) {
    return "Button ID_CAN (new)";
  }
  return "Unknown";
}

function renderMappings() {
  const tbody = document.getElementById("mappingRows");
  tbody.innerHTML = "";

  mappings.forEach((row, idx) => {
    const tr = document.createElement("tr");
    tr.dataset.idx = idx;
    if (latchedRows.has(idx)) tr.classList.add("latched");

    const byteIdx = (typeof row.canByteIndex === "number" && row.canByteIndex < 8) ? row.canByteIndex : 255;
    const bitIdx = clamp(num(row.canBitIndex), 0, 7);
    const byteOptions = [255, 0, 1, 2, 3, 4, 5, 6, 7]
      .map((b) => `<option value="${b}"${byteIdx === b ? " selected" : ""}>${b === 255 ? "\u2014" : b}</option>`)
      .join("");

    // OpenHaldex and Charisma control are exclusive: when either is enabled the
    // other output columns are disabled to make it clear the button only
    // commands that one thing. They are also exclusive of each other.
    const ohEnabled = (num(row.flags) & 4) !== 0;
    const chaEnabled = (num(row.flags) & 8) !== 0;
    const ohMode = (typeof row.openHaldexMode === "number") ? row.openHaldexMode : 0;
    const exDis = (ohEnabled || chaEnabled) ? " disabled" : "";

    tr.innerHTML = `
      <td><input data-field="name" data-idx="${idx}" type="text" value="${escapeAttr(row.name || "")}"></td>
      <td><input data-field="oldButtonId" data-idx="${idx}" type="number" min="0" max="255" value="${num(row.oldButtonId)}" title="${srcByteHint(row)}"${srcByteSuspect(row) ? ' class="warn-cell"' : ""}></td>
      <td><button class="btn tiny secondary" data-learn="old" data-idx="${idx}">Learn</button></td>
      <td><input data-field="newLinButtonId" data-idx="${idx}" type="number" min="0" max="255" value="${num(row.newLinButtonId)}"${exDis}></td>
      <td><button class="btn tiny secondary" data-learn="lin" data-idx="${idx}"${exDis}>Learn</button></td>
      <td><select data-field="canByteIndex" data-idx="${idx}" title="Which byte of the CAN frame this button sets a bit in. &#8212; means NO CAN output for this row (the default for new rows)."${exDis}>${byteOptions}</select></td>
      <td><input data-field="canBitIndex" data-idx="${idx}" type="number" min="0" max="7" value="${bitIdx}"${exDis}></td>
      <td><input data-field="resistiveOhm" data-idx="${idx}" type="number" min="0" max="20000" value="${num(row.resistiveOhm)}"${exDis}></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="pnp" data-idx="${idx}" type="checkbox" title="Activate MOSFET output when pressed" ${(num(row.flags) & 1) ? "checked" : ""}${exDis}></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="latch" data-idx="${idx}" type="checkbox" title="Latch: press to activate, press again to clear (MOSFET, CAN and LIN)" ${(num(row.flags) & 2) ? "checked" : ""}${exDis}></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="openhaldex" data-idx="${idx}" type="checkbox" title="OpenHaldex control (exclusive: overrides MOSFET/CAN/LIN for this button)" ${ohEnabled ? "checked" : ""}${chaEnabled ? " disabled" : ""}></td>
      <td><select data-field="openHaldexMode" data-idx="${idx}" title="OpenHaldex mode to set on press"${ohEnabled ? "" : " disabled"}>${ohModeOptions(ohMode)}</select></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="charisma" data-idx="${idx}" type="checkbox" title="Charisma / Drive Select: press advances the program (exclusive: overrides MOSFET/CAN/LIN for this button). Set the output mode in Settings." ${chaEnabled ? "checked" : ""}${ohEnabled ? " disabled" : ""}></td>
      <td><select data-field="trigger" data-idx="${idx}" title="When this row fires. A roller sends one code both ways, so use two rows: Scroll up and Scroll down. Short/Long split a held button: Short stops matching once the hold passes ~0.8 s, Long starts then — so one code can tap for one action and hold for another. Long press + Latch = toggle on hold."${exDis}>${triggerOptions(num(row.flags))}</select></td>
      <td><button class="btn tiny danger" data-delete="row" data-idx="${idx}">Delete</button></td>
    `;

    tbody.appendChild(tr);
  });

  tbody.querySelectorAll("input, select").forEach((el) => {
    el.addEventListener("change", onEditRow);
  });

  tbody.querySelectorAll("button[data-learn]").forEach((btn) => {
    btn.addEventListener("click", onLearnClick);
  });

  tbody.querySelectorAll("button[data-delete='row']").forEach((btn) => {
    btn.addEventListener("click", onDeleteRowClick);
  });
}

function onDeleteRowClick(event) {
  const idx = Number(event.target.dataset.idx);
  mappings.splice(idx, 1);
  renderMappings();
  setMappingsDirty(true);
}

function onEditRow(event) {
  const idx = Number(event.target.dataset.idx);
  const field = event.target.dataset.field;
  setMappingsDirty(true);

  if (field === "flags" && event.target.dataset.subfield === "pnp") {
    const current = num(mappings[idx].flags);
    mappings[idx].flags = event.target.checked ? (current | 1) : (current & ~1);
    return;
  }
  if (field === "flags" && event.target.dataset.subfield === "latch") {
    const current = num(mappings[idx].flags);
    mappings[idx].flags = event.target.checked ? (current | 2) : (current & ~2);
    return;
  }
  if (field === "flags" && event.target.dataset.subfield === "openhaldex") {
    const current = num(mappings[idx].flags);
    mappings[idx].flags = event.target.checked ? (current | 4) : (current & ~4);
    renderMappings();  // redraw to apply/remove exclusive-mode disabling
    return;
  }
  if (field === "flags" && event.target.dataset.subfield === "charisma") {
    const current = num(mappings[idx].flags);
    mappings[idx].flags = event.target.checked ? (current | 8) : (current & ~8);
    renderMappings();  // redraw to apply/remove exclusive-mode disabling
    return;
  }
  if (field === "oldButtonId") {
    // Typing a code by hand means "match this on the normal button byte" —
    // otherwise a sourceByte left behind by a bad Learn keeps the row pointed
    // at the wrong byte, with nothing on screen explaining why.
    mappings[idx].oldButtonId = num(event.target.value);
    mappings[idx].sourceByte = 0;
    renderMappings();
    return;
  }
  if (field === "trigger") {
    const cleared = num(mappings[idx].flags) & ~TRIGGER_MASK;
    const v = Number(event.target.value);
    const entry = TRIGGERS.find((t) => t[0] === v);
    mappings[idx].flags = cleared | (entry ? entry[2] : 0);
    return;
  }
  if (field === "openHaldexMode") {
    mappings[idx].openHaldexMode = Number(event.target.value);
    return;
  }

  let value = event.target.value;
  if (field !== "name") {
    value = Number(value);
  }
  mappings[idx][field] = value;
}

async function onLearnClick(event) {
  const idx = Number(event.target.dataset.idx);
  const kind = event.target.dataset.learn;

  let target = 1;
  if (kind === "lin") {
    target = 2;
  }

  try {
    const res = await fetch("/api/learn/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ rowIndex: idx, target }),
    });

    if (!res.ok) {
      throw new Error("Learn start failed");
    }

    localLearnUntil = Date.now() + 5000;
    localLearnRow = idx;
    localLearnTarget = target;
    updateLearnBannerFromStatus(null);

    setStatus("Learn started. Press a button within 5 seconds.");
    // The learned value is pulled in by the status poll the moment the
    // firmware reports it (mappingsRevision). One late merge as a backstop,
    // after the window has closed. Not loadSetup(): that replaces the whole
    // table and would discard any unsaved edits on other rows.
    setTimeout(mergeLearnedCodes, 5500);
  } catch (err) {
    setStatus(`Learn error: ${err.message}`);
  }
}

async function saveSetup() {
  const payload = {
    canBroadcastEnabled: document.getElementById("canBroadcastEnabled").checked,
    canBroadcastId: parseInt(document.getElementById("canBroadcastId").value, 16) || 0x1E0,
    paddlesEnabled: document.getElementById("paddlesEnabled").checked,
    hasAuxLight: document.getElementById("hasAuxLight").checked,
    forceBacklight: document.getElementById("forceBacklight").checked,
    forceBacklightPercent: Number(document.getElementById("forceBacklightPercent").value),
    canHoldMs: Math.max(50, num(document.getElementById("canHoldMs").value)) || 250,
    canBitrateKbit: Number(document.getElementById("canBitrateKbit").value) || 500,
    linOutputEnabled: document.getElementById("linOutputEnabled").checked,
    linOutputId: parseInt(document.getElementById("linOutputId").value, 16) || 0x0E,
    digipot20kEnabled: document.getElementById("digipot20kEnabled").checked,
    linLegacyPins: document.getElementById("linLegacyPins").checked,
    linButtonInId: parseHexByte("linButtonInId", 0x0E),
    linAccInId: parseHexByte("linAccInId", 0x0F),
    linTempInId: parseHexByte("linTempInId", 0x3A),
    linLightInId: parseHexByte("linLightInId", 0x0D),
    linButtonByteIndex: clamp(num(document.getElementById("linButtonByteIndex").value), 0, 7),
    // Blank must not become 0: byte 0 is the frame counter, and reading scroll
    // direction from it gives a random sign.
    linRotaryByteIndex: (document.getElementById("linRotaryByteIndex").value.trim() === "")
      ? 3 : clamp(num(document.getElementById("linRotaryByteIndex").value), 0, 8),
    wheelProtocol: Number(document.getElementById("wheelProtocol").value) === 1 ? 1 : 0,
    mqbActByte1: parseHexByte("mqbActByte1", 0xFF),
    mqbActByte2: parseHexByte("mqbActByte2", 0x00),
    mqbActByte3: parseHexByte("mqbActByte3", 0x00),
    chassisProtocol: Number(document.getElementById("chassisProtocol").value) === 0 ? 0 : 1,
    charismaMode: Number(document.getElementById("charismaMode").value) || 0,
    charismaButtonBit: Number(document.getElementById("charismaButtonBit").value) || 22,
    charismaProgramCount: clamp(num(document.getElementById("charismaProgramCount").value), 2, 15) || 4,
    charismaParticipants: charismaParticipantMask(),
    mappingsRevision: seenMappingsRevision,
    auxDimDuty:    Math.round(parseFloat(document.getElementById("auxDimDuty").value) * 10) || 197,
    auxBrightDuty: Math.round(parseFloat(document.getElementById("auxBrightDuty").value) * 10) || 980,
    mappings: mappings.map((m) => ({
      name: String(m.name || "Unnamed"),
      oldButtonId: num(m.oldButtonId),
      newLinButtonId: num(m.newLinButtonId),
      canByteIndex: (typeof m.canByteIndex === "number" && m.canByteIndex < 8) ? m.canByteIndex : 255,
      canBitIndex: clamp(num(m.canBitIndex), 0, 7),
      resistiveOhm: num(m.resistiveOhm),
      flags: num(m.flags),
      openHaldexMode: (typeof m.openHaldexMode === "number") ? m.openHaldexMode : 0,
      sourceByte: (typeof m.sourceByte === "number" && m.sourceByte < 8) ? m.sourceByte : 0,
    })),
  };

  try {
    const res = await fetch("/api/setup/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });

    if (res.status === 409) {
      // A Learn landed between our last status poll and this click. Pull it
      // in rather than overwrite it, and let the user save again.
      await mergeLearnedCodes();
      setStatus("A learned code arrived just now — check the table and press Save Setup again.");
      return;
    }
    if (!res.ok) {
      throw new Error("Save failed");
    }

    const body = await res.json().catch(() => ({}));
    if (body && body.reboot) {
      setStatus(body.message || "LIN pin mapping changed. Device rebooting\u2026");
      return;
    }

    setMappingsDirty(false);
    setStatus("Setup saved.");
  } catch (err) {
    setStatus(`Save error: ${err.message}`);
  }
}

async function learnAuxLimit(target) {
  try {
    const res = await fetch("/api/backlight/learn", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ target }),
    });

    if (!res.ok) {
      const err = await res.text();
      throw new Error(err || "Learn failed");
    }

    setStatus(`Aux ${target} learned from current duty.`);
    await loadSetup();
    await loadStatus();
  } catch (err) {
    setStatus(`Aux learn error: ${err.message}`);
  }
}

// ---- Collapsible cards (setup/buttons/diagnostics collapse by default) ---
function initCollapsibleCards() {
  ["tab-setup", "tab-buttons", "tab-diag"].forEach((panelId) => {
    const panel = document.getElementById(panelId);
    if (!panel) return;
    panel.querySelectorAll(".card").forEach((card) => {
      if (card.classList.contains("no-collapse")) return;
      card.classList.add("collapsible", "collapsed");
      const h2 = card.querySelector("h2");
      if (h2) h2.addEventListener("click", () => card.classList.toggle("collapsed"));
    });
  });
}

function renderCanBytes(bytes, canId) {
  setText("canAddressDisplay", canId != null ? `0x${Number(canId).toString(16).toUpperCase()}` : "--");

  const container = document.getElementById("canByteDisplay");
  if (!container) {
    return;
  }

  container.innerHTML = bytes.slice(0, 8).map((val, byteIdx) => {
    const v = Number(val) & 0xFF;
    const bits = Array.from({ length: 8 }, (_, i) => {
      const bitIdx = 7 - i;
      const on = (v >> bitIdx) & 1;
      return `<span class="bit ${on ? "bit-on" : "bit-off"}" title="Bit ${bitIdx}">${bitIdx}</span>`;
    }).join("");
    const hexVal = v.toString(16).toUpperCase().padStart(2, "0");
    return `<div class="can-byte-row${v !== 0 ? " active" : ""}">`
      + `<span class="can-byte-label">Byte ${byteIdx}</span>`
      + `<span class="can-byte-hex">0x${hexVal}</span>`
      + `<span class="can-bits">${bits}</span>`
      + `</div>`;
  }).join("");
}

function toHexId(id) {
  if (id === undefined || id === null) {
    return "--";
  }
  return `0x${Number(id).toString(16).toUpperCase()}`;
}

function hex2(v) {
  return Number(v).toString(16).toUpperCase().padStart(2, "0");
}

function hex3(v) {
  return Number(v).toString(16).toUpperCase().padStart(3, "0");
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) {
    el.textContent = text;
  }
}

function setStatusValue(id, text, active) {
  const el = document.getElementById(id);
  if (!el) {
    return;
  }
  el.textContent = text;
  el.classList.toggle("muted", !active);
}

function setStatus(text) {
  const el = document.getElementById("otaStatus");
  if (el) {
    el.textContent = text;
  }
}

function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

function clamp(v, min, max) {
  return Math.max(min, Math.min(max, v));
}

function escapeAttr(str) {
  return String(str)
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function parseHexByte(id, dflt) {
  const el = document.getElementById(id);
  const s = el ? el.value.trim() : "";
  if (s === "") return dflt;
  const n = parseInt(s, 16);
  return Number.isFinite(n) ? (n & 0xFF) : dflt;
}

// ---------- LIN monitor log ----------
let logSince = 0;

async function pollLog() {
  try {
    const r = await (await fetch("/api/log?since=" + logSince)).json();
    if (!r) return;
    if (!Array.isArray(r.entries) || r.entries.length === 0) {
      logSince = r.writeIndex != null ? r.writeIndex : logSince;
      return;
    }
    const view = document.getElementById("logView");
    if (!view) { logSince = r.writeIndex; return; }
    const auto = document.getElementById("logAutoScroll");
    const lines = r.entries.map((e) => {
      const ts = (e.ms / 1000).toFixed(2);
      return `[${ts}] ${e.t}`;
    });
    view.textContent += (view.textContent ? "\n" : "") + lines.join("\n");
    if (view.textContent.length > 50000) {
      view.textContent = view.textContent.slice(-40000);
    }
    if (auto && auto.checked) view.scrollTop = view.scrollHeight;
    logSince = r.writeIndex;
  } catch (e) {}
}

// ---------- CAN monitor log ----------
let canLogSince = 0;

async function pushCanLogConfig() {
  const idRaw = document.getElementById("canLogId").value.trim();
  const parsed = parseInt(idRaw, 16);
  await fetch("/api/canlog/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      enabled: document.getElementById("canLogEnabled").checked,
      filterId: (idRaw && !isNaN(parsed)) ? parsed : 0,
      changedOnly: document.getElementById("canLogChangedOnly").checked,
    }),
  });
}

async function pollCanLog() {
  // Only poll while the tab is actually on screen — this runs alongside the
  // 200 ms status poll and there is no point competing with it when hidden.
  const panel = document.getElementById("tab-canlog");
  if (!panel || !panel.classList.contains("active")) return;
  try {
    const r = await (await fetch("/api/canlog?since=" + canLogSince)).json();
    if (!r) return;
    if (!Array.isArray(r.entries) || r.entries.length === 0) {
      canLogSince = r.writeIndex != null ? r.writeIndex : canLogSince;
      return;
    }
    const view = document.getElementById("canLogView");
    if (!view) { canLogSince = r.writeIndex; return; }
    const auto = document.getElementById("canLogAutoScroll");
    const lines = r.entries.map((e) => {
      const ts = (e.ms / 1000).toFixed(2);
      const id = Number(e.id).toString(16).toUpperCase().padStart(3, "0");
      return `[${ts}] 0x${id}  ${e.d}`;
    });
    view.textContent += lines.join("\n") + "\n";
    if (view.textContent.length > 60000) {
      view.textContent = view.textContent.slice(-40000);
    }
    if (auto && auto.checked) view.scrollTop = view.scrollHeight;
    canLogSince = r.writeIndex;
  } catch (e) {}
}

