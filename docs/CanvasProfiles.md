# 소비 앱과 Canvas ABI 일치

Helper Core와 소비 앱은 하나의 iiSharedCanvas 패키지를 선택한다. 새 빌드의 기본 최저 버전은
`0.35.39`이며 `SameMinorVersion` 정책은 같은 메이저·마이너의 이후 패치를 허용한다.
캐시된 값을 기본값으로 되돌리려면 CMake에 `-U IISOCIETYHELPER_CANVAS_VERSION`을 전달한다.
별도 프로필은 최저 버전 `IISOCIETYHELPER_CANVAS_VERSION`과 `iiSharedCanvas_DIR`을 함께 지정한다.

설치한 Helper는 빌드 때 선택한 Canvas 버전을 소비자 쪽 최저 버전으로 내보낸다.
Helper를 `0.35.41`로 빌드하면 소비자는 해당 패치부터 사용할 수 있다. Core 타깃을
이미 가져온 경우에는 같은 설치 경로에서 이 요구를 충족해야 한다. 별도 프로필을 검증할
때는 서로 다른 설치 prefix를 지정한다.

프로필 검증은 선택한 헤더·라이브러리로 Helper와 소비자를 빌드하고, 설치된
패키지를 통해 앱을 링크하는 방식으로 수행한다. 테스트와 실제 모델 생성 결과는
서로 별도로 기록한다.

파일 시스템 회귀 검사의 Canvas 저장·미리보기·충돌 검증은 두 프로필에서
동일하게 빌드한다. 게시 메타데이터 왕복 검증은 해당 API 헤더를 제공하는
Canvas 0.32 이상에서 추가로 수행한다.
