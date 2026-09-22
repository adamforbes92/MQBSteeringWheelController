#pragma once

// Continuously listens to LIN2 (chassis/BCM bus) and reacts per-ID: decodes
// the light frame, and — in Passthru Mode — answers the button frame's
// header as a slave with the wheel's real data. See LIN.cpp for the full
// rationale. Must be called from a tight, near-continuous loop (see
// chassisLinListenerTask() in tasks.cpp), not the normal ~100 ms LIN1 cadence.
void serviceChassisLinBus();
void getButtonState();
void getAccButtonState();
void getTemperatureState();
void sendLightLINFrame();
void sendButtonLINFrame();
void sendLatchedButtonOutputs();
