#pragma once

#include <iiSocietyHelper/Export.h>
#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper/AccountSession.h>

#include <iiAcountManager/AccountManager.h>
#include <SharedStorage.h>
#include <Container.h>
#include <iiSharedCanvas/Media/MediaIo.h>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtGlobal>
#include <memory>
#include <type_traits>
#include <atomic>

class QDataStream;


namespace iiSocietyHelper {

Q_NAMESPACE_EXPORT(IISOCIETYHELPER_EXPORT)

enum class Activity { Unknown, Foreground, Background };
Q_ENUM_NS(Activity)
enum class DepartureReason { Withdrawn, TimedOut };
Q_ENUM_NS(DepartureReason)

struct ApplicationInfo {
    QString id;
    QString name;
    QString version;
    bool operator==(const ApplicationInfo &) const = default;
};

struct IISOCIETYHELPER_EXPORT Peer {
    ApplicationInfo application;
    QString instanceId;
    qint64 processId = 0;
    Activity activity = Activity::Unknown;
    QDateTime startedAt;
    QDateTime lastSeenAt;
    [[nodiscard]] QVariantMap toVariantMap() const;
};

struct ObservationOptions {
    // Empty uses the device-local shared directory (or Society App Group on iOS).
    // Replicated drives and network filesystems cannot hold local IPC state.
    QString directory;
    int heartbeatIntervalMs = 1000;
    int peerTimeoutMs = 5000;
};

// A value snapshot for transfer; no QObject ownership or address crosses IPC.
struct IISOCIETYHELPER_EXPORT ObjectSnapshot {
    QString className;
    QVariantMap properties;
    bool operator==(const ObjectSnapshot &) const = default;
};
IISOCIETYHELPER_EXPORT QDataStream &operator<<(QDataStream &, const ObjectSnapshot &);
IISOCIETYHELPER_EXPORT QDataStream &operator>>(QDataStream &, ObjectSnapshot &);

class IISOCIETYHELPER_EXPORT ObjectCodec final {
public:
    // Values need a registered Qt metatype and QDataStream read/write operators.
    // QObject uses its const invokable toVariantMap(), or readable STORED properties.
    [[nodiscard]] static QVariantMap encode(const QVariant &value, QString *error = nullptr);
    [[nodiscard]] static QVariantMap encode(const QObject *object, QString *error = nullptr);
    // Returns the original value type, or ObjectSnapshot for a QObject sender.
    // The receiving process must register custom value types before decoding.
    [[nodiscard]] static QVariant decode(const QVariantMap &payload, QString *error = nullptr);
};

// Immutable storage selection and already discovered Alias values. No QObject,
// worker, timer, account session or writable file binding is retained. A default
// value is invalid; isValid() means captured, not that the drive is still mounted.
class IISOCIETYHELPER_EXPORT PreparedStorage final {
public:
    PreparedStorage() = default;
    [[nodiscard]] bool isValid() const noexcept { return bool(d); }
private:
    class Data;
    std::shared_ptr<const Data> d;
    friend class FileSystem;
};

// Access to the original Society drive using ordinary native paths. Independent
// of observation and delivery; owned by Helper and usable without start().
class IISOCIETYHELPER_EXPORT FileSystem final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable NOTIFY storageChanged)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY storageChanged)
    Q_PROPERTY(QString containerId READ containerId NOTIFY storageChanged)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY storageChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(iiSocietyContainer::Container *container READ container CONSTANT)
    Q_PROPERTY(QVariantMap containerState READ containerState NOTIFY containerStateChanged)
public:
    enum class Initialization { Discover, Deferred };
    explicit FileSystem(QObject *parent = nullptr);
    // Deferred creates only the binding adapter. No selection, mirror scan,
    // timer or storage-monitor worker starts until an explicit storage command.
    FileSystem(Initialization initialization, QObject *parent = nullptr);
    ~FileSystem() override;

    // Empty discovers Society's configured drive. Pins its canonical path and
    // UUID until open()/refresh(); a failed selection clears the previous one.
    Q_INVOKABLE bool open(const QString &containerPath = {});
    // Capture on this object's owning thread, then transfer the values to a new
    // owner. Adoption checks the pinned drive identity and publishes the saved
    // mirror; it does not reopen selection, walk directories or read payloads.
    [[nodiscard]] PreparedStorage preparedStorage() const;
    bool adoptStorage(const PreparedStorage &storage);
    Q_INVOKABLE bool refresh();
    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] QString rootPath() const;
    [[nodiscard]] QString containerId() const;
    [[nodiscard]] QVariantList sections() const; // {key, name, path}
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] iiSocietyContainer::Container *container() const;
    // Credential-free state published by SocietyContainerDaemon. Delivery is on
    // this QObject's thread; daemonRunning becomes false for stale heartbeats.
    [[nodiscard]] QVariantMap containerState() const;
    Q_INVOKABLE bool synchronize();
    // Nonblocking native mirror scan, coalesced to one active and one pending
    // request. Results are published on this object's thread. Explicit open()
    // and synchronize() invalidate older replies and remain synchronous.
    Q_INVOKABLE void requestSynchronization();
    // Alias snapshots are provided by the storage owner. Applications do not
    // probe paths or persist payloads through the read-only Container object.
    Q_INVOKABLE QVariantMap alias(const QString &sectionKey, const QString &relativePath);
    Q_INVOKABLE QVariantMap aliasForPath(const QString &source);
    Q_INVOKABLE bool openForSource(const QString &source);
    Q_INVOKABLE bool selectDefaultContainer(const QString &source);
    Q_INVOKABLE QUrl preview(const QString &source);
    Q_INVOKABLE QString canvasDestination(const QString &destination);
    iiSharedCanvas::MediaDocumentResult readCanvas(const QString &source);
    Q_INVOKABLE QVariantMap importCanvas(const QString &source);
    Q_INVOKABLE QVariantMap createImageCanvas(const QString &source, const QString &destination);
    Q_INVOKABLE QVariantMap saveCanvas(const QByteArray &encoded, const QString &destination,
                                      const QString &expectedVersion = {});
    Q_INVOKABLE QString publishFile(const QString &source, const QString &sectionKey, const QString &relativePath);
    Q_INVOKABLE bool removeFile(const QString &sectionKey, const QString &relativePath);
    Q_INVOKABLE QVariantMap saveFile(const QString &destination, const QByteArray &bytes, bool overwrite = true);
    Q_INVOKABLE bool removeGenerationInputs(const QString &directory);
    Q_INVOKABLE bool discardLegacyGenerationStorage();
    Q_INVOKABLE bool exportMedia(const QUrl &source, const QUrl &destination, bool video = false);
    // Fresh generation inventory from the selected Society drive. Local owners
    // reconcile disk changes; replicas retain remote download-on-demand models.
    [[nodiscard]] QList<iiSocietyContainer::StoredModel> models();
    QString resolveModel(const QJsonObject &reference, QString *error = nullptr);
    QString path(iiSocietyContainer::StoreSection section, const QString &relativePath, QString *error);
    QStringList files(const QString &key);
    bool available(const QStringList &keys);
    QString requestFiles(const QStringList &keys, QString *error = nullptr);
    QJsonObject requestState(const QString &id);
    bool cancelFileRequest(const QString &id);

    // sectionKey is the stable key from sections(), e.g. "files" or "models".
    // Empty relativePath returns that section. Only a final leaf may be absent.
    Q_INVOKABLE QString path(const QString &sectionKey, const QString &relativePath = {});
    Q_INVOKABLE QUrl url(const QString &sectionKey, const QString &relativePath = {});
    Q_INVOKABLE QString ensureDirectory(const QString &sectionKey, const QString &relativePath);
    Q_INVOKABLE QString ensureRuntimeDirectory(const QString &relativePath);
    // Synchronous native PNG publication, independent of presence/the daemon.
    // Writes into this pinned drive's flat History; collisions preserve originals.
    Q_INVOKABLE QString publishGeneratedImage(const QString &source, const QString &fileName);
    // Android peer apps receive content URIs; use QFile/url() for I/O and entries()
    // for directory listing. Desktop and the Society owner retain native paths.
    Q_INVOKABLE QVariantList entries(const QString &sectionKey, const QString &relativePath = {});
    // Blocking on this object's thread: on-demand Provider details, preview and
    // classification. GUI code can copy SharedStorage to its I/O worker or use
    // Client::inspectHostFile's asynchronous host inspection.
    Q_INVOKABLE QVariantMap fileInformation(const QString &sectionKey, const QString &relativePath);

signals:
    void storageChanged();
    void synchronizationFinished(bool success);
    void containerStateChanged();
    void errorChanged();

private:
    void setError(const QString &message);
    class Private;
    std::unique_ptr<Private> d;
};

// Cooperative presence on one device. Use on the application's event-loop thread.
// A peer becomes visible only after its heartbeat advances while observed.
class IISOCIETYHELPER_EXPORT Helper final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QVariantList observedApplications READ observedApplications NOTIFY peersChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(iiSocietyHelper::FileSystem *fileSystem READ fileSystem CONSTANT)
    Q_PROPERTY(iisacc::accounts::AccountManager *accountManager READ accountManager NOTIFY accountManagerChanged)
    Q_PROPERTY(iisacc::accounts::Account *account READ account NOTIFY accountChanged)
public:
    explicit Helper(QObject *parent = nullptr);
    ~Helper() override;

    [[nodiscard]] bool start(const ApplicationInfo &application,
                             const ObservationOptions &options = {}, QString *error = nullptr);
    Q_INVOKABLE void stop();
    void setActivity(Activity activity);
    // Returns a durable outbox message ID, or empty on failure. Receipt is separate.
    Q_INVOKABLE QString sendData(const QString &topic, const QVariantMap &payload);
    // Capture now and durably enqueue; source objects may change or die afterward.
    Q_INVOKABLE QString sendObject(const QString &topic, QObject *object);
    QString sendObject(const QString &topic, const QObject *object);
    QString sendObject(const QString &topic, const QVariant &value);
    QString sendObject(const QString &topic, const QObject &object) { return sendObject(topic, &object); }
    QString sendObject(const QString &topic, std::nullptr_t) { return sendObject(topic, static_cast<const QObject *>(nullptr)); }
    template <typename T>
        requires (!std::is_pointer_v<T> && !std::is_base_of_v<QObject, T> && !std::is_same_v<T, QVariant>)
    QString sendObject(const QString &topic, const T &value) { return sendObject(topic, QVariant::fromValue(value)); }
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] Peer self() const;
    [[nodiscard]] QList<Peer> peers() const;
    [[nodiscard]] QVariantList observedApplications() const;
    [[nodiscard]] QString directory() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] FileSystem *fileSystem() const;
    // Borrow the host's manager on this event-loop thread. nullptr detaches.
    // Independent of start()/stop(); never changes ownership or signs out.
    // False means a thread mismatch or a binding superseded during notification.
    Q_INVOKABLE bool setAccountManager(iisacc::accounts::AccountManager *manager);
    [[nodiscard]] iisacc::accounts::AccountManager *accountManager() const;
    [[nodiscard]] iisacc::accounts::Account *account() const;
    [[nodiscard]] static QString defaultDirectory(QString *error = nullptr);

signals:
    void runningChanged();
    void peersChanged();
    void peerAppeared(const iiSocietyHelper::Peer &peer);
    void peerUpdated(const iiSocietyHelper::Peer &peer);
    void peerDisappeared(const iiSocietyHelper::Peer &peer, iiSocietyHelper::DepartureReason reason);
    void errorChanged();
    void errorOccurred(const QString &message);
    void dataQueued(const QString &messageId);
    void accountManagerChanged();
    // The reference changed, or the referenced account/author data changed.
    void accountChanged();

private:
    QString sendObjectImpl(const QString &topic, const QObject *object, const QVariant *value);
    class Private;
    std::unique_ptr<Private> d;
};

// Retained for existing 0.1 consumers.
[[nodiscard]] IISOCIETYHELPER_EXPORT QString helloWorld();
IISOCIETYHELPER_EXPORT bool migrateLegacyCache(const QString &source, const QString &destination,
    const std::atomic_bool &cancelled, QString *error);

} // namespace iiSocietyHelper

Q_DECLARE_METATYPE(iiSocietyHelper::Peer)
Q_DECLARE_METATYPE(iiSocietyHelper::ObjectSnapshot)

// Consolidated remote generation and device synchronization APIs.
#include <iiSocietyHelper/Generation/Remote.h>
#include <iiSocietyHelper/Sync/Sync.h>
