#pragma once

/*
  MFSW Controller — firmware version history
  ------------------------------------------
  Bump FW_VERSION in defs.h whenever a new entry is added here.

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
