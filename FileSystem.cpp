#include "iiSocietyHelper.h"
#include <SharedStorage.h>
#include <QDir>
#include <QFileInfo>
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
    explicit Private(FileSystem *owner) : q(owner) {}
    FileSystem *q;
    QString selection;
    QString error;
    std::optional<SharedStorage> storage;
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
    open();
}
FileSystem::~FileSystem() = default;

bool FileSystem::open(const QString &containerPath)
{
    d->selection = containerPath;
#ifdef Q_OS_ANDROID
    d->remote = !iiSocietyContainer::androidIsStorageOwner();
    if (d->remote) {
        d->storage.reset(); d->remoteId.clear(); d->remoteRoot.clear();
        const auto value = d->request(QStringLiteral("open"));
        if (!value.contains("error")) {
            const auto root = value.value("rootPath").toString();
            if (containerPath.isEmpty() || root == containerPath) {
                d->remoteRoot = root;
                d->remoteId = value.value("identifier").toString();
            } else d->setError(QStringLiteral("Android clients use Society's managed storage URI."));
        }
        emit storageChanged();
        return !d->remoteId.isEmpty();
    }
#endif
    QString error;
    d->storage = SharedStorage::open(d->selection, &error);
    const bool available = d->storage.has_value();
    d->setError(error);
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
        const auto relative = QDir(d->storage->drive().sectionPath(*section)).relativeFilePath(entry.absoluteFilePath());
        const auto checked = d->storage->filePath(*section, relative, &error);
        if (!checked.isEmpty() && (entry.isDir() || entry.isFile()))
            result.append(QVariantMap{{"name", entry.fileName()}, {"path", checked}, {"isDirectory", entry.isDir()}, {"size", entry.size()}});
    }
    d->setError({});
    return result;
}
}
