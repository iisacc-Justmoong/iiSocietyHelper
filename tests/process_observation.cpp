#include "CTestSupport.h"
#include "QtEventWait.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include "TestWorkspace.h"
#include "iiSocietyHelper.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
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
            "--application-id", id, "--heartbeat-ms", "100", "--timeout-ms", "5000",
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
public:
    void threeProcessesDiscoverCrashAndRestart() {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/processes-XXXXXX")));
        Participant a, b, c;
        CTEST_CHECK(a.start(directory.path(), "com.iisacc.society"));
        CTEST_CHECK(b.start(directory.path(), "com.iisacc.dreamscapes"));
        CTEST_CHECK(c.start(directory.path(), "com.iisacc.dreamscapes"));
        CTEST_WAIT_MESSAGE_DEFAULT(!a.instance().isEmpty() && !b.instance().isEmpty() && !c.instance().isEmpty(),
                     (a.diagnostics() + b.diagnostics() + c.diagnostics()).constData());
        const auto aid = a.instance(), bid = b.instance(), cid = c.instance();
        CTEST_WAIT_MESSAGE_DEFAULT(a.peers() == QSet<QString>({bid, cid}),
            (a.diagnostics() + b.diagnostics() + c.diagnostics()).constData());
        CTEST_WAIT_COMPARE(b.peers(), QSet<QString>({aid, cid}));
        CTEST_WAIT_COMPARE(c.peers(), QSet<QString>({aid, bid}));
        b.process.kill();
        CTEST_CHECK(b.process.waitForFinished(3000));
        CTEST_CHECK(QFile::exists(QDir(directory.path()).filePath(bid + ".json")));
        CTEST_WAIT_MESSAGE_TIMEOUT(a.departed(bid, "timed-out"), a.diagnostics().constData(), 10000);
        CTEST_WAIT_MESSAGE_TIMEOUT(c.departed(bid, "timed-out"), c.diagnostics().constData(), 10000);
        CTEST_WAIT_COMPARE(a.peers(), QSet<QString>({cid}));
        CTEST_CHECK(b.start(directory.path(), "com.iisacc.dreamscapes", 10000));
        CTEST_WAIT_MESSAGE_DEFAULT(!b.instance().isEmpty(), b.diagnostics().constData());
        const auto restarted = b.instance();
        CTEST_CHECK(restarted != bid);
        CTEST_WAIT_COMPARE(a.peers(), QSet<QString>({restarted, cid}));
        CTEST_WAIT_COMPARE(b.peers(), QSet<QString>({aid, cid}));
        CTEST_WAIT_VERIFY(a.departed(restarted, "withdrawn"), 15000);
        CTEST_WAIT_VERIFY(c.departed(restarted, "withdrawn"), 15000);
        CTEST_WAIT_COMPARE_TIMEOUT(b.process.state(), QProcess::NotRunning, 15000);
        CTEST_COMPARE(b.process.exitCode(), 0);
        CTEST_CHECK(!QFile::exists(QDir(directory.path()).filePath(restarted + ".json")));
    }

    void suspendedPeerExpiresAndReturns() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/suspend-XXXXXX")));
        Helper observer;
        CTEST_CHECK(observer.start({"com.iisacc.observer", "Observer", "1"}, {directory.path(), 100, 5000}));
        helper_ctest::SignalRecorder appeared(&observer, &Helper::peerAppeared);
        helper_ctest::SignalRecorder departed(&observer, &Helper::peerDisappeared);
        Participant peer;
        CTEST_CHECK(peer.start(directory.path(), "com.iisacc.mobile"));
        CTEST_WAIT_COMPARE(observer.peers().size(), 1);
        const auto id = observer.peers().first().instanceId;
        CTEST_COMPARE(::kill(pid_t(peer.process.processId()), SIGSTOP), 0);
        CTEST_WAIT_VERIFY(observer.peers().isEmpty(), 10000);
        CTEST_COMPARE(qvariant_cast<DepartureReason>(departed.last()[1]), DepartureReason::TimedOut);
        CTEST_COMPARE(::kill(pid_t(peer.process.processId()), SIGCONT), 0);
        CTEST_WAIT_COMPARE(observer.peers().size(), 1);
        CTEST_COMPARE(observer.peers().first().instanceId, id);
        CTEST_COMPARE(appeared.size(), 2);
#else
        helper_ctest::skip("POSIX suspend/resume test; cross-process crash/restart runs on all desktop platforms.");
#endif
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2) { std::cerr << "Expected a case name" << std::endl; return 1; }
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    ProcessObservationTests tests;
    runner.run("threeProcessesDiscoverCrashAndRestart", [&] { tests.threeProcessesDiscoverCrashAndRestart(); });
    runner.run("suspendedPeerExpiresAndReturns", [&] { tests.suspendedPeerExpiresAndReturns(); });
    return runner.result();
}
