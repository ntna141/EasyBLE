#pragma once

#include <stddef.h>
#include <stdint.h>

#if !defined(ARDUINO_ARCH_ESP32)
#error "EasyBLE: only ESP32 boards using NimBLE-Arduino are currently supported"
#endif

constexpr uint32_t EasyBLEDefaultMaxMessage = 4096;
constexpr uint32_t EasyBLEMinimumMaxMessage = 256;
constexpr size_t EasyBLEChannelReserve = 1024;
constexpr size_t EasyBLEDefaultChannelRing = 12 * 1024;
