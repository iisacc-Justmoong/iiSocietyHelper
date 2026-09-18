#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include "LocalDirectory.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

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
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto bytes = file.read(maximumRecordBytes + 1);
    if (bytes.size() > maximumRecordBytes) return false;
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
            if (publish()) scan();
        });
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, owner, [this] { scan(); });
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
        const QFileInfo info(root);
        return info.isDir() && !info.isSymLink() && info.canonicalFilePath() == root
            && local::validateDirectory(root, nullptr);
    }

    QString recordPath() const { return QDir(root).filePath(local.instanceId + ".json"); }

    bool publish()
    {
        if (!rootIntact()) {
            q->stop();
            return fail(QStringLiteral("The Society Helper observation directory disappeared or was redirected."));
        }
        auto object = QJsonObject::fromVariantMap(local.toVariantMap());
        object.remove("lastSeenAt");
        object.insert("startedAt", local.startedAt.toString(Qt::ISODateWithMs));
        object.insert("protocol", 1);
        object.insert("heartbeat", QString::number(++sequence));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QSaveFile file(recordPath());
        // Never fall back to overwriting a half-written presence record.
        file.setDirectWriteFallback(false);
        if (!QFileInfo(recordPath()).isSymLink() && file.open(QIODevice::WriteOnly)
            && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            && file.write(bytes) == bytes.size() && file.commit()) {
            local.lastSeenAt = QDateTime::currentDateTimeUtc();
            return true;
        }
        const auto message = QStringLiteral("Cannot publish Society Helper presence: %1").arg(file.errorString());
        q->stop();
        return fail(message);
    }

    void scan()
    {
        if (!running) return;
        if (!rootIntact()) {
            q->stop();
            fail(QStringLiteral("The Society Helper observation directory disappeared or was redirected."));
            return;
        }
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
        QSet<QString> present;
        const auto entries = QDir(root).entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const auto &entry : entries) {
            Record record;
            if (!readRecord(entry, &record) || record.peer.instanceId == local.instanceId) continue;
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
        // A slot may stop/restart this helper. Do not deliver the old scan's remaining events.
        for (const auto &peer : appeared) {
            if (!running || local.instanceId != instance) return;
            qCInfo(presenceLog).noquote() << local.application.id << "observed" << peer.application.id << peer.instanceId;
            emit q->peerAppeared(peer);
        }
        for (const auto &peer : updated) {
            if (!running || local.instanceId != instance) return;
            emit q->peerUpdated(peer);
        }
        for (const auto &[peer, reason] : departed) {
            if (!running || local.instanceId != instance) return;
            qCInfo(presenceLog).noquote() << local.application.id << "lost" << peer.application.id << peer.instanceId
                << (reason == DepartureReason::Withdrawn ? "withdrawn" : "timed-out");
            emit q->peerDisappeared(peer, reason);
        }
        if (running && local.instanceId == instance && (!appeared.isEmpty() || !updated.isEmpty() || !departed.isEmpty()))
            emit q->peersChanged();
    }
};

Helper::Helper(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this))
{
    if (qApp) connect(qApp, &QCoreApplication::aboutToQuit, this, &Helper::stop);
    connect(this, &Helper::peerAppeared, this, [this](const Peer &peer) { sendData("helper.peerAppeared", peer.toVariantMap()); });
    connect(this, &Helper::peerUpdated, this, [this](const Peer &peer) { sendData("helper.peerUpdated", peer.toVariantMap()); });
    connect(this, &Helper::peerDisappeared, this, [this](const Peer &peer, DepartureReason reason) {
        auto payload = peer.toVariantMap();
        payload.insert("reason", reason == DepartureReason::Withdrawn ? "withdrawn" : "timed-out");
        sendData("helper.peerDisappeared", payload);
    });
}

Helper::~Helper() { stop(); }

QString Helper::defaultDirectory(QString *error)
{
    if (error) error->clear();
    const auto override = qEnvironmentVariable("SOCIETY_HELPER_DIRECTORY");
    if (!override.isEmpty()) return override;
#ifdef Q_OS_DARWIN
    QString appleError;
    const auto shared = appleObservationDirectory(&appleError);
    if (!shared.isEmpty()) return shared;
    if (!appleError.isEmpty()) {
        if (error) *error = appleError;
        return {};
    }
#endif
#if defined(Q_OS_ANDROID) || defined(Q_OS_WASM)
    if (error) *error = QStringLiteral("This platform needs a shared observation directory provided by the host integration.");
    return {};
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("iisacc/Society/Helpers/v1"));
#endif
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
    if (root.isEmpty() || !QDir::isAbsolutePath(root) || QDir(root).isRoot())
        return d->fail(rootError.isEmpty() ? QStringLiteral("Provide an absolute shared observation directory.") : rootError, error);
    root = QDir::cleanPath(root);
    if (!local::validateDirectory(root, &rootError)) return d->fail(rootError, error);
    const bool existed = QFileInfo::exists(root);
    if (QFileInfo(root).isSymLink() || !QDir().mkpath(root) || !QFileInfo(root).isDir())
        return d->fail(QStringLiteral("Cannot create the shared observation directory."), error);
    if (!existed && !QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return d->fail(QStringLiteral("Cannot restrict the observation directory to its owner."), error);
    d->root = QFileInfo(root).canonicalFilePath();
    if (!d->delivery.open(d->root, &rootError)) return d->fail(rootError, error);
    d->options = options;
    d->local = {application, QUuid::createUuid().toString(QUuid::WithoutBraces), QCoreApplication::applicationPid(),
                Activity::Unknown, QDateTime::currentDateTimeUtc(), {}};
    d->sequence = 0;
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
    d->scan();
    emit runningChanged();
    return true;
}

void Helper::stop()
{
    const bool wasRunning = d->running;
    const bool hadPeers = !peers().isEmpty();
    if (wasRunning) sendData("helper.stopped", d->local.toVariantMap());
    d->running = false;
    d->timer->stop();
    if (!d->watcher->directories().isEmpty()) d->watcher->removePaths(d->watcher->directories());
    if (!d->local.instanceId.isEmpty() && d->rootIntact()) QFile::remove(d->recordPath());
    d->observations.clear();
    d->delivery.close();
    if (wasRunning) emit runningChanged();
    if (hadPeers) emit peersChanged();
}

void Helper::setActivity(Activity activity)
{
    if (activity != Activity::Unknown && activity != Activity::Foreground && activity != Activity::Background) return;
    if (d->local.activity == activity) return;
    d->local.activity = activity;
    if (d->running && d->publish()) sendData("helper.activity", d->local.toVariantMap());
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
