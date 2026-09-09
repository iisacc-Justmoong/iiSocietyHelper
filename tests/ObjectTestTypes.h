#pragma once
#include <QDataStream>
#include <QDateTime>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <limits>
#include <memory>

struct GenerationJob {
    QString id;
    qint64 frame = 0;
    quint64 seed = 0;
    QByteArray data;
    QDateTime createdAt;
    QUrl source;
    bool operator==(const GenerationJob &) const = default;
};
inline QDataStream &operator<<(QDataStream &stream, const GenerationJob &job)
{
    return stream << quint32(1) << job.id << job.frame << job.seed << job.data << job.createdAt << job.source;
}
inline QDataStream &operator>>(QDataStream &stream, GenerationJob &job)
{
    quint32 version = 0;
    stream >> version;
    if (version != 1) { stream.setStatus(QDataStream::ReadCorruptData); return stream; }
    return stream >> job.id >> job.frame >> job.seed >> job.data >> job.createdAt >> job.source;
}
Q_DECLARE_METATYPE(GenerationJob)

inline GenerationJob sampleJob()
{
    return {"job-한글", std::numeric_limits<qint64>::min(), std::numeric_limits<quint64>::max(),
        QByteArray::fromHex("0001fffe800041"), QDateTime::fromString("2026-09-09T00:00:00.123Z", Qt::ISODateWithMs),
        QUrl("https://example.org/a%20b#result")};
}

class TransferObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(qint64 count MEMBER count)
    Q_PROPERTY(QByteArray bytes MEMBER bytes)
    Q_PROPERTY(QObject *detail MEMBER detail)
    Q_PROPERTY(QString transient READ transient STORED false)
public:
    using QObject::QObject;
    QString title = "original";
    qint64 count = 9007199254740993LL;
    QByteArray bytes = QByteArray::fromHex("00ff80");
    QObject *detail = nullptr;
    QString transient() const { return "not-persistent"; }
};

class CallbackObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString value READ value)
public:
    std::function<void()> onRead;
    QString value() const { if (onRead) onRead(); return "value"; }
};

class DeletingSiblingObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *detail READ detail)
    Q_PROPERTY(int trigger READ trigger)
public:
    mutable std::unique_ptr<TransferObject> child = std::make_unique<TransferObject>();
    QObject *detail() const { return child.get(); }
    int trigger() const { child.reset(); return 1; }
};

struct UnsupportedObject { int value = 1; };
Q_DECLARE_METATYPE(UnsupportedObject)
