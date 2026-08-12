#include "EasyBLE.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "backends/EasyBLE_Backend.h"

namespace {

constexpr uint8_t OpcodeMessage = 0x01;
constexpr uint8_t OpcodeResult = 0x02;

constexpr size_t MessageHeaderSize = 5;
constexpr size_t ResultRecordSize = 2;

// Worst-case buffer content beyond one max payload: an un-drained RESULT ack,
// a message header, and the RESULT slot send() reserves so sendResult() can
// always write unchecked.
constexpr size_t BufferOverhead = MessageHeaderSize + 2 * ResultRecordSize;

constexpr size_t ReadChunkSize = 244;

uint32_t readUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

void writeUint32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

uint8_t* allocateMessage(size_t length) {
  return static_cast<uint8_t*>(heap_caps_malloc_prefer(
      length, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
      MALLOC_CAP_DEFAULT));
}

}  // namespace

bool EasyBLEClass::begin(const char* deviceName, uint32_t maxMessageSize) {
  if (_rxMessage != nullptr || maxMessageSize < EasyBLEMinimumMaxMessage) {
    return false;
  }

  _rxMessage = allocateMessage(maxMessageSize);
  if (_rxMessage == nullptr) {
    return false;
  }

  _maxMessage = maxMessageSize;
  _connected = false;
  resetLink();

  if (!EasyBLEBackend::begin(deviceName, maxMessageSize + BufferOverhead)) {
    free(_rxMessage);
    _rxMessage = nullptr;
    return false;
  }
  return true;
}

void EasyBLEClass::end() {
  EasyBLEBackend::end();
  free(_rxMessage);
  _rxMessage = nullptr;
  _connected = false;
  resetLink();
}

void EasyBLEClass::update() {
  EasyBLEBackend::poll();
  if (!isConnected()) {
    return;
  }

  uint8_t chunk[ReadChunkSize];
  while (true) {
    const size_t length = EasyBLEBackend::read(chunk, sizeof(chunk));
    if (EasyBLEBackend::rxInvalid()) {
      fail();
      return;
    }

    // A disconnect can race the read. Do not parse bytes from an ended session.
    if (!EasyBLEBackend::ready()) {
      return;
    }
    if (length == 0) {
      break;
    }

    processIncoming(chunk, length);

    // A handler called by the parser may end or fail the session.
    if (!_connected || _failed) {
      return;
    }
  }

  if (_awaitingResult && millis() - _sendStart >= EasyBLEResultTimeoutMs) {
    fail();
  }
}

void EasyBLEClass::onData(DataHandler handler) {
  _onData = handler;
}

void EasyBLEClass::onConnect(ConnectHandler handler) {
  _onConnect = handler;
}

void EasyBLEClass::onDisconnect(DisconnectHandler handler) {
  _onDisconnect = handler;
}

void EasyBLEClass::onSendResult(SendResultHandler handler) {
  _onSendResult = handler;
}

bool EasyBLEClass::send(const uint8_t* data, size_t len) {
  if (!_connected || _failed || _awaitingResult ||
      !EasyBLEBackend::ready() || data == nullptr || len == 0 ||
      len > _maxMessage) {
    return false;
  }

  const size_t required = MessageHeaderSize + len + ResultRecordSize;
  if (EasyBLEBackend::availableForWrite() < required) {
    return false;
  }

  uint8_t header[MessageHeaderSize];
  header[0] = OpcodeMessage;
  writeUint32(header + 1, static_cast<uint32_t>(len));
  if (EasyBLEBackend::write(header, sizeof(header)) != sizeof(header) ||
      EasyBLEBackend::write(data, len) != len) {
    fail();
    return false;
  }

  _awaitingResult = true;
  _sendStart = millis();
  return true;
}

bool EasyBLEClass::isSending() const {
  return _awaitingResult;
}

bool EasyBLEClass::isConnected() const {
  return _connected && !_failed && EasyBLEBackend::ready();
}

void EasyBLEClass::processIncoming(const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length && _connected && !_failed) {
    switch (_rxState) {
      case RxParseState::Opcode: {
        const uint8_t opcode = data[offset++];
        switch (opcode) {
          case OpcodeMessage:
            _rxHeaderLength = 0;
            _rxState = RxParseState::MessageLength;
            break;
          case OpcodeResult:
            _rxState = RxParseState::ResultStatus;
            break;
          default:
            fail();
            break;
        }
        break;
      }
      case RxParseState::MessageLength: {
        _rxHeader[_rxHeaderLength++] = data[offset++];
        if (_rxHeaderLength == sizeof(uint32_t)) {
          const uint32_t messageLength = readUint32(_rxHeader);
          if (messageLength == 0 || messageLength > _maxMessage) {
            fail();
            break;
          }
          _rxExpected = messageLength;
          _rxReceived = 0;
          _rxState = RxParseState::MessagePayload;
        }
        break;
      }
      case RxParseState::MessagePayload: {
        const size_t available = length - offset;
        const size_t remaining = _rxExpected - _rxReceived;
        const size_t take = available < remaining ? available : remaining;
        memcpy(_rxMessage + _rxReceived, data + offset, take);
        offset += take;
        _rxReceived += take;
        if (_rxReceived == _rxExpected) {
          _rxState = RxParseState::Opcode;
          if (sendResult() && _onData) {
            _onData(_rxMessage, _rxExpected);
          }
        }
        break;
      }
      case RxParseState::ResultStatus: {
        const uint8_t status = data[offset++];
        _rxState = RxParseState::Opcode;
        if (status > 1 || !_awaitingResult) {
          fail();
          break;
        }
        _awaitingResult = false;
        if (_onSendResult) {
          _onSendResult(status == 1);
        }
        break;
      }
    }
  }
}

bool EasyBLEClass::sendResult() {
  const uint8_t frame[ResultRecordSize] = {OpcodeResult, 1};
  if (EasyBLEBackend::write(frame, sizeof(frame)) != sizeof(frame)) {
    fail();
    return false;
  }
  return true;
}

void EasyBLEClass::resetLink() {
  _rxState = RxParseState::Opcode;
  _rxExpected = 0;
  _rxReceived = 0;
  _rxHeaderLength = 0;
  _awaitingResult = false;
  _failed = false;
}

void EasyBLEClass::fail() {
  _failed = true;
  EasyBLEBackend::disconnect();
}

void EasyBLEBackend::didConnect() {
  if (EasyBLE._connected) {
    return;
  }

  EasyBLE.resetLink();
  EasyBLE._connected = true;
  if (EasyBLE._onConnect) {
    EasyBLE._onConnect();
  }
}

void EasyBLEBackend::didDisconnect() {
  if (!EasyBLE._connected) {
    return;
  }

  EasyBLE._connected = false;
  const bool sendUnresolved = EasyBLE._awaitingResult;
  EasyBLE.resetLink();

  if (sendUnresolved && EasyBLE._onSendResult) {
    EasyBLE._onSendResult(false);
  }
  if (EasyBLE._onDisconnect) {
    EasyBLE._onDisconnect();
  }
}

EasyBLEClass EasyBLE;
