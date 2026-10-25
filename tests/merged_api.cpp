#include <iiSocietyHelper.h>
#include <iiSocietyHelper/AccountSession.h>
#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper/DiscoveryService.h>

#include <QCoreApplication>
#include <QTemporaryDir>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    iiSocietyHelper::Sync::Replica replica;
    iiSocietyHelper::Generation::Remote remote([](const QJsonObject &) { return QString(); });
    if (remote.active() || iiSocietyHelper::Sync::Replica::ChunkBytes < 1) return 10;
    QTemporaryDir fixture(QStringLiteral(HELPER_TEST_DIRECTORY "/merged-api-XXXXXX"));
    if (!fixture.isValid()) return 1;
    // Neither account restoration nor default storage selection touches live data.
    qputenv("SOCIETY_CONTAINER_PATH", fixture.filePath("missing.societycontainer").toUtf8());
    AccountSession account(QUrl(QStringLiteral("http://127.0.0.1:1")));
    iiSocietyHelper::Helper helper;
    iiSocietyHelper::Client client(&account, nullptr);
    const auto syncStatus = client.synchronizationStatus().query().send({});
    if (!syncStatus || *syncStatus != 0) return 11;
    if (!helper.setAccountManager(account.manager()) || helper.accountManager() != account.manager()
        || helper.account() != account.manager()->account()) return 2;
    auto *mirror = client.container();
    if (!mirror || mirror->available() || client.connected() || !client.hostSections().isEmpty()) return 3;
    if (!helper.fileSystem() || !helper.fileSystem()->container()
        || helper.fileSystem()->container() == mirror) return 4;
    if (!helper.fileSystem()->fileInformation("files", "unavailable.txt").isEmpty()) return 9;
    if (client.property("container").value<iiSocietyContainer::Container *>() != mirror) return 5;
    if (!client.listHostDirectory().isEmpty() || !client.statHostEntry("files/item").isEmpty()
        || !client.inspectHostFile("files/item").isEmpty()
        || !client.readHostFile("models/item", 0).isEmpty() || !client.writeHostFile("files/item", "a").isEmpty()
        || !client.createHostDirectory("files/new").isEmpty()) return 6;
    client.refreshContainer();
    client.stop();
    helper.stop();
    if (helper.accountManager() != account.manager()) return 7;
    return client.container() == mirror && !mirror->available() ? 0 : 8;
}
