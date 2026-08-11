#include "../EasyBLE_Config.h"

#if EASYBLE_BACKEND_NIMBLE

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>

#include "../EasyBLE.h"
#include "../EasyBLE_UUIDs.h"
#include "EasyBLE_Backend.h"

#ifndef EASYBLE_QUEUE_DEPTH
#define EASYBLE_QUEUE_DEPTH 16
#endif

namespace {

struct Event {
  uint16_t length;
  uint8_t data[EASYBLE_MAX_PACKET];
};

NimBLEServer* server = nullptr;
NimBLECharacteristic* rxChar = nullptr;
NimBLECharacteristic* txChar = nullptr;
QueueHandle_t eventQueue = nullptr;
std::atomic<bool> peerReady{false};
std::atomic<uint16_t> peerConnectionHandle{0};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer* bleServer, NimBLEConnInfo& connInfo, int reason) override {
    (void)bleServer;
    (void)connInfo;
    (void)reason;
    peerConnectionHandle = 0;
    peerReady = false;
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    if (eventQueue == nullptr) {
      return;
    }

    const NimBLEAttValue& value = characteristic->getValue();
    const size_t len = value.length();
    if (len == 0 || len > EASYBLE_MAX_PACKET) {
      return;
    }

    Event event;
    event.length = static_cast<uint16_t>(len);
    memcpy(event.data, value.data(), len);
    xQueueSend(eventQueue, &event, 0);
  }
};

class TxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo,
                   uint16_t subValue) override {
    (void)characteristic;
    peerConnectionHandle = connInfo.getConnHandle();
    peerReady = subValue != 0;
  }
};

ServerCallbacks serverCallbacks;
RxCallbacks rxCallbacks;
TxCallbacks txCallbacks;

}  // namespace

bool EasyBLEBackend::begin(const char* deviceName) {
  peerReady = false;

  if (eventQueue == nullptr) {
    eventQueue = xQueueCreate(EASYBLE_QUEUE_DEPTH, sizeof(Event));
    if (eventQueue == nullptr) {
      return false;
    }
  } else {
    xQueueReset(eventQueue);
  }

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(EASYBLE_MAX_PACKET + 3);

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);
  server->advertiseOnDisconnect(true);

  NimBLEService* service = server->createService(EASYBLE_SERVICE_UUID);

  rxChar = service->createCharacteristic(
      EASYBLE_RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      EASYBLE_MAX_PACKET);
  rxChar->setCallbacks(&rxCallbacks);

  txChar = service->createCharacteristic(
      EASYBLE_TX_UUID,
      NIMBLE_PROPERTY::NOTIFY,
      EASYBLE_MAX_PACKET);
  txChar->setCallbacks(&txCallbacks);

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(deviceName);
  advertising->addServiceUUID(EASYBLE_SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();
  return true;
}

void EasyBLEBackend::end() {
  if (server != nullptr) {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
  }
  server = nullptr;
  rxChar = nullptr;
  txChar = nullptr;
  peerConnectionHandle = 0;
  peerReady = false;
  if (eventQueue != nullptr) {
    xQueueReset(eventQueue);
  }
}

void EasyBLEBackend::poll() {
  const bool ready = peerReady.load();
  if (ready && !EasyBLE._connected) {
    didConnect();
  }

  if (eventQueue != nullptr) {
    Event event;
    while (xQueueReceive(eventQueue, &event, 0) == pdTRUE) {
      didReceiveFrame(event.data, event.length);
    }
  }

  if (!ready && EasyBLE._connected) {
    didDisconnect();
  }
}

void EasyBLEBackend::disconnect() {
  const uint16_t connectionHandle = peerConnectionHandle.load();
  if (server != nullptr && peerReady.load()) {
    server->disconnect(connectionHandle);
  }
}

bool EasyBLEBackend::sendFrame(const uint8_t* frame, size_t length) {
  if (!EasyBLE._connected || txChar == nullptr || frame == nullptr ||
      length == 0 || length > maximumFrameSize()) {
    return false;
  }
  txChar->setValue(frame, length);
  return txChar->notify();
}

size_t EasyBLEBackend::maximumFrameSize() {
  return EASYBLE_MAX_PACKET;
}

#endif
