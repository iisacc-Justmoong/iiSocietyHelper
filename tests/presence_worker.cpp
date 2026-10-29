#include "../src/Discovery/PresenceWorker_p.h"
#include <QElapsedTimer>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <future>

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
};

QTEST_GUILESS_MAIN(PresenceWorkerTests)
#include "presence_worker.moc"
