#include "../../example/ManualStatusExecutor.h"
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace iiSocietyHelper::Sync;
static void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

int main() {
    ManualStatusExecutor executor;
    auto cancelledWake = executor.reserve([] {});
    std::weak_ptr<StatusWake> retired = cancelledWake;
    check(cancelledWake && cancelledWake->signal(), "host wake could not be scheduled");
    cancelledWake->cancel(); cancelledWake.reset();
    check(retired.expired(), "cancelled wake remained in the host queue");
    int hostWakes = 0, deliveries = 0;
    ManualStatusExecutor coalescing([&] { ++hostWakes; });
    auto reserved = coalescing.reserve([&] { ++deliveries; });
    for (int i = 0; i < 100; ++i) check(reserved->signal(), "reserved wake failed");
    check(hostWakes == 1, "a pending reservation scheduled redundant host wakes");
    coalescing.drain(); check(deliveries == 1, "coalesced wake did not deliver");
    check(reserved->signal() && hostWakes == 2, "reservation did not rearm after delivery");
    reserved->cancel();
    int attempts = 0;
    ManualStatusExecutor recovering([&] { if (++attempts == 1) throw std::runtime_error("wake unavailable"); });
    auto retried = recovering.reserve([&] { ++deliveries; });
    check(!retried->signal() && retried->signal(), "a failed host wake could not be retried");
    recovering.drain(); check(deliveries == 2, "retried host wake was lost"); retried->cancel();
    std::promise<void> failedWakeEntered, failedWakeRelease;
    auto failedWakeStarted = failedWakeEntered.get_future(), failedWakeContinue = failedWakeRelease.get_future();
    std::atomic_bool failWake{true};
    ManualStatusExecutor concurrentFailure([&] {
        if (failWake) { failedWakeEntered.set_value(); failedWakeContinue.wait(); throw std::runtime_error("wake failed"); }
    });
    auto uncertain = concurrentFailure.reserve([&] { ++deliveries; });
    auto firstSignal = std::async(std::launch::async, [&] { return uncertain->signal(); });
    failedWakeStarted.wait();
    const bool falselyAccepted = uncertain->signal();
    failedWakeRelease.set_value(); const bool firstAccepted = firstSignal.get();
    failWake = false;
    const bool recovered = uncertain->signal(); concurrentFailure.drain(); uncertain->cancel();
    check(!falselyAccepted && !firstAccepted && recovered && deliveries == 3,
          "an in-flight failed wake was incorrectly acknowledged or lost");
    std::promise<void> wakeEntered, wakeRelease;
    auto wakeStarted = wakeEntered.get_future(), wakeContinue = wakeRelease.get_future();
    ManualStatusExecutor wakeBarrier([&] { wakeEntered.set_value(); wakeContinue.wait(); });
    auto pendingWake = wakeBarrier.reserve([] {});
    auto signalling = std::async(std::launch::async, [&] { return pendingWake->signal(); });
    wakeStarted.wait();
    auto cancellingWake = std::async(std::launch::async, [&] { pendingWake->cancel(); });
    const bool wakeWaited = cancellingWake.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    wakeRelease.set_value(); cancellingWake.get(); const bool acceptedWake = signalling.get();
    check(wakeWaited && acceptedWake, "cancellation did not wait for an active host wake");
    std::shared_ptr<StatusWake> selfWake;
    ManualStatusExecutor selfCancellingWake([&] { selfWake->cancel(); });
    selfWake = selfCancellingWake.reserve([] {});
    check(selfWake->signal() && !selfWake->signal(), "self-cancelled wake was not retired");
    StatusSource source;
    std::vector<StatusEvent> events;
    const auto owner = std::this_thread::get_id();
    bool onOwner = true;
    auto watch = source.status().subscribe(executor, [&](StatusEvent event) {
        onOwner = onOwner && std::this_thread::get_id() == owner; events.push_back(event);
    }, 2);
    check(bool(watch) && source.status().snapshot().value == 0, "subscription or initial query failed");
    std::thread producer([&] { source.publish(StatusCode::InProgress); }); producer.join();
    check(events.empty() && source.status().snapshot().value == 1, "change was not queued or query was stale");
    executor.drain();
    check(onOwner && events.size() == 1 && events[0].value == 1 && events[0].revision == 1 && !events[0].gap,
          "change did not reach the selected executor");

    executor.setAvailable(false);
    source.publish(StatusCode::Complete);
    check(events.size() == 1 && source.status().snapshot().value == 2 && !watch.resume(), "failed wake lost its pending result");
    executor.setAvailable(true); check(watch.resume(), "pending result could not be resumed"); executor.drain();
    check(events.size() == 2 && events.back().value == 2, "final pending result was lost");

    source.publish(StatusCode::Before); source.publish(StatusCode::InProgress); source.publish(StatusCode::Cancelled);
    executor.drain();
    const auto latest = source.status().snapshot();
    check(events.size() > 2 && events.back().gap && events.back().revision == latest.revision && events.back().value == latest.value,
          "bounded overflow did not deliver a recoverable current state");
    const auto count = events.size();
    source.publish(StatusCode::Failed); watch.close(); executor.drain();
    check(events.size() == count && !watch, "closed subscription delivered a queued callback");

    auto shortSource = std::make_unique<StatusSource>();
    int afterClose = 0;
    auto survivor = shortSource->status().subscribe(executor, [&](StatusEvent) { ++afterClose; });
    shortSource->publish(StatusCode::InProgress); shortSource.reset(); executor.drain();
    check(afterClose == 0 && !survivor, "producer destruction did not retire queued delivery");

    StatusSource barrierSource;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(), releaseFuture = release.get_future();
    auto barrier = barrierSource.status().subscribe(executor, [&](StatusEvent) { entered.set_value(); releaseFuture.wait(); });
    barrierSource.publish(StatusCode::InProgress);
    auto delivering = std::async(std::launch::async, [&] { executor.drain(); });
    enteredFuture.wait();
    auto closing = std::async(std::launch::async, [&] { barrier.close(); });
    const bool waited = closing.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    auto alsoClosing = std::async(std::launch::async, [&] { barrier.close(); });
    const bool bothWaited = alsoClosing.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    release.set_value(); closing.get(); alsoClosing.get(); delivering.get();
    check(waited && bothWaited && !barrier, "concurrent close did not wait for an active callback");

    auto selfClosingSource = std::make_unique<StatusSource>();
    std::promise<void> selfClosed, releaseSelf;
    auto selfClosedFuture = selfClosed.get_future(), selfRelease = releaseSelf.get_future();
    StatusSubscription selfClosing;
    selfClosing = selfClosingSource->status().subscribe(executor, [&](StatusEvent) {
        selfClosing.close(); selfClosed.set_value(); selfRelease.wait();
    });
    selfClosingSource->publish(StatusCode::InProgress);
    auto selfDelivering = std::async(std::launch::async, [&] { executor.drain(); });
    selfClosedFuture.wait();
    auto destroying = std::async(std::launch::async, [&] { selfClosingSource.reset(); });
    const bool destroyWaited = destroying.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    releaseSelf.set_value(); destroying.get(); selfDelivering.get();
    check(destroyWaited, "producer did not wait for a self-closed callback");

    auto reentrantSource = std::make_unique<StatusSource>();
    auto reentrant = reentrantSource->status().subscribe(executor, [&](StatusEvent) { reentrantSource.reset(); });
    reentrantSource->publish(StatusCode::InProgress); executor.drain();
    check(!reentrant, "producer destruction from its own callback did not retire delivery");

    StatusSource failingSource;
    auto failing = failingSource.status().subscribe(executor, [](StatusEvent) { throw std::runtime_error("observer failed"); });
    failingSource.publish(StatusCode::InProgress); executor.drain();
    check(!failing && bool(failing.failure()), "observer failure was not retained");

    StatusSource serialSource;
    std::promise<void> firstEntered, releaseFirst;
    auto firstStarted = firstEntered.get_future(), firstRelease = releaseFirst.get_future();
    std::atomic_int calls{0}; std::atomic_bool released{false}, overlap{false};
    auto serial = serialSource.status().subscribe(executor, [&](StatusEvent) {
        if (++calls == 1) { firstEntered.set_value(); firstRelease.wait(); }
        else if (!released.load()) overlap = true;
    });
    serialSource.publish(StatusCode::InProgress);
    auto firstDelivery = std::async(std::launch::async, [&] { executor.drain(); });
    firstStarted.wait();
    serialSource.publish(StatusCode::Complete);
    auto secondDelivery = std::async(std::launch::async, [&] { executor.drain(); });
    const auto secondFinished = secondDelivery.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    released = true; releaseFirst.set_value();
    secondDelivery.get(); firstDelivery.get();
    check(secondFinished && calls == 2 && !overlap, "one subscription delivered callbacks concurrently");
}
