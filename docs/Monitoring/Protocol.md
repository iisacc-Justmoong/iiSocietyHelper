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

By default the producer indexes all entries across the nine areas, up to 256 directory levels.
The root of each scanned area consumes one entry. Missing/unreadable areas or
exhausted limits report `complete=false`; counts represent observed entries,
not proof of the entire disk. `available` describes a valid container, while
`daemonRunning` describes the producer's heartbeat. They are separate fields.
Scans run serially. By default, the daemon refreshes its heartbeat every second
and waits at least five seconds after a full scan before scanning again. A
heartbeat republishes the last scanned state without traversing the storage.
`--scan-interval-ms` changes the scan interval. For compatibility, specifying
only `--interval-ms` applies that value to both scans and heartbeats. A scan
still runs on the polling thread, so a slow filesystem can delay its heartbeat
and change detection; scans do not overlap. After a scan the heartbeat records
its completion time, so a slow scan does not publish an already expired value.

## 파일별 인덱스와 빠른 목록

모니터는 스캔한 아홉 영역의 파일과 디렉터리를 컨테이너 내부
`.society-runtime/monitor/v1/inventory.sqlite`에 기록한다. 상태 하트비트와
데이터베이스 저장은 별도 작업자에서 진행하며, 저장 중 새 스캔이 도착하면
가장 최근 스냅샷만 기다린다. 영역 지문이 같으면 해당 영역은 다시 쓰지
않는다. 지문에는 경로와 메타데이터 외에도 파일·디렉터리 종류가 포함된다.
명시적인 항목·깊이 제한, 읽기 실패 또는 중단으로 영역이 불완전하면
그 영역을 완전한 인덱스로 제공하지 않는다. 기존 항목을 삭제 처리하지 않는다.
파일 제공자의 경로 계약에서 지원하지 않는 `:` 또는 역슬래시가 항목에 있으면
해당 영역을 불완전하게 표시하고 앱은 실시간 목록으로 돌아간다.

항목에는 컨테이너 ID와 상대 경로로 만든 SHA-256 조회 키, 크기·수정 시각·권한과
이미지 변경 토큰의 SHA-256 메타데이터 리비전, 디렉터리 여부와 표시용 시각이
들어간다. 조회 키는 경로 변경 시 바뀐다. 이 값은 **파일 내용의 SHA-256이 아니며**
동일 크기와 수정 시각을 보존한 변경의 무결성 검증 수단이 아니다. 대형 모델을
읽거나 원본을 복제하지 않는다. 일반 숨김 파일도 인덱스에 포함하며 제공자 제어
파일과 리디렉션은 제외한다. `.iiproject`는 내부 항목을 펼치지 않는다.

캐시는 컨테이너 ID에 바인딩되고 SQLite 트랜잭션으로 갱신된다. 읽는 앱은 캐시를
최초 표시용 잠정 정보로 사용한 후 실제 파일시스템을 확인한다. 캐시가 없거나
손상되었거나 ID가 다르면 기존 조회 경로로 돌아간다. 인덱스만으로 파일 열기
권한을 부여하지 않는다. 미러의 비거주 원격 항목도 기존 동기화 카탈로그로 다룬다.
선택된 컨테이너가 바뀌거나 종료되면 작업자는 게시 전에 취소 여부와 컨테이너
식별자를 다시 확인한다. 지연되는 파일시스템 호출은 별도 작업자로 남을 수 있다.

`SocietyContainerDaemon --index-list Files`는 캐시된 직속 항목을 JSON으로
출력하고 `--index-get KEY`는 해당 키를 조회한다. 두 명령은 선택된 컨테이너와
상태의 식별자를 검사한다. `--once`는 인덱스 게시를 기다리며 실패하거나 제한
시간을 넘기면 실패로 종료한다. `--exit-after-ms`를 함께 지정하면 해당 기한을
인덱스 대기에도 적용한다. 일반 하트비트와 GUI는 이 완료를 기다리지 않는다.

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
