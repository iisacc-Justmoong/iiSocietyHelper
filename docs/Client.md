# Account and host sessions

## Asynchronous Helper lifecycle

`Helper::startAsync()` accepts a start request on the owning event-loop thread;
`startFinished(bool)` reports whether observation was established. Presence and
delivery storage setup run off that thread. `stopAsync()` invalidates pending
work and returns before storage cleanup; `stopFinished()` is the completion
barrier before reusing the Helper or handing a dependent lease to another
owner. A pending start can be cancelled by stopping. The synchronous `start()`
and `stop()` APIs remain available for callers requiring immediate completion.
Applications using asynchronous final shutdown must disconnect the Helper's
default synchronous `aboutToQuit` handler and arrange their own stop ordering.

기기간 동기화 상태는 `Client::synchronizationStatus()`의 독립 스냅샷과 수신
실행기 구독으로 조회한다. 누락 복구·수명 및 기존 API의 이전 방법은
[스냅샷과 상태 계약](Sync/Snapshots.md)을 따른다.

## Container objects and direct host access

The consolidated 0.9.0 SDK preserves `Client::connectHost()` for authenticated access without a local
replica. `container()` exposes `iiSocietyContainer::Container` and `hostSections()`
exposes all nine sections. Asynchronous list/stat/read/write/mkdir operations use
the live host drive and existing account-scoped LAN TLS or relay session.
Reads are version-pinned and chunked; atomic creation preserves existing files.
Apps on the registered host use the selected, identity-checked native drive
without a self peer or another indexer. Other devices retain pinned LAN TLS or
account relay access.
See [HostDrive.md](HostDrive.md) for API, lifecycle, paging and size limits.

`tests/consumer` verifies the installed public API and repeats native LAN TLS and
account-relay access against installed packages, independently of source targets.

## Saved host reconnection

`FileDownload`는 `FileHost` 전송 인터페이스를 받을 수 있다. 기존 `Client*`
생성자는 호환 어댑터를 사용하며, 주입된 구현은 연결 상태·계정/호스트/컨테이너
바인딩과 비동기 stat/read 응답을 제공한다. 다운로드는 기존의 바인딩·버전·해시
검증과 취소 처리를 유지한다. 이 인터페이스만으로 Society 프로세스 경유가
구현된 것은 아니다.

`LocalService`는 Society가 소유한 계정과 `Client`를 사용하며 `LocalClient`에
파일 stat/read, 동기화와 원격 생성 요청을 제공한다. 소켓과 디렉터리의 소유자·권한과
동일 사용자 피어를 확인하고 연결한 계정 바인딩 및 호출자가 주입한 제품 권한 검사를
요청마다 다시 확인한다. 4바이트 길이와 최대 512 KiB JSON 객체 프레임을 사용하며
잘못된 입력, 권한 변경, 연결 종료 시 세션을 닫는다. 이 SDK 경계만으로 제품 앱과
Society의 실제 실행 경로가 자동 연결되거나 라이선스가 검증되는 것은 아니다.
제품은 별도로 서명을 검증한 라이선스의 계정 주체를 `LocalClient`에 설정해야 한다.
서버 주체와 다르거나 주체가 비워지면 연결을 닫아 계정 전환 간 세션 혼합을 막는다.
`observe()`는 저장소를 열지 않고 계정 연결만 유지하며 연결 종료를 알린다.
발견 서비스와 바인딩 주소는 주입할 수 있다. 기본 주소는 일반 네트워크용이며,
격리된 루프백 테스트는 `QHostAddress::LocalHost`를 명시한다.

페어링 증명의 서버 요청은 `AccountManager::postAuthenticated()`를 통해 처리한다.
이 경로는 현재 로그인 바인딩과 서버 origin을 확인하고 쿠키·리디렉션 정책을
계정 모듈에서 관리한다. 기존 `AccountSession`의 직접 로그인 복원과 릴레이 연결은
그대로이며, 이 변경만으로 제품 프로세스의 서버 접속이 제거되지는 않는다.
계정 작업 또는 저장된 페어링 상태의 비동기 복원 중 들어온 페어링 요청은 기억했다가
작업이 끝나면 현재 로그인과 바인딩을 다시 확인하고 처리한다. 로그아웃하면 대기 요청은 폐기한다.

The encrypted group-session record retains the current registered host/container
binding and up to eight private LAN address/UDP-port hints. `savedHostConnection()`
returns nothing after account, host or container changes or logout. Routes are
not authentication and are never exposed as credentials through QML.

On restart, `NearbyDevices::resumeHost()` immediately sends an account-signed
five-second discovery challenge to saved routes, alongside ordinary Bonjour/BLE
discovery. The registered host returns its fresh signed discovery record and the
challenge. The caller validates account scope, host identity, source address,
challenge, lifetime and record signature before feeding normal automatic TLS
pairing. The host validates the client's current signed record first. No old QR,
TLS socket, proof or execution nonce is restored; TLS pinning and fresh mutual
account authentication remain mandatory. Probes retry at five-second intervals
and stop when connected, paused or inactive. Old hosts ignore the additional
message and ordinary discovery remains the fallback for changed addresses.
When a verified device returns with a fresh process nonce, stale discovery
routes and any invitation for its old process are retired immediately rather
than waiting for the automatic pairing attempt's 20-second timeout.
Nonces retired by a fresh authenticated resume exchange are ignored in delayed
Bonjour callbacks for 70 seconds (bounded to 128 entries). A fresh resume proof
can reactivate that process; signed but stale discovery alone cannot roll it back.

Tests cover restart persistence, wrong-host rejection, logout, recovery without
Bonjour announcements, and rejection of a saved route with a wrong account key.

This is the authentication, search, and select download session shared by Society and the consumer app. Society UI process interruption still allows the app to use the Society container and authentication session within the same App Group. It maintains the host's verified namespace and existing TLS / HMAC pairing.

`Client::start(container)` opens the existing Society replica and synchronizes metadata and local submissions. `StorageMap::request(keys)` leaves a fixed-version download request on the disk. Processing is facilitated by `Client::synchronizeNow()` and files are used only after `StorageMap::requestState(id)` is ready. Unverified partial files do not appear in the public path upon cancellation or failure.

Model generation on the host sends a `society.generation` request via `requestHost` and receives `hostResponse`. The caller cannot specify address, credentials, or other hosts and uses only the authentication host connected to the current replica. Since model download and separate requests are handled separately, the host queue can be used before receiving the local model. Requests are not sent before connection or after logout. LAN validates both TLS and account relay in session tests.

The app completes the generation result with an atomic rename/commit inside Society and then calls synchronizeNow. If the network is interrupted, the original file and submission journal remain, and it retries on the next connection. Account and pairing ciphertext is stored by GroupSessionStore inside the selected `.societycontainer/.society-runtime/state`; only the wrapping key remains in the platform credential store.

Implementation of AccountSession, NearbyDevices, AutomaticPairing, and GroupSessionStore was migrated from the Society app to this SDK. Society's QML AccountController is a thin registration wrapper. The consumer app is responsible for app lifecycle, background execution permission, and status display policy.

자동 페어링은 발견·연결 상태 변경에 반응하며, 실제 연결 만료 또는 재시도
기한이 있는 경우에만 단발 타이머를 사용한다. 인증되지 않았거나 대기 중인 기한이
없는 유휴 상태에서는 페어링 검사 타이머를 실행하지 않는다.


Synchronization is included in Helper Core. Dependencies include iiSocietyContainer
0.16.1, iiWebServer 0.6.0, iiAccountManager 0.2.8, iiSharedCanvas Core 0.31.4,
iiFileProvider and the existing Qt adapters. Desktop state encryption uses OpenSSL Crypto and iOS uses CryptoKit. The consolidated SDK does not depend on higher-level Product sources. See Migration.md for the single Helper package and dependency direction. Headers are installed via the `iiSocietyHelper/Client.h` path. The consumer app calls start when creating the repository object, explicitly downloads/file publishes after synchronizeNow, and calls stop on background transition/termination.

`AccountSession::storageScope()` is calculated using the authentication service origin and account sub, and LAN proof and server replication use the same namespace. Replicas of other accounts are not opened. If the initial Society configuration is not complete, a new host is not arbitrarily selected. `client-session.lock` occupancy failure is retried after waiting, and connections from other processes are not forcibly terminated.

`iiSocietyHelper.session` uses an isolated account service and repository, along with actual TLS or loopback relay. Without the Society app process, it checks model request, completion, local result upload, and logout cancellation from the missing original list. Delivery of search records is via injected DiscoveryService and is not an actual Bluetooth radio performance check. Testing disables real-use account restoration.

SDK's installer automatically discovers the CMake project and dependencies in this directory. The installation order of Container, ServerHost, Sync, and Account can be verified with `SDK/install.sh --dry-run --only iiSocietyHelper`. This validation targets macOS and iOS arm64, and does not substitute for Android ·Windows packaging or device execution.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.

`AccountSession` also enables the account SDK's private Society drive-location synchronization:
after sign-in and every 30 seconds, the current account receives `societyContainerDrive` (disk ID,
host device ID, image path and revision). Other devices retain their local replica/mount paths.
A location refresh does not request pairing keys or emit account changes when the value is unchanged.
The account API and SDK contract are documented in `iiAccountManager/docs/ACCOUNT_CONTRACT.md`.

<a id="계정-graphql-호환성"></a>

## Account GraphQL compatibility

iiAccountManager 0.2.8 or higher is required. Login and container location synchronization use the `POST /Account/GraphQL` `appSession` mutation of the corresponding SDK. Pairing also sends `variables.input = {intent: "pairing", device: ...}` to the same endpoint. Storage occurs only after `data.appSession.pairing` is verified, and session expiration for HTTP 200 GraphQL errors is also passed to AccountManager. Cookie scope, account/device binding, key verification, and secure storage contract are maintained. Operational GraphQL server deployment must occur before new client deployment, and REST retry is not performed.

<a id="macos-개발-빌드의-로그인-저장소"></a>

## Container-owned account state

`GroupSessionStore::defaultDirectory()` resolves the selected valid `.societycontainer` without creating a directory. GUI, daemon and SDK consumers use `.society-runtime/state` inside that same package, regardless of the executable name or signing team. No selected package means no fallback directory and no filesystem writes. A created store follows subsequent package selections before writing.

AES-256-GCM ciphertext, 0700/0600 permissions, revocation generations and ciphertext verification remain in use. The wrapping key remains in the platform credential store. Apple production and development signatures retain their distinct Keychain services; changing file placement does not bypass access controls or copy keys between signatures. iOS file protection remains applied to container-owned ciphertext.

Removal first commits the revocation generation and a tombstone. Failure to
commit either is reported as unavailable; an asynchronous legacy Keychain
removal failure is also returned to the caller. A successful revocation marker
prevents restoration of an older ciphertext, but does not establish that the
legacy Keychain entry was removed if the process exits before its callback.
`iiSocietyHelper.session` covers failure of both the tombstone and legacy
removal using isolated fixture stores.

On the first read of a missing record, an existing previous OS Society state location is read only. Only the requested encrypted `.state` record and its revocation marker are copied byte for byte into the current package. Existing destination records or revocations take precedence, so logging out cannot resurrect an old session. Source records, locks and unrelated files are preserved. This transition does not restore or mount legacy disk images.

`iiSocietyHelper.apple_state` checks that an actual ad-hoc app and daemon resolve the same selected package and that resolving paths creates no state. Society.GroupState covers preserved ciphertext migration, restart and revocation races.

LAN clients require server-issued account pairing proofs. Both LAN and relay select the host and container in the authenticated account `societyContainerDrive`; missing registration or another host grants no synchronization. The registered replacement host adopts a fresh mirror while retaining previous files in recovery.

Apple keychain reads, writes and duplicate-item checks execute on worker threads. Completion returns to the session store thread and is cancelled when its QObject is destroyed. Keychain access prompts therefore cannot block the Society or consumer application event loop; existing access controls and encrypted state are preserved.
