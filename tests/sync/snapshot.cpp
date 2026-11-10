#include "../TestWorkspace.h"
#include <iiSocietyHelper/Sync/Controller.h>
#include "../../example/ManualStatusExecutor.h"
#include <SocietyDrive.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <vector>

using namespace iiSocietyHelper::Sync;
namespace {
const QString scope(64, 'a');
void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray get(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString hash(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
}
class SnapshotTests final : public QObject {
    Q_OBJECT
    void cycle(Replica &client, Replica &host, std::function<void(QJsonObject &, QJsonObject &)> inspect = {}) {
        Synchronizer sync(&client); QSignalSpy done(&sync, &Synchronizer::finished);
        ManualStatusExecutor receiver;
        std::vector<int> states;
        auto watching = sync.status().subscribe(receiver, [&](auto event) { states.push_back(event.value); });
        QVERIFY(watching);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            auto request = wire.value("message").toObject(); QJsonObject unused;
            if (inspect) inspect(request, unused);
            auto response = host.handle("client", request);
            if (inspect) inspect(unused, response);
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        QVERIFY2(done[0][1].toBool(), qPrintable(done[0][2].toString()));
        receiver.drain(); QVERIFY(states == std::vector<int>({1, 2}));
        QCOMPARE(sync.status().snapshot().value, 2);
    }
private slots:
    void requestDeadlineTracksOutstandingWork() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/request-deadline-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; QVERIFY(replica.open(root.path(), scope));
        Synchronizer sync(&replica);
        auto *deadline = sync.findChild<QTimer *>(QStringLiteral("Sync.requestDeadline"), Qt::FindDirectChildrenOnly);
        QVERIFY(deadline); QVERIFY(deadline->isSingleShot()); QVERIFY(!deadline->isActive());
        QSignalSpy requests(&sync, &Synchronizer::requestReady);
        QVERIFY(sync.start("host")); QVERIFY(deadline->isActive());
        QTRY_COMPARE(requests.size(), 1);
        const auto id = requests.first().first().toString();
        sync.receive(id, {{"ok", true}, {"pending", true}});
        QVERIFY(deadline->remainingTime() > 100000);
        // An early timer delivery must retain the existing request deadline.
        QVERIFY(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        QVERIFY(deadline->remainingTime() > 100000);
        sync.stop(); QVERIFY(!deadline->isActive());
        sync.receive(id, {{"ok", true}, {"pending", true}});
        QVERIFY(!deadline->isActive());
    }
    void incomingSnapshotDeadlineTracksOnlyAuthorizedWork() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-deadline-XXXXXX.societycontainer")));
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        Controller host({});
        auto *deadline = host.findChild<QTimer *>(QStringLiteral("Sync.snapshotDeadline"), Qt::FindDirectChildrenOnly);
        QVERIFY(deadline); QVERIFY(deadline->isSingleShot()); QVERIFY(!deadline->isActive());
        host.open(root.path(), scope); QTRY_VERIFY_WITH_TIMEOUT(host.available(), 30000);
        host.setPeers({"one", "two"}, {});
        const auto request = [&] {
            return QJsonObject{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1},
                {"scope", scope}, {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"message", QJsonObject{{"action", "manifest"}, {"container", drive->identifier()},
                    {"snapshot", QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}}}}}};
        };
        QVERIFY(host.handle("one", request()).value("pending").toBool());
        QVERIFY(host.handle("two", request()).value("pending").toBool());
        QVERIFY(deadline->isActive()); QVERIFY(deadline->remainingTime() > 100000);
        // A spurious early wake must preserve the actual 120-second deadline.
        deadline->start(1);
        QVERIFY(QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection));
        QVERIFY(deadline->remainingTime() > 100000);
        QCOMPARE(host.status().snapshot().value, 1);
        host.setPeers({"two"}, {}); QVERIFY(deadline->isActive());
        host.setPeers({}, {}); QVERIFY(!deadline->isActive());
        QCOMPARE(host.status().snapshot().value, 4);
        host.closeAndWait();
    }
    void timestampedSnapshotIsAnalyzedBeforeAnyBytesAndReceiptSurvivesReopen() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-client-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/new"), "local snapshot");
        const auto before = QDateTime::currentMSecsSinceEpoch(); bool analyzed = false, sawUpload = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &response) {
            if (request.value("action") == "manifest") {
                const auto snapshot = request.value("snapshot").toObject();
                QCOMPARE(snapshot.value("schema"), 1); QVERIFY(!QUuid(snapshot.value("id").toString()).isNull());
                QVERIFY(snapshot.value("capturedAt").toString().toLongLong() >= before);
            }
            if (response.value("complete").toBool() && response.contains("diff")) {
                analyzed = true;
                if (response.value("diff").toObject().value("added").toInt()) QVERIFY(!QFileInfo::exists(a.filePath("Files/new")));
            }
            if (request.contains("inlineData")) { QVERIFY(analyzed); sawUpload = true; }
        });
        QVERIFY(analyzed); QVERIFY(sawUpload); QCOMPARE(get(a.filePath("Files/new")), "local snapshot");
        const auto receipt = host.lastSnapshot("client"); QVERIFY(!receipt.isEmpty());
        QCOMPARE(receipt.value("diff").toObject().value("added"), 1);
        QVERIFY(receipt.value("snapshot").toObject().value("capturedAt").toString().toLongLong() >= before);
        host.close(); QVERIFY(host.open(a.path(), scope)); QCOMPARE(host.lastSnapshot("client"), receipt);
    }
    void transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete() {
        QFETCH(QString, capturedAt);
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pin-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pin-client-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/item"), "original"); QVERIFY(client.scan());
        const auto entry = client.record("files/item"); const auto state = host.namespaceState();
        const QJsonObject snapshot{{"schema", 1}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"capturedAt", capturedAt}, {"baseSequence", state.value("sequence")}, {"baseRevision", state.value("head")}};
        const auto manifest = hash(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
        const QJsonObject page{{"action", "manifest"}, {"manifest", manifest}, {"replica", client.replicaId()},
            {"offset", "0"}, {"total", "1"}, {"after", "0"}, {"through", entry.value("sequence")},
            {"entries", QJsonArray{entry}}, {"snapshot", snapshot}};
        auto invalid = page; auto malformed = snapshot; malformed.insert("capturedAt", 1); invalid.insert("snapshot", malformed);
        QCOMPARE(host.handle("client", invalid).value("error"), "invalid_snapshot");
        malformed = snapshot; malformed.insert("baseSequence", "42"); malformed.insert("baseRevision", QString(64, 'f')); invalid.insert("snapshot", malformed);
        QCOMPARE(host.handle("client", invalid).value("error"), "snapshot_base_unknown");
        invalid = page; invalid.insert("manifest", QString(64, '0'));
        QCOMPARE(host.handle("client", invalid).value("error"), "manifest_hash_mismatch");
        QVERIFY(!QFileInfo::exists(a.filePath("Files/item")));
        const auto analysis = host.handle("client", page); QVERIFY2(analysis.value("ok").toBool(), qPrintable(analysis.value("error").toString()));
        QCOMPARE(analysis.value("diff").toObject().value("added"), 1);
        QJsonObject complete{{"action", "snapshot.complete"}, {"manifest", manifest}, {"snapshot", snapshot.value("id")}};
        QCOMPARE(host.handle("client", complete).value("error"), "snapshot_transfers_incomplete");
        auto changed = entry; changed.insert("baseRevision", QString(64, 'f')); QVERIFY(Replica::validRecord(changed));
        QJsonObject upload{{"action", "commit"}, {"entry", changed}, {"manifest", manifest},
            {"snapshot", snapshot.value("id")}, {"inlineData", QString::fromLatin1(QByteArray("original").toBase64())}};
        auto unpinned = upload; unpinned.remove("manifest"); unpinned.remove("snapshot");
        QCOMPARE(host.handle("client", unpinned).value("error"), "manifest_required_before_transfer");
        QCOMPARE(host.handle("client", upload).value("error"), "snapshot_entry_mismatch");
        QVERIFY(!QFileInfo::exists(a.filePath("Files/item")));
        upload.insert("entry", entry); QVERIFY(host.handle("client", upload).value("complete").toBool());
        QVERIFY(host.handle("client", complete).value("ok").toBool());
        QCOMPARE(host.lastSnapshot("client").value("snapshot"), snapshot);
        QCOMPARE(host.handle("stranger", complete).value("error"), "manifest_required_before_transfer");
        const auto receipt = host.lastSnapshot("client"); host.close(); QVERIFY(host.open(a.path(), scope));
        QCOMPARE(host.handle("client", complete).value("receipt"), receipt);
    }
    void transferMustMatchTheAnalyzedSnapshotAndAllEntriesMustComplete_data() {
        QTest::addColumn<QString>("capturedAt");
        QTest::newRow("clock-behind") << QString("1");
        QTest::newRow("clock-ahead") << QString("9007199254740991");
    }
    void hostEditAfterAnalysisPreservesBothFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-race-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-race-client-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope));
        put(a.filePath("Files/item"), "base"); cycle(client, host); put(b.filePath("Files/item"), "client edit");
        bool edited = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &response) {
            if (!edited && response.value("diff").toObject().value("updated").toInt()) {
                put(a.filePath("Files/item"), "host edit after analysis"); edited = true;
            }
        });
        QVERIFY(edited); QCOMPARE(get(a.filePath("Files/item")), "host edit after analysis");
        QCOMPARE(get(b.filePath("Files/item")), "host edit after analysis");
        const auto conflicts = QDir(a.filePath("Files")).entryList({"item.sync-conflict-*"}, QDir::Files);
        QCOMPARE(conflicts.size(), 1); QCOMPARE(get(a.filePath("Files/" + conflicts.front())), "client edit");
        QCOMPARE(get(b.filePath("Files/" + conflicts.front())), "client edit");
    }
    void snapshotPagesKeepOneIdentityAndOmissionNeverDeletesHostFiles() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pages-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-pages-client-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope));
        put(a.filePath("Files/keep"), "host only"); cycle(client, host);
        for (int i = 0; i < 130; ++i) put(b.filePath("Files/item-" + QString::number(i)), QByteArray::number(i));
        QHash<QString, QJsonObject> headers; QHash<QString, int> pages; bool multiple = false;
        cycle(client, host, [&](QJsonObject &request, QJsonObject &) {
            if (request.value("action") != "manifest") return;
            const auto header = request.value("snapshot").toObject(); const auto id = header.value("id").toString();
            if (headers.contains(id)) QCOMPARE(header, headers.value(id)); else headers.insert(id, header);
            multiple |= ++pages[id] > 1;
        });
        QVERIFY(multiple);
        for (int i = 0; i < 130; ++i) QCOMPARE(get(a.filePath("Files/item-" + QString::number(i))), QByteArray::number(i));
        const auto state = host.namespaceState();
        const QJsonObject header{{"schema", 1}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"capturedAt", "1"}, {"baseSequence", state.value("sequence")}, {"baseRevision", state.value("head")}};
        const auto manifest = hash({});
        QVERIFY(host.handle("client", {{"action", "manifest"}, {"manifest", manifest}, {"replica", client.replicaId()},
            {"offset", "0"}, {"total", "0"}, {"after", "0"}, {"through", "0"}, {"entries", QJsonArray{}}, {"snapshot", header}}).value("complete").toBool());
        QVERIFY(host.handle("client", {{"action", "snapshot.complete"}, {"manifest", manifest}, {"snapshot", header.value("id")}}).value("ok").toBool());
        QCOMPARE(get(a.filePath("Files/keep")), "host only"); QCOMPARE(host.namespaceState(), state);
    }
    void integerQueriesDeliverSuccessFailureAndCancellationOnTheReceiverLoop() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-status-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path())); Replica replica; QVERIFY(replica.open(root.path(), scope));
        ManualStatusExecutor loop; Synchronizer sync(&replica); std::vector<int> values;
        QCOMPARE(sync.status().snapshot().value, 0);
        auto subscription = sync.status().subscribe(loop, [&](auto event) { values.push_back(event.value); });
        QVERIFY(subscription); QVERIFY(sync.start("host")); QCOMPARE(sync.status().snapshot().value, 1);
        sync.stop(); QCOMPARE(sync.status().snapshot().value, 4); loop.drain();
        QVERIFY(values == std::vector<int>({1, 4}));
        auto connection = connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto) { sync.receive(id, {{"ok", false}, {"error", "offline"}}); });
        QSignalSpy done(&sync, &Synchronizer::finished); QVERIFY(sync.start("host")); QTRY_COMPARE(done.size(), 1);
        loop.drain(); QCOMPARE(sync.status().snapshot().value, 3); QVERIFY(values == std::vector<int>({1, 4, 1, 3}));
        disconnect(connection);
        bool replied = false;
        auto query = loop.reserve([&] { QCOMPARE(sync.status().snapshot().value, 3); replied = true; });
        QVERIFY(query && query->signal()); QVERIFY(!replied); loop.drain(); QVERIFY(replied); query->cancel();
        Controller controller({}); QCOMPARE(controller.status().snapshot().value, 0);
    }
    void closingTheProducerCancelsQueuedReceiverCalls() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-status-close-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path())); Replica replica; QVERIFY(replica.open(root.path(), scope));
        ManualStatusExecutor loop; int calls = 0;
        auto producer = std::make_unique<Synchronizer>(&replica);
        auto watching = producer->status().subscribe(loop, [&](auto) { ++calls; });
        QVERIFY(watching); QVERIFY(producer->start("host")); producer.reset(); loop.drain();
        QCOMPARE(calls, 0); QVERIFY(!watching);
    }
    void invalidCompletionReceiptFailsAndRetryConvergesWithoutDuplicateConflicts() {
        QTemporaryDir a((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-receipt-host-XXXXXX.societycontainer"))), b((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/snapshot-receipt-client-XXXXXX.societycontainer")));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/item"), "verified upload"); Synchronizer sync(&client); QSignalSpy done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject(); auto response = host.handle("client", request);
            if (request.value("action") == "snapshot.complete") response.insert("receipt", QJsonObject{});
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QVERIFY(!done[0][1].toBool()); QCOMPARE(done[0][2].toString(), "invalid_snapshot_receipt");
        QCOMPARE(sync.status().snapshot().value, 3); QCOMPARE(get(a.filePath("Files/item")), "verified upload");
        cycle(client, host); QCOMPARE(get(b.filePath("Files/item")), "verified upload");
        QVERIFY(QDir(a.filePath("Files")).entryList({"item.sync-conflict-*"}, QDir::Files).isEmpty());
    }
};
QTEST_GUILESS_MAIN(SnapshotTests)
#include "snapshot.moc"
