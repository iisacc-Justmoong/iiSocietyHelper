#include "iiSocietyHelper.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif

using namespace iiSocietyHelper;

class Participant {
public:
    QProcess process;
    QByteArray pending;
    QList<QJsonObject> events;
    ~Participant() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
    bool start(const QString &directory, const QString &id, int lifetime = 0) {
        pending.clear();
        events.clear();
        process.start(QStringLiteral(HELPER_OBSERVER_EXECUTABLE), {"--directory", directory,
            "--application-id", id, "--heartbeat-ms", "50", "--timeout-ms", "400",
            "--exit-after-ms", QString::number(lifetime)});
        return process.waitForStarted(3000);
    }
    void read() {
        pending += process.readAllStandardOutput();
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
            events.append(QJsonDocument::fromJson(pending.left(newline)).object());
            pending.remove(0, newline + 1);
        }
    }
    QString instance() {
        read();
        for (const auto &event : events) if (event.value("event") == "ready")
            return event.value("self").toObject().value("instanceId").toString();
        return {};
    }
    QByteArray diagnostics() {
        read();
        QByteArray result = "exit=" + QByteArray::number(process.exitCode()) + " ";
        for (const auto &event : events) result += QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
        return result + process.readAllStandardError();
    }
    QSet<QString> peers() {
        read();
        for (auto it = events.crbegin(); it != events.crend(); ++it) if (it->contains("peers")) {
            QSet<QString> ids;
            for (const auto &peer : it->value("peers").toArray()) ids.insert(peer.toObject().value("instanceId").toString());
            return ids;
        }
        return {};
    }
    bool departed(const QString &instance, const QString &reason) {
        read();
        for (const auto &event : events)
            if (event.value("event") == "departed" && event.value("reason") == reason
                && event.value("peer").toObject().value("instanceId") == instance) return true;
        return false;
    }
};

class ProcessObservationTests : public QObject {
    Q_OBJECT
private slots:
    void threeProcessesDiscoverCrashAndRestart() {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/processes-XXXXXX"));
        Participant a, b, c;
        QVERIFY(a.start(directory.path(), "com.iisacc.society"));
        QVERIFY(b.start(directory.path(), "com.iisacc.dreamscapes"));
        QVERIFY(c.start(directory.path(), "com.iisacc.dreamscapes"));
        QTRY_VERIFY2(!a.instance().isEmpty() && !b.instance().isEmpty() && !c.instance().isEmpty(),
                     (a.diagnostics() + b.diagnostics() + c.diagnostics()).constData());
        const auto aid = a.instance(), bid = b.instance(), cid = c.instance();
        QTRY_COMPARE(a.peers(), QSet<QString>({bid, cid}));
        QTRY_COMPARE(b.peers(), QSet<QString>({aid, cid}));
        QTRY_COMPARE(c.peers(), QSet<QString>({aid, bid}));
        b.process.kill();
        QVERIFY(b.process.waitForFinished(3000));
        QVERIFY(QFile::exists(QDir(directory.path()).filePath(bid + ".json")));
        QTRY_VERIFY(a.departed(bid, "timed-out"));
        QTRY_VERIFY(c.departed(bid, "timed-out"));
        QTRY_COMPARE(a.peers(), QSet<QString>({cid}));
        QVERIFY(b.start(directory.path(), "com.iisacc.dreamscapes", 1800));
        QTRY_VERIFY(!b.instance().isEmpty());
        const auto restarted = b.instance();
        QVERIFY(restarted != bid);
        QTRY_COMPARE(a.peers(), QSet<QString>({restarted, cid}));
        QTRY_COMPARE(b.peers(), QSet<QString>({aid, cid}));
        QTRY_VERIFY(a.departed(restarted, "withdrawn"));
        QTRY_VERIFY(c.departed(restarted, "withdrawn"));
        QTRY_COMPARE(b.process.state(), QProcess::NotRunning);
        QCOMPARE(b.process.exitCode(), 0);
        QVERIFY(!QFile::exists(QDir(directory.path()).filePath(restarted + ".json")));
    }

    void suspendedPeerExpiresAndReturns() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/suspend-XXXXXX"));
        Helper observer;
        QVERIFY(observer.start({"com.iisacc.observer", "Observer", "1"}, {directory.path(), 50, 400}));
        QSignalSpy appeared(&observer, &Helper::peerAppeared);
        QSignalSpy departed(&observer, &Helper::peerDisappeared);
        Participant peer;
        QVERIFY(peer.start(directory.path(), "com.iisacc.mobile"));
        QTRY_COMPARE(observer.peers().size(), 1);
        const auto id = observer.peers().first().instanceId;
        QCOMPARE(::kill(pid_t(peer.process.processId()), SIGSTOP), 0);
        QTRY_VERIFY(observer.peers().isEmpty());
        QCOMPARE(qvariant_cast<DepartureReason>(departed.last()[1]), DepartureReason::TimedOut);
        QCOMPARE(::kill(pid_t(peer.process.processId()), SIGCONT), 0);
        QTRY_COMPARE(observer.peers().size(), 1);
        QCOMPARE(observer.peers().first().instanceId, id);
        QCOMPARE(appeared.size(), 2);
#else
        QSKIP("POSIX suspend/resume test; cross-process crash/restart runs on all desktop platforms.");
#endif
    }
};
QTEST_GUILESS_MAIN(ProcessObservationTests)
#include "process_observation.moc"
