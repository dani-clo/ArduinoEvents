#include <ArduinoEvents.h>

using namespace arduino_events;

struct ButtonPressed {
  int count;
};

struct BootDone {
  bool ok;
};

Subscription subA;
Subscription subB;
Subscription subOnce;

int buttonCounter = 0;

void onButtonA(const ButtonPressed& ev) {
  Serial.print("A: button count = ");
  Serial.println(ev.count);
}

void onButtonB(const ButtonPressed& ev) {
  Serial.print("B: button count = ");
  Serial.println(ev.count);
}

void onBootOnce(const BootDone& ev) {
  Serial.print("Boot event (once), ok = ");
  Serial.println(ev.ok ? "true" : "false");
}

void emitButtonEvent() {
  buttonCounter++;
  Events.post(ButtonPressed{buttonCounter});
}

void unsubscribeB() {
  bool removed = Events.unlisten(subB);
  Serial.print("Unsubscribe B: ");
  Serial.println(removed ? "ok" : "failed");
}

void emitBootAgain() {
  Events.post(BootDone{true});
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 16;
  cfg.workerQueueCapacity = 8;
  arduino_events::begin(cfg);

  subA = Events.listen<ButtonPressed>(onButtonA);
  subB = Events.listen<ButtonPressed>(onButtonB);
  subOnce = Events.listenOnce<BootDone>(onBootOnce);

  Events.post(BootDone{true});
  Events.post(BootDone{true});

  Events.every(1000, emitButtonEvent);
  Events.after(4500, unsubscribeB);
  Events.after(5500, emitBootAgain);
}

void loop() {
  arduino_events::update();
}
