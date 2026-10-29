#pragma once
#include <iiSocietyHelper/Export.h>
#include <iiSocietyHelper/NearbyDevices.h>
#include <iiWebServer.h>
#include <functional>

// The application supplies transport operations; the queue owns no UI or files.
class IISOCIETYHELPER_EXPORT AutomaticPairing final : public QObject {
    Q_OBJECT
public:
    AutomaticPairing(NearbyDevices *nearby, iiWebServer::LanPeer *peer,
        std::function<bool()> host, std::function<bool(const QString &)> join,
        std::function<void()> stop, QObject *parent = nullptr);
    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }
    bool active() const { return !m_current.isEmpty(); }
    QVariantList queue() const;
    QString status() const;
signals:
    void changed();
private:
    void schedule();
    void pump();
    void finish(bool success);
    void reset();
    NearbyDevices *m_nearby;
    iiWebServer::LanPeer *m_peer;
    std::function<bool()> m_host;
    std::function<bool(const QString &)> m_join;
    std::function<void()> m_stop;
    QTimer m_timer;
    QStringList m_queue;
    QString m_current, m_leader;
    QHash<QString, qint64> m_retryAt;
    QHash<QString, int> m_attempts;
    qint64 m_deadline = 0;
    bool m_enabled = true, m_pumping = false, m_ownsTransport = false, m_scheduled = false;
};
