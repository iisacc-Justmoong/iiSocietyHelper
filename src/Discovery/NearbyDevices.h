#pragma once
#include <iiSocietyHelper/Export.h>
#include <iiSocietyHelper/DiscoveryService.h>
#include <QDateTime>
#include <QHash>
#include <QTimer>
#include <QUdpSocket>
#include <QVariantList>
#include <QJsonArray>
#include <optional>

class IISOCIETYHELPER_EXPORT NearbyDevices final : public QObject {
    Q_OBJECT
public:
    explicit NearbyDevices(QObject *parent = nullptr);
    // Injected service and explicit loopback bind isolate discovery tests from
    // account login, the user's session store and the physical LAN.
    NearbyDevices(DiscoveryService *service, QHostAddress bindAddress, QObject *parent = nullptr);
    ~NearbyDevices() override;
    static QString accountScope(const QString &origin, const QString &userId);
    void setIdentity(const QString &scope, const QString &id, const QString &name,
                     const QString &kind, bool desktop);
    void setCredentials(const QJsonObject &credentials, bool hostAvailable = true, const QString &primaryHost = {});
    QString primaryHost() const { return m_record.value("primary").toString(); }
    bool authenticated() const;
    bool desktop() const { return m_record.value("host") == "1"; }
    bool automaticHostAvailable() const { return desktop() && m_record.value("autoHost") == "1"; }
    void clear();
    bool active() const { return m_socket.state() == QAbstractSocket::BoundState; }
    QVariantList devices() const;
    QString status() const;
    QString deviceId() const { return m_record.value("id").toString(); }
    QString deviceName() const { return m_record.value("name").toString(); }
    QString incomingName() const { return m_incoming.value("name").toString(); }
    QString outgoingName() const { return m_outgoing.value("name").toString(); }
    bool hasIncoming() const { return !m_incoming.isEmpty(); }
    bool incomingAutomatic() const { return m_incoming.value("automatic") == "1"; }
    QString incomingId() const { return m_incoming.value("from").toString(); }
    QString remoteVerificationCode() const { return m_remoteVerificationCode; }
    bool invite(const QString &deviceId, const QString &link, bool automatic = false);
    void proveConnection(const QString &code);
    QString accept();
    void decline();
    void cancel();
    void complete();
    QJsonArray connectionHints(const QString &id) const;
    void resumeHost(const QString &host, const QJsonArray &routes);
signals:
    void identityEnding();
    void authenticationEnding();
    void changed();
    void invitationReceived();
    void invitationEnded(QString message);
private:
    struct Endpoint { QString service; QJsonObject record; QHostAddress address; quint16 port; qint64 seen; };
    bool localAddress(const QHostAddress &address) const;
    void found(const QString &service, const QJsonObject &record, const QHostAddress &address, quint16 port, bool freshProof = false);
    void receive();
    void tick();
    void send(const QJsonObject &packet, const Endpoint &endpoint);
    QJsonObject sign(QJsonObject packet) const;
    bool verify(QJsonObject packet) const;
    QJsonObject reply(const QJsonObject &invitation, const QString &operation) const;
    void advertiseAuthentication();
    void sendResume();
    bool receiveResume(const QJsonObject &packet, const QHostAddress &address, quint16 port);
    std::optional<Endpoint> endpoint(const QString &id, const QHostAddress &address = {}, quint16 port = 0) const;
    DiscoveryService *m_service;
    QHostAddress m_bindAddress;
    QUdpSocket m_socket;
    QTimer m_timer;
    QJsonObject m_record, m_incoming, m_outgoing, m_accepted, m_credentials;
    QHash<QString, Endpoint> m_endpoints;
    QHash<QString, qint64> m_seenRequests;
    QHash<QString, qint64> m_retiredInstances;
    QString m_error;
    QString m_remoteVerificationCode;
    qint64 m_nextSend = 0, m_retryAt = 0;
    QString m_resumeHost, m_resumeChallenge;
    QJsonArray m_resumeRoutes;
    qint64 m_resumeAt = 0, m_resumeDeadline = 0;
};
