#pragma once

#include <iiAcountManager/AccountManager.h>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtGlobal>
#include <memory>
#include <type_traits>

class QDataStream;

#if defined(IISOCIETYHELPER_BUILDING_LIBRARY)
#  define IISOCIETYHELPER_EXPORT Q_DECL_EXPORT
#else
#  define IISOCIETYHELPER_EXPORT Q_DECL_IMPORT
#endif

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

// Access to the original Society drive using ordinary native paths. Independent
// of observation and delivery; owned by Helper and usable without start().
class IISOCIETYHELPER_EXPORT FileSystem final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable NOTIFY storageChanged)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY storageChanged)
    Q_PROPERTY(QString containerId READ containerId NOTIFY storageChanged)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY storageChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
public:
    explicit FileSystem(QObject *parent = nullptr);
    ~FileSystem() override;

    // Empty discovers Society's configured drive. Pins its canonical path and
    // UUID until open()/refresh(); a failed selection clears the previous one.
    Q_INVOKABLE bool open(const QString &containerPath = {});
    Q_INVOKABLE bool refresh();
    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] QString rootPath() const;
    [[nodiscard]] QString containerId() const;
    [[nodiscard]] QVariantList sections() const; // {key, name, path}
    [[nodiscard]] QString errorString() const;

    // sectionKey is the stable key from sections(), e.g. "files" or "models".
    // Empty relativePath returns that section. Only a final leaf may be absent.
    Q_INVOKABLE QString path(const QString &sectionKey, const QString &relativePath = {});
    Q_INVOKABLE QUrl url(const QString &sectionKey, const QString &relativePath = {});
    Q_INVOKABLE QString ensureDirectory(const QString &sectionKey, const QString &relativePath);
    // Android peer apps receive content URIs; use QFile/url() for I/O and entries()
    // for directory listing. Desktop and the Society owner retain native paths.
    Q_INVOKABLE QVariantList entries(const QString &sectionKey, const QString &relativePath = {});

signals:
    void storageChanged();
    void errorChanged();

private:
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

} // namespace iiSocietyHelper

Q_DECLARE_METATYPE(iiSocietyHelper::Peer)
Q_DECLARE_METATYPE(iiSocietyHelper::ObjectSnapshot)
