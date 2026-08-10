#pragma once

#include <Arduino.h>
#include "EasyBLE_Config.h"

class EasyBLEClass {
public:
  using DataHandler = void (*)(const uint8_t* data, size_t len);
  using ConnectHandler = void (*)();
  using DisconnectHandler = void (*)();

  bool begin(const char* deviceName);
  void end();
  void update();

  void onData(DataHandler handler);
  void onConnect(ConnectHandler handler);
  void onDisconnect(DisconnectHandler handler);

  bool send(const uint8_t* data, size_t len);
  bool isConnected() const;

private:
  friend struct EasyBLEBackend;

  DataHandler _onData = nullptr;
  ConnectHandler _onConnect = nullptr;
  DisconnectHandler _onDisconnect = nullptr;
  bool _connected = false;
};

extern EasyBLEClass EasyBLE;
