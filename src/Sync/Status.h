#pragma once
#include "iiSocietyHelper/Export.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace iiSocietyHelper::Sync {
enum class StatusCode : int { Before = 0, InProgress = 1, Complete = 2, Failed = 3, Cancelled = 4 };
struct StatusSnapshot final { int value = 0; std::uint64_t revision = 0; };
struct StatusEvent final { int value = 0; std::uint64_t revision = 0; bool gap = false; };

// The host reserves one reusable wake before a subscription is accepted.
// signal is thread-safe, must not wait for delivery, and must queue rather
// than invoke inline. A successful
// signal eventually dispatches while the reservation and host remain alive.
// cancel is idempotent, discards queued delivery, releases the reservation and
// is a barrier for in-flight signals and delivery, except when called by that
// same operation; it rejects future signals and must not wait for itself. Later external
// cancellation must still wait for that delivery.
class IISOCIETYHELPER_EXPORT StatusWake {
public:
    virtual ~StatusWake() = default;
    virtual bool signal() noexcept = 0;
    virtual void cancel() noexcept = 0;
};
class IISOCIETYHELPER_EXPORT StatusExecutor {
public:
    virtual ~StatusExecutor() = default;
    virtual std::shared_ptr<StatusWake> reserve(std::function<void()> delivery) = 0;
};

class Status;
class StatusSource;
class IISOCIETYHELPER_EXPORT StatusSubscription final {
public:
    StatusSubscription() = default;
    ~StatusSubscription();
    StatusSubscription(StatusSubscription &&) noexcept;
    StatusSubscription &operator=(StatusSubscription &&) noexcept;
    StatusSubscription(const StatusSubscription &) = delete;
    StatusSubscription &operator=(const StatusSubscription &) = delete;
    explicit operator bool() const noexcept;
    // Retry delivery after the host has restored an unavailable wake.
    bool resume() noexcept;
    void close() noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;
private:
    friend class Status;
    friend class StatusSource;
    struct State;
    explicit StatusSubscription(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

// Query is synchronous and thread-safe. Subscribe before the first query;
// a revision gap requests reconciliation with snapshot().
class IISOCIETYHELPER_EXPORT Status final {
public:
    ~Status();
    Status(const Status &) = delete;
    Status &operator=(const Status &) = delete;
    [[nodiscard]] StatusSnapshot snapshot() const;
    [[nodiscard]] StatusSubscription subscribe(StatusExecutor &executor,
        std::function<void(StatusEvent)> callback, std::size_t capacity = 64);
private:
    friend class StatusSource;
    Status();
    struct State;
    std::shared_ptr<State> state_;
};

// Only an owner of a source can publish its state; consumers receive Status.
class IISOCIETYHELPER_EXPORT StatusSource final {
public:
    StatusSource();
    ~StatusSource();
    StatusSource(const StatusSource &) = delete;
    StatusSource &operator=(const StatusSource &) = delete;
    [[nodiscard]] Status &status() noexcept { return status_; }
    void publish(StatusCode code);
private:
    Status status_;
};
}
