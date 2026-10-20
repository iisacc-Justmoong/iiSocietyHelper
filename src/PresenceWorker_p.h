#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace iiSocietyHelper::detail {

// Native presence and mirror I/O have one active operation and one pending update.
// No QObject or application event loop is owned by this worker.
class PresenceWorker final {
public:
    PresenceWorker() : m_thread([this] { run(); }) {}
    ~PresenceWorker() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_pending = {};
        }
        m_changed.notify_one();
        m_thread.join();
    }
    PresenceWorker(const PresenceWorker &) = delete;
    PresenceWorker &operator=(const PresenceWorker &) = delete;

    void post(std::function<void()> operation) {
        {
            std::lock_guard lock(m_mutex);
            m_pending = std::move(operation);
        }
        m_changed.notify_one();
    }
    void clearAndWait() {
        std::unique_lock lock(m_mutex);
        m_pending = {};
        m_idle.wait(lock, [this] { return !m_active; });
    }
    void drain() {
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return !m_active && !m_pending; });
    }

private:
    void run() {
        std::unique_lock lock(m_mutex);
        for (;;) {
            m_changed.wait(lock, [this] { return m_stopping || bool(m_pending); });
            if (m_stopping) return;
            auto operation = std::exchange(m_pending, {});
            m_active = true;
            lock.unlock();
            operation();
            lock.lock();
            m_active = false;
            m_idle.notify_all();
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_changed, m_idle;
    std::function<void()> m_pending;
    bool m_active = false, m_stopping = false;
    std::thread m_thread;
};

} // namespace iiSocietyHelper::detail
