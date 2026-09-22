#pragma once

/*
  MFSW Controller — firmware version history
  ------------------------------------------
  Bump FW_VERSION in defs.h whenever a new entry is added here.

Unreleased - Independent Chassis Protocol (PQ/MQB) for the button frame sent to the
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
