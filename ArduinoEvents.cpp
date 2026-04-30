#include "ArduinoEvents.h"

#include <algorithm>
#include <any>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace arduino_events {
namespace {

uint64_t nowMs() {
#if __has_include(<Arduino.h>)
	return static_cast<uint64_t>(millis());
#else
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

struct HandlerEntry {
	uint32_t id = 0;
	size_t eventTypeId = 0;
	std::function<void(const void*)> handler;
	bool once = false;
	bool removed = false;
};

struct TimerEntry {
	uint32_t id = 0;
	uint64_t dueAtMs = 0;
	uint32_t periodMs = 0;
	bool repeat = false;
	bool cancelled = false;
	std::function<void()> callback;
};

struct FutureState {
	bool ready = false;
	bool cancelled = false;
	bool hasError = false;
	uint64_t timeoutAtMs = 0;
	Error error;
	std::any value;

	std::vector<std::function<void(const std::any&)>> onSuccess;
	std::vector<std::function<void(const Error&)>> onError;
	std::vector<std::function<void()>> onFinally;
};

struct RuntimeState {
	Config config;
	bool running = false;
	uint32_t nextSubscriptionId = 1;
	uint32_t nextTimerId = 1;
	uint32_t nextFutureToken = 1;

	std::vector<HandlerEntry> handlers;
	std::vector<TimerEntry> timers;
	std::unordered_map<uint32_t, std::shared_ptr<FutureState>> futures;
	std::deque<std::function<void()>> callbackQueue;

	mutable std::mutex mutex;
};

RuntimeState& rt() {
	static RuntimeState s;
	return s;
}

void queueFutureCallbacksLocked(RuntimeState& state, const std::shared_ptr<FutureState>& fs) {
	const bool runSuccess = fs->ready && !fs->hasError && !fs->cancelled;
	const bool runError = fs->ready && (fs->hasError || fs->cancelled);

	std::shared_ptr<std::any> payload;
	if (runSuccess) {
		payload = std::make_shared<std::any>(fs->value);
	}
	Error err = fs->error;

	auto successHandlers = std::move(fs->onSuccess);
	auto errorHandlers = std::move(fs->onError);
	auto finallyHandlers = std::move(fs->onFinally);

	fs->onSuccess.clear();
	fs->onError.clear();
	fs->onFinally.clear();

	if (runSuccess) {
		for (auto& cb : successHandlers) {
			if (state.callbackQueue.size() >= state.config.eventQueueCapacity) {
				break;
			}
			state.callbackQueue.push_back([cb = std::move(cb), payload]() { cb(*payload); });
		}
	}

	if (runError) {
		for (auto& cb : errorHandlers) {
			if (state.callbackQueue.size() >= state.config.eventQueueCapacity) {
				break;
			}
			state.callbackQueue.push_back([cb = std::move(cb), err]() { cb(err); });
		}
	}

	for (auto& cb : finallyHandlers) {
		if (state.callbackQueue.size() >= state.config.eventQueueCapacity) {
			break;
		}
		state.callbackQueue.push_back(std::move(cb));
	}
}

}  // namespace

Result<void> Result<void>::ok() {
	Result<void> out;
	out.ok_ = true;
	return out;
}

Result<void> Result<void>::err(Error error) {
	Result<void> out;
	out.ok_ = false;
	out.error_ = std::move(error);
	return out;
}

bool Result<void>::isOk() const {
	return ok_;
}

const Error& Result<void>::error() const {
	return error_;
}

Future<void>& Future<void>::then(SuccessHandler onSuccess) {
	Runtime::instance().futureOnSuccess(token_, [handler = std::move(onSuccess)](const std::any&) {
		handler();
	});
	return *this;
}

Future<void>& Future<void>::catchError(ErrorHandler onError) {
	Runtime::instance().futureOnError(token_, std::move(onError));
	return *this;
}

Future<void>& Future<void>::finally(FinallyHandler onFinally) {
	Runtime::instance().futureOnFinally(token_, std::move(onFinally));
	return *this;
}

Future<void>& Future<void>::withTimeout(uint32_t timeoutMs) {
	Runtime::instance().futureSetTimeout(token_, timeoutMs);
	return *this;
}

bool Future<void>::cancel() {
	return Runtime::instance().futureCancel(token_);
}

bool Future<void>::isReady() const {
	return Runtime::instance().futureIsReady(token_);
}

bool Future<void>::hasError() const {
	return Runtime::instance().futureHasError(token_);
}

bool Future<void>::isCancelled() const {
	return Runtime::instance().futureIsCancelled(token_);
}

bool Future<void>::tryGet(Result<void>& out) const {
	std::any value;
	Error error;
	bool hasError = false;
	if (!Runtime::instance().futureTryGetAny(token_, value, error, hasError)) {
		return false;
	}

	if (hasError) {
		out = Result<void>::err(std::move(error));
	} else {
		out = Result<void>::ok();
	}
	return true;
}

bool Deferred<void>::resolve() {
	return Runtime::instance().futureResolveAny(token_, std::any());
}

bool Deferred<void>::reject(Error error) {
	return Runtime::instance().futureReject(token_, std::move(error));
}

bool Deferred<void>::isCancelled() const {
	return Runtime::instance().futureIsCancelled(token_);
}

Runtime& Runtime::instance() {
	static Runtime singleton;
	return singleton;
}

Runtime::Runtime() = default;

bool Runtime::begin(const Config& config) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);

	state.config = config;
	if (state.config.eventQueueCapacity == 0) {
		state.config.eventQueueCapacity = 1;
	}

	state.running = true;
	state.nextSubscriptionId = 1;
	state.nextTimerId = 1;
	state.nextFutureToken = 1;
	state.handlers.clear();
	state.timers.clear();
	state.futures.clear();
	state.callbackQueue.clear();
	return true;
}

void Runtime::end() {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	state.running = false;
	state.handlers.clear();
	state.timers.clear();
	state.futures.clear();
	state.callbackQueue.clear();
}

bool Runtime::isRunning() const {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	return state.running;
}

void Runtime::update(uint32_t budgetMs) {
	auto& state = rt();
	const uint64_t startedAt = nowMs();

	{
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.running) {
			return;
		}

		const uint64_t now = nowMs();

		for (auto& timer : state.timers) {
			if (timer.cancelled || timer.dueAtMs > now) {
				continue;
			}

			if (state.callbackQueue.size() < state.config.eventQueueCapacity) {
				state.callbackQueue.push_back(timer.callback);
			}

			if (timer.repeat) {
				timer.dueAtMs = now + timer.periodMs;
			} else {
				timer.cancelled = true;
			}
		}

		for (auto& item : state.futures) {
			auto& fs = item.second;
			if (!fs || fs->ready || fs->timeoutAtMs == 0 || fs->timeoutAtMs > now) {
				continue;
			}

			fs->ready = true;
			fs->hasError = true;
			fs->cancelled = false;
			fs->error.code = ErrorCode::Timeout;
			fs->error.message = "future timeout";
			queueFutureCallbacksLocked(state, fs);
		}

		state.timers.erase(
				std::remove_if(state.timers.begin(), state.timers.end(),
											 [](const TimerEntry& timer) { return timer.cancelled; }),
				state.timers.end());
	}

	while (true) {
		std::function<void()> callback;
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			if (state.callbackQueue.empty()) {
				break;
			}
			callback = std::move(state.callbackQueue.front());
			state.callbackQueue.pop_front();
		}

		callback();

		if (budgetMs > 0 && (nowMs() - startedAt) >= budgetMs) {
			break;
		}
	}
}

Subscription Runtime::registerHandler(size_t eventTypeId, RawEventHandler handler, bool once) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running) {
		return {};
	}

	HandlerEntry entry;
	entry.id = state.nextSubscriptionId++;
	entry.eventTypeId = eventTypeId;
	entry.handler = std::move(handler);
	entry.once = once;
	state.handlers.push_back(std::move(entry));
	return Subscription{state.handlers.back().id};
}

bool Runtime::emitRaw(size_t eventTypeId, const void* eventData, size_t eventSize) {
	(void)eventSize;

	auto& state = rt();
	std::vector<uint32_t> consumed;
	std::vector<std::function<void(const void*)>> toRun;

	{
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.running) {
			return false;
		}

		for (auto& h : state.handlers) {
			if (h.removed || h.eventTypeId != eventTypeId) {
				continue;
			}
			toRun.push_back(h.handler);
			if (h.once) {
				consumed.push_back(h.id);
			}
		}

		if (!consumed.empty()) {
			for (auto id : consumed) {
				for (auto& h : state.handlers) {
					if (h.id == id) {
						h.removed = true;
						break;
					}
				}
			}

			state.handlers.erase(
					std::remove_if(state.handlers.begin(), state.handlers.end(),
												 [](const HandlerEntry& h) { return h.removed; }),
					state.handlers.end());
		}
	}

	for (auto& fn : toRun) {
		fn(eventData);
	}
	return true;
}

bool Runtime::off(Subscription subscription) {
	if (!subscription) {
		return false;
	}

	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = std::find_if(state.handlers.begin(), state.handlers.end(),
												 [&](const HandlerEntry& h) { return h.id == subscription.id; });
	if (it == state.handlers.end()) {
		return false;
	}

	state.handlers.erase(it);
	return true;
}

uint32_t Runtime::after(uint32_t delayMs, std::function<void()> callback) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running) {
		return 0;
	}

	TimerEntry timer;
	timer.id = state.nextTimerId++;
	timer.dueAtMs = nowMs() + delayMs;
	timer.periodMs = delayMs;
	timer.repeat = false;
	timer.callback = std::move(callback);
	state.timers.push_back(std::move(timer));
	return state.timers.back().id;
}

uint32_t Runtime::every(uint32_t periodMs, std::function<void()> callback) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running || periodMs == 0) {
		return 0;
	}

	TimerEntry timer;
	timer.id = state.nextTimerId++;
	timer.dueAtMs = nowMs() + periodMs;
	timer.periodMs = periodMs;
	timer.repeat = true;
	timer.callback = std::move(callback);
	state.timers.push_back(std::move(timer));
	return state.timers.back().id;
}

bool Runtime::cancelTimer(uint32_t timerId) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	for (auto& timer : state.timers) {
		if (timer.id == timerId) {
			timer.cancelled = true;
			return true;
		}
	}
	return false;
}

Future<void> Runtime::run(std::function<void()> job) {
	const uint32_t token = allocFutureToken();
	if (token == 0) {
		return Future<void>();
	}

	if (!enqueue([this, token, job = std::move(job)]() mutable {
		if (futureIsCancelled(token)) {
			return;
		}
		job();
		futureResolveAny(token, std::any());
	})) {
		Error err;
		err.code = ErrorCode::QueueFull;
		err.message = "runtime queue full";
		futureReject(token, std::move(err));
	}

	return Future<void>(token);
}

Future<void> Runtime::defer(std::function<void(Deferred<void>)> start) {
	const uint32_t token = allocFutureToken();
	if (token == 0) {
		return Future<void>();
	}

	if (!enqueue([start = std::move(start), token]() mutable { start(Deferred<void>(token)); })) {
		Error err;
		err.code = ErrorCode::QueueFull;
		err.message = "runtime queue full";
		futureReject(token, std::move(err));
	}

	return Future<void>(token);
}

void Runtime::futureOnSuccess(uint32_t token, std::function<void(const std::any&)> onSuccess) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready && !fs->hasError && !fs->cancelled) {
		auto payload = std::make_shared<std::any>(fs->value);
		if (state.callbackQueue.size() < state.config.eventQueueCapacity) {
			state.callbackQueue.push_back(
					[cb = std::move(onSuccess), payload]() mutable { cb(*payload); });
		}
		return;
	}

	if (!fs->ready) {
		fs->onSuccess.push_back(std::move(onSuccess));
	}
}

void Runtime::futureOnError(uint32_t token, std::function<void(const Error&)> onError) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready && (fs->hasError || fs->cancelled)) {
		const Error err = fs->error;
		if (state.callbackQueue.size() < state.config.eventQueueCapacity) {
			state.callbackQueue.push_back([cb = std::move(onError), err]() { cb(err); });
		}
		return;
	}

	if (!fs->ready) {
		fs->onError.push_back(std::move(onError));
	}
}

void Runtime::futureOnFinally(uint32_t token, std::function<void()> onFinally) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready) {
		if (state.callbackQueue.size() < state.config.eventQueueCapacity) {
			state.callbackQueue.push_back(std::move(onFinally));
		}
		return;
	}

	fs->onFinally.push_back(std::move(onFinally));
}

void Runtime::futureSetTimeout(uint32_t token, uint32_t timeoutMs) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second || it->second->ready) {
		return;
	}

	if (timeoutMs == 0) {
		it->second->timeoutAtMs = 0;
	} else {
		it->second->timeoutAtMs = nowMs() + timeoutMs;
	}
}

bool Runtime::futureCancel(uint32_t token) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second || it->second->ready) {
		return false;
	}

	auto& fs = it->second;
	fs->ready = true;
	fs->cancelled = true;
	fs->hasError = true;
	fs->error.code = ErrorCode::Cancelled;
	fs->error.message = "future cancelled";
	queueFutureCallbacksLocked(state, fs);
	return true;
}

bool Runtime::futureIsReady(uint32_t token) const {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->ready;
}

bool Runtime::futureHasError(uint32_t token) const {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->ready && it->second->hasError;
}

bool Runtime::futureIsCancelled(uint32_t token) const {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->cancelled;
}

bool Runtime::futureTryGetAny(uint32_t token, std::any& outValue, Error& outError,
															bool& outHasError) const {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second || !it->second->ready) {
		return false;
	}

	outHasError = it->second->hasError;
	if (outHasError) {
		outError = it->second->error;
	} else {
		outValue = it->second->value;
	}
	return true;
}

bool Runtime::futureResolveAny(uint32_t token, std::any value) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second || it->second->ready) {
		return false;
	}

	auto& fs = it->second;
	fs->ready = true;
	fs->cancelled = false;
	fs->hasError = false;
	fs->value = std::move(value);
	queueFutureCallbacksLocked(state, fs);
	return true;
}

bool Runtime::futureReject(uint32_t token, Error error) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second || it->second->ready) {
		return false;
	}

	auto& fs = it->second;
	fs->ready = true;
	fs->cancelled = false;
	fs->hasError = true;
	fs->error = std::move(error);
	queueFutureCallbacksLocked(state, fs);
	return true;
}

uint32_t Runtime::allocFutureToken() {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running) {
		return 0;
	}

	const uint32_t token = state.nextFutureToken++;
	state.futures.emplace(token, std::make_shared<FutureState>());
	return token;
}

bool Runtime::enqueue(std::function<void()> callback) {
	auto& state = rt();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (!state.running || state.callbackQueue.size() >= state.config.eventQueueCapacity) {
		return false;
	}

	state.callbackQueue.push_back(std::move(callback));
	return true;
}

}  // namespace arduino_events

