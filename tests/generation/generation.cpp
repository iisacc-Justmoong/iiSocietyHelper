#include "../TestWorkspace.h"
#include <iiSocietyHelper/Generation/Host.h>
#include <iiSocietyHelper/Generation/Remote.h>
#include <SharedStorage.h>
#include <QTemporaryDir>
#include <QFile>
#include "../SignalRecorder.h"
#include <QCoreApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QScopeGuard>
#include <QImage>
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
public:
    void destructionDoesNotWaitForAnUnresponsiveInference() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-stop-XXXXXX.societycontainer")));
        CTEST_CHECK(setup(root.path()));
        auto entered = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        auto finished = std::make_shared<QSemaphore>();
        const auto unblock = qScopeGuard([release] { release->release(); });
        // A bounded watchdog makes the regression fail rather than hang when
        // a destructor accidentally waits for the blocked engine.
        std::jthread watchdog([release](std::stop_token stop) {
            for (int i = 0; i < 300 && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!stop.stop_requested()) release->release();
        });
        auto host = std::make_unique<Host>([=](const auto &, const auto &, const auto &, const auto &progress) {
            entered->release(); release->acquire();
            progress({iiLocalDiffusion::NativeGenerationStage::Loading, 1, 1});
            finished->release();
            iiLocalDiffusion::NativeGenerationResult result; result.cancelled = true; return result;
        });
        CTEST_CHECK(host->open(root.path()));
        const auto job = makeJob(root.path());
        CTEST_CHECK(host->handle("phone", wire(job, "submit")).value("ok").toBool());
        CTEST_WAIT_VERIFY(entered->available() > 0, 3000);
        QElapsedTimer elapsed; elapsed.start();
        host->shutdownAsync();
        host.reset();
        CTEST_CHECK_MESSAGE(elapsed.elapsed() < 500, "Host destruction waited for inference");
        release->release();
        CTEST_WAIT_VERIFY(finished->available() > 0, 3000);
        QCoreApplication::processEvents();
        CTEST_CHECK(!QFile::exists(root.path() + "/Models/.society-runtime/iiSocietyGeneration/"
                               + job.value("id").toString() + ".png"));
    }
    void serializesMultipleClientsWithoutLosingCompletedJobs() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))); CTEST_CHECK(setup(root.path()));
        std::atomic_int running{0}, peak{0}, runs{0};
        Host host([&](const auto &request, const auto &, const auto &, const auto &) {
            const int count = ++running; peak.store(std::max(peak.load(), count)); ++runs;
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); --running; return pixels(request);
        }); CTEST_CHECK(host.open(root.path()));
        const auto one = makeJob(root.path()), two = makeJob(root.path());
        CTEST_CHECK(host.handle("one", wire(one, "submit")).value("ok").toBool());
        CTEST_CHECK(host.handle("two", wire(two, "submit")).value("ok").toBool());
        CTEST_WAIT_COMPARE(host.handle("one", wire(one, "status")).value("state").toString(), QString("completed"));
        CTEST_WAIT_COMPARE(host.handle("two", wire(two, "status")).value("state").toString(), QString("completed"));
        CTEST_COMPARE(runs.load(), 2); CTEST_COMPARE(peak.load(), 1);
        // Package consolidation must preserve the persisted output directory.
        for (const auto &job : {one, two})
            CTEST_CHECK(QFile::exists(root.path() + "/Models/.society-runtime/iiSocietyGeneration/"
                                  + job.value("id").toString() + ".png"));
    }
    void remoteCompletesWithoutAnyLocalModelAndRetriesSubmitOnce() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))); CTEST_CHECK(setup(root.path()));
        std::atomic_int runs{0};
        bool defaultModifiers = true;
        Host host([&](const auto &request, const auto &options, const auto &, const auto &progress) {
            progress({iiLocalDiffusion::NativeGenerationStage::Loading, 5, 10});
            defaultModifiers = options.defaultModifiers; ++runs; return pixels(request);
        });
        CTEST_CHECK(host.open(root.path()));
        const auto job = makeJob(root.path());
        int serial = 0; bool dropped = false; Remote *receiver = nullptr;
        Remote remote([&](const QJsonObject &request) {
            const auto id = QString::number(++serial);
            const auto reply = host.handle("phone", request);
            if (!dropped) { dropped = true; QTimer::singleShot(0, receiver, [receiver] { receiver->setPaused(true); receiver->setPaused(false); }); }
            else QTimer::singleShot(0, receiver, [receiver, id, reply] { receiver->receive(id, reply); });
            return id;
        }); receiver = &remote;
        helper_ctest::SignalRecorder accepted(&remote, &Remote::accepted);
        helper_ctest::SignalRecorder finished(&remote, &Remote::finished);
        CTEST_CHECK(remote.start(job)); CTEST_WAIT_COMPARE_TIMEOUT(finished.size(), 1, 10000);
        CTEST_COMPARE(accepted.size(), 1); CTEST_COMPARE(runs.load(), 1);
        const auto status = host.handle("phone", wire(job, "status"));
        CTEST_COMPARE(status.value("phase").toString(), QString("loading"));
        CTEST_COMPARE(status.value("step").toInt(), 5); CTEST_COMPARE(status.value("total").toInt(), 10);
        CTEST_CHECK(!defaultModifiers);
        CTEST_COMPARE(finished[0][0].toString(), QString("completed"));
        CTEST_COMPARE(QImage::fromData(finished[0][1].toByteArray()).size(), QSize(64, 64));
        CTEST_COMPARE(finished[0][2].toJsonObject().value("backend").toString(), QString("society-host"));
    }
    void rejectsDifferentOwnerChangedReferenceAndIdReuse() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))); CTEST_CHECK(setup(root.path()));
        Host host([](const auto &request, const auto &, const auto &, const auto &) { return pixels(request); });
        CTEST_CHECK(host.open(root.path())); const auto job = makeJob(root.path());
        CTEST_CHECK(host.handle("phone", wire(job, "submit")).value("ok").toBool());
        CTEST_CHECK(!host.handle("other", wire(job, "status")).value("ok").toBool());
        auto changed = job; changed["prompt"] = "different";
        CTEST_COMPARE(host.handle("phone", wire(changed, "submit")).value("error").toString(), QString("generation_id_conflict"));
        changed["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto model = changed.value("model").toObject(); model["path"] = "../private.safetensors"; changed["model"] = model;
        CTEST_CHECK(!host.handle("phone", wire(changed, "submit")).value("ok").toBool());
        changed["model"] = job.value("model"); changed["width"] = 64.5;
        CTEST_CHECK(!host.handle("phone", wire(changed, "submit")).value("ok").toBool());
    }
    void cancellationBeforeSubmitAndDuringInference() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))); CTEST_CHECK(setup(root.path()));
        std::atomic_int runs{0};
        Host host([&](const auto &, const auto &, const auto &cancelled, const auto &) {
            ++runs; while (!cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            iiLocalDiffusion::NativeGenerationResult result; result.cancelled = true; return result;
        }); CTEST_CHECK(host.open(root.path()));
        const auto cancelled = makeJob(root.path());
        CTEST_COMPARE(host.handle("phone", wire(cancelled, "cancel")).value("state").toString(), QString("cancelled"));
        CTEST_COMPARE(host.handle("phone", wire(cancelled, "submit")).value("state").toString(), QString("cancelled"));
        const auto active = makeJob(root.path()); CTEST_CHECK(host.handle("phone", wire(active, "submit")).value("ok").toBool());
        CTEST_WAIT_COMPARE(runs.load(), 1);
        CTEST_COMPARE(host.handle("phone", wire(active, "cancel")).value("state").toString(), QString("cancelling"));
        CTEST_WAIT_COMPARE(host.handle("phone", wire(active, "status")).value("state").toString(), QString("cancelled"));
    }
    void rejectsCorruptImageWithoutReportingCompletion() {
        QTemporaryDir root((society_test::root(QString::fromUtf8(GENERATION_TEST_DIRECTORY)) + QStringLiteral("/host-XXXXXX.societycontainer"))); CTEST_CHECK(setup(root.path()));
        Host host([](const auto &request, const auto &, const auto &, const auto &) { return pixels(request); });
        CTEST_CHECK(host.open(root.path())); int serial = 0; Remote *receiver = nullptr;
        Remote remote([&](const QJsonObject &request) {
            const auto id = QString::number(++serial); auto reply = host.handle("phone", request);
            if (request.value("action") == "read") {
                auto bytes = QByteArray::fromBase64(reply.value("data").toString().toLatin1()); bytes[0] ^= 1;
                reply["data"] = QString::fromLatin1(bytes.toBase64());
            }
            QTimer::singleShot(0, receiver, [receiver, id, reply] { receiver->receive(id, reply); }); return id;
        }); receiver = &remote;
        helper_ctest::SignalRecorder finished(&remote, &Remote::finished); CTEST_CHECK(remote.start(makeJob(root.path())));
        CTEST_WAIT_COMPARE(finished.size(), 1); CTEST_COMPARE(finished[0][0].toString(), QString("failed"));
        CTEST_CHECK(finished[0][1].toByteArray().isEmpty()); CTEST_CHECK(finished[0][3].toString().contains("checksum"));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    GenerationTests tests;
    runner.run("destructionDoesNotWaitForAnUnresponsiveInference", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.destructionDoesNotWaitForAnUnresponsiveInference(); });
    runner.run("serializesMultipleClientsWithoutLosingCompletedJobs", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.serializesMultipleClientsWithoutLosingCompletedJobs(); });
    runner.run("remoteCompletesWithoutAnyLocalModelAndRetriesSubmitOnce", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.remoteCompletesWithoutAnyLocalModelAndRetriesSubmitOnce(); });
    runner.run("rejectsDifferentOwnerChangedReferenceAndIdReuse", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.rejectsDifferentOwnerChangedReferenceAndIdReuse(); });
    runner.run("cancellationBeforeSubmitAndDuringInference", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.cancellationBeforeSubmitAndDuringInference(); });
    runner.run("rejectsCorruptImageWithoutReportingCompletion", [&] { const auto cleanup = qScopeGuard([] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }); tests.rejectsCorruptImageWithoutReportingCompletion(); });
    return runner.result();
}
