#pragma once
#include <iiFileProviderPresentation.h>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>

namespace iiSocietyHelper::Sync::detail {
inline bool indexControlName(const QString &name) {
    const auto folded = name.toCaseFolded();
    return name.startsWith(".society-") || name.startsWith(".iiserverhost-")
        || name == ".iisacc-temporary.lock" || folded.endsWith(".iisacc-preview")
        || folded.startsWith(".iifile-stage-") || folded == ".iisacc-file-provider"
        || folded.endsWith(".iisacc-lock");
}
inline bool ownedPresentation(const QByteArray &bytes) {
    if(bytes.size() > qsizetype(iiFileProvider::native::Presentation::maximumDocumentBytes)) return false;
    const auto value = QJsonDocument::fromJson(bytes).object();
    return value.value("format") == "iiFileProvider.Presentation" && value.value("schema").toInt() == 1;
}
inline bool providerControlPath(const QString &path) {
    for (const auto &part : path.split('/'))
        if (iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(part.toStdU16String()))) return true;
    return false;
}
}
