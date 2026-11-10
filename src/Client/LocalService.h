#pragma once
#include <iiSocietyHelper/Export.h>
#include <QObject>
#include <QHostAddress>
#include <functional>
#include <memory>
class AccountSession;
class DiscoveryService;

namespace iiSocietyHelper {
// Society owns the account and every network Client. The authorizer must
// validate the current product entitlement on each operation, not just login.
class IISOCIETYHELPER_EXPORT LocalService final : public QObject {
public:
    using Authorizer = std::function<bool(const QString &, const QString &)>;
    LocalService(AccountSession *account, Authorizer authorize, DiscoveryService *discovery = nullptr, QObject *parent = nullptr);
    LocalService(AccountSession *account, Authorizer authorize, DiscoveryService *discovery,
                 QHostAddress bindAddress, QObject *parent = nullptr);
    ~LocalService() override;
    bool listen(const QString &directory);
    void stop();
    QString path() const;
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
