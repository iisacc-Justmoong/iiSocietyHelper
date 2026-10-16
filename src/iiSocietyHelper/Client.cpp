#include <iiFileProviderDevice.h>
#include "Client.h"
#include "AccountSession.h"
#include "PairingCredentials.h"
#include "NearbyDevices.h"
#include "AutomaticPairing.h"
#include <iiSocietySync.h>
#include <SharedStorage.h>
#include <StorageMap.h>
#include <QDir>
#include <QSysInfo>
#include <QTimer>
#include <QScopedValueRollback>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <iiFileProviderPresentation.h>
#include <QTimeZone>
#include <QUuid>
#include <functional>
#include <utility>
#include <algorithm>

namespace iiSocietyHelper {
namespace {
bool drivePath(const QString &path, bool root = false) {
    if (path.isEmpty()) return root;
    if (path.size() > 3500 || !path.isValidUtf16() || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 220 || part.startsWith(".society-")
            || part.startsWith(".iiserverhost-") || iiFileProvider::native::Presentation::isControlFile(std::filesystem::path(part.toStdU16String()))) return false;
    for (const auto section : iiSocietyContainer::allStoreSections())
        if (path.section('/', 0, 0) == iiSocietyContainer::storeSectionKey(section)) return true;
    return false;
}
QJsonObject driveError(const QString &error) { return {{"ok", false}, {"error", error}}; }
}
class Client::Private {
public:
    Client *q;
    QPointer<AccountSession> account;
    DiscoveryService *discovery;
    QHostAddress bindAddress;
    std::unique_ptr<NearbyDevices> nearby;
    std::unique_ptr<AutomaticPairing> pairing;
    iiServerHost::LanPeer lan;
    iiServerHost::Peer relay;
    iiSocietySync::Controller sync;
    QTimer timer;
    std::unique_ptr<iiFileProvider::LockFile> lease;
    QString container, message, registeredHost, registeredContainer;
    QJsonObject binding;
    QUrl server;
    QByteArray credential;
    // Direct consumers are independent authenticated application sessions. A
    // device ID is reserved for its replica synchronizer; reusing it would make
    // a second app contend for the same LAN/relay connection.
    const QString directPeerId = QString::fromLatin1(QCryptographicHash::hash(
        QUuid::createUuid().toRfc4122(), QCryptographicHash::Sha256).toHex());
    QSet<QString> hostRequests;
    iiSocietyContainer::Container mirror;
    QVariantList sections;
    struct DriveRequest {
        QJsonObject envelope;
        std::function<void(QJsonObject)> completion;
        QString transport;
        int pollDelay = 10;
    };
    struct Snapshot {
        QVariantList aliases, sections;
        QSet<QString> keys;
        QString revision, cursor;
    };
    QHash<QString, std::shared_ptr<DriveRequest>> driveRequests;
    struct Inspection {
        QString id, path, version, token;
        QByteArray bytes;
        qint64 size = -1;
    };
    QHash<QString, std::shared_ptr<Inspection>> inspections;
    QHash<QString, QString> driveTransports;
    std::shared_ptr<Snapshot> snapshot;
    bool active = false, updating = false, online = false, direct = false, local = false, refreshAgain = false;
    explicit Private(Client *owner, AccountSession *session, DiscoveryService *service, QHostAddress bind)
        : q(owner), account(session), discovery(service), bindAddress(bind), sync([this](auto peer, auto payload) {
            return server.isEmpty() ? lan.request(peer, payload) : relay.request(peer, payload);
        }), mirror(owner) {
        lan.setPairingAuthentication([this](const QByteArray &context) {
            return account && account->signedIn() && account->pairingCredentials().value("scope").toString() == account->storageScope()
                ? SocietyPairingCredentials::sign(account->pairingCredentials(), context) : QJsonObject{};
        }, [this](const QByteArray &context, const QJsonObject &proof) {
            return account && account->signedIn() && account->pairingCredentials().value("scope").toString() == account->storageScope()
                && SocietyPairingCredentials::verify(account->pairingCredentials(), context, proof);
        });
        timer.setInterval(1000);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { update(); });
        QObject::connect(&sync, &iiSocietySync::Controller::progress, q, &Client::progress);
        QObject::connect(&mirror, &iiSocietyContainer::Container::refreshRequested, q, &Client::refreshContainer);
        QObject::connect(&sync, &iiSocietySync::Controller::synchronized, q, [this] { refreshHost(); emit q->synchronized(); });
        QObject::connect(&sync, &iiSocietySync::Controller::changed, q, [this] {
            if (!sync.errorString().isEmpty()) message = sync.errorString();
            if (local && active) { peers(); return; }
            emit q->changed();
        });
        const auto response = [this](const QString &id, const QJsonObject &result, const QString &) {
            if (const auto request = driveTransports.take(id); !request.isEmpty()) receiveDrive(request, result);
            else if (hostRequests.remove(id)) emit q->hostResponse(id, result);
            else sync.receive(id, result);
        };
        QObject::connect(&lan, &iiServerHost::LanPeer::completed, q, response);
        QObject::connect(&relay, &iiServerHost::Peer::completed, q, response);
        QObject::connect(&lan, &iiServerHost::LanPeer::paired, q, [this](const QString &host) {
            if (account && nearby && host == accountHost())
                account->rememberHostConnection(host, accountContainer(), nearby->connectionHints(host));
            if (nearby) nearby->complete(); update();
        });
        QObject::connect(&relay, &iiServerHost::Peer::peersChanged, q, [this] { update(); });
        QObject::connect(&relay, &iiServerHost::Peer::stateChanged, q, [this] { update(); });
    }
    void clearDrive(const QString &error) {
        snapshot.reset(); refreshAgain = false; sections.clear();
        auto pending = std::move(driveRequests); driveRequests.clear(); driveTransports.clear();
        mirror.synchronize({}, {}, {}, false, error);
        for (auto it = pending.cbegin(); it != pending.cend(); ++it)
            if (!it.value()->completion) emit q->hostDriveResponse(it.key(), driveError(error));
        const auto cancelled = std::move(inspections); inspections.clear();
        for (auto it = cancelled.cbegin(); it != cancelled.cend(); ++it)
            emit q->hostDriveResponse(it.key(), driveError(error));
    }
    void finishInspection(const std::shared_ptr<Inspection> &state, QJsonObject result) {
        if (inspections.value(state->id) != state) return;
        inspections.remove(state->id); emit q->hostDriveResponse(state->id, result);
    }
    void inspectPage(const std::shared_ptr<Inspection> &state) {
        if (inspections.value(state->id) != state) return;
        QJsonObject message{{"action", "drive.inspect"}, {"path", state->path}, {"offset", QString::number(state->bytes.size())}};
        if (!state->version.isEmpty()) { message.insert("version", state->version); message.insert("informationVersion", state->token); }
        const auto request = requestDrive(message, [this, state](const QJsonObject &result) {
            if (inspections.value(state->id) != state) return;
            if (!result.value("ok").toBool()) { finishInspection(state, result); return; }
            bool validOffset = false, validSize = false;
            const auto offset = result.value("offset").toString().toLongLong(&validOffset);
            const auto size = result.value("size").toString().toLongLong(&validSize);
            const auto token = result.value("informationVersion").toString(), version = result.value("version").toString();
            const auto encoded = result.value("data").toString().toLatin1();
            if (!result.value("data").isString() || encoded.size() > ((iiSocietySync::Replica::ChunkBytes + 2) / 3) * 4) {
                finishInspection(state, driveError("invalid_host_information_chunk")); return;
            }
            const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
            const bool validToken = token.size() == 64 && std::all_of(token.cbegin(), token.cend(), [](QChar c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
            if (!validOffset || !validSize || offset != state->bytes.size() || size <= 0
                || size > iiSocietyContainer::SharedStorage::maximumFileInformationBytes || (state->size >= 0 && state->size != size)
                || !decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > iiSocietySync::Replica::ChunkBytes
                || decoded.decoded.toBase64() != encoded || offset + decoded.decoded.size() > size
                || result.value("path").toString() != state->path || result.value("format") != "iiFileProvider.FileInformation"
                || result.value("schema") != 1 || !result.value("eof").isBool()
                || result.value("eof").toBool() != (offset + decoded.decoded.size() == size)
                || !validToken || version.isEmpty() || (!state->version.isEmpty() && (state->version != version || state->token != token))) {
                finishInspection(state, driveError("invalid_host_information_chunk")); return;
            }
            state->version = version; state->token = token; state->size = size; state->bytes += decoded.decoded;
            if (!result.value("eof").toBool()) { inspectPage(state); return; }
            QJsonParseError error;
            const auto information = QJsonDocument::fromJson(state->bytes, &error).object();
            if (error.error != QJsonParseError::NoError || information.value("format") != "iiFileProvider.FileInformation"
                || information.value("schema") != 1 || information.value("path").toString() != state->path
                || information.value("container").toString() != accountContainer()
                || QString::fromLatin1(QCryptographicHash::hash(state->bytes, QCryptographicHash::Sha256).toHex()) != token) {
                finishInspection(state, driveError("invalid_host_information_document")); return;
            }
            finishInspection(state, {{"ok", true}, {"container", accountContainer()}, {"path", state->path},
                {"version", version}, {"informationVersion", token}, {"information", information}});
        });
        if (request.isEmpty()) finishInspection(state, driveError("host_request_capacity"));
    }
    void finishDrive(const QString &id, QJsonObject result) {
        const auto request = driveRequests.take(id); if (!request) return;
        driveTransports.remove(request->transport);
        if (request->completion) request->completion(std::move(result));
        else {
            const auto action = request->envelope.value("message").toObject().value("action").toString();
            if (result.value("ok").toBool() && (action == "drive.write" || action == "drive.mkdir")) refreshHost();
            emit q->hostDriveResponse(id, result);
        }
    }
    void transmitDrive(const QString &id) {
        const auto request = driveRequests.value(id); if (!request) return;
        if (!q->connected()) { finishDrive(id, driveError("host_disconnected")); return; }
        if (local) { receiveDrive(id, sync.handle(QStringLiteral("local-application"), request->envelope)); return; }
        request->transport = server.isEmpty() ? lan.request(accountHost(), request->envelope) : relay.request(accountHost(), request->envelope);
        if (request->transport.isEmpty()) { finishDrive(id, driveError("host_transport_unavailable")); return; }
        driveTransports.insert(request->transport, id);
    }
    QString requestDrive(QJsonObject message, std::function<void(QJsonObject)> completion = {}) {
        if (!q->connected() || driveRequests.size() >= 16) return {};
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto request = std::make_shared<DriveRequest>(); request->completion = std::move(completion);
        message.insert("container", accountContainer());
        request->envelope = {{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", account->storageScope()},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"message", message}};
        driveRequests.insert(id, request);
        // Dispatch after returning the ID, including immediate transport failure.
        QTimer::singleShot(0, q, [this, id] { transmitDrive(id); });
        QTimer::singleShot(30000, q, [this, id] { finishDrive(id, driveError("host_request_timeout")); });
        return id;
    }
    void receiveDrive(const QString &id, const QJsonObject &response) {
        const auto request = driveRequests.value(id); if (!request) return;
        request->transport.clear();
        if (!q->connected()) { finishDrive(id, driveError("host_disconnected")); return; }
        const auto retryBusy = [&](const QJsonObject &failure) {
            const auto error = failure.value("error").toString();
            if (error != "sync_store_busy" && error != "sync_endpoint_busy") return false;
            // These responses precede filesystem execution. A finished busy job
            // is cached, so retry its rejected operation with a fresh token,
            // keeping the public ID and original 30-second deadline unchanged.
            request->envelope.insert("token", QUuid::createUuid().toString(QUuid::WithoutBraces));
            const auto delay = request->pollDelay; request->pollDelay = qMin(250, delay * 2);
            QTimer::singleShot(delay, q, [this, id] { transmitDrive(id); }); return true;
        };
        if (!response.value("ok").toBool()) {
            if (!retryBusy(response)) finishDrive(id, response);
            return;
        }
        if (response.value("pending").toBool() && !response.contains("result")) {
            const auto delay = request->pollDelay; request->pollDelay = qMin(250, delay * 2);
            QTimer::singleShot(delay, q, [this, id] { transmitDrive(id); }); return;
        }
        auto result = response.value("result").toObject();
        if (!result.value("ok").toBool() && retryBusy(result)) return;
        if (!result.value("ok").isBool() || (result.value("ok").toBool() && result.value("container").toString() != accountContainer()))
            result = driveError("host_drive_response_mismatch");
        finishDrive(id, result);
    }
    QString remoteRoot() const {
        // Device IDs are opaque; an all-numeric ID must not be parsed as IPv4.
        QUrl root; root.setScheme("society"); root.setHost("host");
        root.setPath('/' + accountHost() + '/' + accountContainer()); return root.toString();
    }
    void finishSnapshot(const std::shared_ptr<Snapshot> &state, const QString &error = {}) {
        if (snapshot != state) return;
        snapshot.reset(); sections = error.isEmpty() ? state->sections : QVariantList{};
        mirror.synchronize(accountContainer(), remoteRoot(), error.isEmpty() ? state->aliases : QVariantList{}, error.isEmpty(), error);
        emit q->changed();
        if (std::exchange(refreshAgain, false)) QTimer::singleShot(0, q, [this] { refreshHost(); });
    }
    void catalogPage(const std::shared_ptr<Snapshot> &state) {
        QJsonObject message{{"action", "drive.catalog"}};
        if (!state->cursor.isEmpty()) { message.insert("cursor", state->cursor); message.insert("revision", state->revision); }
        const auto id = requestDrive(message, [this, state](const QJsonObject &result) {
            if (snapshot != state) return;
            if (!result.value("ok").toBool()) { finishSnapshot(state, result.value("error").toString("host_catalog_unavailable")); return; }
            const auto revision = result.value("revision").toString(), cursor = result.value("nextCursor").toString();
            if (revision.isEmpty() || (!state->revision.isEmpty() && revision != state->revision) || !result.value("entries").isArray()
                || !result.value("nextCursor").isString() || (!cursor.isEmpty() && (!drivePath(cursor) || cursor.toUtf8() <= state->cursor.toUtf8()))) {
                finishSnapshot(state, "invalid_host_catalog"); return;
            }
            const auto entries = result.value("entries").toArray();
            if (entries.size() > 128 || state->keys.size() + entries.size() > 100000 || (!cursor.isEmpty() && entries.isEmpty())) {
                finishSnapshot(state, "host_catalog_limit"); return;
            }
            for (const auto &value : entries) {
                const auto entry = value.toObject(); const auto path = entry.value("path").toString(), kind = entry.value("kind").toString();
                bool validSize = false; const auto size = entry.value("size").toString().toLongLong(&validSize);
                if (!drivePath(path) || !path.contains('/') || state->keys.contains(path) || (kind != "file" && kind != "directory")
                    || !validSize || size < 0 || entry.value("version").toString().isEmpty()) { finishSnapshot(state, "invalid_host_catalog_entry"); return; }
                state->keys.insert(path);
                // Container Aliases describe files. Directories are navigated
                // through listHostDirectory and must not appear in recentFiles.
                if (kind == "directory") continue;
                const auto section = path.section('/', 0, 0), relative = path.mid(section.size() + 1), name = path.section('/', -1);
                const auto suffix = name.section('.', -1).toLower();
                const bool image = kind == "file" && QStringList{"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff", "avif", "heic", "heif"}.contains(suffix);
                bool validTime = false; const auto modified = entry.value("modified").toString().toLongLong(&validTime);
                state->aliases.append(QVariantMap{{"id", accountContainer() + '/' + path}, {"containerId", accountContainer()},
                    {"key", path}, {"section", section}, {"relativePath", relative}, {"name", name}, {"kind", kind},
                    {"isDirectory", kind == "directory"}, {"exists", true}, {"available", false}, {"source", QUrl{}},
                    {"previewSource", QUrl{}}, {"thumbnailSource", QUrl{}}, {"isImage", image}, {"size", size},
                    {"modified", validTime ? QDateTime::fromMSecsSinceEpoch(modified, QTimeZone::UTC) : QDateTime{}},
                    {"version", entry.value("version").toString()}, {"hash", entry.value("hash").toString()}});
                auto alias = state->aliases.takeLast().toMap();
                alias.insert("informationAvailable", entry.value("information").toObject().value("provider") == "iiFileProvider");
                alias.insert("informationKey", path); state->aliases.append(alias);
            }
            state->revision = revision; state->cursor = cursor;
            if (cursor.isEmpty()) finishSnapshot(state); else catalogPage(state);
        });
        if (id.isEmpty()) finishSnapshot(state, "host_request_capacity");
    }
    void refreshHost() {
        if (!q->connected()) return;
        if (snapshot) { refreshAgain = true; return; }
        const auto state = std::make_shared<Snapshot>(); snapshot = state;
        const auto id = requestDrive({{"action", "drive.describe"}}, [this, state](const QJsonObject &result) {
            if (snapshot != state) return;
            if (!result.value("ok").toBool()) { finishSnapshot(state, result.value("error").toString("host_drive_unavailable")); return; }
            const auto values = result.value("sections").toArray(); QSet<QString> keys;
            for (const auto &value : values) keys.insert(value.toObject().value("key").toString());
            if (result.value("driveProtocol") != 1 || values.size() != 9 || keys.size() != 9) { finishSnapshot(state, "host_drive_protocol_required"); return; }
            for (const auto section : iiSocietyContainer::allStoreSections()) {
                const auto key = iiSocietyContainer::storeSectionKey(section);
                if (!keys.contains(key)) { finishSnapshot(state, "invalid_host_sections"); return; }
                state->sections.append(QVariantMap{{"key", key}, {"name", iiSocietyContainer::storeSectionName(section)}, {"containerId", accountContainer()}});
            }
            catalogPage(state);
        });
        if (id.isEmpty()) finishSnapshot(state, "host_request_capacity");
    }
    void initialize() {
        if (nearby) return;
        if (!account) account = new AccountSession(q);
        nearby = std::make_unique<NearbyDevices>(discovery, bindAddress, q);
        pairing = std::make_unique<AutomaticPairing>(nearby.get(), &lan, [] { return false; }, [this](const QString &link) {
            iiServerHost::LanLink decoded;
            return account && iiServerHost::LanLink::decode(link, &decoded) && decoded.hostId == accountHost()
                && !accountContainer().isEmpty() && lan.join(link, nearby->deviceId(), nearby->deviceName());
        }, [this] { lan.stop(); }, q);
        QObject::connect(account->manager()->account(), &iisacc::accounts::Account::changed, q, [this] { update(); });
        QObject::connect(account, &AccountSession::changed, q, [this] { update(); });
        QObject::connect(account, &AccountSession::pairingCredentialsChanged, q, [this] { update(); });
        QObject::connect(account, &AccountSession::pairingStateRestored, q, [this] { update(); });
        QObject::connect(account, &AccountSession::pairingCredentialsRequired, account, &AccountSession::requestPairingCredentials);
        QObject::connect(account, &AccountSession::sessionEnding, q, [this] { transportStop(); });
        QObject::connect(nearby.get(), &NearbyDevices::identityEnding, &lan, &iiServerHost::LanPeer::stop);
        QObject::connect(nearby.get(), &NearbyDevices::changed, q, [this] { peers(); });
    }
    void transportStop() {
        online = false;
        clearDrive("host_disconnected");
        hostRequests.clear();
        sync.closeAndWait();
        if (nearby) nearby->clear();
        lan.stop(); relay.stop(); server = QUrl(); credential.clear();
        lease.reset();
    }
    QString accountHost() const { return account && account->signedIn() ? account->manager()->account()->societyContainerDrive().value("hostDeviceId").toString() : QString(); }
    QString accountContainer() const { return account && account->signedIn() ? account->manager()->account()->societyContainerDrive().value("containerId").toString() : QString(); }
    void peers() {
        if (!active || !account || !account->signedIn() || (!direct && (!lease || !lease->isLocked()))) return;
        if (local) {
            const bool wasOnline = online; online = sync.available();
            if (wasOnline && !online) clearDrive("host_disconnected");
            if (!wasOnline && online) refreshHost();
            message = online ? "Using the local Society host drive."
                : sync.errorString().isEmpty() ? "Opening the local Society host drive." : sync.errorString();
            emit q->changed(); return;
        }
        const auto host = accountHost(); QStringList authorized;
        if (server.isEmpty()) {
            if (nearby->authenticated() && lan.connected() && lan.pairedDeviceIds().contains(host)) authorized.append(host);
        } else if (relay.isReady() && relay.accountId() == account->manager()->account()->sub()) {
            for (const auto &value : relay.peers()) if (value.toObject().value("peerId") == host) authorized.append(host);
        }
        if (host.isEmpty() || accountContainer().isEmpty()) authorized.clear();
        if (!direct) sync.setPeers(authorized, authorized, host, accountContainer());
        const bool wasOnline = online;
        online = !authorized.isEmpty();
        if (wasOnline && !online) clearDrive("host_disconnected");
        if (!wasOnline && online) refreshHost();
        if (nearby && online) nearby->resumeHost({}, {});
        message = authorized.isEmpty() ? "Waiting for the Society host." : "Connected to the Society host.";
        if (authorized.isEmpty() && server.isEmpty() && pairing) message = pairing->status();
        if (authorized.isEmpty() && !server.isEmpty() && !relay.errorString().isEmpty()) message = relay.errorString();
        if (authorized.isEmpty() && server.isEmpty() && !lan.errorString().isEmpty()) message = lan.errorString();
        emit q->changed();
    }
    void update() {
        if (!active || !account || updating) return;
        QScopedValueRollback guard(updating, true);
        if (!account->signedIn()) { transportStop(); message = "Sign in to Society on this device to connect to its host."; emit q->changed(); return; }
        if (registeredHost != accountHost() || registeredContainer != accountContainer()) {
            registeredHost = accountHost(); registeredContainer = accountContainer(); transportStop();
        }
        if (lan.connected() && (!SocietyPairingCredentials::valid(account->pairingCredentials()) || !lan.pairedDeviceIds().contains(accountHost()))) lan.stop();
        const auto scope = account->storageScope();
        if (scope != binding.value("scope").toString()) { transportStop(); message = "The Society storage belongs to a different account."; emit q->changed(); return; }
        const auto device = account->manager()->deviceInfo();
        if (!direct && !accountHost().isEmpty() && accountHost() == device.value("id").toString()) {
            transportStop(); message = "Use connectHost() to access this device's registered Society drive."; emit q->changed(); return;
        }
        if (direct && !accountHost().isEmpty() && accountHost() == device.value("id").toString()) {
            const auto storage = iiSocietyContainer::SharedStorage::open();
            const bool valid = storage && storage->drive().identifier() == accountContainer()
                && iiSocietySync::Replica::binding(storage->drive().rootPath()).isEmpty()
                && iiSocietySync::Replica::primaryHost(storage->drive().rootPath(), scope) == accountHost();
            if (!valid) {
                transportStop(); local = true;
                message = "The registered local Society host container is unavailable."; emit q->changed(); return;
            }
            const auto root = storage->drive().rootPath();
            if (!local || container != root) { transportStop(); local = true; container = root; }
            sync.openHostAccess(container, scope);
            sync.setPeers({QStringLiteral("local-application")}, {});
            peers(); return;
        }
        if (local) { transportStop(); local = false; container.clear(); }
        if (!direct) {
            if (!lease) {
                lease = std::make_unique<iiFileProvider::LockFile>(QDir(container).filePath(".society-sync/client-session.lock"));
                lease->setStaleLockTime(0);
            }
            if (!lease->isLocked() && !lease->tryLock()) { message = "Waiting for the other Society session to finish."; emit q->changed(); return; }
            sync.open(container, scope);
        }
        const auto name = device.value("name").toString().isEmpty() ? QSysInfo::machineHostName() : device.value("name").toString();
        const auto peerId = direct ? directPeerId : device.value("id").toString();
        const QUrl configured(account->serverConfiguration().value("url").toString());
        if (configured.isEmpty()) {
            if (!server.isEmpty()) { relay.stop(); server = QUrl(); credential.clear(); }
            nearby->setIdentity(scope, peerId, name, device.value("type").toString(), false);
            nearby->setCredentials(account->pairingCredentials(), false, accountHost());
            pairing->setEnabled(account->automaticPairingEnabled());
            if (pairing->enabled() && !lan.connected())
                nearby->resumeHost(accountHost(), account->savedHostConnection().value("routes").toArray());
            else nearby->resumeHost({}, {});
            account->requestPairingCredentials();
        } else {
            pairing->setEnabled(false); nearby->clear();
            const bool valid = configured.isValid() && !configured.host().isEmpty() && configured.userInfo().isEmpty()
                && !configured.hasQuery() && !configured.hasFragment()
                && (configured.scheme() == "wss" || (configured.scheme() == "ws" && QHostAddress(configured.host()).isLoopback()));
            const auto proof = account->relayCredential();
            if (!valid || proof.isEmpty()) { message = "The saved Society server session is unavailable."; emit q->changed(); return; }
            if (server != configured || credential != proof) {
                lan.stop(); relay.stop(); server = configured; credential = proof;
                iiServerHost::PeerOptions options;
                options.relayUrl = server; options.credential = proof; options.peerId = peerId; options.name = name.left(128);
                options.service = "com.iisacc.society.files"; options.hostFiles = false; options.localHostingEnabled = false;
                relay.start(options, [this](const QString &peer, const QJsonObject &payload) { return sync.handle(peer, payload); });
            }
        }
        peers();
    }
};
Client::Client(QObject *parent) : Client(nullptr, nullptr, parent) {}
Client::Client(AccountSession *account, DiscoveryService *discovery, QObject *parent)
    : Client(account, discovery, QHostAddress::AnyIPv4, parent) {}
Client::Client(AccountSession *account, DiscoveryService *discovery, QHostAddress bindAddress, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, account, discovery, bindAddress)) {}
Client::~Client() { stop(); }
bool Client::start(const QString &container) {
    if (d->active && !d->direct && d->container == container) return true;
    stop();
    d->direct = false; d->local = false;
    const auto storage = iiSocietyContainer::SharedStorage::open(container);
    if (!storage) { d->message = "Open the Society storage map on this device first."; emit changed(); return false; }
    d->container = storage->drive().rootPath(); d->binding = iiSocietySync::Replica::binding(d->container);
    if (d->binding.isEmpty()) { d->message = "Using the local Society host storage."; emit changed(); return true; }
    d->active = true; d->initialize(); d->timer.start(); d->update(); return true;
}
bool Client::connectHost() {
    if (d->active && d->direct && d->account && d->account->signedIn()
        && d->binding.value("scope").toString() == d->account->storageScope()) return true;
    stop(); d->initialize();
    if (!d->account || !d->account->signedIn() || d->accountHost().isEmpty() || d->accountContainer().isEmpty()) {
        d->message = "Sign in with an account that has a registered Society host."; emit changed(); return false;
    }
    d->direct = true; d->container.clear(); d->binding = {{"scope", d->account->storageScope()}};
    d->active = true; d->timer.start(); d->update(); return true;
}
void Client::stop() { d->active = false; d->timer.stop(); d->transportStop(); emit changed(); }
void Client::synchronizeNow() { d->update(); if (d->direct) d->refreshHost(); else d->sync.synchronizeNow(); }
QString Client::status() const { return d->message; }
bool Client::connected() const {
    return d->active && d->online && d->account && d->account->signedIn()
        && d->registeredHost == d->accountHost() && d->registeredContainer == d->accountContainer()
        && d->binding.value("scope").toString() == d->account->storageScope();
}
iiSocietyContainer::Container *Client::container() const { return &d->mirror; }
QJsonObject Client::hostBinding() const {
    return connected() ? QJsonObject{{"scope", d->account->storageScope()},
        {"host", d->accountHost()}, {"container", d->accountContainer()}} : QJsonObject{};
}
QVariantList Client::hostSections() const { return d->sections; }
void Client::refreshContainer() { d->refreshHost(); }
QString Client::listHostDirectory(const QString &path, const QString &cursor, const QString &version) {
    if (!drivePath(path, true) || (!cursor.isEmpty() && (cursor.contains('/') || !drivePath(path + '/' + cursor) || version.isEmpty()))) return {};
    QJsonObject message{{"action", "drive.list"}, {"path", path}};
    if (!cursor.isEmpty()) message.insert("cursor", cursor);
    if (!version.isEmpty()) message.insert("version", version);
    return d->requestDrive(message);
}
QString Client::statHostEntry(const QString &path) {
    return drivePath(path) ? d->requestDrive({{"action", "drive.stat"}, {"path", path}}) : QString{};
}
QString Client::inspectHostFile(const QString &path) {
    if (!drivePath(path) || !path.contains('/') || !connected() || d->inspections.size() >= 8) return {};
    auto state = std::make_shared<Private::Inspection>();
    state->id = QUuid::createUuid().toString(QUuid::WithoutBraces); state->path = path;
    d->inspections.insert(state->id, state);
    QTimer::singleShot(0, this, [this, state] { d->inspectPage(state); });
    QTimer::singleShot(30000, this, [this, id = state->id] {
        if (const auto pending = d->inspections.value(id)) d->finishInspection(pending, driveError("host_request_timeout"));
    });
    return state->id;
}
QString Client::readHostFile(const QString &path, qint64 offset, const QString &version) {
    if (!drivePath(path) || !path.contains('/') || offset < 0 || offset > 9007199254740991LL || version.size() > 256) return {};
    QJsonObject message{{"action", "drive.read"}, {"path", path}, {"offset", QString::number(offset)}};
    if (!version.isEmpty()) message.insert("version", version);
    return d->requestDrive(message);
}
QString Client::writeHostFile(const QString &path, const QByteArray &data) {
    if (!drivePath(path) || !path.contains('/') || data.size() > iiSocietySync::Replica::ChunkBytes) return {};
    return d->requestDrive({{"action", "drive.write"}, {"path", path}, {"data", QString::fromLatin1(data.toBase64())}});
}
QString Client::createHostDirectory(const QString &path) {
    return drivePath(path) && path.contains('/') ? d->requestDrive({{"action", "drive.mkdir"}, {"path", path}}) : QString{};
}
QString Client::requestHost(const QJsonObject &payload) {
    if (!connected() || d->local || d->hostRequests.size() >= 64 || payload.value("op") != "society.generation") return {};
    const auto host = d->accountHost();
    const auto id = d->server.isEmpty() ? d->lan.request(host, payload) : d->relay.request(host, payload);
    if (!id.isEmpty()) {
        d->hostRequests.insert(id);
        QTimer::singleShot(30000, this, [this, id] { d->hostRequests.remove(id); });
    }
    return id;
}
}
