# Society Helper consolidation verification (2026-10-20 to 2026-10-21)

## Delivered ownership

The active SDK is iiSocietyHelper **0.10.0**. The former Generation, Monitor and
Sync source projects and default installation prefixes have been retired.
All 30 original implementation/header files map to Helper ownership, including
the replacement export headers. Sync and Generation::Remote are in Core;
Monitoring is native and Qt-free; desktop GenerationHost is optional. All three
are components of one versioned package. See [the public contract](Consolidation.md).

Society, Dreamscapes and PhotoLibrary consumers use the new package, targets,
headers and namespaces. Society's actual NetworkDriveController compiles in the
application-consumer project. Mobile build plans, the Windows dependency graph,
workspace repository registry and installer ordering use the unified SDK.
Lower-level Container, Core, FileProvider and inference modules have no reverse
Helper dependency. PhotoLibrary's Helper dependency is test-only.

## Recorded macOS validation

The host is native macOS arm64, AppleClang 21, CMake 4.4 and Qt 6.8.3 for the
remaining Qt adapters. C++23 is required; no VM or interpreter is added to the
SDK runtime. Test orchestration scripts are development tools.

- Configuration: Full staged source suite; Result: 28/28 passed; Evidence in consolidation directory: `tests-corrected.xml`
- Configuration: Fresh build against default SDK dependencies; Result: 28/28 passed; Evidence in consolidation directory: `default-source-tests.xml`
- Configuration: Installed staged Core/Sync/GenerationHost; Result: 11/11 passed; Evidence in consolidation directory: `consumer-tests.xml`
- Configuration: Installed default SDK Core/Sync/GenerationHost; Result: 11/11 passed; Evidence in consolidation directory: `default-consumer-tests.xml`
- Configuration: Monitoring-only source, Qt discovery disabled; Result: 2/2 passed; Evidence in consolidation directory: `native-final-tests.xml`
- Configuration: Native installed consumer, Monitoring-only package; Result: 1/1 passed; Evidence in consolidation directory: `native-consumer-retest.log`
- Configuration: Native installed consumer, complete staged package; Result: 1/1 passed; Evidence in consolidation directory: `full-native-tests.xml`
- Configuration: Native installed consumer, default SDK; Result: 1/1 passed; no Qt/Core facade/inference loaded; Evidence in consolidation directory: `default-native-tests.xml`, `default-native-loaded-libraries.log`
- Configuration: Standard `install.sh`, end to end; Result: Exit 0; source 28/28 and installed 11/11 passed; Evidence in consolidation directory: `installer-verified.log`
- Configuration: Society/Dreamscapes application consumers; Result: 4/4 passed; Evidence in consolidation directory: `products-tests.xml`
- Configuration: PhotoLibrary, including installed consumers and real TLS integration; Result: 8/8 passed; Evidence in consolidation directory: `photo-tests.xml`
- Configuration: Workspace ownership/ordering/Windows metadata; Result: 4/4 passed; Evidence in consolidation directory: `workspace-tests-final.log`
- Configuration: Society mobile build plans and photo bridge; Result: 3/3 and 2/2 passed; Evidence in consolidation directory: `ios-plan-tests.log`, `photo-plan-tests.log`
- Configuration: FileProvider workspace creation audit; Result: 0 violations, 0 unreadable sources; Evidence in consolidation directory: `file-creation-audit.json`

The evidence directory is
`SDK/build/society-helper-merge-20261020` under Workspace. Results are per
configuration; counts are not summed as unique application scenarios.
Monitoring-only default package lookup rejects a missing Core explicitly;
`COMPONENTS Monitoring` works without importing Qt, Core or inference targets.
Repeated component discovery is tested. The default native consumer was also run
with loader tracing: it loaded no Qt, Helper Core facade or inference backend.
Binary dependency records reject all
three retired library names and Qt dependencies from native Monitoring/daemon.

Generation keeps `.society-runtime/iiSocietyGeneration` as a persistent data path.
A regression checks the actual output paths. Existing wire schemas, IDs, hashes,
confinement, authorization, cancellation, recovery and publication rules remain.
Generation protocol tests use an injected inference engine and synthetic model;
they do not measure model quality or GPU inference.

## Installation and test scheduling repairs

The default SDK dependency set required Canvas Core 0.31.2. The already validated
0.31.2 package and its static libzip dependency were installed, with native
library paths relocated to the default SDK prefixes. Existing installations are
preserved in `default-installations-before.zip`.

The first standard installer consumer reused compiler implicit include paths
from a cache created with old SDK environment overrides. Installed Canvas and
inference headers were present but their include directories were suppressed.
The installer now clears child-process header/loader overrides, discovers SDK
peers, limits build parallelism and recreates only its owned consumer cache.
A fresh default installed consumer subsequently built and passed all 11 tests.
Shell profiles and product build caches were not edited by this installer repair.

Some combined parallel runs exceeded existing 1.8-second/5-second transfer and
fixture lock deadlines. The consolidated source and installed suites now share
a CTest storage resource lock. Timeouts and assertions were not relaxed, and
concurrent peers/threads inside scenarios remain tested. An isolated comparison
ran both the archived original Controller and merged Controller: both passed
all 11 QtTest cases. The later complete source run passed 28/28 with parallel
CTest requested and the resource lock in effect (`installer-verified.log`).
Failed diagnostic logs remain alongside passing results.

## Preservation and scope

Original repositories, complete Git history, ignored build files and existing
working changes are under `retired/`. HEAD and porcelain status matched before
and after each move (`retired-modules.json`). The old default SDK prefixes are
under `retired-installations/`. No Git reset, remote repository deletion or live
container migration was performed. Source/consumer backups precede the changes.

These results establish native macOS source, runtime, package and consumer
behavior. Windows/Linux execution and physical Android/iOS validation were not
performed for this merged release. Historical Windows reports retain the former
package names and results; they are not merged-release execution evidence.
Application consumer tests create plain executables and fixture containers.
Product application deployment and process restart are separate operations.
