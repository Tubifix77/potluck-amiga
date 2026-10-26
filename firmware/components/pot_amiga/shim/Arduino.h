// What amiga500-esp32's chipset takes from Arduino when built for a board (memory.cpp's logmsg prints with
// Serial.print): here the emulator's diagnostic lines are counted and dropped -- a board's console blocks ~10 ms a
// line, and it belongs to Potluck's JSON records.
#pragma once
#include <cstdint>
namespace pot_amiga_shim {
extern uint32_t g_log_lines;
}
struct PotAmigaSerial {
    void print(const char*) { ++pot_amiga_shim::g_log_lines; }
};
extern PotAmigaSerial Serial;
