# iiSocietyHelper: Windows build and execution

The 0.10.0 package includes account/client access, local IPC, synchronization,
remote generation, native monitoring and optional desktop generation hosting.
No separate Generation, Monitor or Sync SDK is configured or linked.

## Dependencies and components

Build the lower-level iiSDKCore, iiFileProvider (including CoreIO and Temporary),
iiSocietyContainer 0.16.1, iiServerHost 0.6.0, iiSharedCanvas Core 0.31.4 EXACT and
iiAcountManager 0.2.8 packages for the same architecture and compiler first.
The default Core facade uses Qt 6.8.3 EXACT Core/Gui/Sql/Network, optional Bluetooth,
OpenSSL Crypto and DNS-SD. Native Monitoring uses only Core, CoreIO and FileTree.
Desktop GenerationHost additionally requires iiLocalDiffusion and Qt Concurrent.

Windows uses native file handles and reparse-point validation in Sync, native
file locks in Monitoring and the existing Windows account/discovery adapters.
Use the SDK [Windows environment and validation tools](../../Windows/README.md)
for matching MSVC/Qt/vcpkg prefixes and the DNS-SD overlay. Do not mix MSVC and
MinGW packages. These commands are run from the Helper repository root in the
prepared developer shell:

```powershell
$ErrorActionPreference = 'Stop'
$prefix = (Join-Path $env:USERPROFILE '.local/SDK/iiSocietyHelper')
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH" `
  "-DCMAKE_INSTALL_PREFIX=$prefix" -DBUILD_TESTING=ON `
  -DIISOCIETYHELPER_BUILD_GENERATION_HOST=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
ctest --test-dir build --output-on-failure --parallel 1 --no-tests=error
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
cmake --install build
if ($LASTEXITCODE -ne 0) { throw 'Install failed' }
cmake -S tests/consumer -B build/consumer/build -G Ninja `
  "-DiiSocietyHelper_DIR=$prefix/lib/cmake/iiSocietyHelper" `
  "-DCMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Consumer configure failed' }
cmake --build build/consumer/build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Consumer build failed' }
ctest --test-dir build/consumer/build --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw 'Installed consumer failed' }
```

When iiLocalDiffusion is unavailable, explicitly set
`IISOCIETYHELPER_BUILD_GENERATION_HOST=OFF`; Core Remote still builds.
For native-only monitoring use `IISOCIETYHELPER_BUILD_CORE=OFF` and
`CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON`, then configure `tests/native-consumer` instead
of `tests/consumer`. Its package lookup explicitly requests Monitoring.
All build directories are named `build`; use a separate workspace evidence
parent when verifying a second configuration.

## Runtime and verification limits

Ensure the selected Helper `bin`, native dependency DLLs and Qt plugins are on
the process search paths. Core uses QSQLITE. SocietyContainerDaemon and
`ii-society-helper` are plain executables, never nested app bundles. The daemon
reports the Helper version. Sync reparse-point fixtures require Windows Developer
Mode or existing symbolic-link creation rights; a shortcut is not a symlink test.
Do not run fixture tests on an existing user container.

The retained pre-consolidation Windows results are in the SDK's
[2026-10-20 verification record](../../Windows/Verification-20261020.md).
Those results cover the former packages and do not establish Windows execution
of this merged release. Current consolidation evidence is recorded separately;
macOS build/runtime and source-level Windows graph checks cannot establish a
native Windows or physical mobile run. Protocol, storage and credential formats
are preserved; no live container migration or application restart is performed.

See [component/API migration](Consolidation.md),
[Sync contracts](Sync/README.md), [native monitor protocol](Monitoring/Protocol.md)
and [generation protocol](Generation/README.md).
