#include "ManualStatusExecutor.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct ManualStatusExecutor::State {
    struct Wake final : iiSocietyHelper::Sync::StatusWake, std::enable_shared_from_this<Wake> {
        std::weak_ptr<State> owner;
        std::function<void()> callback;
        std::mutex mutex;
        std::condition_variable finished;
        bool active = true, queued = false, notified = false, notifying = false;
        std::uint64_t generation = 0;
        std::size_t running = 0, signalling = 0;
        struct Invocation {
            Wake *wake;
            Invocation *previous;
            inline static thread_local Invocation *current = nullptr;
            explicit Invocation(Wake *value) : wake(value), previous(current) { current = this; }
            ~Invocation() { current = previous; }
            static bool contains(Wake *value) {
                for (auto item = current; item; item = item->previous) if (item->wake == value) return true;
                return false;
            }
        };
        bool signal() noexcept override {
            try {
                auto state = owner.lock(); if (!state) return false;
                std::function<void()> onWake;
                std::uint64_t signalled = 0;
                {
                    std::lock_guard lock(mutex);
                    if (!active) return false;
                    if (queued && notified) return true;
                    if (queued && notifying) return false;
                    std::lock_guard queueLock(state->mutex);
                    if (!state->available) return false;
                    onWake = state->onWake;
                    if (!queued) { state->queue.push_back(shared_from_this()); queued = true; ++generation; }
                    signalled = generation; notifying = true; ++signalling;
                }
                struct SignalCompletion {
                    Wake &wake;
                    ~SignalCompletion() { std::lock_guard lock(wake.mutex); --wake.signalling; wake.finished.notify_all(); }
                } completion{*this};
                Invocation invocation(this);
                try { if (onWake) onWake(); }
                catch (...) {
                    std::lock_guard lock(mutex);
                    if (queued && generation == signalled) { notifying = false; notified = false; }
                    return false;
                }
                {
                    std::lock_guard lock(mutex);
                    if (queued && generation == signalled) { notifying = false; notified = true; }
                }
                return true;
            } catch (...) { return false; }
        }
        void cancel() noexcept override {
            std::unique_lock lock(mutex); active = false; queued = false; notified = false; notifying = false;
            if (auto state = owner.lock()) {
                std::lock_guard queueLock(state->mutex);
                std::erase_if(state->queue, [this](const auto &wake) { return wake.get() == this; });
            }
            if (!Invocation::contains(this)) finished.wait(lock, [this] { return running == 0 && signalling == 0; });
        }
        void run() {
            {
                std::lock_guard lock(mutex);
                if (!active || !queued) return;
                queued = false; notified = false; notifying = false; ++running;
            }
            struct Completion {
                Wake &wake;
                ~Completion() { std::lock_guard lock(wake.mutex); --wake.running; wake.finished.notify_all(); }
            } completion{*this};
            Invocation invocation(this);
            callback();
        }
    };
    std::mutex mutex;
    std::deque<std::shared_ptr<Wake>> queue;
    std::function<void()> onWake;
    bool available = true;
};
ManualStatusExecutor::ManualStatusExecutor(std::function<void()> onWake) : state_(std::make_shared<State>()) {
    state_->onWake = std::move(onWake);
}
ManualStatusExecutor::~ManualStatusExecutor() = default;
std::shared_ptr<iiSocietyHelper::Sync::StatusWake> ManualStatusExecutor::reserve(std::function<void()> callback) {
    auto wake = std::make_shared<State::Wake>(); wake->owner = state_; wake->callback = std::move(callback); return wake;
}
void ManualStatusExecutor::drain() {
    for (;;) {
        std::shared_ptr<State::Wake> wake;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->queue.empty()) return;
            wake = std::move(state_->queue.front()); state_->queue.pop_front();
        }
        wake->run();
    }
}
void ManualStatusExecutor::setAvailable(bool available) {
    std::lock_guard lock(state_->mutex); state_->available = available;
}
