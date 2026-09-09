#include "iiSocietyHelper.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace iiSocietyHelper;

class ObservationTests : public QObject
{
    Q_OBJECT
private slots:
    void mutualDiscoveryAndDistinctInstances()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/presence-XXXXXX"));
        QVERIFY(directory.isValid());
        // Keep the production expiry window: shared-volume flushes can exceed 400 ms.
        const ObservationOptions options{directory.path(), 100, 5000};
        Helper society, dreamscapes, secondDreamscapes;
        QSignalSpy appeared(&society, &Helper::peerAppeared);
        QSignalSpy updated(&society, &Helper::peerUpdated);
        QSignalSpy departed(&society, &Helper::peerDisappeared);
        QVERIFY(society.start({"com.iisacc.society", "Society", "1"}, options));
        QVERIFY(dreamscapes.start({"com.iisacc.dreamscapes", "Dreamscapes", "2"}, options));
        QVERIFY(secondDreamscapes.start({"com.iisacc.dreamscapes", "Dreamscapes", "2"}, options));
        QTRY_COMPARE(society.peers().size(), 2);
        QTRY_COMPARE(dreamscapes.peers().size(), 2);
        QTRY_COMPARE(secondDreamscapes.peers().size(), 2);
        QVERIFY(dreamscapes.self().instanceId != secondDreamscapes.self().instanceId);
        for (const auto &peer : society.peers()) {
            QCOMPARE(peer.application.id, "com.iisacc.dreamscapes");
            QVERIFY(peer.instanceId != society.self().instanceId);
            QCOMPARE(peer.processId, QCoreApplication::applicationPid());
        }
        QTest::qWait(200);
        QCOMPARE(appeared.size(), 2);
        QCOMPARE(updated.size(), 0); // A heartbeat alone is not a metadata change.
        dreamscapes.setActivity(Activity::Background);
        QTRY_COMPARE(updated.size(), 1);
        QCOMPARE(qvariant_cast<Peer>(updated[0][0]).activity, Activity::Background);
        dreamscapes.stop();
        QTRY_COMPARE(society.peers().size(), 1);
        QCOMPARE(departed.size(), 1);
        QCOMPARE(qvariant_cast<DepartureReason>(departed[0][1]), DepartureReason::Withdrawn);
        society.stop();
        secondDreamscapes.stop();
        QVERIFY(QDir(directory.path()).entryList({"*.json"}, QDir::Files).isEmpty());
    }

    void staleAndMalformedRecordsNeverBecomePeers()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/stale-XXXXXX"));
        const ObservationOptions options{directory.path(), 50, 400};
        Helper old;
        QVERIFY(old.start({"com.iisacc.old", "Old", "1"}, options));
        const QString oldPath = QDir(directory.path()).filePath(old.self().instanceId + ".json");
        QFile record(oldPath);
        QVERIFY(record.open(QIODevice::ReadOnly));
        const auto bytes = record.readAll();
        record.close();
        old.stop();
        QVERIFY(record.open(QIODevice::WriteOnly));
        QCOMPARE(record.write(bytes), bytes.size());
        record.close();
        QFile garbage(QDir(directory.path()).filePath("garbage.json"));
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write("{broken");
        garbage.close();
        Helper observer;
        QSignalSpy appeared(&observer, &Helper::peerAppeared);
        QVERIFY(observer.start({"com.iisacc.observer", "Observer", "1"}, options));
        QTest::qWait(700);
        QVERIFY(observer.peers().isEmpty());
        QCOMPARE(appeared.size(), 0);
        QVERIFY(QFile::exists(oldPath)); // A peer never deletes another peer's record.
    }

    void invalidConfigurationIsReported()
    {
        Helper helper;
        QString error;
        QVERIFY(!helper.start({"", "Name", "1"}, {}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!helper.isRunning());
        QVERIFY(!helper.start({"com.iisacc.test", "Test", "1"}, {"relative/path", 50, 400}, &error));
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/invalid-XXXXXX"));
        QVERIFY(!helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 0, 400}, &error));
        QVERIFY(!helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 100, 150}, &error));
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 50, 400}, &error));
        QVERIFY(error.isEmpty());
        const auto instance = helper.self().instanceId;
        QVERIFY(!helper.start({"com.iisacc.test", "Test", "1"}, {}, &error));
        QCOMPARE(helper.self().instanceId, instance);
        helper.stop();
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {directory.path(), 50, 400}));
        QVERIFY(helper.self().instanceId != instance);
    }

    void redirectedDirectoryStopsObservation()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/redirect-XXXXXX"));
        const QString root = directory.path() + "/registry";
        Helper helper;
        QSignalSpy errors(&helper, &Helper::errorOccurred);
        QVERIFY(helper.start({"com.iisacc.test", "Test", "1"}, {root, 50, 400}));
        QVERIFY(QDir().rename(root, root + "-moved"));
        QVERIFY(QDir().mkpath(root + "-target"));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(root + "-target", root));
#endif
        QTRY_VERIFY(!helper.isRunning());
        QVERIFY(!errors.isEmpty());
        QVERIFY(!helper.errorString().isEmpty());
        QVERIFY(QDir(root + "-target").entryList(QDir::Files).isEmpty());
    }

    void independentDirectoriesDoNotMix()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/isolation-XXXXXX"));
        Helper a, b;
        QVERIFY(a.start({"com.iisacc.society", "Society", "1"}, {directory.path() + "/a", 50, 400}));
        QVERIFY(b.start({"com.iisacc.dreamscapes", "Dreamscapes", "1"}, {directory.path() + "/b", 50, 400}));
        QTest::qWait(600);
        QVERIFY(a.peers().isEmpty());
        QVERIFY(b.peers().isEmpty());
        qputenv("SOCIETY_HELPER_DIRECTORY", directory.path().toUtf8());
        QCOMPARE(Helper::defaultDirectory(), directory.path());
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
    }

    void resumedObserverRequiresFreshEvidence()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/resume-XXXXXX"));
        Helper a, b;
        const ObservationOptions options{directory.path(), 100, 5000};
        QVERIFY(a.start({"com.iisacc.a", "A", "1"}, options));
        QVERIFY(b.start({"com.iisacc.b", "B", "1"}, options));
        QTRY_COMPARE(a.peers().size(), 1);
        QSignalSpy departed(&a, &Helper::peerDisappeared);
        QSignalSpy appeared(&a, &Helper::peerAppeared);
        // Simulate a suspended app event loop; wall-clock timestamps are irrelevant.
        QThread::msleep(5500);
        QTRY_COMPARE(departed.size(), 1);
        QCOMPARE(qvariant_cast<DepartureReason>(departed.first()[1]), DepartureReason::TimedOut);
        QTRY_COMPARE(appeared.size(), 1);
        QCOMPARE(a.peers().first().instanceId, b.self().instanceId);
    }
};

QTEST_GUILESS_MAIN(ObservationTests)
#include "observation.moc"
