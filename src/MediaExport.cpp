#include "iiSocietyHelper.h"
#include "iiSocietyHelper/MediaExport.h"
#include <File.h>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <iiSharedCanvas/File/DocumentFile.h>
#include <iiSharedCanvas/Render/FrameRenderer.h>
#include <iiSharedCanvas/Media/MediaIo.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <limits>
#include <stdexcept>
namespace iiSocietyHelper {
namespace {
void exportNativePng(const QString &source,const QUrl &destination,const QString &target) {
    using namespace iiSharedCanvas;
    if(destination.isLocalFile()&&QFileInfo(target).suffix().compare("png",Qt::CaseInsensitive)!=0)
        throw std::runtime_error("Choose a PNG file for this canvas image.");
    SerializationLimits limits;
    limits.maximumCanvasPixels=limits.maximumTotalRasterPixels=MediaLimits{}.maxPixelsPerFrame;
    limits.maximumContainerBytes=MediaLimits{}.maxOutputBytes;
    Document document;const auto loaded=DocumentFile::readSnapshot(source.toStdString(),document,limits);
    if(!loaded.ok())throw std::runtime_error(loaded.message);
    const auto region=documentViewRegion(document);
    if(!region.extent.width||!region.extent.height
        ||std::uint64_t(region.extent.width)*region.extent.height>MediaLimits{}.maxPixelsPerFrame
        ||region.extent.width>std::uint32_t(std::numeric_limits<int>::max()/4)
        ||region.extent.height>std::uint32_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("The canvas exceeds the image export size limit.");
    auto rendered=renderFrameTiles(document,0,{{region,region.extent,RasterSampling::Nearest}});
    if(!rendered.ok())throw std::runtime_error(rendered.message);
    if(rendered.tiles.size()!=1)throw std::runtime_error("The canvas image could not be rendered.");
    auto &pixels=rendered.tiles.front().pixels;
    // Borrow the owned frame for synchronous encoding; no second full raster or
    // encoded byte array is needed. The renderer and source document stay alive.
    QImage image(reinterpret_cast<uchar *>(pixels.pixels.data()),int(pixels.width),int(pixels.height),int(pixels.width*4),QImage::Format_ARGB32);
    if(image.isNull())throw std::runtime_error("The canvas image could not be encoded.");
    if(const auto &metadata=document.stableDiffusionMetadata) {
        const auto text=[&](const std::string &key,const std::string &value){if(!key.empty()&&!value.empty())image.setText(QString::fromStdString(key),QString::fromStdString(value));};
        for(const auto &entry:metadata->extraParameters)text(entry.key,entry.value);
        for(const auto &entry:metadata->comfyUi.extraPngInfo)text(entry.key,entry.value);
        text("prompt",metadata->comfyUi.promptJson);text("workflow",metadata->comfyUi.workflowJson);
        text("parameters",metadata->generationParametersText);
    }
    const auto encode=[&](QIODevice &device) {
        QImageWriter writer(&device,"png");
        if(!writer.write(image))throw std::runtime_error(writer.errorString().toStdString());
    };
    if(destination.isLocalFile())iiFileProvider::File::writeWith(target,encode);
#ifdef Q_OS_ANDROID
    else {
        // Document providers expose an opaque picker URI rather than an atomic
        // local stream. Retain the private PNG until that existing copy ends.
        iiFileProvider::native::TemporaryDirectory temporary(std::filesystem::temp_directory_path());
        const auto path=iiFileProvider::File::pathString(temporary.path()/"canvas.png");
        iiFileProvider::File::writeWith(path,encode);
        auto input=iiFileProvider::File::openRead(path);iiFileProvider::File::copyToUrl(*input,destination);
    }
#endif
}
}
MediaExportResult exportMedia(const QUrl &source, const QUrl &destination, bool video)
{
    const auto fail = [](const QString &message) { return MediaExportResult{false, message}; };
    if (!source.isLocalFile() || !QFileInfo(source.toLocalFile()).isFile())
        return fail(FileSystem::tr("The original image file could not be found."));
    QString target;
    if (destination.isLocalFile()) target = destination.toLocalFile();
#ifdef Q_OS_ANDROID
    // Android's system picker grants access to a document URI, not a file path.
    else if (destination.scheme() == "content") target = destination.toString();
#endif
    if (target.isEmpty()) return fail(FileSystem::tr("Choose a location to save the file."));
    if (target.endsWith(".iisc", Qt::CaseInsensitive)) return fail(FileSystem::tr("Use native canvas saving for .iisc documents."));

    try {
        if(QFileInfo(source.toLocalFile()).suffix().compare("iisc",Qt::CaseInsensitive)==0) {
            if(video)return fail(FileSystem::tr("Choose a video file to save this result."));
            exportNativePng(source.toLocalFile(),destination,target);
            return {true,{}};
        }
        auto input = iiFileProvider::File::openRead(source.toLocalFile());
        QImageReader reader(input.get());
        if ((video ? QFileInfo(source.toLocalFile()).suffix().toLower() != "mp4" || input->peek(12).mid(4,4) != "ftyp" : !reader.canRead()) || !input->seek(0))
            return fail(FileSystem::tr("The source file is not a readable media file."));

        if (destination.isLocalFile() && QFileInfo(target).canonicalFilePath() == QFileInfo(source.toLocalFile()).canonicalFilePath()) {
            return {true, {}};
        }
        iiFileProvider::File::copyToUrl(*input, destination);
        return {true, {}};
    } catch (const std::exception &error) {
        return fail(FileSystem::tr("Could not export the file: %1").arg(QString::fromUtf8(error.what())));
    }
}

bool FileSystem::exportMedia(const QUrl &source, const QUrl &destination, bool video)
{
    const auto result = iiSocietyHelper::exportMedia(source, destination, video);
    setError(result.error);
    if (!result.success) return false;
    synchronize();
    return true;
}

}
