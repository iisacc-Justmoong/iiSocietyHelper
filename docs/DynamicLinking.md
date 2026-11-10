# Dynamic linking

The helper facade, monitoring interface and optional desktop generation host
are shared targets. iOS uses frameworks and WebAssembly uses side modules for
the mobile-capable targets. The Swift crypto archive is an internal implementation
of the helper framework, not a separately distributed SDK library. The desktop
generation interface exports its QObject type for Windows consumers.
The Qt facade uses the file provider's own owner-permission flags when
restricting newly created observation directories.
When both installed Native and Qt provider packages expose the Qt file-device
header, configuration rejects different copies. Mixing their incompatible
class layouts with a shared library can otherwise corrupt objects at runtime.
The configuration also compares a selected native provider with any same-named
provider bundled beside an imported Container, Canvas, WebServer, or generation
backend. A different binary is rejected before linking. On native macOS builds,
the imported libraries are also inspected for a different Qt adapter ABI name.
Rebuild those SDK
packages against the same provider and use one coherent package set; do not
repair the error by changing a library search order or replacing a running
installation. The shared-link check rejects unresolved libraries, different
same-named binaries, and, on macOS, an additional incompatible Qt provider ABI.
It also verifies that the binary dependency graph resolves to the selected
native and Qt providers.

Provider class layouts can change between SDK generations even when the C++
names match. Rebuild dependent Qt libraries with the selected provider headers
and binary. The existing separately installed inference backend can be staged
without its stale provider copy when its native provider interface is compatible;
this does not update an existing system installation or prove model inference.
The generation protocol suite uses its original 45-second limit and an injected
test engine; it checks the protocol without launching a product app or model.

The Emscripten package configuration enables CMake shared-library imports for side modules. The builder or application must supply the main-module compile and link options.
