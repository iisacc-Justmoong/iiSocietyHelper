#pragma once
#include "PairingCredentialsFixture.h"
#include <QCryptographicHash>
#include <QFile>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>

class AccountServer : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    QList<QByteArray> headers;
    int status = 200;
    int pairingVersion = 2;
    bool hang = false;
    QJsonObject profile;
    AccountServer() {
        QFile fixture(SOCIETY_ACCOUNT_FIXTURE);
        if (fixture.open(QIODevice::ReadOnly)) profile = QJsonDocument::fromJson(fixture.readAll()).object().value("account").toObject();
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) return;
                    const auto bytes = socket->property("bytes").toByteArray() + socket->readAll();
                    socket->setProperty("bytes", bytes);
                    const auto offset = bytes.indexOf("\r\n\r\n");
                    if (offset < 0) return;
                    int length = 0;
                    for (const auto &line : bytes.left(offset).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < offset + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto wire = QJsonDocument::fromJson(bytes.mid(offset + 4, length)).object();
                    if (!bytes.startsWith("POST /Account/GraphQL ") || !wire.value("query").toString().contains("appSession(input: $input)")) {
                        socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost(); return;
                    }
                    const auto input = wire.value("variables").toObject().value("input").toObject();
                    requests.append(input); headers.append(bytes.left(offset));
                    if (hang) return;
                    const auto intent = input.value("intent").toString();
                    QJsonObject payload{{"account", QJsonValue::Null}, {"session", QJsonValue::Null}};
                    QByteArray cookies;
                    if (status != 200) payload.insert("error", QJsonObject{{"code", status == 401 ? "invalid_credentials" : "unavailable"}});
                    else if (intent != "logout") {
                        payload.insert("account", profile);
                        payload.insert("session", QJsonObject{{"id", QString(32, 'a')}, {"client", "app"}, {"current", true},
                            {"device", input.value("device")}, {"createdAt", "2026-09-09T00:00:00Z"},
                            {"lastSeenAt", "2026-09-09T00:00:00Z"}, {"expiresAt", "2100-01-01T00:00:00Z"}});
                        cookies = "Set-Cookie: iisacc_auth_id=fixture-id; Path=/; HttpOnly\r\n"
                                  "Set-Cookie: iisacc_auth_refresh=fixture-refresh; Path=/; HttpOnly; Max-Age=3600\r\n"
                                  "Set-Cookie: iisacc_login_session=fixture-session; Path=/; HttpOnly; Max-Age=3600\r\n";
                        if (intent == "container") {
                            if (input.contains("containerDrive")) {
                                auto location = input.value("containerDrive").toObject();
                                location.remove("expectedRevision");
                                location.insert("hostDeviceId", input.value("device").toObject().value("id"));
                                location.insert("revision", QUuid::createUuid().toString(QUuid::WithoutBraces));
                                profile.insert("societyContainerDrive", location);
                            }
                            payload = {{"societyContainerDrive", profile.value("societyContainerDrive").isUndefined()
                                ? QJsonValue(QJsonValue::Null) : profile.value("societyContainerDrive")}};
                            cookies.clear();
                        }
                        if (intent == "pairing") {
                            auto proof = pairingVersion == 2 ? localPairingCredentialsFixture(scope())
                                : pairingCredentialsFixture(scope());
                            if (pairingVersion == 1)
                                proof.insert("refreshAt", QDateTime::currentDateTimeUtc().addSecs(200).toString(Qt::ISODate));
                            proof.insert("deviceId", input.value("device").toObject().value("id"));
                            proof.insert("sessionId", QString(32, 'a'));
                            payload = {{"pairing", proof}}; cookies.clear();
                        }
                    }
                    const auto envelope = status == 200 ? QJsonObject{{"data", QJsonObject{{"appSession", payload}}}}
                        : QJsonObject{{"errors", QJsonArray{QJsonObject{{"message", "Rejected"}, {"extensions", QJsonObject{{"status", status}, {"result", payload}}}}}}};
                    const auto body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(200) + " Response\r\nContent-Type: application/json\r\n"
                        "Connection: close\r\n" + cookies + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
    QString scope() const {
        return QString::fromLatin1(QCryptographicHash::hash("society-auto-pair-v1\n" + url().toEncoded() + '\n'
            + profile.value("sub").toString().toUtf8(), QCryptographicHash::Sha256).toHex());
    }
};
