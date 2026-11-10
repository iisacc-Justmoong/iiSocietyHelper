#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <optional>

namespace iiSocietyHelper {
// Incremental framing for the local service. A poisoned stream stays closed;
// callers must separately authenticate peers and validate message semantics.
class LocalFrame final {
public:
    static constexpr qsizetype MaximumBody = 512 * 1024;
    static QByteArray encode(const QJsonObject &message);
    bool append(const QByteArray &bytes);
    std::optional<QJsonObject> take();
    bool invalid() const noexcept { return invalid_; }
    bool pending() const noexcept { return !bytes_.isEmpty(); }
private:
    bool checkHeader();
    QByteArray bytes_;
    bool invalid_ = false;
};
}
