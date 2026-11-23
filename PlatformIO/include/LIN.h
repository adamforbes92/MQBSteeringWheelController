#pragma once

#include <stdint.h>

// Continuously listens to LIN2 (chassis/BCM bus) and reacts per-ID: decodes
// the light frame, and — in Passthru Mode — answers the button frame's
// header as a slave with the wheel's real data. See LIN.cpp for the full
// rationale. Must be called from a tight, near-continuous loop (see
// chassisLinListenerTask() in tasks.cpp), not the normal ~100 ms LIN1 cadence.
void serviceChassisLinBus();
// Passthru relay, LIN1 side (V1.16): with Passthru on and a car heard on LIN2,
// relays every frame both ways byte for byte and returns true — the caller then
// skips its normal LIN1 cycle. False: no car yet (bench behaviour) or Passthru off.
bool servicePassthruRelay();
void getButtonState();
void getAccButtonState();
void getTemperatureState();
void getAudiStatusState();  // Audi wheel frame 0x21, LIN Monitor only
void sendLightLINFrame();
void sendButtonLINFrame();
void sendLatchedButtonOutputs();

// ---- LIN1 wheel detect / ID scan (see runLinScan() in LIN.cpp) ----
// serviceLinScan() runs from the LIN1 task each cycle: a requested scan, or an
// automatic one once the button frame has had no valid answer for 5 s.
struct LinScanHit {
  uint8_t  id;       // frame ID 0x00-0x3B that answered
  uint8_t  len;      // data bytes it really sent
  bool     classic;  // true = LIN 1.x classic checksum, false = 2.x enhanced
  uint8_t  data[8];
  uint16_t baud;     // bus speed it answered at (19200, or 9600 for Volvo)
};
constexpr uint8_t kLinScanMaxHits = 12;
// linScanSuggest: a WHEEL_PROTOCOL_* value when the answer matches a known
// wheel, else one of these.
constexpr uint8_t LIN_SCAN_UNKNOWN = 0xFD;  // something answered, not a known wheel
constexpr uint8_t LIN_SCAN_NOTHING = 0xFE;  // nothing answered at all
constexpr uint8_t LIN_SCAN_NONE    = 0xFF;  // no scan has run yet
extern LinScanHit        linScanHits[kLinScanMaxHits];
extern volatile uint8_t  linScanHitCount;
extern volatile uint32_t linScanSeq;  // +1 per completed scan
extern volatile uint32_t linScanMs;   // millis() it completed
extern volatile uint8_t  linScanSuggest;
void requestLinScan();
void serviceLinScan();

// ---- Wheel profiles (see the "Wheel profiles" section of LIN.cpp) ----
// Once per LIN1 cycle, before any transaction: applies the selected profile's
// LIN1 speed and UART format (8N1 LIN / 8E1 BMW K-bus).
void serviceWheelBus();
uint16_t profileWheelBaud();
