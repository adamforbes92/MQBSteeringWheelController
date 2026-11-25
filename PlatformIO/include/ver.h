#pragma once

/*
  MFSW Controller — firmware version history
  ------------------------------------------
  Bump FW_VERSION in defs.h whenever a new entry is added here.

V1.19 - LIN1 missed-frame timing diagnostic (25/11/2026). Mark's A8 D3 (V1.18):
        the RS6 wheel's lights go out at times; his logs show isolated LIN1
        timeouts, including our own 0x0D light frame (only we drive it) and
        complete, valid frames found in the FIFO just after a "timeout" (e.g.
        0x3A: 00 55 BA 58 FE ED). Rather than guess the cause (task stall vs.
        the library's timing: BREAK faked by a half-baud 0x00 with zero margin,
        timeout = 2x nominal counted from before the BREAK), V1.19 measures it.
        Library (local patch): per-transaction timing - tmBodyUs (header
        written), tmHeaderUs (header echo in), tmDoneUs (done / timed out),
        tmMaxGapUs (longest gap between handler() calls), getTiming().
        LIN.cpp logLinTiming(): on the first failure after a frame was
        answering (reportPoll() and both 0x0D light-send paths), waits up to
        20 ms for the rest of a timed-out frame and logs two "TIMING" lines:
        body / echo / gap / gave-up vs limit, then when it was really complete
        (+late), bytes received / expected, header echo check (00 55 PID).
        At most one per 250 ms; not in Passthru. No behaviour change otherwise.
V1.18 - A8 D3 heated wheel: latch + "warmer by" (24/11/2026). Customer (Mark,
        Messenger, on V1.17 in the car): "When I hold steering wheel heating
        button, heater is on. When release it's off" - and the button can't be
        learned (it's not in 0x0E). The RS6's 0x3A byte 1 is momentary (FE
        held, FF released); V1.17 pulsed the car's d3 = 08 on every flip, so
        press + release = two D3 presses = on, then off. getTemperatureState()
        now sets wheelHeaterPressMs only on the press edge (bit 0 1 -> 0), and
        tasks.cpp polls 0x3A every 2 cycles (~200 ms, was ~1 s) when Chassis
        Protocol is LIN 1.3, so a short tap isn't lost between polls. He also
        asked for a way to make the wheel warmer (VCDS can't on the D3):
        heaterTempOffset (globals, NVS "heatOfs", /api/setup, 0..15) is taken
        off the rim temperature sent to the car in 0x30 d5 (floor 01); the 0x62
        cut-off still tests the wheel's real reading. UI: "Heated wheel: warmer
        by" under Chassis Protocol, shown only for LIN 1.3. His log 16
        (docs/lin_monitor_log.csv-16.json, "sometimes it does this") shows the
        car sending 0x10 F8 5D / F8 1D (lane-assist frame, "high") and once an
        unanswered 0x10 header - what "this" was isn't stated; not acted on.
V1.17 - VAG LIN 1.3 Chassis Protocol (24/11/2026). Customer request (Mark,
        Messenger): an RS6 wheel in an Audi A8 D3, where Chassis Protocol only
        offered PQ / MQB / MEB. CHASSIS_PROTOCOL_LIN13 = 3 (globals.h; /api/setup
        and NVS accept it through CHASSIS_PROTOCOL_MAX). The D3 car is the LIN
        master and asks for its own wheel's frames, classic checksum, which the
        V1.16 relay captures showed (docs/lin_monitor_log.csv-13/15.json):
        asks 0x30 (6 bytes) and 0x21 (2 bytes, D3 wheel: 23 01); sends 0x01
        (5E 03 / 5F 03) and 0x10 (3 bytes, lane-assist vibration: F3 / F5 / F8
        = weak / middle / high, byte 1 bit 5 toggling).
        In normal mode with LIN output on, serviceChassisLinBus() answers F0 /
        61 itself (answerLin13Chassis()): 0x30 = encodeLin13ChassisFrame(), the
        inverse of decodeAudiFrame() over chassisResponseFrame: d0 = counter
        (+ byte 2 as high nibble), d1 = 90 A0 10 20 30 40 50 60 by counter & 7
        (D3 and first-A8 captures), d2 = row's new code, or F1 / F2 -> roller
        movement in d4 high / low nibble, d3 = byte 7 bits, d5 = rim
        temperature. 0x21 = 23 01. sendButtonLINFrame() now clears bytes 2/5/7
        and unmapped codes for ANY wheel into this chassis (was decoded wheels
        only), so a VW wheel's horn byte can't reach d3.
        Heated wheel: the RS6 wheel has no 0x0E code for it, only 0x3A byte 1
        flipping FF <-> FE per press (logs 11/12). getTemperatureState() now
        records each flip (wheelHeaterPressMs, logged "[SW] heated-wheel
        button"); the D3 car gets d3 = 08 for 300 ms per flip.
        Rim temperature (d5, which the car regulates the heater on): the
        wheel's own reading (0x3A byte 0, or a LIN 1.3 wheel's byte 5) while
        under 3 s old and below 0x62; otherwise FF ("sensor open"). Scales look
        alike (~46 at 21 C on both) - NOT confirmed.
        NOT done: vibration to the new wheel (its frame is unknown); 0x01 not
        decoded (backlight: use Aux / Forced). Web UI: option "VAG LIN 1.3
        (Audi A8 D3 etc.; frame 0x30)", saved as 3; the protocol-change prompt
        explains instead of translating codes. NOT tested in a car yet.
V1.16 - Passthru relays any frame, any checksum (23/11/2026). For fitting
        the module in a customer's A8 to capture heating / vibration traffic.
        Up to V1.15 Passthru relayed only two frames and re-built them: the
        car's button poll was answered with 8 bytes + an ENHANCED checksum
        computed over our copy (for a LIN 1.3 wheel, the DECODED frame), the
        light frame was only taken as 4 bytes + enhanced checksum, every other
        ID the car asked for went unanswered, and nothing else the car sent
        reached the wheel (the module sent its own fixed 0x01 "50 03"
        instead). An A8 car (0x30 6 bytes classic, 0x21, 0x01, heater...)
        could not have worked through it.
        Now (LIN.cpp "Passthru relay"), with Passthru on and a car heard on
        LIN2 (3 headers): every LIN2 header is handled generically. An ID is
        watched once: data from the car = the car sends it -> relayed to the
        wheel on LIN1 byte for byte, its checksum byte included (raw BREAK /
        SYNC / PID written to Serial1, not the library, which would add its own
        checksum); silence = the car asks -> the wheel is asked the same PID on
        LIN1 and its answer, every byte as received (any length, classic /
        enhanced / no valid checksum), is what the car gets on its next ask
        (served from LIN2 straight from the header, as before). LIN1 runs every
        1 ms in this state, mirroring the car's schedule; the normal cycle
        (profile polls, our own light / 0x01 frames) does not run. Logging:
        one [RELAY] line per ID with its direction, the car's frames and the
        wheel's answers on change (counter bytes masked) labelled enhanced /
        classic / "no valid checksum", wheel silent / LIN1 echo fault /
        recovered. LIN2 frames with no valid checksum are logged on change in
        passthru (were: one line per 10 s). Lin2FrameLog holds 9 bytes (8 +
        checksum); logFrameOnChange() takes the checksum kind incl. "none".
        Until a car is heard the old bench behaviour runs (profile button poll
        + light frame, every attempt logged); once heard it never returns to it
        until Passthru is toggled, so the car's sleep isn't undone by our polls.
        Not with the BMW K-bus profile. NOT tested on a car yet.
V1.15 - Phone touch UI (22/11/2026). Shared touchui.js / touchui.css from
        _SharedTheme copied into data/ and loaded before style.css / app.js:
        no long-press copy / context menu (text boxes keep it), and scrolling
        the page never moves a slider (a range only changes on a sideways
        drag; touch-action: pan-y). No firmware logic change.

V1.14 - Learn fix + VW heater temperature (18/11/2026). From Mark's RS6 C6
        and A8 D3 logs (docs/lin_monitor_log.csv-2, 4, 10..12.json) and chat.
        Learn: getButtonState() fed every poll to captureLearnedFromFrame(),
        including failed ones, whose recvButtonData is zeroed. That reset each
        constant byte's baseline to 0 ("a byte that returns to 0 is at rest"),
        so the next good frame looked like a fresh press on it. Wheels that
        answer late time out every few seconds, so it happened often: the RS6
        C6 (VW 0x0E, byte 4 always 01) got "Previous" learned as byte 4 = 01,
        which then matched every frame (log 2: btn id=0x01 byte 4 row=0) - the
        customer's stuck Previous; on a LIN 1.3 wheel the decoded byte 4 filler
        0x31 got learned ("sometimes it gets id 0x31"). Learn now only sees
        error-free frames. Rows already learned that way keep the orange
        "unusual byte" outline and need re-learning.
        Heater temperature: noteWheelTemp() (globals) records raw / min / max /
        source from LIN 1.3 0x30 byte 5 and now also VW 0x3A byte 0 (RS6: rose
        4B -> 56 with the heater on); 0x3A byte 1 kept as wheelHeaterByte (RS6:
        FE while heating, FF <-> FE as the heater button was pressed).
        /api/status: wheelTempRaw/Src/Min/Max (+ wheelHeaterByte for 0x3A)
        while fresh (< 3 s), any profile. UI card shows on LIN 1.3 or whenever
        data arrives, labels its source, adds the heater-flag tile. Estimate
        is now raw - 49: both wheels read about 0x46 in a 21 C room (customer),
        where MQB's published raw - 68 gave 2 C and he saw 9 C at 21 C; still
        one point, 1 C per step assumed.
V1.13 - LIN 1.3 wheel: byte 5 is a temperature, not buttons (17/11/2026).
        From a customer's Audi A8 D3 wheel (docs/lin_monitor_log.csv-5..9.json):
        0x30 d5 rose 46 -> 4A under a hair dryer and read FF (0D/0C/09/30/3A
        while making contact) with the heated-rim sensor unplugged, so it is
        the heater temperature. decodeAudiFrame() read its changed bits into
        decoded byte 5, so each temperature step "pressed" a row learned on
        byte 5 for 30 s until the rest caught up (the customer's stuck
        "Previous"). Now only d0's high nibble and d3 are tracked; decoded byte
        5 is always 0. The raw LIN Monitor line still shows d5. Web UI: a
        byte-5 row on a decoded wheel now has the orange "unusual byte" outline,
        to be re-learned or deleted. Same logs: the D3 heating button is d3 =
        08 for as long as it is held (customer's label) - learnable as a byte-7
        row, no change needed.
        Diagnostics: new "Wheel Heater Sensor" card (LIN 1.3 profile only):
        d5 raw, sensor state (FF = open / unplugged), min/max since boot, and
        "raw - 68" in degrees C, MQB's published 0x3A rule, labelled as
        unconfirmed for this wheel. Firmware: wheelTempRaw/Min/Max/Ms
        (globals), set from every good 0x30 frame; /api/status sends
        wheelTempRaw (+ Min/Max) while 0x30 frames are under 3 s old.
V1.12 - Wheel profiles (16/11/2026). Wheel Protocol is now a profile: LIN1
        speed, checksum, frames polled, how buttons are coded and what is sent
        to the wheel. Sources for each: docs/wheel-protocol-research.md. NONE of
        the new profiles has been tested on a real wheel here.
        3 VW Up! / Citigo / Mii (NSF): PQ frame and codes, but 0x0D is 2 bytes
          {brightness, FF} (riquenunes/vw-steering-wheel-gamepad: without the FF
          the wheel stops reporting). PQ family for rotary/shaping
          (wheelIsPqFamily()); scan recognises it by 0x0C (4 bytes) next to 0x0E.
        4 Volvo P1: 9600, classic, poll 0x20 (4 bytes), bit decoder
          (laurynas/volvo_linbus). No light frame known.
        5 Volvo P3: 9600, classic, poll 0x30 + 0x20 (lengths found from the
          answer), bit decoder, 0x10 = FF 00.. every 500 ms (Arduino forum
          1435919).
        6 BMW E-series / Mini R5x MFL on the K/I-bus: Serial1 reopened 9600 8E1
          (reopenWheelUart(), Legacy-PCB swap kept), listen only — nothing on
          LIN1 is ever transmitted in this profile; frames parsed by length +
          XOR; source 0x50 cmd 0x32 volume -> scroll code F1, cmd 0x3B buttons
          01/08/40/80 with press/hold/release (wilhelm-docs, sda2).
        7 Custom: CustomWheelConfig (globals.h), NVS blob "custWheel", /api/setup
          "customWheel": speed 19200/10417/9600, checksum enhanced/classic/auto,
          1-2 button frames (length 0 = found from the answer and remembered,
          re-probed after 5 failures), code-byte (+ optional signed-nibble
          scroll byte) or one-bit-per-button decoder, optional master frame to
          the wheel with a brightness byte and interval.
        Plumbing: LIN library patch setBaudrate() (the library re-applies its
        rate every transaction, so no port reopen); serviceWheelBus() applies
        the profile's speed/format each LIN1 cycle; probeSlaveResponse()
        (shared with the scan) finds an answer's length + checksum; the bit
        decoder keeps a per-byte rest (relearned after 30 s) and ignores a byte
        changing 4 frames running (counter); every decoded profile builds the
        same MQB-shaped frame as the LIN 1.3 one; non-VW profiles skip the
        0x0F / 0x3A polls; logFrameOnChange() (from the LIN2 capture) logs the
        wheel's raw frames. Wheel scan: a second pass at 9600 when nothing
        answers at 19200; hits carry baud; recognises NSF / Volvo P1 / P3.
        UI: grouped Wheel Protocol list; Setup > Advanced > Custom wheel form
        with "Fill from last scan"; scan hits show their speed.
        From a code review before the first build: the generic decoder and
        learned lengths reset when the profile / Custom frames change (else
        the new wheel read as presses for up to 30 s), and report nothing for
        the first 6 frames while counters are recognised; K-bus: no gap timer
        (it measured time between 100 ms reads, so dropped split frames),
        volume clamp no longer blocks the other direction, volume waits while
        a button is held (no false release); a scan stops at once if the
        K-bus profile is chosen mid-scan; the LIN2 capture logs only after the
        RX FIFO is drained (logLine also writes to the serial console, which
        can block — never before answering the BCM), and while collecting a
        frame our own light / button IDs also need a real header (BREAK
        first), so a 55 + PID inside another frame's data isn't answered.

V1.11 - Web UI: "Fit a new wheel" wizard + wheel detect (16/11/2026).
        Buttons tab: a four-step wizard (current wheel -> swap -> new wheel ->
        test & save) on the existing probe Learn (LEARN_PROBE): step 1 records
        each row's To car code from the wheel fitted now and saves (the swap
        powers the controller off), step 2 sets the new wheel's protocol /
        activation preset and waits for it to answer (LIN1 scan), step 3 learns
        each row's From wheel code (rollers go through the scroll-wheel
        wizard; Skip / Not on this wheel / Add extra button), step 4 saves and
        lights each row as its button is pressed, with Export. Progress from
        step 2 survives a page reload. It waits for the wheel to be back at
        rest before arming each Learn (a baseline taken while a button is held
        stops that byte ever capturing).
        Table: columns renamed From wheel (oldButtonId) / To car
        (newLinButtonId), codes shown and typed in hex, each Learn button sits
        in its code cell and counts down while armed; on a phone each row is a
        card with its outputs folded behind an Outputs toggle; the row being
        pressed is highlighted live.
        Fix: every normally learned row had an orange "suspect byte" outline.
        Learn stores the byte it captured from (sourceByte 1 for a plain
        button, captureLearned() in LIN.cpp) but srcByteSuspect() accepted only
        0, 6 and 7; the Button data byte now counts as normal too (and bytes 2
        and 5 on a LIN 1.3 wheel).
        Setup split into "Wheel & car" (always shown) and "Advanced"
        (collapsed). Wheel-detect banner on Dashboard / Setup when the last
        LIN1 scan suggests a different Wheel Protocol (one-click switch), finds
        an unknown wheel, or nothing answers; Diagnostics > LIN Monitor "Scan
        LIN1" button with the answering IDs. Status messages now show on the
        Buttons / Setup tabs (they went to the OTA tab's status line).
        Wheel Protocol 2 relabelled "VAG LIN 1.3 (Audi A4/A8, early Golf 5 /
        Leon / Octavia 2; frame 0x30)".
        Firmware, from the same work: Learn no longer goes deaf when armed
        while a button is held — captureLearnedFromFrame() resets a byte's
        idle reference to 0 when that byte reads 0 (bytes resting at a non-zero
        constant are untouched); fixes the table's Learn and the scroll wizard,
        not just the fit wizard. /api/status adds matchedRow (the row the LIN
        task matched, -1 none; the live highlight uses it instead of guessing
        by name) and uptimeMs (the UI shows how old the last LIN1 scan is).

V1.10 -Audi wheel protocol + serial console (16/11/2026).
        Serial: a customer reported no serial output at all. Up to V1.09 the
        console was only started, and logLine() only mirrored, with
        enableDebug (0 in every release), so nothing reached USB. Now
        Serial.begin(115200) always runs, a [BOOT] line names the firmware and
        board, and every LIN Monitor line is mirrored as "[LOG] ..." (not while
        the SavvyCAN serial analyzer owns the port; it goes back to 115200 when
        that is switched off). DevKit: 4 KB TX ring so the LIN tasks don't
        block on UART0; S3 native USB: 10 ms TX timeout.
        Audi wheel: Wheel Protocol 2 = Audi (LIN 1.3), from the same customer's
        A8 wheel. The capture shows it never answers 0x0E: it answers 0x30
        (6 data bytes) and 0x21 (2 bytes), every frame valid only under the
        CLASSIC checksum. It is the generic VAG LIN 1.3 wheel, not Audi-only:
        the same frames come from an Audi A4 2013 (Arduino forum thread 934599;
        its dumps carry PIDs C1/61/F0) and early PQ35 Leon 1P / Octavia 2
        wheels on a LIN-1.3 J527 (github katzenberger/vag-mfsw-lin-bus-protocol,
        Saleae captures). There d0 low nibble / d1 follow the counter, d2 is
        the wheel's own BUTTON CODE (PQ35: the radio-CAN 0x5C1 codes; A4:
        single-bit values), d3 0x04 = horn (PQ35), d4 = two signed roller
        nibbles (A8 log: only the high one moved, 1..6; A4: 10/F0, 01/0F).
        decodeAudiFrame() builds an MQB-shaped frame: byte1 = d2 as sent, or
        0xF1/0xF2 while a roller moves; byte3 = movement or a timed press stage;
        bytes 7/5/2 = bits of d3/d5/d0-high that differ from rest (first frame;
        relearned after 30 s changed). Learn / rollers / long press / chassis
        shaping run on it unchanged; unmapped codes and bytes 2/5/7 never reach
        the car. The master frame 0x01 = "50 03" (PQ35 J527, ignition on) is
        sent instead of VW's 0x0D; 0x21 is polled ~1 s and logged on change;
        0x0F / 0x3A polls are skipped. Passthru log says v1 for these frames.
        Unknown: A8 controls, and backlight — on B7/B8-era Audi the button
        light is reportedly a terminal-58s PWM wire, not LIN.
        LIN Monitor (the real complaint: the customer's web UI log was empty).
        A normal-mode log held only intact, changed frames, so a wheel on
        another protocol — every poll a TIMEOUT — logged nothing at all. Now:
        - poll health: each polled LIN1 frame logs when it stops answering
          (reason + any bytes that arrived: FIFO residual on TIMEOUT, captured
          frame on CHK), when it recovers, and every 10 s while the BUTTON
          frame stays down (notePollResult() / reportPoll()). Master-send
          failures of the 0x0D light frame are reported the same way.
        - wheel detect: runLinScan() asks every ID 0x00-0x3B for a slave
          response and reports each answer's real length + checksum (OK = 8
          enhanced; CHK with a classic-valid raw buffer = 8 classic; TIMEOUT
          with bytes left in the FIFO = shorter frame, either checksum). Names
          known signatures (0x0E PQ/MQB by idle constants, 0x30 Audi) and says
          when that is not the selected Wheel Protocol. Automatic after 5 s of
          no valid button frame (max once a minute, logged only when the
          result changes), or POST /api/lin/scan. Result in /api/status
          "linScan" {seq, ms, suggest, hits[]}.
        - backlight: the 0x0D frame sent to the wheel is logged on change
          (max 2/s). On LIN2 every frame this device doesn't handle is now
          captured in normal mode too — collected without blocking until the
          bus is quiet or the next real header (valid parity + BREAK) starts,
          length/checksum found from the bytes, logged on first sight and on
          change, per-byte counters masked after 4 changes in a row, max 4/s
          per ID; an unanswered header is noted once. This is how an unknown
          car's backlight frame is found. Replaces passthru's once-per-ID
          "header seen" line. /api/log/clear now takes the log's own lock.

V1.09 - MEB (ID.x) chassis protocol (13/11/2026), from PR #2 (waldo22, MQB
        Atlas/Taos buttons in an ID.4 / ID. Buzz). Chassis Protocol 2 = MEB:
        CHASSIS_PROTOCOL_MEB, accepted by /api/setup and NVS (was clamped back
        to MQB). The firmware builds an MEB chassis frame exactly like MQB (the
        ID.4 captures show the same 0x0E/0x0F layout); defaults are unchanged
        (PQ wheel -> PQ chassis). Web UI: "MEB (ID.x, pre-2024)" option; the
        protocol-change prompt knows MEB's codes (MQB's, except pre-2024 MEB
        Previous/Next = 0x04/0x05, per waldo22's report, not bench-checked).
        Also in the prompt: a CHASSIS change now looks each row's wheel code up
        directly, so a same-family table (the PQ->PQ default, MQB->MEB) can be
        translated to a cross-family chassis at all; paddle/horn rows (source
        byte 6/7) are no longer mistaken for button codes (Paddles (both) 0x03
        looked like PQ Previous). No MEB wheel protocol: not captured.
        Also: GRA paddle frames (0x38A) pause while another module (a real
        gateway) is already sending 0x38A, logged on change; GRA task stack
        2048 -> 3072 bytes for those log lines.
V1.08 - Scroll-wheel wizard (09/10/2026). Adding the partner row from a confirm
        dialog straight after Learn often failed to take — rows were inserted
        while Learn, which is addressed by row index, could still be armed.
        Now, when Learn finds a scroll wheel (or Split / + row is clicked), a
        wizard asks for Scroll UP, Scroll DOWN and an optional Press, showing
        "Up Learned" / "Down Learned" / "Pressed Learned" as each lands, and
        only then replaces the row with one row per movement in a single save.
        The movement used for the original Learn is pre-filled. A press that
        sends the scroll code itself is refused (it would also fire as Scroll
        up). The replaced row's outputs carry over to the direction it was
        learned as. Firmware: new Learn target LEARN_PROBE (4) captures a code
        without writing any row, 10 s window; /api/status reports
        learnProbeSeq / learnProbeCode / learnProbeByte / learnProbeRotary.
V1.07 - Scroll-wheel (roller) detection and assignment, ported from the DevKit-only
        "V1.06 - Scroll" build (that tree's V1.06 is NOT this tree's V1.06 S3
        release; this V1.07 carries both). From the 02/10/2026 MQB capture
        (scroll.csv): a roller reports a signed per-frame detent count in byte 3
        (01..03 up, 0F..0C down) under one code per roller (0x06 left, 0x12
        right); a plain button reports 1 on its first frame, then 4/5/6 held,
        never 2/3 or negative. So a negative or +2/+3 step marks a code as a
        roller, persisted (NVS "rollerCodes"), whether seen during Learn or in
        normal use. Learn sets Scroll up/down on the learned row when the step
        is unambiguous or the code is a known roller (scroll DOWN to learn one),
        and clears a stale direction otherwise. The UI lists detected rollers
        (with Forget), flags roller rows that still fire either way with a
        Split button, and offers/adds the missing Scroll up/down partner row.
        /api/status and /api/setup report rollerCodes; /api/rollers/clear.
        Also picks up the LIN monitor fix from V1.05 (02/10/2026): while a
        control is active, frames differing only in byte 0 are still logged, so
        consecutive scroll detents each appear; "[SW] btn" log lines carry the
        matched row, scroll delta and press stage.
V1.06 - S3 board support, ONE codebase for both PCBs: platformio.ini has two envs,
        esp32dev (-DMFSW_BOARD_DEVKIT, the DevKit boards, unchanged behaviour) and
        esp32-s3 (-DMFSW_BOARD_S3, the ESP32-S3-WROOM-1-N16R8 board). Board macros
        in include/board.h, per-board pins in defs.h. S3 pin map (from
        PCBs/rev3-final): LIN1 TX/RX 17/16, LIN2 TX/RX 6/5, LIN wake 18, LIN NSLP
        15, CAN RX/TX 13/14, X9C103 U/D/INC/CS 9/10/11, high-side drive 21, aux
        light 7, LED 2, native USB on 19/20. Console/esptool over USB-C; 16 MB
        partition table. OTA: /api/ota/info reports boardId, releases.json carries
        per-board images ("boards"), make_release.py --env packages
        Releases/V<ver>/ (DevKit) and Releases/V<ver>-S3/ (S3).
        Also ships everything below that was pending as "Unreleased":
        Independent Chassis Protocol (PQ/MQB) for the button frame sent to the
        chassis/BCM, separate from Wheel Protocol, so any combination bridges (PQ/PQ,
        PQ/MQB, MQB/PQ, MQB/MQB); changing either protocol prompts to update button
        rows still on a known PQ/MQB code, leaving learned/customised rows alone.
        Dashboard "BCM" tile and raw incoming BCM data, independent of the active
        backlight source (the BCM light frame is now polled every cycle regardless
        of source). LIN 2 health split into "any transaction" vs "light frame
        specifically" so a silent BCM isn't masked by button-output traffic.
        LIN Monitor CSV export. Passthru Mode: bypasses mapping/protocol-shaping
        entirely to relay frames unmodified between the wheel and chassis buses,
        and logs every poll/send attempt (success or failure, any length) instead
        of only successful, changed frames. Removed two leftover UI references to
        the LIN ID scanner retired in V1.03. "PNP" wording unified to "High-Side
        MOSFET" throughout the UI.
V1.05 - OTA overhaul (shared ota_manager / wifi_manager v2 + data/ota.js, ported
        from OpenHaldex 9.00): upload callbacks no longer answer mid-body (the
        old per-chunk "200 OK" made the browser drop the connection after the
        first 1.4 kB - a crash in AsyncTCP and a half-written partition, so no
        OTA through the UI had ever completed); filesystem updates unmount
        first, check the announced size, verify the mount and wipe on failure;
        boot only mounts a sane superblock and the web server always starts -
        with no usable UI "/" is a recovery page with the two uploads.
        "Update from GitHub" on the OTA tab (Releases/releases.json via
        tools/make_release.py) plus a Home WiFi (bridge mode) card so the phone
        has internet and the controller at once; power_manager holds WiFi up
        while any browser is active. Assets served no-cache (ETag) instead of
        the hand-bumped ?v=.
V1.04 - Adopted the shared Forbes Automotive UI theme (common style.css) for a
        standardised look across all products; per-product cache-busting on web
        assets (style.css/app.js?v=mfsw-<ver>). wifi_manager/ota_manager made
        project-independent (source of the shared modules).
V1.03 - MQB steering-wheel support. Added a selectable PQ/MQB wheel protocol: MQB
        wheels only report buttons once the master publishes a validated 0x0D
        backlight activation frame (byte 0 brightness; bytes 1-3 an enable gate,
        with older-MQB and MQB Evo presets). Added settings import/export as JSON.
        Retired the interactive LIN ID scanner, wake fuzzer, diagnostic probe and
        raw monitor in favour of the LIN monitor.
V1.02 - Configurable incoming LIN frame IDs (button, accessory, temperature and
        light) so wheels using non-standard IDs are supported, plus a two-bus
        LIN ID scanner in Diagnostics to discover which ID a wheel's buttons use
        (press-to-highlight, one-click assign). Added polling of the accessory-
        button and temperature frames, a live LIN monitor/event log in the UI,
        and a Legacy PCB option that reverses RX/TX on both LIN channels for
        older boards (reboot required).
V1.01 - Latch now applies to CAN and LIN outputs (not just the high-side driver):
        a latched button holds its LIN frame, CAN bit and resistance active until
        pressed again. Added OpenHaldex mode control per button (exclusive) with a
        fixed-mode or "Push-to-Next" option, closed-loop confirmation via the
        OpenHaldex broadcast (0x6B0) and automatic resend until the mode matches.
V1.00 - initial release
*/
