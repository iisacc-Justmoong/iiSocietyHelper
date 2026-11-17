#include "ResidentClient.h"
#include "LocalFrame.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <utility>
#if defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace iiSocietyHelper {
namespace {
QString directoryOrDefault(QString directory) {
    if (directory.isEmpty()) directory=qEnvironmentVariable("SOCIETY_LOCAL_BROKER_DIR");
    return directory.isEmpty()?QDir::homePath()+"/.iisacc-society-ipc-v1":directory;
}
bool trustedPath(const QString &path) {
#if defined(Q_OS_UNIX)
    if (!QDir::isAbsolutePath(path)) return false;
    struct stat parent{}, endpoint{};
    const auto dir=QFile::encodeName(QFileInfo(path).absolutePath()), name=QFile::encodeName(path);
    return !::lstat(dir.constData(),&parent) && S_ISDIR(parent.st_mode) && parent.st_uid==::geteuid() && !(parent.st_mode&0077)
        && !::lstat(name.constData(),&endpoint) && S_ISSOCK(endpoint.st_mode) && endpoint.st_uid==::geteuid() && !(endpoint.st_mode&0077);
#elif defined(Q_OS_WIN)
    Q_UNUSED(path); return true; // The named-pipe server token is verified after connection.
#else
    Q_UNUSED(path); return false;
#endif
}
bool sameUser(QLocalSocket &socket) {
#if defined(Q_OS_DARWIN) || defined(Q_OS_FREEBSD)
    uid_t uid{}; gid_t gid{};
    return ::getpeereid(int(socket.socketDescriptor()),&uid,&gid)==0 && uid==::geteuid();
#elif defined(Q_OS_LINUX)
    struct ucred peer{}; socklen_t n=sizeof(peer);
    return ::getsockopt(int(socket.socketDescriptor()),SOL_SOCKET,SO_PEERCRED,&peer,&n)==0 && peer.uid==::geteuid();
#elif defined(Q_OS_WIN)
    const auto pipe=reinterpret_cast<HANDLE>(socket.socketDescriptor());
    ULONG pid=0;
    if (!pipe || pipe==INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe,&pid) || !pid) return false;
    struct Handle { HANDLE h=nullptr; ~Handle() { if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)}, own, other;
    if (!process.h || !OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&own.h)
        || !OpenProcessToken(process.h,TOKEN_QUERY,&other.h)) return false;
    const auto user=[](HANDLE token) {
        DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size);
        QByteArray bytes(int(size),'\0');
        if (!size || !GetTokenInformation(token,TokenUser,bytes.data(),size,&size)) return QByteArray{};
        return bytes;
    };
    const auto a=user(own.h), b=user(other.h);
    return !a.isEmpty() && !b.isEmpty() && EqualSid(reinterpret_cast<const TOKEN_USER *>(a.constData())->User.Sid,
                                                     reinterpret_cast<const TOKEN_USER *>(b.constData())->User.Sid);
#else
    Q_UNUSED(socket); return false;
#endif
}
}

QString ResidentClient::serverAddress(const QString &directory)
{
    const auto root=directoryOrDefault(directory);
#ifdef Q_OS_WIN
    const auto normalized=QDir::cleanPath(QDir::fromNativeSeparators(root)).toCaseFolded().toUtf8();
    return QStringLiteral("iisacc-society-backend-")+QString::fromLatin1(QCryptographicHash::hash(normalized,QCryptographicHash::Sha256).toHex());
#else
    return QDir(root).filePath(QStringLiteral("backend.sock"));
#endif
}

class ResidentClient::Private {
public:
    explicit Private(ResidentClient *owner, QString directory) : q(owner), address(serverAddress(directory)) {
        socket.setReadBufferSize(2*(LocalFrame::MaximumBody+4));
        frameDeadline.setSingleShot(true); frameDeadline.setInterval(10000);
        QObject::connect(&frameDeadline,&QTimer::timeout,q,[this] { socket.abort(); });
        QObject::connect(&socket,&QLocalSocket::connected,q,[this] {
            if (!sameUser(socket) || !trustedPath(address)) { socket.abort(); return; }
            online=true; QPointer<ResidentClient> guard(q); emit q->connectionChanged(true);
            if (!guard) return;
            send({{"id",subscription},{"op","iisaccsonSubscribe"}});
        });
        QObject::connect(&socket,&QLocalSocket::disconnected,q,[this] { disconnected(); });
        QObject::connect(&socket,&QLocalSocket::errorOccurred,q,[this](QLocalSocket::LocalSocketError) {
            if (socket.state()==QLocalSocket::UnconnectedState) {
                if (!online) { QPointer<ResidentClient> guard(q); emit q->requestFailed({},socket.errorString()); if (!guard) return; }
                disconnected();
            }
        });
        QObject::connect(&socket,&QLocalSocket::readyRead,q,[this] {
            QPointer<ResidentClient> guard(q);
            if (!input.append(socket.readAll())) { socket.abort(); return; }
            while (const auto message=input.take()) {
                receive(*message);
                if (!guard || socket.state()!=QLocalSocket::ConnectedState) return;
            }
            if (input.invalid()) socket.abort();
            else if (input.pending() && !frameDeadline.isActive()) frameDeadline.start();
            else if (!input.pending()) frameDeadline.stop();
        });
    }
    ~Private() {
        QObject::disconnect(&socket,nullptr,q,nullptr);
        QObject::disconnect(&frameDeadline,nullptr,q,nullptr);
        socket.abort();
    }
    ResidentClient *q;
    QString address, subscription=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QLocalSocket socket;
    LocalFrame input;
    QTimer frameDeadline;
    QSet<QString> generations, operations;
    bool online=false;
    bool send(const QJsonObject &message) {
        const auto bytes=LocalFrame::encode(message);
        if (!online || bytes.isEmpty()) return false;
        if (socket.bytesToWrite()+bytes.size()>2*(LocalFrame::MaximumBody+4) || socket.write(bytes)!=bytes.size()) {
            socket.abort(); return false;
        }
        return true;
    }
    void disconnected() {
        frameDeadline.stop(); input=LocalFrame{};
        const bool wasOnline=std::exchange(online,false);
        const auto pending=std::exchange(generations,{}); operations.clear();
        QPointer<ResidentClient> guard(q);
        for (const auto &id:pending) { emit q->requestFailed(id,QStringLiteral("The daemon disconnected before the request finished.")); if (!guard) return; }
        if (wasOnline) emit q->connectionChanged(false);
    }
    void receive(const QJsonObject &message) {
        const auto type=message.value("type").toString(), id=message.value("id").toString();
        if (type=="iisaccsonState" && message.value("subscription").toString()==subscription) {
            emit q->residentChanged(message.value("resident").toObject()); return;
        }
        if (type=="iisaccsonEvent" && generations.contains(id)) {
            const auto value=message.value("event").toObject();
            if (value.value("kind").toString()=="finished") generations.remove(id);
            emit q->generationEvent(id,value); return;
        }
        if (type!="result") return;
        if (id==subscription) {
            if (message.value("accepted").toBool()) emit q->residentChanged(message.value("resident").toObject());
            else emit q->requestFailed(id,message.value("error").toString());
            return;
        }
        if (!generations.contains(id) && !operations.contains(id)) return;
        operations.remove(id);
        if (!message.value("accepted").toBool()) {
            generations.remove(id);
            emit q->requestFailed(id,message.value("error").toString());
        }
    }
};

ResidentClient::ResidentClient(QString directory,QObject *parent) : QObject(parent),d(std::make_unique<Private>(this,std::move(directory))) {}
ResidentClient::~ResidentClient() = default;
void ResidentClient::connectToDaemon() {
    if (d->socket.state()!=QLocalSocket::UnconnectedState) return;
    if (!trustedPath(d->address)) { emit requestFailed({},QStringLiteral("The Society daemon endpoint is unavailable or untrusted.")); return; }
    d->input=LocalFrame{}; d->socket.connectToServer(d->address);
}
void ResidentClient::disconnectFromDaemon() { QPointer<ResidentClient> guard(this); d->socket.abort(); if (guard) d->disconnected(); }
bool ResidentClient::connected() const { return d->online; }
QString ResidentClient::generate(const QJsonArray &messages,const QJsonObject &options) {
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d->send({{"id",id},{"op","iisaccsonGenerate"},{"messages",messages},{"options",options}})) return {};
    d->generations.insert(id); return id;
}
bool ResidentClient::cancel(const QString &requestId) {
    if (!d->generations.contains(requestId)) return false;
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d->send({{"id",id},{"op","iisaccsonCancel"},{"requestId",requestId}})) return false;
    d->operations.insert(id); return true;
}
}
