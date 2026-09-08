#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtGlobal>
#include <memory>

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
    QString directory;
    int heartbeatIntervalMs = 1000;
    int peerTimeoutMs = 5000;
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
public:
    explicit Helper(QObject *parent = nullptr);
    ~Helper() override;

    [[nodiscard]] bool start(const ApplicationInfo &application,
                             const ObservationOptions &options = {}, QString *error = nullptr);
    Q_INVOKABLE void stop();
    void setActivity(Activity activity);
    // Returns a durable outbox message ID, or empty on failure. Receipt is separate.
    Q_INVOKABLE QString sendData(const QString &topic, const QVariantMap &payload);
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] Peer self() const;
    [[nodiscard]] QList<Peer> peers() const;
    [[nodiscard]] QVariantList observedApplications() const;
    [[nodiscard]] QString directory() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] FileSystem *fileSystem() const;
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

private:
    class Private;
    std::unique_ptr<Private> d;
};

// Retained for existing 0.1 consumers.
[[nodiscard]] IISOCIETYHELPER_EXPORT QString helloWorld();

} // namespace iiSocietyHelper

Q_DECLARE_METATYPE(iiSocietyHelper::Peer)
