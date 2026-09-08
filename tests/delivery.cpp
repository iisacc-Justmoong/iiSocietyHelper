#include "DeliveryStore.h"
#include "iiSocietyHelper.h"
#include <QTemporaryDir>
#include <QTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

using namespace iiSocietyHelper;

class DeliveryTests : public QObject {
    Q_OBJECT
private slots:
    void queueSurvivesSenderAndReceiverRestarts()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/delivery-XXXXXX");
        QString messageId;
        {
            Helper helper;
            QVERIFY(helper.start({"com.iisacc.dreamscapes", "Dreamscapes", "1"}, {root.path(), 50, 400}));
            messageId = helper.sendData("generation.queued", {{"prompt", "a forest"}, {"queueId", "job-42"}});
            QVERIFY(!messageId.isEmpty());
        }
        DeliveryStore receiver;
        QVERIFY(receiver.open(root.path()));
        QVERIFY(receiver.readAfter(0).isEmpty()); // Nothing has been accepted by the daemon yet.
        QVERIFY(receiver.receivePending() >= 3); // Started, application data, stopped.
        const auto messages = receiver.readAfter(0);
        QVariantMap received;
        for (const auto &entry : messages) if (entry.toMap().value("id") == messageId) received = entry.toMap();
        QVERIFY(!received.isEmpty());
        QCOMPARE(received.value("topic"), "generation.queued");
        QCOMPARE(received.value("payload").toMap().value("queueId"), "job-42");
        QCOMPARE(received.value("sender").toMap().value("applicationId"), "com.iisacc.dreamscapes");
        const auto cursor = messages.last().toMap().value("sequence").toLongLong();
        QVERIFY(receiver.acknowledge("com.iisacc.society", cursor));
        receiver.close();
        QVERIFY(receiver.open(root.path()));
        QCOMPARE(receiver.receivePending(), 0);
        QCOMPARE(receiver.readAfter(0).size(), messages.size()); // No duplicate receipt.
        QCOMPARE(receiver.acknowledged("com.iisacc.society"), cursor);
        QVERIFY(receiver.readAfter(cursor).isEmpty());
    }

    void acknowledgementsNeverSkipUnreceivedOrGoBackwards()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/cursor-XXXXXX");
        Helper helper;
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        QVERIFY(store.receivePending() > 0);
        const auto cursor = store.readAfter(0).last().toMap().value("sequence").toLongLong();
        QVERIFY(!store.acknowledge("com.iisacc.society", cursor + 100));
        QVERIFY(store.acknowledge("com.iisacc.society", cursor));
        QVERIFY(store.acknowledge("com.iisacc.society", 0));
        QCOMPARE(store.acknowledged("com.iisacc.society"), cursor);
        QVERIFY(!helper.sendData("", {}).size());
        QVERIFY(helper.sendData("large", {{"text", QString(70000, 'x')}}).isEmpty());
    }

    void readsArePagedAndSnapshotsSurviveRestart()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/pages-XXXXXX");
        Helper helper;
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        for (int i = 0; i < 5; ++i) QVERIFY(!helper.sendData("test.data", {{"index", i}}).isEmpty());
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        QCOMPARE(store.receivePending(2), 2);
        QCOMPARE(store.readAfter(0).size(), 2);
        QVERIFY(store.receivePending() > 0);
        auto first = store.readAfter(0, 2);
        QCOMPARE(first.size(), 2);
        auto rest = store.readAfter(first.last().toMap().value("sequence").toLongLong());
        QCOMPARE(rest.size(), 4);
        QVERIFY(store.setDaemonSnapshot({{"instanceId", "test-daemon"}, {"peers", QVariantList{helper.self().toVariantMap()}}}));
        store.close();
        QVERIFY(store.open(root.path()));
        QCOMPARE(store.daemonSnapshot().value("instanceId"), "test-daemon");
    }

    void failedReceiptKeepsTheWholeOutboxBatch()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/rollback-XXXXXX");
        Helper helper;
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {root.path(), 50, 400}));
        QVERIFY(!helper.sendData("force.failure", {{"retained", true}}).isEmpty());
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root.filePath("delivery/delivery.sqlite"));
            QVERIFY(database.open());
            {
                QSqlQuery query(database);
                QVERIFY(query.exec("CREATE TRIGGER fail_receipt BEFORE INSERT ON inbox WHEN NEW.topic='force.failure' BEGIN SELECT RAISE(ABORT, 'simulated storage failure'); END"));
                QCOMPARE(store.receivePending(), -1);
                QVERIFY(!store.errorString().isEmpty());
                QCOMPARE(store.pendingCount(), 2);
                QVERIFY(store.readAfter(0).isEmpty());
                QVERIFY(query.exec("DROP TRIGGER fail_receipt"));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QCOMPARE(store.receivePending(), 2);
        QCOMPARE(store.pendingCount(), 0);
        QCOMPARE(store.readAfter(0).size(), 2);
    }
};
QTEST_GUILESS_MAIN(DeliveryTests)
#include "delivery.moc"
