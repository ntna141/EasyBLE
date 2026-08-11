#pragma once

#include <stddef.h>
#include <stdint.h>

#include "EasyBLE_Config.h"

struct EasyBLEMessageView {
  const uint8_t* data = nullptr;
  size_t length = 0;
};

class EasyBLEProtocol {
public:
  void reset();
  bool beginSend(const uint8_t* message, size_t length);
  void update();

  bool receiveFrame(const uint8_t* frame, size_t frameLength,
                    EasyBLEMessageView& completed);

private:
  static constexpr uint8_t ReceiveWindowSize = 6;
  static constexpr uint32_t AckSendTimeoutMs = 2500;
  static constexpr uint32_t AckReceiveTimeoutMs = 15000;

  enum FrameFlag : uint8_t {
    Start = 1 << 0,
    Continue = 1 << 1,
    End = 1 << 2,
    Ack = 1 << 3,
  };

  struct TxState {
    const uint8_t* message = nullptr;
    size_t length = 0;
    size_t offset = 0;
    uint8_t nextSequence = 0;
    uint8_t oldestUnackedSequence = 0;
    uint8_t newestUnackedSequence = 0;
    uint8_t remoteWindow = ReceiveWindowSize;
    uint32_t ackDeadline = 0;
    bool active = false;
    bool expectingAck = false;
    bool ackTimerRunning = false;
  };

  struct RxState {
    size_t expectedLength = 0;
    size_t receivedLength = 0;
    uint8_t nextSequence = 0;
    uint8_t newestUnackedSequence = 0;
    uint8_t localWindow = ReceiveWindowSize;
    uint32_t ackDeadline = 0;
    bool active = false;
    bool hasUnackedFrame = false;
  };

  void fail();
  bool isValidAck(uint8_t sequence) const;
  void acceptAck(uint8_t sequence);
  bool ackReceiveTimedOut() const;
  void startAckReceiveTimer();
  void stopAckReceiveTimer();
  void restartAckReceiveTimer();
  bool canSendFrame(bool includeAck) const;
  bool ackIsDue() const;
  void recordSentFrame(uint8_t sequence);
  void recordReceivedFrame(uint8_t sequence);
  void recordAckSent();
  bool sendDataFrame(bool includeAck);
  bool sendAckFrame();
  void resetReceiveMessage();
  void resetTransmitMessage();

  TxState _tx;
  RxState _rx;
  bool _failed = false;
  uint8_t _txFrame[EASYBLE_MAX_PACKET];
  uint8_t _txMessage[EASYBLE_MAX_MESSAGE];
  uint8_t _rxMessage[EASYBLE_MAX_MESSAGE];
};
