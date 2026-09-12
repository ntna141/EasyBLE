# Library to make building companion ESP32 devices easier

- Pair iOS apps with ESP32 companion devices over BLE
- Send and receive text or image payloads (that your device can hold in memory)
- Stream large payloads chunk by chunk to storage with `onStream` instead of buffering them
- Stream live bytes to the phone with `channel()`, consumed on iOS as an `AsyncStream`; the `AudioStream` example encodes the XIAO ESP32S3 Sense microphone as IMA ADPCM and the `EasyBLEAudio` CLI decodes it to WAV, live playback, or live [Soniox](https://soniox.com/) transcription
- Simple callbacks for connect, disconnect, and results
- Automatic reconnect after the accessory is paired

## Install

### iOS (Xcode)

File → Add Package Dependencies → `https://github.com/ntna141/EasyBLE`

Use version `0.4.0` or later. If Xcode cannot resolve a tag, set the dependency rule to Branch → `main`.

Add these keys to the host app `Info.plist`:

```xml
<key>NSAccessorySetupKitSupports</key>
<array>
  <string>Bluetooth</string>
</array>
<key>NSAccessorySetupBluetoothServices</key>
<array>
  <string>6E400001-B5A3-F393-E0A9-E50E24DCCA9E</string>
</array>
<key>UIBackgroundModes</key>
<array>
  <string>bluetooth-central</string>
</array>
```

The Swift `EasyBLE` type requires iOS 18.

### Arduino

Copy `libraries/EasyBLE` into your Arduino libraries folder, or install via Library Manager once it is submitted. ESP32 boards only; depends on NimBLE-Arduino 2.4.0 or later.

## Pair and stay connected

Register callbacks, then advertise. Call `update()` from `loop()`.

```cpp
#include <EasyBLE.h>

void onConnect() {
  Serial.println("phone connected");
}

void onDisconnect() {
  Serial.println("phone disconnected");
}

void setup() {
  EasyBLE.onConnect(onConnect);
  EasyBLE.onDisconnect(onDisconnect);
  EasyBLE.begin("EasyBLE-Demo");
}

void loop() {
  EasyBLE.update();
}
```

On iOS, create one `EasyBLE` instance, pair once, then keep that instance alive. After pairing, the session reconnects automatically.

```swift
import EasyBLE
import UIKit

let ble = EasyBLE()

ble.onConnect { print("device connected") }
ble.onDisconnect { print("device disconnected") }

try await ble.pair(name: "EasyBLE-Demo", image: UIImage(named: "Accessory")!)
```

`pair` presents the system accessory picker. `unpair()` removes the accessory from AccessorySetupKit.

```swift
try await ble.unpair()
```

`ble.isConnected` matches `EasyBLE.isConnected()` on the device.

## Send a message to the device

The phone sends a complete payload. The device receives it in `onReceive` if it fits in the receive buffer.

```swift
ble.onSendResult { ok in
    print(ok ? "device accepted" : "device rejected")
}

ble.sendText("hello")
ble.sendImage(jpegData)
ble.send(.text, data: Data("hello".utf8))
```

```cpp
void onReceive(const EasyBLEMessage& message) {
  if (message.type == EasyBLEMessageType::Text) {
    Serial.println(reinterpret_cast<const char*>(message.data));
  }
}

void setup() {
  EasyBLE.onReceive(onReceive);
  EasyBLE.begin("EasyBLE-Demo");
}
```

`message.data` is only valid for the duration of the callback. Text payloads are NUL-terminated.

`begin` defaults to a 4096-byte receive cap (`EasyBLEDefaultMaxMessage`). Larger complete messages are rejected unless you raise that cap or handle them with `onStream`.

## Send a message to the phone

`send` copies the bytes immediately, so the caller buffer can be reused when it returns. Only one send can be in flight.

```cpp
void onSendResult(bool ok) {
  Serial.println(ok ? "phone accepted" : "phone rejected");
}

void onConnect() {
  EasyBLE.sendText("ready");
}

void setup() {
  EasyBLE.onConnect(onConnect);
  EasyBLE.onSendResult(onSendResult);
  EasyBLE.begin("EasyBLE-Demo");
}

void loop() {
  EasyBLE.update();
}
```

```swift
ble.onReceive { message in
    if message.type == .text, let text = String(data: message.data, encoding: .utf8) {
        print(text)
    }
}
```

`send` / `sendText` return `false` when the link is down, a send is already in progress, or the copy cannot be allocated. `EasyBLE.isSending()` / `ble.isSending` stay true until `onSendResult`.

## Stream a large payload onto the device

If `onStream` is set and `Begin` returns `true`, EasyBLE delivers chunks instead of buffering the whole message. `onReceive` is not called for that message.

```cpp
bool onStream(const EasyBLEStreamEvent& event) {
  switch (event.status) {
    case EasyBLEStreamStatus::Begin:
      return event.type == EasyBLEMessageType::Image;
    case EasyBLEStreamStatus::Data:
      // event.data / event.length is this chunk only
      return true;
    case EasyBLEStreamStatus::End:
      return true;
    case EasyBLEStreamStatus::Aborted:
      return true;
  }
  return false;
}

void setup() {
  EasyBLE.onStream(onStream);
  EasyBLE.begin("EasyBLE-Demo", EasyBLEMinimumMaxMessage);
}
```

Return `false` from `Begin` to fall back to a buffered `onReceive`, or from `Data` / `End` to reject the transfer. The phone still uses `send` / `sendImage`; the device chooses whether to stream.

## Stream live bytes to the phone

The channel is a lossy device-to-phone pipe. The descriptor is application-defined (at most 240 bytes) and delivered to the phone before any bytes.

The device can offer a channel:

```cpp
const uint8_t Descriptor[] = {1, 16, 0};  // your format

void onEnabled(bool enabled) {
  if (!enabled) {
    // phone stopped the channel or the link dropped
  }
}

void offer() {
  EasyBLE.channel().open(Descriptor, sizeof(Descriptor));
}

void loop() {
  EasyBLE.update();

  EasyBLEChannel& channel = EasyBLE.channel();
  if (!channel.isEnabled()) {
    return;
  }

  uint8_t block[128];
  size_t n = readSensor(block, sizeof(block));
  if (n > 0 && n <= channel.availableForWrite()) {
    channel.write(block, n);
  }
}
```

Or the phone can ask the device to offer one:

```swift
ble.requestChannel()
```

```cpp
void onRequested() {
  EasyBLE.channel().open(Descriptor, sizeof(Descriptor));
}

void setup() {
  EasyBLE.channel().onRequested(onRequested);
  EasyBLE.channel().onEnabled(onEnabled);
  EasyBLE.begin("EasyBLE-Demo");
}
```

`open` returns `false` when the link is down, a channel is already open, or the ring cannot be allocated. `onEnabled(true)` fires when the phone accepts; `onEnabled(false)` only fires after that, when the phone closes the channel or the link drops. A declined or unanswered offer (15 s) closes the channel silently, so check `isOpen()` if you need to retry.

`write` is all-or-nothing: it returns `length` when the whole block fits in the ring, or `0` and adds `length` to `droppedBytes()` when it does not. The phone sees each run of dropped bytes as one `.gap`. `close()` discards anything still in the ring and sends the end marker. Call `open`, `write`, and `close` from the same task as `update()`; the channel is not thread-safe.

On iOS the offer arrives as `EasyBLEIncomingChannel`. Accept it, then consume `bytes` or `events`.

```swift
ble.onChannelOpen { channel in
    guard channel.descriptor == Data([1, 16, 0]) else {
        channel.close()
        return
    }
    channel.accept()

    Task {
        for await event in channel.events {
            switch event {
            case .data(let data):
                handle(data)
            case .gap(let dropped):
                print("dropped \(dropped) bytes")
            case .ended:
                return
            }
        }
    }
}

ble.requestChannel()
```

`channel.bytes` yields only `.data` payloads. `close()` declines an offer or tells the device to stop an accepted channel. Each `events` / `bytes` stream buffers at most `EasyBLEIncomingChannel.bufferedEventLimit` (256) events and drops the oldest when the consumer falls behind.

See `libraries/EasyBLE/examples/AudioStream` for a microphone stream and `EasyBLEAudio` for the Mac-side decoder.

## Arduino API

```cpp
EasyBLE.begin(name);
EasyBLE.begin(name, maxMessageSize, startupBufferSize);
EasyBLE.end();
EasyBLE.update();

EasyBLE.onReceive(handler);          // void(const EasyBLEMessage&)
EasyBLE.onStream(handler);           // bool(const EasyBLEStreamEvent&)
EasyBLE.onConnect(handler);          // void()
EasyBLE.onDisconnect(handler);       // void()
EasyBLE.onSendResult(handler);       // void(bool)

EasyBLE.send(type, data, length);
EasyBLE.sendText(text);
EasyBLE.isSending();
EasyBLE.isConnected();

EasyBLEChannel& ch = EasyBLE.channel();
ch.onEnabled(handler);               // void(bool)
ch.onRequested(handler);             // void()
ch.open(descriptor, length);         // optional ringSize, default 12 KB
ch.write(data, length);
ch.availableForWrite();
ch.close();
ch.isOpen();
ch.isEnabled();
ch.droppedBytes();
```

```cpp
enum class EasyBLEMessageType : uint8_t { Text = 0x01, Image = 0x02 };

struct EasyBLEMessage {
  EasyBLEMessageType type;
  const uint8_t* data;  // valid only during onReceive
  size_t length;
};

enum class EasyBLEStreamStatus : uint8_t { Begin, Data, End, Aborted };

struct EasyBLEStreamEvent {
  EasyBLEStreamStatus status;
  EasyBLEMessageType type;
  size_t totalLength;
  const uint8_t* data;
  size_t length;
};
```

## Swift API

```swift
let ble = EasyBLE()

try await ble.pair(name:image:)
try await ble.unpair()

ble.sendText("hello")
ble.sendImage(data)
ble.send(.text, data: data)
ble.isConnected
ble.isSending

ble.onReceive { (EasyBLEMessage) in }
ble.onConnect { }
ble.onDisconnect { }
ble.onSendResult { (Bool) in }
ble.onChannelOpen { (EasyBLEIncomingChannel) in }

ble.requestChannel()
```

```swift
public enum EasyBLEMessageType: UInt8, Sendable {
    case text = 0x01
    case image = 0x02
}

public struct EasyBLEMessage: Sendable {
    public var type: EasyBLEMessageType
    public var data: Data
}

public enum EasyBLEChannelEvent: Sendable {
    case data(Data)
    case gap(droppedBytes: Int)
    case ended
}

public final class EasyBLEIncomingChannel {
    public static let bufferedEventLimit: Int
    public let descriptor: Data
    public private(set) var isAccepted: Bool
    public private(set) var isEnded: Bool
    public var events: AsyncStream<EasyBLEChannelEvent> { get }
    public var bytes: AsyncStream<Data> { get }
    public func accept()
    public func close()
}
```
