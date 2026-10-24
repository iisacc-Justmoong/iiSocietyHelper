#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include <iiFileProviderNative.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <SharedStorage.h>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <algorithm>

using namespace iiSocietyHelper;

class ObservationTests : public QObject
{
    Q_OBJECT
private slots:
    void transientUnreadableRecordDoesNotWithdrawAnObservedPeer()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/presence-record-XXXXXX"));
        Helper observer;
        QVERIFY(observer.start({"com.iisacc.record.observer", "Observer", "1"}, {directory.path(), 50, 5000}));
        QSignalSpy appeared(&observer, &Helper::peerAppeared);
        QSignalSpy departed(&observer, &Helper::peerDisappeared);
        const auto instance = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto path = std::filesystem::path(directory.filePath(instance + ".json").toStdU16String());
        QJsonObject record{{"protocol", 1}, {"instanceId", instance},
            {"applicationId", "com.iisacc.record.peer"}, {"name", "Peer"}, {"version", "1"},
            {"processId", QCoreApplication::applicationPid()}, {"activity", "unknown"},
            {"startedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        const auto publish = [&](int sequence) {
            record["heartbeat"] = QString::number(sequence);
            iiFileProvider::native::File::write(path, QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString());
        };
        publish(1); QTest::qWait(200);
        publish(2); QTRY_COMPARE(observer.peers().size(), 1);
        QCOMPARE(appeared.size(), 1);
        iiFileProvider::native::File::write(path, std::string_view("{}"));
        QTest::qWait(250);
        QCOMPARE(observer.peers().size(), 1);
        QCOMPARE(departed.size(), 0);
        publish(3); QTest::qWait(200);
        QCOMPARE(observer.peers().size(), 1);
        QCOMPARE(appeared.size(), 1);
        QVERIFY(iiFileProvider::native::File::remove(path));
        QTRY_VERIFY(observer.peers().isEmpty());
        QCOMPARE(departed.size(), 1);
        QCOMPARE(qvariant_cast<DepartureReason>(departed.first()[1]), DepartureReason::Withdrawn);
    }

    void presenceSurvivesAnOpenProviderReadSnapshot()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/presence-snapshot-XXXXXX"));
        QVERIFY(directory.isValid());
        Helper helper;
        QSignalSpy errors(&helper, &Helper::errorOccurred);
        QVERIFY(helper.start({"com.iisacc.snapshot", "Snapshot", "1"}, {directory.path(), 20, 5000}));
        const auto path = std::filesystem::path(directory.filePath(helper.self().instanceId + ".json").toStdU16String());
        {
            iiFileProvider::native::ReadFile snapshot(path);
            const auto before = QJsonDocument::fromJson(QByteArray::fromStdString(snapshot.read(8192))).object();
            QTest::qWait(500);
            QVERIFY(helper.isRunning());
            QCOMPARE(errors.size(), 0);
            const auto after = QJsonDocument::fromJson(QByteArray::fromStdString(
                iiFileProvider::native::File::read(path, 8192))).object();
            QVERIFY(after.value("heartbeat").toString().toULongLong() > before.value("heartbeat").toString().toULongLong());
        }
        helper.stop();
        QVERIFY(!QFileInfo::exists(QString::fromStdU16String(path.u16string())));
    }

    void presenceResponsiveness()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/responsiveness-XXXXXX"));
        QVERIFY(directory.isValid());
        Helper a, b;
        const ObservationOptions options{directory.path(), 100, 5000};
        QVERIFY(a.start({"com.iisacc.input.probe", "Input probe", "1"}, options));
        QVERIFY(b.start({"com.iisacc.peer.probe", "Peer probe", "1"}, options));
        bool wrongThread = false;
        connect(&a, &Helper::peerAppeared, this, [&] { wrongThread |= QThread::currentThread() != thread(); });
        QTRY_COMPARE(a.peers().size(), 1);
        QTRY_COMPARE(b.peers().size(), 1);
        QSignalSpy departedA(&a, &Helper::peerDisappeared);
        QSignalSpy departedB(&b, &Helper::peerDisappeared);
        QElapsedTimer clock;
        clock.start();
        qint64 previous = 0, maximumGap = 0;
        QList<qint64> gaps;
        QTimer input;
        input.setInterval(5);
        connect(&input, &QTimer::timeout, this, [&] {
            const auto now = clock.nsecsElapsed();
            gaps.append(now - previous);
            maximumGap = std::max(maximumGap, now - previous);
            previous = now;
        });
        input.start();
        QTest::qWait(3000);
        input.stop();
        QVERIFY(!wrongThread);
        QVERIFY(gaps.size() >= 10);
        QTRY_COMPARE(a.peers().size(), 1);
        QCOMPARE(departedA.size(), 0);
        QCOMPARE(departedB.size(), 0);
        std::sort(gaps.begin(), gaps.end());
        qInfo().nospace() << "PRESENCE_RESPONSIVENESS {\"ticks\":" << gaps.size()
            << ",\"p95Ms\":" << gaps[gaps.size() * 95 / 100] / 1000000.0
            << ",\"maxMs\":" << maximumGap / 1000000.0 << '}';
    }

    void stoppedWorkCannotRepublishIntoTheNextSession()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/epochs-XXXXXX"));
        Helper helper;
        const ObservationOptions options{directory.path(), 20, 5000};
        QVERIFY(helper.start({"com.iisacc.epochs", "Epochs", "1"}, options));
        const auto old = helper.self().instanceId;
        for (int i = 0; i < 20; ++i)
            helper.setActivity(i % 2 ? Activity::Foreground : Activity::Background);
        helper.stop();
        QVERIFY(!QFileInfo::exists(directory.filePath(old + ".json")));
        QVERIFY(helper.start({"com.iisacc.epochs", "Epochs", "2"}, options));
        QVERIFY(helper.self().instanceId != old);
        QTest::qWait(200);
        QVERIFY(!QFileInfo::exists(directory.filePath(old + ".json")));
        helper.stop();
        QVERIFY(QDir(directory.path()).entryList({"*.json"}, QDir::Files).isEmpty());
        DeliveryStore outbox;
        QVERIFY(outbox.open(directory.path()));
        // Two starts, two stops and every one of the twenty activity transitions.
        QCOMPARE(outbox.pendingCount(), 24);
    }

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
#ifdef Q_OS_WIN
        // The native storage backend pins this directory without delete sharing.
        QVERIFY(!QDir().rename(root, root + "-moved"));
        QTest::qWait(200);
        QVERIFY(helper.isRunning());
        QVERIFY(errors.isEmpty());
        helper.stop();
        QVERIFY(QDir().rename(root, root + "-moved"));
#else
        QVERIFY(QDir().rename(root, root + "-moved"));
        QVERIFY(QDir().mkpath(root + "-target"));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(root + "-target", root));
#endif
        QTRY_VERIFY(!helper.isRunning());
        QVERIFY(!errors.isEmpty());
        QVERIFY(!helper.errorString().isEmpty());
        QVERIFY(QDir(root + "-target").entryList(QDir::Files).isEmpty());
#endif
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
        QVERIFY(Helper::defaultDirectory().isEmpty());
        qunsetenv("SOCIETY_HELPER_DIRECTORY");
        const auto package = directory.path() + "/selected.societycontainer";
        QVERIFY(QDir().mkdir(package));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(package));
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", package.toUtf8());
        const auto inside = package + "/.society-runtime/isolated";
        qputenv("SOCIETY_HELPER_DIRECTORY", inside.toUtf8());
        QCOMPARE(Helper::defaultDirectory(), inside);
        QVERIFY(!QFileInfo::exists(inside));
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
