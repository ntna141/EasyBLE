#pragma once

#include <stddef.h>
#include <stdint.h>

struct EasyBLEBackend {
  static bool begin(const char* name);
  static void end();
  static void poll();
  static void disconnect();

  static bool sendFrame(const uint8_t* frame, size_t length);
  static size_t maximumFrameSize();

  static void didConnect();
  static void didDisconnect();
  static void didReceiveFrame(const uint8_t* frame, size_t length);
};
