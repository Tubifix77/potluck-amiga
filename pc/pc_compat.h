/* Force-included into the PC harness's build (pc/CMakeLists.txt): what the emulator's published PC path expects from
 * the original project's own main.cpp and a POSIX C library, so its sources build unchanged with MSVC. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern uint8_t *slow_ram; /* the board build defines it in memory.cpp; the PC build declares it nowhere */
#endif

#ifdef _MSC_VER
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
static inline int clock_gettime(int clk, struct timespec *ts) { (void)clk; return timespec_get(ts, TIME_UTC) ? 0 : -1; }
#endif
#endif
