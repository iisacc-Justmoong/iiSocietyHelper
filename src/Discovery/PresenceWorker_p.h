#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace iiSocietyHelper::detail {

// Native presence and mirror I/O have one active operation and one pending update.
class PresenceWorker final {
    struct DetachedRegistry {
        std::mutex mutex;
        std::condition_variable idle;
        std::size_t admitted = 0;
        bool closing = false;
    };
    struct DetachedTicket {
        DetachedRegistry *registry;
        explicit DetachedTicket(DetachedRegistry *value) : registry(value) {}
        ~DetachedTicket() {
            std::lock_guard lock(registry->mutex);
            if (--registry->admitted == 0) registry->idle.notify_all();
        }
    };
    // The registry itself is intentionally never destroyed: a host that does
    // not use the shutdown barrier must not race its mutex during finalization.
    static DetachedRegistry &detachedRegistry() {
        static auto *registry = new DetachedRegistry;
        return *registry;
    }
    struct State {
        std::mutex mutex;
        std::condition_variable changed, idle;
        std::function<void()> pending;
        bool active = false, stopping = false;
    };
public:
    enum class Shutdown { wait, detachActive };
    explicit PresenceWorker(Shutdown shutdown = Shutdown::wait)
        : m_state(std::make_shared<State>()), m_shutdown(shutdown),
          m_thread([state = m_state] { run(state); }) {}
    ~PresenceWorker() {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stopping = true;
            m_state->pending = {};
        }
        m_state->changed.notify_one();
        if (m_shutdown == Shutdown::detachActive) m_thread.detach();
        else m_thread.join();
    }
    PresenceWorker(const PresenceWorker &) = delete;
    PresenceWorker &operator=(const PresenceWorker &) = delete;

    bool post(std::function<void()> operation) {
        if (m_shutdown == Shutdown::detachActive) {
            auto &registry = detachedRegistry();
            std::shared_ptr<DetachedTicket> ticket;
            {
                std::lock_guard lock(registry.mutex);
                if (registry.closing) return false;
                ticket = std::make_shared<DetachedTicket>(&registry);
                ++registry.admitted;
            }
            operation = [ticket = std::move(ticket), operation = std::move(operation)] { operation(); };
        }
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->stopping) return false;
            m_state->pending = std::move(operation);
        }
        m_state->changed.notify_one();
        return true;
    }
    void clearAndWait() {
        std::unique_lock lock(m_state->mutex);
        m_state->pending = {};
        m_state->idle.wait(lock, [state = m_state] { return !state->active; });
    }
    void drain() {
        std::unique_lock lock(m_state->mutex);
        m_state->idle.wait(lock, [state = m_state] { return !state->active && !state->pending; });
    }
    // Irreversible process-shutdown barrier. Call on an external worker, never
    // the UI thread; accepted operations retain their captures until finished.
    static void quiesceDetachedWork() {
        auto &registry = detachedRegistry();
        std::unique_lock lock(registry.mutex);
        registry.closing = true;
        registry.idle.wait(lock, [&] { return registry.admitted == 0; });
    }
    static bool retireDetached(std::function<void()> operation) {
        auto worker = std::make_shared<PresenceWorker>(Shutdown::detachActive);
        return worker->post([worker, operation = std::move(operation)] { operation(); });
    }

private:
    static void run(const std::shared_ptr<State> &state) {
        std::unique_lock lock(state->mutex);
        for (;;) {
            state->changed.wait(lock, [&] { return state->stopping || bool(state->pending); });
            if (state->stopping) return;
            auto operation = std::exchange(state->pending, {});
            state->active = true;
            lock.unlock();
            operation();
            operation = {};
            lock.lock();
            state->active = false;
            state->idle.notify_all();
        }
    }
    std::shared_ptr<State> m_state;
    Shutdown m_shutdown;
    std::thread m_thread;
};

} // namespace iiSocietyHelper::detail
