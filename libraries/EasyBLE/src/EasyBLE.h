#pragma once

#include <Arduino.h>
#include "EasyBLE_Config.h"

class EasyBLEClass {
public:
  using DataHandler = void (*)(const uint8_t* data, size_t len);
  using ConnectHandler = void (*)();
  using DisconnectHandler = void (*)();
  using SendResultHandler = void (*)(bool success);

  bool begin(const char* deviceName,
             uint32_t maxMessageSize = EasyBLEDefaultMaxMessage);
  void end();
  void update();

  void onData(DataHandler handler);
  void onConnect(ConnectHandler handler);
  void onDisconnect(DisconnectHandler handler);
  void onSendResult(SendResultHandler handler);

  bool send(const uint8_t* data, size_t len);
  bool isSending() const;
  bool isConnected() const;

private:
  friend struct EasyBLEBackend;

  enum class RxParseState : uint8_t {
    Opcode,
    MessageLength,
    MessagePayload,
    ResultStatus,
  };

  void processIncoming(const uint8_t* data, size_t length);
  bool sendResult();
  void resetLink();
  void fail();

  DataHandler _onData = nullptr;
  ConnectHandler _onConnect = nullptr;
  DisconnectHandler _onDisconnect = nullptr;
  SendResultHandler _onSendResult = nullptr;

  uint32_t _maxMessage = EasyBLEDefaultMaxMessage;
  uint8_t* _rxMessage = nullptr;
  size_t _rxExpected = 0;
  size_t _rxReceived = 0;
  uint8_t _rxHeader[4] = {};
  uint8_t _rxHeaderLength = 0;
  RxParseState _rxState = RxParseState::Opcode;

  uint32_t _sendStart = 0;
  bool _awaitingResult = false;
  bool _failed = false;
  bool _connected = false;
};

extern EasyBLEClass EasyBLE;
