#include "DeliveryStore.h"
#include "LocalDirectory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace iiSocietyHelper {
#ifdef Q_OS_IOS
bool applePrepareDeliveryDirectory(const QString &path, QString *error);
#endif
namespace {
QByteArray json(const QVariantMap &map) { return QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact); }
QVariantMap map(const QVariant &value) { return QJsonDocument::fromJson(value.toByteArray()).object().toVariantMap(); }
bool validName(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return expression.match(value).hasMatch();
}
}

class DeliveryStore::Private {
public:
    QSqlDatabase database;
    QString connection;
    QString root;
    mutable QString error;
    bool fail(const QString &message) const { error = message; return false; }
    bool ready() const {
        error.clear();
        if (!database.isOpen()) return fail(QStringLiteral("Society delivery storage is not open."));
        const QFileInfo directory(root);
        if (!directory.isDir() || directory.isSymLink() || directory.canonicalFilePath() != root)
            return fail(QStringLiteral("The Society delivery directory disappeared or was redirected."));
        if (!local::validateDirectory(root, &error)) return false;
        for (const auto *name : {"delivery.sqlite", "delivery.sqlite-wal", "delivery.sqlite-shm"})
            if (QFileInfo(QDir(root).filePath(QString::fromLatin1(name))).isSymLink())
                return fail(QStringLiteral("Society delivery files cannot be symbolic links."));
        return true;
    }
    bool exec(const QString &sql) const {
        QSqlQuery query(database);
        return query.exec(sql) || fail(query.lastError().text());
    }
};

DeliveryStore::DeliveryStore() : d(std::make_unique<Private>()) {}
DeliveryStore::~DeliveryStore() { close(); }
void DeliveryStore::close()
{
    if (d->database.isValid()) d->database.close();
    d->database = QSqlDatabase();
    if (!d->connection.isEmpty()) QSqlDatabase::removeDatabase(d->connection);
    d->connection.clear();
}
bool DeliveryStore::isOpen() const { return d->database.isOpen(); }
QString DeliveryStore::errorString() const { return d->error; }

bool DeliveryStore::open(const QString &observationDirectory, QString *error)
{
    close();
    d->error.clear();
    if (error) error->clear();
    const auto fail = [&](const QString &message) {
        close();
        if (error) *error = message;
        return d->fail(message);
    };
    if (!QDir::isAbsolutePath(observationDirectory) || QDir(observationDirectory).isRoot()
        || QFileInfo(observationDirectory).isSymLink())
        return fail(QStringLiteral("Provide a local absolute Society observation directory."));
    QString boundaryError;
    if (!local::validateDirectory(observationDirectory, &boundaryError)) return fail(boundaryError);
    const auto directory = QDir(observationDirectory).filePath("delivery");
    const bool existed = QFileInfo::exists(directory);
    if (QFileInfo(directory).isSymLink() || !QDir().mkpath(directory))
        return fail(QStringLiteral("Cannot create the Society delivery directory."));
    if (!existed && !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return fail(QStringLiteral("Cannot restrict Society delivery storage to its owner."));
    d->root = QFileInfo(directory).canonicalFilePath();
#ifdef Q_OS_IOS
    QString protectionError;
    if (!applePrepareDeliveryDirectory(d->root, &protectionError)) return fail(protectionError);
#endif
    // Changing journal mode/schema concurrently can return SQLITE_BUSY immediately,
    // even with SQLite's busy timeout. Serialize initialization, not normal delivery.
    const auto initializationPath = QDir(d->root).filePath("initialization.lock");
    if (QFileInfo(initializationPath).isSymLink()) return fail(QStringLiteral("Invalid Society delivery initialization lock."));
    QLockFile initialization(initializationPath);
    initialization.setStaleLockTime(0);
    if (!initialization.tryLock(3000)) return fail(QStringLiteral("Society delivery storage is being initialized by another app."));
    for (const auto *name : {"delivery.sqlite", "delivery.sqlite-wal", "delivery.sqlite-shm"})
        if (QFileInfo(QDir(d->root).filePath(QString::fromLatin1(name))).isSymLink())
            return fail(QStringLiteral("Society delivery files cannot be symbolic links."));
    d->connection = "society-delivery-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->database = QSqlDatabase::addDatabase("QSQLITE", d->connection);
    d->database.setDatabaseName(QDir(d->root).filePath("delivery.sqlite"));
    d->database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    if (!d->database.open()) return fail(d->database.lastError().text());
    if (!QFile::setPermissions(d->database.databaseName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(QStringLiteral("Cannot restrict the Society delivery database to its owner."));
    int schema = -1;
    {
        QSqlQuery query(d->database);
        if (query.exec("PRAGMA user_version") && query.next()) schema = query.value(0).toInt();
    }
    if (schema != 0 && schema != 1) return fail(QStringLiteral("Unsupported Society delivery database schema."));
    QString journalMode;
    {
        QSqlQuery query(d->database);
        if (query.exec("PRAGMA journal_mode") && query.next()) journalMode = query.value(0).toString();
    }
    if (journalMode != "wal" && !d->exec("PRAGMA journal_mode=WAL")) return fail(d->error);
    if (!d->exec("PRAGMA synchronous=FULL")) return fail(d->error);
    if (schema == 0) {
        if (!d->exec("BEGIN IMMEDIATE")) return fail(d->error);
        const QStringList statements{
            "CREATE TABLE IF NOT EXISTS outbox (id TEXT PRIMARY KEY, sender BLOB NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, queued_at TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS inbox (sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, sender BLOB NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, queued_at TEXT NOT NULL, received_at TEXT NOT NULL)",
            "CREATE TABLE IF NOT EXISTS consumers (id TEXT PRIMARY KEY, sequence INTEGER NOT NULL)",
            "CREATE TABLE IF NOT EXISTS daemon_state (id INTEGER PRIMARY KEY CHECK(id=1), snapshot BLOB NOT NULL)",
            "PRAGMA user_version=1"};
        for (const auto &statement : statements) if (!d->exec(statement)) {
            const auto message = d->error;
            d->database.rollback();
            return fail(message);
        }
        if (!d->database.commit()) return fail(d->database.lastError().text());
    }
    return true;
}

QString DeliveryStore::enqueue(const Peer &sender, const QString &topic, const QVariantMap &payload)
{
    if (!d->ready()) return {};
    const auto bytes = json(payload);
    if (!validName(topic) || sender.application.id.isEmpty() || QUuid(sender.instanceId).isNull() || bytes.size() > 65536) {
        d->fail(QStringLiteral("Provide a valid topic, sender instance and JSON object no larger than 64 KiB."));
        return {};
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery query(d->database);
    query.prepare("INSERT INTO outbox (id,sender,topic,payload,queued_at) VALUES (?,?,?,?,?)");
    query.addBindValue(id);
    query.addBindValue(json(sender.toVariantMap()));
    query.addBindValue(topic);
    query.addBindValue(bytes);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) { d->fail(query.lastError().text()); return {}; }
    return id;
}

int DeliveryStore::receivePending(int limit)
{
    if (!d->ready()) return -1;
    if (limit < 1 || limit > 256) { d->fail(QStringLiteral("Receipt batch size must be 1–256.")); return -1; }
    if (!d->exec("BEGIN IMMEDIATE")) return -1;
    const auto rollback = [&](const QString &error) { d->database.rollback(); d->fail(error); return -1; };
    QList<QVariantList> rows;
    {
        QSqlQuery query(d->database);
        query.prepare("SELECT id,sender,topic,payload,queued_at FROM outbox ORDER BY rowid LIMIT ?");
        query.addBindValue(limit);
        if (!query.exec()) return rollback(query.lastError().text());
        while (query.next()) rows.append({query.value(0), query.value(1), query.value(2), query.value(3), query.value(4)});
    }
    for (const auto &row : rows) {
        QSqlQuery insert(d->database);
        insert.prepare("INSERT OR IGNORE INTO inbox (id,sender,topic,payload,queued_at,received_at) VALUES (?,?,?,?,?,?)");
        for (const auto &value : row) insert.addBindValue(value);
        insert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!insert.exec()) return rollback(insert.lastError().text());
        QSqlQuery remove(d->database);
        remove.prepare("DELETE FROM outbox WHERE id=?");
        remove.addBindValue(row[0]);
        if (!remove.exec()) return rollback(remove.lastError().text());
    }
    if (!d->database.commit()) return rollback(d->database.lastError().text());
    return int(rows.size());
}

qint64 DeliveryStore::pendingCount() const
{
    if (!d->ready()) return -1;
    QSqlQuery query(d->database);
    if (!query.exec("SELECT COUNT(*) FROM outbox") || !query.next()) { d->fail(query.lastError().text()); return -1; }
    return query.value(0).toLongLong();
}

QVariantList DeliveryStore::readAfter(qint64 sequence, int limit) const
{
    if (!d->ready()) return {};
    if (sequence < 0 || limit < 1 || limit > 256) { d->fail(QStringLiteral("Invalid Society inbox cursor or page size.")); return {}; }
    QSqlQuery query(d->database);
    query.prepare("SELECT sequence,id,sender,topic,payload,queued_at,received_at FROM inbox WHERE sequence>? ORDER BY sequence LIMIT ?");
    query.addBindValue(sequence);
    query.addBindValue(limit);
    if (!query.exec()) { d->fail(query.lastError().text()); return {}; }
    QVariantList result;
    while (query.next()) result.append(QVariantMap{
        {"sequence", query.value(0)}, {"id", query.value(1)}, {"sender", map(query.value(2))}, {"topic", query.value(3)},
        {"payload", map(query.value(4))}, {"queuedAt", query.value(5)}, {"receivedAt", query.value(6)}});
    return result;
}

bool DeliveryStore::acknowledge(const QString &consumer, qint64 sequence)
{
    if (!d->ready()) return false;
    if (!validName(consumer) || sequence < 0) return d->fail(QStringLiteral("Invalid Society consumer or acknowledgement."));
    QSqlQuery query(d->database);
    if (!query.exec("SELECT COALESCE(MAX(sequence),0) FROM inbox") || !query.next()) return d->fail(query.lastError().text());
    if (sequence > query.value(0).toLongLong()) return d->fail(QStringLiteral("Cannot acknowledge data the daemon has not received."));
    query.finish();
    query.prepare("INSERT INTO consumers (id,sequence) VALUES (?,?) ON CONFLICT(id) DO UPDATE SET sequence=MAX(consumers.sequence,excluded.sequence)");
    query.addBindValue(consumer);
    query.addBindValue(sequence);
    return query.exec() || d->fail(query.lastError().text());
}

qint64 DeliveryStore::acknowledged(const QString &consumer) const
{
    if (!d->ready()) return -1;
    QSqlQuery query(d->database);
    query.prepare("SELECT sequence FROM consumers WHERE id=?");
    query.addBindValue(consumer);
    if (!query.exec()) { d->fail(query.lastError().text()); return -1; }
    return query.next() ? query.value(0).toLongLong() : 0;
}

bool DeliveryStore::setDaemonSnapshot(const QVariantMap &snapshot)
{
    if (!d->ready()) return false;
    QSqlQuery query(d->database);
    query.prepare("INSERT INTO daemon_state (id,snapshot) VALUES (1,?) ON CONFLICT(id) DO UPDATE SET snapshot=excluded.snapshot");
    query.addBindValue(json(snapshot));
    return query.exec() || d->fail(query.lastError().text());
}

QVariantMap DeliveryStore::daemonSnapshot() const
{
    if (!d->ready()) return {};
    QSqlQuery query(d->database);
    if (!query.exec("SELECT snapshot FROM daemon_state WHERE id=1")) { d->fail(query.lastError().text()); return {}; }
    return query.next() ? map(query.value(0)) : QVariantMap{};
}
}
