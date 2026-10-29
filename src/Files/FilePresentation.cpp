#include "FilePresentation_p.h"
#include <iiFileProviderPresentation.h>
#include <iiSharedCanvas/Render/DocumentPreview.h>
#include <iiSharedCanvas/Bitmap/BitmapCodec.h>
#include <QCoreApplication>
#include <mutex>

namespace iiSocietyHelper::detail {
void installCanvasPresentation() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        using iiFileProvider::native::FilePreview;
        using iiFileProvider::native::Presentation;
        Presentation::setRenderer(".iisc", [](const std::filesystem::path &path, std::map<std::string, std::string> &properties) -> std::optional<FilePreview> {
            const auto utf8 = path.u8string();
            iiSharedCanvas::SerializationLimits limits;
            limits.maximumContainerBytes = Presentation::maximumSourceBytes;
            limits.maximumCanvasPixels = limits.maximumTotalRasterPixels = 16ull * 1024 * 1024;
            const auto rendered = iiSharedCanvas::readIiscPreview(std::string(utf8.begin(), utf8.end()), {}, limits);
            if (!rendered.ok()) return {};
            iiSharedCanvas::BitmapExportOptions options;
            options.format = "png"; options.extendedCodecs = false;
            const auto encoded = iiSharedCanvas::encodeBitmap(rendered.pixels, options);
            if (!encoded.ok()) return {};
            properties["width"] = std::to_string(rendered.region.extent.width);
            properties["height"] = std::to_string(rendered.region.extent.height);
            properties["frame"] = "0";
            return FilePreview{"image/png", "image", std::string(encoded.bytes.begin(), encoded.bytes.end()),
                unsigned(rendered.pixels.width), unsigned(rendered.pixels.height)};
        });
        // Retire the domain callback and its worker before Qt tears down image
        // codecs. The common provider and its fallback remain independent.
        qAddPostRoutine([] { Presentation::setRenderer(".iisc", {}); (void)Presentation::waitForIdle(std::chrono::seconds(30)); });
    });
}
}
