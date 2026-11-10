#pragma once
#include <iiSocietyHelper/FileHost.h>
#include <memory>

namespace iiSocietyHelper {
// Local, credential-free proxy. All network activity belongs to Society.
class IISOCIETYHELPER_EXPORT LocalClient final : public FileHost {
    Q_OBJECT
public:
    LocalClient(QString product, QString app, QString device, QString socketPath = {}, QObject *parent = nullptr);
    ~LocalClient() override;
    bool start(const QString &container);
    // Hold an authorized account binding without opening a storage replica.
    bool observe();
    bool sessionBound() const;
    bool connectHost() override;
    void stop() override;
    void synchronizeNow();
    // Set only from a separately verified Society license. Changing or
    // clearing the subject immediately tears down the old local session.
    void setExpectedSubject(const QString &subject);
    bool connected() const override;
    QJsonObject hostBinding() const override;
    QString statHostEntry(const QString &path) override;
    QString readHostFile(const QString &path, qint64 offset, const QString &version) override;
    QString requestHost(const QJsonObject &payload);
signals:
    void sessionChanged(bool connected);
    void synchronized();
    void progress(QString path, qint64 completed, qint64 total);
    void hostResponse(QString requestId, QJsonObject response);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
