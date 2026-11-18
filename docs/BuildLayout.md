# Build and test resources

Windows SDK의 `min`·`max` 매크로가 켜진 소비자에서도 모니터링 옵션의 기본
항목 한도를 선언할 수 있도록 표준 극한값 호출을 보호한다. 모니터링 테스트
빌드에는 해당 포함 순서의 컴파일 전용 검사가 포함된다. 이 변경의 Windows
빌드와 설치는 아직 수행하지 않았다.

The module, daemon and test executables are built under `build/`. Existing
Society and Dreamscapes package references use a guarded `build/quit-fix-stage`
link to the retained package in `resources/dependencies/quit-fix-stage`.
Configuration refuses to replace unrelated data or a conflicting link.

Native tests obtain a unique `resources/.t` workspace, and fixture creation,
Qt test output and temporary process data use that workspace. Normal return and
test failure remove the owned workspace. Cross-target tests retain the
`IISOCIETYHELPER_SYNC_TEST_DIRECTORY` device-path override and CTest's target
emulator handling. A forced termination may leave its workspace for review.
Earlier test containers, user-authored probes and potentially private data
are preserved under `resources/.recovery/helper-history-20261031`.

Tests that start daemons, networking, GUI or device access remain available
but must be explicitly requested before execution. No test timeout or storage
serialization requirement is reduced by the workspace changes.

## 로컬 프레임 CTest

로컬 프레임의 분할·병합과 비정상 크기·형식 검사는 CTest가 실행하는 C++
실행 파일로 유지하며 Qt Test를 링크하지 않는다. 해당 검사는 데몬이나 네트워크를
시작하지 않는다. 다른 Helper 검사의 Qt Test 의존성은 별도로 남아 있다.

격리된 파일 경계 검사는 CTest용 C++ 실행 파일로 분할 파일·해시·경로 제한을
확인한다. 파일 해시 진행 콜백의 검증 결과를 호출 뒤에 확인하며, Windows 전용
경우와 임시 작업 공간 정리 계약은 그대로 유지한다.

로컬 전달 저장소 검사는 CTest용 C++ 실행 파일에서 격리된 임시 저장소와
프로세스 내부 Helper를 사용한다. 큐의 재시작·확인·롤백 검사는 유지하며,
실행 중인 사용자의 Society 저장소나 별도 데몬은 사용하지 않는다.

프레즌스 작업자의 큐 제한·종료·이벤트 루프 응답성 검사는 CTest용 C++
실행 파일에서 Qt Core 이벤트 처리만 사용하고 Qt Test를 링크하지 않는다.

계정 참조 검사는 명시적 신호 계수와 CTest 실행 파일을 사용한다. QML
표현식은 프로세스 안에서 평가하며, 계정 참조와 재진입 검사는 그대로 유지한다.

근거리 부트스트랩 인코딩과 BLE 중지 검사는 CTest로 실행하며 두 기존
데이터 행을 유지한다. `Bootstrap.Offline` 선택은 로컬 네트워크 인터페이스를
조회하는 잘못된 끝점 행을 제외하고 라디오가 비활성화된 행만 실행한다.

객체 직렬화·알림·프로세스 간 복원 검사는 CTest로 실행하며 격리된
테스트용 자식 프로세스를 사용한다. 신호 횟수는 Qt Test 대신 타입이
확인된 연결로 기록한다.

로컬 클라이언트와 TLS 소켓 전송 검사는 CTest 실행 파일과 신호 인자 기록기를
사용한다. 두 검사는 각각 로컬 소켓과 TLS 서버를 사용하므로 명시적 실행
요청 없이 실행하지 않는다. 설치형 전달·객체·계정 검사도 Qt Test를 링크하지
않는다.

앱 공유 저장소의 네 사례는 CTest에서 유지하고 소스·설치 소비자 양쪽의
Qt Test 링크를 제거했다. `application_storage_offline` 선택은 격리된 공유
경로와 사용할 수 없는 경로를 검사하며 TLS 서버 사례는 실행하지 않는다.

프로세스 관찰의 중단·재시작 및 플랫폼별 일시정지 검사는 CTest에서 기존
신호 인자와 건너뛰기 조건을 유지한다. 이 검사는 관찰용 자식 프로세스를
실행하므로 별도 실행 요청이 필요하다.

생성 프로토콜의 기존 여섯 사례는 가짜 생성 엔진과 수락·완료 신호 인자를
보존한 CTest 실행기로 이전한다. 소스와 설치형 소비자는 Qt Test에 직접
링크하지 않으며, 임시 모델과 결과는 리소스 fixture에 둔다. macOS 테스트
실행 파일은 선택된 iiSharedCanvas의 런타임 경로를 포함한다.

인덱싱의 기존 여섯 사례와 플랫폼별 건너뛰기는 CTest에서 유지한다.
인덱스 열기 경합은 단독 선택할 수 있으며, 소켓과 대형 파일을 사용하는
사례는 별도 실행 요청이 필요하다.

동기화 복제의 기존 21개 사례는 CTest 실행기로 유지하며 파일시스템 조건에 따른 건너뛰기를 보존한다. 소스 및 설치형 소비자 타깃에서 QtTest 링크를 제거하고, 네트워크 없이 지속 저장을 확인하는 별도 사례를 제공한다.

GenerationHost는 QtConcurrent API를 사용하지 않으므로 이 타깃과 해당 패키지 구성에서 Qt6::Concurrent 요구를 제거한다. 현재 호스트는 여전히 Qt Core의 Future와 객체 API를 사용하며, Apple 상태 저장 경로의 별도 QtConcurrent 요구는 유지된다.

Apple 상태 저장의 기존 두 사례는 CTest에서 선택할 수 있다. 경로 fixture는 resources 작업 공간에 두고, 키체인 비동기 완료의 스레드 확인은 콜백이 끝난 뒤 검사한다. 실제 키체인 왕복은 별도 실행 요청이 필요하며 QtConcurrent를 사용하는 기존 구현은 유지된다.

호스트 드라이브의 기존 여덟 사례는 CTest 실행기로 유지한다. 뮤테이션 잠금 중 고정 읽기 사례는 별도로 선택할 수 있고, 드라이브 접근의 경계·페이징·정보 검사는 기존 리소스 작업 공간과 대기 제한을 사용한다.

동기화 스냅샷의 기존 여덟 단일 사례와 두 시계 데이터 행을 CTest 실행기로 유지한다. 신호 콜백의 검사 실패는 콜백 밖으로 전달하며, 요청 기한 검사는 네트워크 없이 단독 선택할 수 있다.

동기화 네임스페이스의 기존 열두 사례를 CTest 실행기로 유지한다. 서버 fixture가 필요한 S3 사례는 설정이 없으면 건너뛰며, 업로드 확인 콜백에서 본 원본과 수정 성공 여부는 콜백 이후 검사한다. 로컬 이력 검사는 단독 선택할 수 있다.

온디맨드 동기화의 기존 열한 사례는 CTest 실행기로 유지한다. 취소 진행 신호에서 실제 취소가 요청되었고 모두 성공했는지는 콜백 뒤 검사하며, 변경된 버전의 전송 거부는 서버 없이 단독 선택할 수 있다.

동기화 컨트롤러의 열한 사례는 CTest 실행기로 유지한다. 예상 경고의 관찰과 기존 로그 핸들러 복원, 콜백 스레드 및 전송 종류 검사를 보존한다. 재시작 검사는 서버 없이 선택할 수 있고 실제 로컬 TLS 검사는 별도로 실행한다.

동기화 전송의 CTest는 기존 독립 사례와 전송 창·방향별 데이터 행을 보존한다. 사례 이름 또는 `사례:행`으로 선택할 수 있으며, 콜백에서 관찰한 범위·순서·메타데이터와 파일 변경 실패는 완료 후 검증한다. 기본 호스트 소유권 사례는 네트워크 없이 실행할 수 있다.

관찰 테스트 14개는 CTest에서 사례별로 선택한다. 비동기 시작·중지의 게이트는 실패 시에도 소유 객체보다 먼저 해제하며, 관찰·이탈 신호와 이벤트 처리 시간은 기존 검사와 같이 유지한다. 오프라인 선택 검사는 별도 프로세스나 서버를 시작하지 않는다.

파일시스템 검사는 CTest에서 기존 사례와 캔버스 저장 방식별 다섯 데이터 행을 선택할 수 있다. 각 행마다 별도 리소스 작업 공간과 설정 경로를 초기화하고 지연 객체를 정리한다. 안전한 오프라인 선택 사례와 별도로 기존 독립 프로세스·데몬 사례를 보존한다.

클라이언트 세션의 기존 사례와 네 전송 경로 행도 CTest 실행기로 유지한다. 신호 인자와 실패 전파, 이벤트 대기 및 독립 사례 선택을 보존한다. 전체 테스트와 설치 소비자에서 QtTest 패키지·링크를 요구하지 않으며, 기존 서버·계정 연동 사례와 가짜 저장소 오프라인 사례를 구분해 실행한다.
