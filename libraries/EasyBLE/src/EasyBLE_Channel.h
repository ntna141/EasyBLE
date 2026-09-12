#pragma once

#include <Arduino.h>

#include "EasyBLE_Config.h"
#include "EasyBLE_Protocol.h"

class EasyBLEChannel {
public:
  using EnabledHandler = void (*)(bool enabled);
  using RequestedHandler = void (*)();

  void onEnabled(EnabledHandler handler);
  void onRequested(RequestedHandler handler);

  bool open(const uint8_t* descriptor, size_t length,
            size_t ringSize = EasyBLEDefaultChannelRing);
  size_t write(const uint8_t* data, size_t length);
  size_t availableForWrite() const;
  void close();
  bool isOpen() const;
  bool isEnabled() const;
  uint32_t droppedBytes() const;

private:
  friend class EasyBLEClass;

  enum class State : uint8_t {
    Closed,
    Offered,
    Ready,
  };

  void pump(size_t floor);
  void handleControl(bool enabled);
  void reset();
  void releaseRing();
  bool emitFrame(uint8_t flags, const uint8_t* payload, size_t length,
                 size_t floor);
  size_t ringUsed() const;
  void ringPut(const uint8_t* data, size_t length);
  void ringGet(uint8_t* data, size_t length);
  void notifyEnabled(bool enabled);

  State _state = State::Closed;
  uint8_t _descriptor[EasyBLEProtocol::DataMaxPayload] = {};
  uint8_t _descriptorLength = 0;
  bool _pendingHeader = false;
  bool _pendingEnd = false;
  uint32_t _offerStart = 0;
  uint8_t* _ring = nullptr;
  size_t _ringCapacity = 0;
  size_t _head = 0;
  size_t _tail = 0;
  bool _gapPending = false;
  size_t _gapPosition = 0;
  uint32_t _gapBytes = 0;
  uint32_t _droppedTotal = 0;
  EnabledHandler _onEnabled = nullptr;
  RequestedHandler _onRequested = nullptr;
};
