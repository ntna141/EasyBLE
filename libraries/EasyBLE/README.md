# EasyBLE

Minimal BLE transport for Arduino boards. Exposes a Nordic UART Service (NUS) compatible GATT profile so a phone can send binary packets to the device and receive packets back, with a single callback-based API.

## Backends

The backend is selected at compile time:

- ESP32 boards use [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- SAMD, nRF52, Mbed, Renesas, RP2040 and megaAVR boards use [ArduinoBLE](https://github.com/arduino-libraries/ArduinoBLE)

The Library Manager installs both backend libraries alongside EasyBLE, but only the one matching your board is ever compiled; the other adds nothing to the binary. To override the automatic selection, define `EASYBLE_USE_NIMBLE` or `EASYBLE_USE_ARDUINOBLE` before including `EasyBLE.h`.

## Usage

```cpp
#include <EasyBLE.h>

void onData(const uint8_t* data, size_t len) {
  EasyBLE.send(data, len);
}

void setup() {
  EasyBLE.onData(onData);
  EasyBLE.begin("EasyBLE");
}

void loop() {
  EasyBLE.update();
}
```

`update()` must be called from `loop()`. All callbacks fire during `update()`, on the Arduino loop task.

## GATT profile

| UUID | Role |
| --- | --- |
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Service |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | RX, phone writes packets here (write or write without response) |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | TX, device sends packets as notifications |

These are the Nordic UART Service UUIDs, so generic BLE terminal apps (nRF Toolbox, Serial Bluetooth Terminal) work out of the box.

## Connection semantics

The device counts as connected only once the central has subscribed to the TX characteristic. `onConnect` fires at that point, `isConnected()` returns true, and `send()` starts working. On the phone side, subscribe to TX immediately after connecting.

## Packet size

One `send()` call is one notification and one write from the phone is one `onData` callback. There is no fragmentation or reassembly; packets larger than `EASYBLE_MAX_PACKET` are rejected.

- NimBLE backend: 182 bytes by default, safe for the 185-byte ATT MTU iOS negotiates
- ArduinoBLE backend: 20 bytes by default, safe for the minimum 23-byte ATT MTU

Override with a build flag, e.g. `-DEASYBLE_MAX_PACKET=100`. Keep it at or below the negotiated MTU minus 3, otherwise notifications are silently truncated by the stack.

## Buffering

On the NimBLE backend, incoming packets are queued and delivered on the next `update()`. The queue holds `EASYBLE_QUEUE_DEPTH` packets (16 by default); when full, the oldest packet is dropped to make room for the newest. Call `update()` frequently to avoid drops.

## Limitations

- Single connection at a time
- `send()` returning true means the notification was handed to the stack, not that the phone processed it
- On ArduinoBLE boards, cycling `end()` / `begin()` is not well tested upstream; prefer calling `begin()` once
