#include "LIN.h"

#include "API.h"
#include "globals.h"
#include "io.h"

static bool lockLinBus(SemaphoreHandle_t mutex)
{
  if (mutex == nullptr)
  {
    return true;
  }

  return xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlockLinBus(SemaphoreHandle_t mutex)
{
  if (mutex != nullptr)
  {
    xSemaphoreGive(mutex);
  }
}

// LIN monitor: log any received frame whose bytes changed since it was last
// seen (first sight always logs). Keyed by (bus, protected ID); only ever
// called from the steering-wheel LIN task, so no locking is required.
static void logLinFrameIfChanged(uint8_t bus, uint8_t id, const uint8_t* data, uint8_t len)
{
  static uint8_t lastSeen[2][0x40][8] = {};
  static bool    seen[2][0x40] = {};

  const uint8_t b = (bus == 2) ? 1 : 0;
  if (id > 0x3F || len == 0 || len > 8)
  {
    return;
  }

  if (seen[b][id] && memcmp(lastSeen[b][id], data, len) == 0)
  {
    return;
  }
  // Byte 0 of the button frame is a free-running counter that changes every
  // poll; ignore a change that is ONLY in byte 0 so it doesn't flood the log.
  // Except while a control is active (button byte non-zero): the scroll wheel
  // reports movement per frame, so two polls that each say "+1" are two
  // detents and must both appear — collapsing them made a 2x scroll read as 1.
  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
  const bool active = bi < len && data[bi] != 0;
  if (!active && seen[b][id] && len > 1 && memcmp(lastSeen[b][id] + 1, data + 1, len - 1) == 0)
  {
    memcpy(lastSeen[b][id], data, len);
    return;
  }
  memcpy(lastSeen[b][id], data, len);
  seen[b][id] = true;

  char hex[3 * 8 + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < len; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
  }
  if (pos > 0) hex[pos - 1] = '\0';  // trim trailing space

  logLine("%s 0x%02X: %s", (bus == 2) ? "LIN2" : "LIN1", id, hex);
}

// Short label for a LIN_Master_Base error bitmask, for passthru logging.
static const char* linErrorLabel(LIN_Master_Base::error_t err)
{
  if (err == LIN_Master_Base::NO_ERROR)
  {
    return "OK";
  }

  static char buf[32];
  size_t pos = 0;
  auto append = [&](const char* s) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s,", s);
  };
  if (err & LIN_Master_Base::ERROR_TIMEOUT) append("TIMEOUT");
  if (err & LIN_Master_Base::ERROR_CHK)     append("CHK");
  if (err & LIN_Master_Base::ERROR_ECHO)    append("ECHO");
  if (err & LIN_Master_Base::ERROR_STATE)   append("STATE");
  if (err & LIN_Master_Base::ERROR_MISC)    append("MISC");
  if (pos > 0)
  {
    buf[pos - 1] = '\0';  // trim trailing comma
  }
  else
  {
    buf[0] = '?';
    buf[1] = '\0';
  }
  return buf;
}

// Passthru-mode logging: log EVERY poll/send attempt (success or failure, any
// length) rather than only successful, changed frames — so a bench session
// captures the true wire state including timeouts and checksum failures.
//
// Notes the LIN version WE requested — this codebase always requests V2
// (enhanced checksum) for every transaction. There is no way to also report
// whether a frame would have validated under classic V1 instead: the library
// only exposes the data bytes and an aggregate error flag to application
// code (getFrame() copies bufRx+3 regardless of error state, which is how
// error frames still show real bytes here), not the raw PID/checksum bytes
// needed to independently recompute both checksum types. Doing that would
// require either patching the vendored library (fragile — .pio/libdeps gets
// regenerated) or a from-scratch raw-UART sniffer bypassing it entirely.
static void logLinAttempt(uint8_t bus, uint8_t id, const uint8_t* data, uint8_t len, LIN_Master_Base::error_t err,
                          bool classic = false)
{
  char hex[3 * 8 + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < len && i < 8; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
  }
  if (pos > 0)
  {
    hex[pos - 1] = '\0';
  }
  else
  {
    hex[0] = '\0';
  }

  logLine("%s 0x%02X %s %s len=%u: %s", (bus == 2) ? "LIN2" : "LIN1", id, classic ? "v1" : "v2",
          linErrorLabel(err), (unsigned)len, hex);
}

// Drain and log whatever raw bytes are still sitting in the UART's RX FIFO
// immediately after a receiveSlaveResponseBlocking() attempt, independent of
// what logLinAttempt() reported.
//
// The vendored LIN_Master library only ever copies bytes out of the UART once
// AT LEAST the full expected length has arrived (see _receiveFrame() in
// LIN_master_HardwareSerial.cpp: `if (available() >= lenRx-1) readBytes(...)`
// — otherwise it leaves them untouched and reports ERROR_TIMEOUT once its
// window expires). A slave that answers with a different byte count, or under
// the "wrong" checksum version, therefore leaves real bytes stranded in the
// FIFO — invisible to logLinAttempt()'s buf/error output, and logged
// identically to true silence (len=4 of zeroes, TIMEOUT). This reads out
// anything left behind so a passthru capture can tell the two apart.
//
// Safe to call after any attempt (success or failure): the library's own
// _sendBreak() unconditionally flushes and drains the port before the next
// transaction, so bytes we don't consume here would just be discarded anyway.
static void logLinRawResidual(uint8_t bus, uint8_t id, HardwareSerial& port)
{
  if (!port.available())
  {
    return;
  }

  uint8_t raw[16];
  size_t n = 0;
  while (port.available() && n < sizeof(raw))
  {
    raw[n++] = (uint8_t)port.read();
  }

  char hex[3 * sizeof(raw) + 1];
  size_t pos = 0;
  for (size_t i = 0; i < n; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[i]);
  }
  if (pos > 0)
  {
    hex[pos - 1] = '\0';
  }
  else
  {
    hex[0] = '\0';
  }

  logLine("%s 0x%02X RAW residual n=%u: %s", (bus == 2) ? "LIN2" : "LIN1", id, (unsigned)n, hex);
}

// Log the LIN library's own internal capture buffer (BREAK, SYNC, PID, DATA,
// CHK) exactly as it landed — bypassing getFrame()'s assumption that our
// requested length matches the real frame's length.
//
// Complements logLinRawResidual(): that one shows bytes the library never
// consumed (pure TIMEOUT — nothing reached the "full expected length" gate).
// This one shows bytes it DID consume once that gate was reached — i.e. a
// checksum failure, where the frame structurally completed but the trailing
// byte didn't validate. On a genuine timeout the buffer is stale (untouched
// since whatever the last completed capture was), so it's only meaningful
// when TIMEOUT isn't set — a real capture happened this cycle, valid or not.
//
// Why this matters: if the real frame is SHORTER than the NumData we asked
// for, the trailing bytes we label "data" and "checksum" actually belong to
// whatever comes next on the bus (its own BREAK/SYNC/PID) — the checksum
// mismatch we see wouldn't be a wrong algorithm, it'd be us misreading the
// frame boundary entirely. Seeing the raw sequence (a literal 0x55 turning up
// where data was expected is the tell) is the only way to tell these apart.
// Requires LIN_Master_Base::getRawRxBuffer() — a small local patch to the
// vendored library (now kept in lib/LIN master portable/, not pulled via
// lib_deps, specifically so this patch survives rebuilds).
template <typename LinMaster>
static void logLinRawFrame(uint8_t bus, uint8_t id, LinMaster& lin, LIN_Master_Base::error_t err)
{
  if (err & LIN_Master_Base::ERROR_TIMEOUT)
  {
    return;  // bufRx wasn't refreshed this cycle -- stale, not meaningful
  }

  uint8_t len = 0;
  const uint8_t* raw = lin.getRawRxBuffer(len);

  char hex[3 * 12 + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < len && i < 12; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[i]);
  }
  if (pos > 0)
  {
    hex[pos - 1] = '\0';
  }
  else
  {
    hex[0] = '\0';
  }

  logLine("%s 0x%02X FULLBUF(brk,sync,pid,data..,chk) len=%u: %s", (bus == 2) ? "LIN2" : "LIN1", id, (unsigned)len, hex);
}

// ---------------------------------------------------------------------------
// Poll health, logged in NORMAL mode too. Before V1.10 a normal-mode LIN
// Monitor only ever showed frames that arrived intact and had changed, so a
// wheel speaking another protocol (the Audi A8: different ID, length and
// checksum) left it completely blank — exactly what "nothing connected" looks
// like. Now each polled frame reports when it stops answering (with the reason
// and whatever bytes did arrive), every 10 s while it stays down, and when it
// comes back. Passthru logs every attempt itself, so this stays quiet there.
// ---------------------------------------------------------------------------
struct PollHealth
{
  uint8_t  id = 0xFF;
  bool     failing = false;
  uint32_t fails = 0;
  uint32_t failSinceMs = 0;
  uint32_t lastReportMs = 0;
  LIN_Master_Base::error_t lastErr = LIN_Master_Base::NO_ERROR;
};

constexpr uint32_t kPollReportEveryMs = 10000;

// Returns true when the caller should also log the raw bytes of this attempt
// (logLinRawResidual / logLinRawFrame) — i.e. when a line was just written.
// `remind`: repeat every 10 s while down. Only for the button frame — plenty
// of wheels never answer the accessory / temperature frames at all, and a
// reminder forever would fill the log; those report when they change.
static bool notePollResult(PollHealth& h, uint8_t bus, uint8_t id, LIN_Master_Base::error_t err,
                           bool remind = false)
{
  if (h.id != id)  // frame ID changed (protocol / setting): start over
  {
    h = PollHealth();
    h.id = id;
  }
  if (passthruEnabled)
  {
    return false;
  }

  const uint32_t now = millis();
  const char* busName = (bus == 2) ? "LIN2" : "LIN1";
  if (err == LIN_Master_Base::NO_ERROR)
  {
    if (h.failing)
    {
      logLine("%s 0x%02X answering again (after %lu failed polls)", busName, id, (unsigned long)h.fails);
    }
    h.failing = false;
    h.fails = 0;
    return false;
  }

  h.fails++;
  if (!h.failing || err != h.lastErr)
  {
    logLine("%s 0x%02X no valid answer (%s)", busName, id, linErrorLabel(err));
    if (!h.failing) h.failSinceMs = now;
    h.failing = true;
    h.lastErr = err;
    h.lastReportMs = now;
    return true;
  }
  if (remind && now - h.lastReportMs >= kPollReportEveryMs)
  {
    logLine("%s 0x%02X still no valid answer (%s): %lu polls, %lus", busName, id, linErrorLabel(err),
            (unsigned long)h.fails, (unsigned long)((now - h.failSinceMs) / 1000));
    h.lastReportMs = now;
    return true;
  }
  return false;
}

// LIN1 timing diagnostic (V1.19). Every MISSED LIN1 frame (the first failure
// after it was answering - the health log only shows the first of a streak,
// so isolated misses were invisible) gets one "TIMING" line, so we can see
// WHY frames are missed instead of guessing. From the library's per-transaction timing
// (getTiming(), a local patch), all in ms after the BREAK started:
//   body    when the rest of the header was written (nominal 2 byte times,
//           ~1.04 ms at 19200 - much more means the task was held up)
//   echo    when our own BREAK+SYNC+PID had come back (0 = never)
//   gave up when the library declared the timeout, and its limit
//   gap     the longest pause between two runs of the library's frame loop -
//           a stalled task (another task, flash write...) shows up here
// Then it waits up to 20 ms for the rest of the frame and reports when it
// was complete ("+x late") and whether the echo read back as 00 55 PID.
// Reading the bytes empties the FIFO, so the RAW residual line after it is
// usually empty - the bytes are on this line instead. At most one line per
// 250 ms, and nothing in Passthru (which logs every attempt itself).
// Only a frame that WAS answering: a frame this wheel never answers (e.g. an
// RS6 and 0x0F) fails every poll by design, and timing that would just wait
// 20 ms and fill the log forever. So it runs on the first failure after a
// good answer - a missed frame - and not again until the frame answers.
template <typename LinMaster>
static void logLinTiming(const PollHealth& h, uint8_t id, LinMaster& lin, HardwareSerial& port,
                         LIN_Master_Base::error_t err)
{
  if (err == LIN_Master_Base::NO_ERROR || passthruEnabled)
  {
    return;
  }
  if (h.id == id && h.failing)
  {
    return;  // already failing: a silent frame, not a missed one
  }
  static uint32_t lastMs = 0;
  const uint32_t nowMs = millis();
  if (lastMs != 0 && nowMs - lastMs < 250)
  {
    return;
  }
  lastMs = nowMs | 1U;

  const auto t = lin.getTiming();
  const bool timedOut = (err & LIN_Master_Base::ERROR_TIMEOUT) != 0;

  // The bytes: on a timeout they are still arriving / in the FIFO, so wait up
  // to 20 ms for the full frame; otherwise the library already read them.
  uint8_t raw[16];
  uint8_t n = 0;
  uint32_t completeUs = 0;
  if (timedOut)
  {
    const uint32_t waitStart = micros();
    while (micros() - waitStart < 20000)
    {
      if (port.available() >= t.lenRx)
      {
        completeUs = micros() - t.startUs;
        break;
      }
    }
    while (port.available() && n < sizeof(raw))
    {
      raw[n++] = (uint8_t)port.read();
    }
  }
  else
  {
    uint8_t len = 0;
    const uint8_t* buf = lin.getRawRxBuffer(len);
    for (n = 0; n < len && n < sizeof(raw); n++) raw[n] = buf[n];
    completeUs = t.doneUs;
  }

  char hex[3 * sizeof(raw) + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < n; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[i]);
  }
  hex[pos ? pos - 1 : 0] = '\0';

  const uint8_t pid = (uint8_t)(((id ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01) << 6 |
                                ((~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 0x01) << 7) |
                                (id & 0x3F));
  const char* echo = (n < 3) ? "missing" : (raw[1] == 0x55 && raw[2] == pid) ? "ok" : "BAD";

  // Two lines: a log line holds 96 characters.
  logLine("LIN1 0x%02X TIMING %s: body %.2f echo %.2f gap %.2f, gave up %.2f/%.2f ms",
          id, linErrorLabel(err), t.bodyUs / 1000.0f, t.headerUs / 1000.0f, t.maxGapUs / 1000.0f,
          t.doneUs / 1000.0f, t.timeoutUs / 1000.0f);
  if (completeUs)
  {
    logLine("LIN1 0x%02X TIMING done %.2f (%+.2f) %u/%u echo %s: %s", id, completeUs / 1000.0f,
            ((int32_t)(completeUs - t.doneUs)) / 1000.0f, (unsigned)n, (unsigned)t.lenRx, echo, hex);
  }
  else
  {
    logLine("LIN1 0x%02X TIMING never done %u/%u echo %s: %s", id, (unsigned)n, (unsigned)t.lenRx, echo, hex);
  }
}

// One poll's result, logged per notePollResult(): the health line, then (when
// it reported) the bytes that did arrive — stranded in the FIFO on a timeout,
// or the full captured frame on a checksum error. Draining the FIFO is
// harmless: the library flushes it before the next transaction anyway.
template <typename LinMaster>
static void reportPoll(PollHealth& h, uint8_t id, LinMaster& lin, HardwareSerial& port, LIN_Master_Base::error_t err,
                       bool remind = false)
{
  logLinTiming(h, id, lin, port, err);  // V1.19 diagnostic: each missed frame, timed
  if (notePollResult(h, 1, id, err, remind))
  {
    logLinRawResidual(1, id, port);
    logLinRawFrame(1, id, lin, err);
  }
}

static PollHealth healthButton, healthAcc, healthTemp, healthAudiStatus, healthLight;

// The button-frame byte a mapping matches on. 0 (or out of range) means "use
// the global default byte" — keeps pre-existing/hand-added rows on byte 1 while
// Learn assigns paddles/horn their real byte (6/7).
static inline uint8_t mappingSourceByte(const ButtonMapping& m)
{
  if (m.sourceByte >= 1 && m.sourceByte < 8) return m.sourceByte;
  return (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
}

// The byte of the car's frame the row's "To car" code goes in (V1.21):
// carByte when set, else the same byte the wheel sent it on.
static inline uint8_t mappingCarByte(const ButtonMapping& m)
{
  if (m.carByte >= 1 && m.carByte < 8) return m.carByte;
  return mappingSourceByte(m);
}

// Put row m's code in the car's frame. When the car reads it on another byte
// than the wheel sends it on (an RS6 wheel's paddles on byte 6 -> an A8 D3's
// byte 7), the wheel's own value in its byte is cleared, so it isn't passed
// on as well.
static inline void placeCarCode(uint8_t* frame, const ButtonMapping& m)
{
  const uint8_t sb = mappingSourceByte(m);
  const uint8_t cb = mappingCarByte(m);
  if (cb != sb) frame[sb] = 0;
  frame[cb] = m.newLinButtonId;
}

// Free-running nibble counter for the chassis-bound button frame's byte 0,
// matching real MQB gateway captures where that byte increments each send.
// Shared across both chassis protocols; PQ additionally tags it with 0x80.
static uint8_t nextChassisCounter()
{
  static uint8_t counter = 0;
  counter = (counter + 1) & 0x0F;
  return counter;
}

// Build the 8-byte button frame sent AS MASTER to the chassis/BCM, shaped for
// whichever chassisProtocol is selected — independent of wheelProtocol, so
// any wheel <-> chassis combination can be bridged. Byte 1 is always the
// mapped button code (already protocol-specific via the button table).
//   MQB: today's existing, already-validated shape (bytes 0/4 left at 0) —
//        kept as-is so no currently-working setup changes.
//   PQ:  byte 0 = rolling counter tagged with 0x80, byte 4 = 0x60 marker,
//        modelled on github.com/Dimka8901/MQB-MFSW-PQ25's mqbToPq() — worth
//        confirming against a real PQ chassis capture, not independently
//        verified against real hardware here.
static void buildChassisButtonFrame(uint8_t buttonCode, uint8_t* out)
{
  memset(out, 0, 8);
  out[1] = buttonCode;
  if (chassisProtocol == CHASSIS_PROTOCOL_PQ)
  {
    out[0] = nextChassisCounter() | 0x80;
    out[4] = 0x60;
  }
}

// Read the low nibble of `v` as a signed 4-bit value (0x0F -> -1).
static inline int8_t signedNibble(uint8_t v)
{
  const uint8_t n = (uint8_t)(v & 0x0F);
  return (n & 0x08) ? (int8_t)(n - 16) : (int8_t)n;
}

// Reshape a wheel frame into the layout the chassis family expects.
//
// The two families lay frame 0x0E out differently. Measured from a PQ wheel on
// a real PQ car (docs/lin_monitor_log_10_non_passthru.csv) and an MQB wheel on
// the bench:
//
//          byte 0         2     3                        4     6
//   PQ     0xF0 | count   FF    0xF0 | absolute position 42    30
//   MQB    0x80 | count   00    signed movement, 00 idle 31    00
//
// Byte 1 is the button code on both and is translated by the caller from the
// mapping table; bytes 5 and 7 behave alike on both and are left untouched.
//
// Byte 3 is the part that cannot simply be relayed across families: a PQ
// chassis reads it as a position that persists while nothing is moving, an MQB
// one as movement belonging to that frame alone. Handing a PQ chassis an MQB
// delta would park it at "position 0" whenever the control is idle; the reverse
// would report a rotation on every frame. So one side has to integrate and the
// other differentiate, which needs state carried between polls.
//
// Same-family pairs return untouched — that path is confirmed against a real PQ
// car. The cross-family conversions are derived from those two captures and
// have NOT been tested against a chassis of the opposite family.
static void shapeFrameForChassis(uint8_t* frame)
{
  const bool wheelIsPq   = wheelIsPqFamily();  // PQ and NSF (Up!) send a PQ frame
  const bool chassisIsPq = (chassisProtocol == CHASSIS_PROTOCOL_PQ);
  if (wheelIsPq == chassisIsPq)
  {
    return;
  }

  const uint8_t rb = linRotaryByteIndex;
  if (rb >= 8)
  {
    return;  // rotary disabled: nothing to convert, and no byte to convert it in
  }

  static uint8_t position   = 0;      // MQB -> PQ: running total of the deltas
  static uint8_t lastSeen   = 0;      // PQ -> MQB: previous position read
  static bool    haveLast   = false;

  if (chassisIsPq)
  {
    position = (uint8_t)((position + signedNibble(frame[rb])) & 0x0F);
    frame[rb] = (uint8_t)(0xF0 | position);
    frame[0] = (uint8_t)(0xF0 | (frame[0] & 0x0F));
    frame[2] = 0xFF;
    frame[4] = 0x42;
    frame[6] = 0x30;
    return;
  }

  const uint8_t pos  = (uint8_t)(frame[rb] & 0x0F);
  const int8_t  move = haveLast ? signedNibble((uint8_t)(pos - lastSeen)) : 0;
  lastSeen = pos;
  haveLast = true;
  frame[rb] = (uint8_t)(move & 0x0F);
  frame[0] = (uint8_t)(0x80 | (frame[0] & 0x0F));
  frame[2] = 0x00;
  frame[4] = 0x31;
  frame[6] = 0x00;
}

// `rotary` is the scroll movement read from the same frame the code was
// captured from (0 when there is none, or the code came from another frame).
static void captureLearned(uint8_t value, uint8_t byteIdx, int8_t rotary)
{
  expireLearnState();
  if (!learnActive)
  {
    return;
  }

  // Wizard capture: hand the result to the UI and leave the table alone. The
  // sign is reported as-is — the wizard knows which way it asked for, so a +1
  // needs no disambiguating here (see the note below for why Learn does).
  if (learnTarget == LEARN_PROBE)
  {
    if (isRollerDelta(rotary))
    {
      markRollerCode(value);
    }
    portENTER_CRITICAL(&stateMux);
    learnProbeCode = value;
    learnProbeByte = byteIdx;
    learnProbeRotary = rotary;
    learnProbeSeq++;
    portEXIT_CRITICAL(&stateMux);
    logLine("[SW] learn probe: code 0x%02X byte %u d=%+d", value, (unsigned)byteIdx, (int)rotary);
    clearLearnState();
    return;
  }

  if (learnRowIndex >= buttonMappingCount)
  {
    return;
  }

  if (learnTarget == LEARN_OLD_LIN)
  {
    ButtonMapping& m = buttonMappings[learnRowIndex];
    m.oldButtonId = value;
    m.sourceByte = byteIdx;

    // Scroll direction is inferred only when it is certain. Plain buttons move
    // the rotary byte too (their press stage: 1, then 4/5/6 while held —
    // lin_wheel_20260913_160641.csv), so a +1 alone could be either; taking
    // its sign would tag ordinary buttons "Scroll up". But a button's first
    // frame is always stage 1 and never negative (scroll.csv, 02/10/2026), so
    // a negative or +2/+3 step on the captured frame can only be a roller, as
    // can any code already seen doing that. Learning a roller by scrolling it
    // DOWN therefore always lands as a Scroll down row.
    // (getButtonState() has normally marked it already, from this same frame.)
    if (isRollerDelta(rotary))
    {
      markRollerCode(value);
    }
    if (isRollerCode(value) && rotary != 0)
    {
      // Press-stage bits are meaningless on a roller (its byte 3 is movement,
      // not a hold counter), so the direction replaces the whole trigger.
      m.flags = (uint8_t)((m.flags & ~kTriggerMask) |
                          (rotary > 0 ? FLAG_ROTARY_UP : FLAG_ROTARY_DOWN));
    }
    else
    {
      // Not (yet) known as a roller: a direction left over from whatever this
      // row was learned as before would stop it matching a plain button.
      m.flags = (uint8_t)(m.flags & ~(FLAG_ROTARY_UP | FLAG_ROTARY_DOWN));
    }
  }
  else if (learnTarget == LEARN_NEW_LIN)
  {
    // The car's code AND where it was found (V1.21), learned from whatever
    // byte changed - so a car that reads a button on another byte than the
    // new wheel sends it on still gets it in the right place.
    buttonMappings[learnRowIndex].newLinButtonId = value;
    buttonMappings[learnRowIndex].carByte = (byteIdx >= 1 && byteIdx < 8) ? byteIdx : 0;
  }
  // LEARN_NEW_CAN removed: CAN assignment is now explicit byte+bit selection, not learned

  mappingsRevision++;
  clearLearnState();
}

// Learn the button/paddle/horn from a live frame: capture the first byte (1..7,
// skipping the rolling counter in byte 0) that has changed from the idle
// baseline to a non-zero value, recording both its position and value.
// Requires a TRANSITION away from the baseline, not merely a difference from
// it: the previous frame must have agreed with the baseline and this one must
// not. A byte that simply sits at a constant value can therefore never be
// captured, however the baseline was obtained — which is the failure that
// produced rows matching every frame forever.
static void captureLearnedFromFrame(const uint8_t* frame)
{
  static uint8_t prev[8] = {};
  static bool    havePrev = false;

  if (!learnActive)
  {
    havePrev = false;
    return;
  }

  // A byte that returns to 0 is at rest, whatever the baseline said: if Learn
  // was armed while a button was still held, that held code became the
  // baseline, the frame never "agreed" with it again after release, and no
  // later press on that byte could ever be captured. (Bytes resting at a
  // non-zero constant — PQ FF/42/30, MQB 31 — never read 0, so are untouched.)
  for (uint8_t i = 1; i < 8; i++)
  {
    if (frame[i] == 0 && learnBaseline[i] != 0)
    {
      learnBaseline[i] = 0;
    }
  }

  if (havePrev)
  {
    for (uint8_t i = 1; i < 8; i++)
    {
      if (frame[i] != 0 && frame[i] != learnBaseline[i] && prev[i] == learnBaseline[i])
      {
        memcpy(prev, frame, sizeof(prev));
        // Movement belongs to the code on the button byte only; a paddle or
        // horn captured from byte 6/7 says nothing about scrolling.
        const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
        captureLearned(frame[i], i, (i == bi) ? wheelRotaryDelta : 0);
        return;
      }
    }
  }

  memcpy(prev, frame, sizeof(prev));
  havePrev = true;
}

// Standard LIN parity-protected ID (LIN2.0 spec "2.3.1.3 Protected identifier
// field"), replicated locally to match LIN_Master_Base::_calculatePID() —
// needed here because we no longer send our own header for these IDs (see
// serviceChassisLinBus() below) and so never get the library to compute it for us.
static uint8_t calculateProtectedId(uint8_t id)
{
  id &= 0x3F;
  const uint8_t p0 = (uint8_t)((id ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01);
  const uint8_t p1 = (uint8_t)(~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 0x01);
  return (uint8_t)(id | (p0 << 6) | (p1 << 7));
}

// LIN2.x "enhanced" checksum (sum includes the protected ID byte). Matches
// LIN_Master_Base::_calculateChecksum()'s LIN_V2 path exactly — cross-checked
// against a real captured wheel button frame (00 55 8E F3 00 FF F0 42 00 30
// 00 1A: PID 0x8E + that data comes out to checksum 0x1A, matching the byte
// actually received on the wire).
static uint8_t calculateEnhancedChecksum(uint8_t pid, const uint8_t* data, uint8_t numData)
{
  uint16_t chk = pid;
  for (uint8_t i = 0; i < numData; i++)
  {
    chk += data[i];
    if (chk > 255)
    {
      chk -= 255;
    }
  }
  return (uint8_t)(0xFF - (uint8_t)chk);
}

static uint8_t calculateClassicChecksum(const uint8_t* data, uint8_t numData)
{
  uint16_t chk = 0;
  for (uint8_t i = 0; i < numData; i++)
  {
    chk += data[i];
    if (chk > 255)
    {
      chk -= 255;
    }
  }
  return (uint8_t)(0xFF - (uint8_t)chk);
}

// For a frame body d[0..n-1] followed by its checksum d[n]: 1 = classic
// valid, 2 = enhanced valid, 0 = neither. (A frame valid under both — only
// possible by coincidence — reports enhanced.)
static uint8_t checksumKind(uint8_t pid, const uint8_t* d, uint8_t n)
{
  if (calculateEnhancedChecksum(pid, d, n) == d[n]) return 2;
  if (calculateClassicChecksum(d, n) == d[n]) return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// LIN1 wheel detect / ID scan — the "LIN scanner" a customer had to write for
// himself to find out his A8 wheel answers 0x30, built in. Every ID 0x00-0x3B
// is asked for a slave response; anything that answers is reported with its
// real length and checksum type, whichever it is:
//   * answered 8 bytes, enhanced  -> the library accepts it outright;
//   * answered 8 bytes, classic   -> CHK error, full frame in getRawRxBuffer();
//   * answered fewer bytes        -> TIMEOUT, bytes stranded in the UART FIFO
//                                    (after our own break/sync/PID echo).
// Runs by itself when the button frame has had no valid answer for 5 s (at
// most once a minute — nothing is working at that point anyway, so the 1-3 s
// the scan blocks LIN1 costs nothing), or on request from the UI.
// ---------------------------------------------------------------------------
LinScanHit         linScanHits[kLinScanMaxHits] = {};
volatile uint8_t   linScanHitCount = 0;
volatile uint32_t  linScanSeq = 0;
volatile uint32_t  linScanMs = 0;
volatile uint8_t   linScanSuggest = LIN_SCAN_NONE;

static volatile bool linScanRequested = false;
static uint32_t buttonLastOkMs = 0;
static uint32_t buttonFailSinceMs = 0;

void requestLinScan()
{
  linScanRequested = true;
}

static void noteButtonFrameResult(bool ok)
{
  const uint32_t now = millis();
  if (ok)
  {
    buttonLastOkMs = now;
    buttonFailSinceMs = 0;
  }
  else if (buttonFailSinceMs == 0)
  {
    buttonFailSinceMs = now | 1U;
  }
}

static const char* linScanSuggestLabel(uint8_t s)
{
  switch (s)
  {
  case WHEEL_PROTOCOL_PQ:       return "PQ wheel (0x0E)";
  case WHEEL_PROTOCOL_MQB:      return "MQB wheel (0x0E)";
  case WHEEL_PROTOCOL_AUDI:     return "VAG LIN 1.3 wheel (0x30: Audi / early PQ35)";
  case WHEEL_PROTOCOL_NSF:      return "VW Up! / Citigo / Mii wheel (0x0E + 0x0C)";
  case WHEEL_PROTOCOL_VOLVO_P1: return "Volvo P1 wheel (0x20 at 9600)";
  case WHEEL_PROTOCOL_VOLVO_P3: return "Volvo P3 wheel (0x30 / 0x20 at 9600)";
  case LIN_SCAN_NOTHING:        return "nothing answered";
  default:                      return "unknown wheel";
  }
}

static const LinScanHit* findHit(const LinScanHit* hits, uint8_t n, uint8_t id, uint16_t baud)
{
  for (uint8_t i = 0; i < n; i++)
  {
    if (hits[i].id == id && hits[i].baud == baud) return &hits[i];
  }
  return nullptr;
}

// Recognise a wheel from what answered. Only signatures seen on real wheels
// (or in published code for them — docs/wheel-protocol-research.md).
static uint8_t classifyLinScan(const LinScanHit* hits, uint8_t n)
{
  if (n == 0) return LIN_SCAN_NOTHING;

  const LinScanHit* vw = findHit(hits, n, linButtonID, 19200);
  if (vw && vw->len == 8 && !vw->classic)
  {
    // Up!/Citigo/Mii wheels also answer 0x0C (4 bytes; riquenunes/vw-steering-wheel-gamepad).
    const LinScanHit* nsf = findHit(hits, n, 0x0C, 19200);
    if (nsf && nsf->len == 4) return WHEEL_PROTOCOL_NSF;
    // Idle constants differ: PQ bytes 2/4/6 = FF/42/30, MQB 00/31/00.
    if (vw->data[2] == 0xFF || vw->data[4] == 0x42) return WHEEL_PROTOCOL_PQ;
    if (vw->data[4] == 0x31) return WHEEL_PROTOCOL_MQB;
    return (wheelProtocol == WHEEL_PROTOCOL_MQB) ? WHEEL_PROTOCOL_MQB : WHEEL_PROTOCOL_PQ;
  }
  const LinScanHit* lin13 = findHit(hits, n, audiButtonID, 19200);
  if (lin13 && lin13->len == audiButtonLen && lin13->classic)
  {
    return WHEEL_PROTOCOL_AUDI;
  }
  // Volvo runs its wheel bus at 9600: P3 answers 0x30 (left) and 0x20
  // (right), P1 only 0x20 with 4 bytes.
  if (findHit(hits, n, 0x30, 9600)) return WHEEL_PROTOCOL_VOLVO_P3;
  const LinScanHit* p1 = findHit(hits, n, 0x20, 9600);
  if (p1 && p1->len == 4) return WHEEL_PROTOCOL_VOLVO_P1;
  return LIN_SCAN_UNKNOWN;
}

// Ask `id` for a slave response whose length and checksum type are not known,
// and work both out from the bytes. Caller holds steeringWheelLinMutex.
//   * answered 8 bytes, enhanced -> the library accepts it outright;
//   * answered 8 bytes, classic  -> CHK error, full frame in getRawRxBuffer();
//   * answered fewer bytes       -> TIMEOUT, bytes stranded in the UART FIFO
//                                   (after the echo of our own break/sync/PID).
// Returns the data length found (0: no valid answer); `err` is the library's.
static uint8_t probeSlaveResponse(uint8_t id, uint8_t* data, bool& classic, LIN_Master_Base::error_t& err)
{
  uint8_t buf[8] = {};
  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, id, 8, buf);
  err = steeringWheelLIN.getError();
  const uint8_t pid = calculateProtectedId(id);
  classic = false;

  if (err == LIN_Master_Base::NO_ERROR)
  {
    memcpy(data, buf, 8);
    return 8;
  }
  if (!(err & LIN_Master_Base::ERROR_TIMEOUT))
  {
    uint8_t rawLen = 0;
    const uint8_t* raw = steeringWheelLIN.getRawRxBuffer(rawLen);  // brk sync pid d0..d7 chk
    if (rawLen >= 12 && checksumKind(pid, raw + 3, 8) == 1)
    {
      classic = true;
      memcpy(data, raw + 3, 8);
      return 8;
    }
    return 0;
  }

  uint8_t r[16];
  uint8_t rn = 0;
  while (Serial1.available() && rn < sizeof(r))
  {
    r[rn++] = (uint8_t)Serial1.read();
  }
  for (uint8_t p = 1; p < rn; p++)
  {
    if (r[p - 1] != 0x55 || r[p] != pid) continue;
    const uint8_t* d = r + p + 1;
    const uint8_t m = (uint8_t)(rn - p - 1);  // response bytes incl. checksum
    for (int8_t k = (int8_t)((m > 9 ? 9 : m) - 1); k >= 1; k--)
    {
      const uint8_t kind = checksumKind(pid, d, (uint8_t)k);
      if (kind)
      {
        classic = (kind == 1);
        memcpy(data, d, (size_t)k);
        return (uint8_t)k;
      }
    }
    break;
  }
  return 0;
}

// LIN1 speed. VW / Audi wheels run 19200, Volvo 9600, GM reportedly 10417. The
// library re-applies its own rate on every transaction (setBaudrate() patch),
// so changing it is cheap. Caller holds steeringWheelLinMutex.
static uint16_t wheelLinBaudNow = linBaud;
static void setWheelLinBaud(uint16_t baud)
{
  if (baud == wheelLinBaudNow) return;
  wheelLinBaudNow = baud;
  steeringWheelLIN.setBaudrate(baud);
  Serial1.updateBaudRate(baud);
}

uint16_t profileWheelBaud();  // wheel-profile section below

static void runLinScan(bool manual)
{
  if (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS)
  {
    // The K-bus has no master to ask with: the wheel only ever talks first.
    if (manual) logLine("[SCAN] not available with the BMW K-bus profile (the wheel sends unprompted)");
    return;
  }

  LinScanHit hits[kLinScanMaxHits] = {};
  uint8_t n = 0;

  // 19200 first (every VW-group wheel), 9600 only if nothing answered there.
  static const uint16_t kScanBauds[] = {19200, 9600};
  for (uint16_t baud : kScanBauds)
  {
    if (n > 0) break;
    for (uint8_t id = 0x00; id <= 0x3B && n < kLinScanMaxHits; id++)
    {
      if (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS)
      {
        break;  // switched to the K-bus mid-scan: stop transmitting at once
      }
      if (!lockLinBus(steeringWheelLinMutex))
      {
        continue;
      }
      setWheelLinBaud(baud);
      LinScanHit h = {};
      h.id = id;
      h.baud = baud;
      LIN_Master_Base::error_t err;
      h.len = probeSlaveResponse(id, h.data, h.classic, err);
      unlockLinBus(steeringWheelLinMutex);

      if (h.len)
      {
        hits[n++] = h;
      }
      vTaskDelay(1);  // let the other tasks on this core breathe between IDs
    }
  }
  if (lockLinBus(steeringWheelLinMutex))
  {
    setWheelLinBaud(profileWheelBaud());  // back to the profile's own speed
    unlockLinBus(steeringWheelLinMutex);
  }

  const uint8_t suggest = classifyLinScan(hits, n);

  // Same IDs/lengths/checksums as last time and not asked for: say nothing —
  // an unplugged wheel would otherwise log "nothing answered" every minute.
  static uint32_t lastSig = 0;
  uint32_t sig = 2166136261UL;
  for (uint8_t i = 0; i < n; i++)
  {
    sig = (sig ^ hits[i].id) * 16777619UL;
    sig = (sig ^ (uint32_t)(hits[i].len | (hits[i].classic ? 0x80 : 0))) * 16777619UL;
    sig = (sig ^ hits[i].baud) * 16777619UL;
  }
  const bool changed = (linScanSeq == 0) || sig != lastSig;
  lastSig = sig;

  portENTER_CRITICAL(&stateMux);
  memcpy(linScanHits, hits, sizeof(linScanHits));
  linScanHitCount = n;
  linScanSuggest = suggest;
  linScanMs = millis();
  linScanSeq++;
  portEXIT_CRITICAL(&stateMux);

  if (!manual && !changed)
  {
    return;
  }
  logLine("[SCAN] LIN1 %s scan: %u ID(s) answered", manual ? "manual" : "auto", (unsigned)n);
  for (uint8_t i = 0; i < n; i++)
  {
    char hex[3 * 8 + 1];
    size_t pos = 0;
    for (uint8_t b = 0; b < hits[i].len; b++)
    {
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", hits[i].data[b]);
    }
    hex[pos ? pos - 1 : 0] = '\0';
    logLine("[SCAN] 0x%02X %u bytes %s @%u: %s", hits[i].id, (unsigned)hits[i].len,
            hits[i].classic ? "classic" : "enhanced", (unsigned)hits[i].baud, hex);
  }
  if (suggest == LIN_SCAN_NOTHING)
  {
    logLine("[SCAN] nothing answered at 19200 or 9600: check wheel power/ground, the LIN wire, and Setup > Legacy PCB");
  }
  else if (suggest == LIN_SCAN_UNKNOWN)
  {
    logLine("[SCAN] unknown wheel: please Export (CSV) this log and send it to Forbes Automotive");
  }
  else
  {
    logLine("[SCAN] looks like: %s%s", linScanSuggestLabel(suggest),
            suggest == wheelProtocol ? "" : " - not the selected Wheel Protocol");
  }
}

void serviceLinScan()
{
  const uint32_t now = millis();
  if (linScanRequested)
  {
    linScanRequested = false;
    runLinScan(true);
    return;
  }
  static uint32_t lastAutoMs = 0;
  if (passthruEnabled || wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS ||
      buttonFailSinceMs == 0 || now - buttonFailSinceMs < 5000)
  {
    return;
  }
  if (lastAutoMs != 0 && now - lastAutoMs < 60000)
  {
    return;
  }
  lastAutoMs = now | 1U;
  runLinScan(false);
}

// Block (briefly) for exactly `n` more bytes on Serial2, for use immediately
// after a header match where the rest of the frame is expected within a few
// byte-times. Returns false (with whatever partial bytes it got left in
// `out`) if `n` isn't reached before `timeoutMs`.
static bool collectBytes(uint8_t* out, uint8_t n, uint32_t timeoutMs)
{
  uint8_t got = 0;
  const uint32_t start = millis();
  while (got < n && (millis() - start) < timeoutMs)
  {
    if (Serial2.available())
    {
      out[got++] = (uint8_t)Serial2.read();
    }
  }
  return got == n;
}

// The 8 bytes this device answers the BCM's button-frame poll with in NORMAL
// (non-passthru) mode: the wheel's own frame with the matched button's code
// replaced by its translated value. Rebuilt once per LIN1 cycle by
// sendButtonLINFrame(); read by serviceChassisLinBus() under stateMux.
//
// Only the button code is rewritten — every other byte is passed through
// exactly as the wheel sent it, because they carry state the button code alone
// does not. Byte 0 is a counter advancing once per poll on both wheel types,
// and byte 3 reports rotary movement — but NOT the same way, so it cannot be
// interpreted without knowing which wheel sent it:
//   PQ  wheel: 0xF0 | absolute 4-bit position, held at its last value when
//              idle; direction is the difference between consecutive frames.
//   MQB wheel: signed 4-bit movement for that frame alone (0x0F = -1),
//              returning to 0x00 when idle.
// Bytes 2/4/6 are constants that also differ per wheel (PQ FF/42/30, MQB
// 00/31/00). Relaying verbatim keeps all of it correct for a same-family
// wheel/chassis pair; a cross-family bridge would need byte 3 converted
// between those two representations, which this does not attempt. A frame
// synthesised from zeroes (buildChassisButtonFrame()) drops the lot, which is
// why the chassis never responded properly outside passthru.
static uint8_t chassisResponseFrame[8] = {};

// ---------------------------------------------------------------------------
// LIN 1.3 car (CHASSIS_PROTOCOL_LIN13, V1.17): what the car gets when it asks
// for the wheel's own frames. Requested by a customer fitting an RS6 wheel to
// an Audi A8 D3 (Messenger, 24/11/2026). From his captures:
//
//   docs/lin_monitor_log.csv-5..10, 13.json  the D3's own wheel (bench)
//     0x30  6 bytes classic   d0 = counter 0-F, d1 = 90 A0 10 20 30 40 50 60
//                             by (d0 & 7), d2 = button code (0 at rest),
//                             d3 = 08 while the heated-wheel button is held,
//                             d4 = roller nibbles, d5 = rim temperature
//                             (~44-4B at 21 C, rises when heated, FF = sensor
//                             open)
//     0x21  2 bytes classic   23 01, never changes
//   docs/lin_monitor_log.csv-15.json  passthru in the car (V1.16 relay)
//     0x01  car -> wheel      5E 03 / 5F 03 alternating (bench master: 50 03)
//     0x10  car -> wheel      3 bytes, only while lane assist vibrates:
//                             F3 / F5 / F8 = weak / middle / high, byte 1
//                             toggles bit 5 (35/15, 38/18, 3D/1D) - not
//                             decoded further, and NOT passed to the new
//                             wheel (its vibration frame isn't known yet);
//                             it shows in the LIN Monitor as a LIN2 frame.
//
// Built from chassisResponseFrame (the translated, MQB-shaped frame), the
// inverse of decodeAudiFrame(): byte 1 code -> d2, or 0xF1 / 0xF2 -> a
// movement in d4's high / low nibble (from byte 3); byte 2 -> d0's high
// nibble; byte 7 -> d3. So a row's "new" code is the car's 0x30 byte 2 code,
// F1 / F2 for its rollers, and a row whose source byte is 7 sets d3 bits.
//
// The heated-wheel button: a VW wheel reports it only in 0x3A byte 1 (bit 0
// clear while held), so each press (wheelHeaterPressMs, set on the press edge
// only) holds d3 = 08 for kLin13HeatHoldMs - one press of the D3's own button,
// which the car toggles the heater on.
//
// d5, the rim temperature, is what the car regulates the heater on, so it is
// only ever the wheel's real reading (RS6 0x3A byte 0 reads ~46 at 21 C, the
// D3's sensor 44-4B, so the scales look alike - NOT confirmed). Stale (> 3 s),
// no sensor, or at/over 0x62 (VW's cut-off) -> FF "sensor open", which the car
// treats as a fault rather than a cold rim.
// ---------------------------------------------------------------------------
constexpr uint8_t  kLin13ChassisStatus[2] = {0x23, 0x01};  // the D3 wheel's 0x21
constexpr uint8_t  kLin13D1[8] = {0x90, 0xA0, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
constexpr uint32_t kLin13HeatHoldMs = 300;
constexpr uint8_t  kLin13TempCutoff = 0x62;

static void encodeLin13ChassisFrame(const uint8_t* mqb, uint8_t* out)
{
  static uint8_t counter = 0;
  counter = (uint8_t)((counter + 1) & 0x0F);

  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
  const uint8_t rb = linRotaryByteIndex;
  const uint8_t code = mqb[bi];
  const uint8_t move = (rb < 8) ? (uint8_t)(mqb[rb] & 0x0F) : 0;

  memset(out, 0, audiButtonLen);
  out[0] = (uint8_t)(((mqb[2] & 0x0F) << 4) | counter);
  out[1] = kLin13D1[counter & 0x07];
  if (code == 0xF1)      out[4] = (uint8_t)(move << 4);
  else if (code == 0xF2) out[4] = move;
  else                   out[2] = code;
  out[3] = mqb[7];

  const uint32_t now = millis();
  const uint32_t pressMs = wheelHeaterPressMs;
  if (pressMs != 0 && now - pressMs < kLin13HeatHoldMs)
  {
    out[3] |= 0x08;
  }

  const uint32_t tMs = wheelTempMs;
  const uint8_t t = wheelTempRaw;
  const bool fresh = tMs != 0 && now - tMs < 3000 && wheelTempSrc != WHEEL_TEMP_SRC_NONE;
  // heaterTempOffset ("warmer by", Setup): the car is told the rim is that
  // many steps colder, so it heats that much further. The cut-off is still on
  // the wheel's real reading.
  const uint8_t ofs = heaterTempOffset;
  uint8_t shown = (t > ofs) ? (uint8_t)(t - ofs) : 0x01;
  out[5] = (fresh && t < kLin13TempCutoff) ? shown : 0xFF;
}

// Answer a LIN 1.3 car's header for `id` on LIN2: classic checksum, the
// D3 wheel's own lengths. Returns false for an ID we don't answer.
static bool answerLin13Chassis(uint8_t id)
{
  uint8_t frame[audiButtonLen + 1];
  uint8_t n = 0;
  if (id == audiButtonID)
  {
    uint8_t mqb[8];
    portENTER_CRITICAL(&stateMux);
    memcpy(mqb, chassisResponseFrame, sizeof(mqb));
    portEXIT_CRITICAL(&stateMux);
    encodeLin13ChassisFrame(mqb, frame);
    n = audiButtonLen;
  }
  else if (id == audiStatusID)
  {
    memcpy(frame, kLin13ChassisStatus, sizeof(kLin13ChassisStatus));
    n = audiStatusLen;
  }
  else
  {
    return false;
  }

  frame[n] = calculateClassicChecksum(frame, n);
  Serial2.write(frame, n + 1);
  Serial2.flush();
  uint8_t echo[audiButtonLen + 1];
  collectBytes(echo, n + 1, 10);  // our own bytes, read back by the transceiver
  chassisLinLastOkMs = millis();

  if (id == audiButtonID)
  {
    portENTER_CRITICAL(&stateMux);
    memset(lastLinOutFrame, 0, sizeof(lastLinOutFrame));
    memcpy(lastLinOutFrame, frame, n);
    lastLinOutLen = sizeof(lastLinOutFrame);
    lastLinOutId = audiButtonID;
    portEXIT_CRITICAL(&stateMux);
  }
  return true;
}

// Continuously listens to the chassis/BCM bus (LIN2) for ANY valid header
// (SYNC + protected ID) and reacts according to that ID's real role on the
// bus — this device is a SLAVE on LIN2, never the master (see below), so it
// must react to headers the BCM sends on its own schedule rather than issue
// its own.
//
// LIN2 is NOT our bus to master. A passthru capture showed valid, correctly
// parity'd LIN headers for OTHER frame IDs (0x15, 0x19, ...) arriving
// unprompted on this wire — proof a real LIN master (the BCM) is already
// running its own schedule on the chassis segment for EVERY frame on it,
// buttons included. Confirmed on the bench: bridging the wheel's LIN pin
// straight to the chassis LIN pin (bypassing this controller entirely) works
// with no issues at all, which only happens if the BCM is genuinely mastering
// the button ID too and the wheel (a slave-only device) is answering it
// directly — there is no second, independent master role for us to imitate
// by sending our own header.
//
//   - Light frame (linLightInId): an UNCONDITONAL frame — the BCM transmits
//     the header AND its own data bytes itself. We only ever decode it.
//   - Button frame (linButtonInId), ONLY while passthruEnabled: a
//     SLAVE-RESPONSE frame — the BCM sends the header ALONE and waits for an
//     answer. Passthru's whole job is to stand in for the (electrically
//     unreachable, on a different physical LIN segment) wheel here: the
//     instant this device sees that header, it must answer with the wheel's
//     real, last-polled data — otherwise the BCM times out waiting on a
//     slave that never answers, which reads on the car side as "no comms",
//     regardless of how healthy LIN1 (the actual wheel link) looks.
//
// Must run in a TIGHT, near-continuous loop (see chassisLinListenerTask() in
// tasks.cpp) — unlike everything else in this file, a LIN slave has only a
// few byte-times after the PID to start its response, nowhere near this
// device's normal ~100 ms LIN1 poll cadence. Must never call
// receiveSlaveResponseBlocking() or sendMasterRequestBlocking() for the light
// ID (both send a break+header first, competing with the BCM's own).
// ---------------------------------------------------------------------------
// LIN2 capture of every frame this device does NOT handle itself, in normal
// mode too: whatever the car's master publishes on the wheel's bus (backlight,
// heated-wheel status, wake, other slaves' answers) lands in the LIN Monitor
// with its real length and checksum type. This is how an unknown car's
// backlight frame gets found: plug LIN2 into the car's wheel wire and read it
// off the log — no knowledge of the car needed in advance.
//
// Collected without blocking: after an unhandled header, following bytes are
// gathered until the bus goes quiet (2.5 ms) or the next header starts, then
// the longest body whose last byte is a valid checksum (classic or enhanced)
// is taken as the frame. Logged on first sight and when it changes. A byte that
// changes on every frame (a counter or CRC) is left out of "has it changed"
// after 4 changes in a row, so counters don't flood the 200-line log; anything
// else (brightness turning) still shows. At most one line per ID per 250 ms.
// ---------------------------------------------------------------------------
// The most a LIN frame carries after its PID: 8 data bytes + the checksum.
constexpr uint8_t kLinMaxResp = 9;

struct Lin2FrameLog
{
  uint8_t  last[kLinMaxResp];
  uint8_t  logged[kLinMaxResp];
  uint8_t  streak[kLinMaxResp];
  uint8_t  loggedLen;
  bool     seen;
  bool     silentLogged;
  uint32_t logMs;
  uint32_t badMs;
};
static Lin2FrameLog lin2Log[0x40] = {};

// kind: 2 = enhanced checksum, 1 = classic, 0 = no valid checksum (`buf` is then
// every byte as received, the last one included).
static void logFrameOnChange(Lin2FrameLog& s, uint8_t bus, uint8_t id, const uint8_t* buf, uint8_t len, uint8_t kind);

static bool passthruRelayActive();

static void recordLin2Frame(uint8_t pid, const uint8_t* buf, uint8_t n)
{
  const uint8_t id = (uint8_t)(pid & 0x3F);
  Lin2FrameLog& s = lin2Log[id];
  const uint32_t now = millis();
  const bool relay = passthruRelayActive();

  if (n == 0)
  {
    if (relay)
    {
      return;  // the relay logs "the car asks for this" itself (relayLogRoles())
    }
    // The car asked, nobody here answered (e.g. it polls a wheel that isn't
    // on this wire). Worth knowing once: it names the frame the car expects.
    if (!s.silentLogged)
    {
      s.silentLogged = true;
      logLine("LIN2 0x%02X header from the car, no answer on this bus", id);
    }
    return;
  }

  uint8_t len = 0;
  uint8_t kind = 0;
  for (int8_t k = (int8_t)((n > 9 ? 9 : n) - 1); k >= 1; k--)
  {
    kind = checksumKind(pid, buf, (uint8_t)k);
    if (kind)
    {
      len = (uint8_t)k;
      break;
    }
  }

  if (!len && relay)
  {
    // Passthru relays it to the wheel exactly as received whatever its
    // checksum (or none), so log it like any other frame: on change.
    logFrameOnChange(s, 2, id, buf, n > kLinMaxResp ? kLinMaxResp : n, 0);
    return;
  }
  if (!len)
  {
    if (s.badMs == 0 || now - s.badMs >= 10000)
    {
      char hex[3 * 10 + 1];
      size_t pos = 0;
      for (uint8_t i = 0; i < n && i < 10; i++)
      {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
      }
      hex[pos ? pos - 1 : 0] = '\0';
      s.badMs = now | 1U;
      logLine("LIN2 0x%02X %u bytes, no valid checksum: %s", id, (unsigned)n, hex);
    }
    return;
  }

  logFrameOnChange(s, 2, id, buf, len, kind);
}

// The change-detection half of recordLin2Frame(), shared with the wheel
// profiles' LIN1 frames: log on first sight and on change; a byte that changes
// on every frame (counter, CRC) stops counting after 4 changes in a row; at
// most one line per ID per 250 ms.
static void logFrameOnChange(Lin2FrameLog& s, uint8_t bus, uint8_t id, const uint8_t* buf, uint8_t len, uint8_t kind)
{
  const uint32_t now = millis();
  if (len > kLinMaxResp) len = kLinMaxResp;
  char hex[3 * kLinMaxResp + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < len; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
  }
  hex[pos ? pos - 1 : 0] = '\0';

  bool differ = !s.seen || len != s.loggedLen;
  for (uint8_t i = 0; i < len; i++)
  {
    if (s.seen && buf[i] != s.last[i])
    {
      if (s.streak[i] < 255) s.streak[i]++;
    }
    else
    {
      s.streak[i] = 0;
    }
    if (s.streak[i] < 4 && buf[i] != s.logged[i])
    {
      differ = true;
    }
  }
  memcpy(s.last, buf, len);
  if (!differ || (s.seen && now - s.logMs < 250))
  {
    s.seen = true;
    return;  // unchanged, or too soon: a change still differs from `logged` next frame
  }
  s.seen = true;
  memcpy(s.logged, buf, len);
  s.loggedLen = len;
  s.logMs = now;
  logLine("%s 0x%02X: %s (%u bytes, %s)", bus == 2 ? "LIN2" : "LIN1", id, hex, (unsigned)len,
          kind == 2 ? "enhanced" : kind == 1 ? "classic" : "no valid checksum");
}

// ---------------------------------------------------------------------------
// Passthru relay (V1.16): with Passthru Mode on and a car talking on LIN2, the
// module stands in the wheel wire as a transparent frame relay, whatever the
// frame IDs, lengths and checksums (LIN 2.x enhanced, LIN 1.x classic, or
// none valid at all) — nothing is decoded, computed or checksummed by us:
//
//   car publishes a frame (header + its own data)  -> sent to the wheel on
//     LIN1 byte for byte, checksum byte included (backlight, ignition / wake,
//     heater on/off, vibration... whatever the car sends);
//   car asks (header, then silence on LIN2)         -> the wheel is asked the
//     same ID on LIN1, and its answer, byte for byte as received, is what
//     the car gets the next time it asks.
//
// Which of the two an ID is gets learned from the bus: an ID is first watched
// without answering — data from the car means "car sends", silence means "car
// asks". So each ID the car asks for goes unanswered exactly once, and the car
// always gets the wheel's previous answer (one schedule cycle old, ~10-50 ms).
// The wheel is asked as often as the car asks for it, so LIN1 mirrors the car's
// schedule. LIN1 runs at the wheel profile's speed (Setup > Wheel Protocol),
// LIN2 at 19200. Not used with the BMW K-bus profile (not LIN).
//
// Until the car has been heard (3 headers since Passthru was switched on) the
// old bench behaviour runs instead: the profile's button frame is polled and
// its light frame sent, every attempt logged. Once a car has been heard that
// stops for good (until Passthru is switched off and on), so a car going to
// sleep isn't kept awake by the module polling its wheel.
// ---------------------------------------------------------------------------
enum : uint8_t { RELAY_UNKNOWN = 0, RELAY_CAR_ASKS = 1, RELAY_CAR_SENDS = 2 };

struct RelayId
{
  uint8_t  role;
  uint8_t  pid;                      // protected ID exactly as the car sent it
  bool     pollWanted;               // the car asked since the wheel was last asked
  bool     sendPending;              // a car frame waiting to go to the wheel
  uint8_t  sendLen;
  uint8_t  sendBuf[kLinMaxResp];
  uint8_t  ansLen;                   // the wheel's last answer, verbatim (0 = none)
  uint8_t  ansBuf[kLinMaxResp];
  uint32_t ansMs;
};
// Shared by the LIN2 listener and the LIN1 task: every access under stateMux.
static RelayId relayIds[0x40] = {};
static volatile uint32_t relayCarMs = 0;       // last header from the car
static volatile uint8_t  relayCarHeaders = 0;  // headers heard since Passthru went on (capped)
constexpr uint8_t  kRelayCarHeadersNeeded = 3;
// The cached answer is always one car cycle old (the wheel is asked when the car
// asks), and each ask replaces it — or clears it if the wheel has gone quiet. So
// this only guards against LIN1 having stalled; it must exceed the slowest ID's
// period in the car's schedule (some are polled only every second or so).
constexpr uint32_t kRelayAnswerMaxAgeMs = 10000;

static bool passthruRelayActive()
{
  return passthruEnabled && wheelProtocol != WHEEL_PROTOCOL_BMW_KBUS;
}

// LIN2 task only: role per ID as last logged, and IDs whose role changed.
static uint8_t  relayRoleLogged[0x40] = {};
static uint64_t relayRoleDirty = 0;

// A car frame has ended on LIN2 (LIN2 task): n bytes after the PID, 0 = silence.
static void relayCarFrame(uint8_t pid, const uint8_t* buf, uint8_t n)
{
  const uint8_t id = (uint8_t)(pid & 0x3F);
  if (n > kLinMaxResp) n = kLinMaxResp;
  const uint8_t role = n ? RELAY_CAR_SENDS : RELAY_CAR_ASKS;
  portENTER_CRITICAL(&stateMux);
  RelayId& r = relayIds[id];
  r.role = role;
  r.pid = pid;
  if (n)
  {
    memcpy(r.sendBuf, buf, n);
    r.sendLen = n;
    r.sendPending = true;
    r.ansLen = 0;
  }
  else
  {
    r.pollWanted = true;
  }
  portEXIT_CRITICAL(&stateMux);

  if (relayRoleLogged[id] != role)
  {
    relayRoleLogged[id] = role;
    relayRoleDirty |= (1ULL << id);
  }

  // Keep the Dashboard / health tiles fed: any frame with a valid checksum
  // means LIN2 is alive; the light ID in its usual VW shape is the BCM light.
  if (n >= 2 && checksumKind(pid, buf, (uint8_t)(n - 1)))
  {
    chassisLinLastOkMs = millis();
    if (n == 5 && pid == calculateProtectedId(linLightInId) && checksumKind(pid, buf, 4) == 2)
    {
      memcpy(gatewayLightData, buf, 4);
      memcpy(steeringWheelLightData, buf, sizeof(steeringWheelLightData));
      chassisLightLastOkMs = millis();
    }
  }
}

// LIN2 task, after the FIFO is drained: one line per ID whose role changed.
static void relayLogRoles()
{
  while (relayRoleDirty)
  {
    const uint8_t id = (uint8_t)__builtin_ctzll(relayRoleDirty);
    relayRoleDirty &= ~(1ULL << id);
    if (relayRoleLogged[id] == RELAY_CAR_ASKS)
    {
      logLine("[RELAY] LIN2 0x%02X: the car asks for this; passing the wheel's answer back", id);
    }
    else
    {
      logLine("[RELAY] LIN2 0x%02X: the car sends this; passing it to the wheel", id);
    }
  }
}

// ---- LIN1 side (steering-wheel LIN task, steeringWheelLinMutex held) --------
static uint32_t lin1BitUs()
{
  return (1000000UL + wheelLinBaudNow / 2) / (wheelLinBaudNow ? wheelLinBaudNow : linBaud);
}

// BREAK + SYNC + the car's own PID, written straight to the wheel UART (the
// library always appends its own checksum, which is exactly what must not
// happen here). The BREAK is a 0x00 at half speed: 9 bits low = 18 bit times.
static void lin1RawHeader(uint8_t pid)
{
  Serial1.flush();
  while (Serial1.available()) Serial1.read();
  Serial1.updateBaudRate(wheelLinBaudNow / 2);
  Serial1.write((uint8_t)0x00);
  Serial1.flush();
  Serial1.updateBaudRate(wheelLinBaudNow);
  const uint8_t hdr[2] = {0x55, pid};
  Serial1.write(hdr, sizeof(hdr));
  Serial1.flush();
}

// Position just after "55 PID" (our own header's echo) in a LIN1 capture, or -1.
static int lin1AfterHeader(const uint8_t* r, uint8_t n, uint8_t pid)
{
  for (uint8_t p = 1; p < n; p++)
  {
    if (r[p - 1] == 0x55 && r[p] == pid) return p + 1;
  }
  return -1;
}

// Everything LIN1 receives after our header: ends 3 byte times after the last
// byte once anything beyond our own header echo has arrived, or after
// `waitUs` if nothing has (a slave may take up to 1.4x the frame time to
// answer, and the ESP32 UART hands bytes over ~1 ms late). Also ends as soon as
// `wantAfter` bytes have followed the header (what we sent ourselves, or a
// full 9-byte answer).
static uint8_t lin1CollectRaw(uint8_t* out, uint8_t max, uint8_t pid, uint32_t waitUs, uint8_t wantAfter)
{
  const uint32_t bit = lin1BitUs();
  const uint32_t gapUs = 30 * bit + 1500;            // 3 byte times of silence
  const uint32_t hardUs = waitUs + 140 * bit + 3000;  // whole frame, worst case
  const uint32_t start = micros();
  uint32_t last = start;
  uint8_t n = 0;
  while (true)
  {
    while (Serial1.available())
    {
      const uint8_t b = (uint8_t)Serial1.read();
      if (n < max) out[n++] = b;
      last = micros();
    }
    const uint32_t now = micros();
    const int p = lin1AfterHeader(out, n, pid);
    const bool more = (p >= 0) && (n > p);  // bytes after our header
    if ((more && (n - p >= wantAfter || now - last >= gapUs)) ||
        (!more && now - start >= waitUs && now - last >= gapUs) || now - start >= hardUs)
    {
      break;
    }
    delayMicroseconds(100);
  }
  return n;
}

static Lin2FrameLog relayWheelLog[0x40] = {};
static uint8_t      relayWheelState[0x40] = {};  // 0 unknown, 1 answering, 2 silent, 3 bus fault

static void relayNoteWheel(uint8_t id, uint8_t state, uint8_t failedSendLen)
{
  if (relayWheelState[id] == state) return;
  const uint8_t was = relayWheelState[id];
  relayWheelState[id] = state;
  if (state == 2)
  {
    logLine("[RELAY] LIN1 0x%02X: the wheel doesn't answer (the car asks for it); the car gets no answer either", id);
  }
  else if (state == 3)
  {
    logLine("[RELAY] LIN1 0x%02X: our own %u bytes didn't read back on LIN1 - wheel bus fault (wiring, "
            "transceiver asleep, Legacy PCB setting?)", id, (unsigned)failedSendLen);
  }
  else if (was == 2 || was == 3)
  {
    logLine("[RELAY] LIN1 0x%02X: OK again", id);
  }
}

// A car frame to the wheel, byte for byte.
static void relaySendToWheel(uint8_t id, uint8_t pid, const uint8_t* buf, uint8_t n)
{
  lin1RawHeader(pid);
  Serial1.write(buf, n);
  Serial1.flush();
  uint8_t r[16];
  const uint8_t rn = lin1CollectRaw(r, sizeof(r), pid, 0, n);
  const int p = lin1AfterHeader(r, rn, pid);
  const bool echoOk = (p >= 0) && (rn - p >= n) && memcmp(r + p, buf, n) == 0;
  relayNoteWheel(id, echoOk ? 1 : 3, n);
  if (echoOk) swLinLastOkMs = millis();
}

// Ask the wheel for a frame the car asks for, and keep its answer verbatim.
static void relayPollWheel(uint8_t id, uint8_t pid)
{
  lin1RawHeader(pid);
  uint8_t r[16];
  const uint32_t respUs = 140 * lin1BitUs() + 1500;  // 1.4 x (8 data + checksum), + UART latency
  const uint8_t rn = lin1CollectRaw(r, sizeof(r), pid, respUs, kLinMaxResp);
  const int p = lin1AfterHeader(r, rn, pid);
  uint8_t m = 0;
  const uint8_t* d = nullptr;
  if (p >= 0)
  {
    d = r + p;
    m = (uint8_t)(rn - p);
    if (m > kLinMaxResp) m = kLinMaxResp;
  }

  portENTER_CRITICAL(&stateMux);
  RelayId& rel = relayIds[id];
  rel.ansLen = m;
  if (m) memcpy(rel.ansBuf, d, m);
  rel.ansMs = millis();
  portEXIT_CRITICAL(&stateMux);

  if (p < 0)
  {
    relayNoteWheel(id, 3, 2);
    return;
  }
  if (m == 0)
  {
    relayNoteWheel(id, 2, 0);
    return;
  }
  relayNoteWheel(id, 1, 0);
  swLinLastOkMs = millis();

  // Log on change, with the checksum the wheel used (or none valid).
  const uint8_t kind = (m >= 2) ? checksumKind(pid, d, (uint8_t)(m - 1)) : 0;
  logFrameOnChange(relayWheelLog[id], 1, id, d, kind ? (uint8_t)(m - 1) : m, kind);

  // Dashboard "incoming LIN (wheel)": the wheel's button frame.
  const uint8_t btnId = (wheelProtocol == WHEEL_PROTOCOL_AUDI) ? audiButtonID : (uint8_t)(linButtonInId & 0x3F);
  if (id == btnId)
  {
    portENTER_CRITICAL(&stateMux);
    memset(lastLinInFrame, 0, sizeof(lastLinInFrame));
    const uint8_t dl = kind ? (uint8_t)(m - 1) : m;
    memcpy(lastLinInFrame, d, dl > sizeof(lastLinInFrame) ? sizeof(lastLinInFrame) : dl);
    lastLinInLen = sizeof(lastLinInFrame);
    lastLinInId = id;
    portEXIT_CRITICAL(&stateMux);
  }
}

bool servicePassthruRelay()
{
  static bool wasOn = false;
  static bool announced = false;
  const bool on = passthruRelayActive();
  if (on != wasOn)
  {
    wasOn = on;
    announced = false;
    // Fresh start each time Passthru goes on: roles are learned again.
    portENTER_CRITICAL(&stateMux);
    memset(relayIds, 0, sizeof(relayIds));
    relayCarHeaders = 0;
    relayCarMs = 0;
    portEXIT_CRITICAL(&stateMux);
    memset(relayWheelState, 0, sizeof(relayWheelState));
    memset(relayWheelLog, 0, sizeof(relayWheelLog));
    if (on) logLine("[RELAY] Passthru on: relaying the car once it is heard on LIN2 (bench mode until then)");
  }
  if (!on || relayCarHeaders < kRelayCarHeadersNeeded)
  {
    return false;  // bench behaviour (or normal mode)
  }

  if (!announced)
  {
    announced = true;
    logLine("[RELAY] car heard on LIN2: relaying every frame both ways, checksums untouched");
  }

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return true;
  }
  setWheelLinBaud(profileWheelBaud());
  for (uint8_t id = 0; id < 0x40; id++)
  {
    uint8_t buf[kLinMaxResp];
    uint8_t n = 0;
    uint8_t pid = 0;
    bool send = false;
    bool poll = false;
    portENTER_CRITICAL(&stateMux);
    RelayId& r = relayIds[id];
    pid = r.pid;
    if (r.sendPending)
    {
      send = true;
      n = r.sendLen;
      memcpy(buf, r.sendBuf, n);
      r.sendPending = false;
    }
    else if (r.pollWanted && r.role == RELAY_CAR_ASKS)
    {
      poll = true;
      r.pollWanted = false;
    }
    portEXIT_CRITICAL(&stateMux);

    if (send)
    {
      relaySendToWheel(id, pid, buf, n);
    }
    else if (poll)
    {
      relayPollWheel(id, pid);
    }
  }
  unlockLinBus(steeringWheelLinMutex);
  return true;
}

void serviceChassisLinBus()
{
  static uint8_t window[2] = {};  // rolling SYNC, PID
  static uint8_t windowLen = 0;
  static uint8_t beforeWindow = 0xFF;  // byte just ahead of the window (a header's BREAK reads 0x00)

  // Unhandled frame being collected (see recordLin2Frame()).
  static uint8_t  unkPid = 0;
  static bool     unkActive = false;
  static uint8_t  unkBuf[12];
  static uint8_t  unkLen = 0;
  static uint32_t unkLastUs = 0;

  // A finished unhandled frame waits here and is logged only once the RX FIFO
  // is drained (end of this call): logLine() also writes to the serial console,
  // which can block for milliseconds — never before answering the BCM.
  static uint8_t pendPid = 0;
  static uint8_t pendBuf[12];
  static uint8_t pendLen = 0;
  static bool    pendReady = false;
  // Passthru relay (see relayCarFrame()): every header is handled generically.
  const bool relay = passthruRelayActive();
  static bool relayWas = false;
  if (relay != relayWas)
  {
    relayWas = relay;
    memset(relayRoleLogged, 0, sizeof(relayRoleLogged));  // directions are logged afresh
    relayRoleDirty = 0;
  }
  auto stashUnknown = [&](uint8_t n) {
    if (relay)
    {
      relayCarFrame(unkPid, unkBuf, n);  // no logging in here: safe before the FIFO is drained
    }
    if (pendReady)
    {
      recordLin2Frame(pendPid, pendBuf, pendLen);  // two in one pass: rare; the older one goes now
    }
    pendPid = unkPid;
    pendLen = n;
    memcpy(pendBuf, unkBuf, n);
    pendReady = true;
  };

  if (!lockLinBus(chassisLinMutex))
  {
    return;  // bus busy with a normal-mode master transaction; try next pass
  }

  // Bus quiet since the last byte: the unhandled frame is complete.
  if (unkActive && (uint32_t)(micros() - unkLastUs) > 2500)
  {
    unkActive = false;
    stashUnknown(unkLen);
  }

  const uint8_t lightPid = calculateProtectedId(linLightInId);
  const uint8_t buttonPid = calculateProtectedId(linButtonInId);
  // LIN 1.3 car: it asks for 0x30 and 0x21 (PIDs F0 / 61) instead of 0x0E.
  const bool lin13Chassis = (chassisProtocol == CHASSIS_PROTOCOL_LIN13);
  const uint8_t lin13BtnPid = calculateProtectedId(audiButtonID);
  const uint8_t lin13StatusPid = calculateProtectedId(audiStatusID);

  while (Serial2.available())
  {
    const uint8_t b = (uint8_t)Serial2.read();

    if (unkActive)
    {
      if (unkLen < sizeof(unkBuf)) unkBuf[unkLen++] = b;
      unkLastUs = micros();
    }

    if (windowLen < sizeof(window))
    {
      window[windowLen++] = b;
    }
    else
    {
      beforeWindow = window[0];
      window[0] = window[1];
      window[1] = b;
    }

    // Must check on the SAME iteration the window first reaches full (not
    // only after a later shift) — otherwise the very first SYNC+PID pair
    // after every reset (including the one right after each match below)
    // is silently skipped and that frame's header is never recognised.
    if (windowLen < sizeof(window) || window[0] != 0x55)
    {
      continue;
    }
    const uint8_t pid = window[1];
    const bool lin13Pid = lin13Chassis && (pid == lin13BtnPid || pid == lin13StatusPid);
    const bool known = (pid == lightPid) || (pid == buttonPid) || lin13Pid;
    // Only a real header ends or starts an unhandled frame: valid parity and
    // a BREAK (read as 0x00) just ahead of the SYNC. A 0x55 inside some other
    // frame's data must not cut that frame short.
    // (The BREAK is only insisted on while collecting, in case a UART ever
    // swallows it: then unhandled frames are still captured, just less safely.)
    // While collecting, that goes for our own light / button IDs too: a 0x55
    // followed by the button PID inside another frame's data would otherwise
    // be answered mid-frame and collide on the bus.
    const bool realHeader = (calculateProtectedId(pid) == pid) && (beforeWindow == 0x00 || !unkActive);
    // (In the passthru relay every ID is "ours", so every header must be real.)
    if (!realHeader && (relay || !known || unkActive))
    {
      continue;
    }

    if (unkActive)
    {
      // The next frame's BREAK / SYNC / PID were collected as well: drop them.
      uint8_t n = unkLen;
      n = (n >= 2) ? (uint8_t)(n - 2) : 0;
      if (n > 0 && beforeWindow == 0x00 && unkBuf[n - 1] == 0x00) n--;
      unkActive = false;
      stashUnknown(n);
    }

    if (relay)
    {
      const uint8_t id = (uint8_t)(pid & 0x3F);
      relayCarMs = millis();
      if (relayCarHeaders < 255) relayCarHeaders = (uint8_t)(relayCarHeaders + 1);

      // An ID the car asks for, with a fresh wheel answer in hand: give it
      // back exactly as the wheel sent it (its length, its checksum byte).
      uint8_t ans[kLinMaxResp];
      uint8_t n = 0;
      portENTER_CRITICAL(&stateMux);
      RelayId& r = relayIds[id];
      if (r.role == RELAY_CAR_ASKS)
      {
        r.pid = pid;
        r.pollWanted = true;  // and have the wheel asked again for next time
        if (r.ansLen && (uint32_t)(millis() - r.ansMs) < kRelayAnswerMaxAgeMs)
        {
          n = r.ansLen;
          memcpy(ans, r.ansBuf, n);
        }
      }
      portEXIT_CRITICAL(&stateMux);

      if (n)
      {
        Serial2.write(ans, n);
        Serial2.flush();
        uint8_t echo[kLinMaxResp];
        collectBytes(echo, n, 10);  // our own bytes, read back by the transceiver
        chassisLinLastOkMs = millis();
        if (id == (uint8_t)(linButtonInId & 0x3F))
        {
          portENTER_CRITICAL(&stateMux);
          memset(lastLinOutFrame, 0, sizeof(lastLinOutFrame));
          memcpy(lastLinOutFrame, ans, n > sizeof(lastLinOutFrame) ? sizeof(lastLinOutFrame) : n);
          lastLinOutLen = sizeof(lastLinOutFrame);
          lastLinOutId = linButtonInId;
          portEXIT_CRITICAL(&stateMux);
        }
        windowLen = 0;
        continue;
      }

      // Otherwise watch what follows: the car's own data (it sends this frame
      // to the wheel) or silence (it asks, and there's no answer to give yet).
      // Either way relayCarFrame() gets it when the frame ends.
      unkPid = pid;
      unkActive = true;
      unkLen = 0;
      unkLastUs = micros();
      continue;
    }

    // LIN 1.3 car (normal mode; Passthru relays the real wheel above instead):
    // answer its 0x30 / 0x21 asks in the D3 wheel's own shape. With LIN output
    // off they are left unanswered and logged like any other header.
    if (lin13Pid && linOutputEnabled && answerLin13Chassis((uint8_t)(pid & 0x3F)))
    {
      windowLen = 0;
      continue;
    }

    if (pid == buttonPid && (passthruEnabled || linOutputEnabled))
    {
      // Slave-response: answer immediately.
      //   Passthru      -> the wheel's frame verbatim.
      //   Normal output -> the same frame with the button code translated,
      //                    prepared once per LIN1 cycle in sendButtonLINFrame().
      // Both are read under stateMux: they are the only copies written that
      // way. recvButtonData itself is filled directly by a blocking library
      // call with no locking, so reading it from this second task could race a
      // torn (half-old, half-new) frame out of getButtonState() on LIN1.
      uint8_t data[8];
      portENTER_CRITICAL(&stateMux);
      memcpy(data, passthruEnabled ? lastLinInFrame : chassisResponseFrame, sizeof(data));
      portEXIT_CRITICAL(&stateMux);

      uint8_t frame[9];
      memcpy(frame, data, 8);
      frame[8] = calculateEnhancedChecksum(pid, data, 8);
      Serial2.write(frame, sizeof(frame));
      Serial2.flush();

      // A single-wire LIN transceiver echoes our own TX back into RX —
      // drain that echo so it isn't reprocessed as fresh bus traffic (and
      // so the window doesn't falsely resync mid-echo).
      uint8_t echo[9];
      collectBytes(echo, sizeof(echo), 10);

      // Passthru only: the BCM polls this ID ~every 50 ms, so logging every
      // answer in normal mode would flush the 200-line ring buffer of button
      // events in a few seconds. Note this means a normal-mode capture shows
      // no LIN2 traffic at all — absence of it is not evidence the answer
      // path is idle.
      if (passthruEnabled)
      {
        logLinAttempt(2, linButtonInId, data, sizeof(data), LIN_Master_Base::NO_ERROR);
      }
      chassisLinLastOkMs = millis();

      portENTER_CRITICAL(&stateMux);
      memcpy(lastLinOutFrame, data, sizeof(lastLinOutFrame));
      lastLinOutLen = sizeof(lastLinOutFrame);
      lastLinOutId = linButtonInId;
      portEXIT_CRITICAL(&stateMux);

      windowLen = 0;
      continue;
    }

    if (pid == lightPid)
    {
      uint8_t rest[5];  // DATA x4, CHK
      if (collectBytes(rest, sizeof(rest), 10) &&
          calculateEnhancedChecksum(pid, rest, 4) == rest[4])
      {
        memcpy(gatewayLightData, rest, 4);
        chassisLinLastOkMs = millis();    // LIN 2 healthy (any chassis transaction)
        chassisLightLastOkMs = millis();  // LIN 2 healthy specifically for the BCM light frame
        if (passthruEnabled)
        {
          // Raw bench-test bridge: mirror these exact 4 bytes to the wheel,
          // completely unmodified — no Aux/Forced/LIN source selection, no
          // protocol-guessed activation bytes, nothing computed. This is the
          // ONLY place that writes steeringWheelLightData while passthru is
          // on (see steeringWheelLinTask() in tasks.cpp, which skips
          // updateBacklightState() entirely in that mode so it can't stomp
          // this write moments later).
          memcpy(steeringWheelLightData, rest, sizeof(steeringWheelLightData));
          logLinAttempt(2, linLightInId, rest, 4, LIN_Master_Base::NO_ERROR);
        }
        else
        {
          logLinFrameIfChanged(2, linLightInId, rest, 4);
        }
      }
      windowLen = 0;
      continue;
    }

    // Some other ID's header — not one of ours (or our button ID while
    // nothing is set to answer it). Collect whatever follows and log it via
    // recordLin2Frame(): first sight, then changes only, so a busy chassis
    // bus can't drown the button/light entries in the 200-line log. (Up to
    // V1.09 this was a once-per-ID "header seen" line in passthru only,
    // without the data.)
    unkPid = pid;
    unkActive = true;
    unkLen = 0;
    unkLastUs = micros();
  }

  if (pendReady)
  {
    pendReady = false;
    recordLin2Frame(pendPid, pendBuf, pendLen);
  }
  relayLogRoles();

  unlockLinBus(chassisLinMutex);
}

// Handle a fresh button-press event (rising edge only): toggle latch state for
// latching buttons and issue OpenHaldex mode commands. Fires exactly once per
// physical press; momentary CAN/LIN output is handled per-poll elsewhere.
static void handleButtonPressEvent(size_t i)
{
  if (i >= buttonMappingCount)
  {
    return;
  }

  if (buttonMappings[i].flags & FLAG_CHARISMA_CONTROL)
  {
    // Exclusive: this press only advances the Drive Select program. The frame
    // itself is emitted by serviceCharisma(); all this does is move the
    // program on and mark when it was pressed, so the change can be flagged as
    // a manual one by the driver.
    uint8_t next = (uint8_t)(charismaProgram + 1);
    if (next > charismaProgramCount || next > 15)
    {
      next = 1;
    }
    charismaProgram = next;
    charismaPressMs = millis();
    return;
  }

  if (buttonMappings[i].flags & FLAG_OPENHALDEX_CONTROL)
  {
    // Exclusive: this press only commands an OpenHaldex mode change.
    uint8_t target;
    if (buttonMappings[i].openHaldexMode == OPENHALDEX_MODE_PUSH_NEXT)
    {
      // Advance from the pending target if one is still in flight (so rapid
      // presses chain), else from the last broadcast mode; unknown -> Stock.
      uint8_t base = (openHaldexTargetMode != OPENHALDEX_MODE_UNKNOWN)
                         ? openHaldexTargetMode
                         : openHaldexCurrentMode;
      if (base >= OPENHALDEX_MODE_COUNT)
      {
        base = 0;
      }
      target = static_cast<uint8_t>((base + 1) % OPENHALDEX_MODE_COUNT);
    }
    else
    {
      target = (buttonMappings[i].openHaldexMode < OPENHALDEX_MODE_COUNT)
                   ? buttonMappings[i].openHaldexMode
                   : 0;
    }
    openHaldexTargetMode = target;
    openHaldexCmdStartMs = millis();
    openHaldexLastSendMs = 0;  // force an immediate send in serviceOpenHaldex()
    return;
  }

  if (buttonMappings[i].flags & FLAG_LATCH)
  {
    buttonLatched[i] = !buttonLatched[i];  // toggle on each press
    return;
  }

  // plain button: momentary output handled in sendButtonLINFrame()
}

// Signed scroll-wheel movement for this poll, read according to wheelProtocol
// (see linRotaryByteIndex). Call exactly once per button frame — the PQ path
// keeps the previous position to difference against.
static int8_t computeRotaryDelta(const uint8_t* frame, bool frameValid)
{
  static uint8_t lastPos = 0;
  static bool    havePos = false;

  const uint8_t idx = linRotaryByteIndex;
  if (idx >= 8 || !frameValid)
  {
    havePos = false;  // resync on the next good frame rather than invent a jump
    return 0;
  }

  if (wheelIsPqFamily())
  {
    const uint8_t pos = (uint8_t)(frame[idx] & 0x0F);
    const int8_t  d   = havePos ? signedNibble((uint8_t)(pos - lastPos)) : 0;
    lastPos = pos;
    havePos = true;
    return d;
  }

  return signedNibble(frame[idx]);
}

// A row that asks for a direction only matches when the wheel is turning that
// way. Neither bit set (every pre-existing row) or both set means "either".
static bool rotaryDirectionMatches(const ButtonMapping& m)
{
  const bool up   = (m.flags & FLAG_ROTARY_UP) != 0;
  const bool down = (m.flags & FLAG_ROTARY_DOWN) != 0;
  if (up == down)
  {
    return true;
  }
  return up ? (wheelRotaryDelta > 0) : (wheelRotaryDelta < 0);
}

// Likewise for press stage: a short row stops matching the moment the hold
// becomes long, a long row starts matching at that moment. Being disjoint is
// what lets one code carry both a tap action and a hold action.
static bool pressStageMatches(const ButtonMapping& m)
{
  const bool wantShort = (m.flags & FLAG_PRESS_SHORT) != 0;
  const bool wantLong  = (m.flags & FLAG_PRESS_LONG) != 0;
  if (wantShort == wantLong)
  {
    return true;
  }
  const bool isLong = wheelPressStage >= kLongPressStage;
  return wantLong ? isLong : !isLong;
}

static bool mappingQualifiers(const ButtonMapping& m)
{
  return rotaryDirectionMatches(m) && pressStageMatches(m);
}

// Index of the first mapping whose source byte in the frame holds its button
// code, or -1 if none is currently pressed.
static int pressedMappingIndex(const uint8_t* frame)
{
  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (buttonMappings[i].oldButtonId == 0)
    {
      continue;
    }
    if (frame[mappingSourceByte(buttonMappings[i])] != buttonMappings[i].oldButtonId)
    {
      continue;
    }
    if (!mappingQualifiers(buttonMappings[i]))
    {
      continue;
    }
    return static_cast<int>(i);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// VAG LIN 1.3 wheel (WHEEL_PROTOCOL_AUDI): classic checksum, frame 0x30 of 6
// data bytes — a different frame from LIN 2.x VW's 0x0E. Not Audi-only: the
// same frames come from early PQ35 VW-group wheels. What the captures show:
//
//   customer A8 (16/11/2026)  idle  02 10 00 00 00 3F
//                             only change seen: d4 = 10/20/30/40/60, back to 00
//   customer A8 D3 (17/11/2026, docs/lin_monitor_log.csv-5..9.json)
//                             idle  0x 0x 00 00 00 46..4B
//                             heating button held: d3 = 08 for as long as held
//                             d5 rose 46 -> 4A under a hair dryer, read FF
//                             (and 0D/0C/09/30/3A on contact) with the heating
//                             sensor unplugged: the heater temperature
//   Audi A4 2013 (Arduino forum thread 934599)  idle FB B0 00 34 00 FF
//                             d2 = 08 mute, 04 voice, 80 select press, 01 mode
//                             d4 = 10 / F0 vol up/down, 01 / 0F selector up/down
//   Leon 1P 1P0959542 / Octavia 2 3C0959542 on J527 1K0953549BP
//   (github.com/katzenberger/vag-mfsw-lin-bus-protocol, Saleae captures)
//                             idle 02 20 00 00 00 00
//                             d2 = button CODE: Leon 1E 2A 1D 01 02 06 03 07,
//                                  Octavia 04 2B 05 50 02 06 03 07 (the same
//                                  codes the car puts on radio CAN 0x5C1);
//                                  two buttons at once = 00
//                             d3 bit 0x04 = horn
//                             d1 = 80 10 20 30 4A 5A 60 70 with the counter
//
//   d0 low nibble, d1          counter-driven, never a button (d1's low nibble
//                              toggles with the counter on PQ35)
//   d2                         the wheel's own button code, 0 at rest
//   d3, d0 high nibble         constant per wheel at rest (A8 00/0, A4 34/F,
//                              PQ35 00/0); the horn (PQ35) and the heating
//                              button (A8 D3, 0x08) are in d3
//   d4                         two signed 4-bit roller movements
//   d5                         heated-rim temperature sensor (A8 D3: 46..4B
//                              warm, FF unplugged; A4 FF = presumably no sensor;
//                              PQ35 00) — never a button, so not decoded
//
// Decoded into an MQB-shaped frame so Learn, rollers, trigger filters and
// chassis shaping all work on it unchanged:
//
//   byte 0  0x80 | counter
//   byte 1  d2 — the wheel's own code; or 0xF1 / 0xF2 while the d4 high / low
//           roller moves (a roller has no code of its own in this frame)
//   byte 2  bits of d0's high nibble that differ from rest
//   byte 3  roller movement (signed nibble), else press stage
//   byte 4  0x31
//   byte 5  0 (d5 is a temperature, see above)
//   byte 7  bits of d3 that differ from rest (PQ35 horn: 0x04), like VW's horn byte
//
// Bytes 2/7 are learned like paddles/horn are on a VW wheel (Learn records
// the byte it came from). Which A8 control is which is NOT known — the capture
// carries no annotations — so nothing here names one.
// ---------------------------------------------------------------------------
constexpr uint8_t kLin13CodeRollerHi = 0xF1;
constexpr uint8_t kLin13CodeRollerLo = 0xF2;
// "Pressed" for d0/d3 means "differs from rest", rest taken from the first
// good frame. A bit that stays changed this long is the real resting state
// (e.g. a button held at power-up).
constexpr uint32_t kLin13RestRelearnMs = 30000;

static void decodeAudiFrame(const uint8_t* raw, uint8_t* out)
{
  // d0 high nibble and d3 only. d5 is the heater temperature (see above): read
  // as button bits it "pressed" whatever row was learned on byte 5 for 30 s at
  // every temperature step, until the rest below caught up (customer D3).
  static uint8_t  rest[2]        = {};
  static bool     haveRest       = false;
  static uint32_t differSince[2] = {};
  static uint8_t  lastCode       = 0;
  static uint32_t pressStartMs   = 0;

  const uint32_t now = millis();
  const uint8_t cur[2] = {(uint8_t)(raw[0] & 0xF0), raw[3]};
  if (!haveRest)
  {
    memcpy(rest, cur, sizeof(rest));
    haveRest = true;
  }
  for (uint8_t i = 0; i < 2; i++)
  {
    if (cur[i] == rest[i])
    {
      differSince[i] = 0;
    }
    else if (differSince[i] == 0)
    {
      differSince[i] = now | 1U;  // never 0 while timing
    }
    else if (now - differSince[i] >= kLin13RestRelearnMs)
    {
      static const uint8_t kRestByte[2] = {0, 3};
      logLine("[SW] LIN 1.3 wheel: byte %u rest %02X -> %02X", (unsigned)kRestByte[i], rest[i], cur[i]);
      rest[i] = cur[i];
      differSince[i] = 0;
    }
  }

  const int8_t rollHi = signedNibble((uint8_t)(raw[4] >> 4));
  const int8_t rollLo = signedNibble(raw[4]);

  uint8_t code = raw[2];
  int8_t  move = 0;
  if (rollHi != 0)      { code = kLin13CodeRollerHi; move = rollHi; }
  else if (rollLo != 0) { code = kLin13CodeRollerLo; move = rollLo; }

  // A roller code is one by construction — mark it so the UI treats it as a
  // scroll wheel from its first detent, whichever way it was turned.
  if (move != 0 && markRollerCode(code))
  {
    logLine("[SW] code 0x%02X is a scroll wheel (0x30 byte 4 %s nibble)", code,
            code == kLin13CodeRollerHi ? "high" : "low");
  }

  // Press stage, in the MQB wheel's own steps (1 on press, then 4 / 5 / 6 at
  // ~0.8 / 2 / 3 s held — see FLAG_PRESS_LONG), so long-press rows work. The
  // A4 thread reports no long-press difference in the frame itself.
  uint8_t stage = 0;
  if (code != 0 && move == 0)
  {
    if (code != lastCode) pressStartMs = now;
    const uint32_t held = now - pressStartMs;
    stage = held < 800 ? 1 : held < 2000 ? 4 : held < 3000 ? 5 : 6;
  }
  lastCode = code;

  memset(out, 0, 8);
  out[0] = (uint8_t)(0x80 | (raw[0] & 0x0F));
  out[2] = (uint8_t)((cur[0] ^ rest[0]) >> 4);
  out[4] = 0x31;
  out[7] = (uint8_t)(cur[1] ^ rest[1]);  // out[5] stays 0: d5 is a temperature
  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
  out[bi] = code;
  if (linRotaryByteIndex < 8 && linRotaryByteIndex != bi)
  {
    out[linRotaryByteIndex] = (move != 0) ? (uint8_t)(move & 0x0F) : stage;
  }
}

// LIN Monitor line for the raw Audi frame: on a change outside the counter
// nibbles, and on every poll while a roller is moving (two polls that each
// say "+1" are two detents and must both appear).
static void logAudiFrameIfChanged(const uint8_t* raw)
{
  static uint8_t last[audiButtonLen] = {};
  static bool    seen = false;

  uint8_t masked[audiButtonLen];
  memcpy(masked, raw, sizeof(masked));
  masked[0] &= 0xF0;
  masked[1] = 0;  // counter-driven throughout (PQ35: low nibble toggles 0/A with it)
  if (seen && raw[4] == 0 && memcmp(last, masked, sizeof(masked)) == 0)
  {
    return;
  }
  memcpy(last, masked, sizeof(last));
  seen = true;

  char hex[3 * audiButtonLen + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < audiButtonLen; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[i]);
  }
  hex[pos - 1] = '\0';
  logLine("LIN1 0x%02X: %s", audiButtonID, hex);
}

// Audi frame 0x21 (2 bytes, meaning unknown): polled ~1 s for the LIN Monitor
// only, logged on change, so a capture shows whether any control moves it.
void getAudiStatusState()
{
  uint8_t buf[audiStatusLen] = {0};

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V1, audiStatusID, sizeof(buf), buf);
  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();

  if (passthruEnabled)
  {
    logLinAttempt(1, audiStatusID, buf, sizeof(buf), linErr, true);
    logLinRawResidual(1, audiStatusID, Serial1);
    logLinRawFrame(1, audiStatusID, steeringWheelLIN, linErr);
  }
  reportPoll(healthAudiStatus, audiStatusID, steeringWheelLIN, Serial1, linErr);

  unlockLinBus(steeringWheelLinMutex);

  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    return;
  }
  swLinLastOkMs = millis();

  static uint8_t last[audiStatusLen] = {};
  static bool    seen = false;
  if (!passthruEnabled && (!seen || memcmp(last, buf, sizeof(buf)) != 0))
  {
    memcpy(last, buf, sizeof(last));
    seen = true;
    logLine("LIN1 0x%02X: %02X %02X", audiStatusID, buf[0], buf[1]);
  }
}

// ===========================================================================
// Wheel profiles (V1.12). PQ / MQB / NSF use VW's own LIN 2.x frames and VAG
// LIN 1.3 its 0x30 decoder (above). The rest go through two generic paths:
//
//   * LIN, any speed / checksum / frame: Volvo P1, Volvo P3 and Custom. Up to
//     two button frames are polled — length taken from the profile, or worked
//     out from the answer (probeSlaveResponse()) and remembered — decoded by
//     a code byte (VW-style) or one-bit-per-button, into the same MQB-shaped
//     frame the LIN 1.3 decoder builds. An optional master frame is sent to
//     the wheel (backlight / wake) with the current brightness in one byte.
//   * BMW K-bus (E-series / Mini R5x MFL): listen only, 9600 8E1, no master,
//     no header — the wheel sends a frame when a button changes.
//
// Sources for every constant: docs/wheel-protocol-research.md.
// ===========================================================================
struct WheelSpec
{
  uint16_t baud;
  uint8_t  checksum;
  uint8_t  decoder;
  uint8_t  codeByte;
  uint8_t  rollerByte;
  uint8_t  btnId[2];
  uint8_t  btnLen[2];
  uint8_t  lightId;
  uint8_t  lightLen;
  uint8_t  lightData[8];
  uint8_t  lightBrightByte;
  uint8_t  lightBrightMax;
  uint16_t lightEveryMs;
};

static bool wheelUsesGenericPath()
{
  return wheelProtocol == WHEEL_PROTOCOL_VOLVO_P1 || wheelProtocol == WHEEL_PROTOCOL_VOLVO_P3 ||
         wheelProtocol == WHEEL_PROTOCOL_CUSTOM;
}

static void resolveWheelSpec(WheelSpec& s)
{
  memset(&s, 0, sizeof(s));
  s.baud = linBaud;
  s.checksum = WHEEL_CHK_ENHANCED;
  s.decoder = WHEEL_DEC_CODE_BYTE;
  s.codeByte = 1;
  s.rollerByte = 0xFF;
  s.btnId[0] = s.btnId[1] = 0xFF;
  s.lightId = 0xFF;
  s.lightBrightByte = 0xFF;
  s.lightBrightMax = 0x7F;
  s.lightEveryMs = 100;

  switch (wheelProtocol)
  {
  case WHEEL_PROTOCOL_VOLVO_P1:
    // laurynas/volvo_linbus (V50): SWM_ID 0x20 at 9600; 4 data bytes, one bit
    // per button (NEXT 00 10 00 00, VOL+ 00 00 01 00, joystick in byte 0);
    // the posted checksums are classic. No light frame known.
    s.baud = 9600;
    s.checksum = WHEEL_CHK_CLASSIC;
    s.decoder = WHEEL_DEC_BITS;
    s.btnId[0] = 0x20;
    s.btnLen[0] = 4;
    break;

  case WHEEL_PROTOCOL_VOLVO_P3:
    // Arduino forum 1435919 (V40 2012+): the master polls 0x30 (left pad) and
    // 0x20 (right pad) at 9600 with the classic checksum, and sends 0x10 =
    // FF 00 00 00 00 00 00 00 every 500 ms ("lights"). Pad frame lengths and
    // per-button bytes weren't published: lengths are found from the answer,
    // buttons are one code per changed bit.
    s.baud = 9600;
    s.checksum = WHEEL_CHK_CLASSIC;
    s.decoder = WHEEL_DEC_BITS;
    s.btnId[0] = 0x30;
    s.btnId[1] = 0x20;
    s.lightId = 0x10;
    s.lightLen = 8;
    s.lightData[0] = 0xFF;
    s.lightEveryMs = 500;
    break;

  case WHEEL_PROTOCOL_CUSTOM:
  {
    CustomWheelConfig c;
    portENTER_CRITICAL(&stateMux);
    c = customWheel;
    portEXIT_CRITICAL(&stateMux);
    s.baud = c.baud;
    s.checksum = c.checksum;
    s.decoder = c.decoder;
    s.codeByte = c.codeByte;
    s.rollerByte = c.rollerByte;
    memcpy(s.btnId, c.btnId, sizeof(s.btnId));
    memcpy(s.btnLen, c.btnLen, sizeof(s.btnLen));
    s.lightId = c.lightId;
    s.lightLen = c.lightLen;
    memcpy(s.lightData, c.lightData, sizeof(s.lightData));
    s.lightBrightByte = c.lightBrightByte;
    s.lightBrightMax = c.lightBrightMax;
    s.lightEveryMs = c.lightEveryMs;
    break;
  }

  default:
    break;
  }
}

uint16_t profileWheelBaud()
{
  if (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS) return 9600;
  if (!wheelUsesGenericPath()) return linBaud;
  WheelSpec s;
  resolveWheelSpec(s);
  return s.baud;
}

// Once per LIN1 cycle, before any transaction: put the wheel UART in the
// shape the selected profile needs (speed; 8N1 LIN vs 8E1 K-bus).
void serviceWheelBus()
{
  static bool kbusOpen = false;
  const bool wantKbus = (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS);
  const uint16_t baud = profileWheelBaud();
  if (wantKbus == kbusOpen && baud == wheelLinBaudNow)
  {
    return;
  }
  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;  // next cycle
  }
  if (wantKbus != kbusOpen)
  {
    reopenWheelUart(baud, wantKbus ? SERIAL_8E1 : SERIAL_8N1);
    kbusOpen = wantKbus;
    wheelLinBaudNow = baud;
    steeringWheelLIN.setBaudrate(baud);
    logLine("[WHEEL] LIN1 reopened as %s, %u baud", wantKbus ? "K-bus 8E1 (listen only)" : "LIN 8N1",
            (unsigned)baud);
  }
  else
  {
    setWheelLinBaud(baud);
    logLine("[WHEEL] LIN1 speed now %u baud", (unsigned)baud);
  }
  unlockLinBus(steeringWheelLinMutex);
}

// ---- generic LIN path ----------------------------------------------------
struct WheelFrameState
{
  uint8_t data[8];
  uint8_t len;         // bytes in data[] from the last good read
  uint8_t learnedLen;  // length found by probing (profile length 0), 0 = not yet
  bool    classic;     // checksum type of the last good read
  uint8_t fails;       // consecutive failures at learnedLen, to re-probe
  bool    ok;          // this cycle's read was good
};
static WheelFrameState wheelFrames[2] = {};
static PollHealth      healthGeneric[2];
static Lin2FrameLog    wheelFrameLog[2] = {};

// One button frame, per the profile: exact length (or the length learned for
// it) with the profile's checksum, or a probe when the length isn't known.
// Under WHEEL_CHK_AUTO a frame failing one checksum is rechecked against the
// other from the raw buffer. Caller holds steeringWheelLinMutex.
static bool readWheelFrame(uint8_t id, uint8_t wantLen, uint8_t chk, WheelFrameState& st,
                           LIN_Master_Base::error_t& err)
{
  const uint8_t len = wantLen ? wantLen : st.learnedLen;
  if (len == 0)
  {
    bool classic = false;
    uint8_t buf[8] = {};
    const uint8_t n = probeSlaveResponse(id, buf, classic, err);
    const bool chkOk = (chk == WHEEL_CHK_AUTO) || (classic == (chk == WHEEL_CHK_CLASSIC));
    if (n && chkOk)
    {
      memcpy(st.data, buf, n);
      st.len = n;
      st.learnedLen = n;
      st.classic = classic;
      st.fails = 0;
      err = LIN_Master_Base::NO_ERROR;
      logLine("[WHEEL] LIN1 0x%02X answers %u bytes (%s)", id, (unsigned)n, classic ? "classic" : "enhanced");
      return true;
    }
    if (n) err = LIN_Master_Base::ERROR_CHK;  // answered, under the other checksum
    return false;
  }

  const bool useClassic = (chk == WHEEL_CHK_CLASSIC) || (chk == WHEEL_CHK_AUTO && st.classic);
  uint8_t buf[8] = {};
  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(useClassic ? LIN_Master_Base::LIN_V1 : LIN_Master_Base::LIN_V2,
                                                id, len, buf);
  err = steeringWheelLIN.getError();
  if (err == LIN_Master_Base::NO_ERROR)
  {
    memcpy(st.data, buf, len);
    st.len = len;
    st.classic = useClassic;
    st.fails = 0;
    return true;
  }
  if (chk == WHEEL_CHK_AUTO && !(err & LIN_Master_Base::ERROR_TIMEOUT))
  {
    uint8_t rawLen = 0;
    const uint8_t* raw = steeringWheelLIN.getRawRxBuffer(rawLen);  // brk sync pid data.. chk
    if (rawLen >= (uint8_t)(len + 4))
    {
      const uint8_t kind = checksumKind(calculateProtectedId(id), raw + 3, len);
      if (kind)
      {
        memcpy(st.data, raw + 3, len);
        st.len = len;
        st.classic = (kind == 1);
        st.fails = 0;
        err = LIN_Master_Base::NO_ERROR;
        return true;
      }
    }
  }
  if (wantLen == 0 && ++st.fails >= 5)
  {
    st.learnedLen = 0;  // the learned length stopped working: probe again
    st.fails = 0;
  }
  return false;
}

// Time-based press stage, in the MQB wheel's own steps (1, then 4/5/6 at
// ~0.8/2/3 s held) for wheels that don't report one themselves.
static uint8_t timedPressStage(uint8_t code)
{
  static uint8_t  lastCode = 0;
  static uint32_t startMs = 0;
  const uint32_t now = millis();
  if (code == 0)
  {
    lastCode = 0;
    return 0;
  }
  if (code != lastCode)
  {
    lastCode = code;
    startMs = now;
  }
  const uint32_t held = now - startMs;
  return held < 800 ? 1 : held < 2000 ? 4 : held < 3000 ? 5 : 6;
}

// Write the MQB-shaped frame every decoded profile produces.
static void buildDecodedFrame(uint8_t* out, uint8_t code, int8_t move, uint8_t stage)
{
  static uint8_t counter = 0;
  counter = (uint8_t)((counter + 1) & 0x0F);
  memset(out, 0, 8);
  out[0] = (uint8_t)(0x80 | counter);
  out[4] = 0x31;
  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
  out[bi] = code;
  if (linRotaryByteIndex < 8 && linRotaryByteIndex != bi)
  {
    out[linRotaryByteIndex] = (move != 0) ? (uint8_t)(move & 0x0F) : stage;
  }
}

// Decoder memory, per button frame. Reset whenever what it describes changes
// (profile, frame IDs / lengths, decoder — see acquireGenericWheel()) or a
// frame's length does: otherwise the new wheel's bytes would be compared with
// the old one's resting values and read as presses for up to 30 s.
struct GenericDecodeState
{
  uint8_t  rest[8];
  uint8_t  last[8];
  uint8_t  streak[8];
  uint32_t differSince[8];
  uint8_t  len;     // length the state above was built for
  uint8_t  warmup;  // good frames still to see before reporting any press
  bool     haveRest;
};
static GenericDecodeState genericDecode[2] = {};

// Frames after a (re)start during which nothing counts as pressed: a counter
// byte is only recognised as one after 4 changes in a row, and until then its
// first changes would read as buttons.
constexpr uint8_t kGenericWarmupFrames = 6;

static void resetGenericDecoder()
{
  memset(genericDecode, 0, sizeof(genericDecode));
  for (WheelFrameState& st : wheelFrames)
  {
    st = WheelFrameState();
  }
}

// Decode this cycle's good frames into `out`.
//   WHEEL_DEC_CODE_BYTE: the code is the value of codeByte (when it differs
//     from that byte's resting value); rollerByte, if set, holds a signed
//     4-bit movement for that code (MQB-style), else a timed press stage.
//   WHEEL_DEC_BITS: every bit that differs from rest is a button of its own:
//     code 0x40 + frame*64 + byte*8 + bit (frame 0 -> 0x40-0x7F, frame 1 ->
//     0x80-0xBF); the first one wins.
// "Rest" is the first good frame, relearned for a byte that stays changed 30 s
// (a button held at power-up). A byte that changes on 4 frames running is a
// counter / CRC, never a button, and is ignored while it keeps doing so.
static void decodeGenericWheel(const WheelSpec& s, uint8_t* out)
{
  const uint32_t now = millis();

  bool volatileByte[2][8] = {};
  bool warming = false;
  for (uint8_t f = 0; f < 2; f++)
  {
    const WheelFrameState& st = wheelFrames[f];
    if (!st.ok) continue;
    GenericDecodeState& g = genericDecode[f];
    if (g.haveRest && g.len != st.len)
    {
      g = GenericDecodeState();  // the frame's length changed (re-probed): start over
    }
    uint8_t* rest = g.rest;
    uint8_t* last = g.last;
    uint8_t* streak = g.streak;
    uint32_t* differSince = g.differSince;
    if (!g.haveRest)
    {
      memcpy(rest, st.data, st.len);
      memcpy(last, st.data, st.len);
      g.len = st.len;
      g.warmup = kGenericWarmupFrames;
      g.haveRest = true;
    }
    if (g.warmup)
    {
      g.warmup--;
      warming = true;
    }
    for (uint8_t b = 0; b < st.len; b++)
    {
      const uint8_t v = st.data[b];
      if (v != last[b])
      {
        if (streak[b] < 255) streak[b]++;
      }
      else
      {
        streak[b] = 0;
      }
      last[b] = v;
      volatileByte[f][b] = streak[b] >= 4;
      if (volatileByte[f][b])
      {
        rest[b] = v;  // keep following it, so it reads "at rest" if it ever settles
        differSince[b] = 0;
        continue;
      }
      if (v == rest[b])
      {
        differSince[b] = 0;
      }
      else if (differSince[b] == 0)
      {
        differSince[b] = now | 1U;
      }
      else if (now - differSince[b] >= 30000)
      {
        logLine("[WHEEL] LIN1 0x%02X byte %u rest %02X -> %02X", s.btnId[f], (unsigned)b, rest[b], v);
        rest[b] = v;
        differSince[b] = 0;
      }
    }
  }

  uint8_t code = 0;
  int8_t  move = 0;
  for (uint8_t f = 0; f < 2 && code == 0; f++)
  {
    const WheelFrameState& st = wheelFrames[f];
    if (!st.ok) continue;
    if (s.decoder == WHEEL_DEC_CODE_BYTE)
    {
      if (s.codeByte < st.len && !volatileByte[f][s.codeByte] && st.data[s.codeByte] != genericDecode[f].rest[s.codeByte])
      {
        code = st.data[s.codeByte];
        if (s.rollerByte < st.len)
        {
          move = signedNibble(st.data[s.rollerByte]);
        }
      }
    }
    else
    {
      for (uint8_t b = 0; b < st.len && code == 0; b++)
      {
        if (volatileByte[f][b]) continue;
        const uint8_t changed = (uint8_t)(st.data[b] ^ genericDecode[f].rest[b]);
        for (uint8_t bit = 0; bit < 8; bit++)
        {
          if (changed & (1U << bit))
          {
            code = (uint8_t)(0x40 + f * 64 + b * 8 + bit);
            break;
          }
        }
      }
    }
  }

  if (warming)
  {
    code = 0;  // still learning which bytes are counters
    move = 0;
  }
  buildDecodedFrame(out, code, move, move ? 0 : timedPressStage(code));
}

// Read the profile's button frames into recvButtonData. Caller holds the LIN1
// mutex. Returns the first frame's error when no frame answered.
static LIN_Master_Base::error_t acquireGenericWheel(uint8_t& rxId)
{
  WheelSpec s;
  resolveWheelSpec(s);
  setWheelLinBaud(s.baud);

  // Anything that changes what the frames mean (another profile, or the Custom
  // profile edited) starts the decoder and the learned lengths over.
  {
    static uint32_t lastSig = 0;
    uint32_t sig = 2166136261UL;
    const uint8_t parts[] = {wheelProtocol, (uint8_t)(s.baud >> 8), (uint8_t)s.baud, s.checksum, s.decoder,
                             s.codeByte, s.rollerByte, s.btnId[0], s.btnId[1], s.btnLen[0], s.btnLen[1]};
    for (uint8_t p : parts) sig = (sig ^ p) * 16777619UL;
    if (sig != lastSig)
    {
      lastSig = sig;
      resetGenericDecoder();
    }
  }

  bool anyOk = false;
  LIN_Master_Base::error_t firstErr = LIN_Master_Base::ERROR_TIMEOUT;
  for (uint8_t i = 0; i < 2; i++)
  {
    WheelFrameState& st = wheelFrames[i];
    st.ok = false;
    if (s.btnId[i] == 0xFF) continue;

    LIN_Master_Base::error_t err;
    st.ok = readWheelFrame(s.btnId[i], s.btnLen[i], s.checksum, st, err);
    if (passthruEnabled)
    {
      logLinAttempt(1, s.btnId[i], st.data, st.len ? st.len : 8, err, st.classic);
    }
    reportPoll(healthGeneric[i], s.btnId[i], steeringWheelLIN, Serial1, err, i == 0);
    if (st.ok)
    {
      anyOk = true;
      if (!passthruEnabled)
      {
        logFrameOnChange(wheelFrameLog[i], 1, s.btnId[i], st.data, st.len, st.classic ? 1 : 2);
      }
    }
    else if (i == 0)
    {
      firstErr = err;
    }
  }

  rxId = (s.btnId[0] != 0xFF) ? s.btnId[0] : 0;
  if (!anyOk)
  {
    return firstErr;
  }
  decodeGenericWheel(s, recvButtonData);
  return LIN_Master_Base::NO_ERROR;
}

// The profile's master frame to the wheel (backlight / wake), if it has one.
static void sendGenericLight()
{
  WheelSpec s;
  resolveWheelSpec(s);
  if (s.lightId == 0xFF || s.lightLen == 0)
  {
    return;
  }
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs != 0 && now - lastMs < s.lightEveryMs)
  {
    return;
  }
  lastMs = now | 1U;

  uint8_t d[8];
  memcpy(d, s.lightData, sizeof(d));
  if (s.lightBrightByte < s.lightLen)
  {
    // steeringWheelLightData[0] is the brightness every profile computes the
    // same way (Aux PWM / LIN / forced — updateBacklightState()), 0..upperLightsLIN.
    const uint32_t top = upperLightsLIN ? upperLightsLIN : 1;
    uint32_t v = steeringWheelLightData[0];
    if (v > top) v = top;
    d[s.lightBrightByte] = (uint8_t)((v * s.lightBrightMax) / top);
  }
  // Checksum: the profile's; under Auto the one the wheel's own answers use
  // (classic until one has been seen).
  const bool classic = (s.checksum == WHEEL_CHK_CLASSIC) ||
                       (s.checksum == WHEEL_CHK_AUTO && (wheelFrames[0].len == 0 || wheelFrames[0].classic));

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }
  setWheelLinBaud(s.baud);
  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(classic ? LIN_Master_Base::LIN_V1 : LIN_Master_Base::LIN_V2,
                                             s.lightId, s.lightLen, d);
  const LIN_Master_Base::error_t err = steeringWheelLIN.getError();
  if (passthruEnabled)
  {
    logLinAttempt(1, s.lightId, d, s.lightLen, err, classic);
  }
  logLinTiming(healthLight, s.lightId, steeringWheelLIN, Serial1, err);
  notePollResult(healthLight, 1, s.lightId, err);
  unlockLinBus(steeringWheelLinMutex);

  static Lin2FrameLog lightLog = {};
  if (!passthruEnabled)
  {
    logFrameOnChange(lightLog, 1, s.lightId, d, s.lightLen, classic ? 1 : 2);
  }
}

// ---- BMW K-bus / I-bus path ------------------------------------------------
// wilhelm-docs + sda2/arduino-ibus-integration: frame = source, length (bytes
// after it, incl. checksum), destination, command, data.., XOR of every byte
// before the checksum. The MFL is source 0x50:
//   0x32 volume   data bit 0: 1 = up, 0 = down (one step per frame)
//   0x3B buttons  0x01 next, 0x08 previous, 0x40 R/T, 0x80 telephone;
//                 | 0x10 = held, | 0x20 = released
// The wheel only talks when something changes, so there is nothing to poll.
static uint8_t  kbusBuf[40];
static uint8_t  kbusLen = 0;
static uint8_t  kbusCode = 0;
static uint8_t  kbusStage = 0;
static int8_t   kbusMove = 0;
static uint32_t kbusPressMs = 0;
constexpr uint8_t kKbusVolumeCode = 0xF1;  // the volume "roller", as on a LIN 1.3 wheel

static void handleKbusFrame(const uint8_t* f, uint8_t n)
{
  swLinLastOkMs = millis();  // a valid K-bus frame, from anyone: the bus is alive
  if (f[0] != 0x50 || n < 6)
  {
    return;  // another module's frame: not the wheel's
  }
  char hex[3 * 12 + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < n && i < 12; i++)
  {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", f[i]);
  }
  hex[pos ? pos - 1 : 0] = '\0';
  logLine("KBUS MFL: %s", hex);

  const uint8_t cmd = f[3];
  const uint8_t d = f[4];
  if (cmd == 0x32)
  {
    const int8_t step = (d & 0x01) ? 1 : -1;
    kbusMove = (int8_t)constrain(kbusMove + step, -7, 7);  // a signed nibble's range
  }
  else if (cmd == 0x3B)
  {
    const uint8_t button = (uint8_t)(d & 0xC9);
    const uint8_t state  = (uint8_t)(d & 0x30);
    if (state == 0x20 || button == 0)
    {
      kbusCode = 0;
      kbusStage = 0;
    }
    else
    {
      if (button != kbusCode) kbusPressMs = millis();
      kbusCode = button;
      kbusStage = (state == 0x10) ? 4 : 1;
    }
  }
}

static void serviceKbusRx()
{
  // No gap timer: this runs every ~100 ms, so the time between calls says
  // nothing about the gaps between a frame's bytes. A partly arrived frame
  // just waits for its rest; anything broken is slid past by the length /
  // XOR check below.
  const uint32_t now = millis();
  while (Serial1.available())
  {
    const uint8_t b = (uint8_t)Serial1.read();
    if (kbusLen < sizeof(kbusBuf))
    {
      kbusBuf[kbusLen++] = b;
    }
    else
    {
      memmove(kbusBuf, kbusBuf + 1, sizeof(kbusBuf) - 1);
      kbusBuf[sizeof(kbusBuf) - 1] = b;
    }
    while (kbusLen >= 2)
    {
      const uint8_t L = kbusBuf[1];
      if (L < 3 || L > sizeof(kbusBuf) - 2)
      {
        memmove(kbusBuf, kbusBuf + 1, --kbusLen);  // not a frame start: slide on
        continue;
      }
      if (kbusLen < (uint8_t)(L + 2)) break;  // wait for the rest
      uint8_t x = 0;
      for (uint8_t i = 0; i < L + 1; i++) x ^= kbusBuf[i];
      if (x == kbusBuf[L + 1])
      {
        handleKbusFrame(kbusBuf, (uint8_t)(L + 2));
        kbusLen = (uint8_t)(kbusLen - (L + 2));
        memmove(kbusBuf, kbusBuf + L + 2, kbusLen);
      }
      else
      {
        memmove(kbusBuf, kbusBuf + 1, --kbusLen);
      }
    }
  }
  // A lost "released" frame must not hold a button forever.
  if (kbusCode != 0 && now - kbusPressMs > 10000)
  {
    kbusCode = 0;
    kbusStage = 0;
  }
}

static void acquireKbusWheel(uint8_t& rxId)
{
  serviceKbusRx();
  rxId = 0x50;
  // Volume steps wait while a button is held: reporting them would replace the
  // held code for a poll — a false release + re-press, which toggles latches.
  if (kbusMove != 0 && kbusCode == 0)
  {
    if (markRollerCode(kKbusVolumeCode))
    {
      logLine("[SW] code 0x%02X is the K-bus volume (scroll up / down)", kKbusVolumeCode);
    }
    buildDecodedFrame(recvButtonData, kKbusVolumeCode, kbusMove, 0);
    kbusMove = 0;  // one poll per batch of volume steps
    return;
  }
  uint8_t stage = kbusStage;
  if (kbusCode != 0)
  {
    const uint32_t held = millis() - kbusPressMs;
    const uint8_t timed = held < 800 ? 1 : held < 2000 ? 4 : held < 3000 ? 5 : 6;
    if (timed > stage) stage = timed;
  }
  buildDecodedFrame(recvButtonData, kbusCode, 0, kbusCode ? stage : 0);
}

void getButtonState()
{
  memset(recvButtonData, 0, sizeof(recvButtonData));
  memset(transButtonDataLIN, 0, sizeof(transButtonDataLIN));
  memset(transButtonDataCAN, 0, sizeof(transButtonDataCAN));

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  uint8_t rxId = linButtonInId;
  LIN_Master_Base::error_t linErr = LIN_Master_Base::NO_ERROR;

  if (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS)
  {
    // Listen-only bus: nothing to ask, so nothing can "fail" — idle is silence.
    acquireKbusWheel(rxId);
    noteButtonFrameResult(true);
  }
  else if (wheelUsesGenericPath())
  {
    linErr = acquireGenericWheel(rxId);
    noteButtonFrameResult(linErr == LIN_Master_Base::NO_ERROR);
    if (linErr != LIN_Master_Base::NO_ERROR)
    {
      memset(recvButtonData, 0, sizeof(recvButtonData));
    }
    else
    {
      swLinLastOkMs = millis();
    }
  }
  else
  {
    // An Audi wheel answers a different frame (0x30, 6 bytes, classic checksum)
    // — decoded into recvButtonData below. Its wheel-side ID is fixed rather than
    // taken from linButtonInId, which stays the ID the chassis/BCM polls on LIN2.
    const bool audi = (wheelProtocol == WHEEL_PROTOCOL_AUDI);
    rxId = audi ? audiButtonID : (uint8_t)linButtonInId;
    uint8_t audiRaw[audiButtonLen] = {};
    uint8_t* rxBuf = audi ? audiRaw : recvButtonData;
    const uint8_t rxLen = audi ? audiButtonLen : 8;

    steeringWheelLIN.resetStateMachine();
    steeringWheelLIN.resetError();
    steeringWheelLIN.receiveSlaveResponseBlocking(audi ? LIN_Master_Base::LIN_V1 : LIN_Master_Base::LIN_V2,
                                                  rxId, rxLen, rxBuf);

    // Discard the frame if the library flagged any error (timeout, checksum,
    // echo mismatch, etc.).  Without this, partial bytes written by a failed
    // receive leave garbage in recvButtonData[1] which matches a button mapping
    // and makes a button appear pressed when nothing was touched.
    linErr = steeringWheelLIN.getError();

    // Log the raw receive attempt BEFORE any error-based discard below, so a
    // checksum-failed or timed-out frame still shows its actual bytes here.
    if (passthruEnabled)
    {
      logLinAttempt(1, rxId, rxBuf, rxLen, linErr, audi);
      logLinRawResidual(1, rxId, Serial1);
      logLinRawFrame(1, rxId, steeringWheelLIN, linErr);
    }
    reportPoll(healthButton, rxId, steeringWheelLIN, Serial1, linErr, true);
    noteButtonFrameResult(linErr == LIN_Master_Base::NO_ERROR);

    if (linErr != LIN_Master_Base::NO_ERROR)
    {
      memset(recvButtonData, 0, sizeof(recvButtonData));
    }
    else
    {
      swLinLastOkMs = millis();  // LIN 1 healthy
      if (!passthruEnabled)
      {
        if (audi) logAudiFrameIfChanged(audiRaw);  // the raw frame, not the decoded one
        else      logLinFrameIfChanged(1, linButtonInId, recvButtonData, 8);
      }
      if (audi)
      {
        decodeAudiFrame(audiRaw, recvButtonData);
        // Byte 5 = heated-rim temperature (see decodeAudiFrame()), for the
        // Diagnostics "Wheel Heater Sensor" card. FF = sensor open / unplugged.
        noteWheelTemp(audiRaw[5], WHEEL_TEMP_SRC_LIN13);
      }
    }
  }

  unlockLinBus(steeringWheelLinMutex);

  // For an Audi wheel this is the DECODED (MQB-shaped) frame — what the
  // mapping table matches against and what passthru relays; the LIN Monitor
  // carries the raw 0x30 bytes.
  portENTER_CRITICAL(&stateMux);
  memcpy(lastLinInFrame, recvButtonData, sizeof(lastLinInFrame));
  lastLinInLen = sizeof(lastLinInFrame);
  lastLinInId = rxId;
  portEXIT_CRITICAL(&stateMux);

  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;

  // Scroll-wheel movement for this frame, needed before any matching below:
  // a row filtered to one direction depends on it.
  wheelRotaryDelta = computeRotaryDelta(recvButtonData, linErr == LIN_Master_Base::NO_ERROR);
  wheelPressStage  = (linRotaryByteIndex < 8) ? recvButtonData[linRotaryByteIndex] : 0;

  // Remember any code that proves itself a roller in normal use, so the UI can
  // flag rows on it that still fire "either way" — not only ones just learned.
  if (recvButtonData[bi] != 0 && isRollerDelta(wheelRotaryDelta) &&
      markRollerCode(recvButtonData[bi]))
  {
    logLine("[SW] code 0x%02X is a scroll wheel (d=%+d)", recvButtonData[bi], (int)wheelRotaryDelta);
  }

  // Learn captures whichever byte (1..7) changed from the idle baseline, so a
  // paddle (byte 6) or horn (byte 7) is captured with its real byte position.
  if (learnActive)
  {
    if (!learnBaselineReady)
    {
      // First error-free frame since arming: this is the idle reference. Don't
      // also try to capture from it — everything in it is "new" by definition.
      if (linErr == LIN_Master_Base::NO_ERROR)
      {
        memcpy(learnBaseline, recvButtonData, sizeof(learnBaseline));
        learnBaselineReady = true;
      }
    }
    else if (linErr == LIN_Master_Base::NO_ERROR)
    {
      // Only intact frames. A failed poll leaves recvButtonData zeroed, and
      // feeding that in made every constant byte "rest at 0" (see the reset in
      // captureLearnedFromFrame()), so the next good frame looked like a fresh
      // press on it: an RS6 wheel's byte 4 (always 01) was learned as Previous
      // and then matched every frame; a LIN 1.3 wheel's decoded byte 4 (0x31)
      // was learned as a button. Wheels that answer late time out often.
      captureLearnedFromFrame(recvButtonData);
    }
  }

  // Match each mapping at its own source byte so paddles/horn register too.
  const int matchedIdx = pressedMappingIndex(recvButtonData);
  if (matchedIdx >= 0)
  {
    latestLinButtonId = buttonMappings[matchedIdx].oldButtonId;
    latestMatchedRow = (int8_t)matchedIdx;
    latestLinButtonTimestamp = millis();
  }
  else if (recvButtonData[bi] != 0)
  {
    // Unmapped byte-1 press: still surface the raw code for manual entry.
    latestLinButtonId = recvButtonData[bi];
    latestMatchedRow = -1;
    latestLinButtonTimestamp = millis();
  }

  // Rising-edge press detection drives one-shot actions (latch toggling,
  // OpenHaldex commands) exactly once when the matched mapping changes.
  //
  // A rotary row is the exception: the code stays put for as long as the wheel
  // is turning, so edge detection alone would report a single event for a whole
  // sweep. It fires on every poll that reports movement instead, which is what
  // makes three detents act like three presses.
  static int prevMatchedIdx = -1;
  const bool rotaryRow =
      matchedIdx >= 0 &&
      (buttonMappings[matchedIdx].flags & (FLAG_ROTARY_UP | FLAG_ROTARY_DOWN)) != 0;
  if (matchedIdx >= 0 && (matchedIdx != prevMatchedIdx || rotaryRow))
  {
    logLine("[SW] btn id=0x%02X byte %u row=%d d=%+d st=%u",
            buttonMappings[matchedIdx].oldButtonId,
            (unsigned)mappingSourceByte(buttonMappings[matchedIdx]), matchedIdx,
            (int)wheelRotaryDelta, (unsigned)wheelPressStage);
    handleButtonPressEvent(static_cast<size_t>(matchedIdx));
  }
  prevMatchedIdx = matchedIdx;
}

void sendLightLINFrame()
{
  // Light frame goes ONLY to the steering wheel LIN bus.
  // Chassis LIN is a source (read FROM), not a destination for light data.
  if (wheelProtocol == WHEEL_PROTOCOL_BMW_KBUS)
  {
    return;  // listen only: never transmit on someone else's K-bus
  }
  if (wheelUsesGenericPath())
  {
    sendGenericLight();
    return;
  }
  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  // A LIN 1.3 wheel has no 0x0D. Its column module publishes 0x01 instead (2
  // bytes, classic): "50 03" with ignition on, "00 03" off, every 40 ms on a
  // PQ35 J527 (katzenberger captures); an A4 B8 car sends "00 FF" there. What
  // the bytes mean isn't known, so the ignition-on capture is replayed as is.
  // Its brightness: unknown — on B7/B8-era Audi wheels the button lighting is
  // reportedly a separate PWM wire (terminal 58s), not a LIN frame at all.
  const bool lin13 = (wheelProtocol == WHEEL_PROTOCOL_AUDI);
  static uint8_t lin13Frame01[2] = {0x50, 0x03};
  // VW Up! / Citigo / Mii (NSF): 0x0D is only 2 bytes, brightness 00-7F then
  // FF — without the FF the wheel stops reporting buttons
  // (riquenunes/vw-steering-wheel-gamepad).
  const bool nsf = (wheelProtocol == WHEEL_PROTOCOL_NSF);
  uint8_t nsfFrame[2] = {steeringWheelLightData[0], 0xFF};
  const uint8_t  txId  = lin13 ? kLin13MasterId : (uint8_t)linLightInId;
  uint8_t*       txBuf = lin13 ? lin13Frame01 : nsf ? nsfFrame : steeringWheelLightData;
  const uint8_t  txLen = (lin13 || nsf) ? 2 : 4;

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(lin13 ? LIN_Master_Base::LIN_V1 : LIN_Master_Base::LIN_V2, txId,
                                             txLen, txBuf);
  const LIN_Master_Base::error_t err = steeringWheelLIN.getError();

  if (passthruEnabled)
  {
    logLinAttempt(1, txId, txBuf, txLen, err, lin13);
  }
  // A master send failing means the bus itself is wrong (no echo of our own
  // bits: transceiver asleep, RX/TX swapped, shorted), not a quiet wheel.
  logLinTiming(healthLight, txId, steeringWheelLIN, Serial1, err);
  notePollResult(healthLight, 1, txId, err);

  unlockLinBus(steeringWheelLinMutex);

  if (lin13)
  {
    static bool logged = false;
    if (!logged && !passthruEnabled)
    {
      logged = true;
      logLine("LIN1 0x%02X to wheel: %02X %02X (LIN 1.3 master frame)", txId, txBuf[0], txBuf[1]);
    }
    return;
  }

  // Backlight sent to the wheel, logged on change (at most every 500 ms; a
  // dimmer being turned changes it every cycle). With the chassis frames
  // captured on LIN2 (serviceChassisLinBus()), a log shows both ends of the
  // backlight path: what the car asked for and what the wheel was given.
  if (!passthruEnabled)
  {
    static uint8_t  logged[4] = {};
    static uint8_t  loggedLen = 0;
    static uint32_t loggedMs = 0;
    const uint32_t now = millis();
    if ((loggedLen != txLen || memcmp(logged, txBuf, txLen) != 0) && now - loggedMs >= 500)
    {
      memcpy(logged, txBuf, txLen);
      loggedLen = txLen;
      loggedMs = now;
      if (txLen == 2)
      {
        logLine("LIN1 0x%02X to wheel: %02X %02X (backlight)", txId, logged[0], logged[1]);
      }
      else
      {
        logLine("LIN1 0x%02X to wheel: %02X %02X %02X %02X (backlight)", txId, logged[0], logged[1],
                logged[2], logged[3]);
      }
    }
  }
}

void sendButtonLINFrame()
{
  buttonFound = false;

  if (passthruEnabled)
  {
    // Raw bench-test bridge (see README's Passthru Mode): the wheel's button
    // frame is relayed to the chassis bus completely UNMODIFIED and under
    // the SAME ID it was received on — no mapping, no protocol reshaping, no
    // CAN/resistive output.
    //
    // That relay does NOT happen here. This device is a SLAVE on LIN2 (the
    // bridged-pins bench test proves the BCM already masters every ID on
    // that bus, buttons included — see serviceChassisLinBus()'s header
    // comment) so it must answer the BCM's own header rather than transmit
    // an unprompted one of its own; sendMasterRequestBlocking() here would
    // send OUR break+header competing with the BCM's real one, exactly the
    // bug that made LIN2's light frame (0x0D) near-unusable before it was
    // rewritten to passively listen instead of mastering. The actual relay
    // is serviceChassisLinBus()'s slave-response path, reacting the instant
    // it sees the BCM's real header for linButtonInId — see tasks.cpp's
    // chassisLinListenerTask().
    return;
  }

  switch (recvButtonData[6])
  {
  case 1:
    dsgPaddleDown = true;
    break;

  case 2:
    dsgPaddleUp = true;
    break;

  default:
    break;
  }

  // Start the BCM's answer frame as a verbatim copy of what the wheel just
  // sent; the loop below rewrites only the matched button's code. Codes with
  // no mapping row are left as the wheel sent them rather than blanked — on a
  // same-protocol wheel/chassis pair they are already the codes the BCM
  // expects, and blanking them would silently drop every control that hasn't
  // been mapped yet.
  uint8_t response[8];
  memcpy(response, recvButtonData, sizeof(response));

  // Except a LIN 1.3 wheel's (decodeAudiFrame()): its frame is a translation,
  // not a LIN 2.x one. Bytes 2/5/7 there are this firmware's "changed bits"
  // and never go to the car as they are (a matched row writes its own code
  // below); an unmapped button code is cleared with its movement/stage byte
  // too, since the wheel's 0x5C1-style codes aren't the car's LIN codes. Done
  // before shaping, which turns that byte into a PQ chassis' position.
  // Same for ANY wheel into a LIN 1.3 car: bytes 2 and 7 become that car's
  // 0x30 byte 0 high nibble and byte 3 (encodeLin13ChassisFrame()), so a VW
  // wheel's own byte 7 (horn) must not land there unless a row puts it there.
  if (!wheelIsVwLin2() || chassisProtocol == CHASSIS_PROTOCOL_LIN13)
  {
    response[2] = 0;
    response[5] = 0;
    response[7] = 0;
    if (pressedMappingIndex(recvButtonData) < 0)
    {
      response[(linButtonByteIndex < 8) ? linButtonByteIndex : 1] = 0;
      if (linRotaryByteIndex < 8)
      {
        response[linRotaryByteIndex] = 0;
      }
    }
  }

  // Reshape for the chassis family before any code is written in, so a mapping
  // that lives in byte 6 (paddles) overwrites the family constant rather than
  // the other way round. Skipped when the LIN1 read failed: getButtonState()
  // zeroes recvButtonData on error, and feeding that zeroed byte 3 to the
  // rotary conversion would invent a jump away from the real position.
  bool wheelFrameValid = false;
  for (uint8_t b = 0; b < sizeof(response) && !wheelFrameValid; b++)
  {
    wheelFrameValid = recvButtonData[b] != 0;
  }
  if (wheelFrameValid)
  {
    shapeFrameForChassis(response);
  }

  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (buttonMappings[i].oldButtonId == 0)
    {
      continue;
    }
    if (recvButtonData[mappingSourceByte(buttonMappings[i])] != buttonMappings[i].oldButtonId)
    {
      continue;
    }
    if (!mappingQualifiers(buttonMappings[i]))
    {
      continue;  // right code, wrong way round — this row is not the one
    }
    {
        // OpenHaldex and Charisma buttons are exclusive; latch buttons are
        // driven by the persistent latched-output path
        // (sendLatchedButtonOutputs). None emits momentary LIN/CAN/resistive
        // output from here.
        if (buttonMappings[i].flags &
            (FLAG_OPENHALDEX_CONTROL | FLAG_CHARISMA_CONTROL | FLAG_LATCH))
        {
          // The raw press must not reach the BCM: an OpenHaldex or Charisma
          // button is ours alone, and a latch button is asserted below for as
          // long as it stays latched, not for the moment it was physically held.
          response[mappingSourceByte(buttonMappings[i])] = 0;
          buttonFound = true;
          break;
        }

        buildChassisButtonFrame(buttonMappings[i].newLinButtonId, transButtonDataLIN);
        placeCarCode(response, buttonMappings[i]);

        // VW radio CAN (V1.22): the held row's To car code, refreshed every
        // poll it is still held. 250 ms covers a missed poll or two.
        if (buttonMappings[i].newLinButtonId != 0)
        {
          vwRadioCode = buttonMappings[i].newLinButtonId;
          vwRadioUntilMs = millis() + 250;
        }

        // Extend the CAN hold window on every LIN poll cycle that sees this button active
        if (buttonMappings[i].canByteIndex < 8 && buttonMappings[i].canBitIndex < 8)
        {
          const uint32_t holdUntil = millis() + static_cast<uint32_t>(canHoldMs);
          portENTER_CRITICAL(&stateMux);
          memset(canHoldFrame, 0, sizeof(canHoldFrame));
          canHoldFrame[buttonMappings[i].canByteIndex] = static_cast<uint8_t>(1U << buttonMappings[i].canBitIndex);
          canHoldUntil = holdUntil;
          // Mirror immediately so the API status reflects CAN data in the same poll as the button name.
          memcpy(lastCanOutFrame, canHoldFrame, sizeof(lastCanOutFrame));
          lastCanOutLen = sizeof(lastCanOutFrame);
          lastCanOutId = canBroadcastId;
          portEXIT_CRITICAL(&stateMux);
        }

        radioResistance = buttonMappings[i].resistiveOhm;
        radioResistanceMs = millis();  // refresh each poll the button is held

        // LIN output to chassis bus — only when enabled, and only when the
        // configured output ID is one the BCM does NOT poll itself. On the
        // default (linOutputId == linButtonInId) the BCM masters that ID on its
        // own ~50 ms schedule and waits for a slave answer, which
        // serviceChassisLinBus() now supplies from chassisResponseFrame;
        // sending our own header for it as well just collides with the BCM's
        // (the ECHO errors throughout docs/lin_monitor_log_1_passthru.csv).
        if (linOutputEnabled && linOutputId != linButtonInId)
        {
          if (!lockLinBus(chassisLinMutex))
          {
            buttonFound = true;
            break;
          }

          chassisLIN.resetStateMachine();
          chassisLIN.resetError();
          chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linOutputId, 8, transButtonDataLIN);

          if (chassisLIN.getError() == LIN_Master_Base::NO_ERROR)
          {
            chassisLinLastOkMs = millis();  // LIN 2 healthy
          }

          unlockLinBus(chassisLinMutex);
        }

        buttonFound = true;
        break;
    }
  }

  // A latched button holds its translated code in the answer frame until it is
  // pressed again, mirroring what the master-send path does in
  // sendLatchedButtonOutputs().
  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (!buttonLatched[i] ||
        (buttonMappings[i].flags & (FLAG_OPENHALDEX_CONTROL | FLAG_CHARISMA_CONTROL)))
    {
      continue;
    }
    if (buttonMappings[i].newLinButtonId == 0)
    {
      continue;
    }
    placeCarCode(response, buttonMappings[i]);
    break;
  }

  portENTER_CRITICAL(&stateMux);
  memcpy(chassisResponseFrame, response, sizeof(chassisResponseFrame));
  portEXIT_CRITICAL(&stateMux);

  // Publish as "LIN out" whichever frame this mode produces: the mastered one
  // when we transmit it ourselves, otherwise the answer prepared above.
  // serviceChassisLinBus() republishes the identical bytes each time it
  // actually answers, so the two never disagree — but publishing here as well
  // means a bench rig with no chassis attached (nothing polling LIN2, so no
  // answer ever sent) still shows the translated frame the wheel just produced.
  // Whether it is reaching a chassis is a separate question, answered by the
  // LIN 2 health indicator rather than by this field.
  const bool mastered = linOutputEnabled && linOutputId != linButtonInId;

  portENTER_CRITICAL(&stateMux);
  memcpy(lastLinOutFrame, mastered ? transButtonDataLIN : response, sizeof(lastLinOutFrame));
  lastLinOutLen = sizeof(lastLinOutFrame);
  lastLinOutId = linOutputId;
  portEXIT_CRITICAL(&stateMux);
}

// Persistent output for latched buttons: while a button's latch is engaged its
// chassis-LIN frame is resent (and its resistance refreshed) every poll, so the
// chassis sees the button held until it is pressed again. The CAN side is kept
// active in broadcastButtonsCAN(). Called each LIN cycle after sendButtonLINFrame().
void sendLatchedButtonOutputs()
{
  if (passthruEnabled)
  {
    // Passthru is pure listen-and-log — see sendButtonLINFrame(). No latch
    // resend, no resistive output, no chassis LIN transmission while it's on.
    return;
  }

  for (size_t i = 0; i < buttonMappingCount; i++)
  {
    if (!buttonLatched[i])
    {
      continue;
    }

    const ButtonMapping& m = buttonMappings[i];
    if (m.flags & (FLAG_OPENHALDEX_CONTROL | FLAG_CHARISMA_CONTROL))
    {
      continue;  // exclusive-control buttons never latch outputs
    }

    // Keep the mapped resistance asserted while latched.
    if (m.resistiveOhm != 0)
    {
      radioResistance = m.resistiveOhm;
      radioResistanceMs = millis();
    }

    // Resend the translated button ID on the chassis LIN bus each cycle. Same
    // gate as sendButtonLINFrame(): when the BCM polls the output ID itself the
    // latched code rides in chassisResponseFrame instead, so mastering it here
    // would only collide with the BCM's header.
    if (linOutputEnabled && linOutputId != linButtonInId && m.newLinButtonId != 0)
    {
      uint8_t latchedFrame[8];
      buildChassisButtonFrame(m.newLinButtonId, latchedFrame);

      if (lockLinBus(chassisLinMutex))
      {
        chassisLIN.resetStateMachine();
        chassisLIN.resetError();
        chassisLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linOutputId, 8, latchedFrame);
        if (chassisLIN.getError() == LIN_Master_Base::NO_ERROR)
        {
          chassisLinLastOkMs = millis();
        }
        unlockLinBus(chassisLinMutex);
      }

      portENTER_CRITICAL(&stateMux);
      memcpy(lastLinOutFrame, latchedFrame, sizeof(lastLinOutFrame));
      lastLinOutLen = sizeof(lastLinOutFrame);
      lastLinOutId = linButtonID;
      portEXIT_CRITICAL(&stateMux);
    }
  }
}

// Poll the accessory-button frame on the steering-wheel bus. Its button byte is
// fed into the same mapping pipeline as the main button frame, so acc buttons
// behave identically. Default layout mirrors the main frame (button in byte 1).
void getAccButtonState()
{
  uint8_t buf[8] = {0};

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linAccInId, 8, buf);

  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();

  if (passthruEnabled)
  {
    logLinAttempt(1, linAccInId, buf, sizeof(buf), linErr);
    logLinRawResidual(1, linAccInId, Serial1);
    logLinRawFrame(1, linAccInId, steeringWheelLIN, linErr);
  }
  reportPoll(healthAcc, linAccInId, steeringWheelLIN, Serial1, linErr);

  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    memset(buf, 0, sizeof(buf));
  }
  else
  {
    swLinLastOkMs = millis();
    if (!passthruEnabled)
    {
      logLinFrameIfChanged(1, linAccInId, buf, sizeof(buf));
    }
  }

  unlockLinBus(steeringWheelLinMutex);

  portENTER_CRITICAL(&stateMux);
  memcpy(lastAccInFrame, buf, sizeof(lastAccInFrame));
  lastAccInLen = sizeof(lastAccInFrame);
  portEXIT_CRITICAL(&stateMux);

  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
  const uint8_t accButton = buf[bi];
  if (accButton != 0)
  {
    latestLinButtonId = accButton;
    latestLinButtonTimestamp = millis();
    if (learnActive)
    {
      captureLearned(accButton, bi, 0);
    }
    // Merge into the momentary output pipeline only when the main frame is idle.
    if (recvButtonData[bi] == 0)
    {
      recvButtonData[bi] = accButton;
    }
  }

  static uint8_t prevAccRaw = 0;
  if (accButton != 0 && prevAccRaw == 0)
  {
    const int idx = pressedMappingIndex(buf);
    if (idx >= 0)
    {
      logLine("[SW] acc id=0x%02X on 0x%02X", accButton, linAccInId);
      handleButtonPressEvent(static_cast<size_t>(idx));
    }
  }
  prevAccRaw = accButton;
}

// Poll the temperature frame on the steering-wheel bus. Captured for the LIN
// monitor / status only — no control action is taken on it yet.
//
// This is a genuine 2-byte LIN frame, not 8 — confirmed from a passthru
// capture (docs/lin_monitor_log_2..6_passthru.csv) where every attempt timed
// out yet consistently left "00 55 BA FE FE 47" (break, sync, PID 0xBA) in
// the UART FIFO: the enhanced checksum over PID 0xBA + data {FE, FE} comes
// out to exactly 0x47, the trailing byte actually on the wire. Requesting 8
// data bytes meant the library's "enough bytes arrived" gate (needs numData
// bytes) was never satisfied by the real 2-byte reply, so it silently timed
// out on every single poll despite the wheel answering correctly every time.
void getTemperatureState()
{
  uint8_t buf[2] = {0};

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linTempInId, sizeof(buf), buf);

  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();

  if (passthruEnabled)
  {
    logLinAttempt(1, linTempInId, buf, sizeof(buf), linErr);
    logLinRawResidual(1, linTempInId, Serial1);
    logLinRawFrame(1, linTempInId, steeringWheelLIN, linErr);
  }
  reportPoll(healthTemp, linTempInId, steeringWheelLIN, Serial1, linErr);

  unlockLinBus(steeringWheelLinMutex);

  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    return;  // keep last known value
  }
  swLinLastOkMs = millis();
  if (!passthruEnabled)
  {
    logLinFrameIfChanged(1, linTempInId, buf, sizeof(buf));
  }

  portENTER_CRITICAL(&stateMux);
  memcpy(lastTempInFrame, buf, sizeof(buf));
  lastTempInLen = sizeof(buf);
  portEXIT_CRITICAL(&stateMux);

  // Heated VW wheels (RS6 C6 logs, docs/lin_monitor_log.csv-11/12.json): byte 0
  // climbed 4B -> 56 with the heater on; byte 1 stayed FE while it was on and
  // flipped FF <-> FE as its button was pressed. For the Diagnostics card.
  noteWheelTemp(buf[0], WHEEL_TEMP_SRC_0X3A);
  // The heated-wheel button (it has no 0x0E code): byte 1 bit 0 clears while
  // it is held (RS6: FF -> FE on press, back to FF on release; the customer:
  // "when I hold the heating button the heater is on, when released it's off",
  // which is what V1.17 did by pulsing the car on BOTH edges). Only the press
  // edge counts now, so one press = one D3 button press = the car toggles the
  // heater once. The first frame only sets the starting state.
  static bool haveHeater = false;
  if (haveHeater && (wheelHeaterByte & 0x01) && !(buf[1] & 0x01))
  {
    wheelHeaterPressMs = millis() | 1U;
    logLine("[SW] heated-wheel button pressed (0x3A byte 1 %02X -> %02X)", wheelHeaterByte, buf[1]);
  }
  haveHeater = true;
  wheelHeaterByte = buf[1];
}

