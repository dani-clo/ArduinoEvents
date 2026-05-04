#include <ArduinoEvents.h>

using namespace arduino_events;

// Simulates a slow operation (e.g. sensor read, config save, network call).
// In a real sketch this would do actual work; here it just blocks for a bit.
void slowOperation(Deferred<int> deferred) {
  if (deferred.isCancelled()) {
    return;
  }

  // Simulate work with a blocking delay.
  delay(2000);

  // Resolve with a result value when done.
  deferred.resolve(42);
}

// Called while the slow operation is running in the background.
void doOtherStuff() {
  Serial.println("Doing other stuff while waiting...");
}

uint32_t progressTimer = 0;

void onOperationDone(const int& result) {
  Serial.print("Operation done! Result: ");
  Serial.println(result);

  // Stop the progress ticker now that we are done.
  Events.cancelTimer(progressTimer);
}

void onOperationError(const Error& err) {
  Serial.print("Operation failed: ");
  Serial.println(err.message);
}

void setup() {
  Serial.begin(115200);

  arduino_events::begin();

  Serial.println("Starting slow operation...");

  Events.runAsync<int>(slowOperation)
      .onDone(onOperationDone)
      .onError(onOperationError);

  // Print progress every 500 ms while waiting for the result.
  progressTimer = Events.every(500, doOtherStuff);
}

void loop() {
  arduino_events::update();
}
