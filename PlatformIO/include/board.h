#pragma once

// ---------------------------------------------------------------------------
// Board selection — ONE codebase, two PCBs. Pick the board in platformio.ini
// (each [env:...] sets exactly one of these build flags):
//
//   -DMFSW_BOARD_DEVKIT  V1/V2 boards: ESP32 DevKit V1 (WROOM-32) on headers.
//   -DMFSW_BOARD_S3      S3 board: ESP32-S3-WROOM-1-N16R8 (U8) soldered down,
//                        native USB-C, BOOT/RESET buttons, on-board 3.3 V LDO.
//
// Pin numbers live in defs.h under the same macros. Only depends on sdkconfig
// so it can be included from anywhere.
// ---------------------------------------------------------------------------
#include "sdkconfig.h"

#if defined(MFSW_BOARD_DEVKIT) && defined(MFSW_BOARD_S3)
#error "MFSW_BOARD_DEVKIT and MFSW_BOARD_S3 are mutually exclusive - check platformio.ini build_flags"
#endif

// No flag given: follow the chip.
#if !defined(MFSW_BOARD_DEVKIT) && !defined(MFSW_BOARD_S3)
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define MFSW_BOARD_S3 1
#else
#define MFSW_BOARD_DEVKIT 1
#endif
#endif

#ifndef MFSW_BOARD_DEVKIT
#define MFSW_BOARD_DEVKIT 0
#endif
#ifndef MFSW_BOARD_S3
#define MFSW_BOARD_S3 0
#endif

#if MFSW_BOARD_S3 && !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "MFSW_BOARD_S3 must be built for an ESP32-S3 target (board = esp32-s3-devkitc-1)"
#endif
#if MFSW_BOARD_DEVKIT && !defined(CONFIG_IDF_TARGET_ESP32)
#error "MFSW_BOARD_DEVKIT must be built for the classic ESP32 (board = esp32dev)"
#endif

#define MFSW_HAS_NATIVE_USB  MFSW_BOARD_S3  // console/esptool over USB-Serial-JTAG, no CH340

// ---- Identity strings (OTA release channel + boot banner) --------------------
// MFSW_BOARD_ID selects this board's images in Releases/releases.json ("boards")
// and the Releases/V<ver>[-S3]/ layout written by tools/make_release.py.
#if MFSW_BOARD_S3
#define MFSW_BOARD_ID   "esp32s3"
#define MFSW_BOARD_NAME "ESP32-S3 board"
#else
#define MFSW_BOARD_ID   "esp32"
#define MFSW_BOARD_NAME "ESP32 DevKit V1 board"
#endif
