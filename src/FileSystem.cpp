#include <iiFileProviderDevice.h>
#include "FilePresentation_p.h"
#include "PresenceWorker_p.h"
#include "ManagedFileOperation_p.h"
#include <QSet>
#include <QPointer>
#include <utility>
#include "iiSocietyHelper.h"
#include <SharedStorage.h>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QDateTime>
#include <QLocale>
#include <QTimer>
#include <StorageMap.h>
#include <FileOperations.h>
#include <iiFileProvider.h>
#include <iiFileProviderPresentation.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <iiSocietyHelper/Monitoring/Monitor.h>
#ifdef Q_OS_ANDROID
#include <AndroidStorage.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#endif

namespace iiSocietyHelper {
using iiSocietyContainer::SharedStorage;
using iiSocietyContainer::StoreSection;

namespace {
bool providerControl(const QString &relative) {
    for (const auto &part : relative.split('/'))
        if (iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(part.toStdU16String()))) return true;
    return false;
}
QVariantMap fileAlias(const SharedStorage &storage, StoreSection section,
                      const QString &sectionKey, const QString &relativePath,
                      const QString &native, const QJsonObject &object = {})
{
    const QFileInfo info(native);
    const bool available = info.isFile() && !info.isSymLink() && !info.isJunction() && info.canonicalFilePath() == native;
    const auto key = sectionKey + '/' + relativePath;
    const bool exists = available || (!object.isEmpty() && object.value("kind").toString() == "file");
    const auto size = available ? info.size() : object.value("size").toVariant().toLongLong();
    const auto suffix = info.suffix().toLower();
    const bool image = QStringList{"png","jpg","jpeg","webp","bmp","gif","tif","tiff","avif","heic","heif"}.contains(suffix);
    const auto source = available ? QUrl::fromLocalFile(native) : QUrl{};
    auto preview = image ? source : QUrl{};
    if (!preview.isEmpty()) preview.setQuery(QString("v=%1-%2").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()));
    QVariantMap value{{"id", storage.drive().identifier() + '/' + key}, {"containerId", storage.drive().identifier()}, {"key", key},
        {"informationKey", key}, {"informationAvailable", exists},
        {"section", sectionKey}, {"relativePath", relativePath}, {"name", info.fileName()},
        {"exists", exists}, {"available", available}, {"path", available ? native : QString{}},
        {"source", source}, {"previewSource", preview}, {"thumbnailSource", preview},
        {"isImage", image}, {"size", size},
        {"modified", info.lastModified().toUTC()}, {"folderPath", info.absolutePath()},
        {"history", sectionKey == "generation-history"},
        {"iconName", image ? "fileTypesimage" : suffix == "iisc" ? "application" : "fileTypestext"},
        {"description", iiSocietyContainer::storeSectionName(section) + " · " + (image ? "Image" : suffix == "iisc" ? "Canvas" : "Document")},
        {"metadata1", suffix.toUpper() + " · " + QLocale().formattedDataSize(size)},
        {"metadata2", "Society / " + iiSocietyContainer::storeSectionName(section)}};
    if (available) value["version"] = QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size());
    else if (!object.isEmpty()) value["version"] = object.value("version").toString();
    if (!available && !object.isEmpty()) {
        const auto remotePreview = iiSocietyContainer::StorageMap(storage.drive()).previewPath(object);
        value["previewSource"] = remotePreview.isEmpty() ? QUrl{} : QUrl::fromLocalFile(remotePreview);
        value["thumbnailSource"] = value["previewSource"];
    }
    return value;
}

struct MirrorSnapshot {
    QString id, root, error;
    QVariantList aliases;
    bool available = false;
};

// This is value-only I/O. No QObject, timer or application binding is accessed.
MirrorSnapshot scanMirror(const std::optional<SharedStorage> &storage)
{
    MirrorSnapshot result;
    if (!storage || !storage->drive().isValid()) {
        result.error = QStringLiteral("The selected Society drive is unavailable or changed.");
        return result;
    }
    result.id = storage->drive().identifier();
    result.root = storage->drive().rootPath();
    QSet<QString> residentKeys;
    for (const auto section : {StoreSection::Files, StoreSection::Published, StoreSection::GenerationHistory}) {
        const auto key = iiSocietyContainer::storeSectionKey(section);
        const auto directory = storage->filePath(section);
        if (directory.isEmpty()) continue;
        QDirIterator files(directory, QDir::Files | QDir::NoSymLinks,
            key == "generation-history" ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories);
        while (files.hasNext() && result.aliases.size() < 25000) {
            const auto native = files.next();
            if (files.fileName().startsWith('.') || providerControl(files.fileName())) continue;
            const auto relative = QDir(directory).relativeFilePath(native);
            // The batch pins and checks the drive once at each boundary. Each
            // resident leaf still rejects traversal and redirected ancestors.
            const auto logical = key + '/' + relative;
            if (iiSocietyContainer::StorageMap::physicalPath(logical).isEmpty()) continue;
            const auto row = fileAlias(*storage, section, key, relative, native);
            if (row.value("available").toBool()) {
                residentKeys.insert(logical);
                result.aliases.append(row);
            }
        }
    }
    const iiSocietyContainer::StorageMap map(storage->drive());
    if (!map.isLocalAuthority()) for (const auto &entry : map.objects()) {
        if (result.aliases.size() >= 25000) break;
        const auto object = entry.toObject();
        const auto key = object.value("path").toString();
        const auto sectionKey = key.section('/', 0, 0);
        if (object.value("kind") != "file" || residentKeys.contains(key) || providerControl(key)
            || iiSocietyContainer::StorageMap::physicalPath(key).isEmpty()) continue;
        const auto section = sectionKey == "files" ? StoreSection::Files
            : sectionKey == "published" ? StoreSection::Published : StoreSection::GenerationHistory;
        if (sectionKey != "files" && sectionKey != "published" && sectionKey != "generation-history") continue;
        const auto relative = key.mid(sectionKey.size() + 1);
        const auto native = QDir(storage->drive().sectionPath(section)).filePath(relative);
        result.aliases.append(fileAlias(*storage, section, sectionKey, relative, native, object));
        residentKeys.insert(key);
    }
    // A package replaced during a scan must not publish another drive's rows.
    result.available = storage->drive().isValid();
    if (!result.available) {
        result.id.clear(); result.root.clear(); result.aliases.clear();
        result.error = QStringLiteral("The selected Society drive changed during synchronization.");
    }
    return result;
}
} // namespace

class PreparedStorage::Data {
public:
    std::optional<SharedStorage> storage;
    QString selection,error,id,root;
    QVariantList aliases;
    bool available=false;
#ifdef Q_OS_ANDROID
    bool remote=false;
    QString remoteId,remoteRoot;
#endif
};

class FileSystem::Private {
public:
    explicit Private(FileSystem *owner) : q(owner), container(owner) {}
    FileSystem *q;
    iiSocietyContainer::Container container;
    QTimer synchronization;
    std::unique_ptr<detail::PresenceWorker> mirrorWorker;
    std::unique_ptr<detail::PresenceWorker> intakeWorker;
    std::stop_source intakeCancellation;
    quint64 mirrorEpoch = 0;
    bool mirrorInFlight = false, mirrorPending = false;
    QString selection;
    QString error;
    std::optional<SharedStorage> storage;
    iiSocietyHelper::Monitoring::Subscription monitor;
    QVariantMap containerState;
    quint64 monitorEpoch = 0;
    std::filesystem::path monitorRoot;
    std::string monitorId;

    void stopMonitor()
    {
        ++monitorEpoch;
        monitor.stop();
        monitorRoot.clear(); monitorId.clear();
        if (!synchronization.isActive()) synchronization.start();
        if (!containerState.isEmpty()) {
            containerState.clear();
            emit q->containerStateChanged();
        }
    }

    void startMonitor()
    {
        if (!storage) return;
        const auto root = std::filesystem::path(storage->drive().rootPath().toStdU16String());
        const auto id = storage->drive().identifier().toStdString();
        if (root == monitorRoot && id == monitorId) return;
        stopMonitor();
        monitorRoot = root; monitorId = id;
        const auto epoch = monitorEpoch;
        auto *owner = q;
        // Subscription owns a native worker. Only the queued adapter touches
        // QObject state, and selection epochs discard obsolete queued events.
        (void)monitor.start(root, id, [owner, epoch](const iiSocietyHelper::Monitoring::State &state) {
            const auto json = iiSocietyHelper::Monitoring::toJson(state);
            const auto value = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().toVariantMap();
            QMetaObject::invokeMethod(owner, [owner, epoch, value] {
                auto &self = *owner->d;
                if (self.monitorEpoch != epoch) return;
                const auto previous = self.containerState;
                self.containerState = value;
                const bool localAuthority = self.storage && iiSocietyContainer::StorageMap(self.storage->drive()).isLocalAuthority();
                if (value.value("daemonRunning").toBool() && value.value("available").toBool()
                    && value.value("complete").toBool() && localAuthority) self.synchronization.stop();
                else self.synchronization.start();
                // An offline/stale heartbeat updates status; metadata changes
                // refresh the existing read-only mirror and app bindings.
                if (value.value("available").toBool() &&
                    (previous.value("revision") != value.value("revision") || previous.value("instanceId") != value.value("instanceId"))) {
                    owner->requestSynchronization();
                    emit owner->storageChanged();
                }
                emit owner->containerStateChanged();
            }, Qt::QueuedConnection);
        });
    }
#ifdef Q_OS_ANDROID
    bool remote = false;
    QString remoteId;
    QString remoteRoot;
    QJsonObject request(const QString &action, const QString &section = {}, const QString &path = {})
    {
        const auto value = QJsonDocument::fromJson(iiSocietyContainer::androidSharedRequest(action, section, path, remoteId)).object();
        setError(value.value("error").toString());
        return value;
    }
#endif

    void setError(const QString &message)
    {
        if (error == message) return;
        error = message;
        emit q->errorChanged();
    }

    bool ready()
    {
#ifdef Q_OS_ANDROID
        if (remote) {
            if (remoteId.isEmpty()) return q->open(selection);
            return !request(QStringLiteral("open")).contains("error");
        }
#endif
        // An app may start before Society has selected/created its first drive.
        if (!storage) return q->open(selection);
        if (!storage->drive().isValid()) {
            setError(QStringLiteral("The selected Society drive is unavailable or changed. Refresh its selection before accessing files."));
            emit q->storageChanged();
            return false;
        }
        return true;
    }

    std::optional<StoreSection> section(const QString &key)
    {
        for (const auto section : iiSocietyContainer::allStoreSections())
            if (iiSocietyContainer::storeSectionKey(section) == key) return section;
        setError(QStringLiteral("Unknown Society area: %1").arg(key));
        return {};
    }
};

FileSystem::FileSystem(QObject *parent) : FileSystem(Initialization::Discover,parent) {}
FileSystem::FileSystem(Initialization initialization,QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this))
{
    detail::installCanvasPresentation();
    connect(&d->container, &iiSocietyContainer::Container::refreshRequested, this, &FileSystem::requestSynchronization);
    d->synchronization.setInterval(2000);
    connect(&d->synchronization, &QTimer::timeout, this, &FileSystem::requestSynchronization);
    if(initialization==Initialization::Discover) {
        d->synchronization.start();open();
    }
}
FileSystem::~FileSystem() {
    d->intakeCancellation.request_stop();
    ++d->monitorEpoch; ++d->mirrorEpoch;
    d->monitor.stop();
    // The receiver remains alive until its native I/O worker has stopped.
    d->mirrorWorker.reset();
    d->intakeWorker.reset();
}

bool FileSystem::open(const QString &containerPath)
{
    d->intakeCancellation.request_stop(); d->intakeCancellation = std::stop_source{};
    ++d->mirrorEpoch;
    d->selection = containerPath;
#ifdef Q_OS_ANDROID
    d->remote = !iiSocietyContainer::androidIsStorageOwner();
    if (d->remote) {
        d->stopMonitor();
        d->storage.reset(); d->remoteId.clear(); d->remoteRoot.clear();
        const auto value = d->request(QStringLiteral("open"));
        if (!value.contains("error")) {
            const auto root = value.value("rootPath").toString();
            if (containerPath.isEmpty() || root == containerPath) {
                d->remoteRoot = root;
                d->remoteId = value.value("identifier").toString();
            } else d->setError(QStringLiteral("Android clients use Society's managed storage URI."));
        }
        synchronize();
        emit storageChanged();
        return !d->remoteId.isEmpty();
    }
#endif
    QString error;
    d->storage = SharedStorage::open(d->selection, &error);
    const bool available = d->storage.has_value();
    if (!available) d->stopMonitor();
    d->setError(error);
    d->startMonitor();
    synchronize();
    emit storageChanged();
    return available;
}

PreparedStorage FileSystem::preparedStorage() const {
    PreparedStorage result;auto data=std::make_shared<PreparedStorage::Data>();
    data->storage=d->storage;data->selection=d->selection;data->error=d->error;
    data->id=d->container.containerId();data->root=d->container.rootPath();
    data->aliases=d->container.aliases();data->available=d->container.available();
#ifdef Q_OS_ANDROID
    data->remote=d->remote;data->remoteId=d->remoteId;data->remoteRoot=d->remoteRoot;
#endif
    result.d=std::move(data);return result;
}
bool FileSystem::adoptStorage(const PreparedStorage &prepared) {
    if(!prepared.d){setError(QStringLiteral("Capture a storage selection before adopting it."));return false;}
    const auto state=prepared.d;
    if(state->storage&&!state->storage->drive().isValid()) {
        setError(QStringLiteral("The prepared Society drive is unavailable or changed."));return false;
    }
    const QPointer<FileSystem> guard(this);const auto epoch=++d->mirrorEpoch;
    d->mirrorPending=false;d->stopMonitor();if(!guard||d->mirrorEpoch!=epoch)return false;
    d->storage=state->storage;d->selection=state->selection;
#ifdef Q_OS_ANDROID
    d->remote=state->remote;d->remoteId=state->remoteId;d->remoteRoot=state->remoteRoot;
#endif
    d->setError(state->error);if(!guard||d->mirrorEpoch!=epoch)return false;
    d->container.synchronize(state->id,state->root,state->aliases,state->available,state->error);
    if(!guard||d->mirrorEpoch!=epoch)return false;
    d->startMonitor();if(!guard||d->mirrorEpoch!=epoch)return false;
    emit storageChanged();return true;
}
bool FileSystem::refresh() { return open(d->selection); }
bool FileSystem::isAvailable() const {
#ifdef Q_OS_ANDROID
    if (d->remote) return !d->remoteId.isEmpty() && !d->request(QStringLiteral("open")).contains("error");
#endif
    return d->storage && d->storage->drive().isValid();
}
QString FileSystem::rootPath() const {
#ifdef Q_OS_ANDROID
    if (d->remote) return isAvailable() ? d->remoteRoot : QString();
#endif
    return isAvailable() ? d->storage->drive().rootPath() : QString();
}
QString FileSystem::containerId() const {
#ifdef Q_OS_ANDROID
    if (d->remote) return isAvailable() ? d->remoteId : QString();
#endif
    return isAvailable() ? d->storage->drive().identifier() : QString();
}
QString FileSystem::errorString() const { return d->error; }
void FileSystem::setError(const QString &message) { d->setError(message); }
iiSocietyContainer::Container *FileSystem::container() const { return &d->container; }
QVariantMap FileSystem::containerState() const { return d->containerState; }

bool FileSystem::openForSource(const QString &source) {
    const auto drive = iiSocietyContainer::FileOperations::containingDrive(source);
    return drive ? open(drive->rootPath()) : refresh();
}
bool FileSystem::selectDefaultContainer(const QString &source) {
    QString error;
    if (!SharedStorage::setDefaultContainer(source, &error)) { setError(error); return false; }
    return open(source);
}

QVariantMap FileSystem::alias(const QString &sectionKey, const QString &relativePath) {
    if (providerControl(relativePath)) { setError(QStringLiteral("Provider control documents are private.")); return {}; }
    const auto section = d->section(sectionKey);
#ifdef Q_OS_ANDROID
    if (d->remote) return section ? d->container.alias(sectionKey + '/' + relativePath) : QVariantMap{};
#endif
    auto native = section ? path(sectionKey, relativePath) : QString{};
    QJsonObject declared;
    // Resident payloads already prove existence. Read the replica catalog only
    // for missing payloads so a snapshot does not reopen it for every file.
    const QFileInfo resident(native);
    const bool residentAvailable = !native.isEmpty() && resident.isFile() && !resident.isSymLink()
        && !resident.isJunction() && resident.canonicalFilePath() == native;
    if (section && d->storage && !residentAvailable) {
        const iiSocietyContainer::StorageMap map(d->storage->drive());
        if (!map.isLocalAuthority()) declared = map.object(sectionKey + '/' + relativePath);
        if (native.isEmpty() && declared.value("kind").toString() == "file") {
            native = QDir(d->storage->drive().sectionPath(*section)).filePath(relativePath);
            setError({}); // Namespace exists even when parent directories are not downloaded.
        }
    }
    if (!section || native.isEmpty()) return {};
    return fileAlias(*d->storage, *section, sectionKey, relativePath, native, declared);
}
QVariantMap FileSystem::aliasForPath(const QString &source) {
#ifdef Q_OS_ANDROID
    if (d->remote && QUrl(source).scheme() == "content") {
        for (const auto &entry : d->container.aliases()) {
            const auto row = entry.toMap(); if (row.value("source").toUrl() == QUrl(source)) return row;
        }
        return {};
    }
#endif
    const auto absolute = QDir::cleanPath(QFileInfo(source).absoluteFilePath());
    if (d->storage && absolute.startsWith(d->storage->drive().rootPath() + '/') && !d->ready()) return {};
    for (const auto &entry : sections()) {
        const auto section = entry.toMap(); const auto directory = section["path"].toString();
        if (absolute.startsWith(directory + '/')) {
            auto value = alias(section["key"].toString(), QDir(directory).relativeFilePath(absolute));
            if (value.value("available").toBool() && QFileInfo(absolute).suffix().compare("iisc", Qt::CaseInsensitive) == 0)
                try { value["version"] = QString::fromLatin1(QCryptographicHash::hash(iiFileProvider::File::read(absolute, 512LL * 1024 * 1024), QCryptographicHash::Sha256).toHex()); }
                catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
            return value;
        }
    }
    // Explicit external imports/exports have no container key. They are never
    // inserted into its mirror; automatic native documents still belong to Files.
    const QFileInfo info(absolute);
    if (!info.isFile() || info.isSymLink() || info.isJunction() || !detail::samePathSpelling(info.canonicalFilePath(), absolute)) return {};
    QVariantMap value{{"path", absolute}, {"source", QUrl::fromLocalFile(absolute)}, {"exists", true}, {"available", true},
        {"name", info.fileName()}, {"size", info.size()}, {"version", QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size())}};
    if (info.suffix().compare("iisc", Qt::CaseInsensitive) == 0)
        try { value["version"] = QString::fromLatin1(QCryptographicHash::hash(iiFileProvider::File::read(absolute, 512LL * 1024 * 1024), QCryptographicHash::Sha256).toHex()); }
        catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
    return value;
}
bool FileSystem::synchronize() {
#ifdef Q_OS_ANDROID
    if (d->remote) {
        if (!isAvailable()) {
            d->container.synchronize({}, {}, {}, false, errorString()); return false;
        }
        QVariantList values;
        for (const auto &section : {QString("files"), QString("published"), QString("generation-history")}) {
            QStringList pending{QString{}};
            while (!pending.isEmpty() && values.size() < 25000) {
                const auto directory = pending.takeLast();
                const auto response = d->request("list", section, directory);
                if (response.contains("error")) {
                    d->container.synchronize(d->remoteId, d->remoteRoot, {}, false, errorString()); return false;
                }
                for (const auto &entry : response.value("entries").toArray()) {
                    const auto object = entry.toObject(); const auto name = object.value("name").toString();
                    if (name.isEmpty() || name.startsWith('.') || providerControl(name) || name.contains('/') || name.contains('\\') || name.contains(QChar::Null)) continue;
                    const auto relative = directory.isEmpty() ? name : directory + '/' + name;
                    if (object.value("isDirectory").toBool()) {
                        if (section != "generation-history" && relative.count('/') < 64) pending.append(relative);
                        continue;
                    }
                    const auto suffix = QFileInfo(name).suffix().toLower();
                    const bool image = QStringList{"png","jpg","jpeg","webp","bmp","gif","tif","tiff","avif","heic","heif"}.contains(suffix);
                    const auto key = section + '/' + relative; const auto source = QUrl(object.value("path").toString());
                    if (source.scheme() != "content") continue;
                    const auto size = object.value("size").toVariant().toLongLong();
                    values.append(QVariantMap{{"id", d->remoteId + '/' + key}, {"containerId", d->remoteId}, {"key", key},
                        {"section", section}, {"relativePath", relative}, {"name", name}, {"exists", true}, {"available", true},
                        {"path", source.toString()}, {"source", source}, {"previewSource", image ? source : QUrl{}},
                        {"thumbnailSource", image ? source : QUrl{}}, {"isImage", image}, {"size", size},
                        {"history", section == "generation-history"}, {"modified", QDateTime{}},
                        {"iconName", image ? "fileTypesimage" : suffix == "iisc" ? "application" : "fileTypestext"},
                        {"metadata1", suffix.toUpper() + " · " + QLocale().formattedDataSize(size)},
                        {"metadata2", "Society / " + section}});
                }
            }
        }
        d->container.synchronize(d->remoteId, d->remoteRoot, values, true); return true;
    }
#endif
    const auto epoch = ++d->mirrorEpoch; // Invalidate replies queued before an explicit save/refresh.
    const auto snapshot = scanMirror(d->storage);
    const QPointer<FileSystem> guard(this);
    d->setError(snapshot.error);
    if (!guard || epoch != d->mirrorEpoch) return false;
    d->container.synchronize(snapshot.id, snapshot.root, snapshot.aliases, snapshot.available, snapshot.error);
    return snapshot.available;
}

void FileSystem::requestSynchronization()
{
#ifdef Q_OS_ANDROID
    // The existing provider adapter owns its Android request context.
    if (d->remote) { const auto ok = synchronize(); emit synchronizationFinished(ok); return; }
#endif
    if (d->mirrorInFlight) { d->mirrorPending = true; return; }
    d->mirrorInFlight = true;
    const auto epoch = d->mirrorEpoch;
    const auto storage = d->storage;
    const auto intakeStop = d->intakeCancellation.get_token();
    if (!d->mirrorWorker) d->mirrorWorker = std::make_unique<detail::PresenceWorker>();
    d->mirrorWorker->post([this, epoch, storage, intakeStop] {
        MirrorSnapshot snapshot;
        try { snapshot = scanMirror(storage); }
        catch (const std::exception &error) { snapshot.error = QString::fromUtf8(error.what()); }
        // Mirror discovery and RAW capture have independent coalesced owners.
        // Switching a selection must not wait for the previous archive.
        QMetaObject::invokeMethod(this, [this, epoch, storage, intakeStop, snapshot = std::move(snapshot)] {
            const QPointer<FileSystem> guard(this);
            d->mirrorInFlight = false;
            if (epoch == d->mirrorEpoch) {
                d->setError(snapshot.error);
                if (!guard) return;
                if (epoch == d->mirrorEpoch)
                    d->container.synchronize(snapshot.id, snapshot.root, snapshot.aliases, snapshot.available, snapshot.error);
                if (!guard) return;
                if (storage && snapshot.available && !intakeStop.stop_requested()) {
                    if (!d->intakeWorker) d->intakeWorker = std::make_unique<detail::PresenceWorker>();
                    d->intakeWorker->post([this, epoch, storage, intakeStop] {
                        if (intakeStop.stop_requested()) return;
                        QString error; storage->reconcileFiles(&error, intakeStop);
                        if (!error.isEmpty() && !intakeStop.stop_requested())
                            QMetaObject::invokeMethod(this, [this, epoch, error] {
                                if (epoch == d->mirrorEpoch) d->setError(error);
                            }, Qt::QueuedConnection);
                    });
                }
            }
            emit synchronizationFinished(epoch == d->mirrorEpoch && snapshot.available);
            if (guard && std::exchange(d->mirrorPending, false)) requestSynchronization();
        }, Qt::QueuedConnection);
    });
}

QList<iiSocietyContainer::StoredModel> FileSystem::models()
{
    if (!d->ready()) return {};
    if (!d->storage) {
        d->setError(QStringLiteral("Native Society model inventory is unavailable for this storage provider."));
        return {};
    }
    QString error;
    auto result = d->storage->models(&error);
    d->setError(error);
    return result;
}
QString FileSystem::resolveModel(const QJsonObject &reference, QString *error) {
    QString message;
    const auto result = d->ready() && d->storage ? d->storage->resolveModel(reference, &message) : QString{};
    if (message.isEmpty() && result.isEmpty()) message = errorString();
    setError(message); if (error) *error = message; return result;
}
QString FileSystem::path(StoreSection section, const QString &relativePath, QString *error) {
    const auto result = path(iiSocietyContainer::storeSectionKey(section), relativePath);
    if (error) *error = errorString(); return result;
}
QStringList FileSystem::files(const QString &key) {
    return d->ready() && d->storage ? iiSocietyContainer::StorageMap(d->storage->drive()).files(key) : QStringList{};
}
bool FileSystem::available(const QStringList &keys) {
    return d->ready() && d->storage && iiSocietyContainer::StorageMap(d->storage->drive()).available(keys);
}
QString FileSystem::requestFiles(const QStringList &keys, QString *error) {
    QString message;
    const auto result = d->ready() && d->storage ? iiSocietyContainer::StorageMap(d->storage->drive()).request(keys, &message) : QString{};
    if (message.isEmpty() && result.isEmpty()) message = errorString();
    setError(message); if (error) *error = message; return result;
}
QJsonObject FileSystem::requestState(const QString &id) {
    return d->ready() && d->storage ? iiSocietyContainer::StorageMap(d->storage->drive()).requestState(id) : QJsonObject{};
}
bool FileSystem::cancelFileRequest(const QString &id) {
    return d->ready() && d->storage && iiSocietyContainer::StorageMap(d->storage->drive()).cancel(id);
}

QVariantList FileSystem::sections() const
{
    QVariantList result;
    if (!isAvailable()) return result;
    for (const auto section : iiSocietyContainer::allStoreSections()) {
        QString path;
#ifdef Q_OS_ANDROID
        if (d->remote) path = d->remoteRoot + '/' + iiSocietyContainer::storeSectionKey(section);
        else
#endif
        path = d->storage->drive().sectionPath(section);
        result.append(QVariantMap{{"key", iiSocietyContainer::storeSectionKey(section)},
            {"name", iiSocietyContainer::storeSectionName(section)},
            {"path", path}});
    }
    return result;
}

QString FileSystem::path(const QString &sectionKey, const QString &relativePath)
{
    const auto section = d->section(sectionKey);
    if (!section || !d->ready()) return {};
#ifdef Q_OS_ANDROID
    if (d->remote) return d->request(QStringLiteral("path"), sectionKey, relativePath).value("path").toString();
#endif
    QString error;
    const auto path = d->storage->filePath(*section, relativePath, &error);
    d->setError(error);
    return path;
}

QUrl FileSystem::url(const QString &sectionKey, const QString &relativePath)
{
    const auto native = path(sectionKey, relativePath);
#ifdef Q_OS_ANDROID
    if (d->remote) return QUrl(native);
#endif
    return native.isEmpty() ? QUrl() : QUrl::fromLocalFile(native);
}

QString FileSystem::ensureDirectory(const QString &sectionKey, const QString &relativePath)
{
    const auto section = d->section(sectionKey);
    if (!section || !d->ready()) return {};
#ifdef Q_OS_ANDROID
    if (d->remote) return d->request(QStringLiteral("mkdir"), sectionKey, relativePath).value("path").toString();
#endif
    QString error;
    const auto path = d->storage->ensureDirectory(*section, relativePath, &error);
    d->setError(error);
    return path;
}
QString FileSystem::ensureRuntimeDirectory(const QString &relativePath) {
    if (!d->ready() || !d->storage) return {};
    QString error; const auto directory = d->storage->ensureRuntimeDirectory(relativePath, &error);
    setError(error); return directory;
}

QVariantList FileSystem::entries(const QString &sectionKey, const QString &relativePath)
{
    const auto section = d->section(sectionKey);
    if (!section || !d->ready()) return {};
#ifdef Q_OS_ANDROID
    if (d->remote) return d->request(QStringLiteral("list"), sectionKey, relativePath).value("entries").toArray().toVariantList();
#endif
    QString error;
    const auto directory = d->storage->filePath(*section, relativePath, &error);
    if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
        d->setError(error.isEmpty() ? QStringLiteral("Society directory is unavailable.") : error);
        return {};
    }
    QVariantList result;
    for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
        if (iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(entry.fileName().toStdU16String()))) continue;
        const auto relative = QDir(d->storage->drive().sectionPath(*section)).relativeFilePath(entry.absoluteFilePath());
        const auto checked = d->storage->filePath(*section, relative, &error);
        if (!checked.isEmpty() && (entry.isDir() || entry.isFile()))
            result.append(QVariantMap{{"name", entry.fileName()}, {"path", checked}, {"isDirectory", entry.isDir()}, {"size", entry.size()},
                {"informationKey", entry.isFile() ? sectionKey + '/' + relative : QString()}, {"informationAvailable", entry.isFile()}});
    }
    d->setError({});
    return result;
}

QVariantMap FileSystem::fileInformation(const QString &sectionKey, const QString &relativePath)
{
    const auto section = d->section(sectionKey);
    if (!section || !d->ready()) return {};
    if (!d->storage) { d->setError(QStringLiteral("Use the host inspection API for non-native storage.")); return {}; }
    QString error;
    const auto information = d->storage->fileInformation(*section, relativePath, &error);
    d->setError(error); return information.toVariantMap();
}
QString FileSystem::publishGeneratedImage(const QString &source, const QString &fileName)
{
    if (!d->ready()) return {};
    if (!d->storage) {
        d->setError(QStringLiteral("Native PNG publication is unavailable for this storage provider.")); return {};
    }
    QString error;
    const auto path = d->storage->publishGeneratedImage(source, fileName, &error);
    d->setError(error);
    if (!path.isEmpty()) synchronize();
    return path;
}
QString FileSystem::publishFile(const QString &source, const QString &sectionKey, const QString &relativePath) {
    const auto destination = path(sectionKey, relativePath);
    if (destination.isEmpty()) return {};
    if (QFileInfo(relativePath).suffix().compare("iisc", Qt::CaseInsensitive) == 0 && sectionKey != "files") {
        setError("Society canvas documents belong to Files."); return {};
    }
    try { detail::ManagedFileOperation operation(destination, "Import file"); iiFileProvider::File::copy(source, destination, false); operation.published(); synchronize(); return destination; }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
bool FileSystem::removeFile(const QString &sectionKey, const QString &relativePath) {
    const auto selected = alias(sectionKey, relativePath);
    if (!selected.value("available").toBool()) return false;
    try { detail::ManagedFileOperation operation(selected["path"].toString(), "Remove file"); const bool removed = iiFileProvider::File::remove(selected["path"].toString()); if (removed) operation.removed(); synchronize(); return removed; }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return false; }
}
QVariantMap FileSystem::saveFile(const QString &destination, const QByteArray &bytes, bool overwrite) {
    try {
        const auto selected = iiFileProvider::File::absolutePath(destination);
        if (selected.endsWith(".iisc", Qt::CaseInsensitive)) { setError("Use saveCanvas for native documents."); return {}; }
        if (const auto drive = iiSocietyContainer::FileOperations::containingDrive(selected)) {
            if (!d->ready() || drive->identifier() != containerId()) { setError("The export belongs to another Society container."); return {}; }
            bool accepted = false;
            for (const auto &entry : sections()) {
                const auto section = entry.toMap(); const auto root = section["path"].toString();
                if (selected.startsWith(root + '/') && detail::samePathSpelling(path(section["key"].toString(), QDir(root).relativeFilePath(selected)), selected)) accepted = true;
            }
            if (!accepted) { setError("Choose a file in a Society section."); return {}; }
        }
        detail::ManagedFileOperation operation(selected, "Save file");
        if (overwrite) iiFileProvider::File::write(selected, bytes); else iiFileProvider::File::create(selected, bytes);
        operation.published();
        synchronize(); setError({}); return aliasForPath(selected);
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
bool FileSystem::removeGenerationInputs(const QString &directory) {
    const auto base = path("files");
    if (base.isEmpty() || !directory.startsWith(base + "/Dreamscapes/GenerationInputs/")) {
        setError("Only owned generation input directories can be removed."); return false;
    }
    const QFileInfo info(directory);
    if (!info.exists()) return true;
    if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != directory) {
        setError("The input directory was redirected and has been preserved."); return false;
    }
    if (!QDir(directory).removeRecursively()) { setError("Cannot remove generation input files."); return false; }
    synchronize(); return true;
}
}
