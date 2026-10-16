# Client and Helper consolidation in 0.9.0

`iiSocietyHelper` is the single application-facing SDK for Society storage,
accounts, discovery, pairing and same-device collaboration. The former
`iiSocietyClient` sources, native adapters and regression tests are part of this
library. There is no separate Client package or Client library to build or link.

```cmake
find_package(iiSocietyHelper 0.9.0 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE iiSocietyHelper::iiSocietyHelper)
```

Replace `#include <iiSocietyClient/Client.h>` with
`#include <iiSocietyHelper/Client.h>` and `iiSocietyClient::Client` with
`iiSocietyHelper::Client`. The other former Client headers retain their basenames
under `iiSocietyHelper/`, including `AccountSession.h`, `DiscoveryService.h`,
`NearbyDevices.h`, `AutomaticPairing.h`, `GroupSessionStore.h` and
`PairingCredentials.h`. Existing global account and discovery type names remain
unchanged. `<iiSocietyHelper.h>` includes the local Helper, FileSystem, Client
and AccountSession entry points. Recompile consumers; old Client object files
and libraries must not be linked alongside the consolidated library.

The SDK keeps two storage responsibilities explicit:

| API | Responsibility |
| --- | --- |
| `Helper` / `DeliveryStore` | Same-device presence and durable local IPC. |
| `Helper::fileSystem()` / `FileSystem` | Selected native drive, Alias snapshots, canvas persistence, model access and media publication. |
| `Client` | Authenticated host access, read-only container metadata and replica sessions. |
| `AccountSession` | Account restoration, scoped pairing credentials and host reconnection state. |

Create the objects on the application event-loop thread. A local Helper can
reference the same account manager as the host client:

```cpp
#include <iiSocietyHelper.h>

AccountSession account;
iiSocietyHelper::Helper helper;
helper.setAccountManager(account.manager());
iiSocietyHelper::Client host(&account, nullptr);
// After account authentication and registered host selection:
host.connectHost();
// Consume host.hostDriveResponse and host.container() as described in HostDrive.md.
```

`Helper::stop()` ends local presence; `Client::stop()` ends host access and
clears its remote mirror. Neither operation signs out the borrowed account.
The two container objects have independent ownership: local FileSystem aliases
remain native-path snapshots, while Client aliases describe the authenticated
host. Network credentials and filesystem-private state never enter local IPC.
The selected container's encrypted state locations and keychain services do not
change during this source consolidation.

Dependency direction is `Helper -> Sync -> Container/ServerHost` and
`Helper -> AccountManager/SharedCanvas/Monitor`. Sync, Container and account
packages never depend on Helper. Local presence and delivery remain usable
without starting network discovery or signing in. The package resolves Sync at
configure time; it no longer promises independence from that package.

Desktop builds keep the existing shared Helper library; iOS keeps a native
static library and its private CryptoKit bridge. Android retains its native
discovery adapter and existing OpenSSL runtime setting. These are native C++ and
platform implementations with the existing Qt dependency. Consolidation does
not introduce a VM, interpreter, new transport or credential format.

`tests/merged_api.cpp` checks both public surfaces through one target.
The original local delivery, object, filesystem, observation and account tests
run alongside the imported account, state, discovery and host-drive session
tests. `tests/consumer` rebuilds against the installed package and checks that
no Client package is imported and Sync does not acquire a reverse dependency.
`SDK/tests/SocietyHelperConsumers` also compiles Society's real account wrapper
and encrypted-state tests and Dreamscapes' real generation controller against
the installed SDK, without creating app bundles or changing product build caches.
Platform runtime verification is recorded separately from source support.
