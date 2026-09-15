#include "EasyBLE_Channel.h"

#include <string.h>

#include "EasyBLE.h"
#include "EasyBLE_Protocol.h"
#include "backends/EasyBLE_Backend.h"

using namespace EasyBLEProtocol;

void EasyBLEChannel::onEnabled(EnabledHandler handler) {
  _onEnabled = handler;
}

void EasyBLEChannel::onRequested(RequestedHandler handler) {
  _onRequested = handler;
}

void EasyBLEChannel::onClosed(ClosedHandler handler) {
  _onClosed = handler;
}

bool EasyBLEChannel::open(const uint8_t* descriptor, size_t length,
                          size_t ringSize) {
  if (_state != State::Closed || !EasyBLE.isConnected() ||
      length > DataMaxPayload ||
      (descriptor == nullptr && length != 0) ||
      ringSize < DataMaxPayload) {
    return false;
  }

  _ring = static_cast<uint8_t*>(malloc(ringSize));
  if (_ring == nullptr) {
    return false;
  }
  _ringCapacity = ringSize;
  _droppedTotal = 0;

  if (length != 0) {
    memcpy(_descriptor, descriptor, length);
  }
  _descriptorLength = static_cast<uint8_t>(length);
  _pendingHeader = true;
  _offerStart = millis();
  _state = State::Offered;
  return true;
}

size_t EasyBLEChannel::write(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0 || _state != State::Ready) {
    return 0;
  }
  if (length > availableForWrite()) {
    _droppedTotal += length;
    if (!_gapPending) {
      _gapPending = true;
      _gapPosition = _head;
      _gapBytes = 0;
    }
    _gapBytes += static_cast<uint32_t>(length);
    return 0;
  }
  ringPut(data, length);
  return length;
}

size_t EasyBLEChannel::availableForWrite() const {
  return _state == State::Ready ? _ringCapacity - ringUsed() : 0;
}

void EasyBLEChannel::close() {
  if (_state == State::Closed) {
    return;
  }
  _pendingEnd = !_pendingHeader;
  _pendingHeader = false;
  releaseRing();
  _state = State::Closed;
}

bool EasyBLEChannel::isOpen() const {
  return _state != State::Closed;
}

bool EasyBLEChannel::isEnabled() const {
  return _state == State::Ready;
}

uint32_t EasyBLEChannel::droppedBytes() const {
  return _droppedTotal;
}

size_t EasyBLEChannel::ringUsed() const {
  return _head - _tail;
}

void EasyBLEChannel::ringPut(const uint8_t* data, size_t length) {
  const size_t index = _head % _ringCapacity;
  const size_t first = _ringCapacity - index < length ? _ringCapacity - index : length;
  memcpy(_ring + index, data, first);
  memcpy(_ring, data + first, length - first);
  _head += length;
}

void EasyBLEChannel::ringGet(uint8_t* data, size_t length) {
  const size_t index = _tail % _ringCapacity;
  const size_t first = _ringCapacity - index < length ? _ringCapacity - index : length;
  memcpy(data, _ring + index, first);
  memcpy(data + first, _ring, length - first);
  _tail += length;
}

void EasyBLEChannel::releaseRing() {
  free(_ring);
  _ring = nullptr;
  _ringCapacity = 0;
  _head = 0;
  _tail = 0;
  _gapPending = false;
  _gapBytes = 0;
}

void EasyBLEChannel::reset() {
  const bool wasReady = _state == State::Ready;
  const bool wasClosing = _pendingEnd || _awaitingEndAck;
  releaseRing();
  _pendingHeader = false;
  _pendingEnd = false;
  _awaitingEndAck = false;
  _state = State::Closed;
  if (wasReady) {
    notifyEnabled(false);
  }
  if (wasClosing) {
    notifyClosed(false);
  }
}

void EasyBLEChannel::notifyEnabled(bool enabled) {
  if (_onEnabled != nullptr) {
    _onEnabled(enabled);
  }
}

void EasyBLEChannel::notifyClosed(bool acked) {
  if (_onClosed != nullptr) {
    _onClosed(acked);
  }
}

void EasyBLEChannel::handleControl(bool enabled) {
  if (!enabled) {
    if (_pendingEnd || _awaitingEndAck) {
      _pendingEnd = false;
      _awaitingEndAck = false;
      notifyClosed(true);
      return;
    }
    reset();
    return;
  }
  switch (_state) {
    case State::Closed:
      if (_onRequested != nullptr) {
        _onRequested();
      }
      break;
    case State::Offered:
      _state = State::Ready;
      notifyEnabled(true);
      break;
    case State::Ready:
      break;
  }
}

bool EasyBLEChannel::emitFrame(uint8_t flags, const uint8_t* payload,
                               size_t length, size_t floor) {
  if (EasyBLEBackend::availableForWrite() < DataHeaderSize + length + floor) {
    return false;
  }
  uint8_t header[DataHeaderSize] = {OpcodeData, flags};
  writeUint16(header + 2, static_cast<uint16_t>(length));
  if (EasyBLEBackend::write(header, sizeof(header)) != sizeof(header) ||
      (length != 0 && EasyBLEBackend::write(payload, length) != length)) {
    EasyBLE.fail();
    return false;
  }
  return true;
}

void EasyBLEChannel::pump(size_t floor) {
  if (_pendingEnd) {
    if (!emitFrame(DataFlagEnd, nullptr, 0, floor)) {
      return;
    }
    _pendingEnd = false;
    _awaitingEndAck = true;
    _endStart = millis();
  }
  if (_awaitingEndAck && millis() - _endStart >= ResultTimeoutMs) {
    _awaitingEndAck = false;
    notifyClosed(false);
  }
  if (_state == State::Closed) {
    return;
  }
  if (_pendingHeader) {
    if (!emitFrame(DataFlagHeader, _descriptor, _descriptorLength, floor)) {
      return;
    }
    _pendingHeader = false;
    _offerStart = millis();
  }
  if (_state == State::Offered) {
    if (millis() - _offerStart >= ResultTimeoutMs) {
      reset();
    }
    return;
  }

  uint8_t frame[DataMaxPayload];
  while (ringUsed() != 0) {
    size_t take = ringUsed() < DataMaxPayload ? ringUsed() : DataMaxPayload;
    uint8_t flags = 0;
    size_t prefix = 0;
    if (_gapPending) {
      if (_gapPosition == _tail) {
        flags = DataFlagGap;
        prefix = GapPrefixSize;
        if (take > DataMaxPayload - prefix) {
          take = DataMaxPayload - prefix;
        }
      } else if (_gapPosition - _tail < take) {
        take = _gapPosition - _tail;
      }
    }

    if (EasyBLEBackend::availableForWrite() <
        DataHeaderSize + prefix + take + floor) {
      return;
    }

    if (prefix != 0) {
      _gapPending = false;
      writeUint16(frame, _gapBytes > UINT16_MAX
                             ? UINT16_MAX
                             : static_cast<uint16_t>(_gapBytes));
      _gapBytes = 0;
    }
    ringGet(frame + prefix, take);
    if (!emitFrame(flags, frame, prefix + take, floor)) {
      return;
    }
  }
}
