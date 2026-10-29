# iiSocietyHelper 작업 규칙

- 구현과 같은 이름의 헤더를 `src/` 아래에 함께 둔다. 하위 계층은 제품 코드를 참조하지 않는다.
- 동기화의 인증·계정 범위·컨테이너 ID·호스트 확정 리비전 경계를 보존한다.
- 시계는 스냅샷 출처를 나타내며 덮어쓰기 권한을 결정하지 않는다. 충돌 원본과 미확정 편집을 보존한다.
- 객체 간 동기화 상태는 iiSDKCore 쿼리 포트와 구독으로 전달한다. 새 전역 버스나 Core의 도메인 의존성을 만들지 않는다.
- 변경에는 회귀 테스트와 한국어 `docs/` 문서를 포함하고 `README.md`는 영어로 작성한다.
- 빌드와 테스트 산출물은 `build/` 아래에 둔다. 실사용 컨테이너·설치 앱·진행 중인 작업을 테스트 fixture로 사용하지 않는다.
- `install.sh`는 설치 절차를 제공한다. 검증 중의 설치 소비자는 `build/` 아래에 별도 설치한 SDK를 사용한다.

<!-- workspace-docs:agent:start -->
## 문서 구조와 에이전트 탐색

- 프로젝트 루트의 `README.md`는 사람용 영문 진입 문서이며 `AGENT.md`는 에이전트 진입점이다.
- 하위 인간용 Markdown과 구조화된 agent JSON을 함께 유지한다. Markdown이 원본이며 JSON은 원문 해시와 줄 범위를 가진 파생 문서이다.
- 먼저 [docs/agent-index.json](docs/agent-index.json)에서 관련 문서와 JSON 투영을 찾는다. 원문 SHA-256이 다르면 현재 Markdown을 읽고 투영을 갱신한다.
- JSON의 규칙 후보는 검색용 어휘 일치 결과이다. 과거 기록·예제·실패 상태를 현재 지시나 검증 완료로 자동 해석하지 않는다.
- 기존 프로젝트별 규칙, 동결된 증거, 라이선스, 외부 원본, 사용자 데이터와 다른 작업의 변경을 보존한다.
- 이 문서 재편 지침은 과거 작업의 기능 구현·배포를 자동 재개하라는 요청이 아니다. 현재 사용자의 작업 범위를 우선한다.
- 문서를 수정하면 JSON 쌍·색인을 함께 갱신하고 검증한다. 기존 소스 변경에 대한 빌드·테스트 규칙은 계속 따른다.

Workspace에서 이 프로젝트의 문서를 갱신하는 명령은 다음과 같다.

```sh
python3 ../../Engineering/WorkspaceDocumentation/App/workspace_docs.py --workspace ../.. sync --project SDK/iiSocietyHelper
```
<!-- workspace-docs:agent:end -->
