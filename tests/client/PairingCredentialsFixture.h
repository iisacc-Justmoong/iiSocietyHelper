#pragma once
#include <QJsonObject>
#include <QDateTime>

inline QJsonObject pairingCredentialsFixture(const QString &scope, char key = 'a') {
    const auto now = QDateTime::currentDateTimeUtc();
    const auto epoch = now.toMSecsSinceEpoch() / 300000;
    return {{"scope", scope}, {"expiresAt", now.addSecs(300).toString(Qt::ISODate)},
        {"keys", QJsonObject{{QString::number(epoch), QString(64, key)}, {QString::number(epoch + 1), QString(64, key)}}}};
}

inline QJsonObject localPairingCredentialsFixture(const QString &scope, char seed = 'a',
                                                 QDateTime issued = QDateTime::currentDateTimeUtc()) {
    const auto day = issued.toSecsSinceEpoch() / 86400;
    QJsonObject seeds;
    for (int offset = 0; offset < 7; ++offset) seeds.insert(QString::number(day + offset), QString(64, seed));
    return {{"version", 2}, {"scope", scope}, {"issuedAt", issued.toString(Qt::ISODate)},
        {"refreshAt", QDateTime::fromSecsSinceEpoch((day + 6) * 86400, Qt::UTC).toString(Qt::ISODate)},
        {"expiresAt", QDateTime::fromSecsSinceEpoch((day + 7) * 86400, Qt::UTC).toString(Qt::ISODate)}, {"seeds", seeds}};
}
