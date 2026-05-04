#include "ArduinoEvents.h"

#include <algorithm>
#include <any>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

#if __has_include(<zephyr/kernel.h>)
#include <zephyr/kernel.h>
#else
#error "This library requires Zephyr environment."
#endif

namespace arduino_events {

Runtime& Events = Runtime::instance();

namespace {
constexpr size_t kMaxWorkerThreads = 4;
constexpr size_t kWorkerStackSize = 3072;
K_THREAD_STACK_ARRAY_DEFINE(g_workerStacks, kMaxWorkerThreads, kWorkerStackSize);
struct k_work_q g_workerQueues[kMaxWorkerThreads];
bool g_workerQueueStarted[kMaxWorkerThreads] = {false, false, false, false};
}  // namespace

namespace {

template <typename MutexT>
class LockGuard {
 public:
	 explicit LockGuard(MutexT& mutex) : mutex_(mutex) {
		mutex_.lock();
	 }

	 ~LockGuard() {
		mutex_.unlock();
	 }

	 LockGuard(const LockGuard&) = delete;
	 LockGuard& operator=(const LockGuard&) = delete;

 private:
	 MutexT& mutex_;
};

struct RuntimeMutex {
	 RuntimeMutex() {
		k_mutex_init(&mutex_);
	 }

	 void lock() {
		(void)k_mutex_lock(&mutex_, K_FOREVER);
	 }

	 bool try_lock() {
		return k_mutex_lock(&mutex_, K_NO_WAIT) == 0;
	 }

	 void unlock() {
		(void)k_mutex_unlock(&mutex_);
	 }

 private:
	 struct k_mutex mutex_;
};

	struct QueuedCallback {
		void (*invoke)(void*) = nullptr;
		void (*destroy)(void*) = nullptr;
		void* context = nullptr;
	};

	struct CallbackPayload {
		std::function<void()> callback;
	};

	struct WorkerJob {
		struct k_work work;
		std::function<void()> callback;
	};

	void invokeCallbackPayload(void* context) {
		auto* payload = static_cast<CallbackPayload*>(context);
		payload->callback();
	}

	void destroyCallbackPayload(void* context) {
		auto* payload = static_cast<CallbackPayload*>(context);
		payload->~CallbackPayload();
		k_free(payload);
	}

	void workerJobHandler(struct k_work* work) {
		auto* job = CONTAINER_OF(work, WorkerJob, work);
		auto callback = std::move(job->callback);
		job->~WorkerJob();
		k_free(job);
		if (callback) {
			callback();
		}
	}

	struct TimerEntry {
		uint32_t id = 0;
		uint32_t periodMs = 0;
		bool repeat = false;
		bool cancelled = true;
		bool active = false;
		bool pending = false;
		uint32_t generation = 0;
		std::function<void()> callback;
		struct k_work_delayable work;
	};

	struct RuntimeState;

	bool pushCallbackLocked(RuntimeState& state, std::function<void()> callback);
	void drainCallbackQueueLocked(RuntimeState& state);
	void timerWorkHandler(struct k_work* work);

uint64_t nowMs() {
		return k_cyc_to_ms_floor64(static_cast<uint64_t>(sys_clock_cycle_get_32()));
}

struct HandlerEntry {
	uint32_t id = 0;
	size_t eventTypeId = 0;
	std::function<void(const void*)> handler;
	bool once = false;
	bool removed = false;
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
	uint32_t timerGeneration = 0;
	size_t workerQueueCount = 1;
	uint32_t nextWorkerQueue = 0;

	std::vector<HandlerEntry> handlers;
	std::vector<std::unique_ptr<TimerEntry>> timers;
	std::unordered_map<uint32_t, std::shared_ptr<FutureState>> futures;

	struct k_msgq callbackQueue;
	std::vector<char> callbackQueueStorage;
	bool callbackQueueReady = false;

	mutable RuntimeMutex mutex;
};

RuntimeState& rt() {
	static RuntimeState s;
	return s;
}

bool pushCallbackLocked(RuntimeState& state, std::function<void()> callback) {
	if (!state.callbackQueueReady) {
		return false;
	}

	void* rawPayload = k_malloc(sizeof(CallbackPayload));
	if (rawPayload == nullptr) {
		return false;
	}

	auto* payload = new (rawPayload) CallbackPayload{std::move(callback)};
	QueuedCallback queued;
	queued.invoke = invokeCallbackPayload;
	queued.destroy = destroyCallbackPayload;
	queued.context = payload;

	if (k_msgq_put(&state.callbackQueue, &queued, K_NO_WAIT) != 0) {
		destroyCallbackPayload(payload);
		return false;
	}

	return true;
}

void drainCallbackQueueLocked(RuntimeState& state) {
	if (!state.callbackQueueReady) {
		return;
	}

	QueuedCallback queued;
	while (k_msgq_get(&state.callbackQueue, &queued, K_NO_WAIT) == 0) {
		if (queued.destroy != nullptr && queued.context != nullptr) {
			queued.destroy(queued.context);
		}
	}
}

void timerWorkHandler(struct k_work* work) {
	auto* delayable = k_work_delayable_from_work(work);
	auto* timer = CONTAINER_OF(delayable, TimerEntry, work);
	auto& state = rt();

	LockGuard<RuntimeMutex> lock(state.mutex);
	if (!timer->pending) {
		return;
	}

	if (!state.running || !timer->active || timer->cancelled ||
				timer->generation != state.timerGeneration) {
		timer->pending = false;
		timer->active = false;
		timer->cancelled = true;
		timer->callback = nullptr;
		return;
	}

	(void)pushCallbackLocked(state, timer->callback);

	if (timer->repeat && !timer->cancelled) {
		const int rc = k_work_reschedule_for_queue(
				&k_sys_work_q, &timer->work, K_MSEC(timer->periodMs));
		if (rc < 0) {
			timer->pending = false;
			timer->active = false;
			timer->cancelled = true;
			timer->callback = nullptr;
		}
		return;
	}

	timer->pending = false;
	timer->active = false;
	timer->cancelled = true;
	timer->callback = nullptr;
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
			if (!pushCallbackLocked(state, [cb = std::move(cb), payload]() { cb(*payload); })) {
				break;
			}
		}
	}

	if (runError) {
		for (auto& cb : errorHandlers) {
			if (!pushCallbackLocked(state, [cb = std::move(cb), err]() { cb(err); })) {
				break;
			}
		}
	}

	for (auto& cb : finallyHandlers) {
		if (!pushCallbackLocked(state, std::move(cb))) {
			break;
		}
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

Future<void>& Future<void>::onDone(SuccessHandler onSuccess) {
	Runtime::instance().futureOnSuccess(token_, [handler = std::move(onSuccess)](const std::any&) {
		handler();
	});
	return *this;
}

Future<void>& Future<void>::onError(ErrorHandler onError) {
	Runtime::instance().futureOnError(token_, std::move(onError));
	return *this;
}

Future<void>& Future<void>::onFinish(FinallyHandler onFinally) {
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
	LockGuard<RuntimeMutex> lock(state.mutex);
	drainCallbackQueueLocked(state);

	state.config = config;
	if (state.config.eventQueueCapacity == 0) {
		state.config.eventQueueCapacity = 1;
	}
	if (state.config.workerQueueCapacity == 0) {
		state.config.workerQueueCapacity = 1;
	}
	if (state.config.workerThreadCount == 0) {
		state.config.workerThreadCount = 1;
	}

	state.workerQueueCount = static_cast<size_t>(state.config.workerThreadCount);
	if (state.workerQueueCount > kMaxWorkerThreads) {
		state.workerQueueCount = kMaxWorkerThreads;
	}
	state.nextWorkerQueue = 0;

	for (size_t i = 0; i < state.workerQueueCount; ++i) {
		if (g_workerQueueStarted[i]) {
			continue;
		}
		k_work_queue_start(&g_workerQueues[i], g_workerStacks[i],
									 K_THREAD_STACK_SIZEOF(g_workerStacks[i]),
									 CONFIG_SYSTEM_WORKQUEUE_PRIORITY, nullptr);
		g_workerQueueStarted[i] = true;
	}

	const size_t msgSize = sizeof(QueuedCallback);
	state.callbackQueueStorage.assign(state.config.eventQueueCapacity * msgSize, 0);
	k_msgq_init(&state.callbackQueue, state.callbackQueueStorage.data(), msgSize,
						state.config.eventQueueCapacity);
	state.callbackQueueReady = true;

	if (state.timerGeneration == 0) {
		state.timerGeneration = 1;
	} else {
		++state.timerGeneration;
		if (state.timerGeneration == 0) {
			state.timerGeneration = 1;
		}
	}

	if (state.timers.size() != state.config.workerQueueCapacity) {
		state.timers.clear();
		state.timers.reserve(state.config.workerQueueCapacity);
		for (size_t i = 0; i < state.config.workerQueueCapacity; ++i) {
			auto timer = std::make_unique<TimerEntry>();
			timer->generation = state.timerGeneration;
			k_work_init_delayable(&timer->work, timerWorkHandler);
			state.timers.push_back(std::move(timer));
		}
	} else {
		for (auto& timer : state.timers) {
			timer->id = 0;
			timer->periodMs = 0;
			timer->repeat = false;
			timer->cancelled = true;
			timer->active = false;
			timer->generation = state.timerGeneration;
			timer->callback = nullptr;
		}
	}

	state.running = true;
	state.nextSubscriptionId = 1;
	state.nextTimerId = 1;
	state.nextFutureToken = 1;
	state.handlers.clear();
	state.futures.clear();
	return true;
}

void Runtime::end() {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	state.running = false;
	state.handlers.clear();
	for (auto& timer : state.timers) {
		timer->cancelled = true;
		timer->active = false;
		timer->callback = nullptr;
	}
	state.futures.clear();
	drainCallbackQueueLocked(state);
}

bool Runtime::isRunning() const {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	return state.running;
}

void Runtime::update(uint32_t budgetMs) {
	auto& state = rt();
	const uint64_t startedAt = nowMs();

	{
		LockGuard<RuntimeMutex> lock(state.mutex);
		if (!state.running) {
			return;
		}

		const uint64_t now = nowMs();

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

	}

	while (true) {
		QueuedCallback queued;
		{
			LockGuard<RuntimeMutex> lock(state.mutex);
			if (!state.callbackQueueReady ||
					k_msgq_get(&state.callbackQueue, &queued, K_NO_WAIT) != 0) {
				break;
			}
		}

		if (queued.invoke != nullptr && queued.context != nullptr) {
			queued.invoke(queued.context);
		}
		if (queued.destroy != nullptr && queued.context != nullptr) {
			queued.destroy(queued.context);
		}

		if (budgetMs > 0 && (nowMs() - startedAt) >= budgetMs) {
			break;
		}
	}
}

Subscription Runtime::registerHandler(size_t eventTypeId, RawEventHandler handler, bool once) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
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
		LockGuard<RuntimeMutex> lock(state.mutex);
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

bool Runtime::unlisten(Subscription subscription) {
	if (!subscription) {
		return false;
	}

	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
	if (!state.running) {
		return 0;
	}

	for (auto& timer : state.timers) {
		if (timer->active || timer->pending) {
			continue;
		}

		timer->id = state.nextTimerId++;
		timer->periodMs = delayMs;
		timer->repeat = false;
		timer->cancelled = false;
		timer->active = true;
		timer->pending = true;
		timer->generation = state.timerGeneration;
		timer->callback = std::move(callback);

		const int rc = k_work_schedule_for_queue(&k_sys_work_q, &timer->work, K_MSEC(delayMs));
		if (rc < 0) {
			timer->cancelled = true;
			timer->active = false;
			timer->pending = false;
			timer->callback = nullptr;
			return 0;
		}

		return timer->id;
	}

	return 0;
}

uint32_t Runtime::every(uint32_t periodMs, std::function<void()> callback) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	if (!state.running || periodMs == 0) {
		return 0;
	}

	for (auto& timer : state.timers) {
		if (timer->active || timer->pending) {
			continue;
		}

		timer->id = state.nextTimerId++;
		timer->periodMs = periodMs;
		timer->repeat = true;
		timer->cancelled = false;
		timer->active = true;
		timer->pending = true;
		timer->generation = state.timerGeneration;
		timer->callback = std::move(callback);

		const int rc = k_work_schedule_for_queue(&k_sys_work_q, &timer->work, K_MSEC(periodMs));
		if (rc < 0) {
			timer->cancelled = true;
			timer->active = false;
			timer->pending = false;
			timer->callback = nullptr;
			return 0;
		}

		return timer->id;
	}

	return 0;
}

bool Runtime::cancelTimer(uint32_t timerId) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	for (auto& timer : state.timers) {
		if ((timer->active || timer->pending) && timer->id == timerId) {
			timer->cancelled = true;
			timer->active = false;
			return true;
		}
	}
	return false;
}

Future<void> Runtime::runAsync(std::function<void(Deferred<void>)> start) {
	const uint32_t token = allocFutureToken();
	if (token == 0) {
		return Future<void>();
	}

	if (!enqueueWorker([start = std::move(start), token]() mutable {
		start(Deferred<void>(token));
	})) {
		Error err;
		err.code = ErrorCode::QueueFull;
		err.message = "worker queue full";
		futureReject(token, std::move(err));
	}

	return Future<void>(token);
}

void Runtime::futureOnSuccess(uint32_t token, std::function<void(const std::any&)> onSuccess) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready && !fs->hasError && !fs->cancelled) {
		auto payload = std::make_shared<std::any>(fs->value);
		(void)pushCallbackLocked(state,
				[cb = std::move(onSuccess), payload]() mutable { cb(*payload); });
		return;
	}

	if (!fs->ready) {
		fs->onSuccess.push_back(std::move(onSuccess));
	}
}

void Runtime::futureOnError(uint32_t token, std::function<void(const Error&)> onError) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready && (fs->hasError || fs->cancelled)) {
		const Error err = fs->error;
		(void)pushCallbackLocked(state, [cb = std::move(onError), err]() { cb(err); });
		return;
	}

	if (!fs->ready) {
		fs->onError.push_back(std::move(onError));
	}
}

void Runtime::futureOnFinally(uint32_t token, std::function<void()> onFinally) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	if (it == state.futures.end() || !it->second) {
		return;
	}

	auto& fs = it->second;
	if (fs->ready) {
		(void)pushCallbackLocked(state, std::move(onFinally));
		return;
	}

	fs->onFinally.push_back(std::move(onFinally));
}

void Runtime::futureSetTimeout(uint32_t token, uint32_t timeoutMs) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->ready;
}

bool Runtime::futureHasError(uint32_t token) const {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->ready && it->second->hasError;
}

bool Runtime::futureIsCancelled(uint32_t token) const {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	auto it = state.futures.find(token);
	return it != state.futures.end() && it->second && it->second->cancelled;
}

bool Runtime::futureTryGetAny(uint32_t token, std::any& outValue, Error& outError,
															bool& outHasError) const {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
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
	LockGuard<RuntimeMutex> lock(state.mutex);
	if (!state.running) {
		return 0;
	}

	const uint32_t token = state.nextFutureToken++;
	state.futures.emplace(token, std::make_shared<FutureState>());
	return token;
}

bool Runtime::enqueue(std::function<void()> callback) {
	auto& state = rt();
	LockGuard<RuntimeMutex> lock(state.mutex);
	if (!state.running) {
		return false;
	}

	return pushCallbackLocked(state, std::move(callback));
}

bool Runtime::enqueueWorker(std::function<void()> callback) {
	auto& state = rt();
	uint32_t queueIndex = 0;

	{
		LockGuard<RuntimeMutex> lock(state.mutex);
		if (!state.running || state.workerQueueCount == 0) {
			return false;
		}
		queueIndex = state.nextWorkerQueue++ % state.workerQueueCount;
	}

	void* raw = k_malloc(sizeof(WorkerJob));
	if (raw == nullptr) {
		return false;
	}

	auto* job = new (raw) WorkerJob{};
	job->callback = std::move(callback);
	k_work_init(&job->work, workerJobHandler);

	if (k_work_submit_to_queue(&g_workerQueues[queueIndex], &job->work) < 0) {
		job->~WorkerJob();
		k_free(job);
		return false;
	}

	return true;
}

}  // namespace arduino_events

