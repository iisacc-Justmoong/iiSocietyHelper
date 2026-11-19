#include "ResidentNative.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#if !defined(__EMSCRIPTEN__) && (defined(__unix__) || defined(__APPLE__))
#define IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET 1
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace iiSocietyHelper {
namespace {
constexpr std::size_t maxFrame=512*1024;
constexpr std::size_t maxReply=1024*1024; // The Society backend's frame limit.
std::atomic<unsigned long long> sequence{0};

// Distinguish application cancellation, deadline expiry and connection errors.
std::string interruption(const ResidentWait &wait) {
    if (wait.canceled && wait.canceled()) return "The Society daemon request was canceled.";
    if (std::chrono::steady_clock::now() >= wait.deadline) return "The Society daemon request timed out.";
    return {};
}

// Sample the cancellation predicate while idle without renewing the budget
// when the peer sends only part of a frame.
int waitMilliseconds(const ResidentWait &wait) {
    using namespace std::chrono;
    if (wait.deadline == steady_clock::time_point::max()) return wait.canceled ? 50 : -1;
    const auto now = steady_clock::now();
    if (now >= wait.deadline) return 0;
    const auto remaining = ceil<milliseconds>(wait.deadline - now).count();
    return static_cast<int>(std::min<std::int64_t>(remaining, wait.canceled ? 50 : INT_MAX));
}
std::string id() {
#if defined(_WIN32)
    const auto process=GetCurrentProcessId();
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    const auto process=::getpid();
#else
    const int process=0;
#endif
    return "native-"+std::to_string(process)+"-"+std::to_string(++sequence);
}
std::string quote(std::string_view text) {
    static constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for (const unsigned char c:text) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=char(c);
    }
    out+='"'; return out;
}
std::filesystem::path defaultDirectory() {
#if defined(_WIN32)
    if (const auto *configured=_wgetenv(L"SOCIETY_LOCAL_BROKER_DIR"); configured && *configured) return std::filesystem::path(configured);
    if (const auto *home=_wgetenv(L"USERPROFILE"); home && *home) return std::filesystem::path(home)/".iisacc-society-ipc-v1";
#else
    if (const auto *configured=std::getenv("SOCIETY_LOCAL_BROKER_DIR"); configured && *configured) return std::filesystem::path(configured);
    if (const auto *home=std::getenv("HOME"); home && *home) return std::filesystem::path(home)/".iisacc-society-ipc-v1";
#endif
    return {};
}
#if defined(_WIN32)
std::expected<std::wstring,std::string> pipeName(const std::filesystem::path &directory) {
    auto normalized=directory.lexically_normal().generic_wstring();
    while (normalized.size()>3 && normalized.back()==L'/') normalized.pop_back();
    const int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,normalized.data(),int(normalized.size()),nullptr,0,nullptr,nullptr,0);
    if (!n) return std::unexpected("Cannot normalize the broker path.");
    std::wstring lower(n,L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,normalized.data(),int(normalized.size()),lower.data(),n,nullptr,nullptr,0))
        return std::unexpected("Cannot normalize the broker path.");
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,lower.data(),n,nullptr,0,nullptr,nullptr);
    if (!bytes) return std::unexpected("Invalid broker path.");
    std::string utf8(bytes,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,lower.data(),n,utf8.data(),bytes,nullptr,nullptr))
        return std::unexpected("Invalid broker path.");
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objectBytes=0,used=0;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
        return std::unexpected("Cannot open the system SHA-256 provider.");
    const auto finish=[&] { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0); };
    if (BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&used,0)<0) {
        finish(); return std::unexpected("Cannot initialize the system SHA-256 provider.");
    }
    std::vector<unsigned char> object(objectBytes); std::array<unsigned char,32> digest{};
    const bool ok=BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0)>=0
        && BCryptHashData(hash,reinterpret_cast<PUCHAR>(utf8.data()),ULONG(utf8.size()),0)>=0
        && BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    finish(); if (!ok) return std::unexpected("Cannot derive the broker pipe name.");
    static constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring name=L"\\\\.\\pipe\\iisacc-society-backend-";
    for (const auto byte:digest) { name+=hex[byte>>4]; name+=hex[byte&15]; }
    return name;
}
bool sameWindowsUser(HANDLE pipe) {
    ULONG pid=0; if (!GetNamedPipeServerProcessId(pipe,&pid) || !pid) return false;
    struct Handle { HANDLE h=nullptr; ~Handle() { if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)}, own, other;
    if (!process.h || !OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&own.h) || !OpenProcessToken(process.h,TOKEN_QUERY,&other.h)) return false;
    const auto user=[](HANDLE token) {
        DWORD size=0; GetTokenInformation(token,TokenUser,nullptr,0,&size);
        std::vector<unsigned char> bytes(size);
        if (!size || !GetTokenInformation(token,TokenUser,bytes.data(),size,&size)) return std::vector<unsigned char>{};
        return bytes;
    };
    const auto a=user(own.h),b=user(other.h);
    return !a.empty() && !b.empty() && EqualSid(reinterpret_cast<const TOKEN_USER *>(a.data())->User.Sid,
                                                reinterpret_cast<const TOKEN_USER *>(b.data())->User.Sid);
}

// Keep the overlapped pipe handle alive while a transfer reaps its result.
// Serialize starting I/O with close so no transfer can start after cancellation.
struct Pipe final {
    explicit Pipe(HANDLE value) : handle(value) {}
    ~Pipe() { CloseHandle(handle); }
    HANDLE handle;
    std::mutex admission;
    bool closed = false;
};

// Each overlapped transfer owns its event until the system completes or
// acknowledges cancellation. Never leave a pending transfer using a buffer.
std::expected<DWORD,std::string> pipeTransfer(Pipe &pipe, char *readBuffer,
    const char *writeBuffer, DWORD size, const ResidentWait &wait) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return std::unexpected("Cannot create a Society daemon pipe event.");
    struct Event { HANDLE handle; ~Event() { CloseHandle(handle); } } event{operation.hEvent};
    if (auto stopped = interruption(wait); !stopped.empty()) return std::unexpected(std::move(stopped));
    const auto socket=pipe.handle;
    DWORD bytes = 0;
    {
        std::lock_guard lock(pipe.admission);
        if (pipe.closed) return std::unexpected("The Society daemon pipe disconnected.");
        const auto ok = readBuffer ? ReadFile(socket, readBuffer, size, nullptr, &operation)
                                   : WriteFile(socket, writeBuffer, size, nullptr, &operation);
        if (ok && GetOverlappedResult(socket, &operation, &bytes, FALSE)) return bytes;
        if (GetLastError() != ERROR_IO_PENDING) return std::unexpected("The Society daemon pipe disconnected.");
    }
    for (;;) {
        auto stopped = interruption(wait);
        if (stopped.empty()) {
            const auto delay = waitMilliseconds(wait);
            const auto status = WaitForSingleObject(operation.hEvent, delay < 0 ? INFINITE : static_cast<DWORD>(delay));
            if (status == WAIT_TIMEOUT) continue;
            if (status == WAIT_OBJECT_0) {
                if (GetOverlappedResult(socket, &operation, &bytes, FALSE)) return bytes;
                return std::unexpected("The Society daemon pipe disconnected.");
            }
            stopped = "Cannot wait for the Society daemon pipe.";
        }
        CancelIoEx(socket, &operation);
        (void)GetOverlappedResult(socket, &operation, &bytes, TRUE);
        return std::unexpected(std::move(stopped));
    }
}
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
bool trusted(const std::filesystem::path &dir,const std::filesystem::path &path) {
    struct stat a{},b{};
    return !::lstat(dir.c_str(),&a) && S_ISDIR(a.st_mode) && a.st_uid==::geteuid() && !(a.st_mode&0077)
        && !::lstat(path.c_str(),&b) && S_ISSOCK(b.st_mode) && b.st_uid==::geteuid() && !(b.st_mode&0077);
}
bool sameUnixUser(int socket) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid{}; gid_t gid{}; return ::getpeereid(socket,&uid,&gid)==0 && uid==::geteuid();
#elif defined(__linux__)
    struct ucred peer{}; socklen_t n=sizeof(peer);
    return ::getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&peer,&n)==0 && peer.uid==::geteuid();
#else
    (void)socket; return false;
#endif
}

// The socket is nonblocking. Poll covers connect, reads and writes with the
// same absolute deadline, including under backpressure.
std::expected<void,std::string> waitSocket(int socket, short events, const ResidentWait &wait) {
    for (;;) {
        if (auto stopped = interruption(wait); !stopped.empty()) return std::unexpected(std::move(stopped));
        pollfd item{socket, events, 0};
        const int result = ::poll(&item, 1, waitMilliseconds(wait));
        if (result > 0 && !(item.revents & POLLNVAL)) return {};
        if (result == 0 || (result < 0 && errno == EINTR)) continue;
        return std::unexpected("Cannot wait for the Society daemon socket.");
    }
}
#endif
}

class ResidentNative::Impl {
public:
#if defined(_WIN32)
    std::atomic<std::shared_ptr<Pipe>> socket;
#else
    std::atomic<int> socket{-1};
#endif
    std::mutex writeMutex,readMutex;
#if defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    std::mutex socketMutex;
    int duplicateSocket() {
        std::lock_guard lock(socketMutex);
        const auto current=socket.load();
        return current<0 ? -1 : ::fcntl(current,F_DUPFD_CLOEXEC,0);
    }
#endif
};
ResidentNative::ResidentNative() : d(std::make_unique<Impl>()) {}
ResidentNative::~ResidentNative() { close(); }
std::expected<void,std::string> ResidentNative::connect(std::filesystem::path directory) {
    return connect(std::move(directory), {});
}
std::expected<void,std::string> ResidentNative::connect(std::filesystem::path directory, const ResidentWait &wait) {
    if (auto stopped = interruption(wait); !stopped.empty()) return std::unexpected(std::move(stopped));
    close();
    if (directory.empty()) directory=defaultDirectory();
    if (directory.empty() || !directory.is_absolute()) return std::unexpected("The Society broker directory must be absolute.");
#if defined(_WIN32)
    const auto name=pipeName(directory); if (!name) return std::unexpected(name.error());
    const auto handle=CreateFileW(name->c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (handle==INVALID_HANDLE_VALUE) return std::unexpected("The Society daemon pipe is unavailable.");
    struct Handle { HANDLE value; ~Handle() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); } } guard{handle};
    if (!sameWindowsUser(handle)) return std::unexpected("The Society daemon peer is untrusted.");
    auto pipe=std::make_shared<Pipe>(handle);
    guard.value=INVALID_HANDLE_VALUE;
    d->socket.store(std::move(pipe));
    return {};
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    const auto path=directory/"backend.sock"; const auto native=path.string();
    if (!trusted(directory,path)) return std::unexpected("The Society daemon endpoint is unavailable or untrusted.");
    sockaddr_un address{}; address.sun_family=AF_UNIX;
    if (native.size()>=sizeof(address.sun_path)) return std::unexpected("The Society daemon socket path is too long.");
    std::memcpy(address.sun_path,native.c_str(),native.size()+1);
    struct Socket { int fd; ~Socket() { if (fd >= 0) ::close(fd); } } opened{::socket(AF_UNIX,SOCK_STREAM,0)};
    const int socket=opened.fd;
    if (socket<0 || ::fcntl(socket,F_SETFD,FD_CLOEXEC)<0 || ::fcntl(socket,F_SETFL,O_NONBLOCK)<0)
        return std::unexpected("Cannot create the Society daemon socket.");
#ifdef SO_NOSIGPIPE
    int yes=1; (void)::setsockopt(socket,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
#endif
    if (::connect(socket,reinterpret_cast<sockaddr *>(&address),sizeof(address))) {
        if (errno!=EINPROGRESS && errno!=EINTR)
            return std::unexpected("The Society daemon connection failed.");
        if (auto ready=waitSocket(socket,POLLOUT,wait); !ready) return ready;
        int status=0; socklen_t length=sizeof(status);
        if (::getsockopt(socket,SOL_SOCKET,SO_ERROR,&status,&length) || status)
            return std::unexpected("The Society daemon connection failed.");
    }
    if (!trusted(directory,path) || !sameUnixUser(socket))
        return std::unexpected("The Society daemon peer could not be verified.");
    { std::lock_guard lock(d->socketMutex); d->socket.store(std::exchange(opened.fd,-1)); }
    return {};
#else
    return std::unexpected("The local Society daemon transport is unavailable on this platform.");
#endif
}
std::expected<void,std::string> ResidentNative::sendJson(std::string_view object) {
    return sendJson(object, {});
}
std::expected<void,std::string> ResidentNative::sendJson(std::string_view object, const ResidentWait &wait) {
    if (object.empty() || object.size()>maxFrame) return std::unexpected("The Society request frame is invalid or too large.");
    std::lock_guard lock(d->writeMutex);
#if defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    struct SocketLease { int fd; ~SocketLease() { if (fd>=0) ::close(fd); } } lease{d->duplicateSocket()};
    if (lease.fd<0) return std::unexpected("The Society daemon is not connected.");
#endif
    const auto n=object.size(); std::string data(4,'\0');
    data[0]=char(n>>24); data[1]=char(n>>16); data[2]=char(n>>8); data[3]=char(n); data.append(object);
    std::size_t offset=0;
    while (offset<data.size()) {
        if (auto stopped=interruption(wait); !stopped.empty()) { close(); return std::unexpected(std::move(stopped)); }
#if defined(_WIN32)
        const auto socket=d->socket.load();
        if (!socket) return std::unexpected("The Society daemon is not connected.");
        const auto transferred=pipeTransfer(*socket,nullptr,data.data()+offset,DWORD(data.size()-offset),wait);
        if (!transferred) { close(); return std::unexpected(transferred.error()); }
        const auto sent=*transferred;
        if (!sent) { close(); return std::unexpected("The Society daemon connection failed while sending."); }
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
        const auto socket=lease.fd;
#ifdef MSG_NOSIGNAL
        const auto sent=::send(socket,data.data()+offset,data.size()-offset,MSG_NOSIGNAL);
#else
        const auto sent=::send(socket,data.data()+offset,data.size()-offset,0);
#endif
        if (sent<0 && errno==EINTR) continue;
        if (sent<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            if (auto ready=waitSocket(socket,POLLOUT,wait); !ready) { close(); return ready; }
            continue;
        }
        if (sent<=0) { close(); return std::unexpected("The Society daemon connection failed while sending."); }
#else
        return std::unexpected("The local Society daemon transport is unavailable.");
#endif
#if defined(_WIN32) || defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
        offset+=std::size_t(sent);
#endif
    }
    return {};
}
std::expected<std::string,std::string> ResidentNative::receiveJson() {
    return receiveJson({});
}
std::expected<std::string,std::string> ResidentNative::receiveJson(const ResidentWait &wait) {
    std::lock_guard lock(d->readMutex);
#if defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    struct SocketLease { int fd; ~SocketLease() { if (fd>=0) ::close(fd); } } lease{d->duplicateSocket()};
    if (lease.fd<0) return std::unexpected("The Society daemon is not connected.");
#endif
    auto read=[&](char *buffer,std::size_t size) -> std::expected<void,std::string> {
        std::size_t offset=0;
        while (offset<size) {
            if (auto stopped=interruption(wait); !stopped.empty()) return std::unexpected(std::move(stopped));
#if defined(_WIN32)
            const auto socket=d->socket.load();
            if (!socket) return std::unexpected("The Society daemon is not connected.");
            const auto transferred=pipeTransfer(*socket,buffer+offset,nullptr,DWORD(size-offset),wait);
            if (!transferred) return std::unexpected(transferred.error());
            const auto received=*transferred;
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
            const auto received=::recv(lease.fd,buffer+offset,size-offset,0);
            if (received<0 && errno==EINTR) continue;
            if (received<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
                if (auto ready=waitSocket(lease.fd,POLLIN,wait); !ready) return ready;
                continue;
            }
#else
            return std::unexpected("The local Society daemon transport is unavailable on this platform.");
#endif
#if defined(_WIN32) || defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
            if (received<=0) return std::unexpected("The Society daemon disconnected.");
            offset+=std::size_t(received);
#endif
        }
        return {};
    };
    std::array<unsigned char,4> header{};
    if (auto received=read(reinterpret_cast<char *>(header.data()),header.size()); !received) {
        close(); return std::unexpected(received.error());
    }
    const std::size_t size=(std::size_t(header[0])<<24)|(std::size_t(header[1])<<16)|(std::size_t(header[2])<<8)|header[3];
    if (!size || size>maxReply) { close(); return std::unexpected("The Society daemon returned an invalid frame."); }
    std::string body(size,'\0');
    if (auto received=read(body.data(),size); !received) { close(); return std::unexpected(received.error()); }
    return body;
}
std::expected<void,std::string> ResidentNative::subscribe() {
    return sendJson("{\"id\":"+quote(id())+",\"op\":\"iisaccsonSubscribe\"}");
}
std::expected<std::string,std::string> ResidentNative::generate(const std::vector<ResidentMessage> &messages) {
    if (messages.empty() || messages.size()>128) return std::unexpected("A bounded message list is required.");
    const auto ticket=id(); std::string json="{\"id\":"+quote(ticket)+",\"op\":\"iisaccsonGenerate\",\"messages\":[";
    bool first=true;
    for (const auto &message:messages) {
        if (message.role!="user" && message.role!="assistant" && message.role!="system") return std::unexpected("Invalid message role.");
        if (message.content.size()>maxFrame) return std::unexpected("The Society request is too large.");
        if (!first) json+=','; first=false;
        json+="{\"role\":"+quote(message.role)+",\"content\":"+quote(message.content)+"}";
        if (json.size()>maxFrame) return std::unexpected("The Society request is too large.");
    }
    json+="]}"; const auto sent=sendJson(json); if (!sent) return std::unexpected(sent.error()); return ticket;
}
std::expected<void,std::string> ResidentNative::cancel(std::string_view requestId) {
    if (requestId.empty() || requestId.size()>128) return std::unexpected("Invalid request ID.");
    return sendJson("{\"id\":"+quote(id())+",\"op\":\"iisaccsonCancel\",\"requestId\":"+quote(requestId)+"}");
}
void ResidentNative::close() noexcept {
#if defined(_WIN32)
    if (const auto socket=d->socket.exchange({})) {
        std::lock_guard lock(socket->admission);
        socket->closed=true;
        CancelIoEx(socket->handle,nullptr);
    }
#elif defined(IISOCIETYHELPER_HAS_UNIX_RESIDENT_SOCKET)
    std::lock_guard lock(d->socketMutex);
    const auto socket=d->socket.exchange(-1); if (socket>=0) { ::shutdown(socket,SHUT_RDWR); ::close(socket); }
#endif
}
}
