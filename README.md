# iiSocietyHelper

Version 0.10.1 is the single Society integration SDK: account/client access,
local IPC, storage/media operations, device synchronization, native monitoring
and local/remote generation. See [consolidation and component usage](docs/Consolidation.md).

The default `iiSocietyHelper::iiSocietyHelper` target includes synchronization and
remote generation. `iiSocietyHelper::Monitoring` remains a Qt-free C++23 native
component. Desktop hosts select `iiSocietyHelper::GenerationHost`; mobile clients
use the remote generation API without linking the desktop inference host.


[Windows 빌드 및 실행](docs/WindowsBuildAndRun.md)

Package release: **0.10.1**. Earlier versioned feature and verification notes
below describe their original releases. Current package ownership and build
components are defined in [Consolidation.md](docs/Consolidation.md).


Version 0.9.3 adds `FileDownload` for explicit native `.societycontainer`
hydration within the consuming application. It reads the selected drive through
`FileSystem`, uses a dedicated `Client::connectHost()` session for missing bytes,
and runs the replica's bounded chunk writes, SHA-256 verification and atomic
publication on its own worker. It never creates a shared download-request file,
waits for `client-session.lock`, starts Society, or sends a local IPC request.
Direct host sessions have an independent authenticated session identity so a
consumer does not replace another app's device-level LAN or relay connection.
`state` reports preparing/connecting/downloading/verifying/ready/failed/cancelled
with file identity and byte progress. `cancel()` retires the pending open intent;
`ready` contains a fresh, locally available Alias. Drive changes and stale
revisions cannot publish an old selection. Resident files work without a login
or a Client. Network retrieval still requires access to the authenticated host.
The session integration tests exercise LAN TLS and account relay downloads while
another replica-session lease is held, cancellation/retry, integrity failure,
drive changes and native offline reads without a Society application process.

The current package requires iiFileProvider 0.7.1 or newer; synchronization is
included in Helper Core. Media export validates the source
format here, then delegates opening and final native URL publication to
`iiFileProvider::File`. Android document-picker destinations use the provider's
granted `content:` stream adapter. Existing destinations remain intact when
validation or a native stream transfer fails. Classified file libraries use the
separate native `iiFileProvider::Catalog` metadata/collection/search API; generic
single-stream export does not implicitly scan or register the user's directories.

Version 0.9.0 consolidates the former iiSocietyClient module into this SDK and builds and exports an exact iiSharedCanvas 0.31.0 dependency,
which uses iiPaintEngine 0.4.1. Canvas documents cross the Helper API by value;
applications and Helper must use the same canvas headers and library version.

Use the single `iiSocietyHelper::iiSocietyHelper` CMake target for local app
collaboration, native file/canvas access, account sessions, discovery, pairing,
replica synchronization and authenticated host-drive access. The consolidated
host API is `iiSocietyHelper::Client` in `<iiSocietyHelper/Client.h>`.
See [migration and dependency direction](docs/Migration.md),
[host access](docs/HostDrive.md) and [account and connection lifecycle](docs/Client.md).

## Container state and file ownership

`Helper::fileSystem()->container()` exposes an `iiSocietyContainer::Container`
owned by the bridge. It contains read-only Alias snapshots instead of application
copies of stored payloads. The bridge synchronizes the selected drive every two
seconds, on `synchronize()`/`Container::refresh()`, and after publication or save.
The selection remains pinned to the drive identity until explicitly reopened.
Since 0.9.5 the periodic desktop/native scan, monitor-triggered refresh and
`Container::refresh()` run on a bounded C++ I/O worker. Applications can request
the same nonblocking scan with `requestSynchronization()` and observe
`synchronizationFinished(bool)`. Repeated requests coalesce to one active scan
and one pending scan. Completed Alias values are published only on the owning
QObject thread; reopening the drive or calling synchronous `synchronize()`
invalidates earlier replies. Destruction joins the worker before releasing its
receiver. `open()`, explicit `synchronize()` and save/publication APIs keep their
synchronous success and durability contracts. Android's existing ContentProvider
adapter keeps its synchronous request context.
`containerStateChanged` reports daemon status independently of mirror completion;
observe `Container::stateChanged` or `synchronizationFinished` for completed Alias
snapshots instead of assuming that a daemon revision means a scan has finished.

Native mirror scans validate the pinned drive at both boundaries, resolve each
public section once, reject redirected resident leaves and read replica catalog
objects once per batch. They do not reopen and revalidate the drive for every
Alias or perform a linear duplicate search for every remote entry. The mirror is
still metadata only; original payloads and download-on-demand semantics remain
owned by the selected container.
Android clients synchronize this mirror through Society's existing ContentProvider
listing and retain its readable `content:` source/preview addresses. They do not
interpret provider addresses as native filesystem paths.

Use `alias(section, relativePath)` to distinguish logical existence from locally
available contents. A catalog-only replica entry does not create a zero-byte file
or directory. Resident payloads establish existence without reopening the catalog
for each file during synchronization. Raster Alias previews are supplied by the bridge; call `preview()`
for a native `.iisc` preview. Helper renders and caches it beneath the selected
container's private `.society-runtime/canvas-previews`, preserving the document.

`importCanvas()` and `createImageCanvas()` persist native image documents in Files.
`readCanvas()` supplies detached iiSharedCanvas data for application editing.
`saveCanvas()` owns native file creation and updates, accepts encoded document
snapshots, and rejects stale versions before changing stored contents. New native
documents use the SDK's SQLite authoring format; imported serialized canvases are
updated by compare-and-replace. The returned Alias carries the saved version.
Applications never receive a writable DocumentFile binding.
Helper's native write locks are hidden implementation files and are never
published as document Aliases or recent files. The saved version is captured
while holding the write lock, before releasing it and refreshing the mirror.

PNG generation uses `publishGeneratedImage()` in Generation History.
`publishFile()`, `saveFile()` and `exportMedia()` own other publication/export I/O.
Model resolution, file download requests, generation-input cleanup and legacy
cache/queue migration also pass through this bridge. Native canvas writes inside
Society belong to Files. Explicit external export remains supported.

`<iiSocietyHelper/MediaExport.h>` also exposes the stateless
`iiSocietyHelper::exportMedia(source, destination, video)` GUI file-picker adapter.
It validates image/MP4 input and copies the original bytes through iiFileProvider,
including Android document URLs. Its `MediaExportResult` contains success/error;
the caller owns scheduling. It never creates a FileSystem, opens a selected drive,
starts observation or refreshes a mirror. Use it for external Save to File work
that has no drive-state owner. The existing FileSystem member remains compatible
and refreshes its explicitly owned drive after a successful export.

The same GUI adapter header exposes `mediaExportDestination(destination,
nativeCanvas)` for checked file-picker paths and `exportCanvasCopy(document,
destination)` for detached native data. The former retains Society section and
redirect checks without constructing a FileSystem; native copies remain limited
to Files inside Society. The latter creates a new SDK authoring file directly,
rejects occupied names, and requests only that file's provider preview. It does
not round-trip through encoded canvas bytes, activate the selected FileSystem, start a
directory observer, or refresh a mirror. Callers own worker scheduling and must
report its terminal success/error separately from accepting the request.

Canvas operations depend on iiSharedCanvas 0.31.0 and iiSocietyContainer 0.16.0.
Supply the installed iiSharedCanvas codec dependencies, including libzip, in the
CMake prefix when configuring a standalone Helper consumer. Filesystem tests
exercise two independent bridges, Alias deletion, remote-only objects, real native
save/reopen/preview and conflicting document versions.

A C++23/Qt 6.8.3 library for **native app collaboration with local or remote Society storage**. Version 0.8.0 adds container Alias synchronization and Helper-owned canvas persistence and previews to C++ object delivery, account-object references, local Society file-system access, app observation, and persistent IPC. Society communication and container replication across devices are the responsibility of iiSocietyHelper::Sync. The Client API in this library performs account-scoped discovery, pairing and host requests; iiSocietyHelper::Sync owns the underlying replication and transport protocol.

The local app can queue messages and use files even while Society is stopped. Society or the device's daemon receives the message. Account references and object snapshots are not network authenticated and are not automatically propagated via Sync. Host sessions use the Sync implementation inside Helper Core. Native Monitoring
is a separate component of this same package and never imports the Core facade. Local observation and delivery do not start a network session.

Survival signals, delivery SQLite ·ACK are not replication targets. Specifying observation and message directory inside a Society container or a known network file system causes rejection before file creation. `delivery` regression inspection checks this boundary and existing same-device delivery and re-execution behavior.

<a id="앱에서-사용하기"></a>

## Use from app

```cmake
find_package(iiSocietyHelper 0.10.1 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE iiSocietyHelper::iiSocietyHelper)
```

```cpp
#include <iiSocietyHelper.h>
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    iiSocietyHelper::Helper helper;
    QObject::connect(&helper, &iiSocietyHelper::Helper::peerAppeared,
                     &app, [](const iiSocietyHelper::Peer &peer) {
        qInfo() << peer.application.id << peer.instanceId;
    });
    if (!helper.start({"com.iisacc.example", "Example", "1.0.0"})) {
        qWarning() << helper.errorString();
        return 1;
    }
    return app.exec();
}
```

The Helper is created after `QCoreApplication` and created and called from the app's event loop thread, and must remain alive during observation. Merely linking a DLL does not start observation. Call `start()` at the actual app entry point. At termination, `aboutToQuit`, destructor, or explicit `stop()` removes its own record. Upon restart after `stop()`, a new instance ID is issued. Duplicate start calls return an error while maintaining the existing observation. To remove an object in signal handling, use Qt's `deleteLater()`.

Society and Dreamscapes' LVRS `configureEngine` link this lifetime. Qt Quick app can link to `QGuiApplication::applicationStateChanged` at `setActivity()`. `Activity` is Unknown/Foreground/Background and is not treated as a dead app solely because it lost focus.

<a id="계정-객체-참조-060"></a>

## Account object reference ( 0.6.0 )

Connects the `iisacc::accounts::AccountManager` used in the app to the `helper.setAccountManager(&accounts)`. The actual package and header names are the same `iiAcountManager` as the existing repository. The Helper does not copy or own the manager and does not log in; the login is performed by the connected manager. The reference is used from before the observation `start()` and is maintained even after `stop()`.

```cpp
#include <iiSocietyHelper.h> // Also includes the public headers for AccountManager and Account.

iisacc::accounts::AccountManager accounts;
iiSocietyHelper::Helper helper;
if (!helper.setAccountManager(&accounts)) return;
QObject::connect(&helper, &iiSocietyHelper::Helper::accountChanged, &helper, [&] {
    const auto *user = helper.account();
    if (!user || !user->isPresent()) return;
    const QString userId = user->userId();
    const auto *author = user->authorDetails();
    const QVariantMap allValues = user->toVariantMap();
});
accounts.loginWithPassword(email, password);
// Call accounts.verifyEmailCode(code) after accounts.verificationRequired().
```

|API / Qt property|Contract|
| --- | --- |
| `setAccountManager(AccountManager*)` |Connects the manager in the same thread. `nullptr` is disconnect. It can also be called from QML.|
| `accountManager()` / `accountManager` |Is the manager object itself that was connected. After being disconnected or destroyed, it is `nullptr`.|
| `account()` / `account` |Is the manager's fixed `Account` object itself. If not connected, it is `nullptr`; if connected but before login, it is `present == false`.|
| `accountManagerChanged()` |Notifies of connect, replace, disconnect, and manager destruction.|
| `accountChanged()` |Notifies of reference replacement and account and nested author data changes. It sends after the entire changed value is reflected.|

Reads 11 fields of `Account`'s identity, profile, membership, consent, and container drive information, and 20 fields of `AuthorDetails`, and the type-specified links and identifier collection as is. In QML, `societyHelper.account.userId`, `societyHelper.account.authorDetails.organization`, and `societyHelper.accountManager.state` can be used. When there is no connection, it checks `societyHelper.account` first. The account change notification is distinguished from the login request status change, and the login status uses the manager's `state` and `stateChanged`.

If the same manager is set again, there is no duplicate notification. If replaced, the notification connection of the previous manager is broken, and when the manager is destroyed, both references are automatically cleared. Multiple Helpers can reference the same manager. Helper release and destruction do not perform the manager's logout or account initialization. `setAccountManager()` is called from the Helper's event loop thread and the manager is also maintained in the same thread. Other threads return `false` without changes. Even if replaced with another reference or the Helper is destroyed during change notification processing, it returns `false` without sending subsequent notifications of the previous reference.

This connection is a QObject reference within the same process. It does not automatically save or broadcast account data and authentication cookies to the observation record or delivery queue, and does not change file system access permissions, storage location, or remote synchronization policy.

The dependency direction is `iiSocietyHelper → iiAcountManager → Qt Core/Network`. It reuses the public API of the account SDK 0.2.x managed by the existing Workspace. There is no duplication of the account model or login implementation, nor the introduction of a new external package. The CMake target of the Helper exposes the account SDK and also finds the installation settings at `find_dependency`, so the consumer only needs to link the Helper. The account SDK does not reference the Helper or the Container.

<a id="society-파일-시스템-접근-040"></a>

## Society file system access ( 0.4.0 )

Generated image originals use
`fileSystem()->publishGeneratedImage(sourcePng, "<job UUID>-0001.png")`.
This delegates to iiSocietyContainer's atomic, no-replacement PNG publication in
the selected drive's flat `Generation History/`. It preserves PNG bytes, reports
failure through `errorString()`, and works without `start()` or a Society daemon.
The caller validates image decoding and dimensions. Storage selection stays
pinned until `open()`/`refresh()`. Generation does not create `.iisc`; the editor
creates its native document when the user opens an image for editing. This
native-path operation is unavailable to an Android content-URI-only peer.

`Helper` 's `fileSystem()` automatically opens the Society original at creation and continues to use it before `start()` and after `stop()`. Linking only the Helper connects iiSocietyContainer 0.9.0 and Qt are also connected. In the existing QML context, it is used as `societyHelper.fileSystem` .

```cpp
auto *storage = helper.fileSystem();
const auto directory = storage->ensureDirectory("files", "Example/session-1");
if (directory.isEmpty()) { qWarning() << storage->errorString(); return; }
const auto path = storage->path("files", "Example/session-1/result.json");
if (path.isEmpty()) { qWarning() << storage->errorString(); return; }
QSaveFile file(path); // #include <QSaveFile>
const QByteArray bytes = "{\"status\":\"saved\"}";
if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    qWarning() << file.errorString();
```

`path()` is an actual absolute path that `QFile`, `QDir`, `std::filesystem`, and external engines can read and write. `url()` is a local file URL preserving spaces, Korean characters, `#`, and `%`. A QML example is `societyHelper.fileSystem.url("models", "weights.safetensors")`. `sections` is a list of `{key, name, path}`; the keys are `asset-library`, `deleted`, `files`, `forked`, `generation-history`, `models`, `photos`, `published`, and `thinking-space`. `photos` in iiSocietyContainer 0.13.0 uses the top-level `Photos/` path. `filesystem` regression checks verify this path and reads/writes in 9 sections between independent apps.

- The priority of `open()` is the explicit original path, `SOCIETY_CONTAINER_PATH` , and Society common settings. iOS uses the existing Society App Group original. The Helper does not create a new container or change the global default selection.
- A successful selection is fixed to the path and UUID. Even if the default storage changes, the target being worked on is maintained. `refresh()` re-applies the current selection method, and `open(path)` returns to the explicit path, while `open()` without arguments returns to the default selection. If selection fails, it does not use the previous storage.
- If there is no initial storage, it returns an empty path and `fileSystem.errorString` and retries on the next path request. If the opened drive disappears or is replaced, it does not switch to a new UUID before explicit re-selection.
- `available` , `rootPath` , `containerId` , and `sections` are validated upon query. `storageChanged` notifies invalidation detected during selection, re-selection, and access. Since there is no continuous disk monitoring, it calls `refresh()` upon screen return and refresh. File errors are separate from the `Helper.errorString` of observation and delivery, and observation is not stopped.
- `path(key)` is the area directory. The relative path returns everything except the last filename and prepares the parent as `ensureDirectory()` first. Hidden items are supported. Absolute paths, `..`, `.`, empty intermediate elements, backslash, colon, NUL, symbolic links, and junctions are rejected. Path lookup does not create files.

Apps share the same original bytes. Finder, iOS, and File apps expose only `Files/` as before, while the Helper app uses 9 area. Path return does not guarantee file locking or I/O success. Actual errors are handled by the file API, and concurrent editing applies the app's lock and conflict policy. Since the file system may change after return, the path is retrieved again right before the operation. Remote synchronization and OS sandbox permission granting are separate.

iOS consuming apps require the same App Group signature as the existing `iiSocietyContainer_configure_ios_client()` configuration. macOS sandbox apps must also use the original allowed by the OS. The private app directories for Android and WebAssembly are not public storage, and the host must explicitly specify the shared original. Android apps do not provide default shared storage or IPC between apps. [Apple App Group](https://developer.apple.com/documentation/xcode/configuring-app-groups)follows the shared container contract.

Dependency flows from Helper to Container to Qt Core. Discovery, UUID, area, and path checks are managed within the same Workspace, reusing MIT iiSocietyContainer, and no additional servers, drivers, or external libraries are introduced. General I/O uses existing [QFile](https://doc.qt.io/qt-6.8/qfile.html), [, QSaveFile, and](https://doc.qt.io/qt-6.8/qsavefile.html), while Qt license follows the installed version. Implementation is at the root `src/FileSystem.cpp`, and the public API is at `src/iiSocietyHelper.h`.

`iiSocietyHelper.filesystem` and install consumers check standard C++ write to Qt read/modify to standard C++ re-read, 8 areas, rename/delete, URL, invalid paths, late storage preparation, selective pin/unpin, and UUID replacement. All files are confined to `build/`'s temporary container.

`build/install/bin/ii-society-helper --filesystem` outputs the current original's path, UUID, and 8 areas as JSON without starting observation or database. If no default storage exists or it is invalid, it returns an error and exit code 1. The explicit original is specified as `SOCIETY_CONTAINER_PATH`. QML expressions and the read-only behavior of this diagnostic command are also verified by the install consumer.

<a id="관측-계약"></a>

## Observation contract

- `ApplicationInfo`: fixed reverse- DNS ID, display name, and version that distinguish the app.
- `Peer`: app info, per-run UUID, PID, foreground/background status, start time, and the time the last signal was acknowledged. PID is not used as the identifier key to distinguish multiple runs of the same app like PID reuse.
- `peers()` and `observedApplications` for QML contain current observable instances except itself, sorted in order UUID.
- It provides `peerAppeared`, `peerUpdated`, `peerDisappeared`, and `peersChanged`. It does not repeat metadata change events if only the alive signal is updated.
- `DepartureReason::Withdrawn` means the record has disappeared or become invalid, and `TimedOut` means the signal is disconnected. It is not a state that definitively determines the app's termination cause or OS process death.
- It does not judge that the app is running just by reading the stored file. It emits a discovery event only after the heartbeat sequence number increases. Records remaining after forced termination or reboot are not discovered, and files of other Helpers are not deleted.
- The default survival signal interval is 1 seconds, and observation expiration is 5 seconds. Discovery usually requires one or two renewals. It may be delayed according to OS scheduling. Participants changing the interval via `ObservationOptions` must use a timeout that accommodates each other's periods. Mutual discovery/interruption and recovery integration checks also use the actual default expiration time of 5 seconds. Do not mistake external volume file flush delay for a normal running participant's expiration, and the recovery check actually waits 5.5 seconds to stop the event loop to verify both expiration and new heartbeat rediscovery. Separate process checks also use a 5 second expiration, and the restart process runs for 10 seconds to observe until the normal termination event after confirming discovery.
- Recurring presence publication, directory validation and peer-record reads run
  on one private standard C++ worker. Timer and filesystem-watcher requests are
  coalesced while an operation is active; they do not queue an unbounded backlog
  or wait for disk flushes on the application's event loop. Peer collections and
  signals remain on the Helper's owning thread. Session epochs discard late
  results, and `stop()` drains active work before removing its own record, so an
  old heartbeat cannot recreate a withdrawn session. Initial `start()`, explicit
  `stop()` and durable `sendData`/`sendObject` remain synchronous. Atomic record
  publication, owner-only permissions and runtime-directory checks are retained.
  `iiSocietyHelper.presence_worker` covers an intentionally blocked operation,
  and `ObservationTests::presenceResponsiveness` reports event-loop latency
  with real presence I/O; its timings depend on the current host load.
  Automatic activity/peer history is committed on a separate lazy worker and drained
  during normal `stop()`. Every transition retains its own durable outbox record;
  only redundant presence/scan requests are coalesced. `dataQueued` is delivered
  on the owning thread after each commit, including a completed old session's
  notification. Forced termination may lose automatic events not yet committed.
  Explicit `sendData` and `sendObject` still commit before returning to callers.
  Slow history commits do not delay presence heartbeats. Unstarted storage-only
  Helpers allocate neither presence nor history worker threads.
- Expiration uses the monotonic clock inside the observer. The record's date or file modification time is not evidence of being alive. If the observer itself stops for more than the timeout and then returns, it checks for a new signal again.
- If execution fails to access the directory or record, it switches to `running=false` and provides `errorOccurred` / `errorString`. After resolving the cause, it can restart at `start()`.

This is confirmation of the existence of cooperating apps. It does not include searching all installed apps, collecting screen and document content, controlling processes, passing commands between apps, or discovery and synchronization between devices. App IDs are declared by participants, so they are not used for signature verification or authentication means.

<a id="c-객체-전송-070"></a>

## C++ object transfer (0.7.0)

`Helper::sendObject(topic, object)` synchronously serializes the current value of the object and puts it into the existing persistent delivery queue. On success, it returns message UUID, and on failure, it returns an empty string and `errorString()`. `dataQueued(id)` occurs after the actual queue insertion succeeds and one time. Modifying the original value, QObject deletion, or terminating the sending app after successful transmission does not change the value stored in the queue. First, configure the sending app and observer directory with `start()`. During the call, if the getter or serialization operator stops or restarts the Helper, the object is not put into another sending instance name but fails, and even if the Helper is destroyed, it does not perform the remaining object transmission.

|Transfer target|Transmit API|Restoration result|
| --- | --- | --- |
| `QObject*` / `const QObject*` / `const QObject&` | `sendObject(topic, object)` | `ObjectSnapshot { QString className; QVariantMap properties; }` |
|Qt value·registered C++ struct/class| `sendObject(topic, value)` |Original Qt metatype preserved `QVariant`, `value<T>()` to C++ value restoration|
|Already held `QVariant`| `sendObject(topic, variant)` |Type and content of the held value|
|Existing JSON value bundle| `sendData(topic, map)` |Existing `QVariantMap` contract|

QObject uses the explicit storage contract if `Q_INVOKABLE QVariantMap toVariantMap() const` is present. Otherwise, it reads readable `STORED` Q_PROPERTY including inherited user properties. QObject default `objectName`, dynamic properties, and `STORED false` properties are not automatically collected. Nested QObject properties become nested `ObjectSnapshot` and null references become null values. Circular references are rejected. Objects must be read from their own thread, and getters must return valid value·references. The original QObject and execution method·ownership·memory address are not transmitted. If an instance of the original QObject class is needed, the receiving app uses the snapshot and that class's create·update API.

The account model uses the existing full storage contract as is. After login, it holds 10 account fields and 20 author information, and does not automatically transmit via account reference connection alone, but transmits explicitly in the next call.

```cpp
// Call on the same event-loop thread after helper.start(...) and login completion.
helper.setAccountManager(&accounts);
const QString id = helper.sendObject("account.profile", helper.account());

// Receiver: this is the corresponding message obtained from DeliveryStore or SocietyInbox.
QString error;
const QVariant decoded = iiSocietyHelper::ObjectCodec::decode(message["payload"].toMap(), &error);
if (decoded.metaType() == QMetaType::fromType<iiSocietyHelper::ObjectSnapshot>()) {
    const auto snapshot = decoded.value<iiSocietyHelper::ObjectSnapshot>();
    iisacc::accounts::AccountManager reader;
    if (snapshot.className == "iisacc::accounts::Account")
        reader.readAccount(snapshot.properties);
}
```

The account restored in this way is the received profile value. It does not issue login session and server authority. QObject can also be directly transmitted as `societyHelper.sendObject("account.profile", societyHelper.account)` in QML.

General C++ value objects must be copyable·default-constructible, and transmit·receive apps must share the same metatype and QDataStream operator. The next declaration·operator is placed in a common header, and the receive app also calls `qRegisterMetaType`.

```cpp
#include <QDataStream>
#include <QMetaType>
#include <QString>

struct GenerationRequest { QString prompt; quint64 seed = 0; };
inline QDataStream& operator<<(QDataStream& out, const GenerationRequest& value) {
    return out << quint32(1) << value.prompt << value.seed;
}
inline QDataStream& operator>>(QDataStream& in, GenerationRequest& value) {
    quint32 version = 0;
    in >> version;
    if (version != 1) { in.setStatus(QDataStream::ReadCorruptData); return in; }
    return in >> value.prompt >> value.seed;
}
Q_DECLARE_METATYPE(GenerationRequest)

// Run in both processes at app startup.
qRegisterMetaType<GenerationRequest>();
GenerationRequest request{"a forest", 18446744073709551615ULL};
const QString id = helper.sendObject("generation.request", request);

// Check the topic in the receiving app before processing it.
QString error;
const QVariant value = iiSocietyHelper::ObjectCodec::decode(message["payload"].toMap(), &error);
if (value.metaType() == QMetaType::fromType<GenerationRequest>()) {
    const auto restored = value.value<GenerationRequest>();
}
```

User value types including Q_GADGET also follow the same stream operator contract. User value types contained in QObject must also be registered on the receiver side. Arbitrary C++ members are not automatically serialized by meta-type registration alone. User operators define field, schema version, and internal collection limits. `ObjectCodec::encode()` / `decode()` can be used separately from transmission. Unregistered receiver types, unsupported stream operators, primitive and QObject smart pointers within QVariant, truncated data, trailing bytes, incorrect Base64, and unsupported protocol versions are explicitly rejected. Failed objects are not left in the queue as partial messages.

The payload consists of five fields: `format: "iisacc.qt-object"`, `version: 1`, `streamVersion: 22`, `typeName`, and `data`. `data` is the Base64 of Qt 6.8 QDataStream(BigEndian, DoublePrecision) bytes. This format preserves QByteArray, 64 signed and unsigned integers, QDateTime, QUrl, and user value types. Primitive stream writing is limited to 48 KiB, and the final payload including **metadata and Base64 follows the existing 64 KiB limit**. The QObject snapshot being checked and QVariantMap/List/Hash apply a nesting depth 16 and node 4096 limit. Large objects use repository files and references.

The existing outbox/inbox, daemon receive, retry, and acknowledgment locations are used as is, without changing the DB schema. The daemon carries the object payload, and the receiving app restores it at `ObjectCodec::decode()`. No new external library or serialization engine is added by reusing the currently installed Qt Core's QMetaObject, QMetaType, and QDataStream. A separate process check and installation consumer verify restoration after send completion using the same API and public headers.

<a id="society-데몬으로-데이터-전달"></a>

## Data transfer via Society daemon

`Helper::sendData(topic, payload)` places a JSON object of up to 64 KiB in the persistent outgoing queue and returns a message UUID. An empty string indicates a write failure; check `errorString()`. A successful return means **pending data has been stored on disk**, which is distinct from completed receipt by the daemon. Pass references to assets stored in Society rather than the large models or images themselves.

```cpp
const QString messageId = helper.sendData("generation.queued", {
    {"jobId", jobId}, {"modelPath", modelPath}
});
```

The Helper also automatically records events `helper.started`, `helper.activity`, `helper.peerAppeared`, `helper.peerUpdated`, `helper.peerDisappeared`, and `helper.stopped`. History is not added for each survival signal. Even if the daemon is off, it remains in the send queue, and even if the Helper terminates, it does not disappear.

Even if the first execution of multiple apps overlaps, process-level locking is used only for initialization to avoid conflicts with the initial schema and WAL settings. Normal send and receive use SQLite transactions.

Opening a committed schema-1 WAL store does not acquire the initialization lease.
It applies connection-local FULL synchronization and owner-only permissions, while
retaining path and DB/WAL/SHM symlink validation. First creation and journal/schema
setup still use the bounded initialization lease and re-read database state after
acquisition. Driver loading occurs before that lease. Delivery regressions hold
an initialization lease over an already ready store and require successful open
with retained data; they also verify that a new schema cannot be created under
another initializer's lease and that unsupported schemas remain rejected.

The `delivery/delivery.sqlite` of the shared location contains the outbox, inbox, consumer acknowledgment location, and the last daemon snapshot. `DeliveryStore::receivePending()` moves the send data to receive by means of a single SQLite transaction. WAL and FULL synchronization and message ID uniqueness constraints are used, and if the transaction fails, the send data is held. Even if the process is terminated and processed again, the same ID is not stored redundantly. Errors returned by the Qt SQL driver and file system are passed to the caller.

Society product's standalone executable `SocietyDaemon` performs this task. The app body receives data in sequence through `SocietyInbox`, and provides `dataReceived(QVariantMap)` ·recent 100 messages and page read API. `DeliveryStore::readAfter(sequence, limit)` allows reading past data received by the daemon as well. The daemon's last snapshot is the state at the recorded time and is not proof of current survival.

Reading does not delete messages. The consumer must call `acknowledge(consumerId, sequence)` after finishing processing to start from after that upon re-execution. The checkpoint position does not go back and cannot check sequence numbers not yet received. Re-execution before confirmation can re-deliver the same message, so the consumer avoids duplicate processing using the message ID. Since the retention period is not yet defined, received records are not automatically deleted.

Container-owned delivery state replaces the previous App Group Application Support default. The App Group remains an OS entitlement for participating apps; no new observation data is placed there by default. The Caches location in 0.2.0 contained no persistent delivery data, so observation records are rediscovered rather than migrated. iOS cannot run a permanent separate daemon, so the same receiving service runs while Society is running, and the shared outbound queue retains data while the app is suspended. Physical iOS device verification is separate.

References for additional dependencies: the [Qt SQL driver](https://doc.qt.io/qt-6.8/sql-driver.html), [SQLite WAL](https://sqlite.org/wal.html), and [SQLite synchronization settings](https://sqlite.org/pragma.html#pragma_synchronous). Qt and SQLite components follow the licenses of the existing distributions.

<a id="저장-위치와-플랫폼"></a>

## Storage Location and Platform

The default observation location is the selected valid `.societycontainer/.society-runtime/helpers/v1`. Participants selecting the same package share observations and delivery state there. Resolving the default path creates no directory; without a selected valid package, startup waits without using an OS AppData fallback. Observation state and the delivery database are private container state and are excluded from the public Files projection.

iOS and Apple apps bundled with an App Group use `SocietyAppGroup` in Info.plist and the corresponding App Group entitlement. The path is `Library/Application Support/iiSocietyHelper/v1` in the group container and is not replaced with a separate private container for each app. It reuses the same `group.com.iisacc.society` contract set by the iOS app packaging function of existing iiSocietyContainer. An iOS app that is not configured returns a startup error. macOS sandbox apps also require the same App Group configuration.

Background apps of iOS can be suspended by the OS. Suspended apps cannot refresh signals and expire in the observer, and when execution resumes, they are discovered again. It does not guarantee continuous background execution or waking up other apps. Both apps must be scheduled simultaneously for both observers to proceed. The Apple implementation uses Foundation API. On the current host, it validates the compilation of App Group path code, and running two signed iOS apps on two devices requires separate validation.

Since 0.3.1, iOS builds as a static library. Static Qt apps include the SQLite driver through `qt_import_plugins(AppTarget INCLUDE Qt6::QSQLiteDriverPlugin)`. The shared delivery directory and existing DB/WAL/SHM use `NSFileProtectionCompleteUntilFirstUserAuthentication`, and new files inherit the directory's protection. Startup failures before the first unlock after reboot must be handled, with startup retried later. The host's `ios_directory_syntax` checks Foundation API types in this iOS branch. The app can disconnect with `stop()` when suspended and reconnect with `start()` on return. The Society app itself uses `SocietyRuntime` to manage this flow and failure retries.

Android and WebAssembly require a native selected package or an explicitly integrated shared observation transport. Provider-only Android paths do not become host filesystem directories. Default Android inter-app IPC is not included in this version. Current runtime validation is macOS.

`SOCIETY_HELPER_DIRECTORY` may select another private runtime directory only inside the currently selected package; outside overrides fail before creation. Low-level `ObservationOptions.directory` remains available for explicitly integrated device-local transports and isolated SDK tests. Society itself validates explicit paths against its selected package too. Public drive sections remain invalid observation locations because their content can be replicated. Do not use cloud-synchronized folders or network volumes for Helper transport. New final directories and records use owner-only access; existing permissions are preserved. Invalid, oversized, unsupported-version records and symbolic links are ignored.

<a id="구현과-의존성"></a>

## Implementation and Dependencies

`QSaveFile` from the existing Qt Core atomically records `<UUID>.json` for each run, while `QFileSystemWatcher` and timer polling are used together. Records are reread periodically even if watcher notifications are coalesced or missed. Data delivery additionally links Qt Sql and the QSQLITE driver from the existing Qt distribution. No separate message broker or external server package is installed. Maintained SQL APIs in Qt 6.8.3 and SQLite transactions reduce the recovery burden of a custom file journal. Qt usage and redistribution follow the installed distribution's license.

The public header is at `src/iiSocietyHelper.h` of the root, and the implementation is placed at `src/Helper.cpp`, `src/ObjectCodec.cpp`, `src/FileSystem.cpp`, and `src/iiSocietyHelper.cpp` of the same root. Only Apple path resolution is at `src/platform/apple/ObservationDirectory.mm`. The previous `helloWorld()` symbol is maintained for existing consumer compatibility.

References: [Qt shared storage locations](https://doc.qt.io/qt-6.8/qstandardpaths.html), [QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html), [QFileSystemWatcher](https://doc.qt.io/qt-6.8/qfilesystemwatcher.html), [monotonic clock](https://doc.qt.io/qt-6.8/qelapsedtimer.html), [Apple App Groups](https://developer.apple.com/documentation/xcode/configuring-app-groups), and [iOS background execution](https://developer.apple.com/documentation/xcode/configuring-background-execution-modes).

<a id="빌드검증설치"></a>

## Build·Verify·Install

Requires CMake 3.24 or later, C++23, iiSocietyContainer **0.16.1** or later,
iiAcountManager **0.2.8** or later, iiSharedCanvas **0.31.3** Core,
iiServerHost **0.6.0** or later, iiFileProvider and its Temporary/CoreIO packages,
Qt **6.8.3** Core/Gui/Sql/Network, and the QSQLITE driver.
BLE uses Qt Bluetooth. Desktop GenerationHost additionally requires iiLocalDiffusion. Building the account SDK itself requires CMake 3.31 or later. Tests require Qt Test/Qml of the same version; Apple builds require Foundation and an Objective-C++ compiler. All artifacts are placed in `build/`.

```sh
CMAKE_PREFIX_PATH="/Volumes/Storage/Workspace/SDK/iiSocietyContainer/build/install;/Volumes/Storage/Workspace/SDK/iiAcountManager/build/install" \
  INSTALL_PREFIX="$PWD/build/install" ./install.sh
```

This script verifies the actual observation of an independent consumer that uses only public headers and libraries installed at `build/consumer/build/` after build·CTest·install. If `INSTALL_PREFIX` is omitted, it uses the default installation location `$HOME/.local/SDK/iiSocietyHelper` of the existing SDK. This task verifies the `build/install` installation of the Workspace.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/Volumes/Storage/Qt/6.8.3/macos \
  -DiiSocietyContainer_DIR=/Volumes/Storage/Workspace/SDK/iiSocietyContainer/build/install/lib/cmake/iiSocietyContainer \
  -DiiAcountManager_DIR=/Volumes/Storage/Workspace/SDK/iiAcountManager/build/install/lib/cmake/iiAcountManager \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

The test checks mutual discovery of three Helpers, multiple instances of the same app, state changes, normal termination, remaining records after forced termination, restart, process suspension and resumption, observer event loop suspension and resumption, incorrect settings, and directory boundary and isolation. Separate process tests actually run three executables. `iiSocietyHelper.account` and `iiSocietyHelper.installed_account` check object identity, full model access, update, initialization, replacement, destruction, thread boundary, QML reference, and observed data separation by connecting to the actual account SDK.

In the 2026-09-09 0.6.0 account-reference verification, the Helper Release build and CTest **7/7**passed, as did CTest **5/5**in `build/account-reference/consumer`, which linked only against the `build/install` installation. Account-reference checks verified 9 cases on each run (11 passes including initialization and cleanup). The consumer's CMake finds and links only Helper, resolving iiAcountManager 0.2.0 as a dependency from `SDK/iiAcountManager/build/install`. `DYLD_LIBRARY_PATH`, `DYLD_FRAMEWORK_PATH`, and `DYLD_FALLBACK_LIBRARY_PATH` were removed at runtime. The result XML files are `build/account-reference-tests.xml` and `build/account-reference/consumer/account-reference-installed-tests.xml`. This reference verification does not include actual login requests, production deployment, or execution on Android/iOS devices.

In the 2026-09-09 0.7.0 object-transfer verification, the Helper build and CTest **8/8**passed, as did CTest **6/6**in `build/object-transfer/consumer`, freshly built with the public headers and libraries from `build/install`. Object-transfer checks verified 12 cases on each run (14 passes including initialization and cleanup). They checked round trips for C++ value types, 64-bit integers, binary data, dates, and URLs; snapshots of QObject properties and the full account model; rejection of invalid formats, sizes, cycles, and threads; object destruction during getter execution and Helper restarts; and QML calls. Separate processes confirmed that the receiver restores objects from the persistent queue after the sender exits and reads the same message values on relaunch. The consumer resolved iiAcountManager 0.2.0 from the Workspace installation, with the three `DYLD_*` variables above removed at runtime. The result XML files are `build/object-transfer-tests.xml` and `build/object-transfer/consumer/object-transfer-installed-tests.xml`. This verification covers local inter-process delivery on macOS; it does not include production deployment, actual login, or execution on Android/iOS devices.

Install the diagnostic executable as well. If executed from the same location in two terminals, you can check discovery, modification, and exit events in the form of JSON Lines from both sides. The diagnostic executable is also a participant.

```sh
build/install/bin/ii-society-helper --directory "$PWD/build/observe" \
  --application-id com.iisacc.example.one --exit-after-ms 15000
build/install/bin/ii-society-helper --directory "$PWD/build/observe" \
  --application-id com.iisacc.example.two --exit-after-ms 15000
```

The `iisacc.society.helper` Qt logs in the SDK record the start of observation and peer discovery, departure, and errors. UI, you can check whether actual bidirectional observation is enabled in the real app.

<a id="071-기기-내-협업-경계"></a>

## 0.7.1 collaboration boundary within the device

The runtime path of the Helper and DeliveryStore must be on the device's local location. Before starting, inspect all parent paths containing a Society manifest and known network file systems such as SMB / NFS, and do not create observation or message directories within those paths. Since the boundary is rechecked during execution, if the location of an already opened queue becomes a synchronization container, the next operation is rejected. The Helper runtime for an App Group maintains a separate location from the Society data area.

File transfer, change logs, conflict resolution, and receiving from other devices are the responsibility of [the Sync component](docs/Sync/README.md). The Helper's SQLite outbox/inbox/ ACK, running instances, object snapshots, and account references are not part of the network synchronization protocol. Sync now resides in Helper Core and uses ServerHost. Local IPC retains its
separate persistence and network authorization boundaries; package tests reject
imports of the retired SDK targets. The `delivery` check inspects for container boundary violations before starting and during execution, in addition to existing persistence transfer and duplicate prevention.

## License

SPDX-License-Identifier: MIT

The self-written code and documentation of iiSocietyHelper are under the MIT License. The full terms follow [LICENSE](LICENSE). The licenses for external components such as Qt and Apple SDK remain unchanged.

<a id="android-공통-파일-시스템-05"></a>

## Android common file system ( 0.5 )

Android consumer apps call `iiSocietyContainer_configure_android_client(target)` and sign with the same certificate as Society. The internal Society of the ContentProvider app responds to requests even when the app is closed. `fileSystem.open()` selects an existing Society UUID and does not create a separate container. `path()` and `url()` return content URIs from the Android consumer app. Reading and writing are performed via `QFile`, and folders are prepared and enumerated via `ensureDirectory()` and `entries(sectionKey, relativePath)`. Items `entries` are `name`, `path`, `isDirectory`, and `size`. Engines requiring native paths copy the URI to their own cache for use. The absolute path behavior for desktop and Society owned apps is maintained.

The internal provider verifies the signature authority and the signature of the calling UID, and validates the UUID ·area·relative path of all requests. For public Android file apps, only the Files content continues to appear. This file system IPC does not remove the background execution limit for Android app observation. A separate `tests/android/` Qt app checks the creation, reading, writing, enumeration, and rejection of parent paths for 8 areas from the actual app UID via the Helper, and logs the results as `SOCIETY_ANDROID_PEER` JSON in logcat.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.

Account snapshots also include `societyContainerDrive`. Accounts without a drive preserve this field as null and do not omit it, and this is verified in object transfer and restoration tests.

### Fresh generation models (0.7.2)

Native consumers use `helper.fileSystem()->models()` to obtain typed `iiSocietyContainer::StoredModel` snapshots from the selected drive, and check `errorString()` for failed reads. `open()`/`refresh()` revalidates the selection; models are never cached in Helper. The iiSocietyContainer 0.14.1 contract reconciles owner-side imports/deletions while preserving nonresident replica models. This API does not require a running Society window or presence session, and does not enqueue downloads. Android content-provider-only storage currently reports an explicit unsupported native inventory error. The filesystem test covers live additions, Deleted moves and failed selections.

The 0.8.3 package pins iiSharedCanvas 0.31.0 for matching application and storage-owner headers and runtime. Existing detached read/write behavior is covered by the filesystem and installed consumer tests.

The 0.8.3 package aligns by-value canvas storage with iiSharedCanvas 0.31.0.
Native IISC 1.21 mask stacks retain independent operands and appearance when
saved, read or rendered as a preview; application and storage-owner binaries
must be rebuilt together. Existing filesystem and installed-consumer checks
continue to verify native save/reopen and stale-version rejection.

### Container monitor subscription

`FileSystem` automatically subscribes to the Qt-free `iiSocietyHelper::Monitoring` service
for its pinned container. `containerState()` / the QML `containerState` property
contains the daemon identity, container UUID, revision, availability, heartbeat
liveness, completeness, nine section inventories and changed section keys.
`containerStateChanged()` is delivered on the FileSystem QObject's thread.
Opening the same container preserves the subscription; switching containers
clears its state and discards queued events from the previous subscription.

A changed revision refreshes the existing read-only `Container` mirror and
emits `storageChanged()`. Complete inventories of a local authority replace
the mirror's periodic metadata refresh while the daemon is live. Replicas retain
the fallback for preview-only/private download changes; partial inventories also
retain it. Missing/stale/offline daemon state restores the
existing fallback refresh. A daemon heartbeat older than five seconds reports
`daemonRunning=false`; availability and liveness have separate meanings.
The native daemon never copies model weights, reads document payloads or
performs network synchronization. Mobile platforms remain subject to their
existing background execution limits.

```cpp
QObject::connect(helper.fileSystem(), &iiSocietyHelper::FileSystem::containerStateChanged,
                 &app, [&] { const auto state = helper.fileSystem()->containerState(); });
```

Monitoring is built and installed by this SDK; there is no separate monitoring
package to install first. Its native
process and independent-consumer tests cover publication and late subscribers;
`iiSocietyHelper.filesystem` covers two GUI consumers, queued delivery, same-drive
reopening, selection changes and graceful daemon shutdown.

The filesystem suite allows 120 seconds for durable writes on external storage,
including the mask-stack storage/preview regression. This is a test deadline;
production storage transaction and conflict behavior remain unchanged.


### Native image export in 0.10.1

The stateless media-export adapter now reads native IISC without authoring the
source and exports its complete frame-zero document view to PNG. Original raster
and MP4 export still copy their bytes. Native read-only snapshots, source
preservation, alpha/PNG text, atomic output and selected-drive isolation are
covered by source and installed-consumer tests. See [HostDrive](docs/HostDrive.md).
