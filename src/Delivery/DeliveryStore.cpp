#include <File.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderDevice.h>
#include "DeliveryStore.h"
#include "LocalDirectory.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace iiSocietyHelper {
#ifdef Q_OS_IOS
bool applePrepareDeliveryDirectory(const QString &path, QString *error);
#endif
namespace {
namespace sql = iiFileProvider::native::sql;

struct DatabaseFailure { QString message; };
QString message(const sql::Error &error) {
    return QString::fromUtf8(error.message.data(), qsizetype(error.message.size()));
}
template<class T> T require(sql::Result<T> result) {
    if (!result) throw DatabaseFailure{message(result.error())};
    return std::move(*result);
}
void require(sql::Result<void> result) {
    if (!result) throw DatabaseFailure{message(result.error())};
}
std::string text(const QString &value) {
    const auto utf8 = value.toUtf8();
    return std::string(utf8.constData(), std::size_t(utf8.size()));
}
sql::Blob blob(const QByteArray &bytes) {
    const auto *first = reinterpret_cast<const std::uint8_t *>(bytes.constData());
    return sql::Blob(first, first + bytes.size());
}
QVariant variant(const sql::Value &value) {
    return std::visit([](const auto &item) -> QVariant {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return {};
        else if constexpr (std::is_same_v<T, std::int64_t>) return QVariant::fromValue(qlonglong(item));
        else if constexpr (std::is_same_v<T, double>) return item;
        else if constexpr (std::is_same_v<T, std::string>) return QString::fromUtf8(item.data(), qsizetype(item.size()));
        else return QByteArray(reinterpret_cast<const char *>(item.data()), qsizetype(item.size()));
    }, value);
}
QByteArray json(const QVariantMap &map) { return QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact); }
QVariantMap map(const sql::Value &value) { return QJsonDocument::fromJson(variant(value).toByteArray()).object().toVariantMap(); }
bool validName(const QString &value) {
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return expression.match(value).hasMatch();
}
sql::Statement statement(sql::Database &database, std::string_view source,
                         std::initializer_list<sql::Value> values = {}) {
    auto query = require(database.prepare(source));
    int index = 1;
    for (const auto &value : values) require(query.bind(index++, value));
    return query;
}
void finish(sql::Statement &query) {
    if (require(query.step()) != sql::Step::done)
        throw DatabaseFailure{QStringLiteral("Unexpected row from a Society delivery update.")};
}
}

class DeliveryStore::Private {
public:
    std::optional<sql::Database> database;
    QPointer<QThread> owner;
    QString root;
    mutable QString error;
    bool fail(const QString &value) const { error = value; return false; }
    bool ready() const {
        error.clear();
        if (!database) return fail(QStringLiteral("Society delivery storage is not open."));
        if (owner != QThread::currentThread()) return fail(QStringLiteral("Society delivery storage belongs to another thread."));
        const QFileInfo directory(root);
        if (!directory.isDir() || directory.isSymLink() || directory.canonicalFilePath() != root)
            return fail(QStringLiteral("The Society delivery directory disappeared or was redirected."));
        if (!local::validateDirectory(root, &error)) return false;
        for (const auto *name : {"delivery.sqlite", "delivery.sqlite-wal", "delivery.sqlite-shm"})
            if (QFileInfo(iiFileProvider::Directory(root).filePath(QString::fromLatin1(name))).isSymLink())
                return fail(QStringLiteral("Society delivery files cannot be symbolic links."));
        return true;
    }
    bool exec(std::string_view source) const {
        auto result = database->execute(source);
        return result ? true : fail(message(result.error()));
    }
};

DeliveryStore::DeliveryStore() : d(std::make_unique<Private>()) {}
DeliveryStore::~DeliveryStore() { close(); }
void DeliveryStore::close() { d->database.reset(); d->owner.clear(); }
bool DeliveryStore::isOpen() const { return d->database.has_value(); }
bool DeliveryStore::moveToThread(QThread *thread) {
    if (!d->database) return true;
    if (d->owner != QThread::currentThread()) return false;
    d->owner = thread;
    return true;
}
void DeliveryStore::swap(DeliveryStore &other) noexcept { d.swap(other.d); }
QString DeliveryStore::errorString() const { return d->error; }

bool DeliveryStore::open(const QString &observationDirectory, QString *error) {
    close();
    d->error.clear();
    if (error) error->clear();
    const auto fail = [&](const QString &value) {
        close();
        if (error) *error = value;
        return d->fail(value);
    };
    if (!iiFileProvider::Directory::isAbsolutePath(observationDirectory) || iiFileProvider::Directory(observationDirectory).isRoot()
        || QFileInfo(observationDirectory).isSymLink())
        return fail(QStringLiteral("Provide a local absolute Society observation directory."));
    QString boundaryError;
    if (!local::validateDirectory(observationDirectory, &boundaryError)) return fail(boundaryError);
    const auto directory = iiFileProvider::Directory(observationDirectory).filePath("delivery");
    const bool existed = QFileInfo::exists(directory);
    if (QFileInfo(directory).isSymLink() || !iiFileProvider::Directory().mkpath(directory))
        return fail(QStringLiteral("Cannot create the Society delivery directory."));
    if (!existed && !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return fail(QStringLiteral("Cannot restrict Society delivery storage to its owner."));
    d->root = QFileInfo(directory).canonicalFilePath();
#ifdef Q_OS_IOS
    QString protectionError;
    if (!applePrepareDeliveryDirectory(d->root, &protectionError)) return fail(protectionError);
#endif
    const auto initializationPath = iiFileProvider::Directory(d->root).filePath("initialization.lock");
    if (QFileInfo(initializationPath).isSymLink()) return fail(QStringLiteral("Invalid Society delivery initialization lock."));
    const auto validFiles = [&] {
        for (const auto *name : {"delivery.sqlite", "delivery.sqlite-wal", "delivery.sqlite-shm"})
            if (QFileInfo(iiFileProvider::Directory(d->root).filePath(QString::fromLatin1(name))).isSymLink()) return false;
        return true;
    };
    if (!validFiles()) return fail(QStringLiteral("Society delivery files cannot be symbolic links."));
    const auto databaseName = iiFileProvider::Directory(d->root).filePath("delivery.sqlite");
    const auto openDatabase = [&] {
        sql::Options options;
        options.mode = sql::OpenMode::read_write;
        options.busy_timeout_ms = 1000;
        options.maximum_value_bytes = std::numeric_limits<int>::max();
        try {
            auto result = sql::Database::open(std::filesystem::path(databaseName.toStdU16String()), options);
            if (!result) return d->fail(message(result.error()));
            d->database.emplace(std::move(*result));
            d->error.clear();
            d->owner = QThread::currentThread();
            return true;
        } catch (const std::filesystem::filesystem_error &failure) {
            return d->fail(QString::fromUtf8(failure.what()));
        }
    };
    const auto schemaVersion = [&] {
        try {
            auto query = statement(*d->database, "PRAGMA user_version");
            return require(query.step()) == sql::Step::row ? variant(require(query.value(0))).toInt() : -1;
        } catch (const DatabaseFailure &) { return -1; }
    };
    const auto journal = [&] {
        try {
            auto query = statement(*d->database, "PRAGMA journal_mode");
            return require(query.step()) == sql::Step::row ? variant(require(query.value(0))).toString() : QString();
        } catch (const DatabaseFailure &) { return QString(); }
    };
    const auto configureConnection = [&] {
        if (!QFile::setPermissions(databaseName, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return fail(QStringLiteral("Cannot restrict the Society delivery database to its owner."));
        return d->exec("PRAGMA synchronous=FULL") || fail(d->error);
    };
    // A committed version-one WAL store does not need the initialization lease.
    const QFileInfo existing(databaseName);
    if (existing.isFile() && existing.size() > 0 && openDatabase()) {
        const int schema = schemaVersion();
        if (schema > 1) return fail(QStringLiteral("Unsupported Society delivery database schema."));
        if (schema == 1 && journal() == "wal") return configureConnection();
        close();
    }
    // Schema creation and journal transitions are serialized across processes.
    iiFileProvider::LockFile initialization(initializationPath);
    initialization.setStaleLockTime(0);
    if (!initialization.tryLock(3000)) return fail(QStringLiteral("Society delivery storage is being initialized by another app."));
    if (QFileInfo(initializationPath).isSymLink() || !validFiles())
        return fail(QStringLiteral("Society delivery files cannot be symbolic links."));
    if (!iiFileProvider::File::prepareDatabase(databaseName)) return fail(QStringLiteral("Cannot create delivery database."));
    if (!openDatabase()) return fail(d->error);
    const int schema = schemaVersion();
    if (schema != 0 && schema != 1) return fail(QStringLiteral("Unsupported Society delivery database schema."));
    if (journal() != "wal" && !d->exec("PRAGMA journal_mode=WAL")) return fail(d->error);
    if (!configureConnection()) return false;
    if (schema == 0) {
        try {
            auto transaction = require(sql::Transaction::begin(*d->database));
            for (const auto *source : {
                     "CREATE TABLE IF NOT EXISTS outbox (id TEXT PRIMARY KEY, sender BLOB NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, queued_at TEXT NOT NULL)",
                     "CREATE TABLE IF NOT EXISTS inbox (sequence INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE, sender BLOB NOT NULL, topic TEXT NOT NULL, payload BLOB NOT NULL, queued_at TEXT NOT NULL, received_at TEXT NOT NULL)",
                     "CREATE TABLE IF NOT EXISTS consumers (id TEXT PRIMARY KEY, sequence INTEGER NOT NULL)",
                     "CREATE TABLE IF NOT EXISTS daemon_state (id INTEGER PRIMARY KEY CHECK(id=1), snapshot BLOB NOT NULL)",
                     "PRAGMA user_version=1"})
                require(d->database->execute(source));
            require(transaction.commit());
        } catch (const DatabaseFailure &failure) { return fail(failure.message); }
    }
    return true;
}

QString DeliveryStore::enqueue(const Peer &sender, const QString &topic, const QVariantMap &payload) {
    if (!d->ready()) return {};
    const auto bytes = json(payload);
    if (!validName(topic) || sender.application.id.isEmpty() || QUuid(sender.instanceId).isNull() || bytes.size() > 65536) {
        d->fail(QStringLiteral("Provide a valid topic, sender instance and JSON object no larger than 64 KiB."));
        return {};
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    try {
        auto query = statement(*d->database, "INSERT INTO outbox (id,sender,topic,payload,queued_at) VALUES (?,?,?,?,?)",
            {text(id), blob(json(sender.toVariantMap())), text(topic), blob(bytes), text(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))});
        finish(query);
        return id;
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return {}; }
}

int DeliveryStore::receivePending(int limit) {
    if (!d->ready()) return -1;
    if (limit < 1 || limit > 256) { d->fail(QStringLiteral("Receipt batch size must be 1–256.")); return -1; }
    try {
        auto transaction = require(sql::Transaction::begin(*d->database));
        std::vector<std::array<sql::Value, 5>> rows;
        {
            auto query = statement(*d->database, "SELECT id,sender,topic,payload,queued_at FROM outbox ORDER BY rowid LIMIT ?", {std::int64_t(limit)});
            while (require(query.step()) == sql::Step::row) {
                std::array<sql::Value, 5> row;
                for (int i = 0; i < 5; ++i) row[std::size_t(i)] = require(query.value(i));
                rows.push_back(std::move(row));
            }
        }
        for (const auto &row : rows) {
            auto insert = statement(*d->database, "INSERT OR IGNORE INTO inbox (id,sender,topic,payload,queued_at,received_at) VALUES (?,?,?,?,?,?)");
            int index = 1;
            for (const auto &value : row) require(insert.bind(index++, value));
            require(insert.bind(index, text(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))));
            finish(insert);
            auto remove = statement(*d->database, "DELETE FROM outbox WHERE id=?", {row[0]});
            finish(remove);
        }
        require(transaction.commit());
        return int(rows.size());
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return -1; }
}

qint64 DeliveryStore::pendingCount() const {
    if (!d->ready()) return -1;
    try {
        auto query = statement(*d->database, "SELECT COUNT(*) FROM outbox");
        if (require(query.step()) != sql::Step::row) throw DatabaseFailure{QStringLiteral("Cannot count the Society outbox.")};
        return variant(require(query.value(0))).toLongLong();
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return -1; }
}
QVariantList DeliveryStore::readAfter(qint64 sequence, int limit) const {
    if (!d->ready()) return {};
    if (sequence < 0 || limit < 1 || limit > 256) { d->fail(QStringLiteral("Invalid Society inbox cursor or page size.")); return {}; }
    try {
        auto query = statement(*d->database, "SELECT sequence,id,sender,topic,payload,queued_at,received_at FROM inbox WHERE sequence>? ORDER BY sequence LIMIT ?", {std::int64_t(sequence), std::int64_t(limit)});
        QVariantList result;
        while (require(query.step()) == sql::Step::row) result.append(QVariantMap{
            {"sequence", variant(require(query.value(0)))}, {"id", variant(require(query.value(1)))}, {"sender", map(require(query.value(2)))},
            {"topic", variant(require(query.value(3)))}, {"payload", map(require(query.value(4)))},
            {"queuedAt", variant(require(query.value(5)))}, {"receivedAt", variant(require(query.value(6)))}});
        return result;
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return {}; }
}
bool DeliveryStore::acknowledge(const QString &consumer, qint64 sequence) {
    if (!d->ready()) return false;
    if (!validName(consumer) || sequence < 0) return d->fail(QStringLiteral("Invalid Society consumer or acknowledgement."));
    try {
        qint64 maximum;
        {
            auto query = statement(*d->database, "SELECT COALESCE(MAX(sequence),0) FROM inbox");
            if (require(query.step()) != sql::Step::row) throw DatabaseFailure{QStringLiteral("Cannot read the Society inbox cursor.")};
            maximum = variant(require(query.value(0))).toLongLong();
        }
        if (sequence > maximum) return d->fail(QStringLiteral("Cannot acknowledge data the daemon has not received."));
        auto query = statement(*d->database, "INSERT INTO consumers (id,sequence) VALUES (?,?) ON CONFLICT(id) DO UPDATE SET sequence=MAX(consumers.sequence,excluded.sequence)", {text(consumer), std::int64_t(sequence)});
        finish(query);
        return true;
    } catch (const DatabaseFailure &failure) { return d->fail(failure.message); }
}
qint64 DeliveryStore::acknowledged(const QString &consumer) const {
    if (!d->ready()) return -1;
    try {
        auto query = statement(*d->database, "SELECT sequence FROM consumers WHERE id=?", {text(consumer)});
        return require(query.step()) == sql::Step::row ? variant(require(query.value(0))).toLongLong() : 0;
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return -1; }
}
bool DeliveryStore::setDaemonSnapshot(const QVariantMap &snapshot) {
    if (!d->ready()) return false;
    try {
        auto query = statement(*d->database, "INSERT INTO daemon_state (id,snapshot) VALUES (1,?) ON CONFLICT(id) DO UPDATE SET snapshot=excluded.snapshot", {blob(json(snapshot))});
        finish(query);
        return true;
    } catch (const DatabaseFailure &failure) { return d->fail(failure.message); }
}
QVariantMap DeliveryStore::daemonSnapshot() const {
    if (!d->ready()) return {};
    try {
        auto query = statement(*d->database, "SELECT snapshot FROM daemon_state WHERE id=1");
        return require(query.step()) == sql::Step::row ? map(require(query.value(0))) : QVariantMap{};
    } catch (const DatabaseFailure &failure) { d->fail(failure.message); return {}; }
}
}
