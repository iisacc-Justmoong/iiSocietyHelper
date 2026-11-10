#include "LocalService.h"
#include "LocalFrame.h"
#include <iiSocietyHelper/AccountSession.h>
#include <iiSocietyHelper/Client.h>
#include <iiSocietyHelper/DiscoveryService.h>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <utility>
#include <cerrno>
#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiSocietyHelper {
namespace {
bool sameUser(QLocalSocket *socket) {
#if defined(Q_OS_DARWIN) || defined(Q_OS_FREEBSD)
    uid_t uid{}; gid_t gid{};
    return ::getpeereid(int(socket->socketDescriptor()), &uid, &gid) == 0 && uid == ::geteuid();
#elif defined(Q_OS_LINUX)
    struct ucred peer{}; socklen_t n = sizeof(peer);
    return ::getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &n) == 0 && peer.uid == ::geteuid();
#else
    (void)socket; return false;
#endif
}
bool validId(const QString &id) {
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}
}
class LocalService::Private {
public:
    struct Peer {
        QPointer<QLocalSocket> socket;
        QPointer<Client> client;
        LocalFrame input;
        QVariantMap binding;
        QString product, app;
        QHash<QString, QString> driveRequests, hostRequests;
        bool ready = false;
    };
    LocalService *owner;
    QPointer<AccountSession> account;
    QPointer<DiscoveryService> discovery;
    QHostAddress bindAddress;
    Authorizer authorize;
    QLocalServer server;
    QSet<QLocalSocket *> peers;
    QString socketPath;
    int lock = -1;
    Private(LocalService *q, AccountSession *a, Authorizer f, DiscoveryService *discoveryService, QHostAddress bind)
        : owner(q), account(a), discovery(discoveryService), bindAddress(std::move(bind)), authorize(std::move(f)) {}
    bool valid(const std::shared_ptr<Peer> &p) const {
        return p->ready && p->socket && p->socket->state() == QLocalSocket::ConnectedState && p->socket->isOpen()
            && p->client && account && account->signedIn() && authorize
            && account->manager()->matchesSessionBinding(p->binding) && authorize(p->product, p->app);
    }
    void send(const std::shared_ptr<Peer> &p, const QJsonObject &message) {
        if (!p->socket || p->socket->state() != QLocalSocket::ConnectedState || !p->socket->isOpen()) return;
        const auto bytes = LocalFrame::encode(message);
        if (bytes.isEmpty() || p->socket->bytesToWrite() + bytes.size() > 2 * (LocalFrame::MaximumBody + 4)) {
            p->socket->abort(); return;
        }
        if (p->socket->write(bytes) != bytes.size()) p->socket->abort();
    }
    void state(const std::shared_ptr<Peer> &p) {
        if (!valid(p)) { if (p->socket) p->socket->abort(); return; }
        send(p, {{"type", "state"}, {"connected", p->client->connected()}, {"binding", p->client->hostBinding()}});
    }
    void message(const std::shared_ptr<Peer> &p, const QJsonObject &in) {
        const auto op = in.value("op").toString();
        if (!p->ready) {
            const auto product = in.value("productId").toString(), app = in.value("appId").toString();
            if (op != "hello" || !account || !account->signedIn() || !authorize || !authorize(product, app)
                || in.value("deviceId").toString() != account->manager()->deviceInfo().value("id").toString()) {
                p->socket->abort(); return;
            }
            p->binding = account->manager()->sessionBinding(); p->product = product; p->app = app;
            p->client = new Client(account, discovery, bindAddress, p->socket); p->ready = true;
            QObject::connect(p->client, &Client::changed, p->socket, [this, p] { state(p); });
            QObject::connect(p->client, &Client::synchronized, p->socket, [this, p] {
                if (valid(p)) send(p, {{"type", "synchronized"}});
            });
            QObject::connect(p->client, &Client::progress, p->socket, [this, p](const QString &path, qint64 done, qint64 total) {
                if (valid(p)) send(p, {{"type", "progress"}, {"path", path}, {"done", QString::number(done)}, {"total", QString::number(total)}});
            });
            QObject::connect(p->client, &Client::hostDriveResponse, p->socket, [this, p](const QString &id, const QJsonObject &result) {
                const auto original = p->driveRequests.take(id);
                if (valid(p) && !original.isEmpty()) send(p, {{"type", "drive"}, {"id", original}, {"result", result}});
            });
            QObject::connect(p->client, &Client::hostResponse, p->socket, [this, p](const QString &id, const QJsonObject &result) {
                const auto original = p->hostRequests.take(id);
                if (valid(p) && !original.isEmpty()) send(p, {{"type", "host"}, {"id", original}, {"result", result}});
            });
            send(p, {{"type", "ready"}, {"subject", p->binding.value("subject").toString()}}); state(p); return;
        }
        if (!valid(p)) { p->socket->abort(); return; }
        if (op == "start") { const auto path = in.value("container").toString(); if (!QDir::isAbsolutePath(path) || path.size() > 4096 || path.contains(QChar::Null)) { p->socket->abort(); return; } p->client->start(path); return; }
        if (op == "connect") { p->client->connectHost(); return; }
        if (op == "stop") { p->client->stop(); p->driveRequests.clear(); p->hostRequests.clear(); return; }
        if (op == "sync") { p->client->synchronizeNow(); return; }
        const auto id = in.value("id").toString();
        if (!validId(id) || p->driveRequests.size() + p->hostRequests.size() >= 64
            || p->driveRequests.values().contains(id) || p->hostRequests.values().contains(id)) { p->socket->abort(); return; }
        QString upstream; bool host = false;
        if (op == "stat") upstream = p->client->statHostEntry(in.value("path").toString());
        else if (op == "read") {
            bool ok = false; const auto offset = in.value("offset").toString().toLongLong(&ok);
            if (!ok || offset < 0) { p->socket->abort(); return; }
            upstream = p->client->readHostFile(in.value("path").toString(), offset, in.value("version").toString());
        } else if (op == "generation") { host = true; upstream = p->client->requestHost(in.value("payload").toObject()); }
        else { p->socket->abort(); return; }
        if (upstream.isEmpty()) send(p, {{"type", host ? "host" : "drive"}, {"id", id}, {"result", QJsonObject{{"ok", false}, {"error", "transport_unavailable"}}}});
        else {
            (host ? p->hostRequests : p->driveRequests).insert(upstream, id);
            QTimer::singleShot(35000, p->socket, [this, p, upstream, host] {
                auto &pending = host ? p->hostRequests : p->driveRequests;
                const auto original = pending.take(upstream);
                if (valid(p) && !original.isEmpty()) send(p, {{"type", host ? "host" : "drive"},
                    {"id", original}, {"result", QJsonObject{{"ok", false}, {"error", "timeout"}}}});
            });
        }
    }
    void accept() {
        while (auto *socket = server.nextPendingConnection()) {
            if (peers.size() >= 16 || !sameUser(socket)) { socket->abort(); socket->deleteLater(); continue; }
            peers.insert(socket); socket->setReadBufferSize(2 * (LocalFrame::MaximumBody + 4));
            auto p = std::make_shared<Peer>(); p->socket = socket;
            auto *frameDeadline = new QTimer(socket); frameDeadline->setSingleShot(true); frameDeadline->setInterval(10000);
            QObject::connect(frameDeadline, &QTimer::timeout, socket, &QLocalSocket::abort);
            if (account) {
                QObject::connect(account, &AccountSession::sessionEnding, socket, &QLocalSocket::abort);
                QObject::connect(account, &AccountSession::changed, socket, [this, p] {
                    if (p->ready && !valid(p) && p->socket) p->socket->abort();
                });
            }
            QObject::connect(socket, &QLocalSocket::disconnected, owner, [this, socket] { peers.remove(socket); socket->deleteLater(); });
            QObject::connect(socket, &QLocalSocket::readyRead, owner, [this, p, frameDeadline] {
                if (!p->socket || !p->input.append(p->socket->readAll())) { if (p->socket) p->socket->abort(); return; }
                while (const auto in = p->input.take()) { message(p, *in); if (!p->socket || p->socket->state() == QLocalSocket::UnconnectedState) return; }
                if (p->input.invalid() && p->socket) p->socket->abort();
                else if (p->input.pending() && !frameDeadline->isActive()) frameDeadline->start();
                else if (!p->input.pending()) frameDeadline->stop();
            });
            QTimer::singleShot(10000, socket, [p] { if (p->socket && !p->ready) p->socket->abort(); });
            auto *timer = new QTimer(socket); timer->setInterval(5000);
            QObject::connect(timer, &QTimer::timeout, socket, [this, p] { if (p->ready && !valid(p) && p->socket) p->socket->abort(); }); timer->start();
        }
    }
};
LocalService::LocalService(AccountSession *account, Authorizer authorize, DiscoveryService *discovery, QObject *parent)
    : LocalService(account, std::move(authorize), discovery, QHostAddress::AnyIPv4, parent) {}
LocalService::LocalService(AccountSession *account, Authorizer authorize, DiscoveryService *discovery, QHostAddress bindAddress, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, account, std::move(authorize), discovery, std::move(bindAddress))) {
    connect(&d->server, &QLocalServer::newConnection, this, [this] { d->accept(); });
}
LocalService::~LocalService() { stop(); }
bool LocalService::listen(const QString &directory) {
#if defined(Q_OS_UNIX)
    if (d->server.isListening()) return true;
    if (!QDir::isAbsolutePath(directory) || !d->account || !d->authorize) return false;
    const auto root = QFile::encodeName(directory); struct stat st{};
    if (::lstat(root.constData(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) return false;
    const auto lockName = QFile::encodeName(directory + "/helper.lock");
    const int fd = ::open(lockName.constData(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077) || ::flock(fd, LOCK_EX | LOCK_NB)) { ::close(fd); return false; }
    const auto path = directory + "/helper.sock"; const auto encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) { ::close(fd); return false; }
        QLocalSocket probe; probe.connectToServer(path);
        if (probe.waitForConnected(100) || ::unlink(encoded.constData())) { ::close(fd); return false; }
    } else if (errno != ENOENT) { ::close(fd); return false; }
    d->server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!d->server.listen(path) || ::lstat(encoded.constData(), &st) || !S_ISSOCK(st.st_mode)
        || st.st_uid != ::geteuid() || ::chmod(encoded.constData(), 0600)) { d->server.close(); ::close(fd); return false; }
    d->lock = fd; d->socketPath = path;
    return true;
#else
    (void)directory; return false;
#endif
}
void LocalService::stop() {
    const auto peers = d->peers;
    for (auto *peer : peers) peer->abort();
    d->server.close(); d->socketPath.clear();
#if defined(Q_OS_UNIX)
    if (d->lock >= 0) { ::close(d->lock); d->lock = -1; }
#endif
}
QString LocalService::path() const { return d->socketPath; }
}
