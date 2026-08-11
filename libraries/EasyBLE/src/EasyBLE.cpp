#include "EasyBLE.h"
#include "EasyBLE_Protocol.h"
#include "backends/EasyBLE_Backend.h"

namespace {

EasyBLEProtocol protocol;

}  // namespace

bool EasyBLEClass::begin(const char* name) {
  _connected = false;
  protocol.reset();
  return EasyBLEBackend::begin(name);
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

void EasyBLEClass::end() {
  EasyBLEBackend::end();
  protocol.reset();
  _connected = false;
}

void EasyBLEClass::update() {
  EasyBLEBackend::poll();
  protocol.update();
}

bool EasyBLEClass::send(const uint8_t* data, size_t len) {
  if (!_connected) {
    return false;
  }
  return protocol.beginSend(data, len);
}

bool EasyBLEClass::isConnected() const {
  return _connected;
}

void EasyBLEBackend::didConnect() {
  if (EasyBLE._connected) {
    return;
  }

  protocol.reset();
  EasyBLE._connected = true;
  if (EasyBLE._onConnect) {
    EasyBLE._onConnect();
  }
}

void EasyBLEBackend::didDisconnect() {
  if (!EasyBLE._connected) {
    return;
  }

  protocol.reset();
  EasyBLE._connected = false;
  if (EasyBLE._onDisconnect) {
    EasyBLE._onDisconnect();
  }
}

void EasyBLEBackend::didReceiveFrame(const uint8_t* frame, size_t length) {
  EasyBLEMessageView completed;
  if (protocol.receiveFrame(frame, length, completed) && EasyBLE._onData) {
    EasyBLE._onData(completed.data, completed.length);
  }
}

EasyBLEClass EasyBLE;
