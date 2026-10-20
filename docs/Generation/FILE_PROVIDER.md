# File creation ownership in iiSocietyHelper::Generation

All application and SDK file factories delegate creation to iiFileProvider. The
domain keeps format validation, serialization, permissions and error reporting.
Native C++ callers use `iiFileProvider::Native` (C++23, no Qt); existing Qt adapters
use `FileDevice`, `SaveFile`, `TemporaryFile`, `TemporaryDirectory`, `Directory`
and `LockFile`. Their stream/append/exclusive-create semantics are retained.
Complete encoders can use `File::writeWith` or `native::StagedFile`; exceptions
must leave an existing destination intact. Native descriptors retain caller
confinement, no-follow flags and mode bits.

Python AI workers load the provider's native C ABI through `ii_file_provider`;
PNG, JSON, video frames, model archives/checkpoints and PCM/WAV output use owned
streams or borrowed staging paths. Swift builds compile the provider source from
`iiFileProviderSwift`; Android packages merge the common `FileStorage` OS bridge.
An encoder's format logic remains outside the storage owner.

Provider control files (including `.iisacc-temporary.lock`) are not payloads.
Python directory scans use `provider.iterdir`/`rglob`. Lease sentinels remain
persistent for interprocess coordination. Portable `.iisacc.json` companions are
file metadata and remain transferable. User-visible results can be registered in
`FileCatalog`; internal logs and databases do not gain collections implicitly.

The workspace gate is `SDK/iiFileProvider/tools/audit_file_creation.py --check`.
It covers repository-owned runtime factories, including deprecated product source
and AI workers. Compiler/installer artifacts, tests and vendored engines are
reported separately. External codecs receive owned streams, paths or private
directories; the gate is not a sandbox for arbitrary shell commands.

Changed boundaries in this module:

- `src/iiSocietyHelper/Generation/Host.cpp`

Protocol fixtures use actual `.societycontainer` directory packages and preserve
strict cancellation, job ownership, retry, and corrupt-output checks.
