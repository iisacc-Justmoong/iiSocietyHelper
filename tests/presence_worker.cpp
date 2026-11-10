#include "../src/Discovery/PresenceWorker_p.h"
#include "../src/Files/CallbackGate_p.h"
#include <QElapsedTimer>
#include <QTest>
#include <QTimer>
#include <QScopeGuard>
#include <atomic>
#include <future>
#include <semaphore>

class PresenceWorkerTests : public QObject {
    Q_OBJECT
private slots:
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
        connect(&input, &QTimer::timeout, this, [&] { ++ticks; });
        input.start();
        QTest::qWait(100);
        const bool applicationResponded = ticks >= 3 && !finished;
        release.set_value();
        worker.clearAndWait();
        QVERIFY(applicationResponded);
        QVERIFY(finished);
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
        QCOMPARE(completed, std::future_status::ready);
        QCOMPARE(operations.load(), 2);
        QCOMPARE(value.load(), 99);
    }
    void drainPreservesAQueuedCommit() {
        iiSocietyHelper::detail::PresenceWorker worker;
        std::atomic_int committed = 0;
        worker.post([&] { ++committed; });
        worker.drain();
        QCOMPARE(committed.load(), 1);
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
        QCOMPARE(finished.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
        QCoreApplication::processEvents();
        QVERIFY(elapsed < 1000);
        QVERIFY(!called.load());
        QVERIFY(!pendingRan->load());
    }
    void queuedCallbackIsDiscardedWhenGateCloses() {
        QObject owner;
        auto callbacks = std::make_shared<iiSocietyHelper::detail::CallbackGate>(&owner);
        bool called = false;
        callbacks->post([&] { called = true; });
        callbacks->close();
        QCoreApplication::processEvents();
        QVERIFY(!called);
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
        QTRY_VERIFY(entered->load());worker.reset();
        auto quiesced=std::async(std::launch::async, [] { iiSocietyHelper::detail::PresenceWorker::quiesceDetachedWork(); });
        bool heartbeat=false;QTimer::singleShot(0,this,[&]{heartbeat=true;});QTest::qWait(30);
        const bool waited=quiesced.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout;
        released=true;gate->release();
        const bool completed=quiesced.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        QVERIFY(heartbeat);QVERIFY(waited);QVERIFY(completed);
        QVERIFY(finished->load());
        std::atomic_bool ran=false;
        { iiSocietyHelper::detail::PresenceWorker late(iiSocietyHelper::detail::PresenceWorker::Shutdown::detachActive);
          late.post([&]{ran=true;}); }
        QTest::qWait(20);QVERIFY(!ran.load());
    }
};

QTEST_GUILESS_MAIN(PresenceWorkerTests)
#include "presence_worker.moc"
