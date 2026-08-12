#include "EasyBLE.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "backends/EasyBLE_Backend.h"

namespace {

constexpr uint8_t OpcodeResult = 0x02;
constexpr uint8_t OpcodeBegin = 0x03;
constexpr uint8_t OpcodeContinue = 0x04;

constexpr size_t BeginHeaderSize = 8;
constexpr size_t ContinueHeaderSize = 3;
constexpr size_t ResultRecordSize = 2;

constexpr size_t MaxChunkFrameSize =
    BeginHeaderSize + EasyBLEChunkPayloadSize;

// One maximum-sized chunk, a RESULT that may already be queued when a receive
// handler calls send(), and the RESULT slot pumpSend() reserves.
constexpr size_t TxBufferSize = MaxChunkFrameSize + 2 * ResultRecordSize;

// Backpressure permits one incoming chunk plus the final RESULT for an
// outgoing message before update() drains the ring.
constexpr size_t RxBufferSize = MaxChunkFrameSize + ResultRecordSize;

constexpr size_t ReadChunkSize = 244;

uint32_t readUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t readUint16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
      (static_cast<uint16_t>(data[1]) << 8);
}

void writeUint32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

void writeUint16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

uint8_t* allocateMessage(size_t length) {
  return static_cast<uint8_t*>(heap_caps_malloc_prefer(
      length, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
      MALLOC_CAP_DEFAULT));
}

}  // namespace

bool EasyBLEClass::begin(const char* deviceName, uint32_t maxMessageSize) {
  if (_rxMessage != nullptr || maxMessageSize < EasyBLEMinimumMaxMessage ||
      maxMessageSize == UINT32_MAX) {
    return false;
  }

  // One extra byte so received payloads can be NUL-terminated for the
  // handler.
  _rxMessage = allocateMessage(maxMessageSize + 1);
  if (_rxMessage == nullptr) {
    return false;
  }

  _maxMessage = maxMessageSize;
  _connected = false;
  resetLink();

  if (!EasyBLEBackend::begin(deviceName, TxBufferSize, RxBufferSize)) {
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

  if (_txMessage != nullptr && _txOffset < _txLength && !pumpSend()) {
    return;
  }

  if (_awaitingResult && millis() - _sendStart >= EasyBLEResultTimeoutMs) {
    fail();
  }
}

void EasyBLEClass::onReceive(ReceiveHandler handler) {
  _onReceive = handler;
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

bool EasyBLEClass::send(EasyBLEMessageType type, const uint8_t* data,
                        size_t length) {
  if (!_connected || _failed || _txMessage != nullptr ||
      !EasyBLEBackend::ready() || data == nullptr) {
    return false;
  }

  if (length == 0 || length > UINT32_MAX) {
    return false;
  }

  _txMessage = allocateMessage(length);
  if (_txMessage == nullptr) {
    return false;
  }

  memcpy(_txMessage, data, length);
  _txType = type;
  _txLength = static_cast<uint32_t>(length);
  _txOffset = 0;
  _awaitingResult = true;
  _sendStart = millis();

  if (!pumpSend()) {
    resetSend();
    return false;
  }
  return true;
}

bool EasyBLEClass::pumpSend() {
  while (_txMessage != nullptr && _txOffset < _txLength) {
    const uint32_t remaining = _txLength - _txOffset;
    const uint16_t chunkLength = static_cast<uint16_t>(
        remaining < EasyBLEChunkPayloadSize ? remaining
                                            : EasyBLEChunkPayloadSize);

    uint8_t header[BeginHeaderSize];
    size_t headerLength;
    if (_txOffset == 0) {
      header[0] = OpcodeBegin;
      header[1] = static_cast<uint8_t>(_txType);
      writeUint32(header + 2, _txLength);
      writeUint16(header + 6, chunkLength);
      headerLength = BeginHeaderSize;
    } else {
      header[0] = OpcodeContinue;
      writeUint16(header + 1, chunkLength);
      headerLength = ContinueHeaderSize;
    }

    const size_t required = headerLength + chunkLength + ResultRecordSize;
    if (EasyBLEBackend::availableForWrite() < required) {
      return true;
    }

    if (EasyBLEBackend::write(header, headerLength) != headerLength ||
        EasyBLEBackend::write(_txMessage + _txOffset, chunkLength) !=
            chunkLength) {
      fail();
      return false;
    }

    _txOffset += chunkLength;
    _sendStart = millis();
  }
  return true;
}

bool EasyBLEClass::sendText(const char* text) {
  if (text == nullptr) {
    return false;
  }
  return send(EasyBLEMessageType::Text,
              reinterpret_cast<const uint8_t*>(text), strlen(text));
}

bool EasyBLEClass::isSending() const {
  return _txMessage != nullptr;
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
          case OpcodeResult:
            _rxState = RxParseState::ResultStatus;
            break;
          case OpcodeBegin:
            if (_rxExpected != 0) {
              fail();
              break;
            }
            _rxState = RxParseState::BeginType;
            break;
          case OpcodeContinue:
            if (_rxExpected == 0) {
              fail();
              break;
            }
            _rxHeaderLength = 0;
            _rxState = RxParseState::ChunkLength;
            break;
          default:
            fail();
            break;
        }
        break;
      }
      case RxParseState::BeginType: {
        const auto type = static_cast<EasyBLEMessageType>(data[offset++]);
        if (type != EasyBLEMessageType::Text &&
            type != EasyBLEMessageType::Image) {
          fail();
          break;
        }
        _rxType = type;
        _rxHeaderLength = 0;
        _rxState = RxParseState::BeginLength;
        break;
      }
      case RxParseState::BeginLength: {
        _rxHeader[_rxHeaderLength++] = data[offset++];
        if (_rxHeaderLength == sizeof(uint32_t)) {
          const uint32_t messageLength = readUint32(_rxHeader);
          if (messageLength == 0) {
            fail();
            break;
          }
          // Drain an oversized message without storing it, then reject the
          // complete message with the single final RESULT.
          _rxDiscard = messageLength > _maxMessage;
          _rxExpected = messageLength;
          _rxReceived = 0;
          _rxHeaderLength = 0;
          _rxState = RxParseState::ChunkLength;
        }
        break;
      }
      case RxParseState::ChunkLength: {
        _rxHeader[_rxHeaderLength++] = data[offset++];
        if (_rxHeaderLength == sizeof(uint16_t)) {
          const uint16_t chunkLength = readUint16(_rxHeader);
          const size_t remaining = _rxExpected - _rxReceived;
          if (chunkLength == 0 || chunkLength > EasyBLEChunkPayloadSize ||
              chunkLength > remaining) {
            fail();
            break;
          }
          _rxChunkExpected = chunkLength;
          _rxChunkReceived = 0;
          _rxState = RxParseState::ChunkPayload;
        }
        break;
      }
      case RxParseState::ChunkPayload: {
        const size_t available = length - offset;
        const size_t remaining = _rxChunkExpected - _rxChunkReceived;
        const size_t take = available < remaining ? available : remaining;
        if (!_rxDiscard) {
          memcpy(_rxMessage + _rxReceived, data + offset, take);
        }
        offset += take;
        _rxReceived += take;
        _rxChunkReceived += take;

        if (_rxChunkReceived == _rxChunkExpected) {
          _rxState = RxParseState::Opcode;
          _rxChunkExpected = 0;
          _rxChunkReceived = 0;
          _rxHeaderLength = 0;

          if (_rxReceived == _rxExpected) {
            const bool accepted = !_rxDiscard;
            const size_t messageLength = _rxExpected;
            _rxDiscard = false;
            _rxExpected = 0;
            _rxReceived = 0;

            if (!accepted) {
              sendResult(false);
            } else {
              _rxMessage[messageLength] = 0;
              if (sendResult(true) && _onReceive) {
                const EasyBLEMessage message = {
                    _rxType,
                    _rxMessage,
                    messageLength,
                };
                _onReceive(message);
              }
            }
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
        if (status == 0) {
          finishSend(false);
          break;
        }

        if (_txOffset != _txLength) {
          fail();
        } else {
          finishSend(true);
        }
        break;
      }
    }
  }
}

void EasyBLEClass::finishSend(bool success) {
  resetSend();
  if (_onSendResult) {
    _onSendResult(success);
  }
}

bool EasyBLEClass::sendResult(bool accepted) {
  const uint8_t frame[ResultRecordSize] = {
      OpcodeResult, static_cast<uint8_t>(accepted ? 1 : 0)};
  if (EasyBLEBackend::write(frame, sizeof(frame)) != sizeof(frame)) {
    fail();
    return false;
  }
  return true;
}

void EasyBLEClass::resetLink() {
  resetSend();
  _rxState = RxParseState::Opcode;
  _rxType = EasyBLEMessageType::Text;
  _rxExpected = 0;
  _rxReceived = 0;
  _rxChunkExpected = 0;
  _rxChunkReceived = 0;
  _rxHeaderLength = 0;
  _rxDiscard = false;
  _failed = false;
}

void EasyBLEClass::resetSend() {
  free(_txMessage);
  _txMessage = nullptr;
  _txLength = 0;
  _txOffset = 0;
  _txType = EasyBLEMessageType::Text;
  _sendStart = 0;
  _awaitingResult = false;
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
  const bool sendUnresolved = EasyBLE._txMessage != nullptr;
  EasyBLE.resetLink();

  if (sendUnresolved && EasyBLE._onSendResult) {
    EasyBLE._onSendResult(false);
  }
  if (EasyBLE._onDisconnect) {
    EasyBLE._onDisconnect();
  }
}

EasyBLEClass EasyBLE;
