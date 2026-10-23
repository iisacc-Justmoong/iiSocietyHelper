# 소비 앱과 Canvas ABI 일치

Helper와 소비 앱은 같은 iiSharedCanvas ABI를 링크해야 한다. 기본 요구 버전은
0.32.0이며, 기존 Society 0.31.4 구성은 `IISOCIETYHELPER_CANVAS_VERSION=0.31.4`와
그 버전의 `iiSharedCanvas_DIR`를 함께 지정한다. 임의의 버전 간 객체 변환이나
서로 다른 Canvas 라이브러리의 동시 링크는 수행하지 않는다.

설치한 CMake 패키지는 실제 빌드에서 선택한 버전을 `EXACT` 의존성으로 기록한다.
기존 소비자의 SDK를 덮어쓰지 않아야 할 때는 별도 설치 prefix를 지정한다.
이 선택은 전송 SDK를 iiWebServer로 맞추면서 Society의 기존 Canvas ABI와
관리형 파일 저장 설정을 유지하기 위해 추가되었다.

프로필 검증은 선택한 헤더·라이브러리로 Helper와 소비자를 빌드하고, 설치된
패키지를 통해 앱을 링크하는 방식으로 수행한다. 테스트와 실제 모델 생성 결과는
서로 별도로 기록한다.

파일 시스템 회귀 검사의 Canvas 저장·미리보기·충돌 검증은 두 프로필에서
동일하게 빌드한다. 게시 메타데이터 왕복 검증은 해당 API 헤더를 제공하는
Canvas 0.32 이상에서 추가로 수행한다.
