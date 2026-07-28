#pragma once

#include <cstdint>

void canInit();
bool canHealthy();
void pollCanRx();
void broadcastButtonsCAN();
void broadcastGRATask(void* parameter);
void sendOpenHaldexMode(uint8_t mode);
void serviceOpenHaldex();
