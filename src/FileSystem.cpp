#include <iiFileProviderDevice.h>
#include "FilePresentation_p.h"
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
#include <iiSocietyMonitor/Monitor.h>
#ifdef Q_OS_ANDROID
#include <AndroidStorage.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#endif

namespace iiSocietyHelper {
using iiSocietyContainer::SharedStorage;
using iiSocietyContainer::StoreSection;

class FileSystem::Private {
public:
    explicit Private(FileSystem *owner) : q(owner), container(owner) {}
    FileSystem *q;
    iiSocietyContainer::Container container;
    QTimer synchronization;
    QString selection;
    QString error;
    std::optional<SharedStorage> storage;
    iiSocietyMonitor::Subscription monitor;
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
        (void)monitor.start(root, id, [owner, epoch](const iiSocietyMonitor::State &state) {
            const auto json = iiSocietyMonitor::toJson(state);
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
                    owner->synchronize();
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

FileSystem::FileSystem(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this))
{
    detail::installCanvasPresentation();
    connect(&d->container, &iiSocietyContainer::Container::refreshRequested, this, &FileSystem::synchronize);
    d->synchronization.setInterval(2000);
    connect(&d->synchronization, &QTimer::timeout, this, &FileSystem::synchronize);
    d->synchronization.start();
    open();
}
FileSystem::~FileSystem() { ++d->monitorEpoch; d->monitor.stop(); }

bool FileSystem::open(const QString &containerPath)
{
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
    const QFileInfo info(native);
    const bool available = info.isFile() && !info.isSymLink() && !info.isJunction() && info.canonicalFilePath() == native;
    const auto key = sectionKey + '/' + relativePath;
    const auto object = declared;
    const bool exists = available || (!object.isEmpty() && object.value("kind").toString() == "file");
    const auto size = available ? info.size() : object.value("size").toVariant().toLongLong();
    const auto suffix = info.suffix().toLower();
    const bool image = QStringList{"png","jpg","jpeg","webp","bmp","gif","tif","tiff","avif","heic","heif"}.contains(suffix);
    const auto source = available ? QUrl::fromLocalFile(native) : QUrl{};
    auto preview = image ? source : QUrl{};
    if (!preview.isEmpty()) preview.setQuery(QString("v=%1-%2").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()));
    QVariantMap value{{"id", containerId() + '/' + key}, {"containerId", containerId()}, {"key", key},
        {"informationKey", key}, {"informationAvailable", exists},
        {"section", sectionKey}, {"relativePath", relativePath}, {"name", info.fileName()},
        {"exists", exists}, {"available", available}, {"path", available ? native : QString{}},
        {"source", source}, {"previewSource", preview}, {"thumbnailSource", preview},
        {"isImage", image}, {"size", size},
        {"modified", info.lastModified().toUTC()}, {"folderPath", info.absolutePath()},
        {"history", sectionKey == "generation-history"},
        {"iconName", image ? "fileTypesimage" : suffix == "iisc" ? "application" : "fileTypestext"},
        {"description", iiSocietyContainer::storeSectionName(*section) + " · " + (image ? "Image" : suffix == "iisc" ? "Canvas" : "Document")},
        {"metadata1", suffix.toUpper() + " · " + QLocale().formattedDataSize(size)},
        {"metadata2", "Society / " + iiSocietyContainer::storeSectionName(*section)}};
    if (available) value["version"] = QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size());
    else if (!object.isEmpty()) value["version"] = object.value("version").toString();
    if (!available && d->storage && !object.isEmpty()) {
        const auto remotePreview = iiSocietyContainer::StorageMap(d->storage->drive()).previewPath(object);
        value["previewSource"] = remotePreview.isEmpty() ? QUrl{} : QUrl::fromLocalFile(remotePreview);
        value["thumbnailSource"] = value["previewSource"];
    }
    return value;
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
    if (!info.isFile() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != absolute) return {};
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
                    if (name.isEmpty() || name.startsWith('.') || name.contains('/') || name.contains('\\') || name.contains(QChar::Null)) continue;
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
    if (!isAvailable() || !d->storage) {
        d->container.synchronize({}, {}, {}, false, errorString()); return false;
    }
    QVariantList values;
    for (const auto &entry : sections()) {
        const auto section = entry.toMap(); const auto key = section["key"].toString();
        if (key != "files" && key != "published" && key != "generation-history") continue;
        const auto directory = section["path"].toString();
        QDirIterator files(directory, QDir::Files | QDir::NoSymLinks, key == "generation-history" ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories);
        while (files.hasNext() && values.size() < 25000) {
            const auto file = files.next();
            if (files.fileName().startsWith('.')) continue;
            const auto row = alias(key, QDir(directory).relativeFilePath(file));
            if (row.value("available").toBool()) values.append(row);
        }
    }
    const iiSocietyContainer::StorageMap map(d->storage->drive());
    if (!map.isLocalAuthority()) for (const auto &entry : map.objects()) {
        const auto object = entry.toObject(); const auto key = object.value("path").toString();
        const auto section = key.section('/', 0, 0);
        if (object.value("kind").toString() != "file" || (section != "files" && section != "published" && section != "generation-history")) continue;
        if (std::none_of(values.begin(), values.end(), [&](const QVariant &value) { return value.toMap()["key"].toString() == key; })) {
            const auto row = alias(section, key.mid(section.size() + 1)); if (!row.isEmpty()) values.append(row);
        }
    }
    d->setError({});
    d->container.synchronize(containerId(), rootPath(), values, true); return true;
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
    try { iiFileProvider::File::copy(source, destination, false); synchronize(); return destination; }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
bool FileSystem::removeFile(const QString &sectionKey, const QString &relativePath) {
    const auto selected = alias(sectionKey, relativePath);
    if (!selected.value("available").toBool()) return false;
    try { const bool removed = iiFileProvider::File::remove(selected["path"].toString()); synchronize(); return removed; }
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
                if (selected.startsWith(root + '/') && path(section["key"].toString(), QDir(root).relativeFilePath(selected)) == selected) accepted = true;
            }
            if (!accepted) { setError("Choose a file in a Society section."); return {}; }
        }
        if (overwrite) iiFileProvider::File::write(selected, bytes); else iiFileProvider::File::create(selected, bytes);
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
