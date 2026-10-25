#pragma once
#include "Export.h"
#include "Sync/Status.h"
#include <Container.h>
#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <memory>
class AccountSession;
class DiscoveryService;
namespace iiSocietyHelper {
// In-process Society storage service for a local replica or direct host access.
// Consumers never implement their own host credentials or file transport.
class IISOCIETYHELPER_EXPORT Client final : public QObject {
    Q_OBJECT
    Q_PROPERTY(iiSocietyContainer::Container *container READ container CONSTANT)
    Q_PROPERTY(QVariantList hostSections READ hostSections NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
public:
    explicit Client(QObject *parent = nullptr);
    Client(AccountSession *account, DiscoveryService *discovery, QObject *parent = nullptr);
    Client(AccountSession *account, DiscoveryService *discovery, QHostAddress bindAddress, QObject *parent = nullptr);
    ~Client() override;
    bool start(const QString &container);
    // Connect to the account's registered host without opening/creating a local
    // replica. Other devices use fresh account authentication and pinned transport;
    // the registered host device uses its selected, identity-checked native drive.
    Q_INVOKABLE bool connectHost();
    void stop();
    void synchronizeNow();
    QString status() const;
    Sync::Status &synchronizationStatus();
    bool connected() const;
    // Credential-free identity of the currently authenticated host session.
    QJsonObject hostBinding() const;
    // Owned by this Client; its address remains stable until destruction.
    iiSocietyContainer::Container *container() const;
    QVariantList hostSections() const;
    Q_INVOKABLE void refreshContainer();
    // Asynchronous direct host operations. Paths use section keys, e.g.
    // models/example.bin. An empty request ID means validation/connection failure.
    // Listings return nextCursor + version; pass both to continue the same page.
    Q_INVOKABLE QString listHostDirectory(const QString &path = {}, const QString &cursor = {}, const QString &version = {});
    Q_INVOKABLE QString statHostEntry(const QString &path);
    // Rich provider details, embedded preview and classification without reading
    // the original. Assembles bounded, version-checked chunks asynchronously;
    // hostDriveResponse contains a FileInformation object in "information".
    Q_INVOKABLE QString inspectHostFile(const QString &path);
    // Reads at most 256 KiB. Pin the returned version for subsequent chunks.
    Q_INVOKABLE QString readHostFile(const QString &path, qint64 offset = 0, const QString &version = {});
    // Atomic create-only write, at most 256 KiB. Existing entries are preserved.
    Q_INVOKABLE QString writeHostFile(const QString &path, const QByteArray &data);
    Q_INVOKABLE QString createHostDirectory(const QString &path);
    // Network-host requests use only this account's authenticated host. No caller URL,
    // credential or peer selection is accepted.
    QString requestHost(const QJsonObject &payload);
signals:
    void changed();
    void synchronized();
    void progress(QString path, qint64 completed, qint64 total);
    void hostResponse(QString requestId, QJsonObject response);
    // Direct results contain logical paths, never a usable host-native path.
    // read results encode data as base64 and include offset/size/version/eof.
    void hostDriveResponse(QString requestId, QJsonObject response);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
