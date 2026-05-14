# ArduinoEvents

A lightweight event-driven library for Arduino. It provides a simple way to register callbacks and emit events, manage timers and async operations in Arduino sketches with a simple flow.

The goal is to avoid blocking code and keep your sketch loop() clean when multiple activities run in parallel.

## Who Is It For

This library is useful when you want to:

- react to internal events (states, sensors, inputs)
- run periodic or delayed actions (`every`, `after`)
- start an async task and receive the result later (`runAsync`)
- handle timeouts and cancellation

Avoid using it if:

- you only need simple timing (millis())
- you are dealing directly with hardware interrupts

Unlike task schedulers or input-specific libraries, ArduinoEvents is a
general-purpose event system.

## Requirements

- Supported architecture: `zephyr` (see `library.properties`)
- Main include: `#include <ArduinoEvents.h>`

## Quick Start

1. In `setup()`, initialize the library with `arduino_events::begin(...)`
2. Register listeners, timers, or async tasks
3. In `loop()`, always call `arduino_events::update()`

Minimal example:

```cpp
#include <ArduinoEvents.h>

using namespace arduino_events;

struct LedToggleEvent {};

void onLedToggle(const LedToggleEvent&) {
	Serial.println("Toggle LED");
}

void emitToggle() {
	Events.post(LedToggleEvent{});
}

void setup() {
	Serial.begin(115200);
	arduino_events::begin();

	Events.listen<LedToggleEvent>(onLedToggle);
	Events.every(1000, emitToggle);
}

void loop() {
	arduino_events::update();
}
```

## Key Concepts

### 1) Events (`listen`, `listenOnce`, `post`)

- `Events.listen<EventT>(callback)` registers a listener
- `Events.listenOnce<EventT>(callback)` runs once, then auto-removes
- `Events.post(EventT{...})` publishes the event
- `Events.unlisten(subscription)` removes a listener

When you publish an event, callbacks run in the sketch context through `update()`.

### 2) Timers (`after`, `every`, `cancelTimer`)

- `Events.after(ms, callback)` runs once after `ms`
- `Events.every(ms, callback)` runs periodically
- `Events.cancelTimer(id)` cancels a timer

Both APIs return a `timerId` (`uint32_t`). If it returns `0`, the timer was not created.

### 3) Async (`runAsync`, `Future`, `Deferred`)

With `runAsync`, you start background work and get a `Future`:

- success: `future.onDone(...)`
- error: `future.onError(...)`
- always at the end: `future.onFinish(...)`
- timeout: `future.withTimeout(ms)`
- cancellation: `future.cancel()`

Inside the async job you receive a `Deferred<T>`:

- `deferred.resolve(value)` for success
- `deferred.reject(error)` for failure
- `deferred.isCancelled()` to stop work that is no longer needed

## Configuration (`Config`)

You can start with defaults:

```cpp
arduino_events::begin();
```

Or customize:

```cpp
Config cfg;
cfg.eventQueueCapacity = 32;
cfg.workerQueueCapacity = 16;
cfg.workerThreadCount = 1;
cfg.defaultTimeoutMs = 30000;
arduino_events::begin(cfg);
```

Practical meaning of each field:

- `eventQueueCapacity`: how many callbacks can be queued
- `workerQueueCapacity`: internal capacity used for jobs/timers
- `workerThreadCount`: number of parallel workers (max 4)
- `defaultTimeoutMs`: available in config (use `withTimeout(...)` on futures)

## Error Handling

Errors are reported as `Error`:

- `err.code` (`ErrorCode`)
- `err.message`
- `err.nativeCode` (optional native code)

Main error codes:

- `Timeout`
- `Cancelled`
- `QueueFull`
- `InvalidState`
- `Network`
- `Internal`

## Included Examples

- `examples/EventBasics/EventBasics.ino`
	- basic events, `listen`, `listenOnce`, `unlisten`
- `examples/TimersBasics/TimersBasics.ino`
	- one-shot and periodic timers, cancellation
- `examples/RunAsyncBasics/RunAsyncBasics.ino`
	- `runAsync` with result and error handling
- `examples/RunAsyncTimeoutCancel/RunAsyncTimeoutCancel.ino`
	- timeout (`withTimeout`) and cancellation (`cancel`)
- `examples/StateMachineWithEvents/StateMachineWithEvents.ino`
	- event-driven state machine
- `examples/AsyncHttpConnect/AsyncHttpConnect.ino`
	- WiFi workflow + async HTTP request (simulated)

## Best Practices

- Call `arduino_events::update()` in every `loop()`.
- Avoid long callbacks in the main context.
- In async tasks, check `deferred.isCancelled()` before resolving.
- Use `listenOnce` for events that should be consumed only once.
- Explicitly remove listeners and timers you no longer need.

## Quick API

- Init:
	- `bool arduino_events::begin(const Config& = {})`
	- `void arduino_events::update(uint32_t budgetMs = 0)`
- Events:
	- `Events.listen<EventT>(handler)`
	- `Events.listenOnce<EventT>(handler)`
	- `Events.post(event)`
	- `Events.unlisten(subscription)`
- Timer:
	- `Events.after(delayMs, callback)`
	- `Events.every(periodMs, callback)`
	- `Events.cancelTimer(timerId)`
- Async:
	- `Events.runAsync<T>(start)`
	- `Future<T>.onDone(...).onError(...).onFinish(...)`
	- `Future<T>.withTimeout(ms)`
	- `Future<T>.cancel()`

## Runtime Shutdown

If needed, you can stop everything with:

```cpp
Events.end();
```

In many sketches this is not required, but it can help in controlled restart/stop flows.