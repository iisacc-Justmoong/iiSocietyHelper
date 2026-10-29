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

Society가 실행되지 않아도 모델 파일과 생성 이미지를 같은 FileSystem·Alias·
관리 원본 API로 읽고 쓸 수 있다. 이후 Society는 공유 등록을 인수하여 동일한
경로·식별자·관리 이력을 사용한다. iOS의 원본은 App Group 안에 유지되며 File
Provider의 시스템 복제 경로 또는 각 앱의 Documents를 원본으로 삼지 않는다.

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
