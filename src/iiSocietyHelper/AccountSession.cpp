#include <iiAcountManager/GraphQL.h>
#include "AccountSession.h"
#include "DeviceInfo.h"
#include "GroupSessionStore.h"
#include "PairingCredentials.h"
#include <iiAcountManager/SessionStore.h>
#include <QCoreApplication>
#include <QTimer>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QHostAddress>
#include <QCryptographicHash>
#include <iiServerHost.h>

using iisacc::accounts::AccountManager;


AccountSession::AccountSession(QObject *parent)
    : AccountSession(QUrl(qEnvironmentVariable("SOCIETY_ACCOUNT_URL", "https://iisacc.com")), parent) {
#ifndef SOCIETY_DISABLE_SESSION_RESTORE
    if (qEnvironmentVariableIntValue("SOCIETY_DISABLE_SESSION_RESTORE") == 1) return;
    initializePersistence(GroupSessionStore::create(this));
#endif
}

AccountSession::AccountSession(const QUrl &serviceUrl, QObject *parent)
    : QObject(parent), m_manager(serviceUrl, &m_network) {
    m_manager.setDeviceInfo(societyAccountDevice(m_manager.deviceInfo()));
    m_manager.setContainerDriveSyncEnabled(true);
    m_pairingExpiry.setSingleShot(true);
    connect(&m_pairingExpiry, &QTimer::timeout, this, [this] {
        if (validPairingCredentials(m_pairingCredentials)) { schedulePairingExpiry(); return; }
        m_pairingCredentials = {}; m_pairingRequestBlocked = false;
        persistPairingState(); emit pairingCredentialsChanged(); emit pairingCredentialsRequired();
    });
    connect(m_manager.account(), &iisacc::accounts::Account::changed, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::loadingChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::errorChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::authenticatedChanged, this, [this] {
        if (signedIn()) m_pairingSessionEnding = false;
        else {
            m_serverConfiguration = {};
            m_hostConnection = {};
            m_verifiedThisRun = false; clearPairingCredentials();
            ++m_pairingRevision; m_pairingLoadedSession.clear(); m_pairingLoading = false; m_rememberedPeers = {};
        }
        emit changed();
    });
    connect(&m_manager, &AccountManager::loginSessionChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::loginChallengeChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::sessionEnding, this, &AccountSession::discardPairingState);
    connect(&m_manager, &AccountManager::sessionEnding, this, &AccountSession::sessionEnding);
    connect(&m_manager, &AccountManager::restoringSessionChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::sessionStorageErrorChanged, this, &AccountSession::changed);
    connect(&m_manager, &AccountManager::sessionVerified, this, [this] {
        m_verifiedThisRun = true; m_pairingRequestBlocked = false;
        persistPairingState();
        emit pairingCredentialsRequired();
    });
    connect(&m_manager, &AccountManager::loaded, this, [this] {
        if (signedIn()) emit pairingCredentialsRequired();
    });
    connect(this, &AccountSession::changed, this, &AccountSession::rememberedDevicesChanged);
    connect(this, &AccountSession::changed, this, [this] {
        if (!signedIn() || m_pairingSessionEnding) return;
        if (!m_pairingCredentials.isEmpty() && !m_manager.acceptsSessionCredential(m_pairingCredentials.toVariantMap()))
            clearPairingCredentials();
        restorePairingState();
    });
}

AccountSession::AccountSession(const QUrl &serviceUrl, iisacc::accounts::SessionStore *store, QObject *parent)
    : AccountSession(serviceUrl, parent) { initializePersistence(store); }

void AccountSession::initializePersistence(iisacc::accounts::SessionStore *store) {
    if (!store || !m_manager.setSessionStore(store)) return;
    m_stateStore = store;
    QTimer::singleShot(0, this, &AccountSession::restoreCachedAccount);
}
void AccountSession::restoreCachedAccount() {
    if (!m_stateStore || busy() || signedIn()) return;
    const auto revision = m_pairingRevision;
    const QPointer<AccountSession> guard(this);
    // Only the SDK interprets this legacy account record. New account snapshots
    // are owned by the SDK in a separate record of the same encrypted group store.
    m_stateStore->read(pairingStateKey(), [guard, revision](iisacc::accounts::SessionStore::Result result) {
        if (!guard || revision != guard->m_pairingRevision || guard->busy() || guard->signedIn()) return;
        if (result.error == iisacc::accounts::SessionStore::Error::None
            && guard->m_manager.restoreSessionFromCache(result.data)) return;
        guard->m_manager.restoreSession();
    });
}
QString AccountSession::pairingStateKey() const {
    const auto origin = m_manager.serviceUrl().adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toEncoded();
    return "pairing-v1-" + QString::fromLatin1(QCryptographicHash::hash(origin + '\n'
        + m_manager.deviceInfo().value("id").toString().toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool AccountSession::validPairingCredentials(const QJsonObject &proof) const {
    const auto expiry = QDateTime::fromString(proof.value("expiresAt").toString(), Qt::ISODate);
    const auto refresh = QDateTime::fromString(proof.value("refreshAt").toString(), Qt::ISODate);
    return SocietyPairingCredentials::valid(proof, QDateTime::currentDateTimeUtc())
        && m_manager.acceptsSessionCredential(proof.toVariantMap()) && refresh.isValid() && refresh <= expiry;
}
void AccountSession::schedulePairingExpiry() {
    m_pairingExpiry.stop();
    if (!validPairingCredentials(m_pairingCredentials)) return;
    const auto expiry = QDateTime::fromString(m_pairingCredentials.value("expiresAt").toString(), Qt::ISODate);
    m_pairingExpiry.start(int(qBound(qint64(1), QDateTime::currentDateTimeUtc().msecsTo(expiry), qint64(2147483647))));
}
void AccountSession::restorePairingState() {
    if (!m_stateStore || !signedIn() || m_pairingSessionEnding || m_pairingRestoreRetryAt > QDateTime::currentDateTimeUtc()) return;
    const auto session = m_manager.loginSession().value("id").toString();
    if (m_pairingLoadedSession == session) return;
    m_pairingLoadedSession = session; m_pairingLoading = true;
    const auto revision = ++m_pairingRevision;
    const QPointer<AccountSession> guard(this);
    m_stateStore->read(pairingStateKey(), [guard, session, revision](iisacc::accounts::SessionStore::Result result) {
        if (!guard || revision != guard->m_pairingRevision || !guard->signedIn()
            || guard->m_manager.loginSession().value("id").toString() != session) return;
        auto *self = guard.data();
        if (result.error == iisacc::accounts::SessionStore::Error::Unavailable) {
            self->m_pairingLoadedSession.clear(); self->m_pairingLoading = true;
            self->m_pairingRestoreRetryAt = QDateTime::currentDateTimeUtc().addSecs(30);
            self->m_pairingStorageError = tr("Saved pairing information could not be read. Unlock this device and try again.");
            QTimer::singleShot(30050, self, &AccountSession::restorePairingState);
            emit self->changed(); return;
        }
        self->m_pairingLoading = false; self->m_pairingRestoreRetryAt = {};
        const auto record = QJsonDocument::fromJson(result.data).object();
        const auto proof = record.value("credentials").toObject();
        auto binding = record.value("binding").toObject().toVariantMap();
        if (record.value("schemaVersion").toInt() < 3) {
            for (const auto* field : {"origin", "subject", "userId", "deviceId"}) binding.insert(field, record.value(field).toVariant());
            binding.insert("sessionId", proof.value("sessionId").toVariant());
        }
        const bool valid = result.error == iisacc::accounts::SessionStore::Error::None
            && record.value("schemaVersion").toInt() >= 1 && record.value("schemaVersion").toInt() <= 3
            && self->m_manager.matchesSessionBinding(binding);
        if (valid) {
            self->m_automaticPairingEnabled = record.value("automaticEnabled").toBool(true);
            self->m_serverConfiguration = record.value("server").toObject();
            self->m_hostConnection = record.value("hostConnection").toObject();
            self->m_rememberedPeers = record.value("peers").toArray();
            if (self->m_rememberedPeers.size() > 128) self->m_rememberedPeers = {};
            if (self->validPairingCredentials(proof)) self->m_pairingCredentials = proof;
            self->m_pairingRequestBlocked = !self->m_verifiedThisRun
                && (record.value("requestBlocked").toBool() || record.value("pairingFailures").toInt() > 0);
            self->schedulePairingExpiry();
        }
        self->m_pairingStorageError.clear();
        if (valid && record.value("schemaVersion").toInt() < 3) self->persistPairingState();
        emit self->pairingStateRestored(); emit self->pairingCredentialsChanged(); emit self->changed();
    });
}
void AccountSession::persistPairingState() {
    if (!m_stateStore || !signedIn() || m_pairingLoading || m_pairingSessionEnding) return;
    const QJsonObject record{{"schemaVersion", 3}, {"binding", QJsonObject::fromVariantMap(m_manager.sessionBinding())},
        {"credentials", m_pairingCredentials}, {"automaticEnabled", m_automaticPairingEnabled},
        {"peers", m_rememberedPeers}, {"requestBlocked", m_pairingRequestBlocked}, {"server", m_serverConfiguration},
        {"hostConnection", m_hostConnection}};
    const auto revision = ++m_pairingRevision;
    const QPointer<AccountSession> guard(this);
    m_stateStore->write(pairingStateKey(), QJsonDocument(record).toJson(QJsonDocument::Compact), [guard, revision](iisacc::accounts::SessionStore::Result result) {
        if (!guard || revision != guard->m_pairingRevision) return;
        guard->m_pairingStorageError = result.error == iisacc::accounts::SessionStore::Error::None ? QString()
            : tr("Pairing works for this run, but its group-container state could not be saved.");
        emit guard->changed();
    });
}
void AccountSession::discardPairingState() {
    m_pairingSessionEnding = true;
    clearPairingCredentials(); ++m_pairingRevision;
    m_pairingLoadedSession.clear(); m_pairingStorageError.clear(); m_pairingRestoreRetryAt = {};
    m_pairingLoading = false; m_rememberedPeers = {}; m_automaticPairingEnabled = true;
    m_serverConfiguration = {};
    m_hostConnection = {};
    m_verifiedThisRun = false;
    if (m_stateStore) m_stateStore->remove(pairingStateKey(), [](auto) {});
}
void AccountSession::setAutomaticPairingEnabled(bool enabled) {
    if (m_automaticPairingEnabled == enabled) return;
    m_automaticPairingEnabled = enabled; persistPairingState();
}
bool AccountSession::setServerConfiguration(const QJsonObject &configuration) {
    if (!signedIn() || m_pairingLoading || m_pairingSessionEnding) return false;
    if (m_serverConfiguration == configuration) return true;
    m_serverConfiguration = configuration; persistPairingState(); return true;
}
void AccountSession::rememberPairedDevice(const QString &id, const QString &name, const QString &kind) {
    if (!signedIn() || id.isEmpty() || id.size() > 256) return;
    for (qsizetype index = m_rememberedPeers.size(); index > 0; --index)
        if (m_rememberedPeers.at(index - 1).toObject().value("id").toString() == id) m_rememberedPeers.removeAt(index - 1);
    while (m_rememberedPeers.size() >= 128) m_rememberedPeers.removeAt(0);
    m_rememberedPeers.append(QJsonObject{{"id", id}, {"name", name.left(128)}, {"kind", kind.left(32)},
        {"lastPairedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    persistPairingState();
    emit rememberedDevicesChanged();
}
QJsonObject AccountSession::savedHostConnection() const {
    const auto drive = m_manager.account()->societyContainerDrive();
    if (!signedIn() || m_hostConnection.value("scope").toString() != storageScope()
        || m_hostConnection.value("host").toString().isEmpty()
        || m_hostConnection.value("host").toString() != drive.value("hostDeviceId").toString()
        || m_hostConnection.value("container").toString().isEmpty()
        || m_hostConnection.value("container").toString() != drive.value("containerId").toString()) return {};
    return m_hostConnection;
}
bool AccountSession::rememberHostConnection(const QString &host, const QString &container, const QJsonArray &routes) {
    const auto drive = m_manager.account()->societyContainerDrive();
    if (!signedIn() || m_pairingLoading || m_pairingSessionEnding || host.isEmpty() || container.isEmpty()
        || host != drive.value("hostDeviceId").toString() || container != drive.value("containerId").toString()) return false;
    QJsonArray safe;
    for (const auto &value : routes) {
        const auto route = value.toObject();
        const auto address = route.value("address").toString(); const int port = route.value("port").toInt();
        if (iiServerHost::LanLink::localAddress(address) && port > 0 && port <= 65535 && safe.size() < 8)
            safe.append(QJsonObject{{"address", address}, {"port", port}});
    }
    if (safe.isEmpty()) safe = savedHostConnection().value("routes").toArray();
    const QJsonObject connection{{"scope", storageScope()}, {"host", host}, {"container", container}, {"routes", safe}};
    if (connection == m_hostConnection) return true;
    m_hostConnection = connection; persistPairingState(); return true;
}

void AccountSession::clearPairingCredentials() {
    if (m_pairingReply) {
        auto *reply = m_pairingReply.data(); m_pairingReply = nullptr;
        reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    m_pairingExpiry.stop(); m_pairingRequestBlocked = false;
    if (m_pairingCredentials.isEmpty()) return;
    m_pairingCredentials = {}; emit pairingCredentialsChanged();
}

void AccountSession::requestPairingCredentials() {
    if (!signedIn() || busy() || m_pairingLoading || m_pairingSessionEnding || m_pairingReply || m_pairingRequestBlocked
        || (m_pairingCredentials.value("version").toInt() == 2 && validPairingCredentials(m_pairingCredentials))) return;
    const auto service = m_manager.serviceUrl();
    if (service.scheme() != "https" && !(service.scheme() == "http" && QHostAddress(service.host()).isLoopback())) return;
    const auto session = m_manager.loginSession().value("id").toString();
    auto device = m_manager.deviceInfo(); device.insert("pairingVersion", 2);
    QNetworkRequest request(service.resolved(QUrl("/Account/GraphQL")));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Origin", service.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toEncoded());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    m_pairingRequestBlocked = true;
    auto *reply = m_network.post(request, iisacc::accounts::graphql::appSessionRequest(QJsonObject{{"intent", "pairing"},
        {"device", QJsonObject::fromVariantMap(device)}}));
    m_pairingReply = reply;
    QTimer::singleShot(10000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [reply] { if (reply->bytesAvailable() > 8192) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, session, device] {
        m_pairingReply = nullptr; reply->deleteLater();
        if (!signedIn() || m_manager.loginSession().value("id").toString() != session) return;
        const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
#ifdef SOCIETY_GROUP_STATE_RUNTIME_PROBE
        qInfo("Society pairing response status=%d networkError=%d", httpStatus, int(reply->error()));
#endif
        if (reply->bytesAvailable() > 8192) { persistPairingState(); return; }
        const auto decoded = iisacc::accounts::graphql::decode(reply->readAll(), "appSession", httpStatus);
        const auto status = decoded.status;
        const auto body = decoded.body;
        if (m_manager.handleSessionResponse(status, body, session)) return;
        if (!decoded.valid || reply->error() != QNetworkReply::NoError || status != 200) { persistPairingState(); return; }
        const auto proof = QJsonDocument::fromJson(body).object().value("pairing").toObject();
#ifdef SOCIETY_GROUP_STATE_RUNTIME_PROBE
        qInfo("Society pairing metadata version=%d seeds=%lld valid=%d", proof.value("version").toInt(1),
            qlonglong(proof.value("seeds").toObject().size()), validPairingCredentials(proof));
#endif
        if (!validPairingCredentials(proof)) { persistPairingState(); return; }
        m_pairingRequestBlocked = proof.value("version").toInt(1) == 1;
        m_pairingCredentials = proof; schedulePairingExpiry(); persistPairingState(); emit pairingCredentialsChanged();
    });
}

bool AccountSession::login(const QString &email, const QString &password) {
    return !busy() && m_manager.loginWithPassword(email.trimmed(), password);
}
bool AccountSession::refresh() { return !busy() && m_manager.refresh(); }
bool AccountSession::logout() {
    if (busy()) return false;
    return m_manager.logout();
}
void AccountSession::cancelLogin() { if (!signedIn()) m_manager.clear(); }
QByteArray AccountSession::relayCredential() const {
    if (!signedIn()) return {};
    QByteArray credential;
    const auto endpoint = m_manager.serviceUrl().resolved(QUrl("/Account/GraphQL"));
    for (const auto &cookie : m_network.cookieJar()->cookiesForUrl(endpoint)) {
        if (cookie.name() != "iisacc_auth_id" && cookie.name() != "iisacc_auth_refresh" && cookie.name() != "iisacc_login_session") continue;
        if (!credential.isEmpty()) credential += "; ";
        credential += cookie.toRawForm(QNetworkCookie::NameAndValueOnly);
    }
    return credential;
}
QString AccountSession::storageScope() const {
    if (!signedIn()) return {};
    const auto service = m_manager.serviceUrl();
    const auto origin = service.scheme() + "://" + service.host() + ':'
        + QString::number(service.port(service.scheme() == "https" ? 443 : 80));
    return QString::fromLatin1(QCryptographicHash::hash("society-auto-pair-v1\n" + origin.toUtf8() + '\n'
        + m_manager.account()->sub().toUtf8(), QCryptographicHash::Sha256).toHex());
}
