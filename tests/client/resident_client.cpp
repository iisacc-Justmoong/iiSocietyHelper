#include "../../src/Client/ResidentClient.h"
#include "../../src/Client/LocalFrame.h"
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace iiSocietyHelper;
class ResidentClientTests final : public QObject {
    Q_OBJECT
private slots:
    void routesOnlyOwnedEventsAndCancels() {
        QTemporaryDir dir(QDir::current().filePath("resident-client-XXXXXX"));
        QVERIFY(dir.isValid());
        QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(ResidentClient::serverAddress(dir.path())));
        ResidentClient client(dir.path());
        QSignalSpy connection(&client, &ResidentClient::connectionChanged);
        QSignalSpy state(&client, &ResidentClient::residentChanged);
        QSignalSpy events(&client, &ResidentClient::generationEvent);
        QSignalSpy failures(&client, &ResidentClient::requestFailed);
        client.connectToDaemon();
        QTRY_VERIFY(server.hasPendingConnections());
        std::unique_ptr<QLocalSocket> peer(server.nextPendingConnection());
        QTRY_VERIFY(!connection.isEmpty());
        LocalFrame frames;
        auto read = [&]() {
            QTRY_VERIFY(peer->bytesAvailable() > 0);
            QVERIFY(frames.append(peer->readAll()));
        };
        read(); const auto subscribe=frames.take(); QVERIFY(subscribe.has_value());
        QCOMPARE(subscribe->value("op").toString(), "iisaccsonSubscribe");
        peer->write(LocalFrame::encode({{"type","result"},{"id",subscribe->value("id")},{"accepted",true},
                                       {"resident",QJsonObject{{"state","ready"}}}}));
        QTRY_COMPARE(state.size(), 1);
        const auto id=client.generate({QJsonObject{{"role","user"},{"content","hello"}}}, {});
        QVERIFY(!id.isEmpty()); read(); const auto request=frames.take(); QVERIFY(request.has_value());
        QCOMPARE(request->value("id").toString(),id);
        peer->write(LocalFrame::encode({{"type","result"},{"id",id},{"accepted",true}}));
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id","other"},{"event",QJsonObject{{"kind","delta"},{"text","private"}}}}));
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id",id},{"event",QJsonObject{{"kind","delta"},{"text","answer"}}}}));
        QTRY_COMPARE(events.size(),1);
        QCOMPARE(events.at(0).at(0).toString(),id);
        QCOMPARE(events.at(0).at(1).toJsonObject().value("text").toString(),"answer");
        QVERIFY(client.cancel(id)); read(); const auto cancel=frames.take(); QVERIFY(cancel.has_value());
        QCOMPARE(cancel->value("requestId").toString(),id);
        peer->write(LocalFrame::encode({{"type","iisaccsonEvent"},{"id",id},{"event",QJsonObject{{"kind","finished"}}}}));
        QTRY_COMPARE(events.size(),2);
        QVERIFY(!client.cancel(id));
        const auto interrupted=client.generate({QJsonObject{{"role","user"},{"content","pending"}}}, {});
        QVERIFY(!interrupted.isEmpty()); read(); QVERIFY(frames.take().has_value());
        peer->abort();
        QTRY_COMPARE(failures.size(),1);
        QCOMPARE(failures.first().at(0).toString(),interrupted);
        QVERIFY(!client.connected());
    }
};
QTEST_GUILESS_MAIN(ResidentClientTests)
#include "resident_client.moc"
