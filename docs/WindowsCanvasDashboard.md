# Windows 캔버스 저장 소비자

Helper의 캔버스 저장·미리보기·버전 충돌 검사는 실제 provider catalog가 생성된 뒤에도
최근 파일에 사용자 캔버스 하나만 보이는지 확인한다. Helper의 alias 수집과
SocietyContainer의 비동기 목록은 모두 상대 경로 전체에서 provider 제어 디렉터리를
검사한다. 사용자 파일 수 기대값을 완화하지 않는다. 파일 이름이 점으로 시작하는 디렉터리의 Windows 표시 속성과
파일 시스템에서 숨기는 정책을 혼동하지 않는다.
