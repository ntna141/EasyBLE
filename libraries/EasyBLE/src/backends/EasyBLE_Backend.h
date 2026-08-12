#pragma once

#include <stddef.h>
#include <stdint.h>

struct EasyBLEBackend {
  static bool begin(const char* name, uint32_t txBufferSize,
                    uint32_t rxBufferSize);
  static void end();
  static void poll();
  static void disconnect();
  static bool ready();
  static bool rxInvalid();

  static size_t write(const uint8_t* data, size_t length);
  static size_t availableForWrite();
  static size_t read(uint8_t* buffer, size_t length);

  static void didConnect();
  static void didDisconnect();
};
