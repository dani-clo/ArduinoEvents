#include <ArduinoEvents.h>
#include <ZephyrClient.h>
#include <WiFi.h>

#include "arduino_secrets.h"
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = SECRET_SSID;  // your network SSID (name)
char pass[] = SECRET_PASS;  // your network password (use for WPA, or use as key for WEP)

using namespace arduino_events;

struct HttpLikeResponse {
  int status = -1;
  String body;
};

struct WifiConnected {
  String ssid;
};

struct PendingHttpRequest {
  String url;
  uint32_t timeoutMs;
  Deferred<HttpLikeResponse> deferred;
  bool active;
};

Subscription wifiSub;
Future<HttpLikeResponse> pending;
PendingHttpRequest requestCtx;
uint32_t wifiPollTimerId = 0;
bool wifiConnectedEventSent = false;

void onTelemetrySuccess(const HttpLikeResponse& res) {
  Serial.print("HTTP status: ");
  Serial.println(res.status);
  Serial.print("Body: ");
  Serial.println(res.body);
}

void onTelemetryError(const Error& err) {
  Serial.print("HTTP error code: ");
  Serial.println((int)err.code);
  Serial.print("Message: ");
  Serial.println(err.message);
}

void onRequestCompleted() {
  Serial.println("Request completed");
}

void onHealthSuccess(const HttpLikeResponse& res) {
  Serial.print("Health status: ");
  Serial.println(res.status);
}

void onHealthError(const Error& err) {
  Serial.print("Health check failed: ");
  Serial.println((int)err.code);
}

void completeHttpRequest() {
  if (!requestCtx.active) {
    return;
  }

  requestCtx.active = false;

  if (requestCtx.deferred.isCancelled()) {
    return;
  }

  if (requestCtx.url.length() == 0) {
    Error err;
    err.code = ErrorCode::InvalidState;
    err.message = "empty URL";
    requestCtx.deferred.reject(err);
    return;
  }

  if (requestCtx.timeoutMs < 200) {
    Error err;
    err.code = ErrorCode::Timeout;
    err.message = "request timeout";
    requestCtx.deferred.reject(err);
    return;
  }

  HttpLikeResponse response;
  response.status = 200;
  response.body = "{\"device\":\"zephyr\",\"ok\":true}";
  requestCtx.deferred.resolve(response);
}

void startHttpRequest(Deferred<HttpLikeResponse> request) {
  requestCtx.deferred = request;
  requestCtx.active = true;
  Events.after(200, completeHttpRequest);
}

// Example adapter: in real use this wraps a board/client-specific HTTP library.
Future<HttpLikeResponse> httpGetAsync(const String& url, uint32_t timeoutMs = 5000) {
  requestCtx.url = url;
  requestCtx.timeoutMs = timeoutMs;
  return Events.runAsync<HttpLikeResponse>(startHttpRequest);
}

void onWifiConnected(const WifiConnected& ev) {
  Serial.print("WiFi connected: ");
  Serial.println(ev.ssid);

  pending = httpGetAsync("https://api.example.com/telemetry", 2000)
                .onDone(onTelemetrySuccess)
                .onError(onTelemetryError)
                .onFinish(onRequestCompleted);
}

void emitWifiConnected() {
  WifiConnected ev;
  ev.ssid = WiFi.SSID();
  Events.post(ev);
}

void checkWifiConnection() {
  if (wifiConnectedEventSent) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectedEventSent = true;
    if (wifiPollTimerId != 0) {
      Events.cancelTimer(wifiPollTimerId);
      wifiPollTimerId = 0;
    }
    emitWifiConnected();
  }
}

void runHealthCheck() {
  httpGetAsync("https://api.example.com/health", 3000)
      .onDone(onHealthSuccess)
      .onError(onHealthError);
}

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 32;
  cfg.workerThreadCount = 1;
  arduino_events::begin(cfg);

  requestCtx.active = false;
  wifiSub = Events.listen<WifiConnected>(onWifiConnected);

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);

  // Poll connection state and emit WifiConnected only when really connected.
  wifiPollTimerId = Events.every(500, checkWifiConnection);

  // Periodic async workflow example.
  Events.every(10000, runHealthCheck);
}

void loop() {
  // All callbacks are dispatched here without user-managed threads.
  arduino_events::update();
}
