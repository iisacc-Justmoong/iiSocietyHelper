#pragma once

#include "iiSocietyHelper.h"

namespace iiSocietyHelper {

// Durable per-user outbox and daemon inbox. One object/SQL connection per thread.
// Senders enqueue; the Society daemon accepts; readers explicitly acknowledge.
class IISOCIETYHELPER_EXPORT DeliveryStore final {
public:
    DeliveryStore();
    ~DeliveryStore();
    DeliveryStore(const DeliveryStore &) = delete;
    DeliveryStore &operator=(const DeliveryStore &) = delete;

    [[nodiscard]] bool open(const QString &observationDirectory, QString *error = nullptr);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString enqueue(const Peer &sender, const QString &topic, const QVariantMap &payload);
    // Atomic outbox -> inbox move; a successful retry cannot create duplicate receipts.
    [[nodiscard]] int receivePending(int limit = 128);
    [[nodiscard]] qint64 pendingCount() const;
    [[nodiscard]] QVariantList readAfter(qint64 sequence, int limit = 128) const;
    [[nodiscard]] bool acknowledge(const QString &consumer, qint64 sequence);
    [[nodiscard]] qint64 acknowledged(const QString &consumer) const;
    [[nodiscard]] bool setDaemonSnapshot(const QVariantMap &snapshot);
    [[nodiscard]] QVariantMap daemonSnapshot() const;

private:
    class Private;
    std::unique_ptr<Private> d;
};
}
