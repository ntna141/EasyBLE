# EasyBLE

Logical-message transport over BLE for Arduino boards. EasyBLE automatically frames, chunks, acknowledges and reassembles messages while selecting ArduinoBLE or NimBLE at compile time.

## Backends

The backend is selected at compile time:

- ESP32 boards use [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- SAMD, nRF52, Mbed, Renesas, RP2040 and megaAVR boards use [ArduinoBLE](https://github.com/arduino-libraries/ArduinoBLE)

The Library Manager installs both backend libraries alongside EasyBLE, but only the one matching your board is ever compiled; the other adds nothing to the binary. To override the automatic selection, define `EASYBLE_USE_NIMBLE` or `EASYBLE_USE_ARDUINOBLE` before including `EasyBLE.h`.

## Usage

```cpp
#include <EasyBLE.h>

void onData(const uint8_t* data, size_t len) {
  // data is one complete logical message.
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

`update()` must be called from `loop()`. It polls the selected BLE backend and advances pending acknowledgements and outgoing message fragments. All callbacks fire during `update()`, on the Arduino loop task.

## GATT profile

| UUID | Role |
| --- | --- |
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Service |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | RX, phone writes packets here (write or write without response) |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | TX, device sends packets as notifications |

These currently use the Nordic UART Service UUIDs, but the characteristic values contain EasyBLE protocol frames rather than raw UART data. The peer must implement the EasyBLE framing protocol.

## Connection semantics

The device counts as connected only once the central has subscribed to the TX characteristic. `onConnect` fires at that point, `isConnected()` returns true, and `send()` starts working. On the phone side, subscribe to TX immediately after connecting.

## Message framing

One `send()` call queues one logical message. EasyBLE copies the message into its transmit buffer, splits it into frames no larger than `EASYBLE_MAX_PACKET`, and sends frames as the peer's receive window allows. `onData` fires only after the full incoming message has been validated and reassembled.

Frames use this compact layout:

| Field | Presence |
| --- | --- |
| Flags (`START`, `CONTINUE`, `END`, `ACK`) | Always |
| Acknowledged sequence | When `ACK` is set |
| Frame sequence | Always, including stand-alone acknowledgements |
| Total message length, 16-bit little-endian | When `START` is set |
| Payload | Message data frames |

The flag values and field order follow Matter BTP framing:

```text
START = 0x01    CONTINUE = 0x02    END = 0x04    ACK = 0x08

START:    flags | optional acknowledged sequence | sequence | uint16 length | payload
CONTINUE: flags | optional acknowledged sequence | sequence | payload
ACK:      ACK flag | acknowledged sequence | sequence
```

Sequence numbers are unsigned bytes and wrap from 255 to 0. Acknowledgements are cumulative: acknowledging sequence `N` acknowledges every outstanding frame through `N`. An acknowledgement may be carried by itself or piggybacked on an outgoing data frame.

Both peers use a fixed six-frame receive window. Sending a data frame consumes one remote window slot. ACK-only frames advance the sequence number but do not consume a window slot and are not themselves acknowledged. A peer normally reserves the last slot for a data frame carrying an acknowledgement, preventing both directions from filling their windows and deadlocking. Receipt of a cumulative acknowledgement reopens the corresponding slots.

A peer acknowledges immediately when only one local window slot remains. Otherwise it waits up to 2.5 seconds, allowing the acknowledgement to be piggybacked on outbound data first. After sending the first outstanding frame, the sender waits up to 15 seconds for a cumulative acknowledgement. A partial acknowledgement restarts that deadline; acknowledging the newest outstanding frame stops it. Expiration disconnects the BLE session rather than retransmitting frames.

Any protocol violation is fatal: a malformed frame, an unexpected sequence number, a receive-window overrun or an invalid acknowledgement disconnects the BLE session immediately. The protocol resets when the backend reports the disconnection, so a reconnecting peer starts from a clean state.

These framing, window and timeout rules are BTP-style, but EasyBLE does not implement the Matter capabilities handshake or claim Matter interoperability.

`send()` returning true means the message was accepted and copied for asynchronous transmission. It does not mean the peer received it. Only one outgoing message may be active at a time. If an acknowledgement does not arrive before the transport deadline, EasyBLE disconnects and resets when the backend reports that disconnection.

## Limits

`EASYBLE_MAX_PACKET` controls the largest raw BLE frame:

- NimBLE backend: 182 bytes by default, safe for the 185-byte ATT MTU iOS negotiates
- ArduinoBLE backend: 20 bytes by default, safe for the minimum 23-byte ATT MTU

Override with a build flag, e.g. `-DEASYBLE_MAX_PACKET=100`. Keep it at or below the negotiated MTU minus 3, otherwise notifications are silently truncated by the stack.

`EASYBLE_MAX_MESSAGE` controls both transmit and receive message storage:

- NimBLE backend: 4096 bytes by default
- ArduinoBLE backend: 1024 bytes by default

Override it with a build flag when the board has different memory constraints. Outgoing messages larger than this limit are rejected by `send()`; an incoming message declaring a larger total length is treated as a protocol violation and disconnects the session.

## Buffering

On the NimBLE backend, incoming frames are queued and processed on the next `update()`. The queue holds `EASYBLE_QUEUE_DEPTH` frames (16 by default). A new frame is ignored when the queue is full; the next frame then arrives with an unexpected sequence number and the session disconnects. Call `update()` frequently.

## Limitations

- Single connection at a time
- One outgoing logical message at a time
- Incoming messages are currently fully buffered; streaming media delivery is not implemented yet
- The peer must implement the same framing and acknowledgement protocol
- On ArduinoBLE boards, cycling `end()` / `begin()` is not well tested upstream; prefer calling `begin()` once
