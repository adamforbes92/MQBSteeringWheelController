#pragma once

#include <cstdint>

void canInit();
bool canHealthy();
const char* canBusStateName();  // TWAI controller state: running / bus-off / recovering / stopped
void pollCanRx();
void broadcastButtonsCAN();
void broadcastGRATask(void* parameter);
void sendOpenHaldexMode(uint8_t mode);
void serviceOpenHaldex();
void serviceCharisma();
