#pragma once
#include <SharedStorage.h>
#include <FileOperations.h>
#include <DirectoryMetadataIndex.h>
#include <FileCatalog.h>
#include <iiFileProviderPresentation.h>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace iiSocietyHelper::detail {
/// Compare an observed native path without collapsing distinct names on other platforms.
inline bool samePathSpelling(const QString &first, const QString &second) {
#ifdef Q_OS_DARWIN
    return first.normalized(QString::NormalizationForm_C) == second.normalized(QString::NormalizationForm_C);
#else
    return first == second;
#endif
}
/// Coordinate a selected Society publication with directory-owned metadata.
/// This object never snapshots the file or creates an ObjectStore history.
class ManagedFileOperation final {
public:
    /// Pin the destination and, for an existing file, its metadata owner before
    /// any write. A legacy guard retains its classification without copying
    /// replacement bytes into the historical object store.
    ManagedFileOperation(const QString &path, const std::string &description, bool replacing = false) {
        (void)description;
        const auto drive = iiSocietyContainer::FileOperations::containingDrive(path);
        if (!drive) return;
        QString error; auto selected = iiSocietyContainer::SharedStorage::open(drive->rootPath(), &error);
        auto part = drive->sectionForPath(path);
        if (!part && !QFileInfo::exists(path)) part = drive->sectionForPath(QFileInfo(path).absolutePath());
        if (!selected || !part) throw std::runtime_error("The Society destination is unavailable.");
        const auto relative = QDir(drive->sectionPath(*part)).relativeFilePath(path);
        const auto verified = selected->filePath(*part, relative, &error);
        if (verified.isEmpty() || !samePathSpelling(verified, path)) throw std::runtime_error("The Society destination is redirected.");
        root=std::filesystem::path(drive->rootPath().toStdU16String());
        if (iiSocietyContainer::FileRepository::requiresLegacyMigration(root))
            throw std::runtime_error("Legacy file history requires offline migration before publication.");
        storage=std::move(*selected);section=*part;name=relative;
        const auto bytes=std::filesystem::path(verified.toStdU16String()).lexically_relative(root).generic_u8string();
        address={reinterpret_cast<const char *>(bytes.data()),bytes.size()};
        if (replacing) {
            const auto lockPath=QFileInfo(path).absolutePath()+"/."+QFileInfo(path).fileName()+".society-managed.lock";
            lock=std::make_unique<QLockFile>(lockPath);lock->setStaleLockTime(0);
            if (!lock->tryLock()) throw std::runtime_error("The Society file is being edited by another operation.");
        }
        if (!QFileInfo(path).isFile()) return;
        if (!storage->recordFile(section,name,&error)) throw std::runtime_error(error.toStdString());
        // Apple file APIs can return a canonically equivalent spelling rather
        // than the indexed native directory entry. Use the storage owner's
        // verified logical spelling when resolving its sidecar address.
        const auto logical=storage->managedFilePath(section,name);
        const auto slash=logical.find('/');
        if (slash==std::string::npos) throw std::runtime_error("The Society file path is invalid.");
        const auto relativeNative=std::filesystem::path(std::u8string(logical.begin()+slash+1,logical.end()));
        const auto actual=(std::filesystem::path(drive->sectionPath(section).toStdU16String())/relativeNative).lexically_relative(root).generic_u8string();
        address={reinterpret_cast<const char *>(actual.data()),actual.size()};
        const auto database=root/".iisacc-file-provider"/"metadata-paths.sqlite3";
        if (std::filesystem::exists(database)) {
            iiFileProvider::DirectoryMetadataIndex index(root,iiFileProvider::DirectoryMetadataIndex::Access::ReadOnly);
            if (index.contains(address)) { expected=index.read(address);before=index.inspectAsset(address); }
        }
        if (replacing && !expected) {
            const auto sectionRoot=std::filesystem::path(drive->sectionPath(section).toStdU16String());
            auto privateDocument=iiSocietyContainer::FileRepository::classificationDirectory(
                root,iiSocietyContainer::storeSectionKey(section).toStdString())/relativeNative;
            privateDocument += ".iisacc.json";
            const auto metadataRoot=std::filesystem::exists(privateDocument)
                ? iiSocietyContainer::FileRepository::classificationDirectory(root,iiSocietyContainer::storeSectionKey(section).toStdString())
                : std::filesystem::path{};
            legacy=std::make_unique<iiFileProvider::FileCatalog>(sectionRoot,
                iiFileProvider::FileCatalog::Access::ReadWrite,metadataRoot);
            const auto current=legacy->lookupPath(logical.substr(slash+1));
            if (!current) throw std::runtime_error("The legacy classification owner is unavailable.");
            legacyGuard=legacy->prepareReplacement(current->key);
        }
    }
    /// Publish metadata only after the application has completed its own write.
    void published() {
        if (!storage) return;
        const auto native=root/std::filesystem::path(std::u8string(address.begin(),address.end()));
        // Resume presentation on success and on error, after the source lock
        // has been released, so a failed publication does not lose a preview.
        struct ResumePresentation {
            std::filesystem::path path;
            bool armed=false;
            ~ResumePresentation() { if (armed) iiFileProvider::native::Presentation::enqueue(path); }
        } resume{native};
        std::unique_ptr<iiFileProvider::native::SourceAccess> sourceAccess;
        std::unique_lock<iiFileProvider::native::SourceAccess> sourceLock;
        if (expected || legacyGuard) {
            // Preview publication can change native attributes immediately
            // after an atomic save. Stabilize that worker while pinning the
            // replacement, then put preview work back on its normal queue.
            iiFileProvider::native::Presentation::cancelPending(native);
            resume.armed=true;
            sourceAccess=std::make_unique<iiFileProvider::native::SourceAccess>(native);
            sourceLock=std::unique_lock<iiFileProvider::native::SourceAccess>(*sourceAccess);
        }
        if (expected) {
            iiFileProvider::DirectoryMetadataIndex index(root,iiFileProvider::DirectoryMetadataIndex::Access::ReadWrite);
            const auto current=index.inspectReplacement(address,*expected);
            if (current.identity!=before->identity || current.contentVersion!=before->contentVersion || current.size!=before->size)
                (void)index.rebindReplacement(address,*expected,current);
        } else if (legacyGuard) {
            const auto current=legacy->inspectReplacement(*legacyGuard);
            if (current.identity!=legacyGuard->identity || current.contentVersion!=legacyGuard->contentVersion) {
                const auto rebound=legacy->rebindReplacement(*legacyGuard,current);
                if (!rebound.metadataWritten) throw std::runtime_error(rebound.metadataError);
            }
        } else {
            QString error;
            if (!storage->recordFile(section,name,&error)) throw std::runtime_error(error.toStdString());
        }
    }
private:
    std::optional<iiSocietyContainer::SharedStorage> storage;
    iiSocietyContainer::StoreSection section{};
    QString name;
    std::filesystem::path root;
    std::string address;
    std::optional<std::string> expected;
    std::optional<iiFileProvider::DirectoryAssetState> before;
    std::unique_ptr<iiFileProvider::FileCatalog> legacy;
    std::optional<iiFileProvider::CatalogReplacementGuard> legacyGuard;
    std::unique_ptr<QLockFile> lock;
};
}
