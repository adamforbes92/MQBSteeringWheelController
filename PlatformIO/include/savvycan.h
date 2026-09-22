#pragma once

#include <driver/twai.h>

// ============================================================================
// SavvyCAN / CAN analyzer interface
// ============================================================================
// Ported from Can2Cluster (which took it from OpenHaldex / SpeedPulserPro).
// Streams every received CAN frame to an external analyzer:
//   WiFi   — GVRET binary (SavvyCAN) or Lawicel/SLCAN (CANHacker) over TCP:23.
//   Serial — GVRET binary over USB at 1 Mbaud.
// The two are mutually exclusive.
//
// This is the live-streaming counterpart to the built-in CAN monitor: the
// monitor answers "what is on this bus" from the device's own 300-frame ring
// buffer, this hands the whole stream to a real analyzer with no buffer limit.
//
// Connect over WiFi:   SavvyCAN -> Add New Device Connection -> Network
//                      Connection (GVRET), using this device's IP, port 23.
// Connect over Serial: SavvyCAN -> Add New Device Connection -> GVRET
//                      Compatible Device, and pick the COM port.
//
// NOTE serial mode reopens Serial at 1 Mbaud, so the debug console is not
// usable while it is on.

#define ANALYZER_PROTOCOL_GVRET   0  // SavvyCAN GVRET binary (default)
#define ANALYZER_PROTOCOL_LAWICEL 1  // CANHacker SLCAN/Lawicel text

void setupAnalyzer();
void setAnalyzerMode(bool enable);
void setAnalyzerSerialMode(bool enable);
void analyzerQueueFrame(const twai_message_t& frame, uint8_t bus);
