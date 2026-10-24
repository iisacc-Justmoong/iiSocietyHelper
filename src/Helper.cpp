#include <iiFileProviderDevice.h>
#include <iiFileProviderNative.h>
#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include "LocalDirectory.h"
#include "PresenceWorker_p.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <SharedStorage.h>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <utility>
#include <exception>

namespace iiSocietyHelper {
Q_LOGGING_CATEGORY(presenceLog, "iisacc.society.helper")
#ifdef Q_OS_DARWIN
QString appleObservationDirectory(QString *error);
#endif
namespace {
constexpr qint64 maximumRecordBytes = 8192;

QString activityName(Activity activity)
{
    switch (activity) {
    case Activity::Foreground: return QStringLiteral("foreground");
    case Activity::Background: return QStringLiteral("background");
    default: return QStringLiteral("unknown");
    }
}

bool validApplication(const ApplicationInfo &app)
{
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)+$"));
    return app.id.size() <= 255 && id.match(app.id).hasMatch()
        && !app.name.trimmed().isEmpty() && app.name.size() <= 256 && app.version.size() <= 128;
}

bool sameMetadata(const Peer &a, const Peer &b)
{
    return a.application == b.application && a.processId == b.processId
        && a.activity == b.activity && a.startedAt == b.startedAt;
}

struct Record {
    Peer peer;
    quint64 sequence = 0;
};

bool readRecord(const QFileInfo &info, Record *result)
{
    if (!info.isFile() || info.isSymLink() || info.size() > maximumRecordBytes) return false;
    const auto instance = info.completeBaseName();
    if (QUuid(instance).isNull() || QUuid(instance).toString(QUuid::WithoutBraces) != instance) return false;
    QByteArray bytes;
    try {
        iiFileProvider::native::ReadFile file(std::filesystem::path(info.absoluteFilePath().toStdU16String()));
        // Read the pinned snapshot to its actual EOF. The pathname's size can
        // belong to the previous heartbeat while atomic replacement is racing.
        const auto data = file.read(maximumRecordBytes + 1);
        if (data.size() > maximumRecordBytes) return false;
        bytes = QByteArray::fromStdString(data);
    } catch (const iiFileProvider::native::FileError &) { return false; }
    const auto object = QJsonDocument::fromJson(bytes).object();
    if (object.value("protocol").toInt() != 1 || object.value("instanceId").toString() != instance) return false;
    Record record;
    record.peer.instanceId = instance;
    record.peer.application = {object.value("applicationId").toString(), object.value("name").toString(),
                               object.value("version").toString()};
    record.peer.processId = object.value("processId").toInteger();
    record.peer.startedAt = QDateTime::fromString(object.value("startedAt").toString(), Qt::ISODateWithMs);
    const auto activity = object.value("activity").toString();
    if (activity == "foreground") record.peer.activity = Activity::Foreground;
    else if (activity == "background") record.peer.activity = Activity::Background;
    else if (activity != "unknown") return false;
    bool sequenceOk = false;
    record.sequence = object.value("heartbeat").toString().toULongLong(&sequenceOk);
    if (!validApplication(record.peer.application) || record.peer.processId <= 0
        || !record.peer.startedAt.isValid() || !sequenceOk || record.sequence == 0) return false;
    *result = record;
    return true;
}

bool intactRoot(const QString &root)
{
    const QFileInfo info(root);
    return info.isDir() && !info.isSymLink() && info.canonicalFilePath() == root
        && local::validateDirectory(root, nullptr);
}

struct PresenceResult {
    QString error;
    QDateTime publishedAt;
    QList<Record> records;
    QSet<QString> presentRecords;
};

struct AutomaticMessage {
    Peer sender;
    QString topic;
    QVariantMap payload;
};

// All recurring filesystem work consumes values and runs outside the UI thread.
PresenceResult inspectPresence(const QString &root, const Peer &local, quint64 sequence,
                               bool publish, bool scan = true)
{
    PresenceResult result;
    if (!intactRoot(root)) {
        result.error = QStringLiteral("The Society Helper observation directory disappeared or was redirected.");
        return result;
    }
    if (publish) {
        auto object = QJsonObject::fromVariantMap(local.toVariantMap());
        object.remove("lastSeenAt");
        object.insert("startedAt", local.startedAt.toString(Qt::ISODateWithMs));
        object.insert("protocol", 1);
        object.insert("heartbeat", QString::number(sequence));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        const auto path = iiFileProvider::Directory(root).filePath(local.instanceId + ".json");
        if (QFileInfo(path).isSymLink()) {
            result.error = QStringLiteral("Cannot publish Society Helper presence through a redirected path.");
            return result;
        }
        try {
            iiFileProvider::native::StagedFile staged(std::filesystem::path(path.toStdU16String()));
            iiFileProvider::FileDevice file(QString::fromStdU16String(staged.path().u16string()));
            if (!file.open(QIODevice::WriteOnly)
                || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                || file.write(bytes) != bytes.size() || !file.flush()) {
                result.error = QStringLiteral("Cannot publish Society Helper presence: %1").arg(file.errorString());
                return result;
            }
            file.close();
            const auto published = staged.publish();
            if (!published.succeeded) {
                result.error = QStringLiteral("Cannot publish Society Helper presence: %1").arg(QString::fromStdString(published.message));
                return result;
            }
        } catch (const std::exception &error) {
            result.error = QStringLiteral("Cannot publish Society Helper presence: %1").arg(QString::fromUtf8(error.what()));
            return result;
        }
        result.publishedAt = QDateTime::currentDateTimeUtc();
    }
    if (scan) {
        const auto entries = iiFileProvider::Directory(root).entryInfoList({"*.json"},
            iiFileProvider::Directory::Files | iiFileProvider::Directory::NoSymLinks,
            iiFileProvider::Directory::Name);
        for (const auto &entry : entries) {
            const auto instance = entry.completeBaseName();
            if (!QUuid(instance).isNull() && QUuid(instance).toString(QUuid::WithoutBraces) == instance
                && instance != local.instanceId)
                result.presentRecords.insert(instance);
            Record record;
            if (readRecord(entry, &record) && record.peer.instanceId != local.instanceId)
                result.records.append(std::move(record));
        }
    }
    return result;
}
}

QVariantMap Peer::toVariantMap() const
{
    return {{"applicationId", application.id}, {"name", application.name}, {"version", application.version},
            {"instanceId", instanceId}, {"processId", processId}, {"activity", activityName(activity)},
            {"startedAt", startedAt}, {"lastSeenAt", lastSeenAt}};
}

class Helper::Private {
public:
    explicit Private(Helper *owner) : q(owner), timer(new QTimer(owner)), watcher(new QFileSystemWatcher(owner)),
        fileSystem(new FileSystem(owner))
    {
        clock.start();
        QObject::connect(timer, &QTimer::timeout, owner, [this] {
            requestCycle(true);
        });
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, owner, [this] { requestCycle(false); });
    }

    struct Observation {
        Record record;
        qint64 lastAdvance = 0;
        bool visible = false;
    };

    Helper *q;
    QTimer *timer;
    QFileSystemWatcher *watcher;
    FileSystem *fileSystem;
    QPointer<iisacc::accounts::AccountManager> accountManager;
    QPointer<iisacc::accounts::Account> account;
    QList<QMetaObject::Connection> accountConnections;
    quint64 accountRevision = 0;
    QElapsedTimer clock;
    ObservationOptions options;
    QString root;
    QString error;
    Peer local;
    quint64 sequence = 0;
    bool running = false;
    qint64 lastScan = 0;
    QMap<QString, Observation> observations;
    DeliveryStore delivery;
    std::unique_ptr<detail::PresenceWorker> presenceWorker, automaticWorker;
    quint64 presenceEpoch = 0;
    bool cycleInFlight = false;
    bool scanPending = false;
    bool publishPending = false;
    bool automaticInFlight = false;
    bool stopping = false;
    QList<AutomaticMessage> automaticMessages;

    bool fail(const QString &message, QString *output = nullptr)
    {
        const QString copiedMessage = message;
        const QPointer<Helper> guard(q);
        if (output) *output = copiedMessage;
        if (error != copiedMessage) {
            error = copiedMessage;
            emit q->errorChanged();
        }
        if (guard) emit guard->errorOccurred(copiedMessage);
        qCWarning(presenceLog).noquote() << copiedMessage;
        return false;
    }

    bool rootIntact() const
    {
        return intactRoot(root);
    }

    QString recordPath() const { return iiFileProvider::Directory(root).filePath(local.instanceId + ".json"); }

    bool publish()
    {
        const auto result = inspectPresence(root, local, ++sequence, true, false);
        if (result.error.isEmpty()) { local.lastSeenAt = result.publishedAt; return true; }
        q->stop();
        return fail(result.error);
    }

    void requestCycle(bool publish)
    {
        if (!running) return;
        if (cycleInFlight) {
            scanPending = true;
            publishPending |= publish;
            return;
        }
        cycleInFlight = true;
        const auto epoch = presenceEpoch;
        const auto directory = root;
        const auto peer = local;
        const auto heartbeat = publish ? ++sequence : sequence;
        if (!presenceWorker) presenceWorker = std::make_unique<detail::PresenceWorker>();
        presenceWorker->post([this, epoch, directory, peer, heartbeat, publish] {
            PresenceResult result;
            try {
                result = inspectPresence(directory, peer, heartbeat, publish);
            }
            catch (const std::exception &error) { result.error = QString::fromUtf8(error.what()); }
            // stop() drains the worker before the receiver can be destroyed.
            QMetaObject::invokeMethod(q, [this, epoch, result = std::move(result)] {
                const QPointer<Helper> guard(q);
                if (!running || presenceEpoch != epoch) return;
                cycleInFlight = false;
                if (!result.error.isEmpty()) {
                    q->stop();
                    if (guard) fail(result.error);
                    return;
                }
                if (result.publishedAt.isValid()) local.lastSeenAt = result.publishedAt;
                scan(result.records, result.presentRecords);
                if (!guard || !running || presenceEpoch != epoch) return;
                if (std::exchange(scanPending, false))
                    requestCycle(std::exchange(publishPending, false));
            }, Qt::QueuedConnection);
        });
    }

    void queueAutomatic(const QString &topic, const QVariantMap &payload)
    {
        if (!running) return;
        automaticMessages.append({local, topic, payload});
        scheduleAutomatic();
    }

    void scheduleAutomatic()
    {
        if (!running || automaticInFlight || automaticMessages.isEmpty()) return;
        automaticInFlight = true;
        if (!automaticWorker) automaticWorker = std::make_unique<detail::PresenceWorker>();
        const auto epoch = presenceEpoch;
        const auto directory = root;
        auto messages = std::exchange(automaticMessages, {});
        // History commits cannot hold up the independently scheduled heartbeat.
        automaticWorker->post([this, epoch, directory, messages = std::move(messages)] {
            QStringList queued;
            QString error;
            try {
                DeliveryStore outbox;
                if (outbox.open(directory, &error))
                    for (const auto &message : messages) {
                        const auto id = outbox.enqueue(message.sender, message.topic, message.payload);
                        if (id.isEmpty()) error = outbox.errorString();
                        else queued.append(id);
                    }
            } catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
            QMetaObject::invokeMethod(q, [this, epoch, queued, error] {
                const QPointer<Helper> guard(q);
                if (presenceEpoch == epoch) automaticInFlight = false;
                for (const auto &id : queued) {
                    if (!guard) return;
                    emit q->dataQueued(id);
                }
                if (!guard || !running || presenceEpoch != epoch) return;
                if (!error.isEmpty()) fail(error);
                if (!guard || !running || presenceEpoch != epoch) return;
                scheduleAutomatic();
            }, Qt::QueuedConnection);
        });
    }

    void flushAutomatic()
    {
        // Explicit stop() drains the worker first, preserving normal-shutdown
        // durability without making focus and peer changes wait for SQLite.
        const auto messages = std::exchange(automaticMessages, {});
        const QPointer<Helper> guard(q);
        for (const auto &message : messages) {
            if (!guard) return;
            const auto id = delivery.enqueue(message.sender, message.topic, message.payload);
            if (id.isEmpty()) fail(delivery.errorString());
            else emit q->dataQueued(id);
        }
    }

    void scan(const QList<Record> &records, const QSet<QString> &presentRecords)
    {
        if (!running) return;
        if (!watcher->directories().contains(root)) watcher->addPath(root);
        const auto now = clock.elapsed();
        // Update the collection before emitting signals so subscribers may safely query it.
        QList<Peer> appeared, updated;
        QList<QPair<Peer, DepartureReason>> departed;
        if (now - lastScan >= options.peerTimeoutMs) {
            // After our own event loop resumes, old files cannot prove current presence.
            for (const auto &entry : observations)
                if (entry.visible) departed.append({entry.record.peer, DepartureReason::TimedOut});
            observations.clear();
        }
        lastScan = now;
        // A file which exists but cannot be read during replacement does not
        // prove withdrawal. Only a valid advancing record refreshes its timeout.
        QSet<QString> present = presentRecords;
        for (auto record : records) {
            const auto &id = record.peer.instanceId;
            present.insert(id);
            auto it = observations.find(id);
            if (it == observations.end()) {
                // Seeing a file once is not evidence that its owner is alive.
                observations.insert(id, {record, now, false});
                continue;
            }
            if (record.sequence > it->record.sequence) {
                record.peer.lastSeenAt = QDateTime::currentDateTimeUtc();
                if (!it->visible) appeared.append(record.peer);
                else if (!sameMetadata(record.peer, it->record.peer)) updated.append(record.peer);
                *it = {record, now, true};
            }
        }
        for (auto it = observations.begin(); it != observations.end();) {
            if (!present.contains(it.key())) {
                if (it->visible) departed.append({it->record.peer, DepartureReason::Withdrawn});
                it = observations.erase(it);
            } else {
                if (it->visible && now - it->lastAdvance >= options.peerTimeoutMs) {
                    departed.append({it->record.peer, DepartureReason::TimedOut});
                    it->visible = false;
                }
                ++it;
            }
        }
        const auto instance = local.instanceId;
        const QPointer<Helper> guard(q);
        // A slot may stop/restart this helper. Do not deliver the old scan's remaining events.
        for (const auto &peer : appeared) {
            if (!guard || !running || local.instanceId != instance) return;
            qCInfo(presenceLog).noquote() << local.application.id << "observed" << peer.application.id << peer.instanceId;
            emit q->peerAppeared(peer);
        }
        for (const auto &peer : updated) {
            if (!guard || !running || local.instanceId != instance) return;
            emit q->peerUpdated(peer);
        }
        for (const auto &[peer, reason] : departed) {
            if (!guard || !running || local.instanceId != instance) return;
            qCInfo(presenceLog).noquote() << local.application.id << "lost" << peer.application.id << peer.instanceId
                << (reason == DepartureReason::Withdrawn ? "withdrawn" : "timed-out");
            emit q->peerDisappeared(peer, reason);
        }
        if (guard && running && local.instanceId == instance && (!appeared.isEmpty() || !updated.isEmpty() || !departed.isEmpty()))
            emit q->peersChanged();
    }
};

Helper::Helper(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this))
{
    if (qApp) connect(qApp, &QCoreApplication::aboutToQuit, this, &Helper::stop);
    connect(this, &Helper::peerAppeared, this, [this](const Peer &peer) { d->queueAutomatic("helper.peerAppeared", peer.toVariantMap()); });
    connect(this, &Helper::peerUpdated, this, [this](const Peer &peer) { d->queueAutomatic("helper.peerUpdated", peer.toVariantMap()); });
    connect(this, &Helper::peerDisappeared, this, [this](const Peer &peer, DepartureReason reason) {
        auto payload = peer.toVariantMap();
        payload.insert("reason", reason == DepartureReason::Withdrawn ? "withdrawn" : "timed-out");
        d->queueAutomatic("helper.peerDisappeared", payload);
    });
}

Helper::~Helper() { stop(); }

QString Helper::defaultDirectory(QString *error)
{
    if (error) error->clear();
    const auto override = qEnvironmentVariable("SOCIETY_HELPER_DIRECTORY");
    const auto storage = iiSocietyContainer::SharedStorage::open({}, error, true);
    if (!storage) return {};
    if (override.isEmpty()) return storage->runtimePath(QStringLiteral("helpers/v1"), error);
    const auto runtime = storage->runtimePath({}, error);
    const auto relative = iiFileProvider::Directory(runtime).relativeFilePath(iiFileProvider::Directory::cleanPath(override));
    if (runtime.isEmpty() || !iiFileProvider::Directory::isAbsolutePath(override)
        || storage->runtimePath(relative == "." ? QString() : relative, error) != iiFileProvider::Directory::cleanPath(override)) {
        if (error) *error = QStringLiteral("The Helper override must be inside the selected Society container's runtime directory.");
        return {};
    }
    return iiFileProvider::Directory::cleanPath(override);
}

bool Helper::start(const ApplicationInfo &application, const ObservationOptions &options, QString *error)
{
    if (error) error->clear();
    if (!qApp || QThread::currentThread() != thread() || thread() != qApp->thread())
        return d->fail(QStringLiteral("Start Society Helper on the application's event-loop thread."), error);
    if (d->running) return d->fail(QStringLiteral("Society Helper is already observing."), error);
    if (!validApplication(application))
        return d->fail(QStringLiteral("Provide a reverse-DNS application ID, a name and bounded version metadata."), error);
    if (options.heartbeatIntervalMs < 20 || options.heartbeatIntervalMs > 60000
        || options.peerTimeoutMs < qint64(options.heartbeatIntervalMs) * 3 || options.peerTimeoutMs > 300000)
        return d->fail(QStringLiteral("Heartbeat must be 20–60000 ms; timeout must be at least three heartbeats and at most 300000 ms."), error);
    QString rootError;
    QString root = options.directory.isEmpty() ? defaultDirectory(&rootError) : options.directory;
    if (root.isEmpty() || !iiFileProvider::Directory::isAbsolutePath(root) || iiFileProvider::Directory(root).isRoot())
        return d->fail(rootError.isEmpty() ? QStringLiteral("Provide an absolute shared observation directory.") : rootError, error);
    root = iiFileProvider::Directory::cleanPath(root);
    if (!local::validateDirectory(root, &rootError)) return d->fail(rootError, error);
    const bool existed = QFileInfo::exists(root);
    if (QFileInfo(root).isSymLink() || !iiFileProvider::Directory().mkpath(root) || !QFileInfo(root).isDir())
        return d->fail(QStringLiteral("Cannot create the shared observation directory."), error);
    if (!existed && !iiFileProvider::FileDevice::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return d->fail(QStringLiteral("Cannot restrict the observation directory to its owner."), error);
    d->root = QFileInfo(root).canonicalFilePath();
    if (!d->delivery.open(d->root, &rootError)) return d->fail(rootError, error);
    d->options = options;
    d->local = {application, QUuid::createUuid().toString(QUuid::WithoutBraces), QCoreApplication::applicationPid(),
                Activity::Unknown, QDateTime::currentDateTimeUtc(), {}};
    d->sequence = 0;
    ++d->presenceEpoch;
    d->lastScan = d->clock.elapsed();
    d->observations.clear();
    if (!d->publish()) {
        if (error) *error = d->error;
        return false;
    }
    if (!d->error.isEmpty()) {
        d->error.clear();
        emit errorChanged();
    }
    d->running = true;
    sendData("helper.started", d->local.toVariantMap());
    qCInfo(presenceLog).noquote() << application.id << "observing as" << d->local.instanceId;
    d->watcher->addPath(d->root); // Timer polling remains available if the OS cannot watch it.
    d->timer->start(options.heartbeatIntervalMs);
    d->requestCycle(false);
    emit runningChanged();
    return true;
}

void Helper::stop()
{
    if (d->stopping) return;
    d->stopping = true;
    const QPointer<Helper> guard(this);
    ++d->presenceEpoch;
    d->timer->stop();
    if (d->presenceWorker) d->presenceWorker->clearAndWait();
    if (d->automaticWorker) d->automaticWorker->drain();
    d->cycleInFlight = d->scanPending = d->publishPending = false;
    d->automaticInFlight = false;
    d->flushAutomatic();
    if (!guard) return;
    const bool wasRunning = d->running;
    const bool hadPeers = !peers().isEmpty();
    if (wasRunning) sendData("helper.stopped", d->local.toVariantMap());
    if (!guard) return;
    d->running = false;
    d->timer->stop();
    if (!d->watcher->directories().isEmpty()) d->watcher->removePaths(d->watcher->directories());
    if (!d->local.instanceId.isEmpty() && d->rootIntact()) iiFileProvider::FileDevice::remove(d->recordPath());
    d->observations.clear();
    d->delivery.close();
    d->stopping = false;
    if (wasRunning) emit runningChanged();
    if (guard && hadPeers) emit peersChanged();
}

void Helper::setActivity(Activity activity)
{
    if (activity != Activity::Unknown && activity != Activity::Foreground && activity != Activity::Background) return;
    if (d->local.activity == activity) return;
    d->local.activity = activity;
    if (d->running) {
        d->requestCycle(true);
        d->queueAutomatic("helper.activity", d->local.toVariantMap());
    }
}

QString Helper::sendData(const QString &topic, const QVariantMap &payload)
{
    if (!d->running) { d->fail(QStringLiteral("Start Society Helper before sending data.")); return {}; }
    const auto id = d->delivery.enqueue(d->local, topic, payload);
    if (id.isEmpty()) d->fail(d->delivery.errorString());
    else emit dataQueued(id);
    return id;
}

QString Helper::sendObject(const QString &topic, QObject *object)
{
    return sendObject(topic, static_cast<const QObject *>(object));
}
QString Helper::sendObject(const QString &topic, const QObject *object)
{
    return sendObjectImpl(topic, object, nullptr);
}
QString Helper::sendObject(const QString &topic, const QVariant &value)
{
    return sendObjectImpl(topic, nullptr, &value);
}
QString Helper::sendObjectImpl(const QString &topic, const QObject *object, const QVariant *value)
{
    if (QThread::currentThread() != thread()) {
        qCWarning(presenceLog) << "Send objects on Society Helper's event-loop thread.";
        return {};
    }
    if (!d->running) { d->fail(QStringLiteral("Start Society Helper before sending objects.")); return {}; }
    const auto instance = d->local.instanceId;
    const QPointer<Helper> guard(this);
    QString error;
    const auto payload = value ? ObjectCodec::encode(*value, &error) : ObjectCodec::encode(object, &error);
    if (!guard) return {};
    if (!d->running || d->local.instanceId != instance) {
        d->fail(QStringLiteral("Society Helper changed sender while capturing the object."));
        return {};
    }
    if (payload.isEmpty()) { d->fail(error); return {}; }
    return sendData(topic, payload);
}

bool Helper::isRunning() const { return d->running; }
Peer Helper::self() const { return d->local; }
QString Helper::directory() const { return d->root; }
QString Helper::errorString() const { return d->error; }
FileSystem *Helper::fileSystem() const { return d->fileSystem; }
iisacc::accounts::AccountManager *Helper::accountManager() const { return d->accountManager.data(); }
iisacc::accounts::Account *Helper::account() const { return d->account.data(); }

bool Helper::setAccountManager(iisacc::accounts::AccountManager *manager)
{
    if (QThread::currentThread() != thread() || (manager && manager->thread() != thread())) {
        qCWarning(presenceLog) << "Reference AccountManager on the same event-loop thread as Society Helper.";
        return false;
    }
    // A destroyed manager's QPointer is already null when its signal runs; its
    // existing connections still distinguish destruction from a detached no-op.
    if (manager == d->accountManager && (manager || d->accountConnections.isEmpty())) return true;
    for (const auto &connection : d->accountConnections) QObject::disconnect(connection);
    d->accountConnections.clear();
    d->accountManager = manager;
    d->account = manager ? manager->account() : nullptr;
    if (manager) {
        d->accountConnections.append(connect(manager, &QObject::destroyed, this, [this] {
            setAccountManager(nullptr);
        }));
        d->accountConnections.append(connect(d->account, &iisacc::accounts::Account::changed,
            this, &Helper::accountChanged));
    }
    const auto revision = ++d->accountRevision;
    const QPointer<Helper> guard(this);
    const auto current = [&] { return guard && guard->d->accountRevision == revision; };
    emit accountManagerChanged();
    if (!current()) return false;
    emit accountChanged();
    return current();
}

QList<Peer> Helper::peers() const
{
    QList<Peer> peers;
    for (const auto &entry : d->observations) if (entry.visible) peers.append(entry.record.peer);
    return peers;
}
QVariantList Helper::observedApplications() const
{
    QVariantList result;
    for (const auto &peer : peers()) result.append(peer.toVariantMap());
    return result;
}
}
