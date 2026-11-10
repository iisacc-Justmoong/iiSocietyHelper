#pragma once
#include <iiSocietyHelper/Export.h>
#include <QObject>
#include <QJsonObject>

namespace iiSocietyHelper {
// Transport used by FileDownload. Implementations own authentication and must
// publish only responses belonging to their current host/account binding.
class IISOCIETYHELPER_EXPORT FileHost : public QObject {
    Q_OBJECT
public:
    explicit FileHost(QObject *parent = nullptr);
    ~FileHost() override;
    virtual bool connectHost() = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;
    virtual QJsonObject hostBinding() const = 0;
    virtual QString statHostEntry(const QString &path) = 0;
    virtual QString readHostFile(const QString &path, qint64 offset, const QString &version) = 0;
signals:
    void changed();
    void hostDriveResponse(QString requestId, QJsonObject response);
};
}
