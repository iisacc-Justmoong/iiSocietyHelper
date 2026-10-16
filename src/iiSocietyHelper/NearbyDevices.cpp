#include "NearbyDevices.h"
#include "PairingCredentials.h"
#include <iiServerHost.h>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QDebug>
#include <algorithm>

namespace {
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
void trace(const char *message) {
    if (qEnvironmentVariableIntValue("SOCIETY_DISCOVERY_TRACE") == 1) qInfo("Society pairing: %s", message);
}
bool token(const QString &s) { static const QRegularExpression re("\\A[A-Za-z0-9_.:-]{1,128}\\z"); return re.match(s).hasMatch(); }
bool hash(const QString &s) { static const QRegularExpression re("\\A[a-f0-9]{64}\\z"); return re.match(s).hasMatch(); }
QString nonce() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validRecord(const QJsonObject &r) {
    const auto name = r.value("name").toString();
    static const QRegularExpression controls("[\\x00-\\x1f\\x7f]");
    return r.value("v") == "1" && hash(r.value("scope").toString()) && token(r.value("id").toString())
        && token(r.value("nonce").toString()) && !name.isEmpty() && name.size() <= 128 && !name.contains(controls)
        && QStringList{"pc", "phone", "tablet"}.contains(r.value("kind").toString())
        && QStringList{"0", "1"}.contains(r.value("host").toString())
        && (!r.contains("primary") || r.value("primary").toString().isEmpty() || token(r.value("primary").toString()))
        && QJsonDocument(r).toJson(QJsonDocument::Compact).size() <= 1024;
}
}
NearbyDevices::NearbyDevices(QObject *parent) : NearbyDevices(nullptr, QHostAddress::AnyIPv4, parent) {}
NearbyDevices::NearbyDevices(DiscoveryService *service, QHostAddress bindAddress, QObject *parent)
    : QObject(parent), m_service(service ? service : DiscoveryService::create(this)), m_bindAddress(bindAddress) {
    connect(m_service, &DiscoveryService::found, this, [this](const QString &service, const QJsonObject &record,
        const QHostAddress &address, quint16 port) { found(service, record, address, port); });
    connect(m_service, &DiscoveryService::lost, this, [this](const QString &key) {
        const auto removed = m_endpoints.removeIf([&](const auto &item) { return item.value().service == key; });
        if (removed) emit changed();
    });
    connect(m_service, &DiscoveryService::failed, this, [this](const QString &error) {
        if (!active()) return;
        m_error = error; if (!m_retryAt) m_retryAt = now() + 5000; emit changed();
    });
    connect(&m_socket, &QUdpSocket::readyRead, this, &NearbyDevices::receive);
    m_timer.setInterval(1000); connect(&m_timer, &QTimer::timeout, this, &NearbyDevices::tick);
}
NearbyDevices::~NearbyDevices() { clear(); }
QString NearbyDevices::accountScope(const QString &origin, const QString &userId) {
    if (origin.isEmpty() || userId.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash("society-nearby-v1\n" + origin.toUtf8() + '\n' + userId.toUtf8(), QCryptographicHash::Sha256).toHex());
}
void NearbyDevices::setIdentity(const QString &scope, const QString &id, const QString &name, const QString &kind, bool desktop) {
    if (active() && m_record.value("scope") == scope && deviceId() == id && deviceName() == name
        && m_record.value("kind") == kind && m_record.value("host") == (desktop ? "1" : "0")) return;
    clear();
    QJsonObject record{{"v", "1"}, {"scope", scope}, {"id", id}, {"name", name.left(128)},
                       {"kind", kind}, {"host", desktop ? "1" : "0"}, {"nonce", nonce()}};
    if (!validRecord(record)) return;
    m_record = record;
    if (!m_socket.bind(m_bindAddress, 0)) { m_error = tr("Local device discovery could not start: %1").arg(m_socket.errorString()); emit changed(); return; }
    m_timer.start(); m_service->start(m_record, m_socket.localPort()); emit changed();
}
void NearbyDevices::clear() {
    if (active() || !m_record.isEmpty()) emit identityEnding();
    cancel(); m_timer.stop(); m_service->stop(); m_socket.close(); m_record = {};
    m_endpoints.clear(); m_seenRequests.clear(); m_retiredInstances.clear(); m_error.clear(); m_retryAt = 0;
    m_resumeHost.clear(); m_resumeChallenge.clear(); m_resumeRoutes = {}; m_resumeAt = 0; m_resumeDeadline = 0;
    m_credentials = {}; emit changed();
}
bool NearbyDevices::authenticated() const {
    return active() && m_credentials.value("scope") == m_record.value("scope")
        && SocietyPairingCredentials::key(m_credentials, now() / 300000).size() == 32;
}
QJsonObject NearbyDevices::sign(QJsonObject packet) const {
    packet.remove("proof");
    if (!authenticated()) return packet;
    const auto epoch = QString::number(now() / 300000); packet.insert("epoch", epoch);
    const auto key = SocietyPairingCredentials::key(m_credentials, epoch.toLongLong());
    packet.insert("proof", QString::fromLatin1(QMessageAuthenticationCode::hash("society-auto-v1\n"
        + QJsonDocument(packet).toJson(QJsonDocument::Compact), key, QCryptographicHash::Sha256).toHex()));
    return packet;
}
bool NearbyDevices::verify(QJsonObject packet) const {
    if (!authenticated()) return false;
    const auto epoch = packet.value("epoch").toString(); const auto current = now() / 300000;
    if (epoch != QString::number(current) && !(now() % 300000 < 30000 && epoch == QString::number(current - 1))) return false;
    const auto key = SocietyPairingCredentials::key(m_credentials, epoch.toLongLong());
    if (key.size() != 32) return false;
    const auto proof = packet.take("proof").toString();
    if (!hash(proof)) return false;
    const auto expected = QMessageAuthenticationCode::hash("society-auto-v1\n" + QJsonDocument(packet).toJson(QJsonDocument::Compact),
        key, QCryptographicHash::Sha256).toHex(), actual = proof.toLatin1();
    unsigned char difference = 0;
    for (int i = 0; i < 64; ++i) difference |= expected[i] ^ actual[i];
    return difference == 0;
}
void NearbyDevices::setCredentials(const QJsonObject &credentials, bool hostAvailable, const QString &primaryHost) {
    const auto available = hostAvailable && desktop() ? QString("1") : QString("0");
    if (credentials == m_credentials && m_record.value("autoHost") == available && m_record.value("primary").toString() == primaryHost) return;
    const bool hadCredentials = !m_credentials.isEmpty();
    const auto previousEpoch = QString::number(now() / 300000 - 1);
    const auto previousKey = m_credentials.value("keys").toObject().value(previousEpoch);
    m_credentials = credentials;
    if (previousKey.isString() && !credentials.isEmpty() && credentials.value("version").toInt(1) == 1) {
        auto keys = m_credentials.value("keys").toObject(); keys.insert(previousEpoch, previousKey); m_credentials.insert("keys", keys);
    }
    m_record.insert("autoHost", available);
    if (primaryHost.isEmpty()) m_record.remove("primary"); else m_record.insert("primary", primaryHost);
    if (!authenticated()) {
        m_credentials = {};
        if (hadCredentials) emit authenticationEnding();
    }
    advertiseAuthentication();
}
void NearbyDevices::advertiseAuthentication() {
    if (!active()) return;
    auto record = m_record; record.remove("epoch"); record.remove("proof");
    if (authenticated()) record = sign(record);
    if (record == m_record) return;
    m_record = record; m_service->start(m_record, m_socket.localPort()); emit changed();
}
bool NearbyDevices::localAddress(const QHostAddress &address) const {
    return iiServerHost::LanLink::localAddress(address.toString()) && (!address.isLoopback() || m_bindAddress.isLoopback());
}
void NearbyDevices::found(const QString &service, const QJsonObject &record, const QHostAddress &address, quint16 port, bool freshProof) {
    if (!active() || service.size() > 512 || !validRecord(record) || !port || !localAddress(address)
        || record.value("scope") != m_record.value("scope") || record.value("id") == m_record.value("id")) return;
    // Keep every address of this DNS-SD instance, while replacing endpoints
    // belonging to an older process/port when the instance changes.
    const bool verified = verify(record);
    const auto instance = record.value("id").toString() + '/' + record.value("nonce").toString();
    if (freshProof) m_retiredInstances.remove(instance);
    else if (m_retiredInstances.value(instance) > now()) return;
    bool restarted = false;
    m_endpoints.removeIf([&](const auto &item) {
        const auto &old = item.value();
        if (verified && old.record.value("id") == record.value("id")
            && old.record.value("nonce") != record.value("nonce")) {
            if (freshProof) {
                if (m_retiredInstances.size() >= 128) m_retiredInstances.erase(m_retiredInstances.begin());
                m_retiredInstances.insert(old.record.value("id").toString() + '/' + old.record.value("nonce").toString(), now() + 70000);
            }
            restarted = true; return true;
        }
        return old.service == service && (old.port != port || old.record.value("id") != record.value("id")
            || old.record.value("nonce") != record.value("nonce"));
    });
    const auto key = service + '/' + address.toString();
    if (!m_endpoints.contains(key) && m_endpoints.size() >= 128) return;
    m_endpoints.insert(key, {service, record, address, port, now()}); m_error.clear();
    // A fresh process cannot answer an invitation bound to its old nonce.
    // Retire that attempt immediately instead of waiting for its 20s timeout.
    if (restarted && (m_outgoing.value("to") == record.value("id")
        || m_incoming.value("from") == record.value("id") || m_accepted.value("from") == record.value("id"))) {
        cancel(); emit invitationEnded(tr("The other device restarted. Reconnecting."));
    }
    emit changed();
}
std::optional<NearbyDevices::Endpoint> NearbyDevices::endpoint(const QString &id, const QHostAddress &address, quint16 port) const {
    for (const auto &e : m_endpoints) if (e.record.value("id") == id && now() - e.seen < 25000
        && (address.isNull() || e.address == address) && (!port || e.port == port)) return e;
    return {};
}
QVariantList NearbyDevices::devices() const {
    QVariantList result; QSet<QString> ids;
    for (const auto &e : m_endpoints) {
        const auto id = e.record.value("id").toString(); if (ids.contains(id) || now() - e.seen >= 25000) continue; ids.insert(id);
        result.append(QVariantMap{{"id", id}, {"name", e.record.value("name").toString()}, {"kind", e.record.value("kind").toString()},
                                 {"address", e.address.toString()}, {"desktop", e.record.value("host") == "1"},
                                 {"verified", verify(e.record)}, {"autoHost", e.record.value("autoHost") == "1"},
                                 {"primary", e.record.value("primary").toString()}});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0; });
    return result;
}
QString NearbyDevices::status() const {
    if (!m_error.isEmpty()) return m_error;
    if (!active()) return tr("Sign in to iisacc on both devices to discover nearby devices.");
    return devices().isEmpty() ? tr("Searching your local network… Keep Society open on your other device.")
                               : tr("Nearby devices on your iisacc account. The list updates automatically.");
}
void NearbyDevices::send(const QJsonObject &packet, const Endpoint &e) {
    const auto bytes = QJsonDocument(packet).toJson(QJsonDocument::Compact);
    if (bytes.size() <= 4096) {
        const auto sent = m_socket.writeDatagram(bytes, e.address, e.port);
        trace(sent == bytes.size() ? "datagram sent" : "datagram send failed");
    }
}
QJsonArray NearbyDevices::connectionHints(const QString &id) const {
    QJsonArray result;
    for (const auto &e : m_endpoints) {
        if (e.record.value("id") != id || now() - e.seen >= 25000 || !verify(e.record)) continue;
        const QJsonObject hint{{"address", e.address.toString()}, {"port", e.port}};
        if (!result.contains(hint) && result.size() < 8) result.append(hint);
    }
    return result;
}
void NearbyDevices::resumeHost(const QString &host, const QJsonArray &routes) {
    if (host.isEmpty()) { m_resumeHost.clear(); m_resumeRoutes = {}; m_resumeChallenge.clear(); return; }
    if (!authenticated() || desktop() || host != primaryHost() || !token(host)) return;
    QJsonArray safe;
    for (const auto &value : routes) {
        const auto route = value.toObject(); const QHostAddress address(route.value("address").toString());
        const int port = route.value("port").toInt();
        if (localAddress(address) && port > 0 && port <= 65535 && safe.size() < 8)
            safe.append(QJsonObject{{"address", address.toString()}, {"port", port}});
    }
    if (host == m_resumeHost && safe == m_resumeRoutes) return;
    m_resumeHost = host; m_resumeRoutes = safe; m_resumeAt = 0; sendResume();
}
void NearbyDevices::sendResume() {
    if (!authenticated() || m_resumeHost != primaryHost() || m_resumeRoutes.isEmpty() || now() < m_resumeAt) return;
    m_resumeChallenge = nonce(); m_resumeDeadline = now() + 5000; m_resumeAt = now() + 5000;
    const auto packet = sign({{"v", "1"}, {"op", "resume"}, {"scope", m_record.value("scope")},
        {"from", deviceId()}, {"to", m_resumeHost}, {"request", m_resumeChallenge},
        {"expires", QString::number(m_resumeDeadline)}, {"record", m_record}});
    for (const auto &value : m_resumeRoutes) {
        const auto route = value.toObject();
        send(packet, {{}, {}, QHostAddress(route.value("address").toString()), quint16(route.value("port").toInt()), now()});
    }
}
bool NearbyDevices::receiveResume(const QJsonObject &p, const QHostAddress &address, quint16 port) {
    const auto op = p.value("op").toString();
    if (op != "resume" && op != "resume-ready") return false;
    const auto record = p.value("record").toObject();
    const auto request = p.value("request").toString();
    const auto expires = p.value("expires").toString().toLongLong();
    if (p.value("v") != "1" || p.value("to") != deviceId() || p.value("scope") != m_record.value("scope")
        || !token(request) || expires <= now() || expires > now() + 6000 || !validRecord(record)
        || record.value("id") != p.value("from") || record.value("id") == deviceId()
        || record.value("scope") != m_record.value("scope") || !verify(p) || !verify(record)) return true;
    if (op == "resume") {
        // Reacquire fresh discovery only, never grant Files or skip TLS pairing.
        // A registered host can hear its returning client without Bonjour delay.
        if (!automaticHostAvailable() || primaryHost() != deviceId() || record.value("primary") != deviceId()
            || record.value("host") != "0" || m_seenRequests.contains(request) || m_seenRequests.size() >= 128) return true;
        m_seenRequests.insert(request, expires);
        send(sign({{"v", "1"}, {"op", "resume-ready"}, {"scope", m_record.value("scope")},
            {"from", deviceId()}, {"to", record.value("id")}, {"request", request},
            {"expires", p.value("expires")}, {"record", m_record}}), {{}, {}, address, port, now()});
    } else {
        if (desktop() || p.value("from") != m_resumeHost || request != m_resumeChallenge || now() >= m_resumeDeadline
            || record.value("autoHost") != "1" || record.value("primary") != m_resumeHost) return true;
        bool expected = false;
        for (const auto &value : m_resumeRoutes) {
            const auto route = value.toObject();
            expected |= QHostAddress(route.value("address").toString()) == address && route.value("port").toInt() == port;
        }
        if (!expected) return true;
        m_resumeChallenge.clear();
    }
    found("resume:" + record.value("id").toString(), record, address, port, true);
    return true;
}
bool NearbyDevices::invite(const QString &id, const QString &text, bool automatic) {
    const auto peer = endpoint(id); iiServerHost::LanLink link;
    if (!active() || m_record.value("host") != "1" || !peer || !iiServerHost::LanLink::decode(text, &link)
        || link.hostId != deviceId() || link.expiresAt.toMSecsSinceEpoch() <= now()
        || (automatic && !verify(peer->record))) return false;
    cancel();
    m_outgoing = {{"v", "1"}, {"op", "invite"}, {"scope", m_record.value("scope")}, {"from", deviceId()},
                  {"to", id}, {"nonce", peer->record.value("nonce")}, {"request", nonce()}, {"link", text},
                  {"name", peer->record.value("name")}, {"expires", QString::number(link.expiresAt.toMSecsSinceEpoch())}};
    if (automatic) {
        m_outgoing.insert("automatic", "1"); m_outgoing.insert("senderNonce", m_record.value("nonce"));
        m_outgoing = sign(m_outgoing);
    }
    send(m_outgoing, *peer); m_nextSend = now() + 2000; emit changed(); return true;
}
void NearbyDevices::receive() {
    int count = 0;
    while (m_socket.hasPendingDatagrams() && ++count <= 64) {
        if (m_socket.pendingDatagramSize() > 4096) { m_socket.readDatagram(nullptr, 0); continue; }
        const auto datagram = m_socket.receiveDatagram();
        if (!localAddress(datagram.senderAddress())) continue;
        const auto p = QJsonDocument::fromJson(datagram.data()).object();
        if (receiveResume(p, datagram.senderAddress(), datagram.senderPort())) continue;
        // Bonjour can discover one device on several interfaces. The received
        // source must match an observed endpoint, not an arbitrary hash entry.
        const auto peer = endpoint(p.value("from").toString(), datagram.senderAddress(), datagram.senderPort());
        if (!peer) {
            if (qEnvironmentVariableIntValue("SOCIETY_DISCOVERY_TRACE") == 1) {
                QStringList expected;
                for (const auto &candidate : m_endpoints) if (candidate.record.value("id") == p.value("from"))
                    expected.append(candidate.address.toString() + ':' + QString::number(candidate.port));
                qInfo().noquote() << "Society pairing: source" << datagram.senderAddress().toString() << datagram.senderPort()
                    << "observed routes" << expected.join(',');
            }
            continue;
        }
        if (p.value("v") != "1" || p.value("scope") != m_record.value("scope") || p.value("to") != m_record.value("id")
            || !token(p.value("request").toString())) { trace("datagram envelope rejected"); continue; }
        const auto op = p.value("op").toString();
        const bool automatic = p.value("automatic") == "1";
        if (automatic && (!verify(p) || !verify(peer->record) || p.value("nonce") != m_record.value("nonce")
            || p.value("senderNonce") != peer->record.value("nonce"))) {
            if (qEnvironmentVariableIntValue("SOCIETY_DISCOVERY_TRACE") == 1)
                qInfo() << "Society pairing: rejected" << op << "host" << desktop()
                    << "packetProof" << verify(p) << "recordProof" << verify(peer->record)
                    << "recipientInstance" << (p.value("nonce") == m_record.value("nonce"))
                    << "senderInstance" << (p.value("senderNonce") == peer->record.value("nonce"));
            continue;
        }
        if (op == "invite") {
            iiServerHost::LanLink link; const auto request = p.value("request").toString();
            if (peer->record.value("host") != "1" || p.value("nonce") != m_record.value("nonce") || m_seenRequests.contains(request)
                || !m_incoming.isEmpty() || !iiServerHost::LanLink::decode(p.value("link").toString(), &link)
                || link.hostId != peer->record.value("id") || link.expiresAt.toMSecsSinceEpoch() <= now()
                || link.expiresAt.toMSecsSinceEpoch() > now() + 65000) { trace("invitation identity, lifetime or duplicate rejected"); continue; }
            // A discovered peer cannot redirect this device to another LAN host.
            if (!link.addresses.contains(peer->address.toString())) { trace("invitation route rejected"); continue; }
            link.addresses = {peer->address.toString()};
            m_incoming = p; m_incoming.insert("link", link.encode()); m_incoming.insert("name", peer->record.value("name"));
            m_incoming.insert("expires", QString::number(link.expiresAt.toMSecsSinceEpoch()));
            m_seenRequests.insert(request, now() + 70000); emit changed(); emit invitationReceived();
            trace("invitation received");
        } else if ((op == "decline" || op == "accept" || op == "verify") && !m_outgoing.isEmpty() && p.value("request") == m_outgoing.value("request")
                   && p.value("from") == m_outgoing.value("to") && automatic == (m_outgoing.value("automatic") == "1")) {
            if (op == "verify") {
                static const QRegularExpression code("\\A[A-F0-9]{4}-[A-F0-9]{4}-[A-F0-9]{4}\\z");
                if (automatic && code.match(p.value("code").toString()).hasMatch()) {
                    m_remoteVerificationCode = p.value("code").toString(); emit changed();
                }
                continue;
            }
            if (op == "decline") { m_outgoing = {}; emit changed(); emit invitationEnded(tr("The other device declined the request.")); }
            else m_nextSend = now() + 65000;
        } else if (op == "cancel" && !m_incoming.isEmpty() && p.value("request") == m_incoming.value("request")
                   && p.value("from") == m_incoming.value("from") && automatic == incomingAutomatic()) { m_incoming = {}; emit changed(); }
    }
    if (m_socket.hasPendingDatagrams()) QTimer::singleShot(0, this, &NearbyDevices::receive);
}
QString NearbyDevices::accept() {
    if (m_incoming.isEmpty() || m_incoming.value("expires").toString().toLongLong() <= now()) { decline(); return {}; }
    const auto peer = endpoint(m_incoming.value("from").toString());
    if (!peer) { decline(); return {}; }
    if (incomingAutomatic() && (!authenticated() || !verify(peer->record))) { decline(); return {}; }
    send(reply(m_incoming, "accept"), *peer);
    m_accepted = incomingAutomatic() ? m_incoming : QJsonObject{};
    const auto link = m_incoming.value("link").toString(); m_incoming = {}; emit changed(); return link;
}
void NearbyDevices::decline() {
    if (const auto peer = endpoint(m_incoming.value("from").toString()))
        send(reply(m_incoming, "decline"), *peer);
    m_incoming = {}; emit changed();
}
void NearbyDevices::cancel() {
    decline();
    if (const auto peer = endpoint(m_outgoing.value("to").toString()))
    {
        auto packet = m_outgoing; packet.insert("op", "cancel");
        send(packet.value("automatic") == "1" ? sign(packet) : packet, *peer);
    }
    m_outgoing = {}; m_accepted = {}; m_remoteVerificationCode.clear(); emit changed();
}
void NearbyDevices::complete() { m_outgoing = {}; m_accepted = {}; m_remoteVerificationCode.clear(); emit changed(); }
QJsonObject NearbyDevices::reply(const QJsonObject &invitation, const QString &operation) const {
    QJsonObject packet{{"v", "1"}, {"op", operation}, {"scope", m_record.value("scope")}, {"from", deviceId()},
        {"to", invitation.value("from")}, {"request", invitation.value("request")}};
    if (invitation.value("automatic") == "1") {
        packet.insert("automatic", "1"); packet.insert("nonce", invitation.value("senderNonce"));
        packet.insert("senderNonce", m_record.value("nonce")); return sign(packet);
    }
    return packet;
}
void NearbyDevices::proveConnection(const QString &code) {
    if (!authenticated() || m_accepted.isEmpty() || m_accepted.value("expires").toString().toLongLong() <= now()) return;
    if (const auto peer = endpoint(m_accepted.value("from").toString())) {
        auto packet = reply(m_accepted, "verify"); packet.insert("code", code); send(sign(packet), *peer);
    }
}
void NearbyDevices::tick() {
    if (!m_credentials.isEmpty() && !authenticated()) {
        m_credentials = {}; emit authenticationEnding();
    }
    advertiseAuthentication();
    sendResume();
    bool removed = false;
    for (const auto &key : m_endpoints.keys()) if (now() - m_endpoints.value(key).seen >= 25000) { m_endpoints.remove(key); removed = true; }
    for (const auto &key : m_seenRequests.keys()) if (m_seenRequests.value(key) <= now()) m_seenRequests.remove(key);
    for (const auto &key : m_retiredInstances.keys()) if (m_retiredInstances.value(key) <= now()) m_retiredInstances.remove(key);
    if (m_seenRequests.size() > 128) m_seenRequests.clear();
    if (m_retryAt && now() >= m_retryAt) { m_retryAt = 0; m_service->stop(); m_service->start(m_record, m_socket.localPort()); }
    if (!m_incoming.isEmpty() && m_incoming.value("expires").toString().toLongLong() <= now()) { m_incoming = {}; removed = true; }
    if (!m_outgoing.isEmpty()) {
        if (m_outgoing.value("expires").toString().toLongLong() <= now()) { m_outgoing = {}; removed = true; emit invitationEnded(tr("The connection request expired. Select the device to try again.")); }
        else if (m_nextSend <= now()) { if (const auto peer = endpoint(m_outgoing.value("to").toString())) send(m_outgoing, *peer); m_nextSend = now() + 2000; }
    }
    if (removed) emit changed();
}
