# DDisk and PersistentBuffer Source Map

The canonical developer contracts are in the [DDisk](../../../docs/en/core/contributor/distributed-storage/ddisk.md), [PersistentBuffer](../../../docs/en/core/contributor/distributed-storage/persistent-buffer.md), and [direct block group](../../../docs/en/core/contributor/distributed-storage/direct-block-groups.md) contributor pages.

| Task | Entry Points |
|---|---|
| Public requests and payload helpers | `ddisk.h`, `ddisk.cpp`, `ydb/core/protos/blobstorage_ddisk.proto` |
| Actor state, completions, and shutdown | `ddisk_actor.h`, `ddisk_actor.cpp` |
| Session validation | `ddisk_actor_connect.cpp` |
| PDisk owner recovery and PB child creation | `ddisk_actor_boot.cpp` |
| Reservation bookkeeping and allocation ordering | `chunk_manager.h` |
| Chunk allocation, formatting, commit, and deletion | `ddisk_actor_chunks.cpp` |
| Data read/write and owned read values | `ddisk_actor_read_write.cpp`, `direct_io_op.{h,cpp}`, `read_result.h` |
| Integrity format and state | `ddisk_checksums.{h,cpp}`, `integrity_manager.{h,cpp}` |
| Sync source fan-out and owning aggregate | `ddisk_actor_sync.cpp` |
| PB record layout and execution | `persistent_buffer{,_header}.h`, `ddisk_actor_persistent_buffer.cpp` |
| PB allocation and erase state | `persistent_buffer_space_allocator.*`, `persistent_buffer_barriers_manager.*` |
| PB fan-out and partial replies | `write_persistent_buffers_request_actor.*` |
| Monitoring | `ddisk_actor_mon.cpp`, `persistent_buffer_mon.*` |

`ut/` contains focused actor, integrity, sync, batching, barrier, and allocator tests. `ut_large/` contains longer PDisk-backed scenarios. Cross-component allocation and load-actor scenarios live in `../ut_blobstorage/` and `../ut_blobstorage/ut_ddisk/`.

`TChunkManager` owns allocation ordering, reusable reservations, and refill
accounting. Actor-owned records track request-triggered reservation, sequential
zero-format slices, integrity allocation, header and extent formatting,
mapping commits, and reclamation. Allocation waiters share one operation per
virtual chunk. Typed log actions are registered before submission and detached
in a batch before completion actions run. Direct-I/O completions retain buffer
ownership through retirement.

Read, write, and sync requests run as flat root coroutines that await their
device I/O in place. The read and write event handlers are ordinary functions:
they validate, answer rejections and unpublished chunks without a frame, and
only then start `ExecuteDataRead` or `ExecuteDataWrite`. Frames use the actor
runtime's shared TLS allocator, without a DDisk-owned frame cache. TLS cache
occupancy describes idle frames on executor threads, not live frames belonging
to an actor.
Read, Write, and Sync release the incoming event after moving their reply route,
original credentials, payload, and checksums into frame-owned records.
`ExecuteDataWrite` is the single write body: the write handler and each Sync
destination start one, and it awaits allocation readiness, integrity readiness,
data, and mapping durability one step at a time, revalidating the original token
after every wait. Each fully validated Sync source reply starts its destination
write independently; the parent aggregate waits for all accepted destination work
and returns results in input order. Device waits use unique completion cookies
independent of client cookies. A scoped `TDataIoPin` holds a chunk through
accepted data I/O, and a `TDataRequestGuard` counts the frame in
`DataRequestsInFlight`.
`TIoBatch` joins the device operations of one frame. Its atomic pending count
starts at one submission guard; each submission adds one, and each `TDDiskIoOp`
callback fills its own result slot and decrements the count. The unique
transition to zero publishes one `TEvIoBatchDone` through `TActorSystem` to the
frame's private wait cookie. Awaiting the batch releases the guard: a batch that
already finished resumes the frame inline and sends no event, otherwise the
cookie is registered in the same turn. Resuming rearms the counter for the next
batch. The waiter has no cancellation hook, so an accepted batch cannot unwind
early. Callbacks are non-owning references to lambdas in the frame and may run
in any order, including before submission returns; integrity loads mark their
operation critical.
For an allocated, formatted chunk, `ExecuteDataRead` keeps everything in its own
body. An unpublished chunk replies zeroes without I/O. With checksums disabled
it submits and awaits the data read. Otherwise `PrepareRead` either answers from
cached metadata, or claims pair loads: reads of at least 32 KiB
(`MetadataFirstReadThreshold`) await metadata first and skip the data read for an
all-zero plan, while smaller reads submit data and metadata in one batch.
`CompleteMetadataReads` then publishes the pair images, and the frame waits for
its own operation to settle, which also covers loads owned by another request.
`PrepareRead` constructs immediate metadata results in the caller's empty optional
or claims missing pair loads without submitting them. Warm reads move owned
result fields; cold reads inspect the immutable shared plan by const reference.
The owned `TReadChecksums` stores empty, singleton, or vector snapshots; larger
vectors move without reallocating, and copies own independent storage. Single-block
collection resolves one pair and sets AllZero or Passthrough without a mask.
Multi-block collection uses one reserved vector and gathers checksums and usage
in one pass by metadata pair before eviction. A rejected submission enters
Stopping, prevents further submissions, and fails rejected and unsubmitted
operations through the same callback path; accepted siblings still drain first.
Known zero ranges omit data I/O; joins never duplicate existing pair loads.
PDisk fallback submits one raw read per part into the same batch.
`TReadPayload` keeps native `TRcBuf` or fallback `TRope` ownership through I/O
callbacks.
`FinishDDiskRead` zeroes mixed-range holes and converts
native data to rope once for validation and reply attachment. The reply constructor
consumes checksum views immediately into protobuf. Metadata retries resubmit only
overloaded metadata parts and retain successful buffers. Read frames retain
their pins until both I/O and shared metadata settle.

Read validation resolves native credentials without rewriting protobuf. A read
does not wait for chunk allocation: until placement publishes a physical chunk,
including while another request's allocation is still pending, the read returns
zeroes. Disconnect/reconnect invalidates an old parked token even
with unchanged generation/session numbers (`SESSION_MISMATCH`, stale when known
to token history, otherwise invalid); unchanged-token reconnect remains valid.

Requested data blocks must not be written concurrently. Neighboring writes or
syncs may share a metadata pair: a read's checksum/mask snapshot is immutable and
does not wait for an unrelated flush. Whole-range pins protect pending reads,
and shared load records survive reader cancellation. Ordinary actor-thread
completion validates pair images and releases their dependencies. Integrity
mutations and pair flushes use ordinary actor-thread state transitions without
launching coroutines, including on cache misses. Writers share pair loads, apply
each complete mutation in one turn, and record its required version per pair.
Each pair has at most one immutable image in flight; newer mutations coalesce
in cached state for the next image. Successful completion advances the durable
version and alternating slot; a mutation succeeds after all its versions are
durable. Pending loads and writes retain physical ownership through completion,
even if the caller detaches. Allocation, formatting, and background integrity
reclamation use ordinary actor-owned records. Extent placement permits data writes;
readiness also requires extent formatting and all three chunk headers before
the mapping log can be submitted.

Sync validates its complete structure before dispatching all source reads.
It prefetches existing destination metadata without waiting for source data,
then validates each reply's status, payload presence and size, and configured
checksum requirements before starting that input's destination write. A
missing destination is allocated only for a fully valid input. Each destination
retains the complete source payload until its accepted work finishes. Source and
destination completion cookies reject stale or wrong-kind results. Producers
must prevent overlapping physical write/write and read/write ranges; DDisk
does not serialize whole extents or supersede overlapping Sync requests.

Broken and Stopping resolve logical waits without canceling accepted router
I/O waits. Failed branches still drain submitted siblings, retaining their
buffers and physical ownership. Sync abandons pending results from already-sent
source reads on stop and prevents unsubmitted destination writes; submitted
destinations finish before their parent aggregate retires.
Batch and integrity pair-write completions are handled during Stopping too;
reservation release and Gone require `DataRequestsInFlight` to be zero and the
pair-write cookie registry to be empty as well as callback drain. Forced
destruction drains callbacks before releasing remaining registry pins.

Shutdown tests must distinguish the indefinite normal actor drain from the
60-second `io_stalled` diagnostic and the 10-second forced-destructor deadline.
Use explicit callback and mailbox barriers to check intermediate ordering;
wall-clock deadlines are hang watchdogs. Router callbacks retain actor state
until retirement, while canceled fallback operations must publish one result
per request before Gone. Validate native io_uring and PDisk fallback separately.

Review session identity, delayed replies, physical ownership, and replay together when changing a persistent operation. NBS quorum, role rotation, dirty-map routing, and flush scheduling are owned by the [partition implementation](../../nbs/cloud/blockstore/libs/storage/partition_direct/README.md); the [load actor](../../load_test/rfc/nbs_dbg_like/README.md) has its own workload policy.
