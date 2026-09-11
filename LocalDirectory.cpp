#include "LocalDirectory.h"
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

namespace iiSocietyHelper::local {
bool validateDirectory(const QString &path, QString *error) {
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (!QDir::isAbsolutePath(path) || path.startsWith("//") || path.startsWith("\\\\"))
        return fail(QStringLiteral("Society Helper needs a directory on this device."));
    QString existing = QDir::cleanPath(path);
    while (!QFileInfo::exists(existing) && !QDir(existing).isRoot()) existing = QFileInfo(existing).absolutePath();
    const auto canonical = QFileInfo(existing).canonicalFilePath();
    for (const auto &start : {QDir::cleanPath(path), canonical}) {
        if (start.isEmpty()) continue;
        QDir ancestor(start);
        do {
            const QFileInfo manifest(ancestor.filePath(".society-drive.json"));
            if (manifest.exists() || manifest.isSymLink())
                return fail(QStringLiteral("Local Helper presence and messages cannot be stored inside a synchronized Society drive."));
        } while (ancestor.cdUp());
    }
    const auto type = QStorageInfo(existing).fileSystemType().toLower();
    for (const auto *remote : {"nfs", "smb", "cifs", "afp", "sshfs", "webdav", "9p"})
        if (type.contains(remote))
            return fail(QStringLiteral("Society Helper does not transport messages through a network filesystem. Use iiSocietySync between devices."));
    return true;
}
}
