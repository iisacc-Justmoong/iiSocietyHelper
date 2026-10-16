#include <iiFileProviderDevice.h>
#include "iiSocietyHelper.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace iiSocietyHelper {
namespace {
bool jobId(const QString &value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value; }
bool removeObsoleteEntry(const QFileInfo &entry)
{
    if (entry.isSymLink() || entry.isJunction()) {
#ifdef Q_OS_WIN
        if (entry.isDir())
            return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(entry.filePath().utf16())) != 0;
#endif
        return QFile::remove(entry.filePath());
    }
    if (!entry.isDir()) return QFile::remove(entry.filePath());
    for (const auto &child : iiFileProvider::Directory(entry.filePath()).entryInfoList(
             iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::Hidden | iiFileProvider::Directory::System))
        if (!removeObsoleteEntry(child)) return false;
    return iiFileProvider::Directory().rmdir(entry.filePath());
}
bool regularTree(const QString &path) {
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction() || info.canonicalFilePath() != path) return false;
    if (info.isFile()) return true;
    if (!info.isDir()) return false;
    for (const auto &entry : iiFileProvider::Directory(path).entryInfoList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::Hidden))
        if (!regularTree(entry.absoluteFilePath())) return false;
    return true;
}
QByteArray digest(const QString &path, const std::atomic_bool &cancelled) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly)) return {};
    while (!file.atEnd()) {
        if (cancelled) return {};
        const auto bytes = file.read(1024 * 1024); if (file.error() != QFileDevice::NoError) return {};
        hash.addData(bytes);
    }
    return hash.result();
}
}
bool FileSystem::discardLegacyGenerationStorage()
{
    const auto fail = [this](const QString &error) { setError(error); return false; };
    const auto tr = [](const char *text) { return QString::fromUtf8(text); };
    // Upgrade cleanup only. New sessions never serialize or restore their queue.
    for (const auto &relative : {QStringLiteral(".dreamscapes/generation"), QStringLiteral("Generation History/Dreamscapes")}) {
        const auto directory = iiFileProvider::Directory(rootPath()).filePath(relative);
        const QFileInfo info(directory);
        if (!info.exists() && !info.isSymLink() && !info.isJunction()) continue;
        if (!info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != directory)
            return fail(tr("The obsolete generation directory was redirected; it was preserved."));
        iiFileProvider::LockFile legacyWorker(directory + "/.worker.lock");
        legacyWorker.setStaleLockTime(0);
        if (!legacyWorker.tryLock())
            return fail(tr("The previous Dreamscapes version is still using its generation files."));
        for (const auto &entry : iiFileProvider::Directory(directory).entryInfoList(iiFileProvider::Directory::Dirs | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::NoSymLinks)) {
            if (!jobId(entry.fileName()) || entry.canonicalFilePath() != entry.absoluteFilePath()) continue;
            QFile request(entry.filePath() + "/request.json");
            const QFileInfo record(request);
            if (!record.isFile() || record.isSymLink() || record.size() > 1024 * 1024 || !request.open(QIODevice::ReadOnly)) continue;
            auto job = QJsonDocument::fromJson(request.readAll()).object();
            if (job.value("id") != entry.fileName() || job.value("appId") != "com.iisacc.dreamscapes"
                || job.value("schemaVersion") != 1 || job.value("state") != "completed") continue;
            const auto prefix = "Asset Library/Dreamscapes/" + entry.fileName() + '/';
            const auto image = job.value("image").toString();
            if (!image.startsWith(prefix)) continue; // Already published in flat History.
            const auto name = image.mid(prefix.size());
            if (name.contains('/') || name.contains('\\') || name.startsWith('.')
                || !QStringList{"png", "jpg", "jpeg", "webp"}.contains(QFileInfo(name).suffix().toLower()))
                return fail(tr("The previous generation record contains an invalid image path."));
            const auto original = iiFileProvider::Directory(rootPath()).filePath(image);
            if (!QFileInfo::exists(original) && !QFileInfo(original).isSymLink()) continue; // User-deleted result.
            QString error;
            const auto source = path("asset-library", "Dreamscapes/" + entry.fileName() + '/' + name);
            if (source.isEmpty()) return fail(error);
            const auto destination = path("generation-history", entry.fileName() + "-0001." + QFileInfo(name).suffix().toLower());
            if (destination.isEmpty()) return fail(error);
            if (QFileInfo::exists(destination)) {
                // Retry a cleanup interrupted after the final image was committed.
                QFile left(source), right(destination);
                QCryptographicHash a(QCryptographicHash::Sha256), b(QCryptographicHash::Sha256);
                if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)
                    || !a.addData(&left) || !b.addData(&right) || a.result() != b.result())
                    return fail(tr("A generated image with this name already exists. Existing files were preserved."));
            } else if (publishFile(source, "generation-history", QFileInfo(destination).fileName()).isEmpty()) return fail(errorString());
            if (!QFile::remove(source)) return fail(tr("Cannot remove the old generated image after publishing it."));
            const auto oldOutput = QFileInfo(source).absolutePath();
            for (const auto &name : QStringList{"generation.json", QFileInfo(source).fileName() + ".json", QFileInfo(source).completeBaseName() + ".json"}) {
                const auto path = oldOutput + '/' + name;
                const QFileInfo metadata(path);
                if (metadata.isFile() && !metadata.isSymLink() && metadata.canonicalFilePath() == path)
                    QFile::remove(path);
            }
            iiFileProvider::Directory().rmdir(oldOutput); // Preserve unrelated Assets in nonempty directories.
            iiFileProvider::Directory().rmdir(QFileInfo(oldOutput).absolutePath());
        }
        for (const auto &entry : iiFileProvider::Directory(directory).entryInfoList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::Hidden | iiFileProvider::Directory::System)) {
            if (entry.fileName() == ".worker.lock") continue;
            const bool removed = removeObsoleteEntry(entry);
            if (!removed) return fail(tr("Cannot remove the obsolete generation working files."));
        }
        legacyWorker.unlock();
        if (!iiFileProvider::Directory().rmdir(directory)) return fail(tr("Cannot remove the obsolete generation directory."));
    }
    const QFileInfo parent(iiFileProvider::Directory(rootPath()).filePath(".dreamscapes"));
    if (parent.isDir() && !parent.isSymLink() && !parent.isJunction()
        && parent.canonicalFilePath() == parent.absoluteFilePath())
        iiFileProvider::Directory().rmdir(parent.filePath()); // Empty app-owned parent only.
    return true;
}

bool migrateLegacyCache(const QString &source, const QString &destination,
    const std::atomic_bool &cancelled, QString *error)
{
    if (source.isEmpty() || (!QFileInfo::exists(source) && !QFileInfo(source).isSymLink())) return true;
    if (!regularTree(source) || !regularTree(destination) || source == destination
        || destination.startsWith(source + '/') || source.startsWith(destination + '/')) {
        if (error) *error = "The previous model cache was redirected and has been preserved.";
        return false;
    }
    const auto move = [&](auto &&self, const QString &from, const QString &to) -> bool {
        for (const auto &entry : iiFileProvider::Directory(from).entryInfoList(iiFileProvider::Directory::AllEntries | iiFileProvider::Directory::NoDotAndDotDot | iiFileProvider::Directory::Hidden)) {
            if (cancelled) return false;
            const auto target = iiFileProvider::Directory(to).filePath(entry.fileName());
            if (entry.isDir()) {
                if (!QFileInfo::exists(target) && !iiFileProvider::Directory().mkdir(target)) return false;
                if (!regularTree(target) || !QFileInfo(target).isDir() || !self(self, entry.filePath(), target)) return false;
            } else if (QFileInfo::exists(target)) {
                const auto original = digest(entry.filePath(), cancelled);
                if (original.isEmpty() || !regularTree(target) || original != digest(target, cancelled)
                    || cancelled
                    || !QFile::remove(entry.filePath())) return false;
            } else if (!QFile::rename(entry.filePath(), target)) return false;
        }
        return iiFileProvider::Directory().rmdir(from);
    };
    if (!move(move, source, destination)) {
        if (error) *error = cancelled ? "Model cache migration was cancelled."
            : "Cannot move the previous model cache to Society. Existing files were preserved.";
        return false;
    }
    return true;
}
}
