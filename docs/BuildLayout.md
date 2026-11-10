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
