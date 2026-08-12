#pragma once

#include <Arduino.h>
#include "EasyBLE_Config.h"

enum class EasyBLEMessageType : uint8_t {
  Text = 0x01,
  Image = 0x02,
};

struct EasyBLEMessage {
  EasyBLEMessageType type;
  // The payload remains valid only for the duration of the receive callback.
  // It is always NUL-terminated, so Text payloads can be used as a C string.
  const uint8_t* data;
  size_t length;
};

class EasyBLEClass {
public:
  using ReceiveHandler = void (*)(const EasyBLEMessage& message);
  using ConnectHandler = void (*)();
  using DisconnectHandler = void (*)();
  using SendResultHandler = void (*)(bool success);

  bool begin(const char* deviceName,
             uint32_t maxMessageSize = EasyBLEDefaultMaxMessage);
  void end();
  void update();

  void onReceive(ReceiveHandler handler);
  void onConnect(ConnectHandler handler);
  void onDisconnect(DisconnectHandler handler);
  void onSendResult(SendResultHandler handler);

  bool send(EasyBLEMessageType type, const uint8_t* data, size_t length);
  bool sendText(const char* text);
  bool isSending() const;
  bool isConnected() const;

private:
  friend struct EasyBLEBackend;

  enum class RxParseState : uint8_t {
    Opcode,
    MessageType,
    MessageLength,
    MessagePayload,
    ResultStatus,
  };

  void processIncoming(const uint8_t* data, size_t length);
  bool sendResult();
  void resetLink();
  void fail();

  ReceiveHandler _onReceive = nullptr;
  ConnectHandler _onConnect = nullptr;
  DisconnectHandler _onDisconnect = nullptr;
  SendResultHandler _onSendResult = nullptr;

  uint32_t _maxMessage = EasyBLEDefaultMaxMessage;
  uint8_t* _rxMessage = nullptr;
  EasyBLEMessageType _rxType = EasyBLEMessageType::Text;
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
