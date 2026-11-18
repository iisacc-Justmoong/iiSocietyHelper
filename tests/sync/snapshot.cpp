#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Controller.h>
#include "../../example/ManualStatusExecutor.h"
#include <SocietyDrive.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include "../SignalRecorder.h"
#include "../QtEventWait.h"
#include <QCoreApplication>
#include <QEvent>
#include <exception>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <vector>

using namespace iiSocietyHelper::Sync;
namespace {
const QString scope(64, 'a');
void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write(bytes), bytes.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
}
class SnapshotTests final : public QObject {
    void cycle(Replica &client, Replica &host, std::function<void(QJsonObject &, QJsonObject &)> inspect = {}) {
        Synchronizer sync(&client); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        ManualStatusExecutor receiver;
        std::vector<int> states;
        auto watching = sync.status().subscribe(receiver, [&](auto event) { states.push_back(event.value); });
        CTEST_CHECK(watching);
        std::exception_ptr inspectionFailure;
        auto inspectSafely = [&](QJsonObject &request, QJsonObject &response) {
            if (!inspect || inspectionFailure) return;
            try { inspect(request, response); } catch (...) { inspectionFailure = std::current_exception(); }
        };
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            auto request = wire.value("message").toObject(); QJsonObject unused;
            inspectSafely(request, unused);
            auto response = host.handle("client", request);
            inspectSafely(unused, response);
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, 20000);
        if (inspectionFailure) std::rethrow_exception(inspectionFailure);
        CTEST_CHECK_MESSAGE(done[0][1].toBool(), qPrintable(done[0][2].toString()));
        receiver.drain(); CTEST_CHECK(states == std::vector<int>({1, 2}));
        CTEST_COMPARE(sync.status().snapshot().value, 2);
    }
public:
    void requestDeadlineTracksOutstandingWork() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/request-deadline-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; CTEST_CHECK(replica.open(root.path(), scope));
        Synchronizer sync(&replica);
        auto *deadline = sync.findChild<QTimer *>(QStringLiteral("Sync.requestDeadline"), Qt::FindDirectChildrenOnly);
        CTEST_CHECK(deadline); CTEST_CHECK(deadline->isSingleShot()); CTEST_CHECK(!deadline->isActive());
        helper_ctest::SignalRecorder requests(&sync, &Synchronizer::requestReady);
        CTEST_CHECK(sync.start("host")); CTEST_CHECK(deadline->isActive());
        CTEST_WAIT_COMPARE(requests.size(), 1);
        const auto id = requests.first().first().toString();
        sync.receive(id, {{"ok", true}, {"pending", true}});
        CTEST_CHECK(deadline->remainingTime() > 100000);
        // An early timer delivery must retain the existing request deadline.
        CTEST_CHECK(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        CTEST_CHECK(deadline->remainingTime() > 100000);
        sync.stop(); CTEST_CHECK(!deadline->isActive());
        sync.receive(id, {{"ok", true}, {"pending", true}});
        CTEST_CHECK(!deadline->isActive());
    }
    void incomingSnapshotDeadlineTracksOnlyAuthorizedWork() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-deadline-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); CTEST_CHECK(drive);
        Controller host({});
        auto *deadline = host.findChild<QTimer *>(QStringLiteral("Sync.snapshotDeadline"), Qt::FindDirectChildrenOnly);
        CTEST_CHECK(deadline); CTEST_CHECK(deadline->isSingleShot()); CTEST_CHECK(!deadline->isActive());
        host.open(root.path(), scope); CTEST_WAIT_VERIFY(host.available(), 30000);
        host.setPeers({"one", "two"}, {});
        const auto request = [&] {
            return QJsonObject{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1},
                {"scope", scope}, {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"message", QJsonObject{{"action", "manifest"}, {"container", drive->identifier()},
                    {"snapshot", QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}}}}}};
        };
        CTEST_CHECK(host.handle("one", request()).value("pending").toBool());
        CTEST_CHECK(host.handle("two", request()).value("pending").toBool());
        CTEST_CHECK(deadline->isActive()); CTEST_CHECK(deadline->remainingTime() > 100000);
        // A spurious early wake must preserve the actual 120-second deadline.
        deadline->start(1);
        CTEST_CHECK(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        CTEST_CHECK(deadline->remainingTime() > 100000);
        CTEST_COMPARE(host.status().snapshot().value, 1);
        host.setPeers({"two"}, {}); CTEST_CHECK(deadline->isActive());
        host.setPeers({}, {}); CTEST_CHECK(!deadline->isActive());
        CTEST_COMPARE(host.status().snapshot().value, 4);
        host.closeAndWait();
    }
    void timestampedSnapshotIsAnalyzedBeforeAnyBytesAndReceiptSurvivesReopen() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/new"), "local snapshot");
        const auto before = QDateTime::currentMSecsSinceEpoch(); bool analyzed = false, sawUpload = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &response) {
            if (request.value("action") == "manifest") {
                const auto snapshot = request.value("snapshot").toObject();
                CTEST_COMPARE(snapshot.value("schema"), 1); CTEST_CHECK(!QUuid(snapshot.value("id").toString()).isNull());
                CTEST_CHECK(snapshot.value("capturedAt").toString().toLongLong() >= before);
            }
            if (response.value("complete").toBool() && response.contains("diff")) {
                analyzed = true;
                if (response.value("diff").toObject().value("added").toInt()) CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/new")));
            }
            if (request.contains("inlineData")) { CTEST_CHECK(analyzed); sawUpload = true; }
        });
        CTEST_CHECK(analyzed); CTEST_CHECK(sawUpload); CTEST_COMPARE(get(a.filePath("Files/new")), "local snapshot");
        const auto receipt = host.lastSnapshot("client"); CTEST_CHECK(!receipt.isEmpty());
        CTEST_COMPARE(receipt.value("diff").toObject().value("added"), 1);
        CTEST_CHECK(receipt.value("snapshot").toObject().value("capturedAt").toString().toLongLong() >= before);
        host.close(); CTEST_CHECK(host.open(a.path(), scope)); CTEST_COMPARE(host.lastSnapshot("client"), receipt);
    }
    void transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete(const QString& capturedAt) {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pin-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pin-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/item"), "original"); CTEST_CHECK(client.scan());
        const auto entry = client.record("files/item"); const auto state = host.namespaceState();
        const QJsonObject snapshot{{"schema", 1}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"capturedAt", capturedAt}, {"baseSequence", state.value("sequence")}, {"baseRevision", state.value("head")}};
        const auto manifest = hash(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
        const QJsonObject page{{"action", "manifest"}, {"manifest", manifest}, {"replica", client.replicaId()},
            {"offset", "0"}, {"total", "1"}, {"after", "0"}, {"through", entry.value("sequence")},
            {"entries", QJsonArray{entry}}, {"snapshot", snapshot}};
        auto invalid = page; auto malformed = snapshot; malformed.insert("capturedAt", 1); invalid.insert("snapshot", malformed);
        CTEST_COMPARE(host.handle("client", invalid).value("error"), "invalid_snapshot");
        malformed = snapshot; malformed.insert("baseSequence", "42"); malformed.insert("baseRevision", QString(64, 'f')); invalid.insert("snapshot", malformed);
        CTEST_COMPARE(host.handle("client", invalid).value("error"), "snapshot_base_unknown");
        invalid = page; invalid.insert("manifest", QString(64, '0'));
        CTEST_COMPARE(host.handle("client", invalid).value("error"), "manifest_hash_mismatch");
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/item")));
        const auto analysis = host.handle("client", page); CTEST_CHECK_MESSAGE(analysis.value("ok").toBool(), qPrintable(analysis.value("error").toString()));
        CTEST_COMPARE(analysis.value("diff").toObject().value("added"), 1);
        QJsonObject complete{{"action", "snapshot.complete"}, {"manifest", manifest}, {"snapshot", snapshot.value("id")}};
        CTEST_COMPARE(host.handle("client", complete).value("error"), "snapshot_transfers_incomplete");
        auto changed = entry; changed.insert("baseRevision", QString(64, 'f')); CTEST_CHECK(Replica::validRecord(changed));
        QJsonObject upload{{"action", "commit"}, {"entry", changed}, {"manifest", manifest},
            {"snapshot", snapshot.value("id")}, {"inlineData", QString::fromLatin1(QByteArray("original").toBase64())}};
        auto unpinned = upload; unpinned.remove("manifest"); unpinned.remove("snapshot");
        CTEST_COMPARE(host.handle("client", unpinned).value("error"), "manifest_required_before_transfer");
        CTEST_COMPARE(host.handle("client", upload).value("error"), "snapshot_entry_mismatch");
        CTEST_CHECK(!QFileInfo::exists(a.filePath("Files/item")));
        upload.insert("entry", entry); CTEST_CHECK(host.handle("client", upload).value("complete").toBool());
        CTEST_CHECK(host.handle("client", complete).value("ok").toBool());
        CTEST_COMPARE(host.lastSnapshot("client").value("snapshot"), snapshot);
        CTEST_COMPARE(host.handle("stranger", complete).value("error"), "manifest_required_before_transfer");
        const auto receipt = host.lastSnapshot("client"); host.close(); CTEST_CHECK(host.open(a.path(), scope));
        CTEST_COMPARE(host.handle("client", complete).value("receipt"), receipt);
    }
    void hostEditAfterAnalysisPreservesBothFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-race-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-race-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope));
        put(a.filePath("Files/item"), "base"); cycle(client, host); put(b.filePath("Files/item"), "client edit");
        bool edited = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &response) {
            if (!edited && response.value("diff").toObject().value("updated").toInt()) {
                put(a.filePath("Files/item"), "host edit after analysis"); edited = true;
            }
        });
        CTEST_CHECK(edited); CTEST_COMPARE(get(a.filePath("Files/item")), "host edit after analysis");
        CTEST_COMPARE(get(b.filePath("Files/item")), "host edit after analysis");
        const auto conflicts = QDir(a.filePath("Files")).entryList({"item.sync-conflict-*"}, QDir::Files);
        CTEST_COMPARE(conflicts.size(), 1); CTEST_COMPARE(get(a.filePath("Files/" + conflicts.front())), "client edit");
        CTEST_COMPARE(get(b.filePath("Files/" + conflicts.front())), "client edit");
    }
    void snapshotPagesKeepOneIdentityAndOmissionNeverDeletesHostFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pages-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pages-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope));
        put(a.filePath("Files/keep"), "host only"); cycle(client, host);
        for (int i = 0; i < 130; ++i) put(b.filePath("Files/item-" + QString::number(i)), QByteArray::number(i));
        QHash<QString, QJsonObject> headers; QHash<QString, int> pages; bool multiple = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &) {
            if (request.value("action") != "manifest") return;
            const auto header = request.value("snapshot").toObject(); const auto id = header.value("id").toString();
            if (headers.contains(id)) CTEST_COMPARE(header, headers.value(id)); else headers.insert(id, header);
            multiple |= ++pages[id] > 1;
        });
        CTEST_CHECK(multiple);
        for (int i = 0; i < 130; ++i) CTEST_COMPARE(get(a.filePath("Files/item-" + QString::number(i))), QByteArray::number(i));
        const auto state = host.namespaceState();
        const QJsonObject header{{"schema", 1}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"capturedAt", "1"}, {"baseSequence", state.value("sequence")}, {"baseRevision", state.value("head")}};
        const auto manifest = hash({});
        CTEST_CHECK(host.handle("client", {{"action", "manifest"}, {"manifest", manifest}, {"replica", client.replicaId()},
            {"offset", "0"}, {"total", "0"}, {"after", "0"}, {"through", "0"}, {"entries", QJsonArray{}}, {"snapshot", header}}).value("complete").toBool());
        CTEST_CHECK(host.handle("client", {{"action", "snapshot.complete"}, {"manifest", manifest}, {"snapshot", header.value("id")}}).value("ok").toBool());
        CTEST_COMPARE(get(a.filePath("Files/keep")), "host only"); CTEST_COMPARE(host.namespaceState(), state);
    }
    void integerQueriesDeliverSuccessFailureAndCancellationOnTheReceiverLoop() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-status-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path())); Replica replica; CTEST_CHECK(replica.open(root.path(), scope));
        ManualStatusExecutor loop; Synchronizer sync(&replica); std::vector<int> values;
        CTEST_COMPARE(sync.status().snapshot().value, 0);
        auto subscription = sync.status().subscribe(loop, [&](auto event) { values.push_back(event.value); });
        CTEST_CHECK(subscription); CTEST_CHECK(sync.start("host")); CTEST_COMPARE(sync.status().snapshot().value, 1);
        sync.stop(); CTEST_COMPARE(sync.status().snapshot().value, 4); loop.drain();
        CTEST_CHECK(values == std::vector<int>({1, 4}));
        auto connection = connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto) { sync.receive(id, {{"ok", false}, {"error", "offline"}}); });
        helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished); CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE(done.size(), 1);
        loop.drain(); CTEST_COMPARE(sync.status().snapshot().value, 3); CTEST_CHECK(values == std::vector<int>({1, 4, 1, 3}));
        disconnect(connection);
        bool replied = false; int queryStatus = -1;
        auto query = loop.reserve([&] { queryStatus = sync.status().snapshot().value; replied = true; });
        CTEST_CHECK(query && query->signal()); CTEST_CHECK(!replied); loop.drain(); CTEST_CHECK(replied); CTEST_COMPARE(queryStatus, 3); query->cancel();
        Controller controller({}); CTEST_COMPARE(controller.status().snapshot().value, 0);
    }
    void closingTheProducerCancelsQueuedReceiverCalls() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-status-close-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(root.path())); Replica replica; CTEST_CHECK(replica.open(root.path(), scope));
        ManualStatusExecutor loop; int calls = 0;
        auto producer = std::make_unique<Synchronizer>(&replica);
        auto watching = producer->status().subscribe(loop, [&](auto) { ++calls; });
        CTEST_CHECK(watching); CTEST_CHECK(producer->start("host")); producer.reset(); loop.drain();
        CTEST_COMPARE(calls, 0); CTEST_CHECK(!watching);
    }
    void invalidCompletionReceiptFailsAndRetryConvergesWithoutDuplicateConflicts() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-receipt-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-receipt-client-XXXXXX.societycontainer")));
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(a.path())); CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; CTEST_CHECK(host.open(a.path(), scope)); CTEST_CHECK(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/item"), "verified upload"); Synchronizer sync(&client); helper_ctest::SignalRecorder done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject(); auto response = host.handle("client", request);
            if (request.value("action") == "snapshot.complete") response.insert("receipt", QJsonObject{});
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        CTEST_CHECK(sync.start("host")); CTEST_WAIT_COMPARE_TIMEOUT(done.size(), 1, 15000);
        CTEST_CHECK(!done[0][1].toBool()); CTEST_COMPARE(done[0][2].toString(), "invalid_snapshot_receipt");
        CTEST_COMPARE(sync.status().snapshot().value, 3); CTEST_COMPARE(get(a.filePath("Files/item")), "verified upload");
        cycle(client, host); CTEST_COMPARE(get(b.filePath("Files/item")), "verified upload");
        CTEST_CHECK(QDir(a.filePath("Files")).entryList({"item.sync-conflict-*"}, QDir::Files).isEmpty());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    SnapshotTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("requestDeadlineTracksOutstandingWork", [&] { FlushDeferredDeletes cleanup; tests.requestDeadlineTracksOutstandingWork(); });
    runner.run("incomingSnapshotDeadlineTracksOnlyAuthorizedWork", [&] { FlushDeferredDeletes cleanup; tests.incomingSnapshotDeadlineTracksOnlyAuthorizedWork(); });
    runner.run("timestampedSnapshotIsAnalyzedBeforeAnyBytesAndReceiptSurvivesReopen", [&] { FlushDeferredDeletes cleanup; tests.timestampedSnapshotIsAnalyzedBeforeAnyBytesAndReceiptSurvivesReopen(); });
    runner.run("transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete:clock-behind", [&] { FlushDeferredDeletes cleanup; tests.transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete(QStringLiteral("1")); });
    runner.run("transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete:clock-ahead", [&] { FlushDeferredDeletes cleanup; tests.transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete(QStringLiteral("9007199254740991")); });
    runner.run("hostEditAfterAnalysisPreservesBothFiles", [&] { FlushDeferredDeletes cleanup; tests.hostEditAfterAnalysisPreservesBothFiles(); });
    runner.run("snapshotPagesKeepOneIdentityAndOmissionNeverDeletesHostFiles", [&] { FlushDeferredDeletes cleanup; tests.snapshotPagesKeepOneIdentityAndOmissionNeverDeletesHostFiles(); });
    runner.run("integerQueriesDeliverSuccessFailureAndCancellationOnTheReceiverLoop", [&] { FlushDeferredDeletes cleanup; tests.integerQueriesDeliverSuccessFailureAndCancellationOnTheReceiverLoop(); });
    runner.run("closingTheProducerCancelsQueuedReceiverCalls", [&] { FlushDeferredDeletes cleanup; tests.closingTheProducerCancelsQueuedReceiverCalls(); });
    runner.run("invalidCompletionReceiptFailsAndRetryConvergesWithoutDuplicateConflicts", [&] { FlushDeferredDeletes cleanup; tests.invalidCompletionReceiptFailsAndRetryConvergesWithoutDuplicateConflicts(); });
    return runner.result();
}
