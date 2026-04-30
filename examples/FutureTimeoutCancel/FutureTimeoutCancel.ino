#include <ArduinoEvents.h>

using namespace arduino_events;

Deferred<int> successDeferred;
Deferred<int> timeoutDeferred;
Deferred<int> cancelDeferred;

Future<int> successFuture;
Future<int> timeoutFuture;
Future<int> cancelFuture;

void completeSuccessRequest() {
  if (!successDeferred.isCancelled()) {
    successDeferred.resolve(42);
  }
}

void completeLateRequest() {
  if (!timeoutDeferred.isCancelled()) {
    timeoutDeferred.resolve(7);
  }
}

void completeCancellableRequest() {
  if (!cancelDeferred.isCancelled()) {
    cancelDeferred.resolve(99);
  }
}

void startSuccess(Deferred<int> d) {
  successDeferred = d;
  Async().after(300, completeSuccessRequest);
}

void startTimeout(Deferred<int> d) {
  timeoutDeferred = d;
  Async().after(1500, completeLateRequest);
}

void startCancelable(Deferred<int> d) {
  cancelDeferred = d;
  Async().after(1000, completeCancellableRequest);
}

void cancelRequestNow() {
  bool ok = cancelFuture.cancel();
  Serial.print("Cancel request result: ");
  Serial.println(ok ? "cancelled" : "not cancelled");
}

void onSuccessValue(const int& value) {
  Serial.print("Success value = ");
  Serial.println(value);
}

void onTimeoutValue(const int& value) {
  Serial.print("Timeout future value (unexpected) = ");
  Serial.println(value);
}

void onCancelValue(const int& value) {
  Serial.print("Cancel future value (unexpected) = ");
  Serial.println(value);
}

void onFutureError(const Error& err) {
  Serial.print("Error code = ");
  Serial.println((int)err.code);
  Serial.print("Message = ");
  Serial.println(err.message);
}

void onDone() {
  Serial.println("Future finished");
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 24;
  cfg.workerQueueCapacity = 8;
  arduino_events::begin(cfg);

  successFuture = Async().defer<int>(startSuccess);
  successFuture.then(onSuccessValue).catchError(onFutureError).finally(onDone);

  timeoutFuture = Async().defer<int>(startTimeout);
  timeoutFuture.withTimeout(500).then(onTimeoutValue).catchError(onFutureError).finally(onDone);

  cancelFuture = Async().defer<int>(startCancelable);
  cancelFuture.then(onCancelValue).catchError(onFutureError).finally(onDone);
  Async().after(200, cancelRequestNow);
}

void loop() {
  arduino_events::update();
}
