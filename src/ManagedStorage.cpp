#include "iiSocietyHelper.h"
#include <SharedStorage.h>
#include <QJsonArray>
#include <QJsonObject>
#include <array>
#include <algorithm>
#include <stdexcept>

namespace iiSocietyHelper {
using namespace iiFileProvider;
namespace {
std::optional<iiSocietyContainer::SharedStorage> selected(FileSystem &files) {
    if (!files.isAvailable()) throw std::runtime_error("Choose an available native Society drive.");
    QString error; auto storage = iiSocietyContainer::SharedStorage::open(files.rootPath(), &error);
    if (!storage || storage->drive().identifier() != files.containerId()) throw std::runtime_error("The selected Society drive changed.");
    return storage;
}
std::int64_t sequence(const QString &text) {
    if (text.isEmpty()) return 0;
    bool ok = false; const auto value = text.toLongLong(&ok); if (!ok || value < 0) throw std::runtime_error("Invalid journal cursor."); return value;
}
}
bool FileSystem::indexFiles() {
    try { const auto storage = selected(*this); QString error; const bool ok = storage->reconcileFiles(&error); setError(error); if (ok) synchronize(); return ok; }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return false; }
}
QVariantMap FileSystem::searchFiles(const QVariantMap &request) {
    try { const auto storage = selected(*this); QString error; const auto page = storage->searchFiles(QJsonObject::fromVariantMap(request), &error); setError(error); return page.toVariantMap(); }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QVariantList FileSystem::fileJournal(const QString &afterSequence, int limit, const QString &objectKey) {
    try {
        const auto storage = selected(*this); auto repository = storage->fileRepository({}, iiSocietyContainer::FileRepository::Access::ReadOnly);
        const auto events = repository->objects().journal({sequence(afterSequence), std::size_t(limit), objectKey.toStdString()});
        QVariantList result;
        for (const auto &event : events) result.append(QVariantMap{{"sequence", QString::number(event.sequence)},
            {"objectKey", QString::fromStdString(event.objectKey)}, {"version", QString::number(event.version)},
            {"operation", QString::fromStdString(event.operation)}, {"path", QString::fromStdString(event.path)},
            {"sessionKey", QString::fromStdString(event.sessionKey)}, {"deleted", event.deleted},
            {"sha256", QString::fromStdString(event.sha256)}});
        setError({}); return result;
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QVariantList FileSystem::workSessions(const QString &afterSequence, int limit) {
    try {
        const auto storage = selected(*this); auto repository = storage->fileRepository({}, iiSocietyContainer::FileRepository::Access::ReadOnly);
        const auto sessions = repository->objects().sessions({sequence(afterSequence), std::size_t(limit)});
        QVariantList result;
        for (const auto &work : sessions) result.append(QVariantMap{{"sequence", QString::number(work.sequence)},
            {"key", QString::fromStdString(work.session.key)}, {"application", QString::fromStdString(work.context.application)},
            {"applicationVersion", QString::fromStdString(work.context.applicationVersion)}, {"project", QString::fromStdString(work.context.project)},
            {"document", QString::fromStdString(work.context.document)}, {"description", QString::fromStdString(work.session.description)},
            {"startedAtNs", QString::number(work.session.startedAtNs)}, {"endedAtNs", QString::number(work.session.endedAtNs)}});
        setError({}); return result;
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
bool FileSystem::setFileMetadata(const QString &sectionKey, const QString &relative, const QVariantMap &input) {
    try {
        const auto storage = selected(*this); FileMetadata metadata;
        metadata.title = input.value("title").toString().toStdString(); metadata.description = input.value("description").toString().toStdString();
        const auto rating = input.value("rating", 0).toLongLong(); if (rating < 0 || rating > 5) throw std::runtime_error("Rating must be 0 to 5."); metadata.rating = unsigned(rating);
        for (const auto &word : input.value("keywords").toStringList()) metadata.keywords.push_back(word.toStdString());
        for (const auto &key : input.value("collections").toStringList()) metadata.collections.push_back(key.toStdString());
        const auto fields = input.value("fields").toMap(); for (auto it = fields.begin(); it != fields.end(); ++it) metadata.fields.emplace(it.key().toStdString(), it.value().toString().toStdString());
        constexpr std::array<const char *, 8> colors{"none","red","orange","yellow","green","blue","purple","gray"};
        const auto color = input.value("color", "none").toString().toStdString(); const auto it = std::ranges::find(colors, color);
        if (it == colors.end()) throw std::runtime_error("Unknown color label."); metadata.color = static_cast<ColorLabel>(it - colors.begin());
        iiSocietyContainer::FileRepositoryContext context; context.description = "Edit file metadata"; context.work.document = (sectionKey + '/' + relative).toStdString();
        std::optional<iiSocietyContainer::StoreSection> section;
        for (const auto value : storage->drive().sections()) if (iiSocietyContainer::storeSectionKey(value) == sectionKey) section = value;
        if (!section) throw std::runtime_error("Unknown Society section.");
        storage->fileRepository(std::move(context))->observe(storage->managedFilePath(*section, relative), metadata);
        synchronize(); setError({}); return true;
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return false; }
}
bool FileSystem::applyOntology(const std::vector<OntologyChange> &changes, std::uint64_t expectedRevision) {
    try {
        const auto storage = selected(*this); iiSocietyContainer::FileRepositoryContext context; context.description = "Edit file ontology";
        auto repository = storage->fileRepository(std::move(context));
        repository->ontology().apply(changes, expectedRevision, repository->sessionKey()); setError({}); return true;
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return false; }
}
}
