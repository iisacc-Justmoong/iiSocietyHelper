# Backend dependency boundary

iiSocietyHelper consumes `iiSharedCanvas::Core`, not the complete presentation
facade. Canvas persistence, previews and media import/export keep their existing
API and behavior. They no longer require the Qt Quick items or QML library.

The current Helper event-loop, account, network and delivery adapters still use
Qt Core, Network and Sql; media codecs still use Qt Gui. These are explicit
remaining dependencies. The native container monitor and its FileTree storage
dependency can be built and consumed without Qt.

Version 0.10.2 exports Core (the default), Monitoring and optional desktop
GenerationHost from the same package. Core includes Sync and Generation::Remote.
GenerationHost depends on Core; Monitoring is independent of Core. See
[Consolidation.md](Consolidation.md) for targets, options and native-only usage.
A monitoring-only installation requires `COMPONENTS Monitoring`; default package
discovery continues to require Core and fails clearly if Core is unavailable.
# Native provider identity

The exported package resolves the Native provider version used by this build
before CoreIO, whose lower minimum can otherwise import an older binary first.
This ordering does not add Qt to Monitoring. A sibling provider installation is
preferred when present. Imported Native binaries must belong to the selected
Native package; an incompatible pre-existing target is rejected during package
configuration rather than failing later in the dynamic loader.

The installed consumer checks the actual imported binary location against its
package prefix as well as the transitive Canvas version. A correct `*_DIR` cache
value alone is insufficient: a later package search may update that value while
an older imported target remains defined.
