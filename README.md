# Library to make building companion ESP32 devices easier

- Pair iOS apps with ESP32 companion devices over BLE
- Send and receive text or image payloads (that your device can hold in memory)
- Stream large payloads chunk by chunk to storage with `onStream` instead of buffering them
- Simple callbacks for connect, disconnect, and results
- Automatic reconnect after the accessory is paired