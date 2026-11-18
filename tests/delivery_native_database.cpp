#include "CTestSupport.h"
#include "TestWorkspace.h"
#include "DeliveryStore.h"
#include <iiFileProviderDatabase.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <filesystem>
#include <memory>

namespace sql = iiFileProvider::native::sql;
using iiSocietyHelper::DeliveryStore;
using iiSocietyHelper::Peer;

static Peer peer() {
    Peer value;
    value.application = {"com.iisacc.fixture", "Fixture", "1"};
    value.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return value;
}
static std::filesystem::path databasePath(const QTemporaryDir& root) {
    return std::filesystem::path(root.filePath("delivery/delivery.sqlite").toStdU16String());
}
static void roundTrip() {
    QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/native-delivery-XXXXXX");
    CTEST_CHECK(root.isValid());
    DeliveryStore store;
    QString error;
    CTEST_CHECK_MESSAGE(store.open(root.path(), &error), qPrintable(error));
    const auto sender = peer();
    const auto first = store.enqueue(sender, "fixture.first", {{"text", QString::fromUtf8("안녕")}, {"n", 2}});
    const auto second = store.enqueue(sender, "fixture.second", {{"ok", true}});
    CTEST_CHECK(!first.isEmpty() && !second.isEmpty() && first != second);
    CTEST_COMPARE(store.pendingCount(), 2);
    CTEST_COMPARE(store.receivePending(1), 1);
    CTEST_COMPARE(store.pendingCount(), 1);
    auto rows = store.readAfter(0);
    CTEST_COMPARE(rows.size(), 1);
    CTEST_COMPARE(rows[0].toMap().value("id").toString(), first);
    CTEST_COMPARE(rows[0].toMap().value("sender").toMap().value("applicationId").toString(), sender.application.id);
    CTEST_COMPARE(rows[0].toMap().value("payload").toMap().value("text").toString(), QString::fromUtf8("안녕"));
    const auto sequence = rows[0].toMap().value("sequence").toLongLong();
    CTEST_CHECK(!store.acknowledge("fixture", sequence + 1));
    CTEST_CHECK(store.acknowledge("fixture", sequence));
    CTEST_CHECK(store.acknowledge("fixture", 0));
    CTEST_COMPARE(store.acknowledged("fixture"), sequence);
    CTEST_CHECK(store.setDaemonSnapshot({{"marker", "native"}}));
    store.close();
    CTEST_CHECK_MESSAGE(store.open(root.path(), &error), qPrintable(error));
    CTEST_COMPARE(store.daemonSnapshot().value("marker").toString(), QString("native"));
    CTEST_COMPARE(store.acknowledged("fixture"), sequence);
    CTEST_COMPARE(store.receivePending(), 1);
    rows = store.readAfter(sequence);
    CTEST_COMPARE(rows.size(), 1);
    CTEST_COMPARE(rows[0].toMap().value("id").toString(), second);
}
static void failedBatchRollsBack() {
    QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/native-rollback-XXXXXX");
    CTEST_CHECK(root.isValid());
    DeliveryStore store;
    CTEST_CHECK(store.open(root.path()));
    const auto sender = peer();
    CTEST_CHECK(!store.enqueue(sender, "fixture.first", {}).isEmpty());
    CTEST_CHECK(!store.enqueue(sender, "fixture.fail", {}).isEmpty());
    auto native = sql::Database::open(databasePath(root));
    CTEST_CHECK(native);
    CTEST_CHECK(native->execute("CREATE TRIGGER fail_receipt BEFORE INSERT ON inbox WHEN NEW.topic='fixture.fail' BEGIN SELECT RAISE(ABORT, 'fixture failure'); END"));
    CTEST_COMPARE(store.receivePending(), -1);
    CTEST_CHECK(!store.errorString().isEmpty());
    CTEST_COMPARE(store.pendingCount(), 2);
    CTEST_CHECK(store.readAfter(0).isEmpty());
    CTEST_CHECK(native->execute("DROP TRIGGER fail_receipt"));
    CTEST_COMPARE(store.receivePending(), 2);
    CTEST_COMPARE(store.pendingCount(), 0);
    CTEST_COMPARE(store.readAfter(0).size(), 2);
}
static void rejectsFutureSchema() {
    QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/native-schema-XXXXXX");
    CTEST_CHECK(root.isValid());
    DeliveryStore initial; CTEST_CHECK(initial.open(root.path())); initial.close();
    {
        auto native = sql::Database::open(databasePath(root));
        CTEST_CHECK(native);
        CTEST_CHECK(native->execute("PRAGMA user_version=2"));
    }
    DeliveryStore reader; QString error;
    CTEST_CHECK(!reader.open(root.path(), &error));
    CTEST_COMPARE(error, QString("Unsupported Society delivery database schema."));
}
static void rejectsRedirectedDatabase() {
#ifdef Q_OS_UNIX
    QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/native-symlink-XXXXXX");
    CTEST_CHECK(root.isValid());
    const auto outside = root.filePath("outside");
    QFile sentinel(outside);
    CTEST_CHECK(sentinel.open(QIODevice::WriteOnly));
    CTEST_COMPARE(sentinel.write("untouched"), qint64(9));
    sentinel.close();
    CTEST_CHECK(QDir().mkpath(root.filePath("delivery")));
    const auto path = root.filePath("delivery/delivery.sqlite");
    CTEST_CHECK(QFile::link(outside, path));
    CTEST_CHECK(QFileInfo(path).isSymLink());
    DeliveryStore store;
    CTEST_CHECK(!store.open(root.path()));
    CTEST_CHECK(!store.errorString().isEmpty());
    CTEST_CHECK(sentinel.open(QIODevice::ReadOnly));
    CTEST_COMPARE(sentinel.readAll(), QByteArray("untouched"));
#else
    helper_ctest::skip("This fixture requires Unix symbolic links");
#endif
}
static void transferBetweenThreads() {
    QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/native-thread-XXXXXX");
    CTEST_CHECK(root.isValid());
    DeliveryStore store; CTEST_CHECK(store.open(root.path()));
    auto* owner = QThread::currentThread();
    bool workerOk = false;
    auto worker = std::unique_ptr<QThread>(QThread::create([&] {
        workerOk = store.isOpen() && store.pendingCount() == 0 && store.moveToThread(owner);
    }));
    CTEST_CHECK(store.moveToThread(worker.get()));
    CTEST_COMPARE(store.pendingCount(), -1);
    CTEST_CHECK(!store.errorString().isEmpty());
    worker->start();
    const bool finished = worker->wait(15000);
    if (!finished) worker->wait(); // Do not destroy a running QThread on failure.
    CTEST_CHECK(finished);
    CTEST_CHECK(workerOk);
    CTEST_COMPARE(store.pendingCount(), 0);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    runner.run("roundTrip", roundTrip);
    runner.run("failedBatchRollsBack", failedBatchRollsBack);
    runner.run("rejectsFutureSchema", rejectsFutureSchema);
    runner.run("rejectsRedirectedDatabase", rejectsRedirectedDatabase);
    runner.run("transferBetweenThreads", transferBetweenThreads);
    return runner.result();
}
