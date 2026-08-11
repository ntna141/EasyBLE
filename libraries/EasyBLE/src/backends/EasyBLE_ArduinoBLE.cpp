#include "../EasyBLE_Config.h"

#if EASYBLE_BACKEND_ARDUINOBLE

#include <ArduinoBLE.h>
#include "../EasyBLE.h"
#include "../EasyBLE_UUIDs.h"
#include "EasyBLE_Backend.h"

static BLEService service(EASYBLE_SERVICE_UUID);
static BLECharacteristic rxChar(
    EASYBLE_RX_UUID,
    BLEWrite | BLEWriteWithoutResponse,
    EASYBLE_MAX_PACKET);
static BLECharacteristic txChar(
    EASYBLE_TX_UUID,
    BLENotify,
    EASYBLE_MAX_PACKET);

namespace {

BLEDevice peer;
bool hasPeer = false;

void subscribed(BLEDevice device, BLECharacteristic characteristic) {
  (void)characteristic;
  peer = device;
  hasPeer = true;
  EasyBLEBackend::didConnect();
}

void unsubscribed(BLEDevice device, BLECharacteristic characteristic) {
  (void)device;
  (void)characteristic;
  hasPeer = false;
  EasyBLEBackend::didDisconnect();
}

void disconnected(BLEDevice device) {
  (void)device;
  hasPeer = false;
  EasyBLEBackend::didDisconnect();
}

void rxWritten(BLEDevice device, BLECharacteristic characteristic) {
  (void)device;
  const int length = characteristic.valueLength();
  if (length > 0) {
    EasyBLEBackend::didReceiveFrame(
        characteristic.value(), static_cast<size_t>(length));
  }
}

}  // namespace

bool EasyBLEBackend::begin(const char* deviceName) {
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

  BLE.setEventHandler(BLEDisconnected, disconnected);
  rxChar.setEventHandler(BLEWritten, rxWritten);
  txChar.setEventHandler(BLESubscribed, subscribed);
  txChar.setEventHandler(BLEUnsubscribed, unsubscribed);

  BLE.advertise();
  return true;
}

void EasyBLEBackend::end() {
  hasPeer = false;
  BLE.stopAdvertise();
  BLE.end();
}

void EasyBLEBackend::poll() {
  BLE.poll();
}

void EasyBLEBackend::disconnect() {
  if (hasPeer) {
    peer.disconnect();
  }
}

bool EasyBLEBackend::sendFrame(const uint8_t* frame, size_t length) {
  if (!EasyBLE._connected || frame == nullptr || length == 0 ||
      length > maximumFrameSize()) {
    return false;
  }
  return txChar.writeValue(frame, length) != 0;
}

size_t EasyBLEBackend::maximumFrameSize() {
  return EASYBLE_MAX_PACKET;
}

#endif
