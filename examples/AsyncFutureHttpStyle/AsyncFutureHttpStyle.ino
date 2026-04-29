#include <ZephyrEvents.h>

using namespace zephyr_events;

struct HttpLikeResponse {
  int status = -1;
  String body;
};

struct WifiConnected {
  String ssid;
};

// Example adapter: in real use this wraps a board/client-specific HTTP library.
Future<HttpLikeResponse> httpGetAsync(const String& url, uint32_t timeoutMs = 5000) {
  return Async().defer<HttpLikeResponse>(
      [url, timeoutMs](Deferred<HttpLikeResponse> request) mutable {
        // Simulate network completion with a timer callback.
        Async().after(200, [url, timeoutMs, request]() mutable {
          if (request.isCancelled()) {
            return;
          }

          if (url.length() == 0) {
            Error err;
            err.code = ErrorCode::InvalidState;
            err.message = "empty URL";
            request.reject(std::move(err));
            return;
          }

          if (timeoutMs < 200) {
            Error err;
            err.code = ErrorCode::Timeout;
            err.message = "request timeout";
            request.reject(std::move(err));
            return;
          }

          HttpLikeResponse response;
          response.status = 200;
          response.body = "{\"device\":\"zephyr\",\"ok\":true}";
          request.resolve(std::move(response));
        });
      });
}

Subscription wifiSub;
Future<HttpLikeResponse> pending;

void setup() {
  Serial.begin(115200);

  Config cfg;
  cfg.eventQueueCapacity = 32;
  cfg.workerThreadCount = 1;
  zephyr_events::begin(cfg);

  wifiSub = Async().on<WifiConnected>([](const WifiConnected& ev) {
    Serial.print("WiFi connected: ");
    Serial.println(ev.ssid);

    pending = httpGetAsync("https://api.example.com/telemetry", 2000)
                  .then([](const HttpLikeResponse& res) {
                    Serial.print("HTTP status: ");
                    Serial.println(res.status);
                    Serial.print("Body: ");
                    Serial.println(res.body);
                  })
                  .catchError([](const Error& err) {
                    Serial.print("HTTP error code: ");
                    Serial.println(static_cast<int>(err.code));
                    Serial.print("Message: ");
                    Serial.println(err.message);
                  })
                  .finally([]() { Serial.println("Request completed"); });
  });

  // Simulate network stack event.
  Async().after(1000, [] { Async().emit(WifiConnected{"MyWiFi"}); });

  // Periodic async workflow example.
  Async().every(10000, [] {
    httpGetAsync("https://api.example.com/health", 3000)
        .then([](const HttpLikeResponse& res) {
          Serial.print("Health status: ");
          Serial.println(res.status);
        })
        .catchError([](const Error& err) {
          Serial.print("Health check failed: ");
          Serial.println(static_cast<int>(err.code));
        });
  });
}

void loop() {
  // All callbacks are dispatched here without user-managed threads.
  zephyr_events::update();
}
