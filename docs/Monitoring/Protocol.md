# Container monitor protocol v1

`iiSocietyHelper::Monitoring` is a C++23 native SDK. `SocietyContainerDaemon` is its plain
executable producer. Neither links Qt. It reuses `iiSocietyContainer::FileTree`
for bounded metadata traversal and `iiSDKCore` for CBOR and iiFileProvider CoreIO for atomic replacement.

## Scope and identity

The producer follows the existing Society selection document. The default is
`~/Library/Preferences/iisacc/Society/storage.json` on macOS,
`$XDG_CONFIG_HOME/iisacc/Society/storage.json` (or `~/.config/...`) on Linux,
and `%LOCALAPPDATA%/iisacc/Society/storage.json` on Windows.
`SOCIETY_STORAGE_SETTINGS_PATH` or `--selection-file` overrides it. The pointer
must contain numeric `schemaVersion: 1`, an absolute direct `path`, and a
`containerId` UUID matching `.society-drive.json` (`type: SocietyDrive`, numeric
`schemaVersion: 1`, `identifier`). Manifest and selection reads are limited to
16 KiB and validate complete UTF-8 JSON, including escaped Unicode paths.

The producer pins the container's native filesystem identity and UUID. Redirected
ancestors, runtime/section symlinks and Windows reparse points are rejected.
Removing, redirecting or replacing the root stops publication there. A selected
container switch publishes offline state to the previous valid container and
starts a new publisher at the new valid root. No drive is created implicitly.

## Inventory and delivery

Public areas, in wire order, are `files`, `photos`, `asset-library`,
`generation-history`, `models`, `thinking-space`, `forked`, `published`, `deleted`.
Each contains availability, completeness, file/directory counts, logical file
bytes, newest modification time (Unix milliseconds) and a metadata fingerprint.
The fingerprint covers relative paths, byte lengths, modification nanoseconds
and permissions. It detects additions, removals, renames and metadata changes;
it is not a content digest. Same-size edits with preserved modification times
are outside this contract. Payload contents, model weights and credentials are
never read. Symlinks are excluded by FileTree. `.society-runtime` is outside all
public areas and cannot trigger its own inventory revisions.
The metadata stamp of `.society-sync/catalog.json` also invalidates all areas,
so replica namespace changes are delivered before payloads are downloaded. Its
contents are not read and its bytes do not contribute to public-area counts.

Default bounds are 100000 entries across all nine areas and 64 directory levels.
The root of each scanned area consumes one entry. Missing/unreadable areas or
exhausted limits report `complete=false`; counts represent observed entries,
not proof of the entire disk. `available` describes a valid container, while
`daemonRunning` describes the producer's heartbeat. They are separate fields.
Scans run serially, with a one-second wait after each scan by default. Slow
filesystems therefore increase detection latency. The interval and limits are
configurable; the daemon does not overlap scans.

The per-container owner lock is `.society-runtime/monitor/v1/owner.lock` (native
OS file lock). A crashed process releases ownership without deleting a lock file.
The shared state is `.society-runtime/monitor/v1/state.cbor`, committed using
`iiFileProvider::core::AtomicFile`. New runtime directories have owner-only permissions. This
is same-user, local filesystem IPC. It is not a network transport or a substitute
for Society's authenticated synchronization service. Existing private-runtime
sync exclusion continues to apply.

Every successful scan atomically refreshes the heartbeat. `revision` changes
only on logical inventory/availability/error changes. `changedSections` records
keys changed in that revision. Each producer has a new `instanceId`; restart
continues a readable previous revision and publishes a fresh revision. The
snapshot is a latest-state channel, not an event log: intermediate revisions may
be coalesced, and apps must reconcile from the full current state. A late
subscriber immediately receives the current state. GUI consumers refresh their
existing read-only mirror when a revision changes.

`read(root, expectedId)` rejects invalid identity, schema, redirected files and
oversized/malformed CBOR (1 MiB maximum). It reports `daemonRunning=false` after
five seconds without a heartbeat, or for an implausibly future heartbeat.
Callers using scan intervals over five seconds must choose a matching
`staleAfter` when reading. Graceful shutdown publishes `daemonRunning=false`;
forced termination becomes stale. The native Subscription uses the default
five-second liveness timeout and polls snapshots every 250 ms. It suppresses
heartbeat-only callbacks, delivers status/error transitions, and catches handler
exceptions. Callbacks execute on its worker; GUI clients must marshal them to
their UI thread. Partial inventories retain the GUI adapter's fallback refresh.
`stop()` joins the worker; when invoked from its own callback it
requests shutdown and detaches that finishing worker. Publisher calls and
Subscription lifecycle calls must each be serialized by their owner.

## CBOR and JSON

The CBOR document is a fixed-order tuple:

```
(schemaVersion, instanceId, containerId, rootPath, revision, observedAtMs,
 available, complete, daemonRunning, error, sections, changedSections)
```

Each section is a tuple:

```
(key, files, directories, bytes, fingerprint, lastModified, available, complete)
```

`toJson` exposes named fields with `rootPath`. Revision and fingerprint are decimal
strings to preserve 64-bit identity in QML/JavaScript. Other counts are integers.
No QObject, pointer, account token or app-owned object crosses the process
boundary. The binary schema must change before changing tuple order or types.

## Native consumers

```cpp
#include <iiSocietyHelper/Monitoring/Monitor.h>
iiSocietyHelper::Monitoring::Subscription subscription;
auto started = subscription.start(root, containerId, [](const auto& state) {
    // Reconcile from state.sections; forward to the GUI thread if necessary.
});
// subscription.stop() or destruction joins its worker.
```

Link the exported CMake target `iiSocietyHelper::Monitoring`. For GUI apps,
use `iiSocietyHelper::FileSystem::containerState` and `containerStateChanged`;
that adapter handles UI-thread delivery, obsolete events after selection changes
and mirror refresh/fallback. On mobile, OS background limits still apply; this
SDK does not install a persistent iOS/Android daemon.

## Validation

`iiSocietyHelper.Monitoring.monitor` checks revisions, counts, single ownership, identity
and graceful shutdown. `iiSocietyHelper.Monitoring.process` launches an independent daemon,
two native consumers, late selection and selection switching, validates shared
revision delivery and preserves payload bytes. The process suite also force-kills
a producer and verifies subscription liveness expiry without another publisher
writing an offline snapshot. Helper's filesystem suite covers GUI-thread
delivery, two application consumers and subscription replacement.

Pure heartbeats intentionally do not emit subscription/GUI callbacks. The
`observedAtMs` in a delivered callback describes that snapshot's observation;
apps should use `daemonRunning` for liveness instead of recomputing age from a
previously delivered revision. Direct `read()` returns the current heartbeat.
