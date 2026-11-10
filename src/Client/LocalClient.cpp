#include "LocalClient.h"
#include "LocalFrame.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#if defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiSocietyHelper {
namespace {
bool trustedPath(const QString &path) {
#if defined(Q_OS_UNIX)
    if (!QDir::isAbsolutePath(path)) return false;
    struct stat dir{}, sock{};
    const auto parent = QFile::encodeName(QFileInfo(path).absolutePath());
    const auto endpoint = QFile::encodeName(path);
    return !::lstat(parent.constData(), &dir) && S_ISDIR(dir.st_mode) && dir.st_uid == ::geteuid() && !(dir.st_mode & 0077)
        && !::lstat(endpoint.constData(), &sock) && S_ISSOCK(sock.st_mode) && sock.st_uid == ::geteuid() && !(sock.st_mode & 0077);
#else
    (void)path; return false;
#endif
}
bool sameUser(QLocalSocket &socket) {
#if defined(Q_OS_DARWIN) || defined(Q_OS_FREEBSD)
    uid_t uid{}; gid_t gid{};
    return ::getpeereid(int(socket.socketDescriptor()), &uid, &gid) == 0 && uid == ::geteuid();
#elif defined(Q_OS_LINUX)
    struct ucred peer{}; socklen_t n = sizeof(peer);
    return ::getsockopt(int(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &n) == 0 && peer.uid == ::geteuid();
#else
    (void)socket; return false;
#endif
}
QString defaultPath() {
    auto directory = qEnvironmentVariable("SOCIETY_LOCAL_BROKER_DIR");
    if (directory.isEmpty()) directory = QDir::homePath() + "/.iisacc-society-ipc-v1";
    return directory + "/helper.sock";
}
}
class LocalClient::Private {
public:
    LocalClient *q;
    QLocalSocket socket;
    QTimer retry, deadline, frameDeadline;
    LocalFrame input;
    QString product, app, device, path, mode, container, expectedSubject;
    QJsonObject binding;
    QSet<QString> driveRequests, hostRequests;
    bool active = false, ready = false, online = false;
    Private(LocalClient *owner, QString p, QString a, QString id, QString endpoint)
        : q(owner), product(std::move(p)), app(std::move(a)), device(std::move(id)), path(endpoint.isEmpty() ? defaultPath() : std::move(endpoint)) {
        socket.setReadBufferSize(2 * (LocalFrame::MaximumBody + 4));
        retry.setSingleShot(true); retry.setInterval(2000); deadline.setSingleShot(true); deadline.setInterval(10000);
        frameDeadline.setSingleShot(true); frameDeadline.setInterval(10000);
        QObject::connect(&retry, &QTimer::timeout, q, [this] { open(); });
        QObject::connect(&deadline, &QTimer::timeout, q, [this] { socket.abort(); });
        QObject::connect(&frameDeadline, &QTimer::timeout, q, [this] { socket.abort(); });
        QObject::connect(&socket, &QLocalSocket::connected, q, [this] {
            if (!sameUser(socket) || !trustedPath(path)) { socket.abort(); return; }
            send({{"op", "hello"}, {"productId", product}, {"appId", app}, {"deviceId", device}});
        });
        QObject::connect(&socket, &QLocalSocket::disconnected, q, [this] { disconnected(); });
        QObject::connect(&socket, &QLocalSocket::errorOccurred, q, [this](QLocalSocket::LocalSocketError) {
            if (socket.state() == QLocalSocket::UnconnectedState) disconnected();
        });
        QObject::connect(&socket, &QLocalSocket::readyRead, q, [this] {
            const QPointer<LocalClient> guard(q);
            if (!input.append(socket.readAll())) { socket.abort(); return; }
            while (const auto message = input.take()) {
                receive(*message);
                if (!guard || socket.state() != QLocalSocket::ConnectedState) return;
            }
            if (input.invalid()) socket.abort();
            else if (input.pending() && !frameDeadline.isActive()) frameDeadline.start();
            else if (!input.pending()) frameDeadline.stop();
        });
    }
    bool send(const QJsonObject &message) {
        const auto data = LocalFrame::encode(message);
        if (socket.state() != QLocalSocket::ConnectedState || !socket.isOpen() || data.isEmpty()
            || socket.bytesToWrite() + data.size() > 2 * (LocalFrame::MaximumBody + 4)) { socket.abort(); return false; }
        if (socket.write(data) != data.size()) { socket.abort(); return false; }
        return true;
    }
    void open() {
        if (!active || expectedSubject.isEmpty() || socket.state() != QLocalSocket::UnconnectedState) return;
        if (!trustedPath(path)) { retry.start(); return; }
        input = LocalFrame{}; socket.connectToServer(path); deadline.start();
    }
    void disconnected() {
        deadline.stop(); frameDeadline.stop(); const bool wasReady = ready, notify = online || ready;
        ready = online = false; binding = {}; driveRequests.clear(); hostRequests.clear();
        QPointer<LocalClient> guard(q);
        if (wasReady) emit q->sessionChanged(false);
        if (!guard) return;
        if (notify) emit q->changed();
        if (!guard) return;
        if (active && !retry.isActive()) retry.start();
    }
    void receive(const QJsonObject &message) {
        const auto type = message.value("type").toString();
        if (!ready) {
            if (type != "ready" || expectedSubject.isEmpty() || message.value("subject").toString() != expectedSubject) { socket.abort(); return; }
            ready = true; deadline.stop();
            QPointer<LocalClient> guard(q); emit q->sessionChanged(true); if (!guard) return;
            if (mode == "start") send({{"op", "start"}, {"container", container}});
            else if (mode == "connect") send({{"op", "connect"}});
            return;
        }
        if (type == "state") {
            online = message.value("connected").toBool(); binding = online ? message.value("binding").toObject() : QJsonObject{};
            emit q->changed();
        } else if (type == "synchronized") emit q->synchronized();
        else if (type == "progress") {
            bool ok1 = false, ok2 = false;
            const auto done = message.value("done").toString().toLongLong(&ok1), total = message.value("total").toString().toLongLong(&ok2);
            if (!ok1 || !ok2 || done < 0 || total < 0) { socket.abort(); return; }
            emit q->progress(message.value("path").toString(), done, total);
        } else if (type == "drive" || type == "host") {
            const auto id = message.value("id").toString(); auto &pending = type == "drive" ? driveRequests : hostRequests;
            if (!pending.remove(id) || !message.value("result").isObject()) { socket.abort(); return; }
            if (type == "drive") emit q->hostDriveResponse(id, message.value("result").toObject());
            else emit q->hostResponse(id, message.value("result").toObject());
        } else socket.abort();
    }
    bool begin(QString requestedMode, QString root = {}) {
        if (active && mode == requestedMode && container == root) return true;
        const QPointer<LocalClient> guard(q);
        active = false; retry.stop(); deadline.stop(); socket.abort(); if (!guard) return false;
        disconnected(); if (!guard) return false;
        mode = std::move(requestedMode); container = std::move(root); active = true; open(); return true;
    }
    QString request(const QString &op, QJsonObject input, bool host) {
        if (!active || !ready || !online || driveRequests.size() + hostRequests.size() >= 64) return {};
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        input.insert("op", op); input.insert("id", id);
        auto &pending = host ? hostRequests : driveRequests; pending.insert(id);
        const QPointer<LocalClient> guard(q);
        if (!send(input)) { if (guard) pending.remove(id); return {}; }
        return id;
    }
};
LocalClient::LocalClient(QString product, QString app, QString device, QString socketPath, QObject *parent)
    : FileHost(parent), d(std::make_unique<Private>(this, std::move(product), std::move(app), std::move(device), std::move(socketPath))) {}
LocalClient::~LocalClient() {
    QObject::disconnect(&d->socket, nullptr, this, nullptr);
    d->active = false; d->retry.stop(); d->deadline.stop(); d->frameDeadline.stop(); d->socket.abort();
}
bool LocalClient::start(const QString &container) { return !container.isEmpty() && d->begin("start", container); }
bool LocalClient::observe() { return d->begin("observe"); }
bool LocalClient::sessionBound() const { return d->active && d->ready && d->socket.state() == QLocalSocket::ConnectedState; }
bool LocalClient::connectHost() { return d->begin("connect"); }
void LocalClient::stop() {
    const QPointer<LocalClient> guard(this); d->active = false; d->retry.stop(); d->deadline.stop(); d->socket.abort();
    if (guard) d->disconnected();
}
void LocalClient::synchronizeNow() { if (d->ready) d->send({{"op", "sync"}}); }
void LocalClient::setExpectedSubject(const QString &subject) {
    if (d->expectedSubject == subject) return;
    const QPointer<LocalClient> guard(this);
    d->expectedSubject = subject; d->retry.stop(); d->socket.abort(); if (!guard) return;
    d->disconnected(); if (!guard) return;
    if (d->active && !subject.isEmpty()) d->open();
}
bool LocalClient::connected() const { return d->active && d->ready && d->online; }
QJsonObject LocalClient::hostBinding() const { return connected() ? d->binding : QJsonObject{}; }
QString LocalClient::statHostEntry(const QString &path) { return d->request("stat", {{"path", path}}, false); }
QString LocalClient::readHostFile(const QString &path, qint64 offset, const QString &version) {
    return offset < 0 ? QString{} : d->request("read", {{"path", path}, {"offset", QString::number(offset)}, {"version", version}}, false);
}
QString LocalClient::requestHost(const QJsonObject &payload) {
    return payload.value("op") == "society.generation" ? d->request("generation", {{"payload", payload}}, true) : QString{};
}
}
