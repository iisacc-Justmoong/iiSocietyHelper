#include "LocalFrame.h"
#include <QJsonDocument>
#include <QJsonParseError>

namespace iiSocietyHelper {
namespace {
quint32 length(const QByteArray &bytes) {
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}
}
QByteArray LocalFrame::encode(const QJsonObject &message) {
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (body.isEmpty() || body.size() > MaximumBody) return {};
    const auto n = quint32(body.size());
    QByteArray result; result.reserve(4 + body.size());
    result.append(char(n >> 24)); result.append(char(n >> 16)); result.append(char(n >> 8)); result.append(char(n));
    result.append(body); return result;
}
bool LocalFrame::checkHeader() {
    if (bytes_.size() < 4) return true;
    const auto n = length(bytes_);
    if (!n || n > MaximumBody) { invalid_ = true; bytes_.clear(); return false; }
    return true;
}
bool LocalFrame::append(const QByteArray &bytes) {
    if (invalid_) return false;
    if (bytes.size() > 2 * (MaximumBody + 4) - bytes_.size()) { invalid_ = true; bytes_.clear(); return false; }
    bytes_.append(bytes); return checkHeader();
}
std::optional<QJsonObject> LocalFrame::take() {
    if (invalid_ || !checkHeader() || bytes_.size() < 4) return {};
    const auto n = length(bytes_);
    if (bytes_.size() < qsizetype(n) + 4) return {};
    QJsonParseError error; const auto json = QJsonDocument::fromJson(bytes_.mid(4, n), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) { invalid_ = true; bytes_.clear(); return {}; }
    bytes_.remove(0, n + 4);
    return json.object();
}
}
