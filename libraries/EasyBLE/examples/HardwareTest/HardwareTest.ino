#include <EasyBLE.h>

#include <esp_heap_caps.h>
#include <string.h>

namespace {

constexpr char DeviceName[] = "EasyBLE-HWTest";
constexpr char Command[] = "@cmd:bidirectional-interleave";
constexpr char SuccessReport[] = "@report:bidirectional:ok";
constexpr char FailureReport[] = "@report:bidirectional:failed";
constexpr char StreamSuccessReport[] = "@report:stream:ok";
constexpr char StreamFailureReport[] = "@report:stream:failed";
constexpr size_t PhonePayloadSize = 8'192;
constexpr size_t StreamPayloadSize = 200'000;
constexpr size_t DevicePayloadSize = 65'536;
constexpr uint8_t PhoneSeed = 0xA7;
constexpr uint8_t StreamSeed = 0x3C;
constexpr uint8_t DeviceSeed = 0x51;

enum class SendStage : uint8_t {
  Idle,
  Payload,
  Report,
};

SendStage sendStage = SendStage::Idle;
bool phonePayloadReceived = false;
bool devicePayloadAccepted = false;
bool payloadValid = false;
bool reportSent = false;
bool streamReportPending = false;
bool streamReportOk = false;
size_t streamOffset = 0;
size_t streamExpected = 0;
uint8_t streamSeed = PhoneSeed;
bool streamValid = false;

uint8_t deterministicByte(size_t index, uint8_t seed) {
  return static_cast<uint8_t>(static_cast<uint32_t>(seed) +
                              static_cast<uint32_t>(index) * 31U +
                              static_cast<uint32_t>(index / 7U));
}

void maybeSendReport() {
  if (!phonePayloadReceived || !devicePayloadAccepted ||
      sendStage != SendStage::Idle || reportSent) {
    return;
  }

  const char* report = payloadValid ? SuccessReport : FailureReport;
  if (EasyBLE.sendText(report)) {
    reportSent = true;
    sendStage = SendStage::Report;
  }
}

void maybeSendStreamReport() {
  if (!streamReportPending || sendStage != SendStage::Idle) {
    return;
  }

  const char* report = streamReportOk ? StreamSuccessReport : StreamFailureReport;
  if (EasyBLE.sendText(report)) {
    streamReportPending = false;
    sendStage = SendStage::Report;
  }
}

void startDevicePayload() {
  uint8_t* payload = static_cast<uint8_t*>(heap_caps_malloc(
      DevicePayloadSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (payload == nullptr) {
    payload = static_cast<uint8_t*>(malloc(DevicePayloadSize));
  }
  if (payload == nullptr) {
    return;
  }

  for (size_t index = 0; index < DevicePayloadSize; ++index) {
    payload[index] = deterministicByte(index, DeviceSeed);
  }

  if (EasyBLE.send(EasyBLEMessageType::Text, payload, DevicePayloadSize)) {
    sendStage = SendStage::Payload;
  }
  free(payload);
}

void onReceive(const EasyBLEMessage& message) {
  if (message.type == EasyBLEMessageType::Text &&
      message.length == strlen(Command) &&
      memcmp(message.data, Command, message.length) == 0) {
    phonePayloadReceived = false;
    devicePayloadAccepted = false;
    payloadValid = false;
    reportSent = false;
    streamReportPending = false;
    sendStage = SendStage::Idle;
    startDevicePayload();
  }
}

bool onStream(const EasyBLEStreamEvent& event) {
  switch (event.status) {
    case EasyBLEStreamStatus::Begin:
      if (event.type != EasyBLEMessageType::Text) {
        return false;
      }
      if (event.totalLength == PhonePayloadSize) {
        streamSeed = PhoneSeed;
      } else if (event.totalLength == StreamPayloadSize) {
        streamSeed = StreamSeed;
      } else {
        return false;
      }
      streamOffset = 0;
      streamExpected = event.totalLength;
      streamValid = true;
      return true;
    case EasyBLEStreamStatus::Data:
      for (size_t index = 0; index < event.length; ++index) {
        if (event.data[index] != deterministicByte(streamOffset + index, streamSeed)) {
          streamValid = false;
          break;
        }
      }
      streamOffset += event.length;
      return true;
    case EasyBLEStreamStatus::End: {
      const bool ok = streamValid && streamOffset == streamExpected;
      if (streamExpected == PhonePayloadSize) {
        phonePayloadReceived = true;
        payloadValid = ok;
        maybeSendReport();
      } else if (streamExpected == StreamPayloadSize) {
        streamReportOk = ok;
        streamReportPending = true;
        maybeSendStreamReport();
      }
      return ok;
    }
    case EasyBLEStreamStatus::Aborted:
      streamOffset = 0;
      streamExpected = 0;
      streamValid = false;
      return true;
  }
  return false;
}

void onSendResult(bool accepted) {
  if (sendStage == SendStage::Payload) {
    devicePayloadAccepted = accepted;
    sendStage = SendStage::Idle;
    maybeSendReport();
  } else if (sendStage == SendStage::Report) {
    sendStage = SendStage::Idle;
  }
}

void resetTest() {
  sendStage = SendStage::Idle;
  phonePayloadReceived = false;
  devicePayloadAccepted = false;
  payloadValid = false;
  reportSent = false;
  streamReportPending = false;
  streamReportOk = false;
  streamOffset = 0;
  streamExpected = 0;
  streamValid = false;
}

}  // namespace

void setup() {
  EasyBLE.onReceive(onReceive);
  EasyBLE.onStream(onStream);
  EasyBLE.onSendResult(onSendResult);
  EasyBLE.onDisconnect(resetTest);
  EasyBLE.begin(DeviceName, EasyBLEMinimumMaxMessage);
}

void loop() {
  EasyBLE.update();
  maybeSendReport();
  maybeSendStreamReport();
}
