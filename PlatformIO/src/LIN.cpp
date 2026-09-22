#include "LIN.h"

#include "API.h"
#include "globals.h"

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
  if (seen[b][id] && len > 1 && memcmp(lastSeen[b][id] + 1, data + 1, len - 1) == 0)
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
static void logLinAttempt(uint8_t bus, uint8_t id, const uint8_t* data, uint8_t len, LIN_Master_Base::error_t err)
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

  logLine("%s 0x%02X v2 %s len=%u: %s", (bus == 2) ? "LIN2" : "LIN1", id, linErrorLabel(err), (unsigned)len, hex);
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

// The button-frame byte a mapping matches on. 0 (or out of range) means "use
// the global default byte" — keeps pre-existing/hand-added rows on byte 1 while
// Learn assigns paddles/horn their real byte (6/7).
static inline uint8_t mappingSourceByte(const ButtonMapping& m)
{
  if (m.sourceByte >= 1 && m.sourceByte < 8) return m.sourceByte;
  return (linButtonByteIndex < 8) ? linButtonByteIndex : 1;
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
  const bool wheelIsPq   = (wheelProtocol == WHEEL_PROTOCOL_PQ);
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

static void captureLearned(uint8_t value, uint8_t byteIdx)
{
  expireLearnState();
  if (!learnActive || learnRowIndex >= buttonMappingCount)
  {
    return;
  }

  if (learnTarget == LEARN_OLD_LIN)
  {
    buttonMappings[learnRowIndex].oldButtonId = value;
    buttonMappings[learnRowIndex].sourceByte = byteIdx;

    // Direction is NOT inferred here. It looked obvious — capture the sign of
    // the rotary byte while learning — but on the MQB wheel captured in
    // lin_wheel_20260913_160641.csv the plain button codes 0x07 and 0x20 move
    // that byte too (1, then 4, 5, 6 across consecutive held frames, only ever
    // upward, so more like a hold counter than movement). Inferring from it
    // would silently tag ordinary buttons "Up only". The Scroll column on the
    // row is the explicit way to say it instead.
  }
  else if (learnTarget == LEARN_NEW_LIN)
  {
    buttonMappings[learnRowIndex].newLinButtonId = value;
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

  if (havePrev)
  {
    for (uint8_t i = 1; i < 8; i++)
    {
      if (frame[i] != 0 && frame[i] != learnBaseline[i] && prev[i] == learnBaseline[i])
      {
        memcpy(prev, frame, sizeof(prev));
        captureLearned(frame[i], i);
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
void serviceChassisLinBus()
{
  static uint8_t window[2] = {};  // rolling SYNC, PID
  static uint8_t windowLen = 0;

  if (!lockLinBus(chassisLinMutex))
  {
    return;  // bus busy with a normal-mode master transaction; try next pass
  }

  const uint8_t lightPid = calculateProtectedId(linLightInId);
  const uint8_t buttonPid = calculateProtectedId(linButtonInId);

  while (Serial2.available())
  {
    const uint8_t b = (uint8_t)Serial2.read();

    if (windowLen < sizeof(window))
    {
      window[windowLen++] = b;
    }
    else
    {
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

    if (passthruEnabled)
    {
      // Some other ID's header — not one of ours. Log each distinct ID only
      // ONCE (the log ring buffer is just 200 entries — a busy chassis bus
      // can carry dozens of unrelated frames per second, which would drown
      // the button/light entries we actually care about within a second or
      // two). This still answers "does the BCM even schedule 0x0E here, and
      // what else shares this bus" without flooding.
      static uint64_t seenIds = 0;
      const uint8_t id = pid & 0x3F;
      const uint64_t bit = (uint64_t)1 << id;
      if (!(seenIds & bit))
      {
        seenIds |= bit;
        logLine("LIN2 0x%02X header seen (unhandled, first time)", (unsigned)id);
      }
    }
  }

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

  if (wheelProtocol == WHEEL_PROTOCOL_PQ)
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

void getButtonState()
{
  memset(recvButtonData, 0, sizeof(recvButtonData));
  memset(transButtonDataLIN, 0, sizeof(transButtonDataLIN));
  memset(transButtonDataCAN, 0, sizeof(transButtonDataCAN));

  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.receiveSlaveResponseBlocking(LIN_Master_Base::LIN_V2, linButtonInId, 8, recvButtonData);

  // Discard the frame if the library flagged any error (timeout, checksum,
  // echo mismatch, etc.).  Without this, partial bytes written by a failed
  // receive leave garbage in recvButtonData[1] which matches a button mapping
  // and makes a button appear pressed when nothing was touched.
  const LIN_Master_Base::error_t linErr = steeringWheelLIN.getError();

  // Log the raw receive attempt BEFORE any error-based discard below, so a
  // checksum-failed or timed-out frame still shows its actual bytes here.
  if (passthruEnabled)
  {
    logLinAttempt(1, linButtonInId, recvButtonData, 8, linErr);
    logLinRawResidual(1, linButtonInId, Serial1);
    logLinRawFrame(1, linButtonInId, steeringWheelLIN, linErr);
  }

  if (linErr != LIN_Master_Base::NO_ERROR)
  {
    memset(recvButtonData, 0, sizeof(recvButtonData));
  }
  else
  {
    swLinLastOkMs = millis();  // LIN 1 healthy
    if (!passthruEnabled)
    {
      logLinFrameIfChanged(1, linButtonInId, recvButtonData, 8);
    }
  }

  unlockLinBus(steeringWheelLinMutex);

  portENTER_CRITICAL(&stateMux);
  memcpy(lastLinInFrame, recvButtonData, sizeof(lastLinInFrame));
  lastLinInLen = sizeof(lastLinInFrame);
  lastLinInId = linButtonInId;
  portEXIT_CRITICAL(&stateMux);

  const uint8_t bi = (linButtonByteIndex < 8) ? linButtonByteIndex : 1;

  // Scroll-wheel movement for this frame, needed before any matching below:
  // a row filtered to one direction depends on it.
  wheelRotaryDelta = computeRotaryDelta(recvButtonData, linErr == LIN_Master_Base::NO_ERROR);
  wheelPressStage  = (linRotaryByteIndex < 8) ? recvButtonData[linRotaryByteIndex] : 0;

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
    else
    {
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
    logLine("[SW] btn id=0x%02X byte %u", buttonMappings[matchedIdx].oldButtonId,
            (unsigned)mappingSourceByte(buttonMappings[matchedIdx]));
    handleButtonPressEvent(static_cast<size_t>(matchedIdx));
  }
  prevMatchedIdx = matchedIdx;
}

void sendLightLINFrame()
{
  // Light frame goes ONLY to the steering wheel LIN bus.
  // Chassis LIN is a source (read FROM), not a destination for light data.
  if (!lockLinBus(steeringWheelLinMutex))
  {
    return;
  }

  steeringWheelLIN.resetStateMachine();
  steeringWheelLIN.resetError();
  steeringWheelLIN.sendMasterRequestBlocking(LIN_Master_Base::LIN_V2, linLightInId, 4, steeringWheelLightData);

  if (passthruEnabled)
  {
    logLinAttempt(1, linLightInId, steeringWheelLightData, 4, steeringWheelLIN.getError());
  }

  unlockLinBus(steeringWheelLinMutex);
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
        response[mappingSourceByte(buttonMappings[i])] = buttonMappings[i].newLinButtonId;

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
    response[mappingSourceByte(buttonMappings[i])] = buttonMappings[i].newLinButtonId;
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
      captureLearned(accButton, bi);
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

  unlockLinBus(steeringWheelLinMutex);

  if (passthruEnabled)
  {
    logLinAttempt(1, linTempInId, buf, sizeof(buf), linErr);
    logLinRawResidual(1, linTempInId, Serial1);
    logLinRawFrame(1, linTempInId, steeringWheelLIN, linErr);
  }

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
}

