#include "TestWorkspace.h"
#include "DeliveryStore.h"
#include "iiSocietyHelper.h"
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include "CTestSupport.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <SocietyDrive.h>
#include <QDir>
#include <iiFileProviderDevice.h>
#include <QElapsedTimer>
#include <iostream>

using namespace iiSocietyHelper;

class DeliveryTests final {
public:
    void initializedStoreDoesNotWaitForAnotherInitializationLease()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/ready-delivery-XXXXXX")));
        DeliveryStore initial;
        CTEST_CHECK(initial.open(root.path()));
        const QVariantMap marker{{"instanceId", "existing-daemon"}, {"sequence", 42}};
        CTEST_CHECK(initial.setDaemonSnapshot(marker));
        initial.close();
        iiFileProvider::LockFile initializer(root.filePath("delivery/initialization.lock"));
        initializer.setStaleLockTime(0);
        CTEST_CHECK(initializer.tryLock(0));
        DeliveryStore reader;
        QElapsedTimer timer; timer.start();
        QString error;
        CTEST_CHECK_MESSAGE(reader.open(root.path(), &error), qPrintable(error));
        qInfo() << "ready_delivery_open_ms=" << timer.elapsed();
        CTEST_COMPARE(reader.daemonSnapshot(), marker);
        CTEST_COMPARE(reader.pendingCount(), 0);
    }
    void legacySqliteAndNativeDeliveryRemainInteroperable()
    {
        QTemporaryDir root(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/delivery-interop-XXXXXX");
        CTEST_CHECK(root.isValid());
        DeliveryStore initial; CTEST_CHECK(initial.open(root.path())); initial.close();
        Peer sender; sender.application = {"com.iisacc.fixture", "Fixture", "1"};
        sender.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto legacyId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root.filePath("delivery/delivery.sqlite"));
            CTEST_CHECK(database.open());
            {
                QSqlQuery query(database);
                CTEST_CHECK(query.prepare("INSERT INTO outbox (id,sender,topic,payload,queued_at) VALUES (?,?,?,?,?)"));
                query.addBindValue(legacyId);
                query.addBindValue(QJsonDocument(QJsonObject::fromVariantMap(sender.toVariantMap())).toJson(QJsonDocument::Compact));
                query.addBindValue("legacy.message");
                query.addBindValue(QByteArray("{\"source\":\"qt\"}"));
                query.addBindValue("2026-01-01T00:00:00.000Z");
                CTEST_CHECK(query.exec());
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        DeliveryStore store; CTEST_CHECK(store.open(root.path()));
        CTEST_COMPARE(store.receivePending(), 1);
        const auto received = store.readAfter(0);
        CTEST_COMPARE(received.size(), 1);
        CTEST_COMPARE(received[0].toMap().value("id").toString(), legacyId);
        CTEST_COMPARE(received[0].toMap().value("payload").toMap().value("source").toString(), QString("qt"));
        const auto nativeId = store.enqueue(sender, "native.message", {{"source", "native"}});
        CTEST_CHECK(!nativeId.isEmpty()); store.close();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root.filePath("delivery/delivery.sqlite"));
            CTEST_CHECK(database.open());
            {
                QSqlQuery query(database);
                CTEST_CHECK(query.prepare("SELECT payload,typeof(payload) FROM outbox WHERE id=?"));
                query.addBindValue(nativeId); CTEST_CHECK(query.exec()); CTEST_CHECK(query.next());
                CTEST_COMPARE(query.value(1).toString(), QString("blob"));
                CTEST_COMPARE(QJsonDocument::fromJson(query.value(0).toByteArray()).object().value("source").toString(), QString("native"));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void firstSchemaCreationStillRequiresItsInitializationLease()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/new-delivery-XXXXXX")));
        CTEST_CHECK(QDir().mkpath(root.filePath("delivery")));
        iiFileProvider::LockFile initializer(root.filePath("delivery/initialization.lock"));
        initializer.setStaleLockTime(0); CTEST_CHECK(initializer.tryLock(0));
        DeliveryStore store;
        CTEST_CHECK(!store.open(root.path()));
        CTEST_CHECK(!QFileInfo::exists(root.filePath("delivery/delivery.sqlite")));
        initializer.unlock();
        CTEST_CHECK(store.open(root.path()));
    }
    void unsupportedSchemaIsNotAcceptedByReadyOpen()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/unsupported-delivery-XXXXXX")));
        DeliveryStore initial; CTEST_CHECK(initial.open(root.path())); initial.close();
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root.filePath("delivery/delivery.sqlite"));
            CTEST_CHECK(database.open());
            { QSqlQuery query(database); CTEST_CHECK(query.exec("PRAGMA user_version=2")); }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        iiFileProvider::LockFile initializer(root.filePath("delivery/initialization.lock"));
        initializer.setStaleLockTime(0); CTEST_CHECK(initializer.tryLock(0));
        DeliveryStore reader; QString error;
        CTEST_CHECK(!reader.open(root.path(), &error));
        CTEST_COMPARE(error, QString("Unsupported Society delivery database schema."));
    }
    void localMessagesNeverLiveInsideAReplicatedDrive()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/local-boundary-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        for (const auto &path : {root.path(), root.filePath("Files/LocalMessages"), root.filePath("Models/LocalMessages")}) {
            Helper helper;
            CTEST_CHECK(!helper.start({"com.iisacc.test", "Test", "1"}, {path, 50, 400}));
            DeliveryStore store;
            CTEST_CHECK(!store.open(path));
            CTEST_CHECK(!QFileInfo::exists(QDir(path).filePath("delivery")));
        }
        const auto privateDirectory = root.filePath(".society-runtime/helpers/v1");
        Helper privateHelper;
        CTEST_CHECK(privateHelper.start({"com.iisacc.test", "Test", "1"}, {privateDirectory, 50, 400}));
        DeliveryStore privateStore; CTEST_CHECK(privateStore.open(privateDirectory));
        CTEST_CHECK(privateStore.receivePending() > 0);
    }
    void queueSurvivesSenderAndReceiverRestarts()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/delivery-XXXXXX")));
        QString messageId;
        {
            Helper helper;
            CTEST_CHECK(helper.start({"com.iisacc.dreamscapes", "Dreamscapes", "1"}, {root.path(), 50, 400}));
            messageId = helper.sendData("generation.queued", {{"prompt", "a forest"}, {"queueId", "job-42"}});
            CTEST_CHECK(!messageId.isEmpty());
        }
        DeliveryStore receiver;
        CTEST_CHECK(receiver.open(root.path()));
        CTEST_CHECK(receiver.readAfter(0).isEmpty()); // Nothing has been accepted by the daemon yet.
        CTEST_CHECK(receiver.receivePending() >= 3); // Started, application data, stopped.
        const auto messages = receiver.readAfter(0);
        QVariantMap received;
        for (const auto &entry : messages) if (entry.toMap().value("id") == messageId) received = entry.toMap();
        CTEST_CHECK(!received.isEmpty());
        CTEST_COMPARE(received.value("topic"), "generation.queued");
        CTEST_COMPARE(received.value("payload").toMap().value("queueId"), "job-42");
        CTEST_COMPARE(received.value("sender").toMap().value("applicationId"), "com.iisacc.dreamscapes");
        const auto cursor = messages.last().toMap().value("sequence").toLongLong();
        CTEST_CHECK(receiver.acknowledge("com.iisacc.society", cursor));
        receiver.close();
        CTEST_CHECK(receiver.open(root.path()));
        CTEST_COMPARE(receiver.receivePending(), 0);
        CTEST_COMPARE(receiver.readAfter(0).size(), messages.size()); // No duplicate receipt.
        CTEST_COMPARE(receiver.acknowledged("com.iisacc.society"), cursor);
        CTEST_CHECK(receiver.readAfter(cursor).isEmpty());
    }

    void runningLocalQueueStopsIfItsDirectoryBecomesReplicated()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/late-boundary-XXXXXX.societycontainer")));
        Helper helper;
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        CTEST_CHECK(helper.sendData("must.stay.local", {}).isEmpty());
        CTEST_COMPARE(store.receivePending(), -1);
        CTEST_CHECK(!store.errorString().isEmpty());
    }

    void acknowledgementsNeverSkipUnreceivedOrGoBackwards()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/cursor-XXXXXX")));
        Helper helper;
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        CTEST_CHECK(store.receivePending() > 0);
        const auto cursor = store.readAfter(0).last().toMap().value("sequence").toLongLong();
        CTEST_CHECK(!store.acknowledge("com.iisacc.society", cursor + 100));
        CTEST_CHECK(store.acknowledge("com.iisacc.society", cursor));
        CTEST_CHECK(store.acknowledge("com.iisacc.society", 0));
        CTEST_COMPARE(store.acknowledged("com.iisacc.society"), cursor);
        CTEST_CHECK(!helper.sendData("", {}).size());
        CTEST_CHECK(helper.sendData("large", {{"text", QString(70000, 'x')}}).isEmpty());
    }

    void readsArePagedAndSnapshotsSurviveRestart()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/pages-XXXXXX")));
        Helper helper;
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        for (int i = 0; i < 5; ++i) CTEST_CHECK(!helper.sendData("test.data", {{"index", i}}).isEmpty());
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        CTEST_COMPARE(store.receivePending(2), 2);
        CTEST_COMPARE(store.readAfter(0).size(), 2);
        CTEST_CHECK(store.receivePending() > 0);
        auto first = store.readAfter(0, 2);
        CTEST_COMPARE(first.size(), 2);
        auto rest = store.readAfter(first.last().toMap().value("sequence").toLongLong());
        CTEST_COMPARE(rest.size(), 4);
        CTEST_CHECK(store.setDaemonSnapshot({{"instanceId", "test-daemon"}, {"peers", QVariantList{helper.self().toVariantMap()}}}));
        store.close();
        CTEST_CHECK(store.open(root.path()));
        CTEST_COMPARE(store.daemonSnapshot().value("instanceId"), "test-daemon");
    }

    void failedReceiptKeepsTheWholeOutboxBatch()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/rollback-XXXXXX")));
        Helper helper;
        CTEST_CHECK(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        CTEST_CHECK(!helper.sendData("force.failure", {{"retained", true}}).isEmpty());
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root.filePath("delivery/delivery.sqlite"));
            CTEST_CHECK(database.open());
            {
                QSqlQuery query(database);
                CTEST_CHECK(query.exec("CREATE TRIGGER fail_receipt BEFORE INSERT ON inbox WHEN NEW.topic='force.failure' BEGIN SELECT RAISE(ABORT, 'simulated storage failure'); END"));
                CTEST_COMPARE(store.receivePending(), -1);
                CTEST_CHECK(!store.errorString().isEmpty());
                CTEST_COMPARE(store.pendingCount(), 2);
                CTEST_CHECK(store.readAfter(0).isEmpty());
                CTEST_CHECK(query.exec("DROP TRIGGER fail_receipt"));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        CTEST_COMPARE(store.receivePending(), 2);
        CTEST_COMPARE(store.pendingCount(), 0);
        CTEST_COMPARE(store.readAfter(0).size(), 2);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    DeliveryTests tests;
    runner.run("initializedStoreDoesNotWaitForAnotherInitializationLease", [&] { std::cout << "running initializedStoreDoesNotWaitForAnotherInitializationLease\n" << std::flush; tests.initializedStoreDoesNotWaitForAnotherInitializationLease(); });
    runner.run("legacySqliteAndNativeDeliveryRemainInteroperable", [&] { tests.legacySqliteAndNativeDeliveryRemainInteroperable(); });
    runner.run("firstSchemaCreationStillRequiresItsInitializationLease", [&] { std::cout << "running firstSchemaCreationStillRequiresItsInitializationLease\n" << std::flush; tests.firstSchemaCreationStillRequiresItsInitializationLease(); });
    runner.run("unsupportedSchemaIsNotAcceptedByReadyOpen", [&] { std::cout << "running unsupportedSchemaIsNotAcceptedByReadyOpen\n" << std::flush; tests.unsupportedSchemaIsNotAcceptedByReadyOpen(); });
    runner.run("localMessagesNeverLiveInsideAReplicatedDrive", [&] { std::cout << "running localMessagesNeverLiveInsideAReplicatedDrive\n" << std::flush; tests.localMessagesNeverLiveInsideAReplicatedDrive(); });
    runner.run("queueSurvivesSenderAndReceiverRestarts", [&] { std::cout << "running queueSurvivesSenderAndReceiverRestarts\n" << std::flush; tests.queueSurvivesSenderAndReceiverRestarts(); });
    runner.run("runningLocalQueueStopsIfItsDirectoryBecomesReplicated", [&] { std::cout << "running runningLocalQueueStopsIfItsDirectoryBecomesReplicated\n" << std::flush; tests.runningLocalQueueStopsIfItsDirectoryBecomesReplicated(); });
    runner.run("acknowledgementsNeverSkipUnreceivedOrGoBackwards", [&] { std::cout << "running acknowledgementsNeverSkipUnreceivedOrGoBackwards\n" << std::flush; tests.acknowledgementsNeverSkipUnreceivedOrGoBackwards(); });
    runner.run("readsArePagedAndSnapshotsSurviveRestart", [&] { std::cout << "running readsArePagedAndSnapshotsSurviveRestart\n" << std::flush; tests.readsArePagedAndSnapshotsSurviveRestart(); });
    runner.run("failedReceiptKeepsTheWholeOutboxBatch", [&] { std::cout << "running failedReceiptKeepsTheWholeOutboxBatch\n" << std::flush; tests.failedReceiptKeepsTheWholeOutboxBatch(); });
    return runner.result();
}
