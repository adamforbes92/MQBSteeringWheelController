/*
  ota.js — shared OTA tab for the Forbes Automotive ESP32 projects (v2, 2026-09-20)
  --------------------------------------------------------------------------------
  Drives the whole OTA tab from the standard markup (see _SharedTheme/ota_tab.html)
  against the routes registered by ota_manager.cpp and wifi_manager.cpp:

    /api/ota/info   /api/ota/check   /api/ota/fsinfo   POST /api/ota/fs   POST /api/ota
    /api/wifi/sta   /api/wifi/sta/reset   /api/wifi/scan

  Three cards:
    1. Update from GitHub - the page (not the controller) fetches
       <releasesDir>/releases.json and the .bin files from the repo named by
       /api/ota/info, then pushes web UI then firmware through the same
       safety-gated routes as a manual upload. Needs internet IN THIS BROWSER
       while it can still reach the controller - normally: controller joined to
       the home router (Home WiFi card), phone on the same network.
    2. Home WiFi (bridge mode) - how the phone gets that internet.
    3. Update from Files - littlefs.bin then firmware.bin by hand, no internet.

  Rules baked in (learned the hard way on OpenHaldex):
    - every upload sends ?size= so the device can reject a short body;
    - a filesystem failure is reported as "partition cleared, retry / recovery
      page", because the device wipes a rejected image rather than keep half;
    - the Check button always shows what stage it is in and how long it took.

  Ported from OpenHaldex-C6 9.00 (initUpdateCheck / initOtaPage / initWifiSta),
  ids kept the same. Plain ES2017, no dependencies. Load after app.js.
*/
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);

  // ---------------------------------------------------------------------------
  // Device info + release-channel config
  // ---------------------------------------------------------------------------
  let cfg = { repo: "", branch: "main", releasesDir: "Releases", product: "", version: "" };
  let installedVersion = "";
  let mirrors = [];
  let dirApi = "";
  let releasesBase = "";

  const set = (id, v) => { const e = $(id); if (e) e.textContent = (v === undefined || v === null || v === "") ? "--" : v; };

  function applyCfg(info) {
    cfg.repo = info.repo || "";
    cfg.branch = info.branch || "main";
    cfg.releasesDir = (info.releasesDir || "Releases").replace(/^\/+|\/+$/g, "");
    cfg.product = info.product || "";
    cfg.version = info.version || "";
    installedVersion = cfg.version;
    mirrors = cfg.repo ? [
      { name: "GitHub", base: "https://raw.githubusercontent.com/" + cfg.repo + "/" + cfg.branch + "/" + cfg.releasesDir + "/" },
      { name: "jsDelivr", base: "https://cdn.jsdelivr.net/gh/" + cfg.repo + "@" + cfg.branch + "/" + cfg.releasesDir + "/" },
    ] : [];
    dirApi = cfg.repo ? "https://api.github.com/repos/" + cfg.repo + "/contents/" + cfg.releasesDir + "?ref=" + cfg.branch : "";
    releasesBase = mirrors.length ? mirrors[0].base : "";
  }

  function fetchJson(url, opts) {
    return fetch(url, opts).then((r) => r.ok ? r.json() : null).catch(() => null);
  }

  // fetch() with a timeout - a phone that has silently lost its route can
  // otherwise hang for a minute before the browser gives up.
  function tFetch(url, ms, opts) {
    const ctrl = typeof AbortController === "function" ? new AbortController() : null;
    const t = ctrl ? setTimeout(() => ctrl.abort(), ms) : null;
    const o = Object.assign({ cache: "no-store" }, opts || {});
    if (ctrl) o.signal = ctrl.signal;
    return fetch(url, o).finally(() => { if (t) clearTimeout(t); });
  }

  // numeric compare of "x.yy.z" strings: >0 if a newer than b
  function cmpVer(a, b) {
    const pa = String(a || "").split(".").map((n) => parseInt(n, 10) || 0);
    const pb = String(b || "").split(".").map((n) => parseInt(n, 10) || 0);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
      const d = (pa[i] || 0) - (pb[i] || 0);
      if (d) return d;
    }
    return 0;
  }

  function loadInfo() {
    return fetchJson("/api/ota/info").then((i) => {
      if (!i) return null;
      applyCfg(i);
      set("otaFwVersion", i.version);
      set("otaFsVersion", i.fsVersion && i.fsVersion !== "--" ? i.fsVersion : (i.version ? i.version + " (assumed)" : "--"));
      set("otaBoard", i.board);
      set("otaPartition", i.partition);
      set("otaHardware", i.board);
      set("updInstalled", i.version ? "v" + i.version : "--");
      const repoEl = $("updRepo");
      if (repoEl) repoEl.textContent = cfg.repo ? cfg.repo + " / " + cfg.releasesDir : "no release channel configured";
      return i;
    });
  }

  // ---------------------------------------------------------------------------
  // Safety gate (polled): disables uploads while the device says no
  // ---------------------------------------------------------------------------
  let safeAllowed = true;
  function loadSafety() {
    return fetchJson("/api/ota/check").then((s) => {
      const el = $("otaSafe");
      if (!s) { if (el) { el.textContent = "Offline"; el.className = "upd-bad"; } return; }
      safeAllowed = !!s.allowed;
      if (el) { el.textContent = s.allowed ? "Ready" : "Blocked: " + (s.reason || ""); el.className = s.allowed ? "upd-current" : "upd-bad"; }
      const hint = $("otaSafeHint");
      if (hint) hint.textContent = s.allowed ? "" : "Updates are blocked while: " + (s.reason || "the device is busy") + ".";
    });
  }

  // ---------------------------------------------------------------------------
  // Upload core: POSTs a Blob to a safety-gated route. Resolves on 200, rejects
  // with the device's message otherwise. `size` lets the device spot a short
  // upload, which would otherwise leave half an image in the partition.
  // ---------------------------------------------------------------------------
  function uploadBlob(type, blob, filename, onProgress) {
    const isFs = type === "filesystem";
    const url = (isFs ? "/api/ota/fs" : "/api/ota") + "?size=" + blob.size;
    const data = new FormData();
    data.append(isFs ? "filesystem" : "firmware", blob, filename);
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", url);
      xhr.upload.addEventListener("progress", (e) => { if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total); });
      xhr.addEventListener("load", () => {
        let msg = "";
        try { msg = JSON.parse(xhr.responseText).message || ""; } catch (e) { msg = xhr.responseText || ""; }
        if (xhr.status === 200) resolve(msg);
        else reject(new Error(msg || ("Update failed (" + xhr.status + ")")));
      });
      xhr.addEventListener("error", () => reject(new Error("Upload failed - check the connection and retry.")));
      xhr.send(data);
    });
  }

  // ---------------------------------------------------------------------------
  // Card 3: Update from Files (manual, two steps)
  // ---------------------------------------------------------------------------
  function initManual() {
    const btn = $("otaUploadBtn"), typeSel = $("otaType"), fileIn = $("otaBin"), status = $("otaStatus");
    if (!btn || !typeSel || !fileIn) return;
    const KEY = "ota_steps_" + (location.hostname || "dev");
    const loadDone = () => { try { const raw = JSON.parse(localStorage.getItem(KEY) || "{}"); if (!raw.ts || Date.now() - raw.ts > 15 * 60000) return []; return Array.isArray(raw.done) ? raw.done : []; } catch (e) { return []; } };
    const saveDone = (done) => { try { localStorage.setItem(KEY, JSON.stringify({ done, ts: Date.now() })); } catch (e) { /* private mode */ } };
    const render = () => {
      const cur = typeSel.value, done = loadDone();
      document.querySelectorAll("#otaSteps .ota-step").forEach((el) => {
        const s = el.dataset.step;
        el.classList.toggle("done", done.includes(s));
        el.classList.toggle("active", s === cur && !done.includes(s));
      });
    };
    const markDone = (t) => { const d = loadDone(); if (!d.includes(t)) d.push(t); saveDone(d); render(); };
    const setStatus = (m, cls) => { if (status) { status.textContent = m; status.className = "status-line" + (cls ? " " + cls : ""); } };

    btn.addEventListener("click", () => {
      const type = typeSel.value || "firmware";
      if (!fileIn.files.length) { setStatus("Pick a .bin file first.", "error"); return; }
      const file = fileIn.files[0];
      if (!/\.bin$/i.test(file.name)) { setStatus("Please choose a .bin file.", "error"); return; }
      if (!safeAllowed) { setStatus("Blocked: the device is not in a safe state for an update.", "error"); return; }
      const wrap = $("otaProgressWrap"), bar = $("otaProgressBar"), label = $("otaProgressLabel");
      const pct = (f) => { const p = Math.round(f * 100); if (bar) bar.style.width = p + "%"; if (label) label.textContent = p + "%"; };
      btn.disabled = true;
      if (wrap) wrap.hidden = false;
      pct(0);
      setStatus("Uploading " + type + "…");
      uploadBlob(type, file, file.name, pct).then(() => {
        pct(1);
        markDone(type);
        if (type === "filesystem") {
          typeSel.value = "firmware";
          render();
          setStatus("Web UI installed. Now upload firmware.bin (or reload the page if only the UI changed).", "ok");
          if (wrap) wrap.hidden = true;
          btn.disabled = false;
          fileIn.value = "";
        } else {
          setStatus("Firmware installed. Device rebooting… reload the page in ~20 s.", "ok");
          waitForReboot().then((v) => { if (v) { setStatus("Back on v" + v + ". Reloading…", "ok"); setTimeout(() => location.reload(), 1500); } });
        }
      }).catch((err) => {
        let m = err.message;
        if (type === "filesystem") m += " The web UI partition has been cleared - upload littlefs.bin again. If this page won't reload, the device shows a recovery page at its address.";
        setStatus(m, "error");
        if (wrap) wrap.hidden = true;
        btn.disabled = false;
      });
    });
    typeSel.addEventListener("change", render);
    const done = loadDone();
    if (done.includes("filesystem") && !done.includes("firmware")) typeSel.value = "firmware";
    render();
  }

  // ---------------------------------------------------------------------------
  // Card 2: Home WiFi (bridge mode)
  // ---------------------------------------------------------------------------
  function initWifiSta() {
    const p = "otaWifiSta";
    const ssidInput = $(p + "SsidInput"), ssidList = $(p + "SsidList"), scanBtn = $(p + "Scan");
    const pwInput = $(p + "PasswordInput"), pwToggle = $(p + "PasswordToggle"), status = $(p + "Status");
    const btnSave = $(p + "Save"), btnReset = $(p + "Reset"), card = $("otaWifiStaCard");
    if (!ssidInput || !pwInput || !status || !btnSave || !btnReset) return;
    let userEditing = false;
    ssidInput.addEventListener("input", () => { userEditing = true; });
    pwInput.addEventListener("input", () => { userEditing = true; });
    const quality = (r) => (r >= -50 ? "excellent" : r >= -60 ? "good" : r >= -70 ? "fair" : "weak");
    const say = (msg, cls) => { status.textContent = msg; status.className = "status-line" + (cls ? " " + cls : ""); };

    function render(d) {
      if (d && d.enabled === false) { if (card) card.hidden = true; return; }
      if (!d || !d.ssid) say("Disabled - access point only");
      else if (d.connected) {
        const sig = typeof d.rssi === "number" ? " (" + quality(d.rssi) + " signal, " + d.rssi + " dBm)" : "";
        say("✓ Connected to “" + d.ssid + "”" + sig + " - reachable at http://" + d.ip + "/" + (d.mdns ? " and http://" + d.mdns + ".local/" : ""), "ok");
      } else say("Configured for “" + d.ssid + "” - not connected (out of range, wrong password, or still trying; it retries every 5 minutes)");
    }
    function refresh() { fetchJson("/api/wifi/sta").then((d) => { if (!d) return; if (!userEditing) ssidInput.value = d.ssid || ""; render(d); }); }
    refresh();
    setInterval(refresh, 5000);

    if (pwToggle) pwToggle.addEventListener("click", () => { const h = pwInput.type === "password"; pwInput.type = h ? "text" : "password"; pwToggle.textContent = h ? "🙈" : "👁"; });

    if (scanBtn) scanBtn.addEventListener("click", async () => {
      scanBtn.disabled = true;
      const prev = ssidInput.placeholder;
      ssidInput.placeholder = "Scanning…";
      let resp = null;
      for (let i = 0; i < 12; i++) {
        resp = await fetchJson("/api/wifi/scan");
        if (resp && !resp.scanning) break;
        await new Promise((r) => setTimeout(r, 700));
      }
      ssidInput.placeholder = prev;
      scanBtn.disabled = false;
      if (!resp || !Array.isArray(resp.networks)) { say("Scan failed - try again.", "error"); return; }
      if (ssidList) {
        ssidList.innerHTML = "";
        resp.networks.forEach((n) => { const o = document.createElement("option"); o.value = n.ssid; o.textContent = n.ssid + (n.secure ? " 🔒" : "") + " (" + n.rssi + " dBm)"; ssidList.appendChild(o); });
      }
      say(resp.networks.length + " network" + (resp.networks.length === 1 ? "" : "s") + " found - pick from the list");
      ssidInput.focus();
    });

    const post = (url, body) => fetch(url, { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: body ? new URLSearchParams(body).toString() : "" })
      .then((r) => r.json().catch(() => ({ ok: r.ok }))).catch(() => null);

    btnSave.addEventListener("click", async () => {
      const ssid = ssidInput.value.trim(), pwd = pwInput.value;
      if (ssid.length > 32) { say("Network name too long (max 32).", "error"); return; }
      if (pwd.length > 0 && pwd.length < 8) { say("Password must be at least 8 characters, or blank for an open network.", "error"); return; }
      const resp = await post("/api/wifi/sta", { ssid: ssid, password: pwd });
      if (!resp) { say("Failed to reach the device.", "error"); return; }
      if (!resp.ok) { say(resp.error || "Failed to save.", "error"); return; }
      userEditing = false;
      pwInput.value = "";
      if (ssid) say("Saved - connecting to “" + ssid + "”… (the access point restarts: reconnect if you drop off)");
      else say("Disabled - access point only");
    });

    btnReset.addEventListener("click", async () => {
      const resp = await post("/api/wifi/sta/reset");
      if (!resp || !resp.ok) { say("Failed to disable.", "error"); return; }
      userEditing = false;
      ssidInput.value = "";
      pwInput.value = "";
      say("Disabled - access point only");
    });
  }

  // ---------------------------------------------------------------------------
  // Card 1: Update from GitHub (guided)
  // ---------------------------------------------------------------------------
  const FOLDER_RE = /^V(\d+(?:\.\d+)*)$/i;

  async function waitForReboot(onPct) {
    const t0 = Date.now();
    await new Promise((r) => setTimeout(r, 4000));
    while (Date.now() - t0 < 90000) {
      if (onPct) onPct((Date.now() - t0) / 90000);
      try {
        const res = await tFetch("/api/ota/info", 3000);
        if (res.ok) { const i = await res.json(); return i.version || ""; }
      } catch (e) { /* still rebooting */ }
      await new Promise((r) => setTimeout(r, 2000));
    }
    return null;
  }

  function initGuided() {
    const checkBtn = $("updCheckBtn"), installBtn = $("updInstallBtn"), showAll = $("updShowAll"), bridgeBtn = $("updBridgeBtn");
    const picker = $("updPicker"), sel = $("updVersion"), notes = $("updNotes"), status = $("updStatus");
    const wrap = $("updProgressWrap"), bar = $("updProgressBar"), label = $("updProgressLabel");
    if (!checkBtn || !sel) return;

    let index = null, busy = false;
    const setStatus = (m, cls) => { if (status) { status.textContent = m; status.className = "status-line" + (cls ? " " + cls : ""); } };
    const setState = (m, cls) => { const e = $("updState"); if (e) { e.textContent = m; e.className = cls || ""; } };
    const setPct = (f, text) => { const p = Math.max(0, Math.min(100, Math.round(f * 100))); if (bar) bar.style.width = p + "%"; if (label) label.textContent = text ? text + " " + p + "%" : p + "%"; };
    const setStep = (id, state) => document.querySelectorAll("#updSteps .ota-step").forEach((el) => { if (el.dataset.step === id) { el.classList.toggle("active", state === "active"); el.classList.toggle("done", state === "done"); } });
    const resetSteps = () => document.querySelectorAll("#updSteps .ota-step").forEach((el) => el.classList.remove("active", "done"));
    const goBridge = (ev) => { if (ev) ev.preventDefault(); const c = $("otaWifiStaCard"); if (c) { c.scrollIntoView({ behavior: "smooth", block: "start" }); const i = $("otaWifiStaSsidInput"); if (i) setTimeout(() => i.focus(), 400); } };

    const candidates = () => (index && Array.isArray(index.releases)) ? index.releases.filter((r) => r && r.ota && r.firmware && r.filesystem) : [];
    const selected = () => candidates().find((r) => r.version === sel.value) || null;

    function renderNotes() {
      const r = selected();
      if (notes) notes.textContent = r ? ((r.date ? r.date + " — " : "") + (r.notes || "")) : "";
      if (installBtn) installBtn.textContent = r && cmpVer(r.version, installedVersion) < 0 ? "Roll back to v" + r.version : "Install v" + (r ? r.version : "");
    }

    function renderPicker() {
      const all = showAll && showAll.checked;
      const list = candidates().filter((r) => all || (r.channel !== "beta" && cmpVer(r.version, installedVersion) >= 0));
      list.sort((a, b) => cmpVer(b.version, a.version));
      sel.innerHTML = "";
      list.forEach((r) => {
        const o = document.createElement("option");
        o.value = r.version;
        const tags = [];
        if (r.version === index.latest) tags.push("latest");
        if (r.channel === "beta") tags.push("beta");
        if (r.unindexed) tags.push("not in index");
        const c = cmpVer(r.version, installedVersion);
        if (c === 0) tags.push("installed"); else if (c < 0) tags.push("rollback");
        o.textContent = "v" + r.version + (tags.length ? " (" + tags.join(", ") + ")" : "");
        sel.appendChild(o);
      });
      picker.hidden = list.length === 0;
      if (!list.length) {
        if (!all && candidates().length) setStatus("Nothing newer than the installed version. Tick “Show beta / older versions” to roll back.");
        else setStatus("No installable releases listed. Use “Update from Files” below.", "error");
        return;
      }
      sel.value = list.some((r) => r.version === index.latest) ? index.latest : list[0].version;
      renderNotes();
    }

    // Fold the GitHub folder listing into the index: a folder the index doesn't
    // know is offered anyway (no notes); an index entry with no folder is
    // dropped since its downloads would 404.
    function mergeSources(idx, dirs) {
      const byVer = {};
      if (idx && Array.isArray(idx.releases)) idx.releases.forEach((r) => { if (r && r.version) byVer[r.version] = r; });
      if (dirs) {
        dirs.forEach((v) => { if (!byVer[v]) byVer[v] = { version: v, channel: "stable", ota: true, unindexed: true, notes: "Not in the release index yet - no release notes.", firmware: { path: "V" + v + "/firmware.bin" }, filesystem: { path: "V" + v + "/littlefs.bin" } }; });
        Object.keys(byVer).forEach((v) => { if (dirs.indexOf(v) < 0) delete byVer[v]; });
      }
      const releases = Object.keys(byVer).map((v) => byVer[v]).sort((a, b) => cmpVer(b.version, a.version));
      const stable = releases.filter((r) => r.ota && r.firmware && r.filesystem && r.channel !== "beta");
      return { releases, latest: stable.length ? stable[0].version : (releases.length ? releases[0].version : "") };
    }

    async function fetchIndex() {
      let reached = "", netErr = "";
      for (const m of mirrors) {
        let res = null;
        try { res = await tFetch(m.base + "releases.json", 12000, { mode: "cors" }); }
        catch (e) { netErr = m.name + ": " + (e && e.name === "AbortError" ? "timed out" : (e && e.message ? e.message : "unreachable")); continue; }
        if (!res.ok) { reached = m.name + " answered HTTP " + res.status; continue; }
        try { const idx = await res.json(); releasesBase = m.base; return { index: idx }; }
        catch (e) { reached = m.name + " answered HTTP 200 but the release index is not valid JSON"; }
      }
      return reached ? { reached } : { netErr };
    }

    async function fetchFolders() {
      try {
        const res = await tFetch(dirApi, 12000, { mode: "cors", headers: { Accept: "application/vnd.github+json" } });
        if (!res.ok) return { reached: "GitHub API answered HTTP " + res.status };
        const arr = await res.json();
        if (!Array.isArray(arr)) return { reached: "GitHub API returned an unexpected listing" };
        const dirs = [];
        arr.forEach((e) => { const m = e && e.type === "dir" && FOLDER_RE.exec(e.name || ""); if (m) dirs.push(m[1]); });
        return { dirs };
      } catch (e) { return { netErr: "GitHub API: " + (e && e.name === "AbortError" ? "timed out" : (e && e.message ? e.message : "unreachable")) }; }
    }

    async function offlineAdvice(detail) {
      const sta = await fetchJson("/api/wifi/sta");
      const why = detail ? " (" + detail + ")" : "";
      if (sta && sta.ssid && sta.connected) return ["This browser has no internet" + why + ". The controller is already on “" + sta.ssid + "” at http://" + sta.ip + "/ - join this phone to “" + sta.ssid + "”, open http://" + sta.ip + "/" + (sta.mdns ? " (or http://" + sta.mdns + ".local/)" : "") + ", come back to this tab and press Retry.", false];
      if (sta && sta.ssid) return ["This browser has no internet" + why + ". The controller is set up for “" + sta.ssid + "” but isn't connected right now - out of range, wrong password, or still trying (it retries every 5 minutes). Check the Home WiFi card below (Save & Apply reconnects straight away), then join this phone to the same network, open the address the card shows and press Retry.", true];
      return ["This browser has no internet while on the device's own WiFi" + why + ". Connect the controller to your home router in the Home WiFi card below, join this phone to that same network, open the address the card shows and press Retry. No router available? Use “Update from Files” below - it needs no internet here.", true];
    }

    async function check() {
      if (busy) return;
      const t0 = Date.now();
      const secs = () => ((Date.now() - t0) / 1000).toFixed(1) + " s";
      checkBtn.disabled = true;
      checkBtn.textContent = "Checking…";
      if (bridgeBtn) bridgeBtn.hidden = true;
      picker.hidden = true;
      index = null;
      const finish = (needBridge) => { checkBtn.textContent = needBridge === undefined ? "Check for updates" : "Retry"; if (bridgeBtn) bridgeBtn.hidden = !needBridge; checkBtn.disabled = false; };

      setStatus("1/2 Contacting the controller…");
      setState("Checking…");
      let info = null;
      try { const res = await tFetch("/api/ota/info", 6000); if (res.ok) info = await res.json(); } catch (e) { /* below */ }
      if (!info || !info.version) {
        setState("Controller unreachable", "upd-bad");
        setStatus("Can't reach the controller from this browser (gave up after " + secs() + "). Stay on its WiFi - or, if you're using the home router, make sure the Home WiFi card shows Connected and that you opened this page at the address it gives. Then press Retry.", "error");
        finish(false);
        return;
      }
      applyCfg(info);
      set("updInstalled", "v" + info.version);
      if (!mirrors.length) {
        setState("No release channel", "upd-bad");
        setStatus("This firmware has no GitHub release channel configured. Use “Update from Files” below.", "error");
        finish(false);
        return;
      }

      setStatus("2/2 Contacting GitHub for the release list… (controller answered in " + secs() + "; this can take up to 30 s with no internet)");
      const [ir, fr] = await Promise.all([fetchIndex(), fetchFolders()]);
      if (!ir.index && !fr.dirs) {
        if (ir.reached || fr.reached) {
          setState("Release list unavailable", "upd-bad");
          setStatus("The phone is online but the release list could not be read: " + (ir.reached || fr.reached) + ". Nothing is wrong with the controller or the phone - nothing is published for this product yet, or the published files are broken. Use “Update from Files” below.", "error");
          finish(false);
        } else {
          setState("No internet access", "upd-bad");
          const adv = await offlineAdvice((ir.netErr || fr.netErr || "") + ", after " + secs());
          setStatus(adv[0], "error");
          finish(adv[1]);
        }
        return;
      }

      index = mergeSources(ir.index, fr.dirs);
      const latest = index.latest || "";
      set("updLatest", latest ? "v" + latest : "--");
      const c = cmpVer(latest, installedVersion);
      const srcNote = !ir.index ? " (release index unavailable - folder listing only, no release notes)" : !fr.dirs ? " (folder listing unavailable - " + (fr.reached || fr.netErr || "no answer") + "; showing the index only)" : "";
      const via = " Release list from " + mirrors.filter((m) => m.base === releasesBase).map((m) => m.name).join("") + " in " + secs() + ".";
      if (c > 0) { setState("Update available", "upd-available"); setStatus("v" + latest + " is available (installed v" + installedVersion + ")." + srcNote + via); }
      else if (c === 0) { setState("Up to date", "upd-current"); setStatus("You are on the latest release." + srcNote + via); }
      else { setState("Ahead of release", "upd-current"); setStatus("Installed v" + installedVersion + " is newer than the published v" + latest + "." + srcNote + via); }
      renderPicker();
      finish();
    }

    // streamed download with progress; returns a Blob
    async function download(rel, part, stepId) {
      const info = rel[part];
      const url = /^https?:\/\//i.test(info.path) ? info.path : releasesBase + info.path;
      setStep(stepId, "active");
      setStatus("Downloading " + part + " (v" + rel.version + ")…");
      const res = await fetch(url, { cache: "no-store", mode: "cors" });
      if (!res.ok) throw new Error("Download failed: HTTP " + res.status + " for " + info.path);
      const cl = parseInt(res.headers.get("content-length") || "0", 10) || 0;
      const total = cl || info.size || 0;
      const chunks = [];
      let got = 0;
      if (res.body && res.body.getReader) {
        const reader = res.body.getReader();
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          chunks.push(value);
          got += value.length;
          if (total) setPct(got / total, "Download");
        }
      } else {
        const buf = await res.arrayBuffer();
        chunks.push(new Uint8Array(buf));
        got = buf.byteLength;
      }
      if (cl && got !== cl) throw new Error(part + " download was cut short (" + got + " of " + cl + " bytes).");
      if (!got) throw new Error(part + " download was empty.");
      setPct(1, "Download");
      setStep(stepId, "done");
      return new Blob(chunks, { type: "application/octet-stream" });
    }

    async function flash(blob, type, stepId) {
      setStep(stepId, "active");
      setStatus("Flashing " + type + "… do not power off.");
      setPct(0, "Flash");
      await uploadBlob(type, blob, type === "filesystem" ? "littlefs.bin" : "firmware.bin", (f) => setPct(f, "Flash"));
      setPct(1, "Flash");
      setStep(stepId, "done");
    }

    async function runInstall(rel) {
      busy = true;
      checkBtn.disabled = true;
      if (installBtn) installBtn.disabled = true;
      sel.disabled = true;
      resetSteps();
      if (wrap) wrap.hidden = false;
      setPct(0);
      let stage = "check";
      try {
        const safe = await fetchJson("/api/ota/check");
        if (!safe || !safe.allowed) throw new Error("Blocked: " + ((safe && safe.reason) || "device not in a safe state for an update."));

        stage = "dlfs";
        const fsBlob = await download(rel, "filesystem", "dlfs");
        stage = "fs";
        await flash(fsBlob, "filesystem", "fs");

        stage = "verify";
        setStep("verify", "active");
        setStatus("Verifying web UI…");
        const fsi = await fetchJson("/api/ota/fsinfo");
        if (!fsi || !fsi.ok) throw new Error("Web UI verification failed (" + ((fsi && fsi.error) || "not mounted") + "). Retry the update.");
        if (fsi.fsVersion && fsi.fsVersion !== "--" && fsi.fsVersion !== rel.version) throw new Error("Web UI reports v" + fsi.fsVersion + ", expected v" + rel.version + ". Retry the update.");
        setStep("verify", "done");

        stage = "dlfw";
        const fwBlob = await download(rel, "firmware", "dlfw");
        stage = "fw";
        await flash(fwBlob, "firmware", "fw");

        setStep("reboot", "active");
        setStatus("Device rebooting… waiting for it to come back.");
        const v = await waitForReboot((f) => setPct(f, "Reboot"));
        if (v === null) { setStatus("Device didn't respond within 90 s. Reconnect to its WiFi (or the home network) and reload this page.", "error"); }
        else {
          setStep("reboot", "done");
          setPct(1, "Done");
          installedVersion = v;
          set("updInstalled", "v" + v);
          set("otaFwVersion", v);
          if (v === rel.version) { setState("Installed v" + v, "upd-current"); setStatus("Update complete: now running v" + v + ". Reloading the page…", "ok"); setTimeout(() => location.reload(), 2500); }
          else { setState("Rolled back", "upd-bad"); setStatus("Device came back on v" + v + " instead of v" + rel.version + " - the new image was rejected or rolled back. Try again or use “Update from Files”.", "error"); }
        }
      } catch (e) {
        let msg = e.message;
        if (stage === "fs" || stage === "verify") msg += " The controller is still running v" + installedVersion + "; the web UI partition was cleared. Press Install again (or upload littlefs.bin under “Update from Files”). If this page won't load, the controller now shows a recovery page at its address.";
        setStatus(msg, "error");
        if (wrap) wrap.hidden = true;
      }
      busy = false;
      checkBtn.disabled = false;
      if (installBtn) installBtn.disabled = false;
      sel.disabled = false;
    }

    function install() {
      const rel = selected();
      if (!rel || busy) return;
      const dir = cmpVer(rel.version, installedVersion);
      const what = dir < 0 ? "roll back to v" + rel.version : (dir === 0 ? "re-install v" + rel.version : "update to v" + rel.version);
      let msg = "This will " + what + " (currently v" + installedVersion + ").\n\nThe web UI is replaced first, then the firmware, then the device reboots. Keep this page open.";
      if (dir < 0) msg += "\n\nRolling back: older releases may not have this update page, so coming forward again could mean a USB flash. Settings may also be reset.";
      if (!confirm(msg + "\n\nContinue?")) return;
      runInstall(rel);
    }

    checkBtn.addEventListener("click", check);
    if (bridgeBtn) bridgeBtn.addEventListener("click", goBridge);
    const goLink = $("updGoBridge");
    if (goLink) goLink.addEventListener("click", goBridge);
    if (installBtn) installBtn.addEventListener("click", install);
    sel.addEventListener("change", renderNotes);
    if (showAll) showAll.addEventListener("change", () => { if (index) renderPicker(); });
  }

  // ---------------------------------------------------------------------------
  function init() {
    if (!$("otaUploadBtn") && !$("updCheckBtn")) return; // page has no OTA tab
    loadInfo();
    loadSafety();
    setInterval(loadSafety, 3000);
    initManual();
    initWifiSta();
    initGuided();
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
