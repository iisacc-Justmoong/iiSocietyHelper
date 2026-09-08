#include "iiSocietyHelper.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

using namespace iiSocietyHelper;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ii-society-helper");
    QCommandLineParser parser;
    parser.setApplicationDescription("Observe cooperating iisacc apps on this device. Emits JSON Lines.");
    parser.addHelpOption();
    parser.addOptions({
        {{"d", "directory"}, "Shared observation directory (default: platform location).", "path"},
        {{"a", "application-id"}, "Reverse-DNS identity of this observer.", "id", "com.iisacc.helper.observer"},
        {"name", "Display name of this observer.", "name", "Society Helper Observer"},
        {"heartbeat-ms", "Heartbeat interval.", "ms", "1000"},
        {"timeout-ms", "Peer timeout.", "ms", "5000"},
        {"filesystem", "Print the configured Society source paths and exit without starting observation."},
        {"exit-after-ms", "Exit cleanly after this duration; zero runs until terminated.", "ms", "0"}
    });
    parser.process(app);
    const auto write = [](const QJsonObject &event) {
        QTextStream(stdout) << QJsonDocument(event).toJson(QJsonDocument::Compact) << Qt::endl;
    };
    Helper helper;
    if (parser.isSet("filesystem")) {
        const auto *storage = helper.fileSystem();
        const bool available = storage->isAvailable();
        write({{"available", available}, {"rootPath", storage->rootPath()},
            {"containerId", storage->containerId()}, {"sections", QJsonArray::fromVariantList(storage->sections())},
            {"error", storage->errorString()}});
        return available ? 0 : 1;
    }
    const auto snapshot = [&] {
        return QJsonArray::fromVariantList(helper.observedApplications());
    };
    QObject::connect(&helper, &Helper::peerAppeared, &app, [&](const Peer &peer) {
        write({{"event", "appeared"}, {"peer", QJsonObject::fromVariantMap(peer.toVariantMap())}, {"peers", snapshot()}});
    });
    QObject::connect(&helper, &Helper::peerUpdated, &app, [&](const Peer &peer) {
        write({{"event", "updated"}, {"peer", QJsonObject::fromVariantMap(peer.toVariantMap())}, {"peers", snapshot()}});
    });
    QObject::connect(&helper, &Helper::peerDisappeared, &app, [&](const Peer &peer, DepartureReason reason) {
        write({{"event", "departed"}, {"peer", QJsonObject::fromVariantMap(peer.toVariantMap())},
               {"reason", reason == DepartureReason::Withdrawn ? "withdrawn" : "timed-out"}, {"peers", snapshot()}});
    });
    QObject::connect(&helper, &Helper::errorOccurred, &app, [&](const QString &error) {
        write({{"event", "error"}, {"message", error}});
    });
    if (!helper.start({parser.value("application-id"), parser.value("name"), "0.4.0"},
            {parser.value("directory"), parser.value("heartbeat-ms").toInt(), parser.value("timeout-ms").toInt()})) return 1;
    write({{"event", "ready"}, {"self", QJsonObject::fromVariantMap(helper.self().toVariantMap())},
           {"directory", helper.directory()}});
    const int duration = parser.value("exit-after-ms").toInt();
    if (duration > 0) QTimer::singleShot(duration, &app, &QCoreApplication::quit);
    return app.exec();
}
