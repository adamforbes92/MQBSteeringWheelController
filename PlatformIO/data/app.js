document.addEventListener("DOMContentLoaded", initApp);

let mappings = [];
let latchedRows = new Set();  // indices of rows whose latch is engaged, from /api/status
let seenMappingsRevision = null;  // firmware's counter of Learn results; see mergeLearnedCodes()
let rollerCodes = new Set();  // button codes the firmware has seen behave as a scroll wheel

// Returns true when the set actually changed, so callers only redraw then.
function setRollerCodes(list) {
  const next = new Set(Array.isArray(list) ? list.map(Number) : []);
  const changed = next.size !== rollerCodes.size || [...next].some((c) => !rollerCodes.has(c));
  rollerCodes = next;
  return changed;
}

// Unsaved-changes tracking for the button table. Settings controls save
// themselves on change; the table does not — it needs Save Setup, which sits
// below a wide table and is easy to walk away from. So: mark the table dirty on
// any edit, show it, and ask before leaving the tab or the page.
let mappingsDirty = false;
let setupLoaded = false;  // the first /api/setup load has filled the controls

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
    setRollerCodes(setup.rollerCodes);
    let changed = false;
    fresh.forEach((f, i) => {
      const m = mappings[i];
      if (!m) return;
      for (const k of ["oldButtonId", "newLinButtonId", "sourceByte"]) {
        if (num(m[k]) !== num(f[k])) { m[k] = f[k]; changed = true; }
      }
      // Learn also sets the trigger when it recognises a scroll wheel. Only
      // taken for the row being learned, so an unsaved Trigger edit on any
      // other row is not reverted.
      if (i === localLearnRow && localLearnTarget === 1) {
        const trig = num(f.flags) & TRIGGER_MASK;
        if ((num(m.flags) & TRIGGER_MASK) !== trig) {
          m.flags = (num(m.flags) & ~TRIGGER_MASK) | trig;
          changed = true;
        }
      }
    });
    if (changed) {
      // Not marked dirty: the learned value already lives in the firmware,
      // which persists it on its own 5 s cycle. Only browser-side edits need
      // Save Setup, and the Trigger change that usually follows will flag it.
      renderMappings();
      setStatus("Learned code applied.");
    }
    // A Learn that turned out to be a scroll wheel carries on in the wizard —
    // once per Learn, whether or not the code differed from what was there,
    // and only if it actually landed (a timed-out Learn leaves the revision).
    const learnedRow = mappings[localLearnRow];
    if (learnMayBeScroll && localLearnTarget === 1 && learnedRow && rowIsRoller(learnedRow) &&
        seenMappingsRevision !== learnStartRevision) {
      openScrollWizard(localLearnRow);
    }
  } catch (e) {}
}

// ---- Scroll wheels ---------------------------------------------------------
// A roller sends one code whichever way it turns, so it needs two rows: one
// Scroll up, one Scroll down. The firmware marks a code as a roller the first
// time it moves in a way no button can (see isRollerDelta() in globals.h);
// these helpers use that to flag rows, and the wizard below adds the rows.

function defaultButtonByte() {
  const v = num(document.getElementById("linButtonByteIndex").value);
  return v >= 0 && v < 8 ? v : 1;
}

function rowIsRoller(row) {
  const sb = num(row.sourceByte);
  return num(row.oldButtonId) !== 0 && rollerCodes.has(num(row.oldButtonId)) &&
    (sb === 0 || sb === defaultButtonByte());
}

// Direction a row is filtered to: FLAG_ROTARY_UP, FLAG_ROTARY_DOWN, or 0 for
// "either" (neither bit, or both).
function rowDirection(row) {
  const bits = num(row.flags) & (FLAG_ROTARY_UP | FLAG_ROTARY_DOWN);
  return bits === FLAG_ROTARY_UP || bits === FLAG_ROTARY_DOWN ? bits : 0;
}

function hasRowFor(code, dir, exceptIdx) {
  return mappings.some((m, i) => i !== exceptIdx && num(m.oldButtonId) === code &&
    rowIsRoller(m) && rowDirection(m) === dir);
}

// What a roller row still needs: "split" (fires either way: make it Scroll up
// and add a Scroll down row), "add" (one direction, partner missing), or null.
function rollerFix(idx) {
  const row = mappings[idx];
  if (!row || !rowIsRoller(row)) return null;
  const dir = rowDirection(row);
  if (dir === 0) return { kind: "split" };
  const other = dir === FLAG_ROTARY_UP ? FLAG_ROTARY_DOWN : FLAG_ROTARY_UP;
  if (hasRowFor(num(row.oldButtonId), other, idx)) return null;
  return { kind: "add", dir: other };
}

function dirWord(dir) {
  return dir === FLAG_ROTARY_UP ? "up" : "down";
}

const MAX_MAPPING_ROWS = 24;  // kMaxButtonMappings in defs.h

function hexCode(c) {
  return "0x" + num(c).toString(16).toUpperCase().padStart(2, "0");
}

// ---- Scroll-wheel wizard ---------------------------------------------------
// Adding the partner row while the user was still mid-Learn shifted row
// indices under the firmware, which addresses Learn by index, and the new row
// often did not take. So the wizard learns every movement first with probe
// Learns (target 4: the firmware reports what it saw and writes no row), and
// only then replaces the row with one row per movement, in a single save.

const LEARN_PROBE = 4;
const PROBE_WINDOW_MS = 10000;  // kLearnProbeWindowMs in defs.h
const WIZ_PROMPT = {
  up: "Scroll the wheel UP one click",
  down: "Scroll the wheel DOWN one click",
  press: "Press the wheel in",
};

let wiz = null;           // open wizard, null when closed
let lastProbeSeq = null;  // firmware's learnProbeSeq as of the last status poll

// opts.fromFit: opened by the Fit a new wheel wizard, which carries on with
// its next button once this closes (see fitAfterScrollWizard()).
function openScrollWizard(idx, opts = {}) {
  const row = mappings[idx];
  if (!row || wiz) return;
  // The wizard owns this row from here: stop the Learn backstop merging into
  // it once the rows have been replaced.
  clearTimeout(learnBackstopTimer);
  learnMayBeScroll = false;
  localLearnRow = -1;
  localLearnUntil = 0;

  const dir = rowDirection(row);
  wiz = {
    idx,
    row: { ...row },
    code: num(row.oldButtonId),
    // The movement the row was just learned with counts — no need to repeat it.
    up: dir === FLAG_ROTARY_UP,
    down: dir === FLAG_ROTARY_DOWN,
    press: null,
    pressSkipped: false,
    armed: null,
    until: 0,
    baseSeq: null,
    note: "",
    fromFit: !!opts.fromFit,
  };
  const name = String(row.name || "").replace(/\s+(up|down|press)$/i, "");
  document.getElementById("wizName").value =
    (!name || name === "New Button" ? "Scroll" : name).slice(0, 17);
  document.getElementById("wizIntro").textContent = wiz.fromFit
    ? `That was a scroll wheel (${hexCode(wiz.code)}), so "${row.name}" needs one row per movement. ` +
      `Teach it each movement below, then press Add rows. Cancel keeps "${row.name}" on the scroll wheel as one row.`
    : `Row ${idx + 1} is on scroll wheel ${hexCode(wiz.code)}. Teach it each movement below. ` +
      `Nothing changes until you press Add rows, which replaces row ${idx + 1} with one row per movement.`;
  document.getElementById("scrollWizard").hidden = false;
  renderWizard();
  armNextWizardStep();
}

// finished: true when called by finishScrollWizard(), which tells the fit
// wizard itself once the rows are in. Any other close is a cancel.
function closeScrollWizard(finished = false) {
  if (wiz && wiz.armed) fetch("/api/learn/cancel", { method: "POST" }).catch(() => {});
  const fromFit = wiz && wiz.fromFit;
  wiz = null;
  document.getElementById("scrollWizard").hidden = true;
  if (fromFit && !finished) fitAfterScrollWizard(null);
}

async function armWizardStep(step) {
  if (!wiz) return;
  try {
    const res = await fetch("/api/learn/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ rowIndex: 0, target: LEARN_PROBE }),
    });
    if (!res.ok) throw new Error("the device refused it — is the firmware up to date?");
  } catch (err) {
    if (wiz) {
      wiz.armed = null;
      wiz.note = `Couldn't start learning: ${err.message}`;
      renderWizard();
    }
    return;
  }
  if (!wiz) return;  // closed while the request was out
  wiz.armed = step;
  wiz.baseSeq = lastProbeSeq;
  wiz.until = Date.now() + PROBE_WINDOW_MS;
  renderWizard();
}

// Up, then down, then press; nothing armed once each is learned or skipped.
function armNextWizardStep() {
  const next = !wiz.up ? "up" : !wiz.down ? "down"
    : (!wiz.press && !wiz.pressSkipped) ? "press" : null;
  if (next) {
    armWizardStep(next);
  } else {
    wiz.armed = null;
    renderWizard();
  }
}

// Called from every status poll.
function updateWizardFromStatus(s) {
  if (typeof s.learnProbeSeq !== "number") return;
  lastProbeSeq = s.learnProbeSeq;
  if (!wiz || !wiz.armed) return;
  if (wiz.baseSeq === null) wiz.baseSeq = s.learnProbeSeq;

  if (s.learnProbeSeq !== wiz.baseSeq) {
    const step = wiz.armed;
    wiz.armed = null;
    onWizardCapture(step, num(s.learnProbeCode), num(s.learnProbeByte), num(s.learnProbeRotary));
    return;
  }
  if (!s.learnActive && Date.now() > wiz.until) {
    wiz.armed = null;
    wiz.note = "Nothing heard — press Learn to try again.";
  }
  renderWizard();
}

function onWizardCapture(step, code, byte, rotary) {
  wiz.note = "";
  if (step === "press") {
    if (code === wiz.code) {
      // Same code as the scroll: a press's first frame reads as one click up,
      // so a row for it would also fire on every Scroll up.
      wiz.press = null;
      wiz.pressSkipped = true;
      wiz.note = `The press sends the scroll code (${hexCode(code)}), so it can't have a row of its own. Skipped.`;
    } else {
      wiz.press = { code, byte };
    }
  } else if (code !== wiz.code) {
    wiz.note = `Heard ${hexCode(code)}, not the scroll wheel (${hexCode(wiz.code)}). Try again.`;
  } else if (rotary === 0) {
    wiz.note = "The wheel code came in without any movement. Try again.";
  } else {
    // Whichever way it actually turned is what gets learned; the step still
    // asked for stays armed below.
    const turned = rotary > 0 ? "up" : "down";
    wiz[turned] = true;
    if (turned !== step) {
      wiz.note = `That was a ${turned.toUpperCase()} scroll — kept as ${turned === "up" ? "Up" : "Down"}.`;
    }
  }
  renderWizard();
  armNextWizardStep();
}

function renderWizard() {
  if (!wiz) return;
  const learned = {
    up: wiz.up ? `Up Learned (${hexCode(wiz.code)})` : "Not learned",
    down: wiz.down ? `Down Learned (${hexCode(wiz.code)})` : "Not learned",
    press: wiz.press ? `Pressed Learned (${hexCode(wiz.press.code)})`
      : wiz.pressSkipped ? "Skipped" : "Not learned",
  };
  for (const step of ["up", "down", "press"]) {
    const el = document.querySelector(`.wiz-step[data-step="${step}"]`);
    el.classList.toggle("armed", wiz.armed === step);
    el.classList.toggle("done", !!wiz[step]);
    const id = `wizState${step[0].toUpperCase()}${step.slice(1)}`;
    setText(id, wiz.armed === step ? "Listening…" : learned[step]);
  }

  let prompt;
  if (wiz.armed) {
    const sec = Math.max(0, Math.ceil((wiz.until - Date.now()) / 1000));
    prompt = `${WIZ_PROMPT[wiz.armed]} now… ${sec}s`;
  } else if (wiz.up && wiz.down) {
    prompt = wiz.press || wiz.pressSkipped
      ? "All learned — press Add rows."
      : "Press Learn on the last step to add a press row, or Add rows without one.";
  } else {
    prompt = "";
  }
  setText("wizPrompt", [wiz.note, prompt].filter(Boolean).join(" "));

  const n = 2 + (wiz.press ? 1 : 0);
  const add = document.getElementById("wizAddBtn");
  add.disabled = !(wiz.up && wiz.down);
  add.textContent = `Add ${n} rows`;
}

function finishScrollWizard() {
  if (!wiz || !wiz.up || !wiz.down) return;
  const { idx, row, code } = wiz;
  const rowCount = 2 + (wiz.press ? 1 : 0);
  if (mappings.length - 1 + rowCount > MAX_MAPPING_ROWS) {
    alert(`That needs ${rowCount} rows and the table holds ${MAX_MAPPING_ROWS} — delete a row first.`);
    return;
  }
  if (!mappings[idx] || num(mappings[idx].oldButtonId) !== code) {
    alert("The button table changed while the wizard was open — nothing was added.");
    closeScrollWizard();
    return;
  }

  const base = (document.getElementById("wizName").value.trim() || "Scroll").slice(0, 17);
  const sameProtocol = protocolColumns().sameFamily;  // MQB wheel -> MEB chassis counts
  const blank = { canByteIndex: 255, canBitIndex: 0, resistiveOhm: 0, openHaldexMode: 0 };
  // Both directions share the LIN output code: the direction rides in the
  // scroll byte, which is passed through.
  const scrollOut = num(row.newLinButtonId) || (sameProtocol ? code : 0);
  // The replaced row's outputs (CAN, MOSFET, latch...) carry over to the
  // direction it was learned as — Up if it fired either way. The other rows
  // start empty: the other direction almost always wants a different action.
  const keepDir = rowDirection(row) === FLAG_ROTARY_DOWN ? FLAG_ROTARY_DOWN : FLAG_ROTARY_UP;
  const scrollRow = (dir) => {
    const name = `${base} ${dirWord(dir)}`;
    if (dir === keepDir) {
      return { ...row, name, newLinButtonId: scrollOut, flags: (num(row.flags) & ~TRIGGER_MASK) | dir };
    }
    return { ...blank, name, oldButtonId: code, newLinButtonId: scrollOut, flags: dir, sourceByte: num(row.sourceByte) };
  };
  const rows = [scrollRow(FLAG_ROTARY_UP), scrollRow(FLAG_ROTARY_DOWN)];
  if (wiz.press) {
    rows.push({
      ...blank,
      name: `${base} press`,
      oldButtonId: wiz.press.code,
      newLinButtonId: sameProtocol ? wiz.press.code : 0,
      flags: 0,
      sourceByte: wiz.press.byte,
    });
  }

  const fromFit = wiz.fromFit;
  closeScrollWizard(true);
  mappings.splice(idx, 1, ...rows);
  renderMappings();
  // Indices only move now, with no Learn armed, and the device gets the whole
  // set in one go.
  saveSetup();
  setStatus(`Scroll wheel ${hexCode(code)}: row ${idx + 1} replaced with ${rows.length} rows.`);
  if (fromFit) fitAfterScrollWizard(rows);
}

function wireScrollWizard() {
  document.querySelectorAll("[data-wiz-learn]").forEach((btn) => {
    btn.addEventListener("click", () => {
      if (!wiz) return;
      const step = btn.dataset.wizLearn;
      wiz.note = "";
      if (step === "press") wiz.pressSkipped = false;
      armWizardStep(step);
    });
  });
  document.getElementById("wizSkipPress").addEventListener("click", () => {
    if (!wiz) return;
    if (wiz.armed === "press") {
      fetch("/api/learn/cancel", { method: "POST" }).catch(() => {});
      wiz.armed = null;
    }
    wiz.press = null;
    wiz.pressSkipped = true;
    wiz.note = "";
    renderWizard();
  });
  document.getElementById("wizAddBtn").addEventListener("click", finishScrollWizard);
  document.getElementById("wizCancelBtn").addEventListener("click", () => closeScrollWizard());
}

async function forgetRollerCodes() {
  if (!confirm("Forget every code marked as a scroll wheel? Rows keep their triggers; a roller is re-detected the next time it is scrolled down.")) return;
  try {
    const res = await fetch("/api/rollers/clear", { method: "POST" });
    if (!res.ok) throw new Error("request failed");
    setRollerCodes([]);
    renderMappings();
  } catch (err) {
    alert(`Forget scroll wheels failed: ${err.message}`);
  }
}
let pollTimer = null;
let localLearnUntil = 0;
let localLearnRow = -1;
let localLearnTarget = 0;
let learnMayBeScroll = false;  // a Button Ori Learn whose result may open the scroll wizard
let learnBackstopTimer = null;
let learnStartRevision = null;  // mappingsRevision when that Learn was armed

// OpenHaldex mode names, indexed 0-5; 255 = "Push-to-Next".
const OH_MODE_NAMES = ["Stock", "FWD", "50:50", "60:40", "75:25", "Expert"];
const OH_MODE_PUSH_NEXT = 255;

// [pqCode, mqbCode, mebCode] for buttons independently verified against the
// README's documented MQB code table (see "MQB button codes" section) — the
// only rows we're confident enough about to auto-derive. Everything else
// (Phone, Voice/Mic ACC x2, Paddle +/-, Paddles-both, Horn) isn't in that
// list, so "Overwrite Existing Layout" leaves those rows untouched.
// The MEB column is a CHASSIS code only (no MEB wheel protocol): an MEB car
// takes MQB's codes, except that pre-2024 MEB cars ignore Previous/Next
// (0x16/0x15) and skip tracks with Up/Down (0x04/0x05) — per waldo22's ID.4
// report on PR #2, not checked on our own bench. Reportedly 2024+ MEB cars
// accept 0x16/0x15 too; those can pick MQB as the chassis instead.
const KNOWN_BUTTON_CODES = [
  [0x03, 0x16, 0x04], // Previous
  [0x02, 0x15, 0x05], // Next
  [0x1A, 0x19, 0x19], // Voice/Mic
  [0x29, 0x23, 0x23], // Return
  [0x22, 0x04, 0x04], // Up
  [0x23, 0x05, 0x05], // Down
  [0x09, 0x03, 0x03], // Source -
  [0x0A, 0x02, 0x02], // Source +
  [0x28, 0x07, 0x07], // OK
  [0x06, 0x10, 0x10], // Volume +
  [0x07, 0x11, 0x11], // Volume -
];

// Column of KNOWN_BUTTON_CODES -> frame family. MEB rides on MQB's frame
// layout and MQB's codes, so it counts as the MQB family.
const CODE_COLUMN_FAMILY = [0, 1, 1];

// Current selectors as KNOWN_BUTTON_CODES columns: wheel 0 PQ / 1 MQB,
// chassis 0 PQ / 1 MQB / 2 MEB. sameFamily is true for PQ->PQ, MQB->MQB and
// MQB->MEB — the pairs where a button keeps its code (bar MEB's Previous/Next).
// Wheel profiles whose codes are this firmware's own decoding, not a VW button
// set: VAG LIN 1.3 (2), Volvo P1 / P3 (4 / 5), BMW K-bus (6), Custom (7). The
// Up! / Citigo / Mii wheel (3) sends PQ codes, so it counts as PQ.
const DECODED_WHEELS = ["2", "4", "5", "6", "7"];
function wheelIsDecoded(v) {
  const wp = v != null ? String(v) : document.getElementById("wheelProtocol").value;
  return DECODED_WHEELS.includes(wp);
}
// Profiles that answer VW's 0x0E frame (PQ, MQB, Up!).
const VW_0E_WHEELS = [0, 1, 3];

function protocolColumns() {
  const wp = document.getElementById("wheelProtocol").value;
  const wheelCol = wp === "1" ? 1 : 0;
  const cp = document.getElementById("chassisProtocol").value;
  const chassisCol = cp === "2" ? 2 : cp === "1" ? 1 : 0;
  return {
    wheelCol,
    chassisCol,
    // A LIN 1.3 wheel (2) is not treated as sharing codes with any chassis:
    // its decoded codes are its own, so they are never passed through as-is.
    sameFamily: !wheelIsDecoded(wp) && CODE_COLUMN_FAMILY[wheelCol] === CODE_COLUMN_FAMILY[chassisCol],
  };
}

// True when (old, nw) is a known button's code pair across two DIFFERENT
// families (PQ wheel -> MQB/MEB chassis, or MQB wheel -> PQ chassis). Old is
// only ever a wheel code (PQ or MQB column), and those columns are unique, so
// a row can't match two buttons. Same-family pairs aren't matched here: their
// codes coincide across buttons (PQ Previous 0x03 = MQB Source - 0x03).
function crossFamilyPairMatches(codes, old, nw) {
  for (const i of [0, 1]) {
    for (const j of [0, 1, 2]) {
      if (CODE_COLUMN_FAMILY[i] !== CODE_COLUMN_FAMILY[j] && codes[i] === old && codes[j] === nw) {
        return true;
      }
    }
  }
  return false;
}

// Rewrite oldButtonId/newLinButtonId for rows whose current (old, new) pair
// still matches one of the known PQ/MQB/MEB code sets above, using whichever
// code each protocol selector now calls for. A row a user has already
// re-learned to a real-world value no longer matches either side of a known
// pair, so it's left alone automatically — no separate "custom" flag needed.
// Pass {dryRun: true} to just count how many rows WOULD change, without
// touching the mappings array. Returns the number of rows affected.
// `changed` says which selector moved. After a CHASSIS change the wheel is
// the same, so each row's old code is still that wheel's code and a known
// button can be looked up by it directly — that is what lets a same-family
// table (e.g. the PQ->PQ default, or MQB->MEB) translate to a cross-family
// chassis at all. After a WHEEL change the old codes belong to the previous
// wheel, so only a recognised cross-family pair is trusted.
function applyProtocolDefaultsToMappings({ dryRun = false, changed = "wheel" } = {}) {
  const { wheelCol, chassisCol, sameFamily } = protocolColumns();
  // Wheel and chassis in the SAME family (PQ->PQ, MQB->MQB, MQB->MEB) means
  // no translation is needed -- every configured row's new code must equal
  // its old code. This has to apply to every row, not just the ones in
  // KNOWN_BUTTON_CODES above: Phone, Voice/Mic ACC2, Paddle +/-, Paddles-both
  // and Horn were never in that list (no verified MQB code for them), so under
  // the old pair-only logic they stayed stuck at whatever "new" code they
  // shipped with (0 in the default table) even in a same-protocol setup where
  // they trivially should have matched "old". MQB->MEB then swaps in MEB's own
  // code for the known buttons where it differs (Previous/Next); the wheel
  // column is unique, so looking the button up by its wheel code is safe.
  let updated = 0;

  mappings.forEach((m) => {
    const old = num(m.oldButtonId);
    const nw = num(m.newLinButtonId);
    let newOld = old;
    let newNew = nw;

    // Only rows read from the button byte carry button codes. Paddles (byte 6)
    // and Horn (byte 7) hold bit patterns that merely look like codes — the
    // default Paddles (both) row's 0x03 would otherwise be taken for Previous.
    const sb = num(m.sourceByte);
    const onButtonByte = sb === 0 || sb === defaultButtonByte();
    const pair = onButtonByte && KNOWN_BUTTON_CODES.find((codes) => crossFamilyPairMatches(codes, old, nw));
    if (pair) {
      newOld = pair[wheelCol];
      newNew = pair[chassisCol];
    } else if (old !== 0 && (sameFamily || changed === "chassis")) {
      const known = onButtonByte && KNOWN_BUTTON_CODES.find((codes) => codes[wheelCol] === old);
      if (known) newNew = known[chassisCol];
      else if (sameFamily) newNew = old;
      else return;  // unknown code, cross-family: nothing to derive it from
    } else {
      return;
    }

    if (old !== newOld || nw !== newNew) {
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
// button table's known PQ/MQB/MEB codes to match, since we do know the right
// values for a subset of rows (see KNOWN_BUTTON_CODES above). If none
// of the current rows are on a recognised code, say so plainly instead of
// showing a confirm dialog with nothing to actually do.
function promptApplyProtocolDefaults(changed) {
  // A LIN 1.3 wheel (protocol 2) reports its own codes, not a known PQ/MQB
  // button set, so there is nothing to translate them from.
  if (wheelIsDecoded()) {
    setStatus("LIN 1.3 wheel: use Learn on each row, then pick the code for the car.");
    return;
  }
  const candidateCount = applyProtocolDefaultsToMappings({ dryRun: true, changed });
  if (candidateCount === 0) {
    setStatus("No button rows are on a recognised PQ/MQB/MEB code, so there's nothing to update automatically.");
    return;
  }

  const wheelLabel = document.getElementById("wheelProtocol").selectedOptions[0].text;
  const chassisLabel = document.getElementById("chassisProtocol").selectedOptions[0].text;
  const ok = confirm(
    `Update ${candidateCount} button code(s) to match Wheel Protocol (${wheelLabel}) / Chassis Protocol (${chassisLabel})?\n\n` +
    `Rows you've already learned or customised away from the standard PQ/MQB/MEB codes are left alone.\n\n` +
    `This saves immediately.`
  );
  if (!ok) return;

  const updated = applyProtocolDefaultsToMappings({ changed });
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
// hard to spot. The button byte (Setup "Button data byte", 1 by default) and
// bytes 6 and 7 (paddles / horn) are the legitimate positions. Learn stores
// the byte it captured from, so an ordinary button learns as sourceByte 1
// (captureLearned() in LIN.cpp), not 0 — that must not be flagged.
function srcByteHint(row) {
  const sb = num(row.sourceByte);
  if (!sb || sb === defaultButtonByte()) return "Matches the button byte";
  if (sb === 6) return "Matches frame byte 6 (paddles)";
  if (sb === 7) return "Matches frame byte 7 (horn)";
  if (!srcByteSuspect(row)) return `Matches frame byte ${sb}`;
  return `Matches frame byte ${sb} — unusual; if this row is always active, re-learn or delete it`;
}

function srcByteSuspect(row) {
  const sb = num(row.sourceByte);
  if (sb === 0 || sb === defaultButtonByte() || sb === 6 || sb === 7) return false;
  // A LIN 1.3 wheel's decoded frame also carries learnable bits in byte 2
  // (changed bits of its byte 0), so that's legitimate there. Byte 5 was too
  // up to V1.12, but on the A8 D3 it is the heater's temperature: a row
  // learned there was a temperature step, not a button, so it now shows as
  // unusual (orange) to be re-learned or deleted.
  if (wheelIsDecoded() && sb === 2) return false;
  return true;
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
  document.getElementById("forgetRollersBtn").addEventListener("click", forgetRollerCodes);
  wireScrollWizard();
  wireFitWizard();
  wireLinScan();
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
      promptApplyProtocolDefaults("wheel");
    });
  }
  // Custom wheel profile: every field saves on change, like the rest of Setup.
  ["cwBaud", "cwChecksum", "cwBtnId0", "cwBtnLen0", "cwBtnId1", "cwBtnLen1", "cwDecoder",
   "cwCodeByte", "cwRollerByte", "cwLightId", "cwLightLen", "cwLightData", "cwLightBrightByte",
   "cwLightBrightMax", "cwLightEveryMs"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("change", () => { updateCustomWheelUi(); saveSetup(); });
  });
  const cwFromScan = document.getElementById("cwFromScan");
  if (cwFromScan) cwFromScan.addEventListener("click", fillCustomWheelFromScan);
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
      promptApplyProtocolDefaults("chassis");
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
  updateCustomWheelUi();
}

// ---------- Custom wheel profile (Wheel Protocol 7) ----------
// Mirrors CustomWheelConfig in globals.h. 0xFF = "none" for IDs / bytes; a
// length of 0 = "work it out from the wheel's answer".
const CW_NONE = 0xFF;

function cwEl(id) { return document.getElementById(id); }

// "" or a hex byte -> number, or CW_NONE when blank / invalid.
function cwHexOrNone(id) {
  const s = (cwEl(id) ? cwEl(id).value : "").trim();
  if (s === "") return CW_NONE;
  const n = parseInt(s, 16);
  return Number.isFinite(n) && n >= 0 && n <= 0x3B ? n : CW_NONE;
}
function cwByteOrNone(id) {
  const s = (cwEl(id) ? cwEl(id).value : "").trim();
  if (s === "") return CW_NONE;
  const n = Number(s);
  return Number.isInteger(n) && n >= 0 && n <= 7 ? n : CW_NONE;
}

function fillCustomWheelForm(c) {
  if (!c || !cwEl("cwBaud")) return;
  const ids = Array.isArray(c.btnId) ? c.btnId : [];
  const lens = Array.isArray(c.btnLen) ? c.btnLen : [];
  const hexOrBlank = (v) => (v == null || num(v) === CW_NONE ? "" : hex2(v));
  const byteOrBlank = (v) => (v == null || num(v) === CW_NONE ? "" : String(num(v)));
  cwEl("cwBaud").value = String(num(c.baud) || 19200);
  cwEl("cwChecksum").value = String(c.checksum != null ? num(c.checksum) : 2);
  cwEl("cwBtnId0").value = hexOrBlank(ids[0]);
  cwEl("cwBtnLen0").value = String(num(lens[0]));
  cwEl("cwBtnId1").value = hexOrBlank(ids[1]);
  cwEl("cwBtnLen1").value = String(num(lens[1]));
  cwEl("cwDecoder").value = String(num(c.decoder));
  cwEl("cwCodeByte").value = String(c.codeByte != null ? num(c.codeByte) : 1);
  cwEl("cwRollerByte").value = byteOrBlank(c.rollerByte);
  cwEl("cwLightId").value = hexOrBlank(c.lightId);
  cwEl("cwLightLen").value = String(num(c.lightLen) || 4);
  cwEl("cwLightData").value = String(c.lightData || "").trim();
  cwEl("cwLightBrightByte").value = byteOrBlank(c.lightBrightByte);
  cwEl("cwLightBrightMax").value = hex2(c.lightBrightMax != null ? num(c.lightBrightMax) : 0x7F);
  cwEl("cwLightEveryMs").value = String(num(c.lightEveryMs) || 100);
  updateCustomWheelUi();
}

// The form as the firmware's customWheel object (it clamps anything odd too).
function readCustomWheelForm() {
  if (!cwEl("cwBaud")) return undefined;
  const len = (id) => clamp(num(cwEl(id).value), 0, 8);
  return {
    baud: num(cwEl("cwBaud").value) || 19200,
    checksum: num(cwEl("cwChecksum").value),
    decoder: num(cwEl("cwDecoder").value),
    codeByte: clamp(num(cwEl("cwCodeByte").value), 0, 7),
    rollerByte: cwByteOrNone("cwRollerByte"),
    btnId: [cwHexOrNone("cwBtnId0"), cwHexOrNone("cwBtnId1")],
    btnLen: [len("cwBtnLen0"), len("cwBtnLen1")],
    lightId: cwHexOrNone("cwLightId"),
    lightLen: clamp(num(cwEl("cwLightLen").value) || 4, 1, 8),
    lightData: cwEl("cwLightData").value.trim(),
    lightBrightByte: cwByteOrNone("cwLightBrightByte"),
    lightBrightMax: parseHexByte("cwLightBrightMax", 0x7F),
    lightEveryMs: clamp(num(cwEl("cwLightEveryMs").value) || 100, 100, 2000),
  };
}

function updateCustomWheelUi() {
  const fields = cwEl("cwFields");
  if (!fields) return;
  const active = document.getElementById("wheelProtocol").value === "7";
  fields.hidden = !active;
  const hint = cwEl("cwInactiveHint");
  if (hint) hint.hidden = active;
  const codeByte = cwEl("cwDecoder").value === "0";
  cwEl("cwCodeByteRow").hidden = !codeByte;
  cwEl("cwRollerByteRow").hidden = !codeByte;
}

// Take the first wheel frame(s) from the last LIN1 scan: ID, length,
// checksum, speed. The rest (decoder, light frame) stays as set.
function fillCustomWheelFromScan() {
  const hits = linScan && Array.isArray(linScan.hits) ? linScan.hits : [];
  if (!hits.length) {
    setStatus("No scan result yet — run Diagnostics > LIN Monitor > Scan LIN1 with the wheel connected.");
    return;
  }
  const a = hits[0];
  const b = hits[1];
  cwEl("cwBaud").value = String(num(a.baud) || 19200);
  cwEl("cwChecksum").value = a.classic ? "1" : "0";
  cwEl("cwBtnId0").value = hex2(a.id);
  cwEl("cwBtnLen0").value = String(num(a.len));
  cwEl("cwBtnId1").value = b && num(b.baud) === num(a.baud) ? hex2(b.id) : "";
  cwEl("cwBtnLen1").value = b && num(b.baud) === num(a.baud) ? String(num(b.len)) : "0";
  updateCustomWheelUi();
  saveSetup();
  setStatus(`Custom wheel set from the scan: frame ${hexCode(a.id)}${b ? ` and ${hexCode(b.id)}` : ""}. ` +
    "Pick how the buttons are coded, then Learn them in the button table.");
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
    if (setup.customWheel) fillCustomWheelForm(setup.customWheel);
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
    setRollerCodes(setup.rollerCodes);
    setMappingsDirty(false);
    renderMappings();
    setupLoaded = true;
    fitOnSetupLoaded();
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
    renderWheelTemp(s);
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

    // A roller detected in normal use (not just via Learn) changes which rows
    // need flagging, so redraw — only then, as it discards a half-typed edit.
    if (Array.isArray(s.rollerCodes)) {
      if (setRollerCodes(s.rollerCodes)) renderMappings();
      const listEl = document.getElementById("rollerCodesStatus");
      if (listEl) {
        const codes = [...rollerCodes].sort((a, b) => a - b)
          .map((c) => "0x" + c.toString(16).toUpperCase().padStart(2, "0"));
        listEl.textContent = codes.length
          ? `Scroll wheels detected: ${codes.join(", ")}`
          : "Scroll wheels detected: none yet — scroll one down to detect it";
      }
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

    updateWizardFromStatus(s);  // first: it keeps lastProbeSeq current
    updateLearnBannerFromStatus(s);
    // Kept apart so a UI bug in one of these can't pass for the device
    // dropping off (the catch below), which the fit wizard reads as a reboot.
    try {
      updateLinScanFromStatus(s);
      updateLiveRow(s);
      updateFitFromStatus(s);
    } catch (e) {
      console.error(e);
    }
  } catch (err) {
    updateLearnBannerFromStatus(null);
    fitNoteOffline();
  }
}

function updateLearnBannerFromStatus(status) {
  const el = document.getElementById("learnBuilderBanner");
  if (!el) {
    return;
  }

  const now = Date.now();

  if (status && status.learnActive && status.learnTarget === LEARN_PROBE) {
    el.classList.add("active");
    el.textContent = fit ? "Fit a new wheel is listening for the wheel."
      : "Scroll wheel setup is listening for the wheel.";
    markLearnButtons(-1, 0, 0);
    return;
  }

  if (status && status.learnActive) {
    const sec = Math.max(0, Math.ceil((status.learnMsRemaining || 0) / 1000));
    const rowText = Number(status.learnRowIndex || 0) + 1;
    const targetText = learnTargetText(status.learnTarget || 0);

    el.classList.add("active");
    el.textContent = `Learn active for row ${rowText} (${targetText}) - ${sec}s remaining`;
    markLearnButtons(Number(status.learnRowIndex || 0), status.learnTarget || 0, sec);
    return;
  }

  if (localLearnUntil > now) {
    const sec = Math.ceil((localLearnUntil - now) / 1000);
    const rowText = localLearnRow + 1;
    const targetText = learnTargetText(localLearnTarget);
    el.classList.add("active");
    el.textContent = `Learn active for row ${rowText} (${targetText}) - ${sec}s remaining`;
    markLearnButtons(localLearnRow, localLearnTarget, sec);
    return;
  }

  el.classList.remove("active");
  el.textContent = "Learn mode idle.";
  markLearnButtons(-1, 0, 0);
}

// The banner sits at the top of a long table, off screen on a phone, so the
// armed Learn button itself counts down too.
function markLearnButtons(row, target, sec) {
  const kind = target === 1 ? "old" : target === 2 ? "lin" : "";
  document.querySelectorAll("#mappingRows button[data-learn]").forEach((btn) => {
    const on = kind && btn.dataset.learn === kind && Number(btn.dataset.idx) === row;
    btn.classList.toggle("armed", !!on);
    const text = on ? `${sec}s` : "Learn";
    if (btn.textContent !== text) btn.textContent = text;
  });
}

function learnTargetText(target) {
  if (target === 1) {
    // Scrolling down is what proves a code is a roller (a button never reports
    // a negative step), so that is the way to learn one.
    return "From wheel — press it; for a scroll wheel, scroll it DOWN one click";
  }
  if (target === 2) {
    return "To car — press the button on the wheel the car already understands";
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

    // A row on a scroll-wheel code that still fires "either way" fires once
    // per sweep in both directions — flag it and offer the fix in place.
    const fix = rollerFix(idx);
    const trigWarn = fix && fix.kind === "split" ? ' class="warn-cell"' : "";
    const trigTitle = rowIsRoller(row)
      ? (fix && fix.kind === "split"
        ? "Scroll wheel: this row fires both ways and only once per sweep. Split learns up, down and press, then replaces this row with one row each."
        : "Scroll wheel code — the trigger picks which way it turns.")
      : "When this row fires. A roller sends one code both ways, so use two rows: Scroll up and Scroll down. Short/Long split a held button: Short stops matching once the hold passes ~0.8 s, Long starts then — so one code can tap for one action and hold for another. Long press + Latch = toggle on hold.";
    const fixBtn = !fix ? ""
      : fix.kind === "split"
        ? `<button class="btn tiny secondary" data-roller-fix="${idx}" title="Learn up, down and press, then replace this row with one row each">Split &#9650;&#9660;</button>`
        : `<button class="btn tiny secondary" data-roller-fix="${idx}" title="Learn the Scroll ${dirWord(fix.dir)} (and press) for this wheel and add their rows">+ ${fix.dir === FLAG_ROTARY_UP ? "&#9650;" : "&#9660;"} row</button>`;

    // Phone layout: Name / From wheel / To car stay visible, everything else
    // folds away behind the Outputs toggle (CSS does the folding; on a wide
    // screen the toggle column is hidden and every column shows).
    const open = openRows.has(idx);
    if (open) tr.classList.add("open");
    const hexAttrs = 'type="text" inputmode="text" maxlength="4" autocapitalize="characters" autocomplete="off" spellcheck="false"';

    tr.innerHTML = `
      <td class="c-name"><input data-field="name" data-idx="${idx}" type="text" maxlength="17" value="${escapeAttr(row.name || "")}" aria-label="Name"></td>
      <td class="c-more"><button class="btn tiny secondary row-more" data-more="${idx}" type="button" aria-expanded="${open}"><span class="row-more-sum">${escapeAttr(outputsSummary(row))}</span><span class="row-more-arrow">${open ? "&#9652;" : "&#9662;"}</span></button></td>
      <td class="c-from" data-label="From wheel"><div class="code-cell"><span class="code-pre">0x</span><input class="hex-input${srcByteSuspect(row) ? " warn-cell" : ""}" data-field="oldButtonId" data-idx="${idx}" ${hexAttrs} value="${hex2(num(row.oldButtonId))}" title="Code the fitted wheel sends. ${srcByteHint(row)}" aria-label="From wheel code (hex)"><button class="btn tiny secondary" data-learn="old" data-idx="${idx}" type="button" title="Learn: press this, then the button on the wheel fitted now">Learn</button></div></td>
      <td class="c-to" data-label="To car"><div class="code-cell"><span class="code-pre">0x</span><input class="hex-input" data-field="newLinButtonId" data-idx="${idx}" ${hexAttrs} value="${hex2(num(row.newLinButtonId))}" title="Code passed on to the car (chassis LIN output)" aria-label="To car code (hex)"${exDis}><button class="btn tiny secondary" data-learn="lin" data-idx="${idx}" type="button" title="Learn: press this, then the button on a wheel the car already understands"${exDis}>Learn</button></div></td>
      <td class="c-out" data-label="CAN byte"><select data-field="canByteIndex" data-idx="${idx}" title="Which byte of the CAN frame this button sets a bit in. &#8212; means NO CAN output for this row (the default for new rows)."${exDis}>${byteOptions}</select></td>
      <td class="c-out" data-label="CAN bit"><input data-field="canBitIndex" data-idx="${idx}" type="number" min="0" max="7" value="${bitIdx}"${exDis}></td>
      <td class="c-out" data-label="Resistive (&Omega;)"><input data-field="resistiveOhm" data-idx="${idx}" type="number" min="0" max="20000" value="${num(row.resistiveOhm)}"${exDis}></td>
      <td class="c-out c-chk" data-label="MOSFET"><input data-field="flags" data-subfield="pnp" data-idx="${idx}" type="checkbox" title="Activate MOSFET output when pressed" ${(num(row.flags) & 1) ? "checked" : ""}${exDis}></td>
      <td class="c-out c-chk" data-label="Latch"><input data-field="flags" data-subfield="latch" data-idx="${idx}" type="checkbox" title="Latch: press to activate, press again to clear (MOSFET, CAN and LIN)" ${(num(row.flags) & 2) ? "checked" : ""}${exDis}></td>
      <td class="c-out c-chk" data-label="OpenHaldex"><input data-field="flags" data-subfield="openhaldex" data-idx="${idx}" type="checkbox" title="OpenHaldex control (exclusive: overrides MOSFET/CAN/LIN for this button)" ${ohEnabled ? "checked" : ""}${chaEnabled ? " disabled" : ""}></td>
      <td class="c-out" data-label="OH mode"><select data-field="openHaldexMode" data-idx="${idx}" title="OpenHaldex mode to set on press"${ohEnabled ? "" : " disabled"}>${ohModeOptions(ohMode)}</select></td>
      <td class="c-out c-chk" data-label="Charisma"><input data-field="flags" data-subfield="charisma" data-idx="${idx}" type="checkbox" title="Charisma / Drive Select: press advances the program (exclusive: overrides MOSFET/CAN/LIN for this button). Set the output mode in Settings." ${chaEnabled ? "checked" : ""}${ohEnabled ? " disabled" : ""}></td>
      <td class="c-out c-trig" data-label="Trigger"><div class="trig-cell"><select data-field="trigger" data-idx="${idx}" title="${trigTitle}"${trigWarn}${exDis}>${triggerOptions(num(row.flags))}</select>${fixBtn}</div></td>
      <td class="c-out c-del"><button class="btn tiny danger" data-delete="row" data-idx="${idx}" type="button">Delete</button></td>
    `;

    tbody.appendChild(tr);
  });

  tbody.querySelectorAll("input, select").forEach((el) => {
    el.addEventListener("change", (e) => {
      const i = Number(e.target.dataset.idx);
      onEditRow(e);
      refreshRowSummary(i);
    });
  });

  tbody.querySelectorAll("button[data-more]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const i = Number(btn.dataset.more);
      const tr = btn.closest("tr");
      const nowOpen = !openRows.has(i);
      if (nowOpen) openRows.add(i); else openRows.delete(i);
      tr.classList.toggle("open", nowOpen);
      btn.setAttribute("aria-expanded", String(nowOpen));
      btn.querySelector(".row-more-arrow").innerHTML = nowOpen ? "&#9652;" : "&#9662;";
    });
  });
  if (liveRowIdx >= 0) {
    const tr = tbody.querySelector(`tr[data-idx="${liveRowIdx}"]`);
    if (tr) tr.classList.add("live");
  }

  tbody.querySelectorAll("button[data-learn]").forEach((btn) => {
    btn.addEventListener("click", onLearnClick);
  });

  tbody.querySelectorAll("button[data-delete='row']").forEach((btn) => {
    btn.addEventListener("click", onDeleteRowClick);
  });

  tbody.querySelectorAll("button[data-roller-fix]").forEach((btn) => {
    btn.addEventListener("click", () => openScrollWizard(Number(btn.dataset.rollerFix)));
  });
}

// Rows whose outputs are unfolded in the phone layout, by index.
const openRows = new Set();

// One line saying what a row does, shown on its folded Outputs toggle.
function outputsSummary(row) {
  const f = num(row.flags);
  if (f & 4) return "OpenHaldex";
  if (f & 8) return "Charisma";
  const parts = [];
  if (typeof row.canByteIndex === "number" && row.canByteIndex < 8) {
    parts.push(`CAN ${row.canByteIndex}.${clamp(num(row.canBitIndex), 0, 7)}`);
  }
  if (num(row.resistiveOhm)) parts.push(`${num(row.resistiveOhm)} Ω`);
  if (f & 1) parts.push("MOSFET");
  if (f & 2) parts.push("Latch");
  const trig = TRIGGERS.find((t) => t[2] && (f & TRIGGER_MASK) === t[2]);
  if (trig) parts.push(trig[1]);
  return parts.length ? parts.join(" · ") : "Outputs";
}

function refreshRowSummary(idx) {
  const row = mappings[idx];
  const el = document.querySelector(`#mappingRows tr[data-idx="${idx}"] .row-more-sum`);
  if (row && el) el.textContent = outputsSummary(row);
}

// "0E", "0x0E", "e" -> 14; "" -> 0; anything else -> null.
function parseHexCode(text) {
  const s = String(text).trim().replace(/^0x/i, "");
  if (s === "") return 0;
  if (!/^[0-9a-f]{1,2}$/i.test(s)) return null;
  return parseInt(s, 16);
}

function onDeleteRowClick(event) {
  const idx = Number(event.target.dataset.idx);
  mappings.splice(idx, 1);
  // Unfolded rows are tracked by index, which just shifted.
  const shifted = [...openRows].filter((i) => i !== idx).map((i) => (i > idx ? i - 1 : i));
  openRows.clear();
  shifted.forEach((i) => openRows.add(i));
  renderMappings();
  setMappingsDirty(true);
}

function onEditRow(event) {
  const idx = Number(event.target.dataset.idx);
  const field = event.target.dataset.field;

  if (field === "oldButtonId" || field === "newLinButtonId") {
    // Codes are shown and typed in hex, like the README and the LIN Monitor.
    const v = parseHexCode(event.target.value);
    if (v === null) {
      setStatus(`"${event.target.value}" isn't a code — type it in hex, 00 to FF (e.g. 0E).`);
      renderMappings();  // put the old value back
      return;
    }
    setMappingsDirty(true);
    mappings[idx][field] = v;
    // Typing a From wheel code by hand means "match this on the normal button
    // byte" — otherwise a sourceByte left behind by a bad Learn keeps the row
    // pointed at the wrong byte, with nothing on screen explaining why.
    if (field === "oldButtonId") mappings[idx].sourceByte = 0;
    renderMappings();
    return;
  }

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
  if (field === "trigger") {
    const cleared = num(mappings[idx].flags) & ~TRIGGER_MASK;
    const v = Number(event.target.value);
    const entry = TRIGGERS.find((t) => t[0] === v);
    mappings[idx].flags = cleared | (entry ? entry[2] : 0);
    renderMappings();  // the scroll-wheel Split / + row offer depends on it
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
    learnMayBeScroll = target === 1;
    learnStartRevision = seenMappingsRevision;
    updateLearnBannerFromStatus(null);

    setStatus("Learn started. Press a button within 5 seconds.");
    // The learned value is pulled in by the status poll the moment the
    // firmware reports it (mappingsRevision). One late merge as a backstop,
    // after the window has closed. Not loadSetup(): that replaces the whole
    // table and would discard any unsaved edits on other rows.
    clearTimeout(learnBackstopTimer);
    learnBackstopTimer = setTimeout(async () => {
      await mergeLearnedCodes();
      learnMayBeScroll = false;  // window over: later merges are not this Learn
    }, 5500);
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
    // 0 PQ, 1 MQB, 2 VAG LIN 1.3; anything else falls back to PQ (as the firmware does).
    wheelProtocol: [0, 1, 2, 3, 4, 5, 6, 7].includes(Number(document.getElementById("wheelProtocol").value))
      ? Number(document.getElementById("wheelProtocol").value) : 0,
    customWheel: readCustomWheelForm(),
    mqbActByte1: parseHexByte("mqbActByte1", 0xFF),
    mqbActByte2: parseHexByte("mqbActByte2", 0x00),
    mqbActByte3: parseHexByte("mqbActByte3", 0x00),
    // 0 PQ, 1 MQB, 2 MEB; anything else falls back to MQB (the firmware does the same).
    chassisProtocol: [0, 1, 2].includes(Number(document.getElementById("chassisProtocol").value))
      ? Number(document.getElementById("chassisProtocol").value) : 1,
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
      return false;
    }
    if (!res.ok) {
      throw new Error("Save failed");
    }

    const body = await res.json().catch(() => ({}));
    if (body && body.reboot) {
      setStatus(body.message || "LIN pin mapping changed. Device rebooting\u2026");
      return true;
    }

    setMappingsDirty(false);
    setStatus("Setup saved.");
    return true;
  } catch (err) {
    setStatus(`Save error: ${err.message}`);
    return false;
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

// Diagnostics "Wheel Heater Sensor": a VAG LIN 1.3 wheel's 0x30 byte 5 (the
// heated rim's temperature, raw). Shown only on that profile; the firmware
// sends wheelTempRaw only while 0x30 frames are arriving. The degree figure
// borrows MQB's 0x3A rule (raw - 68), which nobody has confirmed for these
// wheels, so it's labelled as such in the page.
function renderWheelTemp(s) {
  const card = document.getElementById("wheelTempCard");
  if (!card) return;
  const lin13 = document.getElementById("wheelProtocol").value === "2";
  card.style.display = lin13 ? "" : "none";
  if (!lin13) return;
  const h = (v) => v.toString(16).toUpperCase().padStart(2, "0");
  if (typeof s.wheelTempRaw !== "number") {
    setText("wheelTempRaw", "--");
    setText("wheelTempState", "No 0x30 frames");
    setText("wheelTempEstimate", "--");
  } else {
    const raw = s.wheelTempRaw;
    setText("wheelTempRaw", `${h(raw)} (${raw})`);
    setText("wheelTempState", raw === 0xFF ? "Open / unplugged" : "Connected");
    setText("wheelTempEstimate", raw === 0xFF ? "--" : `${raw - 68} °C`);
  }
  setText("wheelTempRange", typeof s.wheelTempMin === "number"
    ? `${h(s.wheelTempMin)} / ${h(s.wheelTempMax)}` : "--");
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

// Messages land in every .app-status line (Setup's Wheel & car card and the
// Buttons tab). They used to go to #otaStatus, on the OTA tab, where nobody
// working on the Buttons tab could see them.
function setStatus(text) {
  document.querySelectorAll(".app-status").forEach((el) => {
    el.textContent = text;
  });
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

// Only touch the DOM when the markup really changed: these are refreshed on
// every 200 ms status poll, and replacing a button under a finger as it is
// pressed swallows the tap.
function setHtml(el, html) {
  if (el && el._html !== html) {
    el._html = html;
    el.innerHTML = html;
  }
}

// ---------- LIN1 scan / wheel detect ----------
// GET /api/status "linScan" (runLinScan() in LIN.cpp): seq +1 per completed
// scan, suggest = the wheel protocol the answers match (0 PQ / 1 MQB / 2 VAG
// LIN 1.3 / 3 Up! / 4 Volvo P1 / 5 Volvo P3), 253 something unknown answered, 254 nothing answered, 255 none
// run yet. Firmware before V1.10 has no linScan field: all of this then stays
// hidden. linScan.ms is the device's millis() when it finished; with
// uptimeMs (V1.11) that gives the scan's age.
const WHEEL_PROTOCOL_NAMES = { 0: "PQ", 1: "MQB", 2: "VAG LIN 1.3", 3: "VW Up! / Citigo / Mii", 4: "Volvo P1", 5: "Volvo P3", 6: "BMW K-bus", 7: "Custom" };
const SCAN_UNKNOWN = 253;
const SCAN_NOTHING = 254;
const HINT_UNKNOWN = "Unknown wheel — export the LIN Monitor log (Diagnostics) and send it to Forbes Automotive.";
const HINT_NOTHING = "Nothing answered on LIN1 — check the wheel's power and ground, the LIN wire, and Setup > Advanced > Legacy PCB.";

let linScan = null;           // last linScan from /api/status
let linScanAgeS = null;       // seconds since that scan finished (null: firmware doesn't say)
let lin1Healthy = false;
let linScanPending = null;    // { seq, at } while a scan we asked for is running
let linScanError = "";
let linScanAsked = false;     // the Diagnostics Scan LIN1 button has been used
let detectDismissed = "";     // key of the banner the user closed
try { detectDismissed = sessionStorage.getItem("mfswDetectDismissed") || ""; } catch (e) {}

function scanSuggestText(sug) {
  if (sug === 0) return "a PQ wheel (frame 0x0E)";
  if (sug === 1) return "an MQB wheel (frame 0x0E)";
  if (sug === 2) return "a VAG LIN 1.3 wheel (frame 0x30)";
  if (sug === 3) return "a VW Up! / Citigo / Mii wheel (frames 0x0E + 0x0C)";
  if (sug === 4) return "a Volvo P1 wheel (frame 0x20 at 9600)";
  if (sug === 5) return "a Volvo P3 wheel (frames 0x30 / 0x20 at 9600)";
  if (sug === SCAN_UNKNOWN) return "an unknown wheel";
  if (sug === SCAN_NOTHING) return "nothing — no ID answered";
  return "nothing yet (no scan has run)";
}

function selectedWheelProtocol() {
  return Number(document.getElementById("wheelProtocol").value);
}

async function requestLinScan() {
  linScanError = "";
  linScanPending = { seq: linScan ? linScan.seq : null, at: Date.now() };
  renderLinScanResult();
  try {
    const res = await fetch("/api/lin/scan", { method: "POST" });
    if (!res.ok) throw new Error(res.status === 404 ? "this firmware can't scan LIN1 — update it (OTA tab)" : `HTTP ${res.status}`);
    return true;
  } catch (err) {
    linScanPending = null;
    linScanError = `Scan failed: ${err.message}`;
    renderLinScanResult();
    renderFitLive();
    return false;
  }
}

function updateLinScanFromStatus(s) {
  lin1Healthy = !!s.lin1Healthy;
  const prevSeq = linScan ? linScan.seq : null;
  linScan = (s.linScan && typeof s.linScan.seq === "number") ? s.linScan : null;
  linScanAgeS = (linScan && typeof s.uptimeMs === "number" && s.uptimeMs >= num(linScan.ms))
    ? Math.round((s.uptimeMs - num(linScan.ms)) / 1000) : null;
  // Also redraw while the result shows an age, so "(N s ago)" keeps counting.
  let redraw = !linScan || linScan.seq !== prevSeq || linScanAgeS != null;
  if (linScanPending) {
    if (linScan && linScan.seq !== linScanPending.seq) {
      linScanPending = null;
      redraw = true;
    } else if (Date.now() - linScanPending.at > 6000) {
      linScanPending = null;
      linScanError = linScan ? "The scan didn't finish — try again." : "This firmware doesn't report LIN1 scans — update it (OTA tab).";
      redraw = true;
    }
  }
  if (redraw) renderLinScanResult();
  renderDetectBanners();
}

// Diagnostics > LIN Monitor: the hits of the last scan, manual or automatic.
function renderLinScanResult() {
  const el = document.getElementById("linScanResult");
  if (!el) return;
  const btn = document.getElementById("linScanBtn");
  if (btn) {
    btn.disabled = !!linScanPending;
    btn.textContent = linScanPending ? "Scanning…" : "Scan LIN1";
  }
  if (linScanPending) {
    el.hidden = false;
    setHtml(el, `<div class="scan-head">Scanning LIN1 (1–3 s)…</div>`);
    return;
  }
  if (linScanError) {
    el.hidden = false;
    setHtml(el, `<div class="scan-head warn">${escapeAttr(linScanError)}</div>`);
    return;
  }
  if (!linScan || !linScan.seq) {
    el.hidden = !linScanAsked;
    setHtml(el, `<div class="scan-head">No scan has run yet.</div>`);
    return;
  }
  const sug = num(linScan.suggest);
  const hits = Array.isArray(linScan.hits) ? linScan.hits : [];
  const wp = selectedWheelProtocol();
  const age = linScanAgeS == null ? "" : linScanAgeS < 60 ? ` (${linScanAgeS} s ago)` : ` (${Math.round(linScanAgeS / 60)} min ago)`;
  let head = `Last scan${age}: ${hits.length} ID${hits.length === 1 ? "" : "s"} answered — looks like ${scanSuggestText(sug)}.`;
  if (sug <= 7 && sug !== wp) head += ` Wheel Protocol is set to ${WHEEL_PROTOCOL_NAMES[wp] || wp}.`;
  if (sug === SCAN_UNKNOWN) head += ` ${HINT_UNKNOWN}`;
  if (sug === SCAN_NOTHING) head += ` ${HINT_NOTHING}`;
  const rows = hits.map((h) =>
    `<div class="scan-hit"><span class="scan-id">${hexCode(h.id)}</span>` +
    `<span class="scan-meta">${num(h.len)} byte${num(h.len) === 1 ? "" : "s"}, ${h.classic ? "classic (LIN 1.x)" : "enhanced (LIN 2.x)"}${h.baud ? `, ${num(h.baud)} baud` : ""}</span>` +
    `<code>${escapeAttr(h.data || "")}</code></div>`).join("");
  el.hidden = false;
  setHtml(el, `<div class="scan-head">${escapeAttr(head)}</div>${rows}`);
}

// What the Dashboard / Setup banner should say, or null.
function detectProblem() {
  // Until /api/setup has loaded, the Wheel Protocol select shows its HTML
  // default rather than the device's setting.
  if (!setupLoaded || !linScan || !linScan.seq) return null;
  const sug = num(linScan.suggest);
  const wp = selectedWheelProtocol();
  const key = `${linScan.seq}|${sug}|${wp}`;
  if (sug <= 7) {
    if (sug === wp) return null;
    // A wheel answering under the selected protocol makes an older 0x30-vs-0x0E
    // result stale (the two never answer each other's frame). PQ and MQB both
    // answer 0x0E, so that mismatch counts even while the wheel is talking.
    // Custom (7) may well be set up for exactly this wheel: trust it while it answers.
    if (lin1Healthy && !(VW_0E_WHEELS.includes(sug) && VW_0E_WHEELS.includes(wp))) return null;
    return {
      key, sug, switchable: true,
      text: `The wheel on LIN1 looks like ${scanSuggestText(sug)}, but Wheel Protocol is set to ${WHEEL_PROTOCOL_NAMES[wp] || wp}.`,
    };
  }
  if (lin1Healthy) return null;  // the scan is older than the wheel that now answers
  if (sug === SCAN_UNKNOWN) return { key, text: HINT_UNKNOWN };
  if (sug === SCAN_NOTHING) return { key, text: HINT_NOTHING };
  return null;
}

function renderDetectBanners() {
  const p = detectProblem();
  const show = p && p.key !== detectDismissed;
  const html = !show ? "" :
    `<span class="detect-text">${escapeAttr(p.text)}</span>` +
    `<span class="detect-actions">` +
    (p.switchable ? `<button class="btn tiny primary" type="button" data-detect-switch="${p.sug}">Switch Wheel Protocol to ${WHEEL_PROTOCOL_NAMES[p.sug]}</button>` : "") +
    `<button class="btn tiny secondary" type="button" data-detect-dismiss="${escapeAttr(p.key)}">Dismiss</button></span>`;
  document.querySelectorAll("[data-detect-banner]").forEach((el) => {
    el.hidden = !show;
    setHtml(el, html);
  });
}

// Set Wheel Protocol as if picked in Setup. promptCodes: also offer to update
// known PQ/MQB button codes (Setup's own behaviour); the fit wizard says no,
// as it learns every code itself.
function setWheelProtocol(v, promptCodes) {
  const sel = document.getElementById("wheelProtocol");
  if (!sel || sel.value === String(v)) return;
  sel.value = String(v);
  updateWheelProtocolUi();
  saveSetup();
  if (Number(v) === 1) {
    setStatus("Wheel Protocol set to MQB. If its buttons don't report, try the other Activation preset.");
  }
  if (promptCodes) promptApplyProtocolDefaults("wheel");
  renderDetectBanners();
  renderLinScanResult();
}

function wireLinScan() {
  const btn = document.getElementById("linScanBtn");
  if (btn) {
    btn.addEventListener("click", () => {
      linScanAsked = true;
      requestLinScan();
    });
  }
  document.addEventListener("click", (e) => {
    const sw = e.target.closest("[data-detect-switch]");
    if (sw) {
      setWheelProtocol(Number(sw.dataset.detectSwitch), true);
      return;
    }
    const dis = e.target.closest("[data-detect-dismiss]");
    if (dis) {
      detectDismissed = dis.dataset.detectDismiss;
      try { sessionStorage.setItem("mfswDetectDismissed", detectDismissed); } catch (err) {}
      renderDetectBanners();
    }
  });
}

// ---------- Live row highlight ----------
// The row the firmware matched lights up in the table (and in the fit
// wizard's test list) while its button is held.
let liveRowIdx = -1;
let lastPressText = "";

function updateLiveRow(s) {
  let idx = -1;
  const name = s.pressedButtonName;
  if (name) {
    const code = num(s.buttonIncoming);
    // V1.11 firmware reports the exact row it matched; older firmware only the
    // name + code, which is ambiguous when names repeat.
    if (typeof s.matchedRow === "number" && s.matchedRow >= 0 && s.matchedRow < mappings.length) {
      idx = s.matchedRow;
    } else {
      idx = mappings.findIndex((m) => m.name === name && num(m.oldButtonId) === code);
      if (idx < 0) idx = mappings.findIndex((m) => m.name === name);
    }
    lastPressText = `${name} (${hexCode(code)})`;
  } else if (num(s.buttonIncoming)) {
    lastPressText = `${hexCode(s.buttonIncoming)} — no row uses this code`;
  }
  if (idx === liveRowIdx) return;
  liveRowIdx = idx;
  document.querySelectorAll("#mappingRows tr[data-idx]").forEach((tr) => {
    tr.classList.toggle("live", Number(tr.dataset.idx) === idx);
  });
}

// ---------- Fit a new wheel ----------
// Four steps over the existing Learn API, using probe Learns (target 4: the
// firmware reports what it heard and writes no row), so the table is only
// changed here, row by row, and nothing depends on row indices while a step
// runs:
//   1. Current wheel: each row's "To car" (newLinButtonId) = what the wheel
//      fitted now sends, i.e. what the car understands. Saved at the end of
//      the step, because step 2 powers the controller off.
//   2. Swap: ignition off, fit the new wheel, ignition on; pick its protocol
//      and see it answer (LIN1 scan where the firmware has one).
//   3. New wheel: each row's "From wheel" (oldButtonId + sourceByte) = the new
//      button chosen for it. Rollers go through the scroll-wheel wizard.
//   4. Test and save: saved, then each press lights its row.
// Progress from step 2 on is kept in localStorage, so a page reopened after
// the ignition cycle carries on.

const FIT_KEY = "mfswFitWheel";
const FIT_STEP_NAMES = ["Current wheel", "Swap wheel", "New wheel", "Test & save"];
let fit = null;          // running wizard, null when idle
let fitResume = null;    // saved progress offered on the idle screen
let fitResumeChecked = false;

function newFit(step) {
  return {
    step,
    queue: [], pos: 0,
    armed: false, arming: false, until: 0, baseSeq: null, retryAt: 0,
    waitIdle: false, idleCount: 0, waitSince: 0, rest: null,
    note: "", last: "",
    got: new Map(), skipped: new Set(), none: new Set(),
    carByte: new Map(),           // row -> byte the car's code came from (step 1)
    snapshot: null, savedInStep3: false,
    pendingDup: null, scrollRowFlags: 0,
    saving: false, saveFailed: false,
    swapPhase: "fit", offline: false, step2Since: 0,
    scanAsked: false, scanSeqAtAsk: null,
    forgetRollers: true, extraOpen: false,
  };
}

function cloneRows(rows) {
  return rows.map((r) => ({ ...r }));
}

function fitStore() {
  try {
    if (!fit || fit.step < 2) return;
    const carBytes = {};
    fit.carByte.forEach((b, row) => {
      const i = mappings.indexOf(row);
      if (i >= 0) carBytes[i] = b;
    });
    localStorage.setItem(FIT_KEY, JSON.stringify({
      step: fit.step, swapPhase: fit.swapPhase, at: Date.now(),
      names: mappings.map((m) => m.name), carBytes,
    }));
  } catch (e) {}
}

function fitStoreClear() {
  try { localStorage.removeItem(FIT_KEY); } catch (e) {}
}

function fitStoreRead() {
  try {
    const v = JSON.parse(localStorage.getItem(FIT_KEY) || "null");
    if (v && v.step >= 2 && v.step <= 4 && Date.now() - num(v.at) < 3 * 86400000) return v;
  } catch (e) {}
  return null;
}

function effByte(sb) {
  const b = num(sb);
  return b >= 1 && b < 8 ? b : defaultButtonByte();
}

function fitRow() {
  return fit ? fit.queue[fit.pos] : null;
}

async function fitStart(skipStep1) {
  if (wiz || fit) return;
  if (lastProbeSeq === null) {
    alert("The controller didn't answer, or its firmware is too old for this (it needs V1.08 or later). Check the connection, or update on the OTA tab.");
    return;
  }
  if (mappingsDirty) {
    if (!confirm("The button table has unsaved changes. Save them and start?")) return;
    if (!(await saveSetup())) {
      alert("Couldn't save the button table, so the wheel swap didn't start. Try again.");
      return;
    }
  }
  fitResume = null;
  fitStoreClear();
  fit = newFit(1);
  if (skipStep1) {
    fitEnterStep2();
    return;
  }
  fit.snapshot = cloneRows(mappings);
  fitBeginPass();
}

// Start (or restart) step 1 or 3 from its first row.
function fitBeginPass() {
  // OpenHaldex / Charisma rows send nothing to the car, so step 1 skips them.
  fit.queue = fit.step === 1 ? mappings.filter((m) => !(num(m.flags) & 0x0C)) : mappings.slice();
  fit.pos = 0;
  fit.got.clear();
  fit.skipped.clear();
  fit.none.clear();
  fit.note = "";
  fit.last = "";
  fit.rest = null;
  fit.pendingDup = null;
  fit.extraOpen = false;
  fit.scanAsked = false;
  if (fit.step === 3) {
    fit.snapshot = cloneRows(mappings);
    fit.savedInStep3 = false;
  }
  renderFit();
  if (!fit.queue.length) {
    fitPassDone();
    return;
  }
  fitRearm();
}

// Listen for the next press once the wheel is back at rest. Arming while the
// last button is still held makes the firmware take that frame as its idle
// baseline, and then no later press on that byte is ever captured (see
// captureLearnedFromFrame() in LIN.cpp: the previous frame must equal the
// baseline). Waits at most 4 s, then arms anyway.
function fitRearm() {
  if (!fit) return;
  fit.armed = false;
  fit.waitIdle = true;
  fit.idleCount = 0;
  fit.waitSince = Date.now();
  renderFitLearn();
}

async function fitArm() {
  if (!fit || fit.arming) return;
  fit.arming = true;
  try {
    const res = await fetch("/api/learn/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ rowIndex: 0, target: LEARN_PROBE }),
    });
    if (!res.ok) throw new Error("the device refused it — is the firmware up to date?");
  } catch (err) {
    if (fit) {
      fit.arming = false;
      fit.note = `Couldn't start listening: ${err.message}. Retrying…`;
      fit.waitIdle = true;
      fit.waitSince = Date.now();
      fit.retryAt = Date.now() + 2000;
      renderFitLearn();
    }
    return;
  }
  if (!fit) {
    fetch("/api/learn/cancel", { method: "POST" }).catch(() => {});
    return;
  }
  fit.arming = false;
  if (fit.note.startsWith("Couldn't start listening")) fit.note = "";
  fit.armed = true;
  fit.baseSeq = lastProbeSeq;
  fit.until = Date.now() + PROBE_WINDOW_MS;
  renderFitLearn();
}

function fitCancelArm() {
  if (fit && (fit.armed || fit.arming)) fetch("/api/learn/cancel", { method: "POST" }).catch(() => {});
  if (fit) {
    fit.armed = false;
    fit.waitIdle = false;
  }
}

function parseFrame(hex) {
  if (typeof hex !== "string") return null;
  const b = hex.trim().split(/\s+/).map((h) => parseInt(h, 16));
  return b.length >= 8 && b.every(Number.isFinite) ? b : null;
}

// Wheel at rest: nothing on the button byte, and the paddle / horn / extra
// bytes back where they sat at the start of the step. The scroll byte also
// carries the press stage, and on a PQ wheel an absolute roller position
// that never goes back, so it is not compared.
function frameIdle(frame) {
  if (!frame || frame.every((b) => b === 0)) return true;  // not answering: nothing to wait for
  const bi = defaultButtonByte();
  if (frame[bi] !== 0) return false;
  const rot = num(document.getElementById("linRotaryByteIndex").value);
  if (fit.rest) {
    for (const i of [2, 5, 6, 7]) {
      if (i !== bi && i !== rot && frame[i] !== fit.rest[i]) return false;
    }
  }
  return true;
}

function onFitCapture(code, byte, rotary, force = false) {
  const row = fitRow();
  if (!row) return;
  const dir = rowDirection(row);
  fit.note = "";
  fit.pendingDup = null;

  if (dir) {
    // A Scroll up / Scroll down row: the movement must match.
    if (rotary === 0) {
      fit.note = `Heard ${hexCode(code)} but no scroll movement. Turn the scroll wheel one click.`;
      fitRearm();
      return;
    }
    const turned = rotary > 0 ? FLAG_ROTARY_UP : FLAG_ROTARY_DOWN;
    if (turned !== dir) {
      fit.note = `That was a ${dirWord(turned).toUpperCase()} scroll; this one is ${dirWord(dir).toUpperCase()}.`;
      fitRearm();
      return;
    }
  }

  if (fit.step === 1) {
    row.newLinButtonId = code;
    fit.carByte.set(row, byte);
    fit.got.set(row, { code, byte });
    fit.skipped.delete(row);
    setMappingsDirty(true);
    renderMappings();
    fit.last = `“${row.name}” sends ${hexCode(code)} to the car.`;
    fitAdvance();
    return;
  }

  // Step 3. A move no plain button makes (down, or more than one click), or a
  // code already known as a scroll wheel, means a roller: it needs one row per
  // direction, which the scroll-wheel wizard sets up.
  const isRoller = !dir && rotary !== 0 && byte === defaultButtonByte() &&
    (rotary < 0 || Math.abs(rotary) >= 2 || rollerCodes.has(code));

  if (!force && !isRoller) {
    const trig = num(row.flags) & TRIGGER_MASK;
    const other = mappings.find((m) => m !== row && fit.got.has(m) &&
      num(m.oldButtonId) === code && effByte(m.sourceByte) === effByte(byte) &&
      (num(m.flags) & TRIGGER_MASK) === trig);
    if (other) {
      fit.pendingDup = { code, byte, rotary };
      fit.note = `${hexCode(code)} is already doing “${other.name}”. Press a different button, or use it for both.`;
      fitRearm();
      return;
    }
  }

  fit.scrollRowFlags = num(row.flags);
  row.oldButtonId = code;
  row.sourceByte = byte;
  // As Learn does (captureLearned()): a direction left over from the old
  // wheel would stop a plain button matching.
  if (!dir) row.flags = num(row.flags) & ~(FLAG_ROTARY_UP | FLAG_ROTARY_DOWN);
  if (row.fitExtra && !num(row.newLinButtonId) && protocolColumns().sameFamily) row.newLinButtonId = code;
  fit.got.set(row, { code, byte });
  fit.none.delete(row);
  fit.skipped.delete(row);
  setMappingsDirty(true);

  if (isRoller) {
    row.flags = (num(row.flags) & ~TRIGGER_MASK) | (rotary > 0 ? FLAG_ROTARY_UP : FLAG_ROTARY_DOWN);
    if (!rollerCodes.has(code)) setRollerCodes([...rollerCodes, code]);
    renderMappings();
    fit.armed = false;
    fit.waitIdle = false;
    renderFitLearn();
    openScrollWizard(mappings.indexOf(row), { fromFit: true });
    if (!wiz) {
      // Couldn't open (shouldn't happen): keep it as a plain row.
      row.flags = fit.scrollRowFlags;
      fitAdvance();
    }
    return;
  }

  renderMappings();
  fit.last = `“${row.name}” is now ${hexCode(code)}${effByte(byte) !== defaultButtonByte() ? ` (frame byte ${byte})` : ""}.`;
  fitAdvance();
}

// rows: the rows the scroll-wheel wizard put in place of the learned row, or
// null when it was cancelled.
function fitAfterScrollWizard(rows) {
  if (!fit || fit.step !== 3) return;
  const row = fitRow();
  if (rows) {
    fit.savedInStep3 = true;  // the scroll wizard saves straight away
    rows.forEach((r) => fit.got.set(r, { code: num(r.oldButtonId), byte: num(r.sourceByte) }));
    fit.queue.splice(fit.pos, 1, ...rows);
    fit.pos += rows.length - 1;
    fit.last = `Scroll wheel set up: ${rows.map((r) => r.name).join(", ")}.`;
  } else if (row) {
    row.flags = fit.scrollRowFlags;
    renderMappings();
    fit.note = `Scroll wheel setup cancelled: “${row.name}” stays on ${hexCode(row.oldButtonId)} as one row (use Split in the table later).`;
  }
  fitAdvance();
}

function fitAdvance() {
  fit.pos++;
  fit.pendingDup = null;
  if (fit.pos >= fit.queue.length) {
    fitPassDone();
    return;
  }
  renderFitLearn();
  fitRearm();
}

async function fitPassDone() {
  if (!fit) return;
  fitCancelArm();
  fit.saving = true;
  fit.saveFailed = false;
  renderFit();
  const ok = await saveSetup();
  if (!fit) return;
  fit.saving = false;
  if (!ok) {
    fit.saveFailed = true;
    renderFit();
    return;
  }
  if (fit.step === 1) fitEnterStep2();
  else fitEnterStep4();
}

function fitEnterStep2() {
  fitCancelArm();
  fit.step = 2;
  fit.swapPhase = "fit";
  fit.scanAsked = false;
  fit.offline = false;
  fitStore();
  renderFit();
}

async function fitEnterStep3() {
  if (fit.forgetRollers) {
    // The old wheel's scroll-wheel codes mean nothing on the new one and could
    // collide with one of its buttons. A roller is re-detected when scrolled.
    try {
      await fetch("/api/rollers/clear", { method: "POST" });
      setRollerCodes([]);
      renderMappings();
    } catch (e) {}
  }
  if (!fit) return;
  fit.step = 3;
  fitStore();
  fitBeginPass();
}

function fitEnterStep4() {
  fit.step = 4;
  fit.queue.forEach((r) => { delete r.fitExtra; });
  fitStore();
  renderFit();
}

function fitFinish() {
  fitCancelArm();
  mappings.forEach((r) => { delete r.fitExtra; });
  fit = null;
  fitStoreClear();
  renderFit();
  setStatus("New wheel fitted. Keep the exported backup somewhere safe.");
}

async function fitCancel() {
  if (!fit) return;
  if (fit.step === 1) {
    if (fit.got.size && !confirm("Stop, and forget the codes recorded so far?")) return;
    mappings = cloneRows(fit.snapshot || mappings);
    renderMappings();
    setMappingsDirty(false);
  } else if (fit.step === 2) {
    if (!confirm("Stop fitting a new wheel? The To car codes recorded in step 1 stay saved.")) return;
  } else if (fit.step === 3) {
    if (!confirm("Stop? The new wheel's buttons learned in this step are thrown away and the table goes back to how it was at the start of this step.")) return;
    fitCancelArm();
    mappings = cloneRows(fit.snapshot || mappings);
    renderMappings();
    const resave = fit.savedInStep3;
    fit = null;
    if (resave) await saveSetup();
    else setMappingsDirty(false);
  }
  fitCancelArm();
  fit = null;
  fitStoreClear();
  renderFit();
}

function fitSkip() {
  const row = fitRow();
  if (!row) return;
  if (!fit.got.has(row)) fit.skipped.add(row);
  fit.note = "";
  fit.last = `“${row.name}” left as it was.`;
  fitAdvance();
}

function fitNotOnWheel() {
  const row = fitRow();
  if (!row) return;
  row.oldButtonId = 0;
  row.sourceByte = 0;
  fit.got.delete(row);
  fit.skipped.delete(row);
  fit.none.add(row);
  setMappingsDirty(true);
  renderMappings();
  fit.note = "";
  fit.last = `“${row.name}” is off (From wheel 00): the new wheel has no button for it.`;
  fitAdvance();
}

function fitJump(i) {
  if (!fit || i < 0 || i >= fit.queue.length || wiz) return;
  fit.pos = i;
  fit.note = "";
  fit.pendingDup = null;
  renderFitLearn();
  fitRearm();
}

function fitAddExtra() {
  const input = document.getElementById("fitExtraName");
  const name = ((input && input.value.trim()) || "Extra button").slice(0, 17);
  if (mappings.length >= MAX_MAPPING_ROWS) {
    fit.note = `The table holds ${MAX_MAPPING_ROWS} rows — delete one to add another.`;
    renderFitLearn();
    return;
  }
  const row = {
    name, oldButtonId: 0, newLinButtonId: 0, canByteIndex: 255, canBitIndex: 0,
    resistiveOhm: 0, flags: 0, openHaldexMode: 0, sourceByte: 0, fitExtra: true,
  };
  mappings.push(row);
  setMappingsDirty(true);
  renderMappings();
  fit.queue.splice(fit.pos, 0, row);  // ask for it next
  fit.extraOpen = false;
  fit.note = "";
  renderFit();
  fitRearm();
}

function fitResumeRun() {
  const saved = fitResume;
  fitResume = null;
  if (!saved || fit) return;
  fit = newFit(saved.step);
  fit.swapPhase = saved.swapPhase === "detect" ? "detect" : "fit";
  fit.step2Since = Date.now();
  if (saved.carBytes) {
    Object.keys(saved.carBytes).forEach((k) => {
      const r = mappings[Number(k)];
      if (r && Array.isArray(saved.names) && r.name === saved.names[Number(k)]) fit.carByte.set(r, num(saved.carBytes[k]));
    });
  }
  if (fit.step === 3) {
    fitBeginPass();
  } else {
    if (fit.step === 4) fit.queue = mappings.slice();
    renderFit();
  }
}

// After every /api/setup load.
function fitOnSetupLoaded() {
  if (fit && (fit.step === 1 || fit.step === 3)) {
    // Import (or similar) replaced the table under a running step.
    fitCancelArm();
    if (fit.step === 1) fit.snapshot = cloneRows(mappings);
    fitBeginPass();
    fit.note = "The button table was reloaded, so this step started again.";
    renderFitLearn();
    return;
  }
  if (fitResumeChecked) return;
  fitResumeChecked = true;
  fitResume = fitStoreRead();
  if (fitResume) renderFit();
}

// The status poll failed: during step 2 that is the ignition going off.
function fitNoteOffline() {
  if (fit && fit.step === 2 && !fit.offline) {
    fit.offline = true;
    renderFitLive();
  }
}

function updateFitFromStatus(s) {
  if (!fit) return;
  if (fit.offline) {
    fit.offline = false;
    if (fit.step === 2 && fit.swapPhase === "fit") {
      // Back after the ignition cycle: the new wheel should be on.
      fit.swapPhase = "detect";
      fit.step2Since = Date.now();
      fit.scanAsked = false;
      fitStore();
      renderFit();
    }
  }

  if ((fit.step === 1 || fit.step === 3) && !wiz && !fit.saving && fit.pos < fit.queue.length) {
    const bi = defaultButtonByte();
    const frame = parseFrame(s.incomingLinData);
    if (!fit.rest && frame && frame[bi] === 0 && frame.some((b) => b !== 0)) fit.rest = frame;

    if (fit.waitIdle && !fit.arming && Date.now() >= fit.retryAt) {
      fit.idleCount = frameIdle(frame) ? fit.idleCount + 1 : 0;
      if (fit.idleCount >= 2 || Date.now() - fit.waitSince > 4000) {
        fit.waitIdle = false;
        fitArm();
      }
    } else if (fit.armed) {
      if (typeof s.learnProbeSeq === "number") {
        if (fit.baseSeq === null) fit.baseSeq = s.learnProbeSeq;
        if (s.learnProbeSeq !== fit.baseSeq) {
          fit.armed = false;
          onFitCapture(num(s.learnProbeCode), num(s.learnProbeByte), num(s.learnProbeRotary));
          renderFitLive();
          return;
        }
      }
      // The 10 s window ran out with nothing pressed: keep listening.
      if (!s.learnActive && Date.now() > fit.until) fitRearm();
    }
  }

  if (fit.step === 2 && fit.swapPhase === "detect" && !fit.scanAsked && linScan &&
      Date.now() - fit.step2Since > 1500) {
    fitScan();
  }
  renderFitLive();
}

function fitScan() {
  if (!fit) return;
  fit.scanAsked = true;
  fit.scanSeqAtAsk = linScan ? linScan.seq : null;
  requestLinScan();
  renderFitLive();
}

// ---- rendering ----

function fitStepsHtml() {
  return `<div class="fit-steps">${FIT_STEP_NAMES.map((n, i) => {
    const k = i + 1;
    const cls = k === fit.step ? " active" : k < fit.step ? " done" : "";
    return `<div class="fit-step${cls}"><span class="ota-step-num">${k}</span><span class="fit-step-label">${n}</span></div>`;
  }).join("")}</div>`;
}

function fitPromptText(row) {
  const name = row.name || "this button";
  const dir = rowDirection(row);
  const long = (num(row.flags) & TRIGGER_MASK) === FLAG_PRESS_LONG;
  if (fit.step === 1) {
    if (dir) return `Scroll ${dirWord(dir).toUpperCase()} one click for “${name}” on your current wheel`;
    return `${long ? "Press and hold" : "Press"} “${name}” on your current wheel`;
  }
  if (dir) return `Turn the scroll wheel you want for “${name}” ${dirWord(dir).toUpperCase()} one click`;
  return `${long ? "Press and hold" : "Press"} the button you want to do “${name}”`;
}

function fitDetectHtml() {
  if (fit.offline) {
    return `<div class="fit-wheel-status"><span class="dot"></span><span>Controller not reachable — normal while the ignition is off. Rejoin its Wi-Fi once it's back on.</span></div>`;
  }
  const wp = selectedWheelProtocol();
  let h = `<div class="fit-wheel-status"><span class="dot${lin1Healthy ? " live" : ""}"></span>` +
    `<span>${lin1Healthy ? "Wheel is talking" : "Wheel not answering"} · ${escapeAttr(WHEEL_PROTOCOL_NAMES[wp] || String(wp))}</span>` +
    `<button class="btn tiny secondary" type="button" data-fit="scan"${linScanPending ? " disabled" : ""}>${linScanPending ? "Scanning…" : "Detect wheel"}</button></div>`;
  if (!fit.scanAsked) return h;
  if (linScanPending) return h;
  if (linScanError) return h + `<div class="fit-detect warn">${escapeAttr(linScanError)}</div>`;
  if (!linScan || linScan.seq === fit.scanSeqAtAsk) return h;
  const sug = num(linScan.suggest);
  let t = `Detected ${escapeAttr(scanSuggestText(sug))}.`;
  if (sug <= 7 && sug !== wp) {
    t += ` <button class="btn tiny primary" type="button" data-fit-switch="${sug}">Switch Wheel Protocol to ${WHEEL_PROTOCOL_NAMES[sug]}</button>`;
  } else if (sug <= 7) {
    t += " That matches the selected Wheel Protocol.";
  } else if (sug === SCAN_UNKNOWN) {
    t += ` ${escapeAttr(HINT_UNKNOWN)}`;
  } else if (sug === SCAN_NOTHING) {
    t += ` ${escapeAttr(HINT_NOTHING)}`;
  }
  return h + `<div class="fit-detect">${t}</div>`;
}

function fitListHtml() {
  return fit.queue.map((row, i) => {
    const g = fit.got.get(row);
    const state = g ? hexCode(g.code)
      : fit.none.has(row) ? "off"
      : fit.skipped.has(row) ? "skipped"
      : i === fit.pos ? "now" : "";
    const cls = i === fit.pos ? "cur" : g ? "done" : "";
    return `<li class="${cls}" data-fit-jump="${i}"><span>${escapeAttr(row.name || "(no name)")}</span><span class="fit-list-v">${state}</span></li>`;
  }).join("");
}

function fitWarnings() {
  const out = [];
  mappings.forEach((m) => {
    const name = m.name || "(no name)";
    const f = num(m.flags);
    if (!num(m.oldButtonId)) return;
    const outputs = (typeof m.canByteIndex === "number" && m.canByteIndex < 8) || num(m.resistiveOhm) || (f & 0x0D);
    if (!num(m.newLinButtonId) && !outputs) {
      out.push(`“${name}” does nothing yet: set its To car code or an output in the table below.`);
    }
    const cb = fit.carByte.get(m);
    if (cb !== undefined && effByte(cb) !== effByte(m.sourceByte) && !(f & 0x0C)) {
      out.push(`“${name}”: the car's code came from frame byte ${effByte(cb)}, the new wheel sends it on byte ${effByte(m.sourceByte)}. The controller passes it on in byte ${effByte(m.sourceByte)}, which the car may not read.`);
    }
  });
  return out;
}

// Full redraw: on every step / state change, never from the status poll (it
// would wipe the selects and the name box mid-edit).
function renderFit() {
  const body = document.getElementById("fitBody");
  if (!body) return;
  body._html = undefined;

  if (!fit) {
    let h = `<p class="hint">Swapping to a different steering wheel? Four steps: record what your current wheel sends (what the car understands), fit the new wheel, pick which new button does each job, then test and save.</p>`;
    if (fitResume) {
      h += `<div class="fit-resume">A wheel swap is part-way through (step ${fitResume.step}: ${FIT_STEP_NAMES[fitResume.step - 1]}).` +
        `<div class="button-row"><button class="btn primary" type="button" data-fit="resume">Carry on</button>` +
        `<button class="btn secondary" type="button" data-fit="resume-drop">Start over</button></div></div>`;
    } else {
      h += `<div class="button-row"><button class="btn primary" type="button" data-fit="start">Start</button>` +
        `<button class="btn secondary" type="button" data-fit="start-skip" title="Skip step 1: the To car column already holds the codes the car understands">The car's codes are already set</button></div>` +
        `<p class="hint fit-small">Start with the car's own wheel (or one the car already understands) fitted and working: the Dashboard should show a button when you press it.</p>`;
    }
    body.innerHTML = h;
    return;
  }

  let h = fitStepsHtml();
  if (fit.step === 1 || fit.step === 3) {
    h += `<div id="fitWheel" class="fit-wheel"></div>`;
    if (fit.saving || fit.saveFailed) {
      h += fit.saving
        ? `<div class="fit-prompt">Saving to the controller…</div>`
        : `<div class="fit-prompt">Couldn't save.</div><div class="fit-note">Check you're still connected to the controller, then try again.</div>` +
          `<div class="button-row"><button class="btn primary" type="button" data-fit="retry-save">Try again</button></div>`;
    } else {
      h += `<div class="fit-count" id="fitCount"></div>` +
        `<div class="fit-prompt" id="fitPrompt"></div>` +
        `<div class="fit-sub" id="fitSub"></div>` +
        `<div class="fit-last" id="fitLast"></div>` +
        `<div class="fit-note" id="fitNote"></div>` +
        `<div class="button-row">` +
        `<button class="btn secondary" type="button" data-fit="dup" id="fitDupBtn" hidden>Use it for both</button>` +
        `<button class="btn secondary" type="button" data-fit="skip">Skip</button>` +
        (fit.step === 3 ? `<button class="btn secondary" type="button" data-fit="none" title="The new wheel has no button for this: turn the row off (From wheel 00)">Not on this wheel</button>` : "") +
        `<button class="btn secondary" type="button" data-fit="back">Back</button></div>`;
      if (fit.step === 3) {
        h += fit.extraOpen
          ? `<div class="fit-extra"><label>Name of the extra button<input id="fitExtraName" type="text" maxlength="17" placeholder="Extra button"></label>` +
            `<div class="button-row"><button class="btn primary" type="button" data-fit="extra-add">Add and learn it</button>` +
            `<button class="btn secondary" type="button" data-fit="extra-close">Cancel</button></div></div>`
          : `<div class="button-row"><button class="btn secondary" type="button" data-fit="extra-open">+ Add extra button</button></div>`;
      }
      h += `<details class="fit-list"><summary>All buttons in this step</summary><ol id="fitList"></ol></details>`;
    }
    h += `<div class="button-row fit-foot">` +
      (fit.saving || fit.saveFailed ? "" : `<button class="btn secondary" type="button" data-fit="pass-done">Done — leave the rest as they are</button>`) +
      `<button class="btn danger" type="button" data-fit="cancel">Cancel</button></div>`;
  } else if (fit.step === 2) {
    const wpSel = document.getElementById("wheelProtocol");
    const actSel = document.getElementById("mqbActPreset");
    const opts = (sel) => [...sel.options].map((o) =>
      `<option value="${escapeAttr(o.value)}"${o.value === sel.value ? " selected" : ""}>${escapeAttr(o.text)}</option>`).join("");
    h += `<ol class="fit-howto">` +
      `<li>Turn the ignition <strong>off</strong>.</li>` +
      `<li>Fit the new wheel: LIN, power and ground, plus horn and paddles if it has them.</li>` +
      `<li>Turn the ignition <strong>on</strong>. If your phone dropped the <strong>MFSWController</strong> Wi-Fi, rejoin it. This page carries on from here, even if you reopen it.</li>` +
      `</ol>` +
      `<div class="form-grid">` +
      `<label>New wheel's protocol<select data-fit-sel="wp">${opts(wpSel)}</select></label>` +
      (wpSel.value === "1" ? `<label>Activation preset (MQB)<select data-fit-sel="act">${opts(actSel)}</select></label>` : "") +
      (wpSel.value === "7" ? `<p class="hint">Custom: set the wheel's frames in <strong>Setup &rarr; Advanced &rarr; Custom wheel</strong> first.</p>` : "") +
      `<label class="toggle-row"><span>Forget the old wheel's scroll-wheel codes</span><input type="checkbox" data-fit-sel="forget"${fit.forgetRollers ? " checked" : ""}></label>` +
      `</div>`;
    if (fit.swapPhase === "fit") {
      h += `<div id="fitWheel" class="fit-wheel"></div>` +
        `<div class="button-row"><button class="btn primary" type="button" data-fit="fitted">The new wheel is on and the ignition is on</button></div>`;
    } else {
      h += `<div id="fitWheel" class="fit-wheel"></div>` +
        `<div class="button-row"><button class="btn primary" type="button" data-fit="next3" id="fitNext3">Next: the new wheel's buttons</button></div>`;
    }
    h += `<div class="button-row fit-foot"><button class="btn danger" type="button" data-fit="cancel">Cancel</button></div>`;
  } else {
    const warns = fitWarnings();
    h += `<p class="hint">Saved to the controller. Press each button on the new wheel: its row lights up. Then export a backup.</p>` +
      `<div class="fit-sub" id="fitPressed">Waiting for a press…</div>` +
      `<ul class="fit-test-list" id="fitTestList">${mappings.map((m, i) =>
        `<li data-row="${i}"><span class="fit-test-name">${escapeAttr(m.name || "(no name)")}</span>` +
        `<span class="fit-test-codes">${num(m.oldButtonId) ? hexCode(m.oldButtonId) : "off"} &rarr; ${hexCode(m.newLinButtonId)}</span></li>`).join("")}</ul>` +
      (warns.length ? `<ul class="fit-warn">${warns.map((w) => `<li>${escapeAttr(w)}</li>`).join("")}</ul>` : "") +
      `<div class="button-row"><button class="btn primary" type="button" data-fit="export">Export backup</button>` +
      `<button class="btn secondary" type="button" data-fit="redo3">Back to step 3</button>` +
      `<button class="btn" type="button" data-fit="finish">Finish</button></div>`;
  }
  body.innerHTML = h;
  renderFitLearn();
  renderFitLive();
}

// The prompt part of steps 1 and 3.
function renderFitLearn() {
  if (!fit || (fit.step !== 1 && fit.step !== 3) || fit.saving || fit.saveFailed) return;
  const row = fitRow();
  const total = fit.queue.length;
  setText("fitCount", row ? `${fit.step === 1 ? "Current wheel" : "New wheel"} · button ${fit.pos + 1} of ${total}` : "");
  setText("fitPrompt", row ? fitPromptText(row) : "");
  setText("fitLast", fit.last);
  setText("fitNote", fit.note);
  const dup = document.getElementById("fitDupBtn");
  if (dup) dup.hidden = !fit.pendingDup;
  const list = document.getElementById("fitList");
  if (list) setHtml(list, fitListHtml());
  renderFitSub();
}

function renderFitSub() {
  const el = document.getElementById("fitSub");
  if (!el || !fit) return;
  let t;
  if (wiz) t = "Scroll wheel setup is open…";
  else if (fit.armed) t = "Listening…";
  else if (fit.waitIdle && Date.now() - fit.waitSince > 600) t = "Let go of the button…";
  else t = "Getting ready…";
  const row = fitRow();
  if (fit.step === 3 && row && !rowDirection(row)) t += " (For a scroll wheel, turn it DOWN one click.)";
  if (el.textContent !== t) el.textContent = t;
  el.classList.toggle("on", !!fit.armed);
}

// Live parts, refreshed on every status poll.
function renderFitLive() {
  if (!fit) return;
  const wheel = document.getElementById("fitWheel");
  if (wheel) setHtml(wheel, fitDetectHtml());
  if (fit.step === 1 || fit.step === 3) renderFitSub();
  if (fit.step === 2) {
    const next = document.getElementById("fitNext3");
    if (next) {
      const t = lin1Healthy ? "Next: the new wheel's buttons" : "Continue anyway";
      if (next.textContent !== t) next.textContent = t;
      next.classList.toggle("primary", lin1Healthy);
      next.classList.toggle("secondary", !lin1Healthy);
    }
  }
  if (fit.step === 4) {
    setText("fitPressed", lastPressText ? `Last press: ${lastPressText}` : "Waiting for a press…");
    document.querySelectorAll("#fitTestList li[data-row]").forEach((li) => {
      li.classList.toggle("live", Number(li.dataset.row) === liveRowIdx);
    });
  }
}

function wireFitWizard() {
  const card = document.getElementById("fitCard");
  if (!card) return;
  card.addEventListener("click", (e) => {
    const sw = e.target.closest("[data-fit-switch]");
    if (sw && fit) {
      setWheelProtocol(Number(sw.dataset.fitSwitch), false);
      if (fit.step === 2) renderFit(); else renderFitLive();
      return;
    }
    const jump = e.target.closest("[data-fit-jump]");
    if (jump && fit) {
      fitJump(Number(jump.dataset.fitJump));
      return;
    }
    const b = e.target.closest("[data-fit]");
    if (!b) return;
    switch (b.dataset.fit) {
      case "start": fitStart(false); break;
      case "start-skip": fitStart(true); break;
      case "resume": fitResumeRun(); break;
      case "resume-drop":
        if (!confirm("Start over? (The To car codes already saved stay in the table.)")) break;
        fitResume = null;
        fitStoreClear();
        renderFit();
        break;
      case "skip": fitSkip(); break;
      case "none": fitNotOnWheel(); break;
      case "back": if (fit.pos > 0) fitJump(fit.pos - 1); break;
      case "dup": {
        const d = fit.pendingDup;
        if (d) {
          fit.pendingDup = null;
          fitCancelArm();
          onFitCapture(d.code, d.byte, d.rotary, true);
        }
        break;
      }
      case "extra-open":
        fit.extraOpen = true;
        renderFit();
        { const el = document.getElementById("fitExtraName"); if (el) el.focus(); }
        break;
      case "extra-close": fit.extraOpen = false; renderFit(); break;
      case "extra-add": fitAddExtra(); break;
      case "pass-done": fitPassDone(); break;
      case "retry-save": fitPassDone(); break;
      case "cancel": fitCancel(); break;
      case "scan": fitScan(); break;
      case "fitted":
        fit.swapPhase = "detect";
        fit.step2Since = Date.now();
        fit.scanAsked = false;
        fitStore();
        renderFit();
        break;
      case "next3": fitEnterStep3(); break;
      case "export": exportSettings(); break;
      case "redo3": fit.step = 3; fitStore(); fitBeginPass(); break;
      case "finish": fitFinish(); break;
    }
  });
  card.addEventListener("change", (e) => {
    const el = e.target.closest("[data-fit-sel]");
    if (!el || !fit) return;
    if (el.dataset.fitSel === "wp") {
      setWheelProtocol(Number(el.value), false);
      fit.scanAsked = false;  // the old result was about the other protocol
      renderFit();
    } else if (el.dataset.fitSel === "act") {
      // Same path as Setup's own preset select: fills the bytes and saves.
      const preset = document.getElementById("mqbActPreset");
      preset.value = el.value;
      preset.dispatchEvent(new Event("change"));
    } else if (el.dataset.fitSel === "forget") {
      fit.forgetRollers = el.checked;
    }
  });
  card.addEventListener("keydown", (e) => {
    if (e.target.id === "fitExtraName" && e.key === "Enter") fitAddExtra();
  });
  renderFit();
}

