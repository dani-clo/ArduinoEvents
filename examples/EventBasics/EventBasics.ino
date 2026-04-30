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
  Async().emit(ButtonPressed{buttonCounter});
}

void unsubscribeB() {
  bool removed = Async().off(subB);
  Serial.print("Unsubscribe B: ");
  Serial.println(removed ? "ok" : "failed");
}

void emitBootAgain() {
  Async().emit(BootDone{true});
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 16;
  cfg.workerQueueCapacity = 8;
  arduino_events::begin(cfg);

  subA = Async().on<ButtonPressed>(onButtonA);
  subB = Async().on<ButtonPressed>(onButtonB);
  subOnce = Async().once<BootDone>(onBootOnce);

  Async().emit(BootDone{true});
  Async().emit(BootDone{true});

  Async().every(1000, emitButtonEvent);
  Async().after(4500, unsubscribeB);
  Async().after(5500, emitBootAgain);
}

void loop() {
  arduino_events::update();
}
