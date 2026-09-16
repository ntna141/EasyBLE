#include "../EasyBLE_Config.h"

#include <NimBLEDevice.h>

#include <atomic>

#include "../EasyBLE.h"
#include "../EasyBLE_Protocol.h"
#include "EasyBLE_Backend.h"

namespace {

class StreamServer : public NimBLEStreamServer {
public:
  using NimBLEStream::drainTx;
};

StreamServer deviceToPhoneStream;
StreamServer phoneToDeviceStream;
std::atomic<bool> rxOverflowed{false};
std::atomic<bool> sessionEnded{false};
std::atomic<uint16_t> pendingHandle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> pendingSince{0};
bool lowPower = false;
bool connParamsDirty = false;
uint32_t connParamsAt = 0;

void requestConnParams() {
  connParamsDirty = true;
  connParamsAt = millis() + EasyBLEProtocol::ConnParamsDelayMs;
}

void applyConnParams() {
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t handle = deviceToPhoneStream.getPeerHandle();
  if (server == nullptr || handle == BLE_HS_CONN_HANDLE_NONE) {
    connParamsDirty = false;
    return;
  }
  if (!connParamsDirty || static_cast<int32_t>(millis() - connParamsAt) < 0) {
    return;
  }
  connParamsDirty = false;
  if (lowPower) {
    server->updateConnParams(handle, EasyBLEProtocol::IdleIntervalMin, EasyBLEProtocol::IdleIntervalMax,
                             EasyBLEProtocol::IdleLatency, EasyBLEProtocol::SupervisionTimeout);
  } else {
    server->updateConnParams(handle, EasyBLEProtocol::ActiveIntervalMin, EasyBLEProtocol::ActiveIntervalMax, 0,
                             EasyBLEProtocol::SupervisionTimeout);
  }
}

void discardSessionIo() {
  deviceToPhoneStream.flush();

  uint8_t discarded[64];
  while (phoneToDeviceStream.read(discarded, sizeof(discarded)) != 0) {
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
    pendingSince = millis();
    pendingHandle = connectionHandle;
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int) override {
    if (pendingHandle == connInfo.getConnHandle()) {
      pendingHandle = BLE_HS_CONN_HANDLE_NONE;
    }
    if (deviceToPhoneStream.getPeerHandle() == connInfo.getConnHandle()) {
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

  NimBLEService* service =
      server->createService(NimBLEUUID(EasyBLEProtocol::ServiceUUID));
  NimBLECharacteristic* deviceToPhone = service == nullptr
      ? nullptr
      : service->createCharacteristic(
            NimBLEUUID(EasyBLEProtocol::DeviceToPhoneUUID), NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* phoneToDevice = service == nullptr
      ? nullptr
      : service->createCharacteristic(
            NimBLEUUID(EasyBLEProtocol::PhoneToDeviceUUID),
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);

  if (deviceToPhone == nullptr || phoneToDevice == nullptr ||
      !deviceToPhoneStream.begin(deviceToPhone, txBufferSize, 0) ||
      !phoneToDeviceStream.begin(phoneToDevice, 0, rxBufferSize)) {
    deviceToPhoneStream.end();
    phoneToDeviceStream.end();
    NimBLEDevice::deinit(true);
    return false;
  }

  phoneToDeviceStream.setRxOverflowCallback(onRxOverflow);
  deviceToPhoneStream.setCallbacks(&streamCallbacks);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(EasyBLEProtocol::ServiceUUID);
  advertising->setName(deviceName);
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
  deviceToPhoneStream.end();
  phoneToDeviceStream.end();
  NimBLEDevice::deinit(true);
}

void EasyBLEBackend::poll() {
  // NimBLEStream can stop sending on larger writes and never retry. We picked a smaller chunk size to avoid this
  // Ask it to send whatever is still queued so a stalled write can finish.
  deviceToPhoneStream.drainTx();

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
    requestConnParams();
  }
  applyConnParams();

  const uint16_t pending = pendingHandle.load();
  if (pending == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  if (ready()) {
    pendingHandle = BLE_HS_CONN_HANDLE_NONE;
    return;
  }
  if (millis() - pendingSince.load() >= EasyBLEProtocol::SetupTimeoutMs) {
    pendingHandle = BLE_HS_CONN_HANDLE_NONE;
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server != nullptr) {
      server->disconnect(pending);
    }
  }
}

void EasyBLEBackend::disconnect() {
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t peerHandle = deviceToPhoneStream.getPeerHandle();
  if (server != nullptr && peerHandle != BLE_HS_CONN_HANDLE_NONE) {
    server->disconnect(peerHandle);
  }
}

void EasyBLEBackend::setLowPower(bool enabled) {
  if (lowPower == enabled) {
    return;
  }
  lowPower = enabled;
  requestConnParams();
}

bool EasyBLEBackend::ready() {
  return deviceToPhoneStream.ready() && phoneToDeviceStream.ready();
}

bool EasyBLEBackend::rxInvalid() {
  return rxOverflowed.load();
}

size_t EasyBLEBackend::write(const uint8_t* data, size_t length) {
  return deviceToPhoneStream.write(data, length);
}

size_t EasyBLEBackend::availableForWrite() {
  return deviceToPhoneStream.availableForWrite();
}

size_t EasyBLEBackend::read(uint8_t* buffer, size_t length) {
  return phoneToDeviceStream.read(buffer, length);
}
