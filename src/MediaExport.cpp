#include "iiSocietyHelper.h"
#include <File.h>
#include <QFileInfo>
#include <QImageReader>
namespace iiSocietyHelper {
bool FileSystem::exportMedia(const QUrl &source, const QUrl &destination, bool video)
{
    const auto fail = [this](const QString &message) { setError(message); return false; };
    if (!source.isLocalFile() || !QFileInfo(source.toLocalFile()).isFile())
        return fail(tr("The original image file could not be found."));
    QString target;
    if (destination.isLocalFile()) target = destination.toLocalFile();
#ifdef Q_OS_ANDROID
    // Android's system picker grants access to a document URI, not a file path.
    else if (destination.scheme() == "content") target = destination.toString();
#endif
    if (target.isEmpty()) return fail(tr("Choose a location to save the file."));
    if (target.endsWith(".iisc", Qt::CaseInsensitive)) return fail(tr("Use native canvas saving for .iisc documents."));

    try {
        auto input = iiFileProvider::File::openRead(source.toLocalFile());
        QImageReader reader(input.get());
        if ((video ? QFileInfo(source.toLocalFile()).suffix().toLower() != "mp4" || input->peek(12).mid(4,4) != "ftyp" : !reader.canRead()) || !input->seek(0))
            return fail(tr("The source file is not a readable media file."));

        if (destination.isLocalFile() && QFileInfo(target).canonicalFilePath() == QFileInfo(source.toLocalFile()).canonicalFilePath()) {
            setError({}); synchronize();
            return true;
        }
        iiFileProvider::File::copyToUrl(*input, destination);
        setError({}); synchronize();
        return true;
    } catch (const std::exception &error) {
        return fail(tr("Could not export the file: %1").arg(QString::fromUtf8(error.what())));
    }
}

}
