#include <ArduinoEvents.h>

using namespace arduino_events;

uint32_t heartbeatTimerId = 0;
uint32_t oneShotTimerId = 0;
int heartbeatCount = 0;

void onOneShot() {
  Serial.println("One-shot timer fired");
}

void onHeartbeat() {
  heartbeatCount++;
  Serial.print("Heartbeat #");
  Serial.println(heartbeatCount);

  if (heartbeatCount >= 5) {
    bool ok = Async().cancelTimer(heartbeatTimerId);
    Serial.print("Heartbeat timer cancelled: ");
    Serial.println(ok ? "yes" : "no");
  }
}

void cancelOneShotBeforeFire() {
  bool ok = Async().cancelTimer(oneShotTimerId);
  Serial.print("One-shot cancelled before firing: ");
  Serial.println(ok ? "yes" : "no");
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 16;
  cfg.workerQueueCapacity = 8;
  arduino_events::begin(cfg);

  oneShotTimerId = Async().after(3000, onOneShot);
  heartbeatTimerId = Async().every(1000, onHeartbeat);

  Async().after(1500, cancelOneShotBeforeFire);
}

void loop() {
  arduino_events::update();
}
