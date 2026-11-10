#pragma once
#include <QMetaObject>
#include <QObject>
#include <memory>
#include <mutex>
#include <utility>

namespace iiSocietyHelper::detail {
// Serializes posting against owner destruction. Queued callbacks also check
// the gate on the owner's thread before accessing any owner state.
class CallbackGate final : public std::enable_shared_from_this<CallbackGate> {
public:
    explicit CallbackGate(QObject *owner) : owner_(owner) {}
    void close() { std::lock_guard lock(mutex_); owner_ = nullptr; }
    template<class F> void post(F callback) {
        std::lock_guard lock(mutex_);
        if (!owner_) return;
        auto gate = shared_from_this();
        QMetaObject::invokeMethod(owner_, [gate, callback = std::move(callback)]() mutable {
            { std::lock_guard guard(gate->mutex_); if (!gate->owner_) return; }
            callback();
        }, Qt::QueuedConnection);
    }
private:
    std::mutex mutex_;
    QObject *owner_;
};
}
