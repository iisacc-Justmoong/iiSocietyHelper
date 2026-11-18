#include "../CTestSupport.h"
#include "../QtEventWait.h"
#include "../SignalRecorder.h"
#include <QCoreApplication>
#include "../../src/Client/ResidentClient.h"
#include "../../src/Client/LocalFrame.h"
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

using namespace iiSocietyHelper;
class ResidentClientTests final : public QObject {
public:
    void routesOnlyOwnedEventsAndCancels() {
        QTemporaryDir dir(QDir::current().filePath("resident-client-XXXXXX"));
        CTEST_CHECK(dir.isValid());
        QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
        CTEST_CHECK(server.listen(ResidentClient::serverAddress(dir.path())));
        ResidentClient client(dir.path());
        helper_ctest::SignalRecorder connection(&client, &ResidentClient::connectionChanged);
        helper_ctest::SignalRecorder state(&client, &ResidentClient::residentChanged);
        helper_ctest::SignalRecorder events(&client, &ResidentClient::generationEvent);
        helper_ctest::SignalRecorder failures(&client, &ResidentClient::requestFailed);
        client.connectToDaemon();
        CTEST_WAIT_DEFAULT(server.hasPendingConnections());
        std::unique_ptr<QLocalSocket> peer(server.nextPendingConnection());
        CTEST_WAIT_DEFAULT(!connection.isEmpty());
        LocalFrame frames;
        auto read = [&]() {
            CTEST_WAIT_DEFAULT(peer->bytesAvailable() > 0);
            CTEST_CHECK(frames.append(peer->readAll()));
        };
        read(); const auto subscribe=frames.take(); CTEST_CHECK(subscribe.has_value());
        CTEST_COMPARE(subscribe->value("op").toString(), "iisaccsonSubscribe");
        peer->write(LocalFrame::encode({{"type","result"},{"id",subscribe->value("id")},{"accepted",true},
                                       {"resident",QJsonObject{{"state","ready"}}}}));
        CTEST_WAIT_COMPARE(state.size(), 1);
        const auto id=client.generate({QJsonObject{{"role","user"},{"content","hello"}}}, {});
        CTEST_CHECK(!id.isEmpty()); read(); const auto request=frames.take(); CTEST_CHECK(request.has_value());
        CTEST_COMPARE(request->value("id").toString(),id);
        peer->write(LocalFrame::encode({{"type","result"},{"id",id},{"accepted",true}}));
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id","other"},{"event",QJsonObject{{"kind","delta"},{"text","private"}}}}));
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id",id},{"event",QJsonObject{{"kind","delta"},{"text","answer"}}}}));
        CTEST_WAIT_COMPARE(events.size(),1);
        CTEST_COMPARE(events.at(0).at(0).toString(),id);
        CTEST_COMPARE(events.at(0).at(1).toJsonObject().value("text").toString(),"answer");
        CTEST_CHECK(client.cancel(id)); read(); const auto cancel=frames.take(); CTEST_CHECK(cancel.has_value());
        CTEST_COMPARE(cancel->value("requestId").toString(),id);
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id",id},{"event",QJsonObject{{"kind","finished"}}}}));
        CTEST_WAIT_COMPARE(events.size(),2);
        CTEST_CHECK(!client.cancel(id));
        const auto interrupted=client.generate({QJsonObject{{"role","user"},{"content","pending"}}}, {});
        CTEST_CHECK(!interrupted.isEmpty()); read(); CTEST_CHECK(frames.take().has_value());
        peer->abort();
        CTEST_WAIT_COMPARE(failures.size(),1);
        CTEST_COMPARE(failures.first().at(0).toString(),interrupted);
        CTEST_CHECK(!client.connected());
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    ResidentClientTests tests;
    runner.run("routesOnlyOwnedEventsAndCancels", [&] { tests.routesOnlyOwnedEventsAndCancels(); });
    return runner.result();
}
