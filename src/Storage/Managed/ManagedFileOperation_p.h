#pragma once
#include <SharedStorage.h>
#include <FileOperations.h>
#include <QDir>
#include <QFileInfo>
#include <stdexcept>

namespace iiSocietyHelper::detail {
inline bool samePathSpelling(const QString &first, const QString &second) {
#ifdef Q_OS_DARWIN
    return first.normalized(QString::NormalizationForm_C) == second.normalized(QString::NormalizationForm_C);
#else
    return first == second;
#endif
}
/// Capture an existing original before a write; record the completed file in the
/// same work session. Non-Society exports use the ordinary provider publication.
class ManagedFileOperation final {
public:
    ManagedFileOperation(const QString &path, const std::string &description) {
        const auto drive = iiSocietyContainer::FileOperations::containingDrive(path);
        if (!drive) return;
        QString error; const auto storage = iiSocietyContainer::SharedStorage::open(drive->rootPath(), &error);
        auto section = drive->sectionForPath(path);
        if (!section && !QFileInfo::exists(path)) section = drive->sectionForPath(QFileInfo(path).absolutePath());
        if (!storage || !section) throw std::runtime_error("The Society destination is unavailable.");
        const auto relative = QDir(drive->sectionPath(*section)).relativeFilePath(path);
        const auto verified = storage->filePath(*section, relative, &error);
        if (verified.isEmpty() || !samePathSpelling(verified, path)) throw std::runtime_error("The Society destination is redirected.");
        resolve = [storage = *storage, section = *section, relative] { return storage.managedFilePath(section, relative); };
        logical = resolve();
        iiSocietyContainer::FileRepositoryContext context; context.description = description; context.work.document = logical;
        repository = storage->fileRepository(std::move(context));
        if (QFileInfo(path).isFile()) repository->observe(logical);
    }
    void published() { if (repository) repository->observe(resolve()); }
    void removed() { if (repository) repository->removed(logical); }
private:
    std::string logical;
    std::function<std::string()> resolve;
    std::unique_ptr<iiSocietyContainer::FileRepository> repository;
};
}
