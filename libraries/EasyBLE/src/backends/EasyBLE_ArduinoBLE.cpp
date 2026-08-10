#include "../EasyBLE_Config.h"

#if EASYBLE_BACKEND_ARDUINOBLE

#include <ArduinoBLE.h>
#include "../EasyBLE.h"
#include "../EasyBLE_UUIDs.h"

static BLEService service(EASYBLE_SERVICE_UUID);
static BLECharacteristic rxChar(
    EASYBLE_RX_UUID,
    BLEWrite | BLEWriteWithoutResponse,
    EASYBLE_MAX_PACKET);
static BLECharacteristic txChar(
    EASYBLE_TX_UUID,
    BLENotify,
    EASYBLE_MAX_PACKET);

struct EasyBLEBackend {
  static void setConnected(bool connected) {
    if (connected == EasyBLE._connected) {
      return;
    }
    EasyBLE._connected = connected;
    if (connected) {
      if (EasyBLE._onConnect) {
        EasyBLE._onConnect();
      }
    } else if (EasyBLE._onDisconnect) {
      EasyBLE._onDisconnect();
    }
  }

  static void subscribed(BLEDevice device, BLECharacteristic characteristic) {
    (void)device;
    (void)characteristic;
    setConnected(true);
  }

  static void unsubscribed(BLEDevice device, BLECharacteristic characteristic) {
    (void)device;
    (void)characteristic;
    setConnected(false);
  }

  static void disconnected(BLEDevice device) {
    (void)device;
    setConnected(false);
  }

  static void rxWritten(BLEDevice device, BLECharacteristic characteristic) {
    (void)device;
    const int len = characteristic.valueLength();
    if (len > 0 && EasyBLE._onData) {
      EasyBLE._onData(characteristic.value(), static_cast<size_t>(len));
    }
  }
};

void EasyBLEClass::onData(DataHandler handler) {
  _onData = handler;
}

void EasyBLEClass::onConnect(ConnectHandler handler) {
  _onConnect = handler;
}

void EasyBLEClass::onDisconnect(DisconnectHandler handler) {
  _onDisconnect = handler;
}

bool EasyBLEClass::begin(const char* deviceName) {
  _connected = false;

  if (!BLE.begin()) {
    return false;
  }

  BLE.setLocalName(deviceName);
  BLE.setDeviceName(deviceName);

  static bool attributesAdded = false;
  if (!attributesAdded) {
    service.addCharacteristic(rxChar);
    service.addCharacteristic(txChar);
    attributesAdded = true;
  }
  BLE.addService(service);
  BLE.setAdvertisedService(service);

  BLE.setEventHandler(BLEDisconnected, EasyBLEBackend::disconnected);
  rxChar.setEventHandler(BLEWritten, EasyBLEBackend::rxWritten);
  txChar.setEventHandler(BLESubscribed, EasyBLEBackend::subscribed);
  txChar.setEventHandler(BLEUnsubscribed, EasyBLEBackend::unsubscribed);

  BLE.advertise();
  return true;
}

void EasyBLEClass::end() {
  BLE.stopAdvertise();
  BLE.end();
  _connected = false;
}

void EasyBLEClass::update() {
  BLE.poll();
}

bool EasyBLEClass::send(const uint8_t* data, size_t len) {
  if (!_connected || data == nullptr || len == 0 || len > EASYBLE_MAX_PACKET) {
    return false;
  }
  return txChar.writeValue(data, len) != 0;
}

bool EasyBLEClass::isConnected() const {
  return _connected;
}

EasyBLEClass EasyBLE;

#endif
