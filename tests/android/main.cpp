#include <iiSocietyHelper.h>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTimer::singleShot(0, &app, [&] {
        iiSocietyHelper::FileSystem storage;
        QString error;
        const auto require = [&](bool value, const QString &message) { if (!value && error.isEmpty()) error = message; };
        require(storage.isAvailable(), storage.errorString());
        require(storage.sections().size() == 8, "The peer must see all eight internal areas");
        require(storage.rootPath().startsWith("content://com.iisacc.society.internal/"), "The peer must use Society's URI");
        for (const auto &entry : storage.sections()) {
            const auto key = entry.toMap().value("key").toString();
            const auto directory = storage.ensureDirectory(key, "Android Helper Test");
            require(!directory.isEmpty(), storage.errorString());
            const auto url = storage.url(key, "Android Helper Test/한글.bin");
            require(url.scheme() == "content", "The peer must receive a content URI");
            QFile file(url.toString());
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), file.errorString());
            require(file.write("from-android-peer") == 17, file.errorString()); file.close();
            require(file.open(QIODevice::ReadOnly), file.errorString());
            require(file.readAll() == "from-android-peer", "Peer read did not return Society data"); file.close();
            const auto rows = storage.entries(key, "Android Helper Test");
            require(rows.size() == 1 && rows.first().toMap().value("name") == QStringLiteral("한글.bin"), "Peer directory listing failed");
        }
        require(storage.path("files", "../Models/escape").isEmpty(), "Peer traversal escaped its area");
        require(storage.refresh(), storage.errorString());
        qInfo().noquote() << "SOCIETY_ANDROID_PEER" << QJsonDocument(QJsonObject{{"ok", error.isEmpty()},
            {"identifier", storage.containerId()}, {"error", error}}).toJson(QJsonDocument::Compact);
        app.exit(error.isEmpty() ? 0 : 1);
    });
    return app.exec();
}
