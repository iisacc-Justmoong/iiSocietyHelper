# 기기 스냅샷과 안전 동기화

`iiSocietyHelper::Sync::Synchronizer`는 각 기기의 `.societycontainer`를 로컬에서
검사하고, 미확정 변경을 고정된 스냅샷으로 만들어 인증된 호스트에 전송한다.
파일 내용은 기존의 검증된 청크 전송을 사용한다. 컨테이너 전체를 매번 복사하는
대신, 확인된 커서 이후의 변경과 명시적인 삭제 레코드를 보낸다.

## 스냅샷 생성과 호스트 분석

호스트의 `describe` 응답은 `snapshotVersion: 1`을 제공한다. 새 송신자는 이
기능을 제공하는 호스트만 사용하며, 지원하지 않으면 `snapshot_protocol_required`로
실패한다. 기존 `protocol: 2`, `namespaceVersion: 1`과 인증·계정·컨테이너 경계는
계속 적용된다. 모든 기기가 새 스냅샷 계약을 사용하려면 송신 기기도 갱신해야 한다.

각 `manifest` 페이지의 `snapshot`은 다음 필드를 가진다.

| 필드 | 의미 |
| --- | --- |
| `schema` | 스냅샷 형식 버전, 정수 `1` |
| `id` | 송신 기기에서 생성한 UUID |
| `capturedAt` | 송신 기기의 UTC Unix epoch 밀리초를 십진 문자열로 표현한 시각 |
| `baseSequence` | 송신 기기가 확인한 호스트 저널의 순번 문자열 |
| `baseRevision` | 그 순번에 해당하는 호스트 리비전, 초기 상태는 빈 문자열 |

페이지는 같은 스냅샷 헤더, 송신 복제본 ID, 커서 범위와 전체 항목 SHA-256을
공유한다. 페이지당 128개·256 KiB, 전체 250,000개, 동시 기기 32개로 제한한다.
내용 해시, 벡터 시계와 기준 리비전을 포함한 항목이 전송 도중 바뀌면 기존
스냅샷에 섞지 않고 실패시키거나 다음 스냅샷에서 다시 제출한다. OS의 원자적인
볼륨 스냅샷을 생성하는 API가 아니라, 검사·고정된 변경 메타데이터의 스냅샷이다.

호스트는 스냅샷의 기준 리비전이 자신의 이력에 존재하는지 확인하고, 마지막
페이지까지 해시를 검증한 후 `diff`에 `added`, `updated`, `deleted`, `unchanged`,
`conflicts`의 정수 개수를 응답한다. 분석 자체는 공개 파일이나 변경 저널을
수정하지 않는다. 페이지 분석은 해당 시점의 확정 이력을 기준으로 하며, 이
계획을 파일에 적용할 권한으로 취급하지 않는다.

`begin/chunk/commit`은 스냅샷 ID와 manifest ID를 함께 전송한다. 호스트는 분석한
정확한 항목과 일치하는 요청만 받는다. 적용 직전에 operation lock 안에서 실제
파일과 확정 리비전을 다시 확인한다. 그 사이 호스트나 다른 기기가 수정했다면
기존 충돌 처리로 양쪽 내용을 보존한다. stale 삭제가 새 파일을 지우거나,
메타데이터 우선 기기의 미다운로드 파일이 삭제로 해석되어서는 안 된다.

`capturedAt`은 출처·진단용 시각이다. 시계 오차나 시각 역행을 허용하고, 최신
파일의 승자나 삭제 권한을 이 시각으로 결정하지 않는다. 호스트의 확정 리비전,
항목별 `baseRevision`, 파일 해시와 물리 상태가 병합 판단 기준이다.

송신자는 모든 항목을 처리한 뒤 `snapshot.complete`로 호스트 확인을 받는다.
호스트는 모든 항목의 적용을 확인하고 로컬 SQLite의 `sync_snapshots`에 기기별
최신 완료 영수증을 저장한다. 영수증은 스냅샷 헤더·manifest 해시·커서·차이
요약·호스트 수신 시각을 포함한다. `Replica::lastSnapshot(peer)`로 조회하며
재시작 후에도 유지된다. 이 메서드는 Replica를 소유한 스레드에서 호출한다.
미완료 전송은 완료 영수증을 남기지 않는다. 완료 요청의 재전송은 같은 영수증을
반환하고, 호스트가 재시작했다면 전송 계획부터 다시 제출한다. 검증된 부분 파일과
저널은 기존 재시도 계약을 유지한다.

## 정수 상태와 iiSDKCore 쿼리

`Controller::status()`, `Synchronizer::status()`와
`Client::synchronizationStatus()`는 동일한 `Sync::Status` 계약을 제공한다.
`Client::status()`의 기존 문자열 연결 상태와 별개이다.

| 값 | `StatusCode` | 의미 |
| --- | --- | --- |
| `0` | `Before` | 아직 동기화를 시작하지 않았거나 새 컨테이너를 선택한 상태 |
| `1` | `InProgress` | 스냅샷 분석·송수신·확정 처리 중 |
| `2` | `Complete` | 호스트 영수증 및 로컬 확정 반영까지 성공한 상태 |
| `3` | `Failed` | 인증·프로토콜·전송·검증·저장 실패 |
| `4` | `Cancelled` | 진행 중인 동기화 취소 또는 컨테이너 종료 |

상태 조회 응답과 변경 구독의 값은 모두 `int` 하나이다. 진행률이나 객체 전체를
포함하지 않는다. 같은 상태를 반복 방출하지 않으며, 다음 동기화는 다시 `1`로
전이한다. 호스트 Controller는 여러 기기의 수신 작업이 남아 있으면 `1`을 유지하고,
진행 없는 수신은 120초 후 실패로 처리한다. 직접 `drive.*` 접근과 파일 색인은
컨테이너 간 동기화 완료로 간주하지 않는다. 상세 실패는 송신측의 기존
`errorString()`·`finished` 결과와 호스트 요청의 오류 응답을 사용한다.

```cpp
#include <iiSocietyHelper/Sync/Controller.h>
#include <iiSDKCore/Runtime/EventLoop.h>

ii::runtime::EventLoop receiver;
auto &status = controller.status(); // Client는 client.synchronizationStatus()
auto subscription = status.changes().subscribe(receiver, {}, [](auto event) {
    const int state = event.value;
    // 감시 객체의 실행 영역에서 state만 처리한다.
});
auto current = status.query().send(iiSocietyHelper::Sync::ReadStatus{});
if (current) consume(*current);
receiver.poll(); // 실제 앱에서는 수신 객체의 Core 실행기를 구동한다.
```

`query().send()`는 atomic 현재값을 읽으므로 다른 스레드에서도 호출할 수 있다.
비동기 조회는 `query().post(receiver, ReadStatus{}, completion)`을 사용한다.
구독은 수신 객체가 선택한 EventLoop/SerialQueue/Runtime에서 실행되며 새 스레드를
생성하지 않는다. 구독 자체는 이전 상태를 재생하지 않으므로 먼저 구독한 뒤
현재값을 조회한다. 현재값 조회와 이벤트의 원자적인 결합은 제공하지 않는다.
코어 큐의 용량·역압력·수명·취소 계약을 따르며, 장시간 수신하지 않은 구독은
`stats()`와 `resume()`으로 전달 상태를 확인하고 현재값을 다시 조회할 수 있다.
구독과 캡처한 수신 상태를 안전한 순서로 닫는다.

공개 쿼리 헤더 때문에 Helper Core 소비자는 `iiSDKCore >= 0.18`의 Runtime에
공개 의존한다. Monitoring 전용 구성은 기존 최소 버전을 유지한다. Qt의 `slots`
매크로 충돌은 Helper 헤더 경계에서만 처리하며 Core에 Qt 의존성을 추가하지 않는다.

## 검증

`tests/sync/snapshot.cpp`는 로컬 시각, 호스트 분석 선행, 항목 고정, 불완전 완료
거부, 시계 오차, 분석 이후 호스트 수정과 충돌 보존, 영수증 재시작 복원,
정수 쿼리·수신 루프 구독·실패·취소를 검증한다. 기존 Sync 및 실제 LAN TLS/relay
통합 테스트도 함께 실행한다. `example/sync_status.cpp`는 공개 상태 API의 실행 예제이다.

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

### 2026-10-25 검증 기록

- macOS arm64에서 iiSharedCanvas 0.32.0, Qt 6.8.3, iiSDKCore 0.18.1을 선택하여 전체 빌드를 완료하였다.
- 전체 CTest 31개 중 최초 30개가 통과하였다. TLS 8,388,625바이트 재개 테스트는 설치 소비자 빌드와 겹친 실행에서 30초 기준을 초과하였다. 빌드 종료 후 같은 30초 기준으로 분리 재실행하여 재개 다운로드 16.697초, 업로드 19.026초와 동일 SHA-256을 확인하였다. 제한 시간을 변경하지 않았다.
- 새 Snapshot QtTest는 초기화·정리를 포함한 10개 항목이 통과하였다. 기존 Controller의 실제 TLS 전송에서도 호스트와 클라이언트의 정수 상태 `1`·`2` 구독 전달을 검증하였다.
- `build/snapshot-sdk`에 설치하고 `build/snapshot-consumer/build`의 독립 소비자에서 설치본 테스트 6개를 실행하였다. 최초 5개가 통과했고, 기존 직접 다운로드의 LAN 데이터 사례는 첫 바이트 수신 전 35초 대기 제한에 걸렸다. 해당 LAN 사례를 같은 기준으로 분리 재실행하여 통과하였다. 원래 실패와 재실행 결과를 모두 보존하였다.
- 로더 기록으로 소비자가 staging 설치의 Helper와 선택한 Canvas 0.32.0을 실제 로드한 것을 확인하였다. 검증 대상은 격리된 로컬 복제본·계정 fixture·LAN TLS·relay이며, 물리적인 여러 기기의 제품 배포 증거와 구분한다.

재현 환경과 원본 결과는 `build/snapshot-source-environment.json`,
`build/snapshot-full-tests.xml`, `build/snapshot-tls-retry.xml`,
`build/snapshot-installed-tests.xml`, `build/snapshot-installed-lan-retry.xml`,
`build/snapshot-installed-loader.log`, `build/snapshot-validation.json`에 남긴다.
