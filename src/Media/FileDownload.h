#pragma once
#include <iiSocietyHelper/Export.h>
#include <QObject>
#include <QVariantMap>
#include <memory>

namespace iiSocietyHelper {
class FileSystem;
class Client;

// One explicit download into the selected native .societycontainer. The supplied
// dedicated Client uses connectHost(), never the replica's shared-session lease
// or a request file consumed by another application. FileSystem/Client must
// outlive this object. All filesystem and journal transfer work runs off-thread.
class IISOCIETYHELPER_EXPORT FileDownload final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
public:
    FileDownload(FileSystem *files, Client *client, QObject *parent = nullptr);
    ~FileDownload() override;
    QVariantMap state() const;
    bool active() const;
    // Container identity is mandatory: a stale UI Alias cannot select a new drive.
    // Starting a different file cancels the previous open intent. Repeating the
    // same active selection does not restart or duplicate the download.
    Q_INVOKABLE bool start(const QString &key, const QString &containerId);
    Q_INVOKABLE void cancel();
signals:
    void changed();
    void ready(QVariantMap file);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
