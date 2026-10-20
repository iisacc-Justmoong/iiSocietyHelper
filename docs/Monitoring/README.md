# Native monitoring in iiSocietyHelper 0.10.0

Monitoring and SocietyContainerDaemon belong to the single Helper package.
The C++23 native library uses iiSDKCore, iiFileProvider CoreIO and
`iiSocietyContainer::FileTree`; it has no Qt header or binary dependency.
Atomic snapshots are created through iiFileProvider CoreIO.

From the Helper repository root, a monitoring-only build uses:

```sh
cmake -S . -B build -DIISOCIETYHELPER_BUILD_CORE=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build
```

Select installed native dependencies through `CMAKE_PREFIX_PATH`. Consumers use
`find_package(iiSocietyHelper 0.10.0 CONFIG REQUIRED COMPONENTS Monitoring)` and
link `iiSocietyHelper::Monitoring`. Include
`<iiSocietyHelper/Monitoring/Monitor.h>`; API namespace is
`iiSocietyHelper::Monitoring`. A default package lookup requires Core even in a
monitoring-only installation. Repeated explicit component lookups are supported.

`SocietyContainerDaemon --watch --root PATH` and `--read-state --root PATH`
provide JSON diagnostics without publication. See `--help` for bounded scans.
The [protocol](Protocol.md) describes state, liveness and ownership contracts.
The source tests are `tests/monitoring`, and `tests/native-consumer` independently
rejects Qt, the Core facade and the inference target. Monitoring is also available
in the complete Helper installation without importing those components.

See [Windows build and execution](WindowsBuildAndRun.md) and
[all component options](../Consolidation.md).
