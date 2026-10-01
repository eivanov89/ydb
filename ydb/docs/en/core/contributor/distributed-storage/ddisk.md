# DDisk

DDisk provides block-addressed storage for direct block clients. A request identifies a tablet, a virtual chunk, and a byte range. DDisk manages the mapping to local PDisk chunks, integrity metadata, and I/O completion. Group replication and user-visible quorum are chosen by the client.

DDisk shares PDisk and slot-management infrastructure with VDisk, but implements a different interface. VDisk stores blob parts for the DS proxy; DDisk serves the events in [ddisk.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk.h) and [blobstorage_ddisk.proto](https://github.com/ydb-platform/ydb/blob/main/ydb/core/protos/blobstorage_ddisk.proto).

## Ownership and Startup

[NodeWarden](node-warden.md) creates a DDisk actor for a slot configured as DDisk. The actor initializes its PDisk owner, restores chunk-map snapshots and log increments, restores integrity mappings, reconciles orphan reservations, and creates its [PersistentBuffer](persistent-buffer.md) child. The child has its own service ID and event handlers but shares the parent's PDisk ownership and PB resource lifecycle.

Unless `ForcePDiskFallback` is set, DDisk asks for a submit-only io_uring client in `TEvYardInit` and passes its configured `IdleSpinUs`. On the first such request, PDisk duplicates its device handle and creates and starts one `TUringRouter` shared by the DDisk slots and their PB children. The first requester therefore selects `IdleSpinUs` for the shared router for that PDisk incarnation; later requesters use the existing setting. DDisk and PB hold shared `IUringRouterClient` references and cannot control the router lifecycle.

`ForcePDiskFallback` opts out of the shared router and selects the PDisk raw-event path. An unavailable device handle, unsupported platform, or failed io_uring probe also falls back. In that path, `TEvChunkReadRaw` and `TEvChunkWriteRaw` carry PDisk owner and owner round. Logging and chunk management continue to use PDisk services with either data-I/O backend.

`TDDiskConfig::DevNullMode` (default `false`) changes the shared router's data-I/O semantics for disposable tests: accepted writes complete without changing the device, and reads return zero-filled buffers. It requires a working io_uring router, cannot be combined with `ForcePDiskFallback`, and fails initialization if the router is unavailable. All DDisk slots attaching to the shared router on one PDisk must request the same mode; a conflicting slot is rejected, and changing mode requires a PDisk restart. The NBS flat `DevNullMode` field overrides `GlobalDDiskConfig.DevNullMode` when explicitly set, including `false`. The stress tool exposes this mode as `--ddisk-devnull`.

PDisk formatting, chunk management, and mapping logs still perform real I/O. DevNull data and integrity images are not persistent: after metadata eviction or restart, zero-filled integrity reads do not constitute valid checksum metadata. Checksummed DevNull benchmarks must first write zero-valued used data and keep its checksum metadata resident. Set `IntegrityChecksumCacheBytes` large enough to retain all used integrity pairs in the working set; zero disables caching. Nonzero writes cannot be read back successfully with checksum verification because data reads return zeroes. Use normal device I/O for cold-cache, recovery, and persistence validation.

## Sessions {#sessions}

A client establishes a session with `TEvConnect` for each DDisk or PB recipient. The connection metadata includes tablet ID, tablet generation, direct block group index, and the recipient kind. DDisk sessions additionally use `DDiskSessionSeqNo`; PB sessions do not use that sequence number to distinguish sessions.

`TEvConnectResult` returns the instance GUID and an opaque `TConnectionToken`. Normal requests carry the token, and the receiver resolves it to server-side connection metadata. Clients should use the token constructors in `TQueryCredentials` rather than synthesizing the token's fields or serializing the initial metadata on every request.

Connections are keyed by `(TabletId, DirectBlockGroupIndex)`. An older generation, or an older DDisk session sequence within the same generation, cannot supersede an active newer session: such a connect returns `BLOCKED`. An ordinary request with stale or invalid session credentials returns `SESSION_MISMATCH`. The instance GUID is generated for each actor incarnation, not only after detected data loss. Connecting, disconnecting, replacing a session, and restarting the service affect token validity. A client must restore a valid connection before retrying operations under its retry policy.

Read, write, and Sync validation resolve credentials into native `TQueryCredentials` without rewriting the request protobuf. They retain the original credentials across allocation or metadata waits and revalidate before using a destination chunk. Disconnecting and reconnecting invalidates an old parked token even when generation and session numbers are unchanged. A pending write reports `SESSION_MISMATCH`; Sync reports it for the affected input and returns an aggregate failure. Read validation describes a recognized old token as “stale” and an unrecognized token as “invalid”. Repeating connect without changing the token remains valid.

Internal DDisk/PB forwarding uses `TQueryCredentials::ForInternal`. This has different validation from an ordinary client request, including support for a peer without an existing client connection. It is a server-to-server mechanism, not a replacement for client session establishment.

## Addressing and Writes

`TBlockSelector` contains `VChunkIndex`, `OffsetInBytes`, and `Size`. Data chunk mappings are keyed by `(TabletId, VChunkIndex)`. The direct block group index separates sessions and PB namespaces; it does not add another dimension to the DDisk data chunk map. Clients sharing a DDisk under one tablet must therefore assign virtual chunk indexes consistently across their DBGs.

Reads and writes operate on nonempty ranges aligned to the 4 KiB integrity unit and contained within a PDisk chunk. Write payloads may be fragmented or have unaligned buffer addresses: direct I/O preparation copies them into aligned storage when needed, while suitably aligned rope chunks can use scatter/gather I/O. Use the event payload helpers; the payload ID is an event-local reference, not a persistent data identifier.

The `interface/UnalignedWritePayloads` counter counts incoming writes with fragmented payloads or buffer addresses not aligned to the device sector size. Each request is counted once, including when chunk allocation delays its execution.

The first write to a virtual chunk can wait while data and integrity resources are allocated. Concurrent requests for that virtual chunk share one allocation. With checksums enabled, a write acknowledgment waits for the data write, integrity update, and durable allocation mapping. The write handler releases its incoming event and awaits one ordinary actor-owned aggregate. Disjoint writes and Sync segments in the same integrity pair may submit data concurrently; the integrity manager persists each mutation's required pair version.

Allocation, formatting, metadata preparation, data completion, and mapping commit advance through actor-owned records. A chunk pin covers accepted work until its physical results and metadata dependencies retire. The actor revalidates the original client credentials before destination submission after any wait.

DDisk coroutine frames use the actor runtime's shared [TLS allocator](../actor-system/coroutine-actors.md), without a private actor-owned cache or allocator override. Cache occupancy statistics measure idle frames retained by executor threads, not live DDisk frames; request registries, chunk pins, and completion drain determine whether an actor's work has retired.

For an allocated, formatted chunk, the read handler uses one top-level coroutine and one ordinary awaiter: `auto result = co_await ReadDDisk(...); FinishDDiskRead(context, result);`. Preparation, metadata loading, completion aggregation, and result processing create no child coroutines. `TDDiskReadAwaiter` has three modes: **Ready** for immediate errors or validated zeroes, **DataEvent** for checksums disabled or immediately available metadata, and **Cold** for pending metadata dependencies. Only Cold allocates aggregate state and enters the pending-read registry. DataEvent reuses the pooled operation and an actor-owned indexed completion slot, without registering a generic event waiter or allocating a global wait cookie.

Each indexed slot holds a generation, an attached awaiter, and a chunk pin. The token `(generation << 32) | (index + 1)` survives registry vector growth; zero is invalid. Released slots are reused through a free list, but a slot is permanently retired before its generation can wrap. `TEvIndexedReadResult` carries the token, status, error, trace span, and owned payload, using envelope cookie zero; generic I/O waits retain their nonzero cookies. On the actor thread, completion validates the token, transfers the result, detaches the awaiter, and releases the slot before resuming the continuation inline. Duplicate or stale tokens cannot complete a newer read. Cancellation or awaiter destruction detaches the continuation but retains submitted slots and their chunk pins until terminal result processing. An abandoned reservation that has not been submitted releases immediately. Router rejection and fallback cancellation use the same terminal-result path.

Before I/O submission, a read stores its requester, interconnect session, client cookie, tablet/chunk identity, and start time in an inline coroutine reply context and releases the incoming event. Pooled indexed operations do not copy this routing context. The awaiter or cold context owns a plain inline `TDDiskReadResult` and returns it by value; read finalization does not allocate a generic `TEvDDiskIoResult`. The operation and completion event retain the trace span until terminal processing, including cancellation. A cold data-part completion updates status, error, and payload while retaining the context's span.

A logical read submits one ordinary scalar router operation for its data range and each newly claimed metadata part. DDisk owns their shared completion object, whose stable per-part buffers and result slots are allocated before submission. Its atomic pending count starts at the number of parts plus one submission guard. Each callback writes its own result slot, releases its child operation, and decrements the counter with acquire/release ordering. Only the transition to zero aggregates all results and publishes one completion through `TActorSystem`. Releasing the submission guard uses the same path, so callbacks may complete in any order, including before a submission call returns. The router handles queue pressure and short reads separately for each accepted scalar operation.

If a submission is rejected, DDisk enters Stopping, stops submitting further parts, and completes the rejected and unsubmitted slots locally. The aggregate still waits for every accepted sibling, including error and drop callbacks. PDisk fallback sends one raw-read message per part and likewise aggregates every result before completing the logical read. The read also joins metadata loads already owned by another operation; it can finish its own I/O while still waiting for a shared load. Detached cold contexts and chunk pins remain alive until both I/O and shared metadata settle; completion handlers retain a local shared context across callbacks. Accepted buffers and logical-operation state remain owned through callback retirement and actor-side terminal-result processing.

`TReadPayload` owns either no data, a native `TRcBuf`, or a fallback `TRope`. Indexed and aggregated completions move this ownership, keeping native buffers native until reply preparation. `FinishDDiskRead` zeroes holes in mixed ranges through the payload helper, then converts native client data to rope once before optional payload checksum validation and attachment. This transfers successful data without copying the payload. Broken-state handling, status mapping, byte accounting, short-I/O counters, and tracing still apply; error replies carry neither payload nor checksums. `TEvReadResult` consumes a `TConstArrayRef<ui64>` immediately into protobuf, with no retained view or intermediate checksum-vector copy.

The read contract requires that the requested data blocks are not written concurrently. Writing or syncing neighboring blocks in the same metadata pair is allowed. Reads capture checksums and the used-block mask together as an immutable snapshot, which remains valid across neighboring mutations and cache eviction. Available snapshots do not wait for a neighboring metadata flush. If already submitted data turns out to cover only never-written blocks, the read still consumes its data completion before returning zeroes.

An unallocated virtual chunk reads as zeroes. Chunk allocation and restored integrity state affect how the implementation recognizes never-written blocks within an allocated chunk; do not treat a successful read as evidence that the range has previously been written.

## Integrity

Wire checksums are unsalted XXH3-64 values, one per 4 KiB payload block. When `TDDiskConfig::EnableChecksums` is enabled, write handlers reject missing, incorrectly counted, or mismatching checksums before allocating resources or issuing I/O.

DDisk stores integrity metadata separately from data. Stored checksums are sealed with logical and physical identity information, while the wire protocol and checksum cache use the pure payload checksum. Each integrity metadata block uses a pair of slots, self-checksums, identity/generation checks, and a sequence number to select a valid durable version after recovery. The implementation supports layouts for different device atomic-write properties; their exact format belongs to [ddisk_checksums.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_checksums.h).

`TIntegrityManager::PrepareRead` is an ordinary actor-local operation. Its caller supplies an empty `std::optional<TOperationResult>`: immediate results are constructed there, while cold preparation leaves it empty and returns a pending handle plus descriptors for newly claimed pair loads. Warm reads move response-owned fields from this result; cold reads copy only response fields and inspect the shared immutable read plan by const reference, preserving repeated waits. An owned `TReadChecksums` stores an empty value, one inline `ui64`, or a vector. Empty and singleton inputs are normalized; larger vectors move without reallocating, and copies own independent storage. This representation covers metadata/read results and unallocated-chunk zero replies; cached arrays and write, PB, and sync inputs keep their existing representations. After preparation and error checks, single-block collection resolves its pair once and stores the checksum inline, without constructing a mask: a known unused block selects `AllZero`, while a used block or unknown bitmap selects `Passthrough`. Multi-block collection uses one reserved vector and collects checksums, the used-block count, and the mixed-range mask in a single pass by metadata pair using the already resolved extent. Warm collection touches each relevant pair once for LRU accounting and evicts only after constructing the snapshot; cold collection retains preparation/load touches. Missing cached state is valid for holes, and unknown bitmaps preserve passthrough behavior. Pair errors retain precedence, and a missing-checksum error is published only after the read plan is complete; checksums are published only on success. It pins the complete metadata range before claiming loads and joins existing loads without duplication. Preparation submits no I/O; the read combines its data range and the claimed pairs in one logical operation with separate scalar submissions:

| Metadata state | Parts submitted by this read |
| --- | --- |
| Cached, used or mixed range | Data |
| Cached, all-zero range | None |
| Cold, known all-zero range | Newly claimed metadata pairs |
| Cold, unknown, used or mixed range | Data and newly claimed metadata pairs |

`CompletePairReads` validates image sizes and copies metadata into the existing local integrity-block array, using `memcpy` for native buffers or rope extraction for fallback. It validates and publishes all completed pair images before notifying dependent readers and writers on the actor thread. Pair slot selection, identity and digest checks, and lost-write detection also apply after cache eviction. Shared load records have their own lifetime: detaching a canceled reader does not cancel the loads. Stopping resolves logical read handles with failure while retaining their pins until shared loads retire; the Cold awaiter still waits for that retirement before finishing. Final metadata results and snapshots are owned by their handles.

Integrity mutations start eagerly and return owning result handles. Mutation processing and pair flushing use ordinary actor-thread state transitions, without launching coroutines for either warm or cold metadata. Writers share existing pair loads, then apply each complete mutation in one turn and record its required version for every affected pair. Each pair has at most one submitted immutable image; newer mutations update cached state and coalesce into the next image. Successful completion advances the durable version and alternating slot. A mutation succeeds only after all its required versions are durable. Allocation, header and extent formatting, and background reclamation also use ordinary actor-owned records.

Pending loads and pair writes retain physical ownership through completion even if the caller detaches; failed loads still join accepted siblings before releasing operation pins. Pair-write completions continue during Stopping. Reservation release and Gone additionally wait for the pair-write cookie registry to become empty, so callback drain alone cannot overtake an unprocessed completion.

Data errors and critical metadata errors retain separate results. Metadata overload errors retain the existing retry limits and delays; only overloaded metadata parts are resubmitted, preserving successful data and metadata buffers. These error retries require additional scalar router admissions. Completion processing continues during Stopping, and accepted work retains buffer and chunk ownership through retirement.

New extent placement and readiness are separate milestones. Placement permits data writes while formatting continues. Readiness requires both extent formatting and all three integrity-chunk headers, and gates submission of the mapping log. Deleted extents retain their slots until deletion is durable and outstanding formatting has retired.

Changing checksum modes is a format and recovery concern, not only a performance setting. DDisk validates checksum-mode compatibility against restored state. PB has a separate on-disk checksum setting, described in [{#T}](persistent-buffer.md#integrity).

## Synchronization

`TEvSync` is the unified pull-and-write operation used for both PB-to-DDisk flush and DDisk-to-DDisk repair. It contains source identities and segments; each segment selects either a PB record or a DDisk source range. The destination issues reads to those services, validates the returned data, and writes the destination range.

All destination segments in one request must belong to one virtual chunk. The sync handler checks nonempty aligned ranges, one segment kind per input, and chunk bounds before sending any source read. Producers must exclude overlapping physical write/write and read/write ranges, including across clients that share a DDisk. DDisk does not serialize entire extents or supersede overlapping Sync requests. The wire `OUTDATED` status remains available for source-service results; DDisk treats such a source reply as a failed input.

The Sync handler releases its incoming event and awaits one actor-owned aggregate. The actor registers and sends all source reads at once, and starts metadata preparation for existing destination ranges independently of source replies. Each fully valid reply can start its destination write while sibling sources remain pending. A missing destination is allocated only after a source reply has a successful status, present payload of exact size, and valid checksum count and hash under the configured policy. Each destination retains its complete source payload until its accepted work finishes. The aggregate joins accepted destination data, metadata, and mapping durability, then replies with per-input results in original request order.

Source and destination cookies are registered before submission and retired before completion notifications; stale or wrong-kind replies cannot finish a different input. All source reads are sent at admission. Stopping abandons pending source results and prevents unsubmitted destination writes while retaining accepted destination work and its buffers until terminal results are processed. Detaching the request awaiter does not release those actor-owned records or pins.

The operation reports `TEvSyncResult` after processing its destination work. It does not erase source PB records. The client decides when enough replicas have been flushed, whether repair is required, and when [PB erase](persistent-buffer.md#erase) is safe. A PB source can be remote even when it occupies the same logical DBG index as the destination DDisk.

## Failure and Recovery

Chunk-map snapshots and PDisk log increments restore ownership and integrity-extent mappings. After successful, complete log replay, DDisk computes orphan candidates as `OwnedChunksOnBoot` minus the union of restored data chunks, listed integrity chunks, every integrity chunk referenced by a restored extent, and PB chunks. Extent references protect chunks even when they are absent from the integrity-chunk list. DDisk computes this union before boot-time reclamation changes the mappings, so references recovered from both snapshots and later log increments are protected. This conservative protection does not make an otherwise invalid recovery mapping valid; existing recovery validation still applies. Failed or incomplete recovery does not attempt reconciliation.

DDisk submits one orphan at a time through `TEvChunkForget`, tracks delivery, and waits for its reply before continuing. PB creation, new allocations, and client readiness remain blocked until reconciliation completes. DDisk explicitly sets `IsDDisk = true` on its reserve and forget requests; the field defaults to `false` for other callers. For flagged forget requests, PDisk preserves the request cookie in all replies and validates the owner and owner round when executing the request, so a delayed request from an older incarnation cannot release reused chunks. Existing VDisk behavior, including error responses, cookie handling, and logging severity, is unchanged.

Successful cleanup and chunk-validation rejections both allow reconciliation to continue. DDisk preserves rejected chunks and logs their IDs and rejection reasons at WARN. For flagged requests, PDisk also logs BPD91 at WARN for expected transitional states: `DATA_ON_QUARANTINE`, `DATA_RESERVED_DELETE_IN_PROGRESS`, `DATA_COMMITTED_DELETE_IN_PROGRESS`, `DATA_RESERVED_DELETE_ON_QUARANTINE`, and `DATA_COMMITTED_DELETE_ON_QUARANTINE`. PDisk's existing I/O and log completion reclaim these chunks; reconciliation adds no retry or forced deletion. Rejection of a committed orphan (`DATA_COMMITTED`) remains an ERROR in PDisk, and other unexpected validation failures retain their existing severity. A PDisk session or device error, or request nondelivery, enters Stopping. Poison cancels remaining reconciliation, and a late reply cannot resume bootstrap. PDisk returns terminal `CORRUPTED` replies when shutdown aborts flagged queued reserve or forget requests, including requests in the dedicated forget queue. Unflagged queued requests are silently discarded as before.

PB then performs its own chunk scan and record recovery. Connection state must be re-established by clients after service replacement.

## Shutdown and Restart {#shutdown-and-restart}

DDisk and PB must always wait for their accepted asynchronous router I/O and
callbacks before acknowledging shutdown. Neither actor may publish Gone while
those callbacks still own its state. The PDisk fallback path instead cancels
actor-owned requests and processes their terminal results before Gone; the
PDisk stop barrier below drains the underlying device I/O.

PDisk session loss starts the same idempotent Stopping state as poison, but
only poison authorizes actor death and the shutdown acknowledgement. Stopping
rejects new requests with `SESSION_MISMATCH`, cancels actor-owned fallback I/O
and parked retries, and continues processing submitted I/O results. Cancellation
balances counters and publishes results before the final mailbox barrier.
Existing completions may finish writes only when their integrity and allocation
log durability conditions are satisfied; shutdown starts no further I/O.

Broken and Stopping wake logical waits that cannot progress, but do not cancel
accepted router-I/O waits. A failed request joins already-submitted sibling I/O
before replying; its buffers and physical chunk pins remain owned until terminal
results are consumed. Logical operations and outstanding physical producers
both protect chunks from deletion. Remaining logical waits and request replies
finish before the final mailbox barrier and Gone. Indexed read results are processed
in both running and Stopping states. Reservation release and shutdown completion
also require zero active indexed slots, including slots whose readers were canceled.
Forced destruction drains callbacks before releasing any remaining registry pins.

Chunk-map `TEvLog` requests track delivery; a nondelivery notification correlated
with an outstanding request by its cookie enters Stopping.

After its own I/O drain and terminal-result processing, DDisk requests release
of its known reservations through `TEvChunkForget`, provided PDisk initialization
and log replay have completed. Candidates include unused reserved chunks,
abandoned formatting and data allocations, and integrity chunks, but only when
their commit log has never been submitted. A submitted commit excludes a chunk
even if its acknowledgement is still pending; PB allocations are also excluded.
The request carries the current PDisk owner and owner round. Accepted router
I/O must have retired before release; PDisk quarantines chunks with remaining
fallback device I/O until that I/O finishes.

Broken retains abandoned formatting and unlogged data allocations separately
from unused reservations, so PB cannot reuse a chunk while an old DDisk write
may still target it. Fresh reservations that have not been used for DDisk I/O
remain available to PB. Successful reserve replies received during Stopping
are collected without starting allocation or formatting. After DDisk's own drain,
late replies can trigger further forget requests while the actor remains alive.
Each chunk is submitted for release at most once per actor incarnation, so
follow-up requests contain only new IDs, regardless of earlier forget replies.

An outstanding reserve request (`ChunkManager.IsReservationInFlight()`) prevents DDisk from publishing
Gone, even after its own drain and PB shutdown finish. Every terminal reserve
reply clears this barrier; successful replies contribute their chunks to the
release set, and release waits for the existing I/O barrier. Reserve delivery is
tracked: nondelivery clears the pending request and enters Stopping. PDisk returns
a terminal `CORRUPTED` reply when shutdown aborts a queued reserve request marked
`IsDDisk`. There is no
timeout that abandons an outstanding reservation.

Shutdown does not wait for forget replies and does not retry forget requests.
These acknowledgements remain best effort: lost or rejected forget requests can
leave reservations in a running PDisk across DDisk-only restarts. Startup
reconciliation repairs unreferenced reservations left by earlier incarnations,
subject to the validation and error handling above. A PDisk restart discards
never-committed reservations. This cleanup requires no new event types, durable
format, or log schema and does not change the protocol for deleting committed
chunks.

DDisk and PB drain their own I/O concurrently. PB releases its router reference
before sending `TEvGone` to its concrete parent actor. The parent waits for its
own drain, the concrete child's `TEvGone`, and resolution of its reserve request,
releases its router reference, then notifies Warden. Tracked child poison
handles an already absent PB; duplicate poison and notifications are harmless.
After 60 seconds each actor with outstanding callbacks contributes one to
`ddisks/io_stalled`, cleared as soon as its own drain finishes. Shutdown diagnostics
also report waiting for PB and an outstanding reserve request. Normal shutdown
waits indefinitely for stalled I/O or an unresolved reservation.

The callback retirement count and stopping flag share one atomic state. Callback
cleanup and result publication precede retirement; completion and retry
cancellation events finish before actor destruction. Forced actor destruction
retains all members while waiting up to 10 seconds using monotonic time, then
aborts if callbacks still own actor state. This forced-destruction deadline is
separate from the 60-second stalled-I/O diagnostic and does not bound normal
shutdown waits.

Critical integrity/formatting overload errors permit 20 resubmissions, delayed
by `min(1 ms × 2^(retry−1), 100 ms)`. The immediate callback event transfers the
operation to an actor-owned map; timers contain only IDs. Broken/Stopping cancels
parked operations and stale timers do nothing. Exhaustion returns `ERROR` with
attempt count and last errno; ordinary-I/O error mapping is unchanged.

PB restore consumes payloads only after successful reads. The first failed read
enters Broken and fails queued requests with `ERROR`; late completions only
retire accounting and cannot parse data, resume recovery or publish readiness.
The restore set continues to mean chunks already scheduled.

For a NodeWarden-requested PDisk restart, NodeWarden first requests shutdown of
all affected DDisk actor incarnations. Each DDisk requests shutdown of its PB
and waits for its own drain, the concrete child's `TEvGone`, and resolution of any
outstanding reserve request. Only after the last DDisk's Gone does NodeWarden
send PDisk restart permission. Replacement DDisk/PB startup remains fenced while waiting for these actors and while the
PDisk restart is in flight. PB drain/Gone, DDisk drain, and reserve resolution
must all precede DDisk Gone, which precedes PDisk restart permission. The drains
and reserve resolution may finish in any order.

A replacement PDisk cannot begin device I/O until the previous PDisk's I/O has
retired. `TPDisk::Stop()` always calls the shared router's `StopSync()`, even if
DDisk/PB or retained initialization results still hold router clients. This
closes admission, waits for publishers and terminal callbacks, joins the issuer,
retires the ring, and closes the router's duplicated device descriptor. PDisk
then stops its source block device before replacement bootstrap. An old client
can outlive this barrier, but it rejects new work and no longer owns an active
ring or device descriptor. Healthy draining has no timeout; failure to retire
kernel ownership on a broken ring aborts the process instead of permitting
replacement startup with old I/O still live.

The same synchronous I/O barrier applies when PDisk stops or restarts
independently of the NodeWarden-requested sequence. DDisk and PB observe the
stopped router on a rejected submission and enter Stopping; PDisk session loss
also enters Stopping. There is no unsolicited router notification to an idle
actor. PDisk waits for accepted router operations and their callbacks to retire,
which protects actor state while those callbacks run. This barrier does not wait
for the actors' final mailbox processing or Gone notifications. Those actors
still drain their results, and poison remains required before actor death and
notification to Warden. Do not equate this independent I/O barrier with the
additional actor-Gone ordering of a requested restart.

`TEvDeleteTabletChunks` retires a tablet's data chunk mappings. While deletion is in flight, writes and sync requests for that tablet can return `BUSY`. Controller claim removal and local chunk deletion are separate operations; callers must arrange their order and retire outstanding client work.

## Source and Test Map

| Area | Source | Focused Tests |
|---|---|---|
| Actor state and sessions | [ddisk_actor.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor.cpp), [ddisk_actor_connect.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor_connect.cpp) | `ut/ddisk_actor_ut.cpp` |
| Boot, log, and chunks | [ddisk_actor_boot.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor_boot.cpp), [ddisk_actor_chunks.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor_chunks.cpp) | `ut/ddisk_actor_pdisk_ut.cpp` |
| Reservation bookkeeping and allocation ordering | [chunk_manager.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/chunk_manager.h) | `ut/chunk_manager_ut.cpp` |
| Read/write and I/O adapters | [ddisk_actor_read_write.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor_read_write.cpp), [direct_io_op.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/direct_io_op.cpp) | `ut/ddisk_actor_checksum_ut.cpp`, `ut/ddisk_actor_ut.cpp` |
| Integrity state | [integrity_manager.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/integrity_manager.cpp) | `ut/integrity_manager_ut.cpp` |
| Synchronization | [ddisk_actor_sync.cpp](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/ddisk_actor_sync.cpp) | `ut/ddisk_sync_ut.cpp`, `ut/ddisk_actor_ut.cpp` |

Paths in the test column are relative to `ydb/core/blobstorage/ddisk`. `ut_large` contains longer PDisk-backed I/O and synchronization scenarios. Select the relevant target and test cases rather than running the entire distributed storage test tree for a local change.

## See Also

- [{#T}](../distributed-storage.md)
- [{#T}](direct-block-groups.md)
- [{#T}](persistent-buffer.md)
- [DDisk source map](https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/ddisk/README.md)
