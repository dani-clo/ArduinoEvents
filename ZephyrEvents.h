#pragma once

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#else
#include <string>
using String = std::string;
#endif

#include <cstddef>
#include <cstdint>
#include <any>
#include <functional>
#include <type_traits>
#include <utility>

namespace zephyr_events {

struct Config {
	size_t eventQueueCapacity = 32;
	size_t workerQueueCapacity = 16;
	uint8_t workerThreadCount = 1;
	uint32_t defaultTimeoutMs = 30000;
};

enum class ErrorCode : uint8_t {
	None = 0,
	Timeout,
	Cancelled,
	QueueFull,
	InvalidState,
	Network,
	Internal,
};

struct Error {
	ErrorCode code = ErrorCode::None;
	int nativeCode = 0;
	String message;
};

template <typename T>
class Result {
 public:
	static Result<T> ok(T value);
	static Result<T> err(Error error);

	bool isOk() const;
	const T& value() const;
	T& value();
	const Error& error() const;

 private:
	Result() = default;

	bool ok_ = false;
	T value_{};
	Error error_{};
};

template <>
class Result<void> {
 public:
	static Result<void> ok();
	static Result<void> err(Error error);

	bool isOk() const;
	const Error& error() const;

 private:
	Result() = default;

	bool ok_ = false;
	Error error_{};
};

template <typename T>
class Future {
 public:
	using SuccessHandler = std::function<void(const T&)>;
	using ErrorHandler = std::function<void(const Error&)>;
	using FinallyHandler = std::function<void()>;

	Future() = default;

	Future<T>& then(SuccessHandler onSuccess);
	Future<T>& catchError(ErrorHandler onError);
	Future<T>& finally(FinallyHandler onFinally);
	Future<T>& withTimeout(uint32_t timeoutMs);

	bool cancel();
	bool isReady() const;
	bool hasError() const;
	bool isCancelled() const;

	bool tryGet(Result<T>& out) const;

 private:
	friend class Runtime;
	explicit Future(uint32_t token) : token_(token) {}

	uint32_t token_ = 0;
};

template <>
class Future<void> {
 public:
	using SuccessHandler = std::function<void()>;
	using ErrorHandler = std::function<void(const Error&)>;
	using FinallyHandler = std::function<void()>;

	Future() = default;

	Future<void>& then(SuccessHandler onSuccess);
	Future<void>& catchError(ErrorHandler onError);
	Future<void>& finally(FinallyHandler onFinally);
	Future<void>& withTimeout(uint32_t timeoutMs);

	bool cancel();
	bool isReady() const;
	bool hasError() const;
	bool isCancelled() const;

	bool tryGet(Result<void>& out) const;

 private:
	friend class Runtime;
	explicit Future(uint32_t token) : token_(token) {}

	uint32_t token_ = 0;
};

struct Subscription {
	uint32_t id = 0;

	explicit operator bool() const { return id != 0; }
};

template <typename EventT>
using EventHandler = std::function<void(const EventT&)>;

template <typename T>
class Deferred {
 public:
	Deferred() = default;

	bool resolve(T value);
	bool reject(Error error);
	bool isCancelled() const;

 private:
	friend class Runtime;
	explicit Deferred(uint32_t token) : token_(token) {}

	uint32_t token_ = 0;
};

template <>
class Deferred<void> {
 public:
	Deferred() = default;

	bool resolve();
	bool reject(Error error);
	bool isCancelled() const;

 private:
	friend class Runtime;
	explicit Deferred(uint32_t token) : token_(token) {}

	uint32_t token_ = 0;
};

class Runtime {
 public:
	static Runtime& instance();

	bool begin(const Config& config = {});
	void end();
	bool isRunning() const;

	// Call from sketch loop() to dispatch queued callbacks on sketch context.
	void update(uint32_t budgetMs = 0);

	template <typename EventT>
	Subscription on(EventHandler<EventT> handler) {
		return registerHandler(typeId<EventT>(),
													 [handler = std::move(handler)](const void* rawEvent) {
														 handler(*static_cast<const EventT*>(rawEvent));
													 },
													 false);
	}

	template <typename EventT>
	Subscription once(EventHandler<EventT> handler) {
		return registerHandler(typeId<EventT>(),
													 [handler = std::move(handler)](const void* rawEvent) {
														 handler(*static_cast<const EventT*>(rawEvent));
													 },
													 true);
	}

	template <typename EventT>
	bool emit(const EventT& event) {
		return emitRaw(typeId<EventT>(), static_cast<const void*>(&event), sizeof(EventT));
	}

	bool off(Subscription subscription);

	uint32_t after(uint32_t delayMs, std::function<void()> callback);
	uint32_t every(uint32_t periodMs, std::function<void()> callback);
	bool cancelTimer(uint32_t timerId);

	template <typename T>
	Future<T> run(std::function<T()> job) {
		return submitJob<T>(std::move(job));
	}

	Future<void> run(std::function<void()> job);

	template <typename T>
	Future<T> defer(std::function<void(Deferred<T>)> start) {
		return createDeferred<T>(std::move(start));
	}

	Future<void> defer(std::function<void(Deferred<void>)> start);

	// Internal hooks used by Future/Deferred templates.
	void futureOnSuccess(uint32_t token, std::function<void(const std::any&)> onSuccess);
	void futureOnError(uint32_t token, std::function<void(const Error&)> onError);
	void futureOnFinally(uint32_t token, std::function<void()> onFinally);
	void futureSetTimeout(uint32_t token, uint32_t timeoutMs);

	bool futureCancel(uint32_t token);
	bool futureIsReady(uint32_t token) const;
	bool futureHasError(uint32_t token) const;
	bool futureIsCancelled(uint32_t token) const;
	bool futureTryGetAny(uint32_t token, std::any& outValue, Error& outError,
									 bool& outHasError) const;
	bool futureResolveAny(uint32_t token, std::any value);
	bool futureReject(uint32_t token, Error error);

 private:
	using RawEventHandler = std::function<void(const void*)>;

	Runtime();

	template <typename EventT>
	static size_t typeId() {
		static const uint8_t kTypeTag = 0;
		return reinterpret_cast<size_t>(&kTypeTag);
	}

	Subscription registerHandler(size_t eventTypeId, RawEventHandler handler, bool once);
	bool emitRaw(size_t eventTypeId, const void* eventData, size_t eventSize);
	uint32_t allocFutureToken();
	bool enqueue(std::function<void()> callback);

	template <typename T>
	Future<T> submitJob(std::function<T()> job);

	template <typename T>
	Future<T> createDeferred(std::function<void(Deferred<T>)> start);
};

// Convenient global accessor for sketches.
inline Runtime& Async() { return Runtime::instance(); }

inline bool begin(const Config& config = {}) { return Async().begin(config); }
inline void update(uint32_t budgetMs = 0) { Async().update(budgetMs); }

template <typename T>
Result<T> Result<T>::ok(T value) {
	Result<T> out;
	out.ok_ = true;
	out.value_ = std::move(value);
	return out;
}

template <typename T>
Result<T> Result<T>::err(Error error) {
	Result<T> out;
	out.ok_ = false;
	out.error_ = std::move(error);
	return out;
}

template <typename T>
bool Result<T>::isOk() const {
	return ok_;
}

template <typename T>
const T& Result<T>::value() const {
	return value_;
}

template <typename T>
T& Result<T>::value() {
	return value_;
}

template <typename T>
const Error& Result<T>::error() const {
	return error_;
}

template <typename T>
Future<T>& Future<T>::then(SuccessHandler onSuccess) {
	Runtime::instance().futureOnSuccess(
		token_, [handler = std::move(onSuccess)](const std::any& value) {
			handler(std::any_cast<const T&>(value));
		});
	return *this;
}

template <typename T>
Future<T>& Future<T>::catchError(ErrorHandler onError) {
	Runtime::instance().futureOnError(token_, std::move(onError));
	return *this;
}

template <typename T>
Future<T>& Future<T>::finally(FinallyHandler onFinally) {
	Runtime::instance().futureOnFinally(token_, std::move(onFinally));
	return *this;
}

template <typename T>
Future<T>& Future<T>::withTimeout(uint32_t timeoutMs) {
	Runtime::instance().futureSetTimeout(token_, timeoutMs);
	return *this;
}

template <typename T>
bool Future<T>::cancel() {
	return Runtime::instance().futureCancel(token_);
}

template <typename T>
bool Future<T>::isReady() const {
	return Runtime::instance().futureIsReady(token_);
}

template <typename T>
bool Future<T>::hasError() const {
	return Runtime::instance().futureHasError(token_);
}

template <typename T>
bool Future<T>::isCancelled() const {
	return Runtime::instance().futureIsCancelled(token_);
}

template <typename T>
bool Future<T>::tryGet(Result<T>& out) const {
	std::any value;
	Error error;
	bool hasError = false;
	if (!Runtime::instance().futureTryGetAny(token_, value, error, hasError)) {
		return false;
	}

	if (hasError) {
		out = Result<T>::err(std::move(error));
		return true;
	}

	out = Result<T>::ok(std::any_cast<T>(std::move(value)));
	return true;
}

template <typename T>
bool Deferred<T>::resolve(T value) {
	return Runtime::instance().futureResolveAny(token_, std::any(std::move(value)));
}

template <typename T>
bool Deferred<T>::reject(Error error) {
	return Runtime::instance().futureReject(token_, std::move(error));
}

template <typename T>
bool Deferred<T>::isCancelled() const {
	return Runtime::instance().futureIsCancelled(token_);
}

template <typename T>
Future<T> Runtime::submitJob(std::function<T()> job) {
	const uint32_t token = allocFutureToken();
	if (token == 0) {
		return Future<T>();
	}

	if (!enqueue([this, token, job = std::move(job)]() mutable {
		if (futureIsCancelled(token)) {
			return;
		}
		futureResolveAny(token, std::any(job()));
	})) {
		Error err;
		err.code = ErrorCode::QueueFull;
		err.message = "runtime queue full";
		futureReject(token, std::move(err));
	}

	return Future<T>(token);
}

template <typename T>
Future<T> Runtime::createDeferred(std::function<void(Deferred<T>)> start) {
	const uint32_t token = allocFutureToken();
	if (token == 0) {
		return Future<T>();
	}

	if (!enqueue([start = std::move(start), token]() mutable {
		start(Deferred<T>(token));
	})) {
		Error err;
		err.code = ErrorCode::QueueFull;
		err.message = "runtime queue full";
		futureReject(token, std::move(err));
	}

	return Future<T>(token);
}

}  // namespace zephyr_events

