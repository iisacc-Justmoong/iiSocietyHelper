# Backend dependency boundary

iiSocietyHelper consumes `iiSharedCanvas::Core`, not the complete presentation
facade. Canvas persistence, previews and media import/export keep their existing
API and behavior. They no longer require the Qt Quick items or QML library.

The current Helper event-loop, account, network and delivery adapters still use
Qt Core, Network and Sql; media codecs still use Qt Gui. These are explicit
remaining dependencies. The native container monitor and its FileTree storage
dependency can be built and consumed without Qt.

Version 0.10.1 exports Core (the default), Monitoring and optional desktop
GenerationHost from the same package. Core includes Sync and Generation::Remote.
GenerationHost depends on Core; Monitoring is independent of Core. See
[Consolidation.md](Consolidation.md) for targets, options and native-only usage.
A monitoring-only installation requires `COMPONENTS Monitoring`; default package
discovery continues to require Core and fails clearly if Core is unavailable.
