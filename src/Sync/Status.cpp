#include "Status.h"
#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace iiSocietyHelper::Sync {
struct StatusSubscription::State final {
    mutable std::mutex mutex;
    std::shared_ptr<StatusWake> wake;
    std::shared_ptr<std::function<void(StatusEvent)>> callback;
    std::vector<StatusEvent> queue;
    std::size_t head = 0, count = 0;
    bool closed = false, draining = false;
    std::exception_ptr error;
    ~State() { close(); }


    void enqueue(StatusEvent event) {
        std::shared_ptr<StatusWake> notify;
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            if (count == queue.size()) {
                head = 0; count = 1; event.gap = true; queue[0] = event;
            } else {
                queue[(head + count) % queue.size()] = event; ++count;
            }
            notify = wake;
        }
        if (notify) (void)notify->signal();
    }
    void drain() {
        {
            std::lock_guard lock(mutex);
            if (closed || draining) return;
            draining = true;
        }
        for (;;) {
            StatusEvent event;
            std::shared_ptr<std::function<void(StatusEvent)>> deliver;
            {
                std::lock_guard lock(mutex);
                if (closed || !count) { draining = false; return; }
                event = queue[head]; head = (head + 1) % queue.size(); --count; deliver = callback;
            }
            try { (*deliver)(event); }
            catch (...) {
                { std::lock_guard lock(mutex); error = std::current_exception(); }
                close(); return;
            }
        }
    }
    void close() noexcept {
        std::shared_ptr<StatusWake> retired;
        {
            std::lock_guard lock(mutex);
            if (!closed) {
                closed = true; count = 0; callback.reset();
                std::vector<StatusEvent>().swap(queue);
            }
            // A simultaneous closer must also cross the host's delivery
            // barrier. Retain the cancelled handle for later close calls,
            // including a self-close followed by a close on another thread.
            retired = wake;
        }
        if (retired) retired->cancel();
    }
};

struct Status::State final {
    mutable std::mutex mutex;
    StatusSnapshot current;
    bool alive = true;
    std::vector<std::weak_ptr<StatusSubscription::State>> subscribers;
};

StatusSubscription::~StatusSubscription() { close(); }
StatusSubscription::StatusSubscription(StatusSubscription &&) noexcept = default;
StatusSubscription &StatusSubscription::operator=(StatusSubscription &&other) noexcept {
    if (this != &other) { close(); state_ = std::move(other.state_); }
    return *this;
}
StatusSubscription::operator bool() const noexcept {
    if (!state_) return false;
    std::lock_guard lock(state_->mutex); return !state_->closed;
}
bool StatusSubscription::resume() noexcept {
    if (!state_) return false;
    std::shared_ptr<StatusWake> wake;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return false;
        if (!state_->count) return true;
        wake = state_->wake;
    }
    return wake && wake->signal();
}
void StatusSubscription::close() noexcept { if (state_) state_->close(); }
std::exception_ptr StatusSubscription::failure() const noexcept {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex); return state_->error;
}

Status::Status() : state_(std::make_shared<State>()) {}
Status::~Status() = default;
StatusSnapshot Status::snapshot() const { std::lock_guard lock(state_->mutex); return state_->current; }
StatusSubscription Status::subscribe(StatusExecutor &executor, std::function<void(StatusEvent)> callback, std::size_t capacity) {
    if (!callback || !capacity || capacity > 4096) return {};
    auto subscription = std::make_shared<StatusSubscription::State>();
    subscription->callback = std::make_shared<std::function<void(StatusEvent)>>(std::move(callback));
    subscription->queue.resize(capacity);
    const std::weak_ptr<StatusSubscription::State> weak = subscription;
    subscription->wake = executor.reserve([weak] { if (const auto state = weak.lock()) state->drain(); });
    if (!subscription->wake) return {};
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->alive) { subscription->close(); return {}; }
        std::erase_if(state_->subscribers, [](const auto &subscriber) { return subscriber.expired(); });
        state_->subscribers.emplace_back(subscription);
    }
    return StatusSubscription(std::move(subscription));
}
StatusSource::StatusSource() = default;
StatusSource::~StatusSource() {
    std::vector<std::weak_ptr<StatusSubscription::State>> subscribers;
    {
        std::lock_guard lock(status_.state_->mutex);
        status_.state_->alive = false; subscribers = std::move(status_.state_->subscribers);
    }
    for (const auto &subscriber : subscribers) if (auto state = subscriber.lock()) state->close();
}
void StatusSource::publish(StatusCode code) {
    std::lock_guard lock(status_.state_->mutex);
    auto &current = status_.state_->current;
    const auto value = int(code);
    if (!status_.state_->alive || current.value == value) return;
    current.value = value; ++current.revision;
    const StatusEvent event{current.value, current.revision, false};
    auto &subscribers = status_.state_->subscribers;
    for (auto it = subscribers.begin(); it != subscribers.end();) {
        if (auto state = it->lock()) { state->enqueue(event); ++it; }
        else it = subscribers.erase(it);
    }
}
}
