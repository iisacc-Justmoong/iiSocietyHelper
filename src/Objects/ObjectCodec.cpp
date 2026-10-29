#include "iiSocietyHelper.h"

#include <QBuffer>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QThread>

namespace iiSocietyHelper {
namespace {
constexpr qsizetype maximumPayloadBytes = 64 * 1024;
constexpr qint64 maximumStreamBytes = 48 * 1024;
constexpr int maximumDepth = 16;
constexpr int maximumNodes = 4096;
constexpr auto format = "iisacc.qt-object";

bool fail(QString &error, const QString &message) { error = message; return false; }
bool validName(const QString &name) { return !name.isEmpty() && name.size() <= 256 && name.isValidUtf16() && !name.contains(QChar::Null); }
bool pointerType(QMetaType type)
{
    const auto flags = type.flags();
    return flags.testFlag(QMetaType::IsPointer) || flags.testFlag(QMetaType::PointerToQObject)
        || flags.testFlag(QMetaType::SharedPointerToQObject) || flags.testFlag(QMetaType::WeakPointerToQObject)
        || flags.testFlag(QMetaType::TrackingPointerToQObject);
}
bool checkValue(const QVariant &value, QString &error, int depth, int &nodes)
{
    if (depth > maximumDepth || ++nodes > maximumNodes)
        return fail(error, QStringLiteral("Object exceeds the depth or collection limit."));
    if (!value.isValid()) return true; // A null/absent property inside a snapshot.
    const auto type = value.metaType();
    if (pointerType(type)) return fail(error, QStringLiteral("Pointer values cannot be transferred; pass QObject directly for a snapshot."));
    if (!type.hasRegisteredDataStreamOperators())
        return fail(error, QStringLiteral("Object type has no QDataStream operators: %1.").arg(QString::fromUtf8(type.name())));
    if (type == QMetaType::fromType<ObjectSnapshot>()) {
        const auto snapshot = value.value<ObjectSnapshot>();
        return (validName(snapshot.className) || fail(error, QStringLiteral("Invalid snapshot class name.")))
            && checkValue(snapshot.properties, error, depth + 1, nodes);
    }
    if (type.id() == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (!it.key().isValidUtf16()) return fail(error, QStringLiteral("Invalid object property name."));
            if (!checkValue(it.value(), error, depth + 1, nodes)) return false;
        }
    } else if (type.id() == QMetaType::QVariantHash) {
        const auto map = value.toHash();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (!it.key().isValidUtf16()) return fail(error, QStringLiteral("Invalid object property name."));
            if (!checkValue(it.value(), error, depth + 1, nodes)) return false;
        }
    } else if (type.id() == QMetaType::QVariantList) {
        for (const auto &entry : value.toList()) if (!checkValue(entry, error, depth + 1, nodes)) return false;
    }
    return true;
}

// QBuffer normally grows without a limit. Stop the stream before a serializer
// can create an oversized queue message, including through seeks.
class BoundedBuffer final : public QBuffer {
public:
    bool seek(qint64 position) override { return position <= maximumStreamBytes && QBuffer::seek(position); }
protected:
    qint64 writeData(const char *data, qint64 length) override
    {
        if (length < 0 || length > maximumStreamBytes - pos()) return -1;
        return QBuffer::writeData(data, length);
    }
};
void configureStream(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_6_8);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

bool capture(const QObject *, ObjectSnapshot &, QString &, QSet<const QObject *> &, int, int &);
bool captureValue(const QVariant &value, QVariant &result, QString &error,
                  QSet<const QObject *> &active, int depth, int &nodes)
{
    if (depth > maximumDepth || ++nodes > maximumNodes)
        return fail(error, QStringLiteral("Object exceeds the depth or collection limit."));
    const auto type = value.metaType();
    if (type.flags().testFlag(QMetaType::PointerToQObject)) {
        const auto *object = value.value<QObject *>();
        if (!object) { result = QVariant::fromValue(nullptr); return true; }
        ObjectSnapshot child;
        if (!capture(object, child, error, active, depth + 1, nodes)) return false;
        result = QVariant::fromValue(child);
    } else if (type.id() == QMetaType::QVariantMap) {
        QVariantMap map;
        const auto values = value.toMap();
        for (auto it = values.begin(); it != values.end(); ++it) {
            QVariant captured;
            if (!captureValue(it.value(), captured, error, active, depth + 1, nodes)) return false;
            map.insert(it.key(), captured);
        }
        result = map;
    } else if (type.id() == QMetaType::QVariantHash) {
        QVariantHash map;
        const auto values = value.toHash();
        for (auto it = values.begin(); it != values.end(); ++it) {
            QVariant captured;
            if (!captureValue(it.value(), captured, error, active, depth + 1, nodes)) return false;
            map.insert(it.key(), captured);
        }
        result = map;
    } else if (type.id() == QMetaType::QVariantList) {
        QVariantList list;
        for (const auto &entry : value.toList()) {
            QVariant captured;
            if (!captureValue(entry, captured, error, active, depth + 1, nodes)) return false;
            list.append(captured);
        }
        result = list;
    } else result = value;
    return true;
}
bool capture(const QObject *object, ObjectSnapshot &result, QString &error,
             QSet<const QObject *> &active, int depth, int &nodes)
{
    if (!object) return fail(error, QStringLiteral("Cannot transfer a null QObject."));
    if (object->thread() != QThread::currentThread())
        return fail(error, QStringLiteral("Capture QObject on its owning thread."));
    if (depth > maximumDepth || ++nodes > maximumNodes)
        return fail(error, QStringLiteral("Object exceeds the depth or collection limit."));
    if (active.contains(object)) return fail(error, QStringLiteral("QObject snapshot contains a reference cycle."));
    active.insert(object);
    const auto remove = qScopeGuard([&] { active.remove(object); });
    const QPointer<const QObject> guard(object);
    const auto *meta = object->metaObject();
    result.className = QString::fromUtf8(meta->className());
    QVariantMap properties;
    const int methodIndex = meta->indexOfMethod("toVariantMap()");
    const auto method = meta->method(methodIndex);
    if (methodIndex >= 0 && method.isConst() && method.returnMetaType() == QMetaType::fromType<QVariantMap>()) {
        if (!method.invoke(const_cast<QObject *>(object), Qt::DirectConnection, Q_RETURN_ARG(QVariantMap, properties)))
            return fail(error, QStringLiteral("Could not read the object's toVariantMap contract."));
        if (!guard) return fail(error, QStringLiteral("QObject was destroyed while capturing its values."));
        QVariant normalized;
        if (!captureValue(properties, normalized, error, active, depth + 1, nodes)) return false;
        if (!guard) return fail(error, QStringLiteral("QObject was destroyed while capturing its values."));
        result.properties = normalized.toMap();
    } else {
        for (int i = QObject::staticMetaObject.propertyCount(); i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            if (!property.isReadable() || !property.isStored()) continue;
            const auto name = QString::fromUtf8(property.name());
            const auto value = property.read(object);
            if (!guard) return fail(error, QStringLiteral("QObject was destroyed while capturing its values."));
            if (!value.isValid()) return fail(error, QStringLiteral("Could not read QObject property: %1.").arg(name));
            // Finish nested QObject capture before another getter can destroy it.
            QVariant normalized;
            if (!captureValue(value, normalized, error, active, depth + 1, nodes)) return false;
            if (!guard) return fail(error, QStringLiteral("QObject was destroyed while capturing its values."));
            result.properties.insert(name, normalized);
        }
    }
    return true;
}
}

QDataStream &operator<<(QDataStream &stream, const ObjectSnapshot &snapshot)
{
    return stream << snapshot.className << snapshot.properties;
}
QDataStream &operator>>(QDataStream &stream, ObjectSnapshot &snapshot)
{
    return stream >> snapshot.className >> snapshot.properties;
}

QVariantMap ObjectCodec::encode(const QVariant &value, QString *output)
{
    if (output) output->clear();
    QString error;
    const auto reject = [&](const QString &message) { if (output) *output = message; return QVariantMap{}; };
    int nodes = 0;
    if (!value.isValid()) return reject(QStringLiteral("Cannot transfer an invalid QVariant."));
    if (!checkValue(value, error, 0, nodes)) return reject(error);
    const auto name = QString::fromUtf8(value.metaType().name());
    if (!validName(name)) return reject(QStringLiteral("Invalid object metatype name."));
    BoundedBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QDataStream stream(&buffer);
    configureStream(stream);
    try {
        if (!value.metaType().save(stream, value.constData()) || stream.status() != QDataStream::Ok)
            return reject(QStringLiteral("Object serialization failed or exceeds the 64 KiB message limit."));
    } catch (...) { return reject(QStringLiteral("Object serializer threw an exception.")); }
    QVariantMap payload{{"format", format}, {"version", 1}, {"streamVersion", int(QDataStream::Qt_6_8)},
        {"typeName", name}, {"data", QString::fromLatin1(buffer.data().toBase64())}};
    if (QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Compact).size() > maximumPayloadBytes)
        return reject(QStringLiteral("Encoded object exceeds the 64 KiB message limit."));
    return payload;
}

QVariantMap ObjectCodec::encode(const QObject *object, QString *output)
{
    if (output) output->clear();
    ObjectSnapshot snapshot;
    QString error;
    QSet<const QObject *> active;
    int nodes = 0;
    try {
        if (capture(object, snapshot, error, active, 0, nodes)) return encode(QVariant::fromValue(snapshot), output);
    } catch (...) { error = QStringLiteral("QObject snapshot getter threw an exception."); }
    if (output) *output = error;
    return {};
}

QVariant ObjectCodec::decode(const QVariantMap &payload, QString *output)
{
    if (output) output->clear();
    const auto reject = [&](const QString &message) { if (output) *output = message; return QVariant{}; };
    if (payload.size() != 5 || payload.value("format") != format
        || payload.value("typeName").metaType().id() != QMetaType::QString
        || payload.value("data").metaType().id() != QMetaType::QString
        || payload.value("data").toString().size() > maximumPayloadBytes)
        return reject(QStringLiteral("Invalid Society object envelope."));
    const auto json = QJsonObject::fromVariantMap(payload);
    if (json.value("version") != 1 || json.value("streamVersion") != int(QDataStream::Qt_6_8)
        || QJsonDocument(json).toJson(QJsonDocument::Compact).size() > maximumPayloadBytes)
        return reject(QStringLiteral("Unsupported or oversized Society object envelope."));
    const auto name = payload.value("typeName").toString();
    if (!validName(name)) return reject(QStringLiteral("Invalid object metatype name."));
    qRegisterMetaType<ObjectSnapshot>();
    const auto type = QMetaType::fromName(name.toUtf8());
    if (!type.isValid() || pointerType(type) || !type.isDefaultConstructible() || !type.hasRegisteredDataStreamOperators())
        return reject(QStringLiteral("Register the receiving object's metatype and QDataStream operators before decoding."));
    const auto encoded = payload.value("data").toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > maximumStreamBytes || decoded.decoded.toBase64() != encoded)
        return reject(QStringLiteral("Invalid Society object Base64 data."));
    QDataStream stream(decoded.decoded);
    configureStream(stream);
    QVariant value(type, nullptr);
    try {
        if (!type.load(stream, value.data()) || stream.status() != QDataStream::Ok || !stream.atEnd())
            return reject(QStringLiteral("Truncated, incompatible or trailing object stream data."));
    } catch (...) { return reject(QStringLiteral("Object deserializer threw an exception.")); }
    QString error;
    int nodes = 0;
    if (!checkValue(value, error, 0, nodes)) return reject(error);
    return value;
}
}
