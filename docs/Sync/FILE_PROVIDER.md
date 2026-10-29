# File creation ownership in iiSocietyHelper::Sync

Confined file creation, transfer staging and replica persistence use provider factories. Empty journal database creation is reserved before the domain SQL driver opens it. Namespace/revision and synchronization rules remain here.

The common [ownership contract](../../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/Sync/ConfinedFiles.cpp`
- `src/Sync/ConfinedFiles_win.cpp`
- `src/Sync/ObjectProvider.cpp`
- `src/Sync/RemoteFiles.cpp`
- `src/Sync/Replica.cpp`
