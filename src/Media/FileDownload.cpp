#include <iiSocietyHelper/FileDownload.h>
#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper.h>
#include <iiSocietyHelper/Sync/Sync.h>
#include <StorageMap.h>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>

namespace iiSocietyHelper {
namespace {
QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }
class DownloadWorker final : public QObject {
public:
    std::unique_ptr<iiSocietyHelper::Sync::Replica> replica;
    QJsonObject entry, binding;
    std::shared_ptr<std::atomic_bool> cancellation;
    void close() { replica.reset(); entry = {}; binding = {}; }
    QJsonObject retryBusy(const std::function<QJsonObject()> &operation) {
        QElapsedTimer wait; wait.start();
        for (;;) {
            if (cancellation && cancellation->load()) return failure("cancelled");
            auto result = operation();
            if (result.value("error") != "sync_store_busy" || wait.elapsed() >= 2000) return result;
            QThread::msleep(10);
        }
    }
    QJsonObject handle(const QString &action, QJsonObject message = {}) {
        if (!replica) return failure("download_closed");
        message.insert("action", action); message.insert("entry", entry);
        message.insert("expectedVersion", entry.value("version"));
        return retryBusy([&] { return replica->handle(binding.value("host").toString(), message, false); });
    }
    QJsonObject prepare(const QString &root, const QString &id, const QString &key,
                        const QString &version, const std::shared_ptr<std::atomic_bool> &cancelled) {
        close(); cancellation = cancelled; binding = iiSocietyHelper::Sync::Replica::binding(root);
        if (binding.value("container") != id || !binding.value("complete").toBool()) return failure("download_replica_unavailable");
        replica = std::make_unique<iiSocietyHelper::Sync::Replica>();
        replica->setCancellation([cancelled] { return cancelled->load(); });
        if (!replica->open(root, binding.value("scope").toString())) return failure(replica->errorString());
        if (replica->containerId() != id || replica->binding() != binding) return failure("remote_container_changed");
        entry = replica->record(key);
        if (entry.value("kind") != "file" || entry.value("version") != version) return failure("object_changed_select_again");
        auto result = handle("begin");
        result.insert("size", entry.value("size")); result.insert("binding", binding);
        return result;
    }
    QJsonObject commit() {
        auto result = handle("commit");
        if (result.value("ok").toBool()) result = retryBusy([&] {
            return replica->publishStorageMap() ? QJsonObject{{"ok", true}} : failure(replica->errorString());
        });
        return result;
    }
};
}

class FileDownload::Private {
public:
    FileDownload *q;
    QPointer<FileSystem> files;
    QPointer<Client> client;
    QThread thread;
    DownloadWorker *worker = nullptr;
    QTimer deadline;
    QElapsedTimer progressClock;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(true);
    quint64 epoch = 0;
    QVariantMap value{{"state", "idle"}};
    QString root, id, key, request, nativeVersion;
    QJsonObject binding;
    qint64 offset = 0, total = 0;
    bool active = false, prepared = false, pending = false;
    Private(FileDownload *owner, FileSystem *storage, Client *transport) : q(owner), files(storage), client(transport) {
        deadline.setSingleShot(true); deadline.setInterval(30000);
        QObject::connect(&deadline, &QTimer::timeout, q, [this] { finish("failed", "download_timeout"); });
        if (client) {
            QObject::connect(client, &Client::changed, q, [this] { advance(); });
            QObject::connect(client, &Client::hostDriveResponse, q, [this](const QString &token, const QJsonObject &response) {
                if (!active || token != request) return;
                request.clear();
                if (!response.value("ok").toBool()) { finish("failed", response.value("error").toString()); return; }
                if (!sameStorage()) { finish("cancelled", "remote_container_changed"); return; }
                bool validSize = false;
                const auto size = response.value("size").toString().toLongLong(&validSize);
                const auto version = response.value("version").toString();
                if (response.value("path") != key || response.value("kind") != "file" || !validSize || size != total
                    || version.isEmpty() || (!nativeVersion.isEmpty() && version != nativeVersion)) {
                    finish("failed", "invalid_download_response"); return;
                }
                if (nativeVersion.isEmpty()) { nativeVersion = version; advance(); return; }
                bool validOffset = false;
                const auto position = response.value("offset").toString().toLongLong(&validOffset);
                const auto encoded = response.value("data").toString().toLatin1();
                if (encoded.size() > ((iiSocietyHelper::Sync::Replica::ChunkBytes + 2) / 3) * 4) { finish("failed", "invalid_download_chunk"); return; }
                const auto bytes = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (!validOffset || position != offset || !response.value("data").isString() || !bytes
                    || bytes.decoded.size() != qMin(iiSocietyHelper::Sync::Replica::ChunkBytes, total - offset)
                    || bytes.decoded.toBase64() != encoded || !response.value("eof").isBool()
                    || response.value("eof").toBool() != (offset + bytes.decoded.size() == total)) {
                    finish("failed", "invalid_download_chunk"); return;
                }
                const auto next = offset + bytes.decoded.size();
                post([position, encoded](DownloadWorker &io) { return io.handle("chunk", {{"offset", QString::number(position)}, {"data", QString::fromLatin1(encoded)}}); },
                    [this, next](const QJsonObject &result) {
                        if (!accepted(result)) return;
                        offset = next; deadline.start();
                        if (!progressClock.isValid() || progressClock.elapsed() >= 75 || offset == total) {
                            progressClock.start(); value["completed"] = offset; emit q->changed();
                        }
                        advance();
                    });
            });
        }
        if (files) QObject::connect(files->container(), &iiSocietyContainer::Container::stateChanged, q, [this] {
            if (active && !sameStorage()) finish("cancelled", "remote_container_changed");
        });
    }
    ~Private() {
        cancelled->store(true); deadline.stop();
        if (thread.isRunning()) { thread.quit(); thread.wait(); }
    }
    bool sameStorage() const { return files && files->isAvailable() && files->containerId() == id && files->rootPath() == root; }
    void closeWorker() {
        if (worker) QMetaObject::invokeMethod(worker, [io = worker] { io->close(); }, Qt::QueuedConnection);
    }
    void post(std::function<QJsonObject(DownloadWorker &)> operation, std::function<void(QJsonObject)> done) {
        if (!worker) {
            worker = new DownloadWorker; worker->moveToThread(&thread);
            QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater); thread.start();
        }
        pending = true;
        const auto generation = epoch; const auto token = cancelled; QPointer<FileDownload> guard(q);
        QMetaObject::invokeMethod(worker, [this, io = worker, guard, generation, token, operation = std::move(operation), done = std::move(done)] {
            const auto result = token->load() ? failure("cancelled") : operation(*io);
            if (guard) QMetaObject::invokeMethod(guard, [this, guard, generation, token, result, done] {
                if (!guard || generation != epoch || token->load()) return;
                pending = false; done(result);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }
    bool accepted(const QJsonObject &result) {
        if (result.value("ok").toBool()) return true;
        finish("failed", result.value("error").toString("download_failed")); return false;
    }
    void finish(const QString &state, const QString &error = {}) {
        active = false; prepared = false; pending = false; request.clear(); deadline.stop(); cancelled->store(true);
        value["state"] = state; value["error"] = error; value["completed"] = offset;
        if (client) client->stop();
        closeWorker(); emit q->changed();
    }
    void complete() {
        if (!sameStorage()) { finish("cancelled", "remote_container_changed"); return; }
        files->synchronize();
        const auto file = files->alias(key.section('/', 0, 0), key.section('/', 1));
        if (!file.value("available").toBool() || !file.value("exists").toBool() || file.value("containerId") != id) {
            finish("failed", "downloaded_file_unavailable"); return;
        }
        offset = total; finish("ready"); emit q->ready(file);
    }
    void advance() {
        if (!active || !prepared || pending || !request.isEmpty() || !client) return;
        if (!sameStorage()) { finish("cancelled", "remote_container_changed"); return; }
        if (!client->connected()) return;
        const auto session = client->hostBinding();
        for (const auto *field : {"scope", "host", "container"})
            if (session.value(field) != binding.value(field)) { finish("failed", "remote_container_changed"); return; }
        value["state"] = "downloading"; emit q->changed();
        if (nativeVersion.isEmpty()) request = client->statHostEntry(key);
        else if (offset < total) request = client->readHostFile(key, offset, nativeVersion);
        else {
            value["state"] = "verifying"; emit q->changed();
            deadline.start(120000);
            post([](DownloadWorker &io) { return io.commit(); }, [this](const QJsonObject &result) { if (accepted(result)) complete(); });
            return;
        }
        if (request.isEmpty()) finish("failed", "host_transport_unavailable");
        else deadline.start();
    }
};

FileDownload::FileDownload(FileSystem *files, Client *client, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, files, client)) {}
FileDownload::~FileDownload() = default;
QVariantMap FileDownload::state() const { return d->value; }
bool FileDownload::active() const { return d->active; }
void FileDownload::cancel() { if (d->active) d->finish("cancelled"); }
bool FileDownload::start(const QString &key, const QString &containerId) {
    if (d->active && d->key == key && d->id == containerId && d->sameStorage()) return true;
    cancel(); ++d->epoch; d->cancelled = std::make_shared<std::atomic_bool>(false);
    d->key = key; d->id = containerId; d->root = d->files ? d->files->rootPath() : QString{};
    d->offset = d->total = 0; d->nativeVersion.clear(); d->binding = {}; d->progressClock.invalidate();
    d->value = {{"state", "preparing"}, {"key", key}, {"containerId", containerId}, {"completed", 0}, {"total", 0}};
    if (!d->sameStorage() || iiSocietyContainer::StorageMap::physicalPath(key).isEmpty()) {
        d->finish("failed", "remote_container_changed"); return false;
    }
    const auto file = d->files->alias(key.section('/', 0, 0), key.section('/', 1));
    d->value["name"] = file.value("name");
    if (!file.value("exists").toBool()) { d->finish("failed", "download_file_not_found"); return false; }
    d->active = true; d->deadline.start(); emit changed();
    if (file.value("available").toBool()) {
        const auto epoch = d->epoch;
        QTimer::singleShot(0, this, [this, epoch] { if (d->active && d->epoch == epoch) d->complete(); });
        return true;
    }
    const auto root = d->root, id = d->id, version = file.value("version").toString(); const auto token = d->cancelled;
    d->post([root, id, key, version, token](DownloadWorker &io) { return io.prepare(root, id, key, version, token); },
        [this](const QJsonObject &result) {
            if (!d->accepted(result)) return;
            bool validSize = false, validOffset = false;
            d->total = result.value("size").toString().toLongLong(&validSize);
            d->offset = result.value("offset").toString("0").toLongLong(&validOffset);
            if (!validSize || !validOffset || d->offset < 0 || d->total < d->offset) { d->finish("failed", "invalid_download_size"); return; }
            d->value["total"] = d->total; d->value["completed"] = d->offset;
            if (result.value("complete").toBool()) { d->complete(); return; }
            d->binding = result.value("binding").toObject(); d->prepared = true; d->value["state"] = "connecting"; emit changed();
            if (!d->client || !d->client->connectHost()) { d->finish("failed", "host_session_unavailable"); return; }
            d->advance();
        });
    return true;
}
}
