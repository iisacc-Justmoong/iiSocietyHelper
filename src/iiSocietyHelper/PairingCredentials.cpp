#include "PairingCredentials.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>

namespace {
bool hexKey(const QString &value) {
    static const QRegularExpression expression("\\A[a-f0-9]{64}\\z");
    return expression.match(value).hasMatch();
}
}
bool SocietyPairingCredentials::valid(const QJsonObject &proof, QDateTime now) {
    const auto expiry = QDateTime::fromString(proof.value("expiresAt").toString(), Qt::ISODate);
    if (!hexKey(proof.value("scope").toString()) || !expiry.isValid() || expiry <= now) return false;
    if (proof.value("version").toInt(1) == 1) {
        if (expiry > now.addSecs(600)) return false;
        const auto keys = proof.value("keys").toObject();
        return keys.size() >= 2 && keys.size() <= 3
            && hexKey(keys.value(QString::number(now.toSecsSinceEpoch() / 300)).toString());
    }
    if (proof.value("version").toInt() != 2) return false;
    const auto issued = QDateTime::fromString(proof.value("issuedAt").toString(), Qt::ISODate);
    const auto refresh = QDateTime::fromString(proof.value("refreshAt").toString(), Qt::ISODate);
    if (!issued.isValid() || issued > now.addSecs(300) || !refresh.isValid()
        || refresh < issued || refresh > expiry || expiry > issued.addDays(7)) return false;
    const auto firstDay = issued.toSecsSinceEpoch() / 86400;
    const auto seeds = proof.value("seeds").toObject();
    if (seeds.isEmpty() || seeds.size() > 7 || expiry.toSecsSinceEpoch() > (firstDay + seeds.size()) * 86400) return false;
    for (qsizetype offset = 0; offset < seeds.size(); ++offset)
        if (!hexKey(seeds.value(QString::number(firstDay + offset)).toString())) return false;
    return true;
}
QByteArray SocietyPairingCredentials::key(const QJsonObject &proof, qint64 epoch, QDateTime now) {
    if (!valid(proof, now) || epoch < 0) return {};
    if (proof.value("version").toInt(1) == 1) {
        const auto value = proof.value("keys").toObject().value(QString::number(epoch)).toString();
        return hexKey(value) ? QByteArray::fromHex(value.toLatin1()) : QByteArray();
    }
    const auto seed = proof.value("seeds").toObject().value(QString::number(epoch / 288)).toString();
    if (!hexKey(seed)) return {};
    return QMessageAuthenticationCode::hash("society-auto-pair-key-v2\n" + proof.value("scope").toString().toUtf8()
        + '\n' + QByteArray::number(epoch), QByteArray::fromHex(seed.toLatin1()), QCryptographicHash::Sha256);
}

QJsonObject SocietyPairingCredentials::sign(const QJsonObject &credentials, const QByteArray &context) {
    const auto epoch = QDateTime::currentSecsSinceEpoch() / 300;
    const auto secret = key(credentials, epoch);
    if (secret.isEmpty()) return {};
    return {{"scope", credentials.value("scope")}, {"epoch", QString::number(epoch)},
        {"mac", QString::fromLatin1(QMessageAuthenticationCode::hash("society-lan-account-v1\n" + context, secret, QCryptographicHash::Sha256).toHex())}};
}
bool SocietyPairingCredentials::verify(const QJsonObject &credentials, const QByteArray &context, const QJsonObject &proof) {
    bool ok = false; const auto epoch = proof.value("epoch").toString().toLongLong(&ok);
    const auto current = QDateTime::currentSecsSinceEpoch() / 300;
    if (!ok || epoch < current - 1 || epoch > current + 1 || proof.value("scope") != credentials.value("scope")) return false;
    const auto secret = key(credentials, epoch);
    if (secret.isEmpty() || !hexKey(proof.value("mac").toString())) return false;
    const auto expected = QMessageAuthenticationCode::hash("society-lan-account-v1\n" + context, secret, QCryptographicHash::Sha256).toHex();
    const auto actual = proof.value("mac").toString().toLatin1();
    unsigned difference = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(expected[i] ^ actual[i]);
    return difference == 0;
}
