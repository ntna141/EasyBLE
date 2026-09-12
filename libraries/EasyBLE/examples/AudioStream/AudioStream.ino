#include <ESP_I2S.h>
#include <EasyBLE.h>

namespace {

constexpr char DeviceName[] = "EasyBLE-Audio";
constexpr int8_t PdmClockPin = 42;
constexpr int8_t PdmDataPin = 41;
constexpr int ButtonPin = 0;
constexpr uint32_t SampleRate = 16000;
constexpr uint32_t ReadTimeoutMs = 100;
constexpr int32_t MicGain = 8;

constexpr uint8_t CodecImaAdpcm = 1;
constexpr size_t BlockSamples = 256;
constexpr size_t BlockHeaderBytes = 4;
constexpr size_t BlockBytes = BlockHeaderBytes + BlockSamples / 2;

const uint8_t Descriptor[] = {
    CodecImaAdpcm,
    1,
    static_cast<uint8_t>(SampleRate),
    static_cast<uint8_t>(SampleRate >> 8),
    static_cast<uint8_t>(SampleRate >> 16),
    static_cast<uint8_t>(SampleRate >> 24),
    static_cast<uint8_t>(BlockSamples),
    static_cast<uint8_t>(BlockSamples >> 8),
};

const int8_t AdpcmIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                    -1, -1, -1, -1, 2, 4, 6, 8};

const int16_t AdpcmStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

I2SClass i2s;
int16_t pcm[BlockSamples];
uint8_t block[BlockBytes];
int32_t predictor = 0;
int8_t stepIndex = 0;
int32_t dcOffset = 0;
bool capturing = false;
bool buttonWasDown = false;

uint8_t encodeSample(int16_t sample) {
  int32_t step = AdpcmStepTable[stepIndex];
  int32_t diff = sample - predictor;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int32_t delta = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    delta += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 2;
    diff -= step;
    delta += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 1;
    delta += step;
  }
  predictor += (code & 8) ? -delta : delta;
  if (predictor > 32767) {
    predictor = 32767;
  } else if (predictor < -32768) {
    predictor = -32768;
  }
  stepIndex += AdpcmIndexTable[code];
  if (stepIndex < 0) {
    stepIndex = 0;
  } else if (stepIndex > 88) {
    stepIndex = 88;
  }
  return code;
}

void conditionSamples() {
  for (size_t i = 0; i < BlockSamples; ++i) {
    const int32_t x = pcm[i];
    dcOffset += (x - dcOffset) >> 8;
    int32_t y = (x - dcOffset) * MicGain;
    if (y > 32767) {
      y = 32767;
    } else if (y < -32768) {
      y = -32768;
    }
    pcm[i] = static_cast<int16_t>(y);
  }
}

void encodeBlock() {
  block[0] = static_cast<uint8_t>(predictor);
  block[1] = static_cast<uint8_t>(predictor >> 8);
  block[2] = static_cast<uint8_t>(stepIndex);
  block[3] = 0;
  for (size_t i = 0; i < BlockSamples; i += 2) {
    const uint8_t low = encodeSample(pcm[i]);
    const uint8_t high = encodeSample(pcm[i + 1]);
    block[BlockHeaderBytes + i / 2] = low | (high << 4);
  }
}

bool startMic() {
  i2s.setPinsPdmRx(PdmClockPin, PdmDataPin);
  if (!i2s.begin(I2S_MODE_PDM_RX, SampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_MONO)) {
    return false;
  }
  i2s.setTimeout(ReadTimeoutMs);
  predictor = 0;
  stepIndex = 0;
  dcOffset = 0;
  capturing = true;
  return true;
}

void stopMic() {
  if (!capturing) {
    return;
  }
  capturing = false;
  i2s.end();
}

void offerAudio() {
  if (EasyBLE.channel().open(Descriptor, sizeof(Descriptor))) {
    Serial.println("audio offered, waiting for phone");
  }
}

void stopAudio() {
  stopMic();
  EasyBLE.channel().close();
  Serial.println("audio idle");
}

void onChannelEnabled(bool enabled) {
  if (!enabled) {
    stopMic();
    Serial.println("audio idle");
    return;
  }
  if (startMic()) {
    Serial.println("audio streaming");
  } else {
    Serial.println("microphone failed to start");
    EasyBLE.channel().close();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(ButtonPin, INPUT_PULLUP);
  EasyBLE.channel().onRequested(offerAudio);
  EasyBLE.channel().onEnabled(onChannelEnabled);
  EasyBLE.begin(DeviceName);
}

void loop() {
  EasyBLE.update();

  const bool buttonDown = digitalRead(ButtonPin) == LOW;
  if (buttonDown && !buttonWasDown) {
    if (EasyBLE.channel().isOpen()) {
      stopAudio();
    } else {
      offerAudio();
    }
  }
  buttonWasDown = buttonDown;

  if (!capturing) {
    return;
  }
  const size_t got = i2s.readBytes(reinterpret_cast<char*>(pcm), sizeof(pcm));
  if (got != sizeof(pcm)) {
    return;
  }
  conditionSamples();
  encodeBlock();
  EasyBLE.channel().write(block, BlockBytes);
}
