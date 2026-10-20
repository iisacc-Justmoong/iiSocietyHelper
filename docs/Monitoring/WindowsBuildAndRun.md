# Monitoring in the unified Helper Windows build

Build and install **iiSocietyHelper 0.10.0**, following the
[common Windows procedure](../WindowsBuildAndRun.md). This feature no longer
has a standalone SDK directory, version, install script or CMake package.
Its source tests are registered under `iiSocietyHelper.Monitoring.*` in Helper's `build/`.
See [component options and migrated headers](../Consolidation.md).

The former standalone package's Windows execution results remain in the
[historical SDK verification record](../../../Windows/Verification-20261020.md).
They must not be reported as merged-release execution results. The current
source migration retains native platform implementations and their tests;
actual execution evidence is platform-specific.
