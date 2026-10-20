# iiSocietyHelper::Generation

[Windows 빌드 및 실행](WindowsBuildAndRun.md)

Part of iiSocietyHelper **0.10.0**. Remote belongs to the default Core target;
desktop Host requires `COMPONENTS GenerationHost` and
`iiSocietyHelper::GenerationHost`. Public headers are
`iiSocietyHelper/Generation/Remote.h` and `iiSocietyHelper/Generation/Host.h`.
Build from the Helper repository root; see [Consolidation.md](../Consolidation.md).


C++23 is the SDK that runs the model on Society's authenticated host. `Remote` connects to `iiSocietyHelper::Client`'s `requestHost` / `hostResponse`, and `Host` connects to Society's existing LAN TLS or account authentication relay request handler. No separate port or URL authentication repository is created.

`submit → status → read` does not transmit the model. The host verifies the original container and model reference and executes iiLocalDiffusion from the FIFO work queue. Results are stored in Society's device-specific runtime area and are published to Generation History after the client verifies SHA-256 and image size. Device-to-device replication of result files after generation is performed via Society synchronization.

Progress responses are delivered together with `phase` and the `step/total` of the corresponding step. Since model preparation/loading and image generation steps are distinguished, the client's background progress indicator does not mistake the loading value for denoising progress.

The host's default executor uses `generateNativeImageWithResidentWeights`. When a model file starts execution from the queue, its entire contents are loaded into anonymous process memory in a single sequential read; subsequent tensor loading uses that in-memory copy. No derived Q8 disk cache is created. Unsupported formats or failures in loading into memory return a job failure instead of falling back to disk streaming. The OS may compress or swap anonymous pages according to its own policies, but Society does not specify the swap location. If the original exists only on an external disk, the initial load must read it. Therefore, the “no external-disk use” guarantee applies to model-tensor reads and the Q8 cache after the initial transfer; it does not eliminate the initial copy from the external original into RAM.

The client requests model and generation resource downloads to StorageMap after `accepted`. This request is independent of generation and remains as Society's persistent download request even in case of failure, cancellation, or app switching. Download completion is not a condition for receiving remote results. Subsequent tasks for a fully downloaded model can use local inference.

The caller must check authentication and host role. Host additionally checks the job owning device, UUID reuse, changed model reference, path deviation, request size, queue size, and result chunk range. Retransmission of the same UUID does not cause duplicate execution, and cancellation status is maintained even if cancellation arrives before submission. After connection disconnection, jobs of the same process are re-queried. Upon host process restart, the memory queue is not recovered and an explicit job-not-found error is returned.

Host output is limited to 64 MiB, concurrent pending jobs are 64, and session records are 4096. The mobile package builds only Remote. iOS Network transmission in the background follows OS execution permission, and already accepted host inference continues even if the app is suspended. Model download continues from an executable Society session.

Build at `build/` and host pre-execution at `iiSocietyHelper.Generation.Protocol`, and verify reconnection, duplicate submission, cancellation, permission boundary, and result validation. The test injection engine provides transmission and scheduling evidence and does not substitute for actual model quality validation.
