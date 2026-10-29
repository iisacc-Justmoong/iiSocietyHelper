#pragma once
#include "Export.h"

#include <iiAccountManager.h>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointer>
#include <QDateTime>
#include <QTimer>

// One account session for the application. UI and device transport borrow it.
class IISOCIETYHELPER_EXPORT AccountSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(iisacc::accounts::AccountManager *manager READ manager CONSTANT)
    Q_PROPERTY(QObject *account READ account CONSTANT)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool codeRequired READ codeRequired NOTIFY changed)
    Q_PROPERTY(QString errorString READ errorString NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString email READ email NOTIFY changed)
    Q_PROPERTY(QString userId READ userId NOTIFY changed)
    Q_PROPERTY(QString membership READ membership NOTIFY changed)
    Q_PROPERTY(QVariantList rememberedDevices READ rememberedDevices NOTIFY rememberedDevicesChanged)
public:
    explicit AccountSession(QObject *parent = nullptr);
    explicit AccountSession(const QUrl &serviceUrl, QObject *parent = nullptr);
    AccountSession(const QUrl &serviceUrl, iisacc::accounts::SessionStore *store, QObject *parent);
    iisacc::accounts::AccountManager *manager() { return &m_manager; }
    QObject *account() const { return m_manager.account(); }
    bool signedIn() const { return m_manager.isAuthenticated(); }
    bool busy() const { return m_manager.isLoading() || m_manager.isRestoringSession(); }
    bool codeRequired() const { return !m_manager.loginChallenge().isEmpty(); }
    QString errorString() const { return !m_manager.errorString().isEmpty() ? m_manager.errorString()
        : !m_manager.sessionStorageError().isEmpty() ? m_manager.sessionStorageError() : m_pairingStorageError; }
    QString displayName() const { return m_manager.account()->displayLabel(); }
    QString email() const { return m_manager.account()->email(); }
    QString userId() const { return m_manager.account()->userId(); }
    QString membership() const { return m_manager.account()->toVariantMap().value("societyCloudMembership").toString(); }
    // Active cookies stay in C++ and are sent only to the configured trusted
    // account server. Direct LAN pairing does not use them.
    QByteArray relayCredential() const;
    // Stable namespace shared by LAN, relay and all apps on this device.
    QString storageScope() const;
    // C++ only; restored from an authenticated, bounded group-container cache.
    QJsonObject pairingCredentials() const { return m_pairingCredentials; }
    QJsonArray rememberedPeers() const { return m_rememberedPeers; }
    QVariantList rememberedDevices() const { return signedIn() ? m_rememberedPeers.toVariantList() : QVariantList{}; }
    bool automaticPairingEnabled() const { return m_automaticPairingEnabled; }
    QJsonObject serverConfiguration() const { return m_serverConfiguration; }
    bool setServerConfiguration(const QJsonObject &configuration);
    void setAutomaticPairingEnabled(bool enabled);
    void rememberPairedDevice(const QString &id, const QString &name, const QString &kind);
    // Encrypted routing hints, scoped to the current account's registered drive.
    // They grant no peer access; every reconnect still authenticates freshly.
    QJsonObject savedHostConnection() const;
    bool rememberHostConnection(const QString &host, const QString &container, const QJsonArray &routes);
    void requestPairingCredentials();
    Q_INVOKABLE bool login(const QString &email, const QString &password);
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE bool logout();
    Q_INVOKABLE void cancelLogin();
signals:
    void changed();
    void rememberedDevicesChanged();
    void sessionEnding();
    void pairingCredentialsChanged();
    void pairingStateRestored();
    void pairingCredentialsRequired();
private:
    void initializePersistence(iisacc::accounts::SessionStore *store);
    void restoreCachedAccount();
    void schedulePairingExpiry();
    void restorePairingState();
    void persistPairingState();
    void discardPairingState();
    QString pairingStateKey() const;
    bool validPairingCredentials(const QJsonObject &proof) const;
    void clearPairingCredentials();
    QNetworkAccessManager m_network;
    iisacc::accounts::AccountManager m_manager;
    QPointer<QNetworkReply> m_pairingReply;
    QJsonObject m_pairingCredentials;
    QJsonObject m_serverConfiguration;
    QJsonObject m_hostConnection;
    QDateTime m_pairingRestoreRetryAt;
    QTimer m_pairingExpiry;
    QPointer<iisacc::accounts::SessionStore> m_stateStore;
    QJsonArray m_rememberedPeers;
    QString m_pairingLoadedSession, m_pairingStorageError;
    quint64 m_pairingRevision = 0;
    bool m_pairingRequestBlocked = false, m_verifiedThisRun = false;
    bool m_pairingLoading = false, m_automaticPairingEnabled = true, m_pairingSessionEnding = false;
};
