# Canvas presentation through the file owner

## Rich container information

`FileSystem::fileInformation(sectionKey, relativePath)` exposes the selected
container's provider-owned `iiFileProvider.FileInformation` schema-1 document.
The result contains real details, a bounded embedded preview and the validated
published classification: title, description, keywords, rating, color label,
custom fields and collection names/keys. `classification.status` distinguishes
`ready`, `absent` and `invalid`. This is a blocking QObject-thread API; GUI code
should use a copied `SharedStorage` on its I/O worker for local selection, or the
asynchronous Client API for a host. Filesystem aliases and entries carry an
`informationKey`; original contents are not fetched while merely listing them.

`Client::inspectHostFile(logicalPath)` works through the same authenticated host
as direct file reads. It returns a request ID immediately and emits one
`hostDriveResponse` with `information` when complete. It fetches only the bounded
information document, assembles version-pinned 256-KiB chunks, verifies the
whole-document SHA-256 and rejects invalid sizes, offsets, revisions or
container/path mismatches. Inspection is limited to eight concurrent requests,
3 MiB per document and a 30-second overall deadline. Disconnect cancels pending
results. Remote aliases advertise `informationAvailable` and `informationKey`;
their original `source` remains empty until separately requested.

The container [file-information contract](../../iiSocietyContainer/docs/FilePresentation.md)
defines details and explicit failure states. The native implementation and
provider ownership remain below the Helper/Container application adapters.

## Canvas integration

FileSystem installs a read-only `.iisc` renderer in iiFileProvider 0.7 through
`Presentation::setRenderer`. The adapter uses the existing iiSharedCanvas
`readIiscPreview` compositor and PNG encoder, fits frame 0 within 512 pixels and
uses the provider's 32 MiB source admission budget and a 16-Mpixel total decoded
raster budget. Native-size projections retain exact mask coverage. Snapshot and working-file
decoding remain owned by iiSharedCanvas; the provider has no upward SDK link.

The common owner stores PNG bytes and source-bound file details in native
attributes plus a portable hidden companion. Existing information cards can be
upgraded once the domain renderer is available. Canvas saves notify the owner
after the working file closes; IISC WAL changes are part of the native source
version. Renderer retirement waits for in-flight work before Qt codec teardown.

FileSystem's existing canvas preview API keeps returning a private runtime PNG
URL. It now materializes the provider's bounded embedded PNG instead of rendering
an unbounded full-resolution canvas for that cache. Annotation documents and
original source bytes are retained. Generated image history remains PNG and
editor persistence remains IISC.
User file listings exclude provider presentation, catalog, staging and lock
documents while preserving ordinary public hidden files.

See the shared [presentation contract](../../iiFileProvider/docs/FILE_PRESENTATION.md).
Regression covers actual composed canvas pixels, a changed embedded preview after
editing, strict save-version conflicts and existing Helper filesystem behavior.
