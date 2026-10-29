#pragma once
#include <iiSocietyHelper/Export.h>
#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

namespace iiSocietyHelper::Generation {
using Sender = std::function<QString(const QJsonObject &)>;
// One remote job. Transport remains owned by the authenticated Society SDK.
// Repeated submits are idempotent; accepted jobs survive a client disconnect.
class IISOCIETYHELPER_EXPORT Remote final : public QObject {
    Q_OBJECT
public:
    explicit Remote(Sender send, QObject *parent = nullptr);
    bool start(const QJsonObject &job);
    void receive(const QString &requestId, const QJsonObject &response);
    void cancel();
    void setPaused(bool paused);
    bool active() const { return !m_job.isEmpty(); }
signals:
    void accepted();
    void progress(QJsonObject status);
    void finished(QString state, QByteArray png, QJsonObject generation, QString error);
private:
    void tick();
    void complete(const QString &state, const QString &error = {});
    Sender m_send;
    QTimer m_timer;
    QElapsedTimer m_pendingAge, m_disconnected;
    QJsonObject m_job, m_result;
    QString m_pending, m_action;
    QByteArray m_image;
    bool m_accepted = false, m_cancelled = false, m_paused = false;
};
}
