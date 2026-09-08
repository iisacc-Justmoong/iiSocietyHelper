#include <iiSocietyHelper.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Independent File System Client");
    iiSocietyHelper::Helper helper;
    auto *storage = helper.fileSystem();
    if (!storage->isAvailable() || app.arguments().size() != 2) return 1;
    const bool writing = app.arguments().at(1) == "write";
    for (const auto &value : storage->sections()) {
        const auto key = value.toMap().value("key").toString();
        if (writing && storage->ensureDirectory(key, "Helper IO/한글").isEmpty()) return 2;
        const auto path = storage->path(key, "Helper IO/한글/shared #%.bin");
        if (path.isEmpty()) return 3;
        const auto nativePath = QFile(path).filesystemFileName();
        if (writing) {
            std::ofstream file(nativePath, std::ios::binary | std::ios::trunc);
            file << "from-peer:" << key.toStdString();
            file.close();
            if (!file) return 4;
        } else {
            std::ifstream file(nativePath, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            if (!file || bytes != "from-app:" + key.toStdString()) return 5;
            if (!std::filesystem::is_regular_file(nativePath)) return 6;
        }
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"rootPath", storage->rootPath()},
        {"containerId", storage->containerId()}, {"sectionCount", storage->sections().size()}})
        .toJson(QJsonDocument::Compact) << Qt::endl;
    return 0;
}
