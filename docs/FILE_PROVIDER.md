# File creation ownership in iiSocietyHelper

Helper/group-session directories, initialization locks and delivery database creation use iiFileProvider. The unified client-facing API, container authority and transport behavior stay in Helper.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/Storage/Canvas/CanvasStorage.cpp`
- `src/Delivery/DeliveryStore.cpp`
- `src/Files/FileSystem.cpp`
- `src/Storage/Generation/GenerationStorage.cpp`
- `src/Core/Helper.cpp`
- `src/Client/Client.cpp`
- `src/Storage/GroupSessionStore.cpp`
- `src/Storage/GroupSessionStore.h`

로컬 전달 저장소는 iiFileProvider Native의 SQLite 연결, 바인딩된 문과 트랜잭션을 사용한다. 기존 WAL 및 스키마, 초기화 잠금, 파일 권한, 메시지 BLOB과 소유 스레드 이전 계약을 유지한다. 동기화 저널은 별도로 Qt Sql을 사용한다.
