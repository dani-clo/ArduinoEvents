#include <ArduinoEvents.h>

using namespace arduino_events;

enum AppState {
  STATE_IDLE,
  STATE_CONNECTING,
  STATE_READY,
  STATE_ERROR
};

struct StartConnectEvent {};
struct ConnectOkEvent {};
struct ConnectFailEvent {};
struct ResetEvent {};

AppState appState = STATE_IDLE;

void printState() {
  Serial.print("Current state: ");
  if (appState == STATE_IDLE) {
    Serial.println("IDLE");
  } else if (appState == STATE_CONNECTING) {
    Serial.println("CONNECTING");
  } else if (appState == STATE_READY) {
    Serial.println("READY");
  } else {
    Serial.println("ERROR");
  }
}

void scheduleConnectOk() {
  Events.post(ConnectOkEvent{});
}

void scheduleConnectFail() {
  Events.post(ConnectFailEvent{});
}

void scheduleReset() {
  Events.post(ResetEvent{});
}

void scheduleStartConnect() {
  Events.post(StartConnectEvent{});
}

void onStartConnect(const StartConnectEvent&) {
  if (appState != STATE_IDLE) {
    return;
  }

  appState = STATE_CONNECTING;
  Serial.println("Starting connection...");
  printState();

  Events.after(1000, scheduleConnectOk);
}

void onConnectOk(const ConnectOkEvent&) {
  if (appState != STATE_CONNECTING) {
    return;
  }

  appState = STATE_READY;
  Serial.println("Connection OK");
  printState();

  Events.after(2500, scheduleConnectFail);
}

void onConnectFail(const ConnectFailEvent&) {
  if (appState != STATE_READY && appState != STATE_CONNECTING) {
    return;
  }

  appState = STATE_ERROR;
  Serial.println("Connection failed");
  printState();

  Events.after(1500, scheduleReset);
}

void onReset(const ResetEvent&) {
  appState = STATE_IDLE;
  Serial.println("Reset done");
  printState();

  Events.after(1000, scheduleStartConnect);
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 24;
  cfg.workerQueueCapacity = 8;
  arduino_events::begin(cfg);

  Events.listen<StartConnectEvent>(onStartConnect);
  Events.listen<ConnectOkEvent>(onConnectOk);
  Events.listen<ConnectFailEvent>(onConnectFail);
  Events.listen<ResetEvent>(onReset);

  printState();
  Events.after(500, scheduleStartConnect);
}

void loop() {
  arduino_events::update();
}
