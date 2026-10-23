# Society synchronization in iiSocietyHelper 0.10.0

Sync is included in the default Helper Core target. Include
`<iiSocietyHelper/Sync/Replica.h>` and use `iiSocietyHelper::Sync`.
Build from the iiSocietyHelper repository root; see [component usage](../Consolidation.md).

[Windows build and execution](WindowsBuildAndRun.md)

The bounded host-drive paging regression establishes a stable fixture with
iiFileProvider's presentation lifecycle barrier before pinning a directory
page. Preview-companion publication legitimately changes native directory
metadata; production requests continue to reject mixed directory snapshots.
Rich information and asynchronous intake remain covered by separate tests.
Scans revalidate the visible child names when only a directory's native metadata
changes during preview publication. Real new/deleted names and changed file
payloads still reject the snapshot. Confined deletion drains that file's preview
job and removes only a verified provider presentation through the pinned parent
descriptor/handle, allowing an emptied directory to be removed without following
links or deleting unrelated companions.
An otherwise empty folder containing only the provider's persistent lease file
is moved under private recovery while its lease is locked exclusively. The
stable lock inode is retained, rather than unlinked beneath waiting writers.

Historical standalone package release: **0.8.4** (2026-10-19). This patch packages the current
source snapshot; build, test and installation evidence is recorded in the SDK
patch-release report. Earlier versioned feature notes remain historical.


Targeted in-process downloads can include `expectedVersion` in local
`Replica::handle` begin/chunk/commit calls. The replica checks the selected
revision under its operation lock before accepting bytes or publishing a file.
Changing metadata or a local edit invalidates the old download; the existing
size/SHA-256 verification and atomic publication remain authoritative.

## File presentation and classification

`iiFileProvider` presentation documents contain local file identities and native
change tokens. Live replica scanning excludes `.iisacc-preview` companions,
provider publication stages, private catalog directories and lock files. Each
verified Society drive regenerates these derived previews after receiving the
original asset. User-edited `.iisacc.json` classification documents continue to
replicate, including keywords, ratings and color labels. Explicit object archives
can still include portable preview companions through iiFileProvider packaging.
The `presentationsAreRegeneratedWhileUserClassificationStillReplicates` replica
regression verifies this distinction.
Direct host `drive.stat` finishes provider enrichment before issuing its native
version, so adding preview attributes cannot invalidate the following chunked
read. This work stays on the existing storage worker; source changes still fail
the confined file version checks.
Confined hashing/chunk reads normalize only the provider's own native attribute
change-time using the already pinned file identity. This performs no additional
path resolution and still detects external edits that restore modification time.
Public host listings/catalogs and direct file reads hide provider control paths;
the lower replication protocol still transports user classification documents.

## Direct container drive protocol

Version 0.8.1 adds `drive.describe/catalog/list/stat/read/write/mkdir` inside the
existing authenticated `society.sync` envelope. Controller retains authorized
peer, scope, container UUID and exact-token checks and performs I/O on its storage
worker. Mirrors cannot act as the primary host. No extra listener is required.

`Controller::openHostAccess()` serves only the direct `drive.*` endpoint, without
replication, filesystem watchers or another background indexer. iiSocietyClient
uses it for apps on the registered host device; the existing host owns indexing
and committed catalog metadata.

Catalog pages pin one namespace revision. Live list/stat/read operations use the
confined native filesystem independently of indexing. Pages contain at most 128
entries and 240 KiB of metadata. Native-version-pinned reads and atomic create-only
writes are limited to 256 KiB per call. Directory creation requires an existing
parent. Section roots, existing targets, private state and redirect boundaries
remain protected. `iiSocietyHelper.Sync.HostDrive` covers these contracts.

Regression fixtures create valid `.societycontainer` directory packages. The
ConfinedFiles public/private recovery test exercises the current directory drive
layout and root replacement; it no longer creates a rejected legacy disk image.

Applications consume the protocol through iiSocietyClient 0.2.0; its
`docs/HostDrive.md` documents public methods, lifecycle and error handling.

<a id="메타데이터-우선과-선택-다운로드"></a>

## Metadata priority and selective download

The host's initial indexing runs in a separate thread and SQLite connection from replication-request handling. After examining directory listings, it finalizes SHA-256 starting with small files and thumbnails, saving in short transactions after at most 64 items or approximately 50ms. The preceding batch is committed before reading files larger than 1MiB. File hashing does not hold the replication-work lock; `changes` responses from `Controller` provide already committed lists. Small-file listings and transfers can proceed before hashing of a new large model completes.

The index is maintained even if the index is cancelled or the next file fails. Upon restart, items with the same file identifier, size, and timestamp are not re-read. The deletion decision is performed after reconfirming the absence of the current path after the entire traversal and file inspection. The file identification status is re-inspected even after the hash to avoid confirming stale hashes from concurrent modifications. `iiSocietyHelper.Sync.Indexing` checks for false deletions caused by large hash metadata responses, early confirmation of small items, reuse after cancellation, and concurrent file regeneration.

Using `Replica::handle(peer, request)` directly maintains the existing synchronous indexing behavior. Consumers with a separate index manager can query only the list confirmed by the third argument `refreshIndex=false`. The operation to open a separate SQLite connection waits up to 2 seconds when competing with short index transactions. Host termination and container replacement cancel and clean up the index thread, while asynchronous app termination does not block the calling thread. The initial SHA-256 calculation itself and the filesystem I/O time for individual large files are still required.

Idle hosts do not re-index the entire tree every second. File monitoring events request an index after the existing 75ms debounce, and manual synchronization also requests it immediately. Monitoring is limited to a maximum of 8,192 files, and in POSIX, to 1/8, iOS of the process file handle soft limit, while in Android, it is additionally limited to a maximum of 128 files. kqueue monitoring ensures that it does not exhaust the handles required by TLS, SQLite, or the original file. Changes outside this limit or missed events are supplemented by a full traversal every 30 seconds. Replication requests and client synchronization timers continue to operate at 1 second intervals. If a new local file is created during an ongoing index, it is checked in the next index cycle, and received pushes use their own verification and revision confirmation path without waiting for index completion.

`SOCIETY_SYNC_TRACE=1` displays only the monitoring count limit applied during initialization in the diagnostic log. The `Indexing` check verifies whether file reading, TCP socket opening, and manual re-indexing can continue after a process that lowered the file handle limit to 256 has listed 300 files. The reproduction and disappearance of real-device kqueue errors due to the difference between the macOS and iOS monitoring backends are verified via separate device logs.

`Controller` first replicates the host's revisions and lists using the default `MetadataFirst` policy. Existing consumers using `Synchronizer` directly retain the default `FullReplica` and select it through `setContentPolicy`. An entry without an original in `StorageMap` is not interpreted as a local-file deletion. When a host change reaches a verified cache, the previous bytes are invalidated. Local changes not yet approved follow the existing conflict-preservation path.

`StorageMap::request(keys)` fixes the selected version. After completion, SHA-256 ·size is verified and atomically published, and host version changes cause the request to fail, requiring reselection. Cancel stops additional downloads after the in-progress chunk bundle and does not publish the unfinished original. Partial files are continued from the next explicit request. Files completed locally are submitted to the host via the same protocol and receive a confirmed revision.

Even when many previous uncommitted changes remain on the client, metadata for remote-only files is applied and published first. After preserving the structural order of directories and deletions, processing proceeds through metadata, host confirmation of held bytes, explicitly selected originals, and the remaining identifying data and conflict reconciliation. A selected original completes its request immediately after its own file validation and atomic commit, without waiting for other files' uploads or conflict reconciliation. Existing local changes continue to use the conflict-preservation path and are neither deleted nor forcibly approved.

New local files not yet in the host journal are submitted after selective download and before adjusting the original of irrelevant conflict files. Maintain the entire submission manifest validation and parent directory order, prioritizing recent local changes among new files. At this time, do not advance the cursor by passing unprocessed items. After upload, re-fetch the host journal and first reflect the confirmed revision of that file. `OnDemand` Regression checks verify whether list posting, selective download completion, new file upload, and host confirmation proceed even when the response of irrelevant conflict files is stopped.

Installs consumers' `installed_on_demand` same priority·cancel·select source checks are executed against the deployed SDK.

In asynchronous index recursion, the first namespace change signal is not considered as the completion of indexing for all files. Offline file publishing and slow provider concurrent local commits verify that the target path appears in the actual repository map.

Photo aliases and previews, each no larger than 512 KiB, and the model's `model_index.json`, no larger than 1 MiB, are received first as identifying data. Ordinary image previews are generated on the host as 256 px JPEGs. Decoding input is limited to 32 MiB and 64 MP, and responses to 512 KiB. An unsupported preview does not hide the original list. Originals are transferred only on selected requests. After provider I/O, journal re-entry waits for the lock for up to 2 seconds, so temporary contention with list publication does not report an already successful save as a failure.

At most 16 previews are processed per round. The next round handles new model requests and generated-result uploads before all previews for a large gallery are received. `iiSocietyHelper.Sync.OnDemand` checks this processing order, initial lists and previews without original transfers, downloading only selected models, generated-result uploads, preventing false deletions after restart, rejecting changed versions, and cancellation and resumption during transfer.

`BleDiscovery`/`NearbyBootstrap` exchange small connection information with nearby devices. Authenticated Wi-Fi/LAN file transfers process at most 4 chunks concurrently and use binary-body negotiation from iiWebServer 0.6. Refer to [connection flow, API, platform support, and verification scope](NearbySync.md). Host permissions, committed revisions, and provider separation follow the [Namespace contract](Namespace.md). Resumption and SHA-256 verification are retained.

It uses `iiSocietyContainer` 0.14.0 and above. Files have no default directory. Documents, Audios, 3D objects are also regular user items and are treated identically for remote deletion, file replacement, and retransmission. Unsynchronized content in non-empty local directories follows existing conflict preservation rules. Initial host adoption preserves the entire existing user folder as a recovery area and leaves no empty default folder in Files. `Replica` and `Controller` tests validate this contract. The top-level `Photos/` is an independent `photos` section.

C++23/Qt SDK synchronizes Society container data running on different devices. It accepts authenticated Society connections from the same account to perform change detection, bidirectional transfer, resume recovery, and conflict preservation. `helloWorld()` is maintained for existing consumer compatibility.

<a id="책임과-의존-방향"></a>

## Responsibilities and dependency direction

|Components|Tasks performed|Dependency boundaries|
| --- | --- | --- |
| Helper Core | Same-device IPC, account/client access, storage and synchronization | Uses Container, Account, ServerHost and native Monitoring. |
| `iiSocietyHelper::Sync` | Container replication and authenticated host file access | Implementation within Helper Core; no separate SDK package. |
| iiSocietyContainer |Container UUID, 9 areas, and local shared OS provider|Does not know the synchronization protocol.|
| iiWebServer |Authenticated general request/response, LAN TLS legacy relay, limited file service|Society  does not know change history and conflict policy.|
| `iiSocietyHelper::Client` | Account discovery, pairing and host access | Uses Core Sync and Account; never references Product sources. |
|Society  app|First login/host selection, app lifecycle/screen|Connects Client's shared authentication configuration and Sync and manages host roles.|

Reuses the existing Qt 6.8.3 Core/Gui/Network/Sql, SQLite, and TLS from iiWebServer 0.6.0. No new external library, server, or paid service is added. Hashing uses Qt's [QCryptographicHash](https://doc.qt.io/qt-6.8/qcryptographichash.html), inter-process work exclusion uses [QLockFile](https://doc.qt.io/qt-6.8/qlockfile.html), and the persistent journal uses [SQLite WAL](https://sqlite.org/wal.html). Maintenance and licensing follow the terms of the existing Qt/SQLite/SDK configuration. Only the version and conflict policies specific to Society are implemented in this SDK.

<a id="공개-api"></a>

## Public API

- `Controller` : dedicated work thread, container open/close, authenticated device set, sequential synchronization, remote request handling.
- `Replica` : Namespace permissions/revision graph of a single thread, SQLite  change journal and local projection. `scan`, `changes`, `record`, `handle`, per-device cursor persistence.
- `ObjectProvider` : host's content hash-based byte placement/recovery. Executes `DirectoryObjectProvider` and `RemoteObjectProvider` to `Controller::placeObject` / `restoreObject`.
- `Synchronizer` : bidirectional transfer of one remote Society and one session. Separates transfer implementation with `requestReady` and `receive`.
- `RemoteFiles`, `filesHandler`: Remote Files browsing, atomic download, and Files-only services. Full container replication is a separate authenticated `society.sync` protocol.

Windows `filesHandler` provides Files listing, metadata, read, and new file/directory creation using Sync's native file access. Uploads publish as public hard links after completing private temporary files without overwriting. If the same name already exists, it preserves the existing file and rejects. Universal iiWebServer POSIX FileShare is called from Windows or ServerHost does not back-reference Sync. This Files projection does not open other areas such as Models, and full container changes are handled via the Replica protocol.

```cpp
#include <iiSocietyHelper/Sync/Sync.h>

// transport is a Society transport object that has already verified account credentials and TLS.
iiSocietyHelper::Sync::Controller sync([&](const QString &peer, const QJsonObject &request) {
    return transport.request(peer, request);
});
sync.open(containerRoot, verifiedAccountScope); // 64-character lowercase SHA-256 scope
sync.setPeers(authorizedPeerIds, remoteHostIds);
// transport request handler: sync.handle(authenticatedPeerId, envelope)
// transport completion callback: sync.receive(transportRequestId, response)
// Logout, container change, or OS background execution time expiry: sync.close()
```

`Controller` and its transfer callback are used in the app thread. `open()` is asynchronous and checks the result via `available()` after `changed()`. File hash, SQL, and actual application are performed in a dedicated thread. `synchronized(peer)` means reapplying the host commit result of the submitted change in that session and that the cursor was saved, but it does not mean the entire initial indexing still in progress on the host is finished. It records local changes even if there is no connected peer. The remote host list is deduplicated and sorted for sequential processing, local file changes are detected via QFileSystemWatcher, merged during 75ms, and synchronization is immediately scheduled. Changes that arrive while in progress are reprocessed immediately upon completion. Replication and client rechecks occur every 1 seconds, and missing events and monitoring limit exceeded paths for idle hosts are supplemented by full indexing every 30 seconds. OS monitoring paths are up to 8,192 in number. Mobile Society performs both download and upload via client request without creating a host listener.

`Replica::handle()` is an internal work API that has completed authentication. It must not be exposed directly to the network. `Controller::handle()` checks the currently authenticated peer set, scope, protocol, and request size. Merely knowing the public scope does not grant access rights. The SDK receives a password, cookie, or daily seed, or does not send a iisacc HTTP request. Account switching and logout in the app cancel previous responses and in-progress file operations by changing the work generation number.

<a id="호스트의-동일-드라이브를-미러링하는-절차"></a>

## Procedure for mirroring the host's same drive

Authentication verifies the logical UUID and device replica UUID of the authenticated host via protocol 2 + `namespaceVersion: 1`'s `describe` response. The previous independent drive merge protocol 1 is rejected. The first connection pins the host to `bindHost` and preserves the contents of the existing 9 client areas to `.society-sync/detached/<old-id>-<migration-id>/`. Since the move plan is committed to SQLite first and the area root is maintained, recovery occurs at `open()` even if UUID changes or some moves are interrupted immediately after.

`SocietyDrive::adoptReplicaIdentity` adopts the host UUID and transitions the native public status to `replicaReady: false`. After receiving and catching up with host metadata and additional changes during transfer, it publishes to `completeReplica` and starts bidirectional synchronization. Files exclusive to the previous client are not uploaded. To add existing files to the new drive, they are explicitly retrieved from the preserved recovery area. Offline editing, creation, and deletion after initial mirror completion are propagated to another client via the host on the next connection.

`binding(path)` reads the primary, logical UUID, completion status, and recovery path of the local mirror. `mirrorChanged` is the event when the product reopens the drive object and screen. The device local ownership record of `claimPrimaryHost` / `primaryHost` prevents a new desktop with a smaller ID from replacing the existing host. The mirror does not automatically become the host.

<a id="저장과-전송"></a>

## Save and Transfer

Synchronizes the relative paths of 9 areas. Ordinary files, directories, and deletion records are the targets. The root's `.society-drive.json`, `.society-sync/`, login and pairing group status, and Helper messages are not transferred. The logical container UUIDs of the host and all clients are identical. The replica UUID and OS provider's internal registration ID are maintained per device. Finder, Files, and public `filesHandler` continue to expose only `Files/`.

iiSocietyContainer also synchronizes empty folders by type created in `Models/` of the new container as ordinary directory items. Therefore, the number of file changes and page boundary tests compare after the cursor of the container's initial scan. The manifest format of the initial folder is also checked, and the first mirror recovery test jointly verifies the private preservation of previous files and the restoration of the host's empty model folder structure.

`.society-sync/journal.sqlite` is device-local metadata and is bound to the account scope and container UUID. It stores per-file SHA-256, vector clock, version, increasing cursor, deletion tombstone, and the relative device's replica/container ID and bidirectional cursor. If the device and file ID, size, and modification/change time of the actual file remain unchanged, the hash is not recalculated. The change list reads up to a fixed upper-limit cursor of 128 items, approximately 256 KiB at a time. Changes after that are sent in the next session. Sessions with no changes do not send file bytes.

`describe.manifestVersion = 1` relative peers complete bidirectional identifier exchange before file bytes. The host collects all `changes` pages, and the client also sends the relative path, type, size, SHA-256, vector clock, version, and sequence number first to `manifest` pages. Each manifest has a maximum of 250,000 items and is sent in units of 128 items, 256 KiB. The full identifier is the value where the compact JSON of each item and the newline are placed in order at SHA-256. The receiver validates the page order, cursor, replica ID, and full hash, then ACKs the last page. If the ACK is out of sync, file bytes are not transmitted. Manifest reception does not modify files or temporary chunks, and the client validates and stores the host revision in a separate journal page before applying the file. Independent files of the first mirror are excluded with an empty manifest, and newly created changes or conflict copies during download are all re-announced before upload. The transmission request is bound to the confirmed manifest ID. Older hosts that do not advertise Namespace support reject the connection and return an update error.

Large files re-check the manifest at chunk boundaries after processing approximately 1 seconds of payload. Since processing occurs in directory preparation, deletion from deep paths, and small files, new small changes do not wait for the entire large file to complete. Partial files and incomplete cursors are held and resume from `begin.offset` without resending the completion chunk. Manifest ACKs are re-verified each time they are updated. This time is a reservation baseline and is not the completion upper bound including request delay, hashing, and file application time. Download and upload new modifications/deletions with priority and verify chunk offset continuity via delayed response regression.

The file is written to 256 KiB chunks in `.society-sync/transfers/`. Retransmission of the same chunk is not applied redundantly. After verifying the completed size and SHA-256, it is installed via atomic relative rename within the directory. Damaged completed temporary files are removed to receive again in the next attempt while keeping the existing destination. A retained partial file resumes from already verified bytes; after a replica closes, housekeeping may remove abandoned bytes immediately and the next attempt restarts from offset zero. If the in-progress source changes, the corresponding transfer is failed and the new version is processed in the next round.

Managed temporary writers use the Qt-free `iiFileProvider::TemporaryLease` primitive. A receiver retains a shared lease on `.society-sync/transfers/` from an accepted file `begin` across separate `chunk` and `commit` requests until completion or `Replica::close()`. An idle replica holds no transfer lease. Provider restoration holds a shared lease for the entire `provider-<UUID>.part` lifetime, including remote I/O while `operation.lock` is released. Windows Files uploads retain an uploads-directory lease through publication and temporary removal, and Windows filesystem capability probes retain a `.society-sync/` lease until both `capability-<UUID>` and its `.link` hard link are removed. macOS cross-volume installation retains a destination-parent lease around `.society-install-<UUID>` staging. Remote provider object, verification, and download staging also holds its private runtime-directory lease. Failure to acquire a safe lease returns the existing operation error without creating temporary bytes.

`StorageMaintenance::runOnce()` can remove recognized abandoned parts and staging files only after an exclusive parent-directory lease succeeds. The shared lease protects all active writes in that parent directory; it does not turn a partial transfer into durable user data. The durable `.iisacc-temporary.lock` sentinel is excluded from replicated records and Windows Files requests. Private recovery files, journal databases and WAL/SHM files remain outside this temporary-cleanup policy. The `iiSocietyHelper.Sync.Replica` regression runs housekeeping between transfer requests, after completion while the replica remains open, after closing an incomplete transfer, and during provider I/O with the journal lock released; it also rejects sentinel replication.

The vector clock is used for tracking causal relationships of submissions, and the confirmed order is determined by the host's revision journal. The client submits the base revision and receives the host's decision. Editing files of an old base preserves the host state and keeps other content `.sync-conflict-<version>`. In structural conflicts, the directory is maintained to avoid losing child files, and files are left as conflict copies. Filename length and existing conflict names are checked, and case-insensitive Unicode normalization conflicts are returned as errors. It follows detailed decision rules [](Namespace.md).

Before replacing or deleting a regular file, the existing file's hard link and path/visual JSON are saved to `.society-sync/recovery/<UUID>`. Recovery data is not transferred to other devices and is not automatically deleted. Since it is not a separate snapshot/backup product, if another process continues to modify existing open inodes, the recovery hard link may also be reflected.

<a id="경계와-운용-제약"></a>

## Boundary and operational constraints

- macOS / iOS / Android /Linux uses POSIX file access, while Windows uses native NT file handles. Windows local volumes must support file IDs and hard links. Since opening checks for actual recovery link creation, reading, and cleanup in the private area, it is not judged solely by support flags. Volumes that do not support recovery hard links, such as UNC /network drives and FAT, are rejected. Windows cross-build and actual Windows /Wine execution results are recorded separately.
- The journal and file projections for consumer apps reside on device-local disks. Object storage on NAS/S3 uses ObjectProvider. Known network file systems such as SMB/NFS, symbolic links and special files, container replacement, and journal-bypass paths are rejected. POSIX restricts paths through `openat`/`O_NOFOLLOW` and root-inode checks. Windows uses `RootDirectory`-relative opens through [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile), rejects reparse points, and verifies the volume and 128-bit file ID. All parent-directory handles are retained throughout the operation to prevent path replacement. Concurrent writes to files being read or hashed are rejected; sharing violations are retried during the next synchronization.
- Reserved device names in Windows, ADS separators, trailing dots and spaces, and names that cannot be represented in Win32 are rejected without modification. Names are not automatically normalized or changed to other names.
- Component names are UTF-8 220 bytes or less, and paths are 3,500 UTF-16 code units or less. The OS's shorter path limit also applies. A single container check allows a maximum of 250,000 items, and a single vector clock allows a maximum of 64 replicas.
- The initially authenticated primary host ID is fixed. If the same host selects a different drive, existing mirrors and untransmitted changes are preserved in the recovery area, and a new initial mirror is started. Arbitrary automatic switchover to another primary is rejected. Journal recovery for the same logical host rechecks by resetting the cursor.
- Deletion records and recovery data are not automatically garbage collected. The possibility of returning permanently offline devices is maintained, and disk usage is cumulative. Transmission stops when storage space is insufficient.
- Windows chunk data is recorded with write-through and `FlushFileBuffers`, and replacement, hard link, and recovery moves are performed as open directory relative NT operations. Process interruption recovery and power-off durability are distinguished, and Windows power-off testing is separate.
- Atomicity is at the file level. It does not snapshot multiple files or the database in use as a single transaction. The consuming app must store a synchronizable completion state via database backup/close and consistent file export.
- Requests to `Controller` wait up to 120 seconds and recheck pending responses at 1 ~ 100ms intervals. The hash does not block UI threads and checks cancellation between each block. After termination, callbacks on the send object must also be released.

<a id="빌드설치검증"></a>

## Build, install, and verify

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos;$HOME/.local/SDK"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_PREFIX_PATH="/Volumes/Storage/Qt/6.8.3/macos;$HOME/.local/SDK" \
  -DiiSocietyHelper_DIR="$HOME/.local/SDK/iiSocietyHelper/lib/cmake/iiSocietyHelper"
cmake --build build/consumer/build --parallel 4
ctest --test-dir build/consumer/build --output-on-failure
```

Public headers are installed in `include/`; source headers reside beside their implementations. iOS uses a static library, while desktop and Android use shared libraries. Install `iiFileProviderTemporary` (target `iiFileProvider::Temporary`), iiSocietyContainer 0.15.0 or later with its `Maintenance` target, and iiWebServer 0.6.0 for each ABI first. The standalone temporary package has no Qt dependency and does not require the higher-level iiFileProvider GUI or provider targets.

File access contract checks verify chunk retransmission, hash and partial read, original preservation, actual symbolic link bypass rejection, recovery move retry, and root replacement. Windows generates a reparse fixture with `CreateSymbolicLinkW`, so Developer Mode or symbolic link creation permission is required. The `QFile::link` Windows shortcut is not used for fallback verification. For cross-execution, specify `IISOCIETYHELPER_SYNC_TEST_DIRECTORY` as the path visible to the target runtime `build/`.

The checks cover 9 areas excluding private roots, page cursors, actual bidirectional files/directories/deletions, receiving past deletion history on new devices, concurrent modify/delete/type conflicts, abort/lost ACK/retry, hash errors/path bypass/filename conflicts, authentication revocation/token reuse rejection, and handling the mobile client role of actual loopback TLS. Installing consumers run the same feature checks without referencing headers/libraries from the source tree and also verify that retired package targets are not imported. Wi-Fi and OS background verification between physical devices are distinguished from these checks.

The real loopback TLS test allows up to five seconds for the initial client upload and a host filesystem edit to reach their destinations, retaining bounded convergence checks under concurrent disk and indexing load; this test budget is not a product latency guarantee.

`./install.sh` performs the same configuration/build/check/install and the installation consumer checks of `build/consumer/build/`. Paths can be changed to `QT_PREFIX_PATH`, `INSTALL_PREFIX`, `CMAKE_PREFIX_PATH`, and the default SDK search path includes `$HOME/.local/SDK`.

## License

SPDX-License-Identifier: MIT

The self-written code and documentation of iiSocietyHelper::Sync are under the MIT License. The full terms follow [preserved Sync license](../../licenses/Sync.LICENSE). Licenses for Qt and other external components are maintained separately.

Regression checks verify if 0.4.0 multi-manifest ACKs precede the first byte, if byte transmission from incorrect ACKs occurs 140 times, if actual TLS propagation of creation/modification/host changes without manual calls happens 0 times, and if it ends within the 1.8 second inspection limit. Period and inspection limits are not completion times guaranteed by the network or OS.

Apple platform large files SHA-256 use OS default CommonCrypto. Deployment Qt 6.8.3 software SHA-256 bottlenecks are reduced while maintaining hash format, file handle/modification checks, and per-block cancellation contracts. System libraries from macOS / iOS SDKs are used without additional packages or server dependencies, and other platforms maintain existing Qt paths. Empty files, SHA padding boundaries, 1MiB read boundaries, and binary inputs are compared against Qt's independent results. API rationale is [Apple CommonCrypto   CommonDigest](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/CC_SHA512_Update.3cc.html) documentation.

When returning desktop process ownership, complete the cancellation of the previous task and the termination of file/DB handles before passing the lock to Controller::closeAndWait(). A regular close() maintains the existing asynchronous cancellation. The controller check verifies whether the next Replica can acquire the lock immediately upon return during an actual large-scale file scan.


<a id="모바일-비동기-실행-050"></a>

## Mobile asynchronous execution ( 0.5.0 )

`Controller::inspectContainer(path, scope)` reads from existing replica work thread in container UUID ·mirror binding·primary host and passes to `containerInspected` . No need to call `Replica::binding` / `primaryHost` directly from UI. Query does not create database or replica directory and discards old results when new query arrives. Signals and `RequestSender` are processed in Controller-owned thread and file system work is executed serially in work thread.

`open` 's initial preparation, SQLite , monitoring registration, hash, manifest, and chunk processing are asynchronous. After initial open failure, it can retry by `open` with same path and account. In-progress or prepared same container is not re-initialized redundantly. `close` requests cancellation and returns immediately. When discarding mobile object, calling `shutdownAsync()` causes work thread to clean up resources and terminate itself. Subsequently, that Controller cannot be opened again. Desktop execution right handover maintains `closeAndWait()` . Default destructor waits for work cleanup for desktop compatibility.

`RemoteFiles` 's destination open, Base64 chunk check, write, and final `QSaveFile::commit()` are also performed in separate work thread. While saving, it maintains `busy()` and passes `downloadFinished` only after commit completion. Cancellation and object destruction do not wait for work and invalidate late callbacks. Failed transfer preserves existing destination file. Already running OS file operation checks cancellation upon return.

0.5.0 changes the ownership implementation and destruction boundary of RemoteFiles, so consumers must rebuild. The dynamic library's ABI name is separated as `0.5`; the existing `0` library is not replaced with the new implementation. Synchronization protocol 2 and the disk format are retained. It reuses [worker objects and queued connections](https://doc.qt.io/qt-6/qthread.html)from the existing Qt Core, with no new external dependencies.

`iiSocietyHelper.Sync.Controller` performs asynchronous lookup, discards stale results, retries after initial failure, checks 1 GiB during asynchronous termination, hands over desktop execution rights, preserves successful downloads and incomplete files, and checks actual local TLS bidirectional synchronization.

Photos is the top-level section of Container 0.13.0. It is automatically enumerated from `allStoreSections()` to apply normal section transfer and host adoption rules. Existing path migration is performed by the Container, and all synchronized participating devices must use the new layout. Society. Photos' TLS integration test checks `Photos/`'s alias, preview, and original channel together.

<a id="무결성-검사-진행-보고"></a>

## Integrity check progress report

`Replica::setVerificationProgress` and `Controller::verificationProgress` deliver the actual bytes of SHA-256 processing. It starts from per-file 0, and progress events mean processed bytes. It does not stand in for hash success or transfer completion. The controller delivers up to approximately 10 Hz from its own thread, along with start and end chunks, and discards events from the previous container. Separated from the transfer progress signal, this allows observation of progress even while the mobile OS's persistent execution task is performing large model checks. OS execution permission is the responsibility of the consumer app. ConfinedFiles test checks accurate hash, per-chunk progress, and intermediate cancellation together.

## Provider file information on the host drive

`drive.describe` advertises `inspect` and `fileInformationSchema: 1`. File rows
from `drive.list`, `drive.catalog` and `drive.stat` carry an `information`
descriptor (`provider: iiFileProvider`, `action: drive.inspect`, `schema: 1`).
Listing remains metadata-only; neither indexing nor downloading a local replica
is required for direct host inspection.

Send `drive.inspect` with the pinned container identity, logical `path` such as
`files/brief.txt`, and optional decimal-string `offset` (default `0`). The host
uses `SharedStorage::fileInformation` and returns the provider's actual details,
preview and published classification as one JSON document split into at most
256-KiB decoded chunks. Each response includes `data` (base64), `offset`, `size`,
`eof`, the native source `version`, and an `informationVersion` SHA-256 of the
whole serialized document. The document is limited to 3 MiB.

Every continuation must carry both returned versions. `drive_file_changed` rejects
a changed original; `drive_information_changed` rejects an updated preview or
classification even when the original is unchanged. Offset, document bounds,
control names, container identity and ordinary confined-path checks apply before
data is returned. Private companions are never published as user files, and no
host-native address is added to the projection. The operation shares the existing
authenticated account/peer channel and permissions of direct file reads.

`iiSocietyHelper::Client::inspectHostFile` assembles these chunks asynchronously,
validates their sequence, bounds and document checksum, and emits one
`hostDriveResponse` containing `information`. It does not read/materialize the
original. Unknown formats retain a truthful information card. Missing or invalid
classification is explicit, independent of content-preview availability.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
# Native Files volume

On macOS, Society's public APFS volume contains only the Files namespace. `ConfinedFiles` resolves
the Files section through the container SDK's verified volume identity; other sections and recovery
metadata remain on the private data volume. Public-volume removal invalidates the open filesystem.
Transfers crossing these two volumes copy to a temporary destination before publishing the file, and
recovery copies preserve the previous bytes. Arbitrary symlinks remain rejected. Native integration
coverage lives in `tests/sync/confined_files.cpp`; all fixtures are created below `build/`.

`Controller::setPeers` accepts an authenticated account host and container expectation. A matching authority may replace the old mirror host; existing data and pending edits move to `.society-sync/detached/` before adoption. A mismatched host/container fails before any adoption or upload. Without an explicit expectation, the existing primary-host restriction remains.

### Local failure diagnostics

Set `SOCIETY_SYNC_TRACE=1` when launching a development build to log changed synchronization error messages. It is disabled by default and does not print account credentials, proof keys or protocol payloads. The UI retry state alone does not establish a successful host connection.

Initial `MetadataFirst` connection does not wait for the full download of photo identification files and previews. It first prepares the host's verified list and model index, and recovers up to 16 photo identification files in subsequent synchronizations. The missing list is retained on disk and continues after restart, and the original model is downloaded only if explicitly requested.

`hostValidated` is the point at which host/container match and prepared mirror are confirmed in the currently authenticated transfer. It occurs only after bootstrap completion in the first connection. The meaning of `synchronized`'s full round-trip completion is retained, and consumers can separate the photo upload queue and onboarding readiness status.
