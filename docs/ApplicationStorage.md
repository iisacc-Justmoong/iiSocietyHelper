# iisacc 제품군의 공통 컨테이너 접근

`FileSystem::openForApplication("com.iisacc.dreamscapes")`는 기존 Society 선택을
재사용하고, 선택이 없으면 iiSocietyContainer가 결정한 제품군 공통 논리 디스크를 연다.
Windows·Linux는 앱 이름에 의존하지 않는 사용자 공유 데이터 위치를 사용한다.
데스크톱의 `IISACC_SHARED_STORAGE_ROOT`는 모든 제품에 같은 공통 루트를 지정하는
배포 설정이다. Apple은 실제 App Group, Android는 동일 서명을 검증하는 Provider로
같은 `.societycontainer` 계약에 접근한다. Android 클라이언트에는 현재 Society의
Provider 패키지 설치가 필요하다. 공유 권한이나 위치가 잘못되면 앱 전용 Documents·Cache로
원본을 대체하지 않는다. 문서 보기 객체는 기존 `Initialization::Deferred`를
유지하고 실제 저장소 소유자만 부트스트랩한다.

관찰 전용 `Helper`는 `Helper(FileSystem::Initialization::Deferred)`로 생성하여
별도의 파일 목록을 미리 열지 않을 수 있다. 이 경우 Helper의 존재 알림은 기존대로
사용할 수 있으며, 내장 FileSystem은 명시적으로 열기 전까지 선택되지 않는다.
기본 `Helper(QObject *)` 생성자는 이전처럼 저장소를 즉시 발견한다. 이 옵션은
Helper의 `start()` 또는 `stop()`이 비동기라는 뜻은 아니다.

이벤트 루프를 가진 호스트는 `startAsync()`와 `stopAsync()`를 선택할 수 있다.
시작은 경로 확인, presence 게시와 시작 기록을 워커에서 수행하고 `startFinished`로
결과를 전달한다. 종료는 새 관찰을 멈춘 뒤 기존 presence 작업과 자동 기록을
워커에서 마무리하고 `stopFinished`로 완료를 알린다. 시작 중 종료도 취소·정리를
기다린 뒤 완료된다. 동기 `start()`·`stop()`과 `sendData()` 계약은 유지한다.
비동기 경로는 주기적 관찰을 사용하므로 운영체제 파일 감시가 없는 경우와 동일한
heartbeat 간격의 지연이 있을 수 있다. 최종 프로세스 종료 전에 `stopFinished`를
기다려야 하며, 이벤트 루프 자체를 그 동안 막아서는 안 된다.

Society가 실행되지 않아도 모델 파일과 생성 이미지를 같은 FileSystem·Alias·
관리 원본 API로 읽고 쓸 수 있다. 이후 Society는 공유 등록을 인수하여 동일한
경로·식별자·관리 이력을 사용한다. iOS의 원본은 App Group 안에 유지되며 File
Provider의 시스템 복제 경로 또는 각 앱의 Documents를 원본으로 삼지 않는다.

전송처럼 저장소 선택을 오래 유지하는 작업은 시작 시 고정한 경로와 컨테이너
식별자를 `FileSystem::matchesStorage()`로 함께 검증한다. 호출할 때마다 드라이브가
여전히 유효한지 확인하므로, 같은 경로의 컨테이너가 교체되거나 Android 공유
제공자가 끊긴 경우 기존 작업을 계속하지 않는다. 경로·식별자 조회마다 같은
드라이브를 다시 검사하지 않아도 되지만, 이것은 영구 캐시나 판본 검증의 대체가
아니다. 내용의 판본과 무결성은 기존 전송 계약에서 별도로 검증한다.

논리 디스크가 처음 인증된 호스트의 미러로 전환될 때 기존 앱 파일을 공개 섹션에
보존한다. 초기 호스트 상태 적용 후 이 파일을 일반 변경 제안으로 제출한다.
충돌과 실제 바이트 검증은 기존 Replica·Synchronizer가 처리한다. 독립 패키지에
대한 기존 보관·복구 동작은 유지한다. 호스트 논리 UUID가 바뀌어도 기기 내
저장소 ID로 연결한 관리 이력은 유지한다.

Monitoring도 명시적인 논리 디스크 선언과 정상 드라이브 manifest를 확인하여
같은 소스를 감시한다. 감시 데몬이나 Society 프로세스의 실행 여부가 로컬 파일
입출력 성공 조건은 아니다.

`tests/application_storage.cpp`는 로컬 모델·생성 이미지 저장과 Society 인수,
서로 다른 제품의 같은 저장소 재사용, 잘못된 공유 루트의 대체 저장소 생성 거부,
실제 LAN TLS를 사용한 모델 수신과
기존 생성 결과의 호스트 이전을 검증한다.

동기화 완료 신호는 해당 스냅샷 라운드의 완료를 나타낸다. 최초 호스트 상태 적용
후 발견한 앱 원본은 후속 제안 라운드에서 제출될 수 있다. 회귀 테스트는 각 파일의
수신과 충돌 사본 생성, 바이트 일치까지 기다리며 저장 장치 부하를 제품의 동기화
지연 제한으로 해석하지 않는다.
