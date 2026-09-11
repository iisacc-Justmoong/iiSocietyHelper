#pragma once
#include <QString>

namespace iiSocietyHelper::local {
// IPC state belongs to this device, never to a replicated Society drive.
bool validateDirectory(const QString &path, QString *error);
}
