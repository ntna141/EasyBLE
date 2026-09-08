#include "EasyBLE.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "backends/EasyBLE_Backend.h"

namespace {

constexpr uint8_t OpcodeResult = 0x02;
constexpr uint8_t OpcodeBegin = 0x03;
constexpr uint8_t OpcodeContinue = 0x04;
constexpr uint8_t OpcodeOffer = 0x05;
constexpr uint8_t OpcodeAck = 0x06;

constexpr size_t BeginHeaderSize = 8;
constexpr size_t ContinueHeaderSize = 3;
constexpr size_t ResultRecordSize = 2;
constexpr size_t AckRecordSize = 1;
constexpr size_t ControlRecordsSize = 2 * (ResultRecordSize + AckRecordSize);

constexpr size_t MaxChunkFrameSize =
    BeginHeaderSize + EasyBLEChunkPayloadSize;

constexpr size_t TxBufferSize = MaxChunkFrameSize + ControlRecordsSize;
constexpr size_t RxBufferSize = MaxChunkFrameSize + ControlRecordsSize;

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

bool EasyBLEClass::begin(const char* deviceName, uint32_t maxMessageSize,
                         uint32_t startupBufferSize) {
  if (_started || maxMessageSize < EasyBLEMinimumMaxMessage ||
      maxMessageSize == UINT32_MAX) {
    return false;
  }

  _rxStartupCapacity =
      startupBufferSize < maxMessageSize ? startupBufferSize : maxMessageSize;
  if (!allocateRxBuffer(_rxStartupCapacity)) {
    return false;
  }

  _maxMessage = maxMessageSize;
  _connected = false;
  resetLink();

  if (!EasyBLEBackend::begin(deviceName, TxBufferSize, RxBufferSize)) {
    free(_rxMessage);
    _rxMessage = nullptr;
    _rxCapacity = 0;
    return false;
  }
  _started = true;
  return true;
}

void EasyBLEClass::end() {
  EasyBLEBackend::end();
  free(_rxMessage);
  _rxMessage = nullptr;
  _rxCapacity = 0;
  _started = false;
  _connected = false;
  resetLink();
}

bool EasyBLEClass::allocateRxBuffer(size_t capacity) {
  free(_rxMessage);
  _rxMessage = allocateMessage(capacity + 1);
  _rxCapacity = _rxMessage == nullptr ? 0 : capacity;
  return _rxMessage != nullptr;
}

bool EasyBLEClass::ensureRxCapacity(size_t length) {
  if (_rxMessage != nullptr && length <= _rxCapacity) {
    return true;
  }
  if (allocateRxBuffer(length)) {
    return true;
  }
  allocateRxBuffer(_rxStartupCapacity);
  return false;
}

void EasyBLEClass::shrinkRxBuffer() {
  if (!_started || _rxCapacity <= _rxStartupCapacity) {
    return;
  }
  allocateRxBuffer(_rxStartupCapacity);
}

bool EasyBLEClass::acceptMessage(size_t length) {
  _rxExpected = length;
  _rxReceived = 0;
  _rxStreaming = _onStream != nullptr &&
      notifyStream(EasyBLEStreamStatus::Begin, length, nullptr, 0);
  // Drain an oversized message without storing it, then reject the
  // complete message with the single final RESULT.
  _rxDiscard = !_rxStreaming &&
      (length > _maxMessage || !ensureRxCapacity(length));
  return !_rxDiscard;
}

void EasyBLEClass::resetRxMessage() {
  _rxExpected = 0;
  _rxReceived = 0;
  _rxDiscard = false;
  _rxStreaming = false;
  _rxOffered = false;
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

void EasyBLEClass::onStream(StreamHandler handler) {
  _onStream = handler;
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
  if (_txMessage == nullptr || _txOffset >= _txLength || _awaitingAck) {
    return true;
  }

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

  const size_t required =
      headerLength + chunkLength + ResultRecordSize + AckRecordSize;
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
  _awaitingAck = _txOffset < _txLength;
  _sendStart = millis();
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
          case OpcodeAck:
            if (!_awaitingAck) {
              fail();
              break;
            }
            _awaitingAck = false;
            pumpSend();
            break;
          case OpcodeBegin:
            if (_rxExpected != 0 && !_rxOffered) {
              fail();
              break;
            }
            _rxHeaderOpcode = opcode;
            _rxState = RxParseState::HeaderType;
            break;
          case OpcodeOffer:
            if (_rxExpected != 0) {
              fail();
              break;
            }
            _rxHeaderOpcode = opcode;
            _rxState = RxParseState::HeaderType;
            break;
          case OpcodeContinue:
            if (_rxExpected == 0 || _rxOffered) {
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
      case RxParseState::HeaderType: {
        const auto type = static_cast<EasyBLEMessageType>(data[offset++]);
        if ((type != EasyBLEMessageType::Text &&
             type != EasyBLEMessageType::Image) ||
            (_rxOffered && type != _rxType)) {
          fail();
          break;
        }
        _rxType = type;
        _rxHeaderLength = 0;
        _rxState = RxParseState::HeaderLength;
        break;
      }
      case RxParseState::HeaderLength: {
        _rxHeader[_rxHeaderLength++] = data[offset++];
        if (_rxHeaderLength != sizeof(uint32_t)) {
          break;
        }
        const uint32_t messageLength = readUint32(_rxHeader);
        _rxHeaderLength = 0;
        if (messageLength == 0) {
          fail();
          break;
        }
        if (_rxHeaderOpcode == OpcodeOffer) {
          _rxOffered = acceptMessage(messageLength);
          if (!_rxOffered) {
            resetRxMessage();
          }
          sendResult(_rxOffered);
          _rxState = RxParseState::Opcode;
          break;
        }
        if (_rxOffered) {
          if (messageLength != _rxExpected) {
            fail();
            break;
          }
          _rxOffered = false;
        } else {
          acceptMessage(messageLength);
        }
        _rxState = RxParseState::ChunkLength;
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
          if (_rxStreaming && !ensureRxCapacity(chunkLength)) {
            abortStream();
            _rxDiscard = true;
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
          memcpy(_rxMessage + (_rxStreaming ? _rxChunkReceived : _rxReceived),
                 data + offset, take);
        }
        offset += take;
        _rxReceived += take;
        _rxChunkReceived += take;

        if (_rxChunkReceived == _rxChunkExpected) {
          const size_t chunkLength = _rxChunkExpected;
          _rxState = RxParseState::Opcode;
          _rxChunkExpected = 0;
          _rxChunkReceived = 0;
          _rxHeaderLength = 0;

          if (_rxStreaming &&
              !notifyStream(EasyBLEStreamStatus::Data, _rxExpected,
                            _rxMessage, chunkLength)) {
            _rxStreaming = false;
            _rxDiscard = true;
          }

          if (_rxReceived != _rxExpected) {
            sendAck();
            break;
          }

          const bool accepted = !_rxDiscard;
          const bool streaming = _rxStreaming;
          const size_t messageLength = _rxExpected;
          resetRxMessage();

          if (!accepted) {
            sendResult(false);
          } else if (streaming) {
            sendResult(notifyStream(EasyBLEStreamStatus::End, messageLength,
                                    nullptr, 0));
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
          shrinkRxBuffer();
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

bool EasyBLEClass::sendControl(const uint8_t* frame, size_t length) {
  if (EasyBLEBackend::write(frame, length) != length) {
    fail();
    return false;
  }
  return true;
}

bool EasyBLEClass::sendResult(bool accepted) {
  const uint8_t frame[ResultRecordSize] = {
      OpcodeResult, static_cast<uint8_t>(accepted ? 1 : 0)};
  return sendControl(frame, sizeof(frame));
}

bool EasyBLEClass::sendAck() {
  const uint8_t frame[AckRecordSize] = {OpcodeAck};
  return sendControl(frame, sizeof(frame));
}

bool EasyBLEClass::notifyStream(EasyBLEStreamStatus status, size_t totalLength,
                                const uint8_t* data, size_t length) {
  const EasyBLEStreamEvent event = {status, _rxType, totalLength, data, length};
  return _onStream(event);
}

void EasyBLEClass::abortStream() {
  if (!_rxStreaming) {
    return;
  }
  _rxStreaming = false;
  notifyStream(EasyBLEStreamStatus::Aborted, _rxExpected, nullptr, 0);
}

void EasyBLEClass::resetLink() {
  abortStream();
  resetSend();
  _rxState = RxParseState::Opcode;
  _rxType = EasyBLEMessageType::Text;
  _rxChunkExpected = 0;
  _rxChunkReceived = 0;
  _rxHeaderLength = 0;
  resetRxMessage();
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
  _awaitingAck = false;
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
  EasyBLE.shrinkRxBuffer();

  if (sendUnresolved && EasyBLE._onSendResult) {
    EasyBLE._onSendResult(false);
  }
  if (EasyBLE._onDisconnect) {
    EasyBLE._onDisconnect();
  }
}

EasyBLEClass EasyBLE;
