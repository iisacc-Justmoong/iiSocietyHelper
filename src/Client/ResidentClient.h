#pragma once
#include <iiSocietyHelper/Export.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <memory>

namespace iiSocietyHelper {
// A per-application connection to the daemon-owned iisaccson inference service.
class IISOCIETYHELPER_EXPORT ResidentClient final : public QObject {
    Q_OBJECT
public:
    explicit ResidentClient(QString brokerDirectory = {}, QObject *parent = nullptr);
    ~ResidentClient() override;
    static QString serverAddress(const QString &brokerDirectory);
    void connectToDaemon();
    void disconnectFromDaemon();
    bool connected() const;
    QString generate(const QJsonArray &messages, const QJsonObject &options = {});
    bool cancel(const QString &requestId);
signals:
    void connectionChanged(bool connected);
    void residentChanged(const QJsonObject &status);
    void generationEvent(const QString &requestId, const QJsonObject &event);
    void requestFailed(const QString &requestId, const QString &error);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
