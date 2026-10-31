# Durable operations, storage and recovery

[Implementation index](IMPLEMENTATION-index.md)

This module explains the local command journal and mutation lifecycle.
[Administration](IMPLEMENTATION-admin.md) owns exact Bridge requests and their
postconditions; [core](IMPLEMENTATION-core.md) owns policy and event scheduling;
[HTTP](IMPLEMENTATION-http.md) owns signed previews and client receipts.

## Source map

| Source | Entry points and responsibility |
| --- | --- |
| [durable/durable.h](durable/durable.h) | Durable state, commands/results, operations, attempts, background obligations and acceptance inputs |
| [durable/durable.cpp](durable/durable.cpp) | Pure `Apply`, `CancelError`, `IsAttemptClosed`, `HandoverComplete`, `ValidateAcceptance`, `BackgroundDone` |
| [durable/workflow.cpp](durable/workflow.cpp) | `PlanWorkflow` and immutable step vectors |
| [durable/store.cpp](durable/store.cpp) | `TLocalStore` binding, transactions, log/snapshot validation, replay and unknown resolution |
| [durable/codec.cpp](durable/codec.cpp) | Canonical serialization, SHA256 hashes and `MutationArgumentsHash` |
| [runtime/controller.cpp](runtime/controller.cpp) | `Propose`, `OnStorage`, `TryAcceptance`, `BackgroundChanges`, `MaybeCheckpoint`, dispatch grants and outcome reservations |
| [runtime/gateway.cpp](runtime/gateway.cpp) | `TGatewayBudget`, dispatch/closure ordering and underlying SDK future ownership |
| [runtime/resources.cpp](runtime/resources.cpp) | Serialized storage executor, local-stop latch, stop marker and independent stop audit |

## State and command boundaries

`TDurableState` binds a cluster, member, local backend and membership
incarnation. It contains the applied log index, controller revision, accepted
generation high-water mark, requested mode, ownership/history flags, operations,
attempts, automatic-plan suppression keys and command/request receipts.

`Apply(original, command)` copies its input and deterministically returns the
next state and result. It performs no I/O, SDK calls, clock reads or random ID
generation. Freshness, readiness, observations and audit time are explicit
command inputs. Command-ID deduplication precedes request-ID deduplication;
both precede revision checks. A reused ID with another payload is rejected.
Valid processed commands, including business rejections, advance the applied
index and retain receipts; successful commands also advance controller revision.

An operation owns an immutable plan and tracks revision, next step, attempts,
audit events and background obligations. One `OrdinaryOwner` serializes normal
workflow progress. One `UnresolvedAttempt` blocks every subsequent mutation
invocation. Terminal operation status alone does not establish that all server
background work is settled.

`TMutationAttempt` records the exact method/vector/target, endpoint, node,
baseline, topology and workflow revision, designated member/process and intent
index. `MutationArgumentsHash` binds method, updates, target, preflight generation
and empty quorum-piles. Live permissions are separate in-memory objects and are
never serialized or recreated by journal replay.

## Admission, storage acknowledgement and dispatch

1. The controller validates a submission, computes an `Apply` candidate and
   emits a storage request while retaining one pending proposal. It publishes
   only the installed durable state, never that candidate.
2. `TRuntimeResources::SubmitStore` sends the blocking transaction to the single
   storage thread. Its queue bounds queued plus active jobs to 256. Actor
   workers neither use SQLite directly nor wait for filesystem work.
3. `TLocalStore::Propose` checks the installed prefix/content digest, applies
   the command again, and writes the command, hash, envelope and result in one
   transaction. A durable business rejection still has a committed receipt.
4. `OnStorage` checks the correlated reply, applied index, controller revision
   and result before installing it. Only successful committed `AdmitAttempt`
   can create a one-use dispatch permission. See the admin guide for handoff
   validation.
5. Unknown local commit completion blocks authority and invalidates readiness.
   `ResolveUnknown` looks up the original command ID and reloads the journal
   before any new write. The controller installs the committed result or closes
   the uncommitted proposal; it does not infer success from a candidate.

The controller reserves one of 16 outcome slots before admitting an intent.
It retains that reservation until the terminal gateway result is durably
recorded, including after early observation acceptance. The gateway separately
retains SDK future capacity until settlement. Neither an accepted state nor a
timeout invents a second future result or frees an unsettled future slot.

On restart, replay restores durable mode, attempts and uncertainty. Process
identity changes and no live grant is restored, so an admitted intent cannot
silently dispatch again. This is local once-per-attempt handoff machinery;
exclusive administration remains an operational assumption without a server
lease or fencing token.

## RPC outcomes and observation acceptance

`RpcOutcome` correlates the designated process/member and records one terminal
RPC disposition. The current SDK adapter supplies status and redacted issues;
qualification-only outcome fields default to false.

| Outcome | Durable effect |
| --- | --- |
| SDK success | Mark accepted, release unresolved slot, create a background obligation. |
| Qualified definitive rejection | Close the attempt and reject the operation; suppress an automatic plan with the same suppression key. |
| Qualified Add/Remove background error | Accept the request while retaining its background obligation and attention state. |
| Other failure/timeout/start error | Mark ambiguous, retain unresolved slot and require reconciliation/recovery. |
| Gateway `NeverDispatched` proof | Close without an external effect; queue executing automatic work again only while still in Auto, otherwise pause for explicit Resume. |

Definitive classification is deliberately narrow. The reducer recognizes the
exact unsupported-pile-state error; generation-mismatch rejection and pinned
lifecycle-background classification require explicit qualification inputs.
Arbitrary `INTERNAL_ERROR`, `BAD_REQUEST` or matching current state is not a
negative proof. There is no retry of an ambiguously delivered mutation.

`TryAcceptance` is a separate positive proof path. It requires an ordered,
fresh admin quorum started after this process's recorded handoff, the same
topology, exclusive history, qualified no-replay and acceptance-marker behavior,
and provenance. `ValidateAcceptance` then checks a distinguishing transition
reachable by the exact request, preserving unmentioned states except explicitly
qualified YDB follow-ups. A no-op/already-present target is insufficient.
Late contradictory RPC rejection sets `StateConflict` even after acceptance.

Production runtime initialization leaves these qualification capabilities unset.
Qualified acceptance and Auto success tests inject them. They verify protocol
machinery under explicit assumptions rather than qualify the production SDK
path or close a live ambiguous request automatically.

## Background obligations, modes and operator controls

`Reconcile` uses fresh coherent state and `ExclusiveHistory` to complete the
postconditions listed in the admin guide. Accepted RPC completion and observed
workflow completion are different events. Rejoin additionally requires fresh
qualified self-health for the exact target; the current incomplete live health
adapter keeps that final obligation pending after SYNCHRONIZED.

Failover can preempt the ordinary owner only after the unresolved slot is clear.
The old operation becomes Preempted; its continuing obligations remain guarded.
Conversion blocks all other work, pending membership constrains its exact target
and further membership work, and abandoned promotion roles must be explicitly
superseded. A disjoint emergency does not cancel ongoing Add/Remove work.

First start requests Manual mode with no ownership. Manual/Auto dispatch requires
an exclusive-administration acknowledgement and deployment-plan reference;
policy also uses the independent-failure-domain acknowledgement. Entering
Observe revokes active ownership immediately. It cancels never-attempted queued
work and pauses other nonterminal work while retaining `ExclusiveHistory` until
all unresolved attempts and background obligations hand over. Qualified late
acceptance/reconciliation can continue without permitting another dispatch.
An intermediate maintenance step remains paused until explicit Resume.

`CancelError` permits cancellation of remaining steps only when none is executing,
all attempts are closed and background work is complete. Applied earlier steps
remain applied. Resume requires a Paused operation, current revisions, settled
prior postconditions and current readiness. Evidence-bound Resume additionally
needs a newly matching confirmed decision; it cannot turn into ordinary Manual
work after restart or permission expiry.

`ResolveExternal` requires a reason, explicit unfenced-recovery acknowledgement,
attestation that the old executor is stopped and server work reconciled, and
fresh coherent state at or above the generation high-water mark. It closes local
obligations without proving the old RPC outcome, requests Manual mode, revokes
ownership and ends exclusive history. A new acknowledgement is required for
subsequent work. See the [operator runbook](docs/operations.md) for procedures.

## Filesystem and codec

`TLocalStore` requires an owner-only nonsymlink state directory. It creates the
final directory with mode 0700 when absent; its parent must already exist. It
takes a nonblocking exclusive `controller.lock`, opens `skipper.sqlite3` with
WAL, `synchronous=FULL`, foreign keys and zero SQLite busy timeout, and checks
schema/cluster/member/backend binding. Startup syncs the state directory and its
physically resolved parent before exposing recovered state.

Schema 1 stores metadata, the complete indexed command log and one snapshot.
Load checks snapshot digest/binding, every contiguous log envelope/hash and
snapshot-prefix receipt, then deterministically replays the suffix. Corruption,
missing authoritative records, unsupported schema and reused process identity
fail closed. Checkpoints do not delete the log.

The codec uses sorted canonical JSON, required exact field sets, decimal-text
integers, explicit optional nulls, depth 64 and SHA256 hashes. Decode re-encodes
to reject noncanonical data. `MaxDurableRecordBytes` is 256 MiB. Oversized commands
are rejected before insertion; oversized checkpoints keep the previous snapshot
and complete log, report a skip, and leave storage available. Other checkpoint
failures block readiness. `MaybeCheckpoint` normally waits for 1,000 new commands
or 60 s, with a shutdown opportunity after outstanding outcomes are settled.

## Independent local stop

`TLocalStopLatch` closes the live gate immediately, independently of controller
or journal progress. Persistence runs on the storage executor: write/flush a
temporary `local-stop` marker, rename it, sync the directory, and append the
separate `local-stop.audit`. Startup loads a valid marker before admission.
A failed marker write leaves a visible volatile stop and keeps dispatch closed.

Release binds process and stop revision, deduplicates by request ID/body, audits
the release, removes/syncs the marker and only then opens the latch if the
revision is still current. A stale release cannot clear a newer stop. Stop audit
has no shared ordering with the command-log index. Display logs are described
in [metrics and observability](IMPLEMENTATION-metrics.md).

## Review and focused coverage

Trace each changed state transition through `Apply`, codec fields, store replay,
controller installed-prefix checks and external effects. For crash/reorder cases,
preserve the distinction between committed intent, live grant, handoff, RPC
settlement, positive acceptance and background completion.

Use [durable tests](durable/ut/durable_ut.cpp) for deduplication, ambiguity,
Observe/preemption/cancel/recovery, checkpoint limits and corrupt-store cases;
[controller tests](runtime/ut/controller_ut.cpp) for unknown admission, restart
without grants, confirmation recovery, qualified acceptance and late outcomes;
[gateway tests](runtime/ut/gateway_ut.cpp) for closure versus handoff and future
capacity; and [resource tests](runtime/ut/resources_ut.cpp) for stop/release
persistence and races. Targets are `durable/ut` and `runtime/ut` under this
directory. Follow the active `ydb-build-test` skill for execution.
