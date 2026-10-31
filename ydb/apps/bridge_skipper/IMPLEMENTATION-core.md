# Controller loop, policy and configuration

[Implementation index](IMPLEMENTATION-index.md)

Read this module for process ownership, scheduling, policy decisions and timing.
Read [healthcheck](IMPLEMENTATION-healthcheck.md) for observation construction,
[admin](IMPLEMENTATION-admin.md) for topology and RPC dispatch,
[operations](IMPLEMENTATION-operations.md) for durable transitions, and
[HTTP](IMPLEMENTATION-http.md) for public admission and projections.

## Source map

| Source | Entry points and responsibility |
| --- | --- |
| [main.cpp](main.cpp) | `main`, `Run`: validation, resources, actors, listener, shutdown |
| [config/config.h](config/config.h), [config/config.cpp](config/config.cpp) | `TConfig`, `ParseConfig`, `ValidateConfig`, `ValidateStartup` |
| [runtime/controller.h](runtime/controller.h), [runtime/controller.cpp](runtime/controller.cpp) | `TControllerCore`, `TImpl`, `TControllerActor`: serialized controller state and effects |
| [runtime/events.h](runtime/events.h) | Owned read, storage, mutation and permission envelopes |
| [domain/scheduler.cpp](domain/scheduler.cpp) | `VerifiedInventory`, `TBoundedScheduler`: fixed samples and retained capacity |
| [domain/policy.cpp](domain/policy.cpp) | `EvaluatePolicy`: decision over supplied observations and `TSkipperClock` time |
| [domain/evidence.cpp](domain/evidence.cpp), [domain/types.cpp](domain/types.cpp) | Validation, aggregation, confirmation and canonical identities |
| [domain/clock.cpp](domain/clock.cpp) | `TSkipperClock`: replaceable time provider used by deterministic tests |
| [runtime/resources.cpp](runtime/resources.cpp) | Storage executor, immutable publication, callback gate and local stop |

## Process and thread ownership

1. `main` installs the clock provider and signal handlers, parses explicit CLI
   arguments, loads configuration once, then reads credentials. `--help` and
   `--version` return before loading configuration. `--check-config` validates
   syntax and credential files without opening listeners, contacting YDB or
   creating the state directory.
2. `Run` creates `TRuntimeResources` before actors. Its dedicated storage thread
   opens/replays the store and loads the local-stop marker. The main thread may
   wait for startup; actor workers do not perform this blocking startup I/O.
3. One SDK driver serves all endpoint clients. NActors has two pools with two
   workers each: `skipper-control` for gateway/controller and `skipper-http` for
   HTTP/proxy/poller. Each actor serializes its own mailbox.
4. `TControllerCore` owns mutable controller state and emits effects. It has no
   SDK handle, filesystem work or blocking waits. `TControllerActor::Flush`
   publishes the immutable snapshot before sending read, storage and dispatch
   effects, so the gateway validates against an installed durable prefix.
5. SDK callbacks retain a `TCompletionGate`, recipient ID and owned result.
   The gate serializes `Send` with `Close`; foreign producers never retain an
   actor object or an activation context. Storage jobs run on one executor.

## Main loop and progress

`TControllerActor::Bootstrap` observes the stop latch, ticks the core, flushes
effects, and schedules a wakeup every 250 ms. A timer generation rejects old
wakeups. Read/storage/mutation replies also enter the core and flush effects;
progress does not wait for a whole health round.

`TControllerCore::Tick` runs these stages in order:

1. Clear the per-tick set of tried manual preflight endpoints.
2. Close an expired health round, select the corroborated admin view, and publish
   readiness. Start a new eligible round and independently poll inventory/admin
   sources. Poll intervals schedule from the current time, without catch-up
   bursts.
3. If a durable proposal has unknown admission, issue one correlated lookup.
4. Call `Pump`, consider a checkpoint, and publish the resulting snapshot.

`Pump` stops while a proposal is pending or storage is unavailable. Otherwise it
handles one next action, with priority: queued RPC outcomes, external submissions,
durable generation high-water advancement, background reconciliation, qualified
observation acceptance, automatic admission, then the ordinary workflow's next
preflight. The first four proposing paths return after issuing one proposal.
The durable reducer and gateway remain separate checks on every external effect.

Idle ticks publish observations but do not journal health snapshots. Checkpoints
are considered after 1,000 applied commands or 60 seconds with new applied work;
shutdown also considers a checkpoint once outcome obligations permit it.

## Readiness, epochs and revocation

`Fresh` requires a non-future admin view younger than the observation TTL, no
admin blocker, and generation at least the durable accepted high-water mark.
`Ready` additionally requires available storage, no shutdown/local stop/state
conflict, and an emergency-eligible topology. Lifecycle work also requires a
fresh lifecycle configuration; emergency failover may retain a stale,
uncontradicted configuration under the topology rules in the admin module.

The publication distinguishes authority readiness, new-mutation readiness and
dispatch readiness. Dispatch can use its own installed unresolved-attempt slot;
new admissions cannot. Ownership acknowledgement and a requested mode other
than Observe are required for dispatch. Automatic dispatch further requires a
qualified profile, Auto mode and no sampling blocker.

`TEpoch` binds topology revision, complete admin fingerprint, policy revision and
readiness generation. `ConfirmationGeneration` separately revokes evidence
bindings. Readiness changes, local stop, topology/admin changes and invalidated
confirmation prevent a queued callback or grant from regaining authority after
recovery. A later valid round cannot revive an old permission.

`TryAutomatic` can rebind an existing queued automatic operation to a fresh
confirmed decision with the same suppression key, before an attempt exists.
Its suppression key ignores readiness generation while preserving the intended
update and other decision bindings. Once an intent exists, the negative-closure
and ambiguity rules in the operations module govern recovery.

## Decision algorithm

`EvaluatePolicy` returns Blocked, Stable or Failover. The following logic is
implemented in the domain model; production Auto remains gated as described
below.

1. Validate the public state, its epoch fingerprint, qualification and round
   times. Accept only observations from this round/process/epoch with verified
   sampled identities, valid brackets and unexpired evidence. Reject duplicate
   responder IDs and duplicate pile samples.
2. Build a report for each frozen sample. A sample has up to the configured node
   count and threshold `floor(sample_size / 2) + 1`. Missing responders remain in
   that denominator. Endpoint/node conflicts are excluded by `VerifiedInventory`.
3. For GLOBAL_BSC, all answers represent one shared source. Contradictory
   determinate assessments block the decision. One fixed pile sample must have
   a majority of complete observations covering both the candidate and every
   failed target; per-target majorities cannot be stitched together.
4. For local profiles, use only eligible reports with independent origins.
   Failure requires a strict majority of eligible reporters; UNKNOWN target rows
   stay in the denominator. The selected candidate must report itself healthy
   and agree that each proposed target failed. A special complete two-active-pile
   mutual-failure case retains the current primary when both local self-views
   are healthy.
5. Prefer the healthy current primary. Otherwise iterate synchronized candidates
   in named-map order and require a verified, fresh direct endpoint. Choose the
   lowest eligible node ID within the candidate pile.
6. Block unsupported resynchronizing-target evidence and require the independent
   placement acknowledgement for GLOBAL_BSC or a sole local reporter. No approved
   targets yields Stable. Failover disconnects all approved targets together;
   primary loss also assigns the candidate PRIMARY and explicitly clears any
   surviving abandoned PROMOTED role. `ValidateExactUpdate` checks the overlay.
7. Select the necessary fresh justification and set expiry from the earliest
   health-call start plus TTL among those selected observations. Require the
   remaining dispatch margin. Returned evidence retains its original expiry.

`CloseRound` evaluates only once its deadline is reached. `TConfirmation` requires
the same normalized decision in consecutive round IDs, with actual round starts
no farther apart than `ConfirmationStartGap`. The canonical key includes roles,
exact update, profile/source and epoch, rather than rotating sample/request IDs.
Blocked, changed, duplicate or excessively separated rounds revoke confirmation.

## Configuration and time budgets

[config/example.yaml](config/example.yaml) shows the external key names.
`ParseConfig` accepts one YAML document, rejects unknown/duplicate keys and
malformed shapes, and limits the file to 1 MiB. Schema version is 1; cluster ID
and a nonempty endpoint list are required. The only authority backend is `local`.
Endpoints require explicit `grpc://` or `grpcs://`, a port in 1..65535, one common
scheme and bracketed IPv6 where needed. No path, query or userinfo is accepted.
Credentials are bounded startup-only reads; cert/key must be paired, and a token
over plaintext requires explicit opt-in. Changing configuration requires restart.

Defaults come from `config/config.h`. Durations below are seconds.

| Configuration key | Default | Role |
| --- | --- | --- |
| `http.listen` | `127.0.0.1:8080` | Single HTTP authority boundary |
| `evidence.profile` | `global_bsc_v1` | Parser selection, not certification |
| `round_interval_seconds` / `round_deadline_seconds` | 15 / 12 | Health-round cadence and closure |
| `bracket_admin_timeout_seconds` | 1 | Each surrounding admin read |
| `health_operation_timeout_seconds` / `health_client_timeout_seconds` | 8 / 9 | Server health budget and client timeout |
| `admin_interval_seconds` / `config_interval_seconds` | 1 / 30 | Independent state and inventory polling |
| `read_rpc_timeout_seconds` | 10 | Ordinary admin/config/discovery read budget |
| `observation_ttl_seconds` | 20 | Evidence freshness |
| `admission_budget_seconds` / `dispatch_margin_seconds` | 5 / 2 | Sizing/telemetry budget and minimum handoff slack |
| `scheduler_tolerance_seconds` / `confirmation_start_gap_seconds` | 5 / 20 | Validated scheduling slack and confirmation start gap |
| `sample_nodes_per_pile` / `per_pile_health_concurrency` | 3 / 3 | Frozen sample size and chain capacity |
| `global_admin_concurrency` | 16 | Admin capacity, including two operation slots |
| `mutation_client_timeout_seconds` / `membership_client_timeout_seconds` | 120 / 600 | Update/Convert and Add/Remove client timeouts |
| `logging.level` / `logging.display_ring_lines` | `info` / 1000 | See [metrics and logs](IMPLEMENTATION-metrics.md) |

Polling keys are under `polling`; mutation timeout keys are under `operations`.
`operations.attention_after_seconds` defaults to 300 for failover/switchover/
takedown, 600 for primary maintenance, null for rejoin, and 3,600 for
convert/add/remove. Attention thresholds affect monitoring, not RPC cancellation.

`ValidateConfig` and `ValidateSchedulerConfig` enforce:

- Positive whole-second durations; `2 * bracket + health_client < round_deadline
  <= round_interval`.
- `health_operation < health_client < observation_ttl`.
- `observation_ttl >= round_deadline + admission_budget + dispatch_margin`.
- `confirmation_start_gap >= round_interval + scheduler_tolerance`.
- Sample size 1..101, positive per-pile health concurrency, and admin capacity
  at least 3. The frozen sample uses the smaller of configured sample size and
  verified inventory size. If that actual sample exceeds per-pile chain capacity,
  `CONFIG_BUDGET` blocks automatic decisions without shrinking the sample.

`AdmissionBudget` is a sizing/measurement input, never an extra dispatch lease.
Dispatch uses the original admin or health TTL and rechecks absolute validity.
The scheduler retains unsettled chain reservations after round closure; the
gateway retains RPC capacity until the underlying future settles. A late/hung
request cannot free capacity merely by reaching its logical deadline.

## Shutdown and present limits

SIGINT/SIGTERM asks the main coordinator to close admission, poison the
controller into draining mode and quiesce the gateway. Main waits up to 10 seconds
for the observed drain conditions, then stops the SDK with `Stop(true)`, joins
storage, closes the completion gate, and stops/cleans up the actor system.
The 10-second drain window is not a bound on SDK/storage shutdown. Actor
destruction does not join foreign producers or infer cancellation of YDB work.

`RuntimeOptions` leaves production evidence qualification, decoder/provenance
hooks and observation-acceptance qualification flags empty. Live SelfCheck lacks
expected physical-group inventory integration and keeps assessments UNKNOWN.
`IsProductionAutoQualified` returns false for every profile. The NodeWarden
profile is unavailable. Qualified test fixtures prove controller machinery under
injected evidence, not a complete live assessment path. See
[qualification](docs/qualification.md) for remaining implementation and real-build
qualification work.

## Review and focused validation

Trace any loop change through `Tick`, `Pump`, the relevant result handler and
`TControllerActor::Flush`; check that snapshots precede effects and pending or
unknown durability cannot expose a candidate state. For policy changes, preserve
UNKNOWN, fixed denominators, source provenance, original TTL and revocation.

Start with `config/ut`, `domain/ut` and `runtime/ut` under this application.
Existing regressions cover timing boundaries, no sample shrinkage, no catch-up
burst, complete global vectors, local-origin independence, pre-intent revocation,
queued Auto recovery, original TTL after admission-budget overrun, and no idle
journal writes. Use the active `ydb-build-test` skill and personal checkout build
policy for execution. Source inspection of these cases is not a passing run.
