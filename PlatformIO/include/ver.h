#pragma once

/*
  MFSW Controller — firmware version history
  ------------------------------------------
  Bump FW_VERSION in defs.h whenever a new entry is added here.

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
