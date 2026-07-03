document.addEventListener("DOMContentLoaded", initApp);

let mappings = [];
let pollTimer = null;
let localLearnUntil = 0;
let localLearnRow = -1;
let localLearnTarget = 0;

function initApp() {
  initTabs();
  wireControls();
  loadSetup();
  loadStatus();
  pollTimer = setInterval(loadStatus, 200);
}

function initTabs() {
  const tabs = document.querySelectorAll(".tab");
  tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
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
    });
    renderMappings();
  });

  document.getElementById("saveSetupBtn").addEventListener("click", saveSetup);
  document.getElementById("otaUploadBtn").addEventListener("click", uploadOta);
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
  ["canBroadcastEnabled", "paddlesEnabled", "linOutputEnabled"].forEach((id) => {
    document.getElementById(id).addEventListener("change", saveSetup);
  });

  // Force brightness slider.
  const pctSlider = document.getElementById("forceBacklightPercent");
  const pctDisplay = document.getElementById("forceBacklightPctDisplay");
  pctSlider.addEventListener("input", () => { pctDisplay.textContent = pctSlider.value; });
  pctSlider.addEventListener("change", saveSetup);

  // Diagnostic: PNP toggle.
  document.getElementById("diagPnpActive").addEventListener("change", async function () {
    await fetch("/api/diag/pnp", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ active: this.checked }),
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
    document.getElementById("linOutputEnabled").checked = !!setup.linOutputEnabled;
    document.getElementById("linOutputId").value = hex2(setup.linOutputId != null ? setup.linOutputId : 0x0E);
    document.getElementById("auxDimDuty").value    = Number((setup.auxDimDuty    != null ? setup.auxDimDuty    : 197) / 10).toFixed(1);
    document.getElementById("auxBrightDuty").value = Number((setup.auxBrightDuty != null ? setup.auxBrightDuty : 980) / 10).toFixed(1);

    mappings = Array.isArray(setup.mappings) ? setup.mappings : [];
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

    setText("canStatusBadge", s.canEnabled ? (s.canHealthy ? "CAN: Healthy" : "CAN: No Data") : "CAN: Off");
    setText("buttonIncoming", s.buttonIncoming ? toHexId(s.buttonIncoming) : "--");
    setText("pressedButtonName", s.pressedButtonName || "--");
    setText("incomingLinData", s.incomingLinData || "--");
    setText("outgoingLinData", s.outgoingLinData || "--");
    setText("outgoingCanId", toHexId(s.outgoingCanId));
    setText("outgoingCanData", s.outgoingCanData || "--");
    renderCanBytes(Array.isArray(s.canBytes) ? s.canBytes : [], s.canBroadcastId);
    setText("backlightSource", s.backlightSource || "--");
    setText("backlightState", s.backlightState || (s.backlightOn ? "ON" : "OFF"));
    setText("backlightPercent", `${Number(s.backlightPercent || 0)}%`);
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
    if (s.hasResistiveStereo) {
      setStatusValue("resistiveHealthState", `${Number(s.resistiveOhmNow || 0)} \u03A9`, true);
    } else {
      setStatusValue("resistiveHealthState", "Disabled", false);
    }

    const btnLabel = s.pressedButtonName
      ? `${s.pressedButtonName} (${toHexId(s.latestButtonId)})`
      : toHexId(s.latestButtonId || 0);
    setText("liveButtonBadge", `Last Button: ${btnLabel}`);

    if (s.learnActive) {
      const sec = Math.ceil((s.learnMsRemaining || 0) / 1000);
      setText("learnBadge", `Learn: Active (${sec}s)`);
    } else {
      setText("learnBadge", "Learn: Idle");
    }

    if (s.FW_VERSION) {
      const fwStr = `FW: ${s.FW_VERSION}`;
      setText("fwVersion", fwStr);
      const otaEl = document.getElementById("fwVersionOta");
      if (otaEl) otaEl.textContent = fwStr;
    }

    updateLearnBannerFromStatus(s);
  } catch (err) {
    setText("learnBadge", "Learn: Status Error");
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

    const byteIdx = (typeof row.canByteIndex === "number" && row.canByteIndex < 8) ? row.canByteIndex : 255;
    const bitIdx = clamp(num(row.canBitIndex), 0, 7);
    const byteOptions = [255, 0, 1, 2, 3, 4, 5, 6, 7]
      .map((b) => `<option value="${b}"${byteIdx === b ? " selected" : ""}>${b === 255 ? "\u2014" : b}</option>`)
      .join("");

    tr.innerHTML = `
      <td><input data-field="name" data-idx="${idx}" type="text" value="${escapeAttr(row.name || "")}"></td>
      <td><input data-field="oldButtonId" data-idx="${idx}" type="number" min="0" max="255" value="${num(row.oldButtonId)}"></td>
      <td><button class="btn tiny secondary" data-learn="old" data-idx="${idx}">Learn</button></td>
      <td><input data-field="newLinButtonId" data-idx="${idx}" type="number" min="0" max="255" value="${num(row.newLinButtonId)}"></td>
      <td><button class="btn tiny secondary" data-learn="lin" data-idx="${idx}">Learn</button></td>
      <td><select data-field="canByteIndex" data-idx="${idx}">${byteOptions}</select></td>
      <td><input data-field="canBitIndex" data-idx="${idx}" type="number" min="0" max="7" value="${bitIdx}"></td>
      <td><input data-field="resistiveOhm" data-idx="${idx}" type="number" min="0" max="20000" value="${num(row.resistiveOhm)}"></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="pnp" data-idx="${idx}" type="checkbox" title="Activate PNP output when pressed" ${(num(row.flags) & 1) ? "checked" : ""}></td>
      <td style="text-align:center"><input data-field="flags" data-subfield="latch" data-idx="${idx}" type="checkbox" title="Latch PNP: press to latch on, press again to unlatch" ${(num(row.flags) & 2) ? "checked" : ""}></td>
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
}

function onEditRow(event) {
  const idx = Number(event.target.dataset.idx);
  const field = event.target.dataset.field;

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
    setTimeout(loadSetup, 700);
    setTimeout(loadSetup, 1500);
    setTimeout(loadSetup, 2500);
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
    linOutputEnabled: document.getElementById("linOutputEnabled").checked,
    linOutputId: parseInt(document.getElementById("linOutputId").value, 16) || 0x0E,
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
    })),
  };

  try {
    const res = await fetch("/api/setup/save", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });

    if (!res.ok) {
      throw new Error("Save failed");
    }

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

async function uploadOta() {
  const fileInput = document.getElementById("otaBin");
  const status = document.getElementById("otaStatus");

  if (!fileInput.files || fileInput.files.length === 0) {
    status.textContent = "Pick a .bin file first.";
    return;
  }

  const file = fileInput.files[0];
  const data = new FormData();
  data.append("firmware", file, file.name);

  status.textContent = "Uploading...";

  try {
    const res = await fetch("/api/ota", {
      method: "POST",
      body: data,
    });

    if (!res.ok) {
      throw new Error("Upload failed");
    }

    status.textContent = "Upload complete, device rebooting.";
  } catch (err) {
    status.textContent = `OTA error: ${err.message}`;
  }
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
