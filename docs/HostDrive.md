# Container objects and direct host drive access

iiSocietyHelper 0.9.0 exposes the registered host's container object and live
filesystem through its existing authenticated LAN TLS or account relay session.
The host must link iiSocietySync 0.8.1 or later. Client uses the public Container
SDK 0.16.0 interface; Container remains independent of Client and higher layers.

## Connection and container object

`Client::connectHost()` requires a signed-in AccountSession and an account with a
registered host/container. It does not open, create, lock or hydrate a local
replica. Account credential persistence retains its existing container-owned
state policy. `Client::start(localReplica)` remains available for synchronization
and durable selective downloads; both modes expose direct host access.

On the registered host device, the account device ID equals the host ID. Client
opens the selected native container after verifying its UUID, account scope and
primary-host binding. It uses `Controller::openHostAccess()` on the storage worker
without creating a self peer, listener or second indexer. The same drive methods
work for apps on the host computer and for apps on other devices. An unavailable
or mismatched local selection fails without registering another peer with the
host's ID. `requestHost(society.generation)` retains its network-host-only behavior;
the local direct session exposes drive operations.

`container()` returns a borrowed `iiSocietyContainer::Container*` owned by Client
with a stable address. It provides `containerId`, `aliases`, `alias(key)`, recent
file filters and `refresh()`. `hostSections()` returns all nine section keys/names.
These properties can be bound through the app's existing GUI adapter.

Aliases contain files and their verified native modification times when the host
projection still matches the indexed version. Directories are navigated through
`listHostDirectory`; they do not appear in the Container's recent-file filters.

Connection and metadata readiness are distinct: `connected()` may be true while
`container()->available()` is false. Refresh negotiates drive protocol 1, reads
revision-bound catalog pages and publishes one complete snapshot atomically.
Revision changes, malformed responses or more than 100,000 catalog entries fail the
refresh. Call `refreshContainer()` or `container()->refresh()` to retry. Connection
and replica synchronization completion also trigger refreshes.
Catalog cursors follow UTF-8 binary ordering and must be preserved as opaque values.

The catalog is committed metadata and can lag external edits while indexing runs.
Direct list/stat/read operations access the live filesystem independently of it.
Host-view Aliases use `exists=true`, `available=false` and empty local source/preview
URLs. A `society://host/<hostId>/<containerId>` root identifies the host container; it is not a
native file path or OS mount. Use direct reads for bytes, or StorageMap selective
downloads for a local file. Native stat/read versions differ from catalog hashes.

## Public operations

Methods return a request ID. An empty ID means local rejection because of
disconnection, invalid input or the limit of 16 pending requests. Accepted calls
finish asynchronously with `hostDriveResponse(id, reply)`. Always check `ok` and
`error`. Callers cannot select another host, URL or credential.

| Method | Contract |
| --- | --- |
| `listHostDirectory(path, cursor, version)` | Empty path lists nine sections. Other directories return at most 128 entries and 240 KiB of metadata, `nextCursor` and `version`. Pass both cursor and version to continue; a changed directory fails. |
| `statHostEntry(path)` | Live `path`, `kind`, decimal-string `size` and native `version`, for section directories or entries. |
| `readHostFile(path, offset, version)` | At most 256 KiB, base64 `data`, decimal-string `offset`/`size`, `version` and `eof`. First read may omit version; pin the returned version for subsequent chunks. |
| `writeHostFile(path, bytes)` | Atomic create-only publication of at most 256 KiB in an existing directory. Existing targets are preserved. Larger uploads use the existing replica submission path. |
| `createHostDirectory(path)` | Creates one immediate child in an existing directory, preserving existing entries and section roots. |

Paths are logical section keys such as `files/Documents/note.txt`,
`models/model.safetensors`, `photos/image.jpg` and `generation-history/output.png`.
Preserve returned native Unicode spelling. Access is confined to the selected
container's nine public sections, including its configured Files volume.
Absolute paths, traversal, backslashes, private state, lease sentinels and
symlink/junction escapes are rejected. Host OS paths and credentials are never
returned as usable file paths.

```cpp
#include <iiSocietyHelper/Client.h>
iiSocietyHelper::Client client(account, discovery); // Existing signed-in account.
QObject::connect(&client, &iiSocietyHelper::Client::hostDriveResponse,
                 &app, [](const QString &id, const QJsonObject &reply) {
    if (!reply.value("ok").toBool()) return; // Surface reply["error"].
    if (reply.contains("data")) {
        const auto bytes = QByteArray::fromBase64(reply.value("data").toString().toLatin1());
        // Continue at offset + bytes.size(), pinning reply["version"].
    }
});
QString request;
QObject::connect(&client, &iiSocietyHelper::Client::changed, &app, [&] {
    if (client.connected() && request.isEmpty())
        request = client.readHostFile("models/model.safetensors", 0);
});
client.connectHost();
```

## Authorization, lifecycle and verification

The `drive.*` messages travel inside `society.sync` protocol 2 / namespace version
1. Controller verifies authorized peer, account scope, container UUID and exact
request token, then performs I/O on its replica worker. A mirror cannot be the
primary host. Existing Society dispatch through `Controller::handle()` works when
the host links the updated Sync SDK; there is no additional listener or weaker
pairing mode. Pending polls reuse the exact envelope/token.

Explicit `sync_store_busy` and `sync_endpoint_busy` rejections occur before
filesystem execution. Client retries only these rejections with a fresh token,
retaining the public request ID and original deadline. A cached accepted or
pending operation is never given a new token or replayed after transport failure.

Each request has a 30-second client deadline and is not automatically replayed
after failure. If a timeout/disconnect follows a host commit, a create operation's
outcome can be uncertain: reconnect and stat before retrying. Stop, logout,
account/host/container changes and connection loss invalidate the mirror and
complete pending public calls with `host_disconnected`.

Tests cover native loopback LAN TLS and account relay without a Society app
process or client replica, multi-chunk reads, creation, metadata refresh,
cancellation, confinement, paging, changed versions, unauthorized peers/scopes
and exact-token retries. These tests do not establish production-host, physical
mobile-device or non-macOS execution.

Direct writes use the existing leased `.society-write-*` staging convention.
Container Maintenance can remove staging abandoned by an interrupted process;
active writes and successfully published files remain protected. The host-drive
tests cover orphan reclamation and preservation of the published file.

The combined client integration suite allows 120 seconds for direct and replica
sessions over both LAN and relay. This aggregate runner limit does not extend
the 30-second SDK request deadline or individual transfer/test assertions.
