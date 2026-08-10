#include <EasyBLE.h>

void onData(const uint8_t* data, size_t len) {
  EasyBLE.send(data, len);
}

void setup() {
  EasyBLE.onData(onData);
  EasyBLE.onConnect([]() {});
  EasyBLE.onDisconnect([]() {});
  EasyBLE.begin("EasyBLE");
}

void loop() {
  EasyBLE.update();
}
