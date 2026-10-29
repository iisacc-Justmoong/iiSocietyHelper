# Society Helper 소스 책임 분류

클라이언트 계정·연결·발견·세션 저장·미디어 처리는 `src/Account`, `Client`, `Discovery`, `Storage`, `Media`에 둔다. 동기화, 모니터링, 원격 생성은 `src/Sync`, `Monitoring`, `Generation`으로 독립하며 공개 헤더와 구현을 함께 둔다. 공통 내보내기 정의는 `src/Core`에 있다.

빌드 forwarding 헤더와 설치 헤더는 기존 `<iiSocietyHelper/...>`, `<iiSocietyHelper/Sync/...>`, `<iiSocietyHelper/Monitoring/...>`, `<iiSocietyHelper/Generation/...>` 경로를 유지한다. `tests/source_layout.cmake`와 `tests/module_boundary.py`가 배치와 모듈 소유 경계를 검사한다. 실사용 컨테이너 대신 격리된 시험 데이터를 사용한다.

기존 평탄한 최상위 구현도 `Core/`, `Files/`, `Delivery/`, `Objects/`와 `Storage/`의 관리·Canvas·생성 책임으로 나누었다. 기존 `<iiSocietyHelper.h>` 및 `<DeliveryStore.h>` 설치 경로는 유지한다.

모니터링 테스트는 각 실행에 고유한 빌드 하위 임시 디렉터리를 사용하며 종료 시 정리한다. 기존 테스트 디렉터리나 실제 사용자 컨테이너를 재사용하지 않는다.
