#include "../EasyBLE_Config.h"

#include <NimBLEDevice.h>

#include <atomic>

#include "../EasyBLE.h"
#include "../EasyBLE_UUIDs.h"
#include "EasyBLE_Backend.h"

namespace {

class StreamServer : public NimBLEStreamServer {
public:
  using NimBLEStream::drainTx;
};

StreamServer streamServer;
std::atomic<bool> rxOverflowed{false};
std::atomic<bool> sessionEnded{false};

void discardSessionIo() {
  streamServer.flush();

  uint8_t discarded[64];
  while (streamServer.read(discarded, sizeof(discarded)) != 0) {
  }
}

void endSession() {
  discardSessionIo();
  rxOverflowed = false;
  sessionEnded = true;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    const uint16_t connectionHandle = connInfo.getConnHandle();
    server->updatePhy(connectionHandle, BLE_GAP_LE_PHY_2M_MASK,
                      BLE_GAP_LE_PHY_2M_MASK, 0);
    server->setDataLen(connectionHandle, 251);
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int) override {
    if (streamServer.getPeerHandle() == connInfo.getConnHandle()) {
      endSession();
    }
  }
};

ServerCallbacks serverCallbacks;

class StreamCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&,
                   uint16_t subValue) override {
    if (subValue == 0) {
      endSession();
    }
  }
};

StreamCallbacks streamCallbacks;

NimBLEStream::RxOverflowAction onRxOverflow(const uint8_t*, size_t, void*) {
  rxOverflowed = true;
  return NimBLEStream::DROP_NEW_DATA;
}

}  // namespace

bool EasyBLEBackend::begin(const char* deviceName, uint32_t txBufferSize,
                           uint32_t rxBufferSize) {
  rxOverflowed = false;
  sessionEnded = false;

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(247);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  if (!streamServer.begin(NimBLEUUID(EASYBLE_SERVICE_UUID),
                          NimBLEUUID(EASYBLE_STREAM_UUID), txBufferSize,
                          rxBufferSize, false)) {
    NimBLEDevice::deinit(true);
    return false;
  }

  streamServer.setRxOverflowCallback(onRxOverflow);
  streamServer.setCallbacks(&streamCallbacks);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(deviceName);
  advertising->addServiceUUID(EASYBLE_SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();
  return true;
}

void EasyBLEBackend::end() {
  if (!NimBLEDevice::isInitialized()) {
    return;
  }
  NimBLEDevice::stopAdvertising();
  rxOverflowed = false;
  sessionEnded = false;
  streamServer.end();
  NimBLEDevice::deinit(true);
}

void EasyBLEBackend::poll() {
  // NimBLEStream can stop sending on larger writes and never retry. We picked a smaller chunk size to avoid this
  // Ask it to send whatever is still queued so a stalled write can finish.
  streamServer.drainTx();

  if (rxInvalid() && !ready()) {
    endSession();
  }

  if (sessionEnded.exchange(false)) {
    didDisconnect();
  }

  if (rxInvalid()) {
    if (!EasyBLE._failed) {
      EasyBLE.fail();
    }
    return;
  }

  if (ready() && !EasyBLE._connected) {
    didConnect();
  }
}

void EasyBLEBackend::disconnect() {
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t peerHandle = streamServer.getPeerHandle();
  if (server != nullptr && peerHandle != BLE_HS_CONN_HANDLE_NONE) {
    server->disconnect(peerHandle);
  }
}

bool EasyBLEBackend::ready() {
  return streamServer.ready();
}

bool EasyBLEBackend::rxInvalid() {
  return rxOverflowed.load();
}

size_t EasyBLEBackend::write(const uint8_t* data, size_t length) {
  return streamServer.write(data, length);
}

size_t EasyBLEBackend::availableForWrite() {
  return streamServer.availableForWrite();
}

size_t EasyBLEBackend::read(uint8_t* buffer, size_t length) {
  return streamServer.read(buffer, length);
}
