#pragma once

/*
  MFSW Controller — firmware version history
  ------------------------------------------
  Bump FW_VERSION in defs.h whenever a new entry is added here.

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
