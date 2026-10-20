#include <iiSocietyHelper/Generation/Host.h>
#include <iiSocietyHelper/Generation/Remote.h>
#include <SharedStorage.h>
#include <QTemporaryDir>
#include <QFile>
#include <QSignalSpy>
#include <QImage>
#include <QTest>
#include <QUuid>
#include <thread>
using namespace iiSocietyHelper::Generation;
namespace {
QJsonObject makeJob(const QString &root) {
    const auto storage = iiSocietyContainer::SharedStorage::open(root);
    return {{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"prompt", "host-only model"},
        {"width", 64}, {"height", 64}, {"steps", 1}, {"seed", 42},
        {"model", storage->models().first().reference(storage->drive().identifier())}};
}
QJsonObject wire(const QJsonObject &job, const QString &action) {
    return {{"op", "society.generation"}, {"schema", 1}, {"action", action}, {"id", job.value("id")}, {"job", job}};
}
bool setup(const QString &root) {
    QString error;
    if (!iiSocietyContainer::SocietyDrive::create(root, &error)) { qWarning().noquote() << error; return false; }
    QFile weights(root + "/Models/host.safetensors");
    return weights.open(QIODevice::WriteOnly) && weights.write("test weights") == 12;
}
iiLocalDiffusion::NativeGenerationResult pixels(const iiLocalDiffusion::NativeGenerationRequest &request) {
    return {std::vector<std::uint8_t>(request.width * request.height * 3, 125), request.width, request.height};
}
}
class GenerationTests : public QObject {
    Q_OBJECT
private slots:
    void serializesMultipleClientsWithoutLosingCompletedJobs() {
        QTemporaryDir root(GENERATION_TEST_DIRECTORY "/host-XXXXXX.societycontainer"); QVERIFY(setup(root.path()));
        std::atomic_int running{0}, peak{0}, runs{0};
        Host host([&](const auto &request, const auto &, const auto &, const auto &) {
            const int count = ++running; peak.store(std::max(peak.load(), count)); ++runs;
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); --running; return pixels(request);
        }); QVERIFY(host.open(root.path()));
        const auto one = makeJob(root.path()), two = makeJob(root.path());
        QVERIFY(host.handle("one", wire(one, "submit")).value("ok").toBool());
        QVERIFY(host.handle("two", wire(two, "submit")).value("ok").toBool());
        QTRY_COMPARE(host.handle("one", wire(one, "status")).value("state").toString(), QString("completed"));
        QTRY_COMPARE(host.handle("two", wire(two, "status")).value("state").toString(), QString("completed"));
        QCOMPARE(runs.load(), 2); QCOMPARE(peak.load(), 1);
        // Package consolidation must preserve the persisted output directory.
        for (const auto &job : {one, two})
            QVERIFY(QFile::exists(root.path() + "/Models/.society-runtime/iiSocietyGeneration/"
                                  + job.value("id").toString() + ".png"));
    }
    void remoteCompletesWithoutAnyLocalModelAndRetriesSubmitOnce() {
        QTemporaryDir root(GENERATION_TEST_DIRECTORY "/host-XXXXXX.societycontainer"); QVERIFY(setup(root.path()));
        std::atomic_int runs{0};
        bool defaultModifiers = true;
        Host host([&](const auto &request, const auto &options, const auto &, const auto &progress) {
            progress({iiLocalDiffusion::NativeGenerationStage::Loading, 5, 10});
            defaultModifiers = options.defaultModifiers; ++runs; return pixels(request);
        });
        QVERIFY(host.open(root.path()));
        const auto job = makeJob(root.path());
        int serial = 0; bool dropped = false; Remote *receiver = nullptr;
        Remote remote([&](const QJsonObject &request) {
            const auto id = QString::number(++serial);
            const auto reply = host.handle("phone", request);
            if (!dropped) { dropped = true; QTimer::singleShot(0, receiver, [receiver] { receiver->setPaused(true); receiver->setPaused(false); }); }
            else QTimer::singleShot(0, receiver, [receiver, id, reply] { receiver->receive(id, reply); });
            return id;
        }); receiver = &remote;
        QSignalSpy accepted(&remote, &Remote::accepted), finished(&remote, &Remote::finished);
        QVERIFY(remote.start(job)); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(accepted.size(), 1); QCOMPARE(runs.load(), 1);
        const auto status = host.handle("phone", wire(job, "status"));
        QCOMPARE(status.value("phase").toString(), QString("loading"));
        QCOMPARE(status.value("step").toInt(), 5); QCOMPARE(status.value("total").toInt(), 10);
        QVERIFY(!defaultModifiers);
        QCOMPARE(finished[0][0].toString(), QString("completed"));
        QCOMPARE(QImage::fromData(finished[0][1].toByteArray()).size(), QSize(64, 64));
        QCOMPARE(finished[0][2].toJsonObject().value("backend").toString(), QString("society-host"));
    }
    void rejectsDifferentOwnerChangedReferenceAndIdReuse() {
        QTemporaryDir root(GENERATION_TEST_DIRECTORY "/host-XXXXXX.societycontainer"); QVERIFY(setup(root.path()));
        Host host([](const auto &request, const auto &, const auto &, const auto &) { return pixels(request); });
        QVERIFY(host.open(root.path())); const auto job = makeJob(root.path());
        QVERIFY(host.handle("phone", wire(job, "submit")).value("ok").toBool());
        QVERIFY(!host.handle("other", wire(job, "status")).value("ok").toBool());
        auto changed = job; changed["prompt"] = "different";
        QCOMPARE(host.handle("phone", wire(changed, "submit")).value("error").toString(), QString("generation_id_conflict"));
        changed["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto model = changed.value("model").toObject(); model["path"] = "../private.safetensors"; changed["model"] = model;
        QVERIFY(!host.handle("phone", wire(changed, "submit")).value("ok").toBool());
        changed["model"] = job.value("model"); changed["width"] = 64.5;
        QVERIFY(!host.handle("phone", wire(changed, "submit")).value("ok").toBool());
    }
    void cancellationBeforeSubmitAndDuringInference() {
        QTemporaryDir root(GENERATION_TEST_DIRECTORY "/host-XXXXXX.societycontainer"); QVERIFY(setup(root.path()));
        std::atomic_int runs{0};
        Host host([&](const auto &, const auto &, const auto &cancelled, const auto &) {
            ++runs; while (!cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            iiLocalDiffusion::NativeGenerationResult result; result.cancelled = true; return result;
        }); QVERIFY(host.open(root.path()));
        const auto cancelled = makeJob(root.path());
        QCOMPARE(host.handle("phone", wire(cancelled, "cancel")).value("state").toString(), QString("cancelled"));
        QCOMPARE(host.handle("phone", wire(cancelled, "submit")).value("state").toString(), QString("cancelled"));
        const auto active = makeJob(root.path()); QVERIFY(host.handle("phone", wire(active, "submit")).value("ok").toBool());
        QTRY_COMPARE(runs.load(), 1);
        QCOMPARE(host.handle("phone", wire(active, "cancel")).value("state").toString(), QString("cancelling"));
        QTRY_COMPARE(host.handle("phone", wire(active, "status")).value("state").toString(), QString("cancelled"));
    }
    void rejectsCorruptImageWithoutReportingCompletion() {
        QTemporaryDir root(GENERATION_TEST_DIRECTORY "/host-XXXXXX.societycontainer"); QVERIFY(setup(root.path()));
        Host host([](const auto &request, const auto &, const auto &, const auto &) { return pixels(request); });
        QVERIFY(host.open(root.path())); int serial = 0; Remote *receiver = nullptr;
        Remote remote([&](const QJsonObject &request) {
            const auto id = QString::number(++serial); auto reply = host.handle("phone", request);
            if (request.value("action") == "read") {
                auto bytes = QByteArray::fromBase64(reply.value("data").toString().toLatin1()); bytes[0] ^= 1;
                reply["data"] = QString::fromLatin1(bytes.toBase64());
            }
            QTimer::singleShot(0, receiver, [receiver, id, reply] { receiver->receive(id, reply); }); return id;
        }); receiver = &remote;
        QSignalSpy finished(&remote, &Remote::finished); QVERIFY(remote.start(makeJob(root.path())));
        QTRY_COMPARE(finished.size(), 1); QCOMPARE(finished[0][0].toString(), QString("failed"));
        QVERIFY(finished[0][1].toByteArray().isEmpty()); QVERIFY(finished[0][3].toString().contains("checksum"));
    }
};
QTEST_GUILESS_MAIN(GenerationTests)
#include "generation.moc"
