#include "../src/Discovery/PresenceWorker_p.h"
#include "../src/Files/CallbackGate_p.h"
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QScopeGuard>
#include <atomic>
#include <future>
#include <semaphore>

namespace {
void processFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeoutMilliseconds) return false;
        processFor(10);
    }
    return true;
}
}

class PresenceWorkerTests final {
public:
    void slowIoDoesNotBlockTheApplicationLoop() {
        iiSocietyHelper::detail::PresenceWorker worker;
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        std::atomic_bool finished = false;
        worker.post([&] { started.set_value(); gate.wait(); finished = true; });
        started.get_future().wait();
        int ticks = 0;
        QTimer input;
        input.setInterval(5);
        QObject::connect(&input, &QTimer::timeout, &input, [&] { ++ticks; });
        input.start();
        processFor(100);
        const bool applicationResponded = ticks >= 3 && !finished;
        release.set_value();
        worker.clearAndWait();
        CTEST_CHECK(applicationResponded);
        CTEST_CHECK(finished);
    }
    void pendingUpdatesAreBoundedAndKeepTheLatest() {
        iiSocietyHelper::detail::PresenceWorker worker;
        std::promise<void> started, release, complete;
        auto gate = release.get_future().share();
        std::atomic_int operations = 0, value = -1;
        worker.post([&] { ++operations; started.set_value(); gate.wait(); });
        started.get_future().wait();
        for (int i = 0; i < 100; ++i)
            worker.post([&, i] { ++operations; value = i; if (i == 99) complete.set_value(); });
        release.set_value();
        const auto completed = complete.get_future().wait_for(std::chrono::seconds(5));
        worker.clearAndWait();
        CTEST_COMPARE(completed, std::future_status::ready);
        CTEST_COMPARE(operations.load(), 2);
        CTEST_COMPARE(value.load(), 99);
    }
    void drainPreservesAQueuedCommit() {
        iiSocietyHelper::detail::PresenceWorker worker;
        std::atomic_int committed = 0;
        worker.post([&] { ++committed; });
        worker.drain();
        CTEST_COMPARE(committed.load(), 1);
    }
    void closingDuringBlockedIoDoesNotWaitOrCallDestroyedOwner() {
        auto owner = std::make_unique<QObject>();
        auto callbacks = std::make_shared<iiSocietyHelper::detail::CallbackGate>(owner.get());
        auto worker = std::make_unique<iiSocietyHelper::detail::PresenceWorker>(
            iiSocietyHelper::detail::PresenceWorker::Shutdown::detachActive);
        std::promise<void> started, release, finished;
        auto gate = release.get_future().share();
        std::atomic_bool called = false;
        worker->post([callbacks, gate, &started, &finished, &called] {
            started.set_value(); gate.wait();
            callbacks->post([&called] { called = true; });
            finished.set_value();
        });
        started.get_future().wait();
        auto pendingRan = std::make_shared<std::atomic_bool>(false);
        worker->post([pendingRan] { *pendingRan = true; });
        callbacks->close();
        QElapsedTimer timer; timer.start();
        worker.reset(); owner.reset();
        const auto elapsed = timer.elapsed();
        release.set_value();
        CTEST_COMPARE(finished.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
        QCoreApplication::processEvents();
        CTEST_CHECK(elapsed < 1000);
        CTEST_CHECK(!called.load());
        CTEST_CHECK(!pendingRan->load());
    }
    void queuedCallbackIsDiscardedWhenGateCloses() {
        QObject owner;
        auto callbacks = std::make_shared<iiSocietyHelper::detail::CallbackGate>(&owner);
        bool called = false;
        callbacks->post([&] { called = true; });
        callbacks->close();
        QCoreApplication::processEvents();
        CTEST_CHECK(!called);
    }
    // This irreversible barrier is deliberately last in this test process.
    void detachedWorkQuiescesBeforeStaticDestruction() {
        auto worker=std::make_unique<iiSocietyHelper::detail::PresenceWorker>(
            iiSocietyHelper::detail::PresenceWorker::Shutdown::detachActive);
        auto gate=std::make_shared<std::binary_semaphore>(0);
        auto entered=std::make_shared<std::atomic_bool>(false);
        auto finished=std::make_shared<std::atomic_bool>(false);
        bool released=false;const auto release=qScopeGuard([&]{if(!released)gate->release();});
        worker->post([gate,entered,finished]{*entered=true;gate->acquire();*finished=true;});
        CTEST_CHECK(waitUntil([&] { return entered->load(); }, 5000));worker.reset();
        auto quiesced=std::async(std::launch::async, [] { iiSocietyHelper::detail::PresenceWorker::quiesceDetachedWork(); });
        QObject heartbeatContext;
        bool heartbeat=false;QTimer::singleShot(0,&heartbeatContext,[&]{heartbeat=true;});processFor(30);
        const bool waited=quiesced.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout;
        released=true;gate->release();
        const bool completed=quiesced.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        CTEST_CHECK(heartbeat);CTEST_CHECK(waited);CTEST_CHECK(completed);
        CTEST_CHECK(finished->load());
        std::atomic_bool ran=false;
        { iiSocietyHelper::detail::PresenceWorker late(iiSocietyHelper::detail::PresenceWorker::Shutdown::detachActive);
          late.post([&]{ran=true;}); }
        processFor(20);CTEST_CHECK(!ran.load());
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    PresenceWorkerTests tests;
    runner.run("slowIoDoesNotBlockTheApplicationLoop", [&] { tests.slowIoDoesNotBlockTheApplicationLoop(); });
    runner.run("pendingUpdatesAreBoundedAndKeepTheLatest", [&] { tests.pendingUpdatesAreBoundedAndKeepTheLatest(); });
    runner.run("drainPreservesAQueuedCommit", [&] { tests.drainPreservesAQueuedCommit(); });
    runner.run("closingDuringBlockedIoDoesNotWaitOrCallDestroyedOwner", [&] { tests.closingDuringBlockedIoDoesNotWaitOrCallDestroyedOwner(); });
    runner.run("queuedCallbackIsDiscardedWhenGateCloses", [&] { tests.queuedCallbackIsDiscardedWhenGateCloses(); });
    runner.run("detachedWorkQuiescesBeforeStaticDestruction", [&] { tests.detachedWorkQuiescesBeforeStaticDestruction(); });
    return runner.result();
}
