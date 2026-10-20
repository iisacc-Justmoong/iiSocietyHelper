# Unified Society Helper SDK

Version 0.10.0 consolidates the former generation, monitor and synchronization
SDKs into the iiSocietyHelper package. Their source implementations and tests
belong to this repository. Applications no longer discover or link those three
separate packages. Local IPC, account/client APIs, file/canvas/media operations,
remote generation, synchronization and native monitoring share one owner.

| Capability | CMake component / target | Public C++ API |
| --- | --- | --- |
| Accounts, client, storage, local IPC, sync, remote generation | Core / `iiSocietyHelper::iiSocietyHelper` | Existing Helper APIs, `iiSocietyHelper::Sync`, `iiSocietyHelper::Generation::Remote` |
| Native container monitoring and daemon | Monitoring / `iiSocietyHelper::Monitoring` | `iiSocietyHelper::Monitoring` |
| Desktop inference host | GenerationHost / `iiSocietyHelper::GenerationHost` | `iiSocietyHelper::Generation::Host` |

These are components of one SDK package, not independently versioned modules.
Monitoring remains a C++23 native library with no Qt header or binary dependency.
Core uses the existing Qt adapters. Remote generation belongs to Core and works
on supported mobile clients. The desktop host is optional, so mobile and native
monitor consumers do not import the diffusion inference backend.

```cmake
find_package(iiSocietyHelper 0.10.0 CONFIG REQUIRED)
target_link_libraries(application PRIVATE iiSocietyHelper::iiSocietyHelper)

# Desktop hosts explicitly select the inference component.
find_package(iiSocietyHelper 0.10.0 CONFIG REQUIRED COMPONENTS GenerationHost)
target_link_libraries(host PRIVATE iiSocietyHelper::GenerationHost)
```

```cpp
#include <iiSocietyHelper.h>
#include <iiSocietyHelper/Sync/Replica.h>
#include <iiSocietyHelper/Generation/Remote.h>
// Only desktop hosts include this header and link GenerationHost:
#include <iiSocietyHelper/Generation/Host.h>
```

Native monitoring consumers can disable Qt discovery entirely:

```cmake
find_package(iiSocietyHelper 0.10.0 CONFIG REQUIRED COMPONENTS Monitoring)
target_link_libraries(monitor PRIVATE iiSocietyHelper::Monitoring)
```

```cpp
#include <iiSocietyHelper/Monitoring/Monitor.h>
iiSocietyHelper::Monitoring::Publisher publisher;
```

The source project also supports `IISOCIETYHELPER_BUILD_CORE=OFF` for a native
monitoring-only installation. Full desktop builds default
`IISOCIETYHELPER_BUILD_GENERATION_HOST` to ON; mobile/WebAssembly builds default
it to OFF. `IISOCIETYHELPER_WITH_BLE` controls the existing BLE discovery backend.
All targets require C++23. GenerationHost requires Core; Core consumes Monitoring.
Monitoring never references Core. Export sets and package dependency discovery
follow that direction, including repeated component discovery by a consumer.

## Preserved data and process contracts

The consolidation does not migrate live containers or restart running apps.
Container IDs, authentication/account proofs, journal revisions, wire formats,
chunk hashes, recovery plans, cancellation and object/file presentation contracts
remain intact. Generation's existing `.society-runtime/iiSocietyGeneration`
directory is retained as a persistent data path so outputs retain their existing location. The in-memory job queue still has
the existing process lifetime; a host restart does not recover accepted jobs. Its name does not imply a dependency on a
retired SDK package.

`SocietyContainerDaemon` is now installed by Helper's Monitoring component.
`ii-society-helper` remains the Core observation/diagnostic executable. Both are
plain native executables. The native daemon does not pull in the Core facade or
Qt Quick. Products resolve the daemon from the selected Helper installation.

## Migration and verification

Replace includes with `iiSocietyHelper/Sync/...`,
`iiSocietyHelper/Monitoring/Monitor.h` and `iiSocietyHelper/Generation/...`.
The corresponding namespaces are `iiSocietyHelper::Sync`,
`iiSocietyHelper::Monitoring` and `iiSocietyHelper::Generation`. No compatibility
package or old target alias is installed. Existing Helper APIs remain available.

The original protocol, monitor, process, synchronization, recovery, filesystem,
account and client tests are retained under Helper. Installed Core/Monitoring/
GenerationHost consumers verify the new exports. Native-only configuration and
linkage checks independently protect the no-Qt monitoring contract. Workspace
consumers cover migrated Society and Dreamscapes headers, host generation and
photo synchronization. Platform build plans and the Windows dependency graph
use the single package.

Source and repository history of the retired modules are preserved outside the
active SDK tree under the consolidation evidence's `retired/` directory. This
archive includes existing uncommitted work; no repository reset or remote
repository deletion is part of the consolidation.

The repository's `install.sh` discovers installed SDK peers under
`~/.local/SDK`, preserves explicit `CMAKE_PREFIX_PATH` priority, builds and tests
Core and selected components, installs, then builds/tests a separate installed
consumer in `build/consumer/build`. It clears inherited header and loader
search overrides for these child commands, without editing shell profiles. The installer recreates only its consumer
verification cache to avoid stale compiler implicit-header paths. Build parallelism
is controlled by `CMAKE_BUILD_PARALLEL_LEVEL` (four by default); CTest accepts
`CTEST_PARALLEL_LEVEL`.
The workspace installer also discovers dependency declarations from the
component CMake files, so lower-level packages precede Helper. Workspace tests
check registry removal, this ordering, Windows edges and Product package usage.

The consolidated source and installed-consumer CTest suites share a storage
resource lock. Their strict transfer and cancellation deadlines are unchanged.
This prevents parallel CTest jobs from competing for the fixture volume;
concurrent clients, processes and worker threads inside each scenario remain
part of the tests. It does not serialize application instances or runtime APIs.

Recorded validation and preservation evidence: [ConsolidationVerification.md](ConsolidationVerification.md).
