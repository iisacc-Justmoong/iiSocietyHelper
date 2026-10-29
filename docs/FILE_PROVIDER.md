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
