#pragma once

#include <iiSocietyHelper/Export.h>
#include <QString>
#include <QUrl>

namespace iiSharedCanvas { struct Document; }

namespace iiSocietyHelper {

struct MediaExportResult {
    bool success = false;
    QString error;
};

struct MediaExportDestination {
    QString path;
    QString error;
};

// GUI file-picker destination policy, without a FileSystem/drive observer.
// Native canvases belong to Files; other media must remain in a declared
// Society section. Explicit external files retain normal provider validation.
[[nodiscard]] IISOCIETYHELPER_EXPORT MediaExportDestination mediaExportDestination(
    const QUrl &destination, bool nativeCanvas = false);

// Create a new native document copy directly from detached data. Never replaces
// an existing file, activates a selected FileSystem, or synchronizes a mirror. A targeted
// provider preview notification is retained. Scheduling belongs to the caller.
[[nodiscard]] IISOCIETYHELPER_EXPORT MediaExportResult exportCanvasCopy(
    const iiSharedCanvas::Document &document, const QUrl &destination);

// GUI file-picker adapter, including Android document URLs. Uses FileProvider
// media validation/copying without opening a Society drive, monitoring it or
// refreshing its mirror. The caller owns scheduling and completion delivery.
// Native IISC sources export frame zero as a full document-view PNG, retaining
// alpha and available PNG text. Legacy/working inputs are detached read-only
// snapshots; this is a flattened image command, not a native document copy.
[[nodiscard]] IISOCIETYHELPER_EXPORT MediaExportResult exportMedia(
    const QUrl &source, const QUrl &destination, bool video = false);

}
