#include "ble_bridge.h"

void bleInit(const char* deviceName) {}
bool bleConnected() { return false; }
bool bleSecure() { return false; }
uint32_t blePasskey() { return 0; }
void bleClearBonds() {}
size_t bleAvailable() { return 0; }
int bleRead() { return -1; }
size_t bleWrite(const uint8_t* data, size_t len) { return 0; }
