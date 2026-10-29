#include <iiFileProviderDevice.h>
#include "Host.h"
#include <SharedStorage.h>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QPointer>
#include <QImage>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <deque>

namespace iiSocietyHelper::Generation {
namespace {
QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }
bool identifier(const QString &id) { return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id; }
bool integer(const QJsonValue &value, int minimum, int maximum) {
    return value.isDouble() && value.toDouble() == value.toInt(-1) && value.toInt() >= minimum && value.toInt() <= maximum;
}
QString phase(iiLocalDiffusion::NativeGenerationStage stage) {
    using Stage = iiLocalDiffusion::NativeGenerationStage;
    switch (stage) {
    case Stage::Waiting: return "waiting";
    case Stage::Preparing: return "preparing";
    case Stage::Loading: return "loading";
    case Stage::Encoding: return "encoding";
    case Stage::Denoising: return "denoising";
    case Stage::Decoding: return "decoding";
    case Stage::Computing: return "computing";
    }
    return {};
}
}
class Host::Private {
public:
    struct Job {
        QString peer;
        QJsonObject input, status;
        QString output;
        qint64 touched = QDateTime::currentMSecsSinceEpoch();
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    };
    struct Result { QString id, error; QByteArray hash; qint64 bytes = 0; bool cancelled = false; quint64 epoch = 0; };
    Host *q;
    Engine engine;
    std::optional<iiSocietyContainer::SharedStorage> storage;
    QMap<QString, Job> jobs;
    std::deque<QString> queue;
    QFutureWatcher<Result> worker;
    QString active;
    quint64 epoch = 0;
    Private(Host *owner, Engine run) : q(owner), engine(std::move(run)) {
        if (!engine) engine = [](const auto &request, const auto &options, const auto &cancelled, const auto &progress) {
            return iiLocalDiffusion::generateNativeImageWithResidentWeights(request, options, cancelled, progress);
        };
        QObject::connect(&worker, &QFutureWatcher<Result>::finished, q, [this] {
            const auto result = worker.result(); active.clear();
            auto it = jobs.find(result.id);
            if (result.epoch == epoch && it != jobs.end()) {
                auto &job = it.value();
                const bool cancelled = result.cancelled || *job.cancelled;
                job.status["state"] = cancelled ? "cancelled" : result.error.isEmpty() ? "completed" : "failed";
                job.status["error"] = result.error;
                if (!cancelled && result.error.isEmpty()) {
                    job.status["bytes"] = double(result.bytes);
                    job.status["sha256"] = QString::fromLatin1(result.hash);
                }
            }
            pump();
        });
    }
    void pump() {
        if (!storage || !active.isEmpty() || worker.isRunning() || queue.empty()) return;
        const auto id = queue.front(); queue.pop_front();
        auto it = jobs.find(id);
        if (it == jobs.end() || *it->cancelled) { QTimer::singleShot(0, q, [this] { pump(); }); return; }
        auto &job = it.value();
        QString error;
        const auto model = storage->resolveModel(job.input.value("model").toObject(), &error);
        const auto runtime = storage->ensureDirectory(iiSocietyContainer::StoreSection::Models,
            ".society-runtime/iiSocietyGeneration", &error);
        const auto resources = storage->ensureDirectory(iiSocietyContainer::StoreSection::Models,
            ".generation-resources/iiLocalDiffusion", &error);
        const auto output = storage->filePath(iiSocietyContainer::StoreSection::Models,
            ".society-runtime/iiSocietyGeneration/" + id + ".png", &error);
        if (model.isEmpty() || runtime.isEmpty() || resources.isEmpty() || output.isEmpty()) {
            job.status["state"] = "failed"; job.status["error"] = error;
            QTimer::singleShot(0, q, [this] { pump(); }); return;
        }
        iiLocalDiffusion::NativeGenerationRequest request;
        request.modelPath = iiFileProvider::FileDevice::encodeName(model).toStdString();
        request.prompt = job.input.value("prompt").toString().toStdString();
        request.width = job.input.value("width").toInt(); request.height = job.input.value("height").toInt();
        request.steps = job.input.value("steps").toInt();
        request.seed = job.input.value("seed").toInteger();
        iiLocalDiffusion::NativeGenerationOptions options;
        options.resourceDirectory = iiFileProvider::FileDevice::encodeName(resources).toStdString();
        options.defaultModifiers = iiFileProvider::FileDevice::exists(resources + "/generation-defaults.json");
        job.output = output;
        job.status["state"] = "running";
        job.status["generation"] = QJsonObject{{"backend", "society-host"}, {"seed", double(request.seed)},
            {"model", job.input.value("model")}, {"width", request.width}, {"height", request.height}, {"steps", request.steps}};
        active = id;
        const auto currentEpoch = epoch;
        const auto cancelled = job.cancelled;
        const auto run = engine;
        const QPointer<Host> self(q);
        worker.setFuture(QtConcurrent::run([self, run, request, options, cancelled, id, output, currentEpoch] {
            Result result; result.id = id; result.epoch = currentEpoch;
            try {
                const auto pixels = run(request, options, *cancelled, [self, id, currentEpoch](const auto &event) {
                    if (self) QMetaObject::invokeMethod(self, [self, id, currentEpoch, event] {
                        if (!self || self->d->epoch != currentEpoch || !self->d->jobs.contains(id)) return;
                        auto &status = self->d->jobs[id].status;
                        status["phase"] = phase(event.stage);
                        status["step"] = event.step; status["total"] = event.total;
                    }, Qt::QueuedConnection);
                });
                result.cancelled = *cancelled || pixels.cancelled;
                if (result.cancelled) return result;
                result.error = QString::fromStdString(pixels.error);
                if (!result.error.isEmpty()) return result;
                if (pixels.width != request.width || pixels.height != request.height
                    || pixels.rgb.size() != size_t(request.width) * request.height * 3) {
                    result.error = "The host engine returned invalid image dimensions."; return result;
                }
                QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
                const QImage image(pixels.rgb.data(), pixels.width, pixels.height, pixels.width * 3, QImage::Format_RGB888);
                if (!image.save(&buffer, "PNG") || png.size() > 64 * 1024 * 1024) {
                    result.error = "Cannot encode the generated host image."; return result;
                }
                if (*cancelled) { result.cancelled = true; return result; }
                if (iiFileProvider::FileDevice::exists(output)) { result.error = "A host result already exists for this job."; return result; }
                iiFileProvider::SaveFile file(output);
                if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit()) {
                    result.error = "Cannot save the generated host image."; return result;
                }
                result.bytes = png.size(); result.hash = QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex();
            } catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
            catch (...) { result.error = "The host inference engine failed."; }
            return result;
        }));
    }
};
Host::Host(Engine engine, QObject *parent) : QObject(parent), d(std::make_unique<Private>(this, std::move(engine))) {}
Host::~Host() { close(); d->worker.waitForFinished(); }
bool Host::open(const QString &container) {
    if (d->storage && d->storage->drive().rootPath() == container && d->storage->drive().isReady()) return true;
    close(); d->storage = iiSocietyContainer::SharedStorage::open(container); return d->storage.has_value();
}
void Host::close() {
    ++d->epoch;
    for (auto &job : d->jobs) *job.cancelled = true;
    d->storage.reset(); d->jobs.clear(); d->queue.clear();
}
QJsonObject Host::handle(const QString &peer, const QJsonObject &request) {
    if (peer.isEmpty() || !d->storage) return failure("generation_host_unavailable");
    const auto id = request.value("id").toString(), action = request.value("action").toString();
    if (request.value("op") != "society.generation" || request.value("schema").toInt() != 1 || !identifier(id))
        return failure("invalid_generation_request");
    auto it = d->jobs.find(id);
    if (it != d->jobs.end() && it->peer != peer) return failure("generation_owner_mismatch");
    if (action == "submit") {
        const auto input = request.value("job").toObject();
        if (it != d->jobs.end()) {
            if (!it->input.isEmpty() && it->input != input) return failure("generation_id_conflict");
            return it->status;
        }
        const auto model = input.value("model").toObject();
        if (input.value("id").toString() != id || input.value("prompt").toString().trimmed().isEmpty()
            || input.value("prompt").toString().size() > 32000
            || !integer(input.value("width"), 64, 4096) || input.value("width").toInt() % 8
            || !integer(input.value("height"), 64, 4096) || input.value("height").toInt() % 8
            || !integer(input.value("steps"), 1, 1000)) return failure("invalid_generation_parameters");
        if (model.value("format") != "safetensors" && model.value("format") != "unified")
            return failure("remote_generation_model_format_unsupported");
        QString error;
        if (d->storage->resolveModel(model, &error).isEmpty()) return failure(error);
        if (d->queue.size() >= 64 || d->jobs.size() >= 4096) return failure("generation_queue_full");
        Private::Job job; job.peer = peer; job.input = input;
        job.status = {{"ok", true}, {"schema", 1}, {"id", id}, {"state", "queued"}};
        d->jobs.insert(id, job); d->queue.push_back(id);
        QTimer::singleShot(0, this, [this] { d->pump(); }); return job.status;
    }
    if (action == "cancel" && it == d->jobs.end()) {
        if (d->jobs.size() >= 4096) return failure("generation_queue_full");
        Private::Job job; job.peer = peer; *job.cancelled = true;
        job.status = {{"ok", true}, {"schema", 1}, {"id", id}, {"state", "cancelled"}};
        d->jobs.insert(id, job); return job.status;
    }
    if (it == d->jobs.end()) return failure("generation_job_not_found");
    it->touched = QDateTime::currentMSecsSinceEpoch();
    if (action == "status") return it->status;
    if (action == "cancel") {
        const auto state = it->status.value("state").toString();
        if (state == "queued" || state == "running" || state == "cancelling") {
            *it->cancelled = true; it->status["state"] = state == "queued" ? "cancelled" : "cancelling";
        }
        return it->status;
    }
    if (action == "read" && it->status.value("state") == "completed") {
        const auto offset = request.value("offset").toInteger(-1);
        const auto path = d->storage->filePath(iiSocietyContainer::StoreSection::Models,
            ".society-runtime/iiSocietyGeneration/" + id + ".png");
        iiFileProvider::FileDevice file(path);
        if (offset < 0 || offset >= it->status.value("bytes").toInteger() || path.isEmpty()
            || !file.open(QIODevice::ReadOnly) || !file.seek(offset)) return failure("invalid_generation_result_offset");
        const auto data = file.read(192 * 1024);
        if (data.isEmpty()) return failure("generation_result_unavailable");
        return {{"ok", true}, {"schema", 1}, {"id", id}, {"offset", double(offset)}, {"data", QString::fromLatin1(data.toBase64())}};
    }
    return failure("invalid_generation_action");
}
}
