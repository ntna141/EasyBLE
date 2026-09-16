#pragma once

#include <stddef.h>
#include <stdint.h>

namespace EasyBLEProtocol {

constexpr char ServiceUUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char PhoneToDeviceUUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char DeviceToPhoneUUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr uint8_t OpcodeResult = 0x02;
constexpr uint8_t OpcodeBegin = 0x03;
constexpr uint8_t OpcodeContinue = 0x04;
constexpr uint8_t OpcodeOffer = 0x05;
constexpr uint8_t OpcodeAck = 0x06;
constexpr uint8_t OpcodeData = 0x07;
constexpr uint8_t OpcodeControl = 0x08;

constexpr uint8_t DataFlagHeader = 0x01;
constexpr uint8_t DataFlagGap = 0x02;
constexpr uint8_t DataFlagEnd = 0x04;

constexpr size_t BeginHeaderSize = 8;
constexpr size_t ContinueHeaderSize = 3;
constexpr size_t ResultRecordSize = 2;
constexpr size_t AckRecordSize = 1;
constexpr size_t ControlRecordsSize = 2 * (ResultRecordSize + AckRecordSize);
constexpr size_t DataHeaderSize = 4;
constexpr size_t GapPrefixSize = 2;
constexpr size_t DataMaxPayload = 240;
constexpr uint16_t ChunkPayloadSize = 3072;
constexpr uint32_t ResultTimeoutMs = 15000;
constexpr uint32_t SetupTimeoutMs = 10000;
constexpr uint16_t ActiveIntervalMin = 12;
constexpr uint16_t ActiveIntervalMax = 24;
constexpr uint16_t IdleIntervalMin = 24;
constexpr uint16_t IdleIntervalMax = 48;
constexpr uint16_t IdleLatency = 9;
constexpr uint16_t SupervisionTimeout = 400;
constexpr uint32_t ConnParamsDelayMs = 2000;

inline uint32_t readUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

inline uint16_t readUint16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
      (static_cast<uint16_t>(data[1]) << 8);
}

inline void writeUint32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

inline void writeUint16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

}  // namespace EasyBLEProtocol
