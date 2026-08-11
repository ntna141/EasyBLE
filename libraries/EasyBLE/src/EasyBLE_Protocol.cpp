#include "EasyBLE_Protocol.h"

#include <Arduino.h>
#include <string.h>

#include "backends/EasyBLE_Backend.h"

namespace {

uint16_t readUint16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
      (static_cast<uint16_t>(data[1]) << 8);
}

void writeUint16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

bool deadlineReached(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

}  // namespace

void EasyBLEProtocol::reset() {
  _tx = TxState{};
  _rx = RxState{};
  _failed = false;
}

bool EasyBLEProtocol::beginSend(const uint8_t* message, size_t length) {
  if (_failed || _tx.active || message == nullptr || length == 0 ||
      length > EASYBLE_MAX_MESSAGE || length > UINT16_MAX) {
    return false;
  }

  memcpy(_txMessage, message, length);
  _tx.message = _txMessage;
  _tx.length = length;
  _tx.offset = 0;
  _tx.active = true;
  return true;
}

void EasyBLEProtocol::update() {
  if (_failed) {
    return;
  }

  if (ackReceiveTimedOut()) {
    fail();
    return;
  }

  if (_rx.hasUnackedFrame) {
    if (_tx.active && canSendFrame(true)) {
      sendDataFrame(true);
      return;
    }

    if (ackIsDue()) {
      sendAckFrame();
    }
    return;
  }

  if (_tx.active && canSendFrame(false)) {
    sendDataFrame(false);
  }
}

bool EasyBLEProtocol::receiveFrame(const uint8_t* frame, size_t frameLength,
                                   EasyBLEMessageView& completed) {
  completed = EasyBLEMessageView{};
  if (_failed) {
    return false;
  }

  if (frame == nullptr || frameLength == 0) {
    fail();
    return false;
  }

  size_t cursor = 0;
  const uint8_t flags = frame[cursor++];
  const uint8_t dataFlags = flags & (Start | Continue | End);
  if ((flags & ~(Start | Continue | End | Ack)) != 0 || flags == 0) {
    fail();
    return false;
  }

  uint8_t acknowledgedSequence = 0;
  if ((flags & Ack) != 0) {
    if (cursor >= frameLength) {
      fail();
      return false;
    }
    acknowledgedSequence = frame[cursor++];
  }

  if (cursor >= frameLength) {
    fail();
    return false;
  }
  const uint8_t sequence = frame[cursor++];
  if (sequence != _rx.nextSequence) {
    fail();
    return false;
  }

  if (dataFlags == 0) {
    if (cursor != frameLength || !isValidAck(acknowledgedSequence)) {
      fail();
      return false;
    }

    acceptAck(acknowledgedSequence);
    _rx.nextSequence++;
    return false;
  }

  if (_rx.localWindow == 0) {
    fail();
    return false;
  }

  const bool isStart = (flags & Start) != 0;
  const bool isContinue = (flags & Continue) != 0;
  const bool isEnd = (flags & End) != 0;
  if ((isStart && isContinue) || (!isStart && !isContinue)) {
    fail();
    return false;
  }

  if (isStart) {
    if (_rx.active || frameLength - cursor < sizeof(uint16_t)) {
      fail();
      return false;
    }

    const uint16_t totalLength = readUint16(frame + cursor);
    cursor += sizeof(uint16_t);
    if (totalLength == 0 || totalLength > EASYBLE_MAX_MESSAGE) {
      fail();
      return false;
    }

    _rx.active = true;
    _rx.expectedLength = static_cast<size_t>(totalLength);
    _rx.receivedLength = 0;
  } else if (!_rx.active) {
    fail();
    return false;
  }

  const size_t payloadLength = frameLength - cursor;
  if (payloadLength == 0 ||
      payloadLength > _rx.expectedLength - _rx.receivedLength) {
    fail();
    return false;
  }

  const size_t completedLength = _rx.receivedLength + payloadLength;
  if (isEnd != (completedLength == _rx.expectedLength)) {
    fail();
    return false;
  }

  if ((flags & Ack) != 0) {
    if (!isValidAck(acknowledgedSequence)) {
      fail();
      return false;
    }
    acceptAck(acknowledgedSequence);
  }

  recordReceivedFrame(sequence);
  memcpy(_rxMessage + _rx.receivedLength, frame + cursor, payloadLength);
  _rx.receivedLength += payloadLength;

  if (!isEnd) {
    return false;
  }

  completed.data = _rxMessage;
  completed.length = _rx.receivedLength;
  resetReceiveMessage();
  return true;
}

void EasyBLEProtocol::fail() {
  _failed = true;
  EasyBLEBackend::disconnect();
}

bool EasyBLEProtocol::isValidAck(uint8_t sequence) const {
  if (!_tx.expectingAck) {
    return false;
  }

  for (uint8_t index = 0; index < _tx.outstandingCount; index++) {
    if (_tx.outstandingSequences[index] == sequence) {
      return true;
    }
  }
  return false;
}

void EasyBLEProtocol::acceptAck(uint8_t sequence) {
  uint8_t acknowledgedCount = 0;
  while (acknowledgedCount < _tx.outstandingCount) {
    acknowledgedCount++;
    if (_tx.outstandingSequences[acknowledgedCount - 1] == sequence) {
      break;
    }
  }

  const uint8_t remaining = _tx.outstandingCount - acknowledgedCount;
  for (uint8_t index = 0; index < remaining; index++) {
    _tx.outstandingSequences[index] =
        _tx.outstandingSequences[index + acknowledgedCount];
  }
  _tx.outstandingCount = remaining;
  _tx.remoteWindow = ReceiveWindowSize - remaining;

  if (remaining == 0) {
    _tx.expectingAck = false;
    stopAckReceiveTimer();
  } else {
    restartAckReceiveTimer();
  }
}

bool EasyBLEProtocol::ackReceiveTimedOut() const {
  return _tx.ackTimerRunning && deadlineReached(_tx.ackDeadline);
}

void EasyBLEProtocol::startAckReceiveTimer() {
  if (!_tx.ackTimerRunning) {
    _tx.ackDeadline = millis() + AckReceiveTimeoutMs;
    _tx.ackTimerRunning = true;
  }
}

void EasyBLEProtocol::stopAckReceiveTimer() {
  _tx.ackDeadline = 0;
  _tx.ackTimerRunning = false;
}

void EasyBLEProtocol::restartAckReceiveTimer() {
  stopAckReceiveTimer();
  startAckReceiveTimer();
}

bool EasyBLEProtocol::canSendFrame(bool includeAck) const {
  return includeAck ? _tx.remoteWindow > 0 : _tx.remoteWindow > 1;
}

bool EasyBLEProtocol::ackIsDue() const {
  return _rx.hasUnackedFrame &&
      (_rx.localWindow <= 1 || deadlineReached(_rx.ackDeadline));
}

void EasyBLEProtocol::recordSentFrame(uint8_t sequence) {
  _tx.outstandingSequences[_tx.outstandingCount++] = sequence;
  _tx.expectingAck = true;
  _tx.nextSequence++;
  _tx.remoteWindow--;
  startAckReceiveTimer();
}

void EasyBLEProtocol::recordReceivedFrame(uint8_t sequence) {
  const bool startAckTimer = !_rx.hasUnackedFrame;
  _rx.nextSequence++;
  _rx.newestUnackedSequence = sequence;
  _rx.localWindow--;
  _rx.hasUnackedFrame = true;

  if (startAckTimer) {
    _rx.ackDeadline = millis() + AckSendTimeoutMs;
  }
}

void EasyBLEProtocol::recordAckSent() {
  _rx.localWindow = ReceiveWindowSize;
  _rx.hasUnackedFrame = false;
  _rx.ackDeadline = 0;
}

bool EasyBLEProtocol::sendDataFrame(bool includeAck) {
  const size_t maximumFrameSize = EasyBLEBackend::maximumFrameSize();
  const bool isStart = _tx.offset == 0;
  size_t headerLength = 1 + 1;
  if (includeAck) {
    headerLength++;
  }
  if (isStart) {
    headerLength += sizeof(uint16_t);
  }

  if (maximumFrameSize <= headerLength ||
      maximumFrameSize > sizeof(_txFrame)) {
    return false;
  }

  const size_t remaining = _tx.length - _tx.offset;
  const size_t payloadCapacity = maximumFrameSize - headerLength;
  const size_t payloadLength =
      remaining < payloadCapacity ? remaining : payloadCapacity;
  const bool isEnd = payloadLength == remaining;

  size_t cursor = 0;
  uint8_t flags = isStart ? Start : Continue;
  if (isEnd) {
    flags |= End;
  }
  if (includeAck) {
    flags |= Ack;
  }
  _txFrame[cursor++] = flags;

  if (includeAck) {
    _txFrame[cursor++] = _rx.newestUnackedSequence;
  }

  const uint8_t sequence = _tx.nextSequence;
  _txFrame[cursor++] = sequence;
  if (isStart) {
    writeUint16(_txFrame + cursor, static_cast<uint16_t>(_tx.length));
    cursor += sizeof(uint16_t);
  }

  memcpy(_txFrame + cursor, _tx.message + _tx.offset, payloadLength);
  cursor += payloadLength;

  if (!EasyBLEBackend::sendFrame(_txFrame, cursor)) {
    return false;
  }

  _tx.offset += payloadLength;
  recordSentFrame(sequence);
  if (includeAck) {
    recordAckSent();
  }
  if (isEnd) {
    resetTransmitMessage();
  }
  return true;
}

bool EasyBLEProtocol::sendAckFrame() {
  if (EasyBLEBackend::maximumFrameSize() < 3) {
    return false;
  }

  _txFrame[0] = Ack;
  _txFrame[1] = _rx.newestUnackedSequence;
  const uint8_t sequence = _tx.nextSequence;
  _txFrame[2] = sequence;
  if (!EasyBLEBackend::sendFrame(_txFrame, 3)) {
    return false;
  }

  _tx.nextSequence++;
  recordAckSent();
  return true;
}

void EasyBLEProtocol::resetReceiveMessage() {
  _rx.expectedLength = 0;
  _rx.receivedLength = 0;
  _rx.active = false;
}

void EasyBLEProtocol::resetTransmitMessage() {
  _tx.message = nullptr;
  _tx.length = 0;
  _tx.offset = 0;
  _tx.active = false;
}
