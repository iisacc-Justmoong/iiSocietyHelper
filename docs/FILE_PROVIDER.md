# File creation ownership in iiSocietyHelper

Helper/group-session directories, initialization locks and delivery database creation use iiFileProvider. The unified client-facing API, container authority and transport behavior stay in Helper.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/CanvasStorage.cpp`
- `src/DeliveryStore.cpp`
- `src/FileSystem.cpp`
- `src/GenerationStorage.cpp`
- `src/Helper.cpp`
- `src/iiSocietyHelper/Client.cpp`
- `src/iiSocietyHelper/GroupSessionStore.cpp`
- `src/iiSocietyHelper/GroupSessionStore.h`
