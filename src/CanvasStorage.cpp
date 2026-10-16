#include <iiFileProviderDevice.h>
#include "iiSocietyHelper.h"
#include <iiSharedCanvas/Bitmap/BitmapCodec.h>
#include <iiSharedCanvas/File/DocumentFile.h>
#include <iiSharedCanvas/Render/FrameRenderer.h>
#include <iiSharedCanvas/Serialization/IiscCodec.h>
#include <iiFileProvider.h>
#include <iiFileProviderPresentation.h>
#include <FileOperations.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
namespace iiSocietyHelper {
using namespace iiSharedCanvas;
namespace {
SerializationLimits limits() {
    SerializationLimits value;
    value.maximumCanvasPixels = value.maximumTotalRasterPixels = MediaLimits{}.maxPixelsPerFrame;
    value.maximumContainerBytes = MediaLimits{}.maxOutputBytes; return value;
}
MediaDocumentResult failed(const std::string &message) {
    MediaDocumentResult result; result.result = {MediaIoCode::InvalidData, message}; return result;
}
QByteArray bytesOf(const Document &document) {
    const auto encoded = encodeIisc(document, limits());
    if (!encoded.ok()) throw std::runtime_error(encoded.error.message);
    return {reinterpret_cast<const char *>(encoded.bytes.data()), qsizetype(encoded.bytes.size())};
}
QString checkedDestination(const QString &destination) {
    const auto absolute = iiFileProvider::File::absolutePath(destination);
    if (QFileInfo(absolute).suffix().compare("iisc", Qt::CaseInsensitive) != 0)
        throw std::runtime_error("Save the canvas to an .iisc file.");
    if (const auto drive = iiSocietyContainer::FileOperations::containingDrive(absolute)) {
        QString error; const auto storage = iiSocietyContainer::SharedStorage::open(drive->rootPath(), &error);
        const auto files = storage ? storage->filePath(iiSocietyContainer::StoreSection::Files, {}, &error) : QString{};
        if (files.isEmpty() || !absolute.startsWith(files + '/') || storage->filePath(iiSocietyContainer::StoreSection::Files,
            QDir(files).relativeFilePath(absolute), &error) != absolute)
            throw std::runtime_error("Save Society canvas documents in Files.");
    }
    return absolute;
}
QString lockPath(const QString &destination) {
    const QFileInfo file(destination);
    return QDir(file.absolutePath()).filePath('.' + file.fileName() + ".lock");
}
void createOrReuse(const QString &destination, const QByteArray &bytes) {
    iiFileProvider::LockFile lock(lockPath(destination));
    if (!lock.tryLock()) throw std::runtime_error("The canvas is being written by another operation.");
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()) {
        if (!QFileInfo(destination).isSymLink() && iiFileProvider::File::read(destination, bytes.size() + 1) == bytes) return;
        throw std::runtime_error("An existing canvas differs and has been preserved.");
    }
    iiFileProvider::File::create(destination, bytes);
}
}
MediaDocumentResult FileSystem::readCanvas(const QString &source) {
    try {
        const QFileInfo info(source);
        if (!info.isFile() || info.isSymLink() || info.isJunction()) return failed("Choose a readable local image or canvas.");
        if (info.suffix().compare("iisc", Qt::CaseInsensitive) == 0) {
            if (iiFileProvider::File::readPrefix(source, 16).startsWith("SQLite format 3")) {
                DocumentFile file; const auto opened = file.open(source.toStdString(), limits());
                if (!opened.ok()) return failed(opened.message);
                return {*file.document(), {}};
            }
            const auto bytes = iiFileProvider::File::read(source, limits().maximumContainerBytes);
            auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(bytes.constData()), std::size_t(bytes.size())}, limits());
            if (!decoded.ok()) return failed(decoded.error.message);
            return {std::move(decoded.document), {}};
        }
        BitmapImportOptions options; options.assetId = "image.fill"; options.extendedCodecs = false;
        auto imported = importBitmap(source.toStdString(), options);
        if (!imported.ok()) return failed(imported.result.message);
        Document document; document.extent = {imported.asset.pixels.width, imported.asset.pixels.height};
        document.layers.emplace_back(StaticBitmapLayer{{"image.fill.layer", info.fileName().toStdString()}, StaticSource{imported.asset.id}});
        document.assets.emplace_back(std::move(imported.asset));
        if (!imported.text.empty()) {
            StableDiffusionMetadata metadata; metadata.software = "Dreamscapes";
            for (const auto &entry : imported.text) metadata.extraParameters.push_back({entry.key, entry.value});
            document.stableDiffusionMetadata = std::move(metadata);
        }
        return {std::move(document), {}};
    } catch (const std::exception &e) { return failed(e.what()); }
}
QString FileSystem::canvasDestination(const QString &destination) {
    try { const auto path = checkedDestination(destination); setError({}); return path; }
    catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QVariantMap FileSystem::importCanvas(const QString &source) {
    try {
        auto imported = readCanvas(source);
        if (!imported.ok()) throw std::runtime_error(imported.result.message);
        if (!isAvailable()) throw std::runtime_error(errorString().toStdString());
        const QFileInfo info(source); const auto canonical = info.canonicalFilePath();
        const auto files = path("files"); if (files.isEmpty()) return {};
        const bool native = info.suffix().compare("iisc", Qt::CaseInsensitive) == 0;
        if (native && canonical.startsWith(files + '/')) return aliasForPath(canonical);
        const auto bytes = native ? iiFileProvider::File::read(canonical, limits().maximumContainerBytes) : bytesOf(imported.document);
        const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        const auto name = (native ? info.completeBaseName() : info.fileName()) + '.' + QString::fromLatin1(digest) + ".iisc";
        const auto destination = path("files", name); if (destination.isEmpty()) return {};
        createOrReuse(destination, bytes); synchronize(); setError({}); return aliasForPath(destination);
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QVariantMap FileSystem::createImageCanvas(const QString &source, const QString &destination) {
    try {
        auto imported = readCanvas(source);
        if (!imported.ok()) throw std::runtime_error(imported.result.message);
        const auto selected = checkedDestination(destination);
        createOrReuse(selected, bytesOf(imported.document)); synchronize(); setError({}); return aliasForPath(selected);
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QVariantMap FileSystem::saveCanvas(const QByteArray &encoded, const QString &destination, const QString &expectedVersion) {
    try {
        const auto selected = checkedDestination(destination);
        if (const auto owner = iiSocietyContainer::FileOperations::containingDrive(selected);
            owner && (!isAvailable() || owner->identifier() != containerId()))
            throw std::runtime_error("The canvas belongs to another or unavailable Society container.");
        auto decoded = decodeIisc({reinterpret_cast<const std::uint8_t *>(encoded.constData()), std::size_t(encoded.size())}, limits());
        if (!decoded.ok()) throw std::runtime_error(decoded.error.message);
        iiFileProvider::LockFile lock(lockPath(selected));
        if (!lock.tryLock()) throw std::runtime_error("The canvas is being written by another operation.");
        DocumentFile file;
        if (expectedVersion.isEmpty()) {
            const auto created = file.create(selected.toStdString(), decoded.document, limits());
            if (!created.ok()) throw std::runtime_error(created.message);
        } else {
            const auto current = aliasForPath(selected);
            if (!current.value("available").toBool() || current.value("version").toString() != expectedVersion)
                throw std::runtime_error("The canvas changed outside this session. Existing contents were preserved.");
            if (iiFileProvider::File::readPrefix(selected, 16).startsWith("SQLite format 3")) {
                const auto opened = file.open(selected.toStdString(), limits());
                if (!opened.ok()) throw std::runtime_error(opened.message);
                const auto edited = file.edit([&](Document &document) { document = decoded.document; return true; });
                if (!edited.ok()) throw std::runtime_error(edited.message);
            } else {
                const auto previous = iiFileProvider::File::read(selected, limits().maximumContainerBytes);
                if (QString::fromLatin1(QCryptographicHash::hash(previous, QCryptographicHash::Sha256).toHex()) != expectedVersion)
                    throw std::runtime_error("The canvas changed outside this session. Existing contents were preserved.");
                iiFileProvider::File::update(selected, previous, encoded);
            }
        }
        file.close();
        iiFileProvider::native::Presentation::enqueue(std::filesystem::path(selected.toStdU16String()));
        const auto saved = aliasForPath(selected);
        lock.unlock(); synchronize(); setError({}); return saved;
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
QUrl FileSystem::preview(const QString &source) {
    try {
        const auto current = aliasForPath(source);
        if (!current.value("available").toBool()) return {};
        if (QFileInfo(source).suffix().compare("iisc", Qt::CaseInsensitive) != 0)
            return current.value("previewSource", current.value("source")).toUrl();
        const auto directory = ensureRuntimeDirectory("canvas-previews"); if (directory.isEmpty()) return {};
        const auto identity = source.toUtf8() + ':' + current.value("version").toString().toUtf8();
        const auto destination = QDir(directory).filePath(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()) + ".png");
        if (QFileInfo(destination).isSymLink() || QFileInfo(destination).isDir()) throw std::runtime_error("The preview destination is redirected or occupied.");
        if (!QFileInfo(destination).isFile()) {
            const auto record = iiFileProvider::native::Presentation::prepare(std::filesystem::path(source.toStdU16String()));
            if (record.preview.kind != "image" || record.preview.mimeType != "image/png")
                throw std::runtime_error("A canvas content preview is unavailable.");
            createOrReuse(destination, {record.preview.bytes.data(), qsizetype(record.preview.bytes.size())});
        }
        setError({}); return QUrl::fromLocalFile(destination);
    } catch (const std::exception &e) { setError(QString::fromUtf8(e.what())); return {}; }
}
}
