#include "DiscoveryService.h"
#include <iiServerHost.h>
#include <QHostInfo>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QtEndian>
#include <dns_sd.h>
#include <QDebug>
#ifdef Q_OS_DARWIN
#include <netinet/in.h>
#endif

namespace {
constexpr char serviceType[] = "_society-pair._udp";
void trace(const QString &message) {
    if (qEnvironmentVariableIntValue("SOCIETY_DISCOVERY_TRACE") == 1) qInfo().noquote() << "Society DNS-SD:" << message;
}
class DnsSdService final : public DiscoveryService {
    struct Operation : QObject {
        DNSServiceRef ref = nullptr;
        QSocketNotifier *notifier = nullptr;
        DnsSdService *owner;
        QString key;
        QJsonObject record;
        quint16 port = 0;
        explicit Operation(DnsSdService *service) : QObject(service), owner(service) {}
        ~Operation() override { if (ref) DNSServiceRefDeallocate(ref); }
    };
    struct Service { QByteArray name, type, domain; uint32_t interfaceIndex; QPointer<Operation> resolving; };
    QHash<QString, Service> services;
    QList<Operation *> operations;
    QTimer refresh;
    QJsonObject identity;
    quint64 generation = 0;
    Operation *make() { auto *op = new Operation(this); operations.append(op); return op; }
    void retire(Operation *op) {
        if (op->notifier) op->notifier->setEnabled(false);
        operations.removeAll(op); op->deleteLater();
    }
    void error(DNSServiceErrorType code) {
        const auto message = code == kDNSServiceErr_PolicyDenied
            ? tr("Allow Society to access the local network in system settings.")
            : tr("Local device discovery is unavailable (%1). Retrying…").arg(code);
        QTimer::singleShot(0, this, [this, message] { emit failed(message); });
    }
    bool watch(Operation *op, DNSServiceErrorType code) {
        if (code != kDNSServiceErr_NoError) { error(code); retire(op); return false; }
        const auto fd = DNSServiceRefSockFD(op->ref);
        if (fd < 0) { error(kDNSServiceErr_Unknown); retire(op); return false; }
        op->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, op);
        connect(op->notifier, &QSocketNotifier::activated, this, [this, op] {
            const auto result = DNSServiceProcessResult(op->ref);
            if (result != kDNSServiceErr_NoError) { error(result); retire(op); }
        });
        return true;
    }
    void resolve(const QString &key) {
        if (!services.contains(key) || services.value(key).resolving) return;
        auto &service = services[key]; auto *op = make(); op->key = key; service.resolving = op;
        const auto result = DNSServiceResolve(&op->ref, 0, service.interfaceIndex, service.name.constData(), service.type.constData(),
            service.domain.constData(), [](DNSServiceRef, DNSServiceFlags, uint32_t interfaceIndex, DNSServiceErrorType code,
                                         const char *, const char *hostname, uint16_t port, uint16_t length,
                                         const unsigned char *txt, void *context) {
                auto *operation = static_cast<Operation *>(context); auto *self = operation->owner;
                const auto key = operation->key; const auto current = self->generation;
                trace(QStringLiteral("resolve result %1, TXT bytes %2").arg(code).arg(length));
                if (code == kDNSServiceErr_NoError && length <= 1024) {
                    QJsonObject record;
                    for (const auto *field : {"v", "scope", "id", "name", "kind", "host", "nonce", "epoch", "proof", "autoHost", "primary"}) {
                        uint8_t size = 0; const auto *value = TXTRecordGetValuePtr(length, txt, field, &size);
                        if (value) record.insert(field, QString::fromUtf8(static_cast<const char *>(value), size));
                    }
                    const auto host = QString::fromUtf8(hostname);
                    trace(QStringLiteral("same account scope %1, local hostname %2").arg(record.value("scope") == self->identity.value("scope")).arg(host.endsWith(".local.", Qt::CaseInsensitive)));
                    if (record.value("scope") == self->identity.value("scope") && record.value("id") != self->identity.value("id")
                        && host.endsWith(".local.", Qt::CaseInsensitive) && host.size() <= 255) {
#ifdef Q_OS_DARWIN
                        auto *addressOp = self->make(); addressOp->key = key; addressOp->record = record; addressOp->port = qFromBigEndian(port);
                        if (self->services.contains(key)) self->services[key].resolving = addressOp;
                        const auto result = DNSServiceGetAddrInfo(&addressOp->ref, 0, interfaceIndex, kDNSServiceProtocol_IPv4, hostname,
                            [](DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType error, const char *,
                               const struct sockaddr *address, uint32_t, void *context) {
                                auto *op = static_cast<Operation *>(context); auto *owner = op->owner;
                                trace(QStringLiteral("IPv4 result %1").arg(error));
                                if (!error && (flags & kDNSServiceFlagsAdd) && address && address->sa_family == AF_INET) {
                                    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
                                    const QHostAddress ip(qFromBigEndian(ipv4->sin_addr.s_addr));
                                    if (owner->services.contains(op->key) && iiServerHost::LanLink::localAddress(ip.toString()) && !ip.isLoopback())
                                        emit owner->found(op->key, op->record, ip, op->port);
                                }
                                // Keep the bounded query alive: additional A
                                // records can arrive in a later DNS batch.
                                if (error) {
                                    if (owner->services.contains(op->key) && owner->services[op->key].resolving == op) owner->services[op->key].resolving = nullptr;
                                    owner->retire(op);
                                }
                            }, addressOp);
                        if (self->watch(addressOp, result)) QTimer::singleShot(8000, addressOp, [self, addressOp] {
                            if (self->services.contains(addressOp->key) && self->services[addressOp->key].resolving == addressOp)
                                self->services[addressOp->key].resolving = nullptr;
                            self->retire(addressOp);
                        });
#else
                        QHostInfo::lookupHost(host, self, [self, current, key, record, port](const QHostInfo &info) {
                            trace(QStringLiteral("address result %1, addresses %2").arg(info.error()).arg(info.addresses().size()));
                            if (current != self->generation || !self->services.contains(key)) return;
                            for (const auto &address : info.addresses()) {
                                if (iiServerHost::LanLink::localAddress(address.toString()) && !address.isLoopback()) {
                                    emit self->found(key, record, address, qFromBigEndian(port));
                                }
                            }
                        });
#endif
                    }
                }
                if (self->services.contains(key) && self->services[key].resolving == operation) self->services[key].resolving = nullptr;
                self->retire(operation);
            }, op);
        if (!watch(op, result)) service.resolving = nullptr;
        else QTimer::singleShot(8000, op, [this, op, key] {
            if (services.contains(key) && services[key].resolving == op) services[key].resolving = nullptr;
            retire(op);
        });
    }
public:
    explicit DnsSdService(QObject *parent) : DiscoveryService(parent) {
        refresh.setInterval(6000);
        connect(&refresh, &QTimer::timeout, this, [this] { for (const auto &key : services.keys()) resolve(key); });
    }
    ~DnsSdService() override { stop(); }
    void start(const QJsonObject &record, quint16 port) override {
        stop(); identity = record;
        TXTRecordRef txt; TXTRecordCreate(&txt, 0, nullptr);
        for (auto it = record.begin(); it != record.end(); ++it) {
            const auto value = it.value().toString().toUtf8();
            if (value.size() > 255 || TXTRecordSetValue(&txt, it.key().toUtf8().constData(), uint8_t(value.size()), value.constData()) != kDNSServiceErr_NoError) {
                TXTRecordDeallocate(&txt); error(kDNSServiceErr_BadParam); return;
            }
        }
        auto *registration = make();
        const auto name = ("Society-" + record.value("nonce").toString().left(12)).toUtf8();
        const auto code = DNSServiceRegister(&registration->ref, 0, 0, name.constData(), serviceType, "local.", nullptr,
            qToBigEndian(port), TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt),
            [](DNSServiceRef, DNSServiceFlags, DNSServiceErrorType result, const char *, const char *, const char *, void *context) {
                if (result != kDNSServiceErr_NoError) static_cast<Operation *>(context)->owner->error(result);
            }, registration);
        TXTRecordDeallocate(&txt);
        if (!watch(registration, code)) return;
        auto *browse = make();
        if (!watch(browse, DNSServiceBrowse(&browse->ref, 0, 0, serviceType, "local.",
            [](DNSServiceRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType result,
               const char *name, const char *type, const char *domain, void *context) {
                auto *self = static_cast<Operation *>(context)->owner;
                trace(QStringLiteral("browse result %1, added %2").arg(result).arg(bool(flags & kDNSServiceFlagsAdd)));
                if (result != kDNSServiceErr_NoError) { self->error(result); return; }
                const auto key = QString::number(interfaceIndex) + ':' + QString::fromUtf8(name);
                if (!(flags & kDNSServiceFlagsAdd)) {
                    if (auto op = self->services.value(key).resolving) self->retire(op);
                    self->services.remove(key); emit self->lost(key); return;
                }
                if (!self->services.contains(key) && self->services.size() < 128)
                    self->services.insert(key, {name, type, domain, interfaceIndex, nullptr});
                self->resolve(key);
            }, browse))) return;
        refresh.start();
    }
    void stop() override {
        ++generation; refresh.stop(); services.clear(); identity = {};
        const auto pending = operations; for (auto *op : pending) retire(op);
    }
};
}
DiscoveryService *DiscoveryService::createNetwork(QObject *parent) { return new DnsSdService(parent); }
