# DDisk and PersistentBuffer Source Map

The canonical developer contracts are in the [DDisk](../../../docs/en/core/contributor/distributed-storage/ddisk.md), [PersistentBuffer](../../../docs/en/core/contributor/distributed-storage/persistent-buffer.md), and [direct block group](../../../docs/en/core/contributor/distributed-storage/direct-block-groups.md) contributor pages.

| Task | Entry Points |
|---|---|
| Public requests and payload helpers | `ddisk.h`, `ddisk.cpp`, `ydb/core/protos/blobstorage_ddisk.proto` |
| Actor state, completions, and shutdown | `ddisk_actor.h`, `ddisk_actor.cpp` |
| Session validation | `ddisk_actor_connect.cpp` |
| PDisk owner recovery and PB child creation | `ddisk_actor_boot.cpp` |
| Reservation bookkeeping and allocation ordering | `chunk_manager.h` |
| Chunk allocation, formatting, commit, and deletion coroutines | `ddisk_actor_chunks.cpp` |
| Data read/write | `ddisk_actor_read_write.cpp`, `direct_io_op.{h,cpp}` |
| Integrity format and state | `ddisk_checksums.{h,cpp}`, `integrity_manager.{h,cpp}` |
| Unified sync and overlap ordering | `ddisk_actor_sync.cpp`, `segment_manager.{h,cpp}` |
| PB record layout and execution | `persistent_buffer{,_header}.h`, `ddisk_actor_persistent_buffer.cpp` |
| PB allocation and erase state | `persistent_buffer_space_allocator.*`, `persistent_buffer_barriers_manager.*` |
| PB fan-out and partial replies | `write_persistent_buffers_request_actor.*` |
| Monitoring | `ddisk_actor_mon.cpp`, `persistent_buffer_mon.*` |

`ut/` contains focused actor, integrity, sync, batching, barrier, and allocator tests. `ut_large/` contains longer PDisk-backed scenarios. Cross-component allocation and load-actor scenarios live in `../ut_blobstorage/` and `../ut_blobstorage/ut_ddisk/`.

`TChunkManager` owns allocation ordering, reusable reservations, and refill
accounting. Actor coroutines own asynchronous PDisk requests, formatting,
placement, durability waits, and startup orphan reconciliation. Allocation
waiters share one operation per virtual chunk; serialized writes and sync
segments use FIFO extent admission. `LogWaiters` dispatches batched log results
by LSN to actor-local continuations. Log submission captures snapshots and
records commit intent before suspension; direct-I/O completions retain buffer
ownership through retirement.

Read, write, and sync request coroutines join their submitted data and metadata
work before replying. Generic device waits use unique completion cookies independent of
client cookies; indexed DDisk reads use their own tokens and envelope cookie zero. Write and sync data submission helpers return native event
awaiters; a scoped `TDataIoPin` holds the chunk through the data completion.
Ready allocation, FIFO admission, and commit checks avoid child wait coroutines.
For an allocated, formatted chunk, `ReadDDisk` returns one ordinary awaiter to
the read handler. Ready needs no registry entry. DataEvent uses reusable actor-owned
indexed slots without aggregate allocation or generic waiter registration; Cold
joins its parent I/O and shared metadata loads. Slot tokens combine generation
and index, survive vector growth, and reject stale or duplicate completions. A
slot is never reused after generation exhaustion. Cancellation detaches the
awaiter but retains submitted slots and chunk pins until terminal processing;
completion releases the slot before resuming inline.
`PrepareRead` constructs immediate metadata results in the awaiter's empty optional
or claims missing pair loads without submitting them. Warm reads move owned
result fields; cold reads inspect the immutable shared plan by const reference.
Collection reuses the resolved extent and gathers checksums and usage in one
pass by metadata pair before eviction. The single DirectIo object combines data and newly
claimed metadata parts in one initial router admission. Known zero ranges omit
data I/O; joins never duplicate existing pair loads. PDisk fallback submits one
raw read per part and aggregates their completions. Data buffers transfer to the
reply directly; checksum vectors reach the reply constructor by const reference
without an intermediate copy. Metadata retries resubmit only failed metadata parts.

Requested data blocks must not be written concurrently. Neighboring writes or
syncs may share a metadata pair: a read's checksum/mask snapshot is immutable and
does not wait for an unrelated flush. Whole-range pins protect pending reads,
and shared load records survive reader cancellation. Ordinary actor-thread
completion validates pair images and releases their dependencies. Writers use
the same ordinary pair submission interface; one flush coroutine per pair
serializes immutable images and tracks each mutation's required durable version.
Background integrity reclamation creates a log-wait coroutine only when it
submits a reclamation log. Extent placement permits data writes;
readiness also requires extent formatting and all three chunk headers before
the mapping log can be submitted.

Sync directly awaits one input request or one surviving destination segment;
multiple requests or segments use task groups. Both paths share payload and
checksum slicing, including a singleton range trimmed by supersession.

Broken and Stopping resolve logical waits without canceling accepted router
I/O waits. Failed branches still drain submitted siblings, retaining their
buffers and physical ownership. Sync cancels superseded preparation only;
submitted destination segments finish before extent admission is released.
Indexed completions are handled during Stopping too; reservation release and
Gone require zero active slots as well as callback drain. Forced destruction
drains callbacks before releasing remaining registry pins.

Shutdown tests must distinguish the indefinite normal actor drain from the
60-second `io_stalled` diagnostic and the 10-second forced-destructor deadline.
Use explicit callback and mailbox barriers to check intermediate ordering;
wall-clock deadlines are hang watchdogs. Router callbacks retain actor state
until retirement, while canceled fallback operations must publish one result
per request before Gone. Validate native io_uring and PDisk fallback separately.

Review session identity, delayed replies, physical ownership, and replay together when changing a persistent operation. NBS quorum, role rotation, dirty-map routing, and flush scheduling are owned by the [partition implementation](../../nbs/cloud/blockstore/libs/storage/partition_direct/README.md); the [load actor](../../load_test/rfc/nbs_dbg_like/README.md) has its own workload policy.
