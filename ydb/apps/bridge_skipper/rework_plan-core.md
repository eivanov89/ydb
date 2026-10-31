# Rework plan: core runtime, configuration and persistence

[Index](rework_plan.md). This plan replaces the process structure, the main loop, configuration and all durable state, and absorbs the former operations module. The decision logic is in [policy](rework_plan-policy.md); RPC settings and outcomes are in [admin](rework_plan-admin.md).

## Current behaviour and evidenced gaps

### Process structure

`main.cpp:42-56` builds an actor system with two pools, which runs the following:

- a gateway actor that enforces budgets, a one-use dispatch permission and two validation passes (`runtime/gateway.cpp`);
- a controller actor that drives about 60 fields of `TControllerCore::TImpl` (`runtime/controller.cpp:59-177`);
- an HTTP actor.

`TRuntimeResources` (`runtime/resources.cpp:164-211`) adds three more pieces:

- a SQLite storage thread;
- `TCompletionGate`, which serialises SDK callbacks against shutdown;
- the local-stop latch.

### Path from a decision to a mutation

A decision reaches YDB in five steps:

1. `AdmitOperation` is committed to SQLite.
2. A same-node preflight `GetClusterState` runs.
3. `AdmitAttempt` is committed to SQLite.
4. A permission is minted.
5. The gateway re-checks and then sends the request (`runtime/controller.cpp:1032-1252`).

### Evidenced gaps

| ID | Class | Gap |
| --- | --- | --- |
| C-G1 | D | **Any non-SUCCESS mutation outcome is Ambiguous.** This blocks every later mutation, including failover. Only `ResolveExternal` clears it, and that also forces Manual mode and revokes ownership (`durable/durable.cpp:467-530`, `:315`, `:628-661`). |
| C-G2 | D | **Restarting with an admitted intent is a permanent block.** Outcomes are bound to the old process (`runtime/controller.cpp:905`). Asserted by `runtime/ut/controller_ut.cpp:947-962`. |
| C-G3 | D | **An in-flight Add or Remove holds the single mutation slot.** It holds it for up to 600 s (`config/config.h:65`), or forever if its outcome is ambiguous. Failover is blocked meanwhile (`durable/durable.cpp:315-317,442`). |
| C-G4 | D | **Rejoin never completes in production.** `FreshHealthyPiles` requires qualified evidence (`durable/durable.cpp:49-50`, `runtime/controller.cpp:684-703`). Rejoin therefore owns the ordinary workflow forever. |
| C-G5 | D | **A failed checkpoint disables authority until restart.** The checkpoint is only an optimisation, yet the failure is sticky (`durable/store.cpp:413-415`, `runtime/controller.cpp:1748-1752`). |
| C-G6 | D | **`StateConflict` has no reset path.** It only has a setter (`durable/durable.cpp:484`). |
| C-G7 | P | **The default SelfCheck operation timeout (8 s) leaves BSC only 4 s.** The server's default budget is 20 s (`ydb/core/health_check/health_check.cpp:869-870`; default `THealthCheckConfig.Timeout` in `ydb/core/protos/config.proto`). |
| C-G8 | S | **25 timing and capacity keys**, most of them used only by validation or telemetry. Examples: `admission_budget_seconds`, `scheduler_tolerance_seconds`, `confirmation_start_gap_seconds` (`config/config.cpp:198-291`, `329-345`). |

## Intended behaviour and design

### Threads and actors

The design keeps NActors, because the HTTP proxy and logging already use it. The structure collapses to:

- one pool with 2 threads;
- `TSkipperActor`;
- the HTTP actor.

**TSkipperActor.** It owns all mutable state and ticks every 250 ms.

- At most one call is outstanding per `(endpoint, method)`.
- An endpoint that still has a pending call is skipped. It does not wait.
- Every call has both a client deadline and a server operation timeout (overview §3.3.1).
- SDK futures complete via `Subscribe` → `ActorSystem->Send(SelfId, ...)`.
- A tick never waits on a future.

**Snapshot.** Each tick publishes an immutable `TSkipperSnapshot` behind a mutex-guarded `std::shared_ptr<const ...>`. The HTTP actor and the metrics scrape read it.

**Shutdown order.** On SIGINT or SIGTERM:

1. Stop scheduling.
2. Call `TDriver::Stop(true)`, which waits for in-flight calls and their callbacks.
3. Stop the actor system.

The actor system outlives every SDK callback, and a `Send` to a dead actor is dropped. This makes `TCompletionGate` unnecessary.

### Persistence

The state is a small `state.json` file, using the schema in the [index](rework_plan.md#shared-interface-decisions).

**Writing.**

- Writes go to a temporary file, then fsync, rename, and fsync of the directory.
- Writes run synchronously in the actor. They happen only when the high-water mark rises (from a selected view, or to G+1 after `Success` or `GenerationMoved`; see [admin](rework_plan-admin.md) A6) or when a pending-mutation record changes. Each write is a few milliseconds, which is acceptable.
- `skipper.lock` is taken with a non-blocking `flock`. This protects against a second process on the same directory.
- If `cluster_id` does not match the configuration, startup fails.

**Why each field is kept.**

- `generation_high_water`: skipper refuses any view whose maximum generation is below it (`STALE_HISTORY`).
  - Without it, a skipper that restarts while seeing only the far side of a partition (still at an older generation) could fail over that side. That would create split brain (overview §1.2, demo §2.3).
- `pending_mutation`: implements decision D7 (see [admin](rework_plan-admin.md) A6).
  - Two different updates at the same generation G can both commit on opposite sides of a partition (`distconf_invoke_bridge.cpp:195`).
  - A restart must not forget an unresolved one.
- `cluster_id`: binds the file to the configuration.

**Failure handling and overrides.**

- If a write fails, skipper does not send a mutation and reports the `STATE_WRITE_FAILED` blocker. Observation continues.
- `--clear-pending-mutation` is an explicit, logged operator override at startup. Deleting `state.json` also resets the high-water mark, and the runbook documents it as a deliberate split-brain recovery step.

### Configuration

Configuration is one YAML document. It is read once; changes require a restart.

```yaml
cluster_id: prod-bridge-1
mode: manual                      # manual | auto
endpoints:                        # pile name -> gRPC endpoints; the implicit pile uses its SelfCheck name
  dc-a: [grpcs://a1:2135, grpcs://a2:2135]
  dc-b: [grpcs://b1:2135, grpcs://b2:2135]
auth: {ca_file, cert_file, key_file, token_file, allow_plaintext_token}   # existing semantics
http: {listen: 127.0.0.1:8080}
timing:
  state_poll_interval_seconds: 1
  read_timeout_seconds: 5
  round_interval_seconds: 10
  selfcheck_operation_timeout_seconds: 12   # BSC gets 6 s
  selfcheck_client_timeout_seconds: 14
  evidence_ttl_seconds: 30
  confirm_seconds: 20
  bsc_warmup_seconds: 30
  mutation_operation_timeout_seconds: 25
  mutation_client_timeout_seconds: 30
health_endpoints_per_pile: 2
logging: {level: info}
```

`ValidateConfig` enforces the following:

- The endpoint scheme is `grpc://` or `grpcs://`, the same for all endpoints, with a port in 1..65535. This reuses the existing `ParseEndpoint`.
- At least one pile is configured, and endpoints are unique.
- `read_timeout < evidence_ttl`.
- `selfcheck_operation < selfcheck_client`.
- `selfcheck_client + round_interval < evidence_ttl`, so that a valid reply exists in each round.
- `confirm_seconds >= round_interval`.
- `bsc_warmup_seconds >= 20` (15 s VDisk status expiry plus a 5 s sysview cycle; overview §3.2.5).
- `mutation_operation < mutation_client`.

**Removed keys:**

- `schema_version`, `controller.authority_backend`, `evidence.profile`;
- `bracket_admin_timeout_seconds`, `round_deadline_seconds`, `admin_interval_seconds` (renamed `state_poll_interval_seconds`), `config_interval_seconds`;
- `observation_ttl_seconds` (renamed `evidence_ttl_seconds`), `admission_budget_seconds`, `dispatch_margin_seconds`, `scheduler_tolerance_seconds`, `confirmation_start_gap_seconds`;
- `sample_nodes_per_pile`, `per_pile_health_concurrency`, `global_admin_concurrency`;
- `membership_client_timeout_seconds`, `operations.attention_after_seconds`, `logging.display_ring_lines`;
- the top-level `ydb_endpoints` list (replaced by `endpoints` keyed by pile).

**CLI arguments:**

- Kept: `--config`, `--state-dir`, `--check-config`, `--help`, `--version`.
- Added: `--clear-pending-mutation`.
- Removed: `--member-id`.

No compatibility shim is provided for the old configuration. This is a pre-production tool with no deployed users, and nothing in the repository requires one.

## Removals

| Item | Reason the invariant still holds |
| --- | --- |
| All of `durable/`: `durable.*`, `store.*`, `codec.*`, `workflow.*` and `ut/`; the SQLite dependency | Lifecycle workflows are removed (D1). Mutation exactly-once is provided by the server compare-and-set plus the pending record. Background obligations can be read from `GetClusterState`. |
| `runtime/controller.*` (`TControllerCore`, `Pump`, epochs, readiness generations, grants, outcome reservations, handoff timing, checkpoints) | Decide-and-send happens in one actor turn with `expected_generation`, so there is no queued authority to revoke. A stale decision is rejected by the server with `ABORTED`. |
| `runtime/gateway.*` (`TGatewayBudget`, `ValidateLiveDispatch`, `ValidateDispatch`, the one-use attempt) | Replaced by `runtime/ydb_clients.*`. The budget becomes "one outstanding call per (endpoint, method)" inside the actor. |
| `runtime/resources.*` (storage executor, `TCompletionGate`, local-stop latch, marker, audit, log ring) | The storage executor is replaced by the synchronous small-file write. The gate is replaced by the shutdown order. The local stop is replaced by stopping the process or setting `mode: manual` and restarting (D2). The log ring is replaced by stderr logs ([observability](rework_plan-observability.md)). |
| `runtime/log_writer.*` | The NActors logger replaces it. |
| `runtime/events.h` (current envelopes) | Rewritten as a few `TEv*Reply` events with owned results. |
| `domain/clock.*` (`TSkipperClock`) | `now` is passed into domain functions explicitly. The actor uses `TActivationContext::Now()`. |
| `domain/scheduler.*`, `domain/topology.*`, `domain/evidence.*` | Replaced; see [admin](rework_plan-admin.md) and [healthcheck](rework_plan-healthcheck.md). |
| The `--member-id` binding and membership incarnation | There is one controller per state directory, and `flock` is kept. |

**Retained safeguards:**

- `flock` on the state directory;
- the owner-only state directory, keeping the existing mode 0700 check;
- startup failure reported as one JSON line on stderr;
- bounded credential reads;
- credential redaction in RPC issue text (`TRedactor`, `runtime/gateway.cpp:63-88`), moved into `ydb_clients`.

## Ordered tasks

**C1. Configuration.** Depends on nothing.
- Rewrite `TConfig`, `ParseConfig` and `ValidateConfig` with the schema above.
- Keep endpoint parsing, credential loading and the strict YAML checks (unknown and duplicate keys, 1 MiB limit).
- Update `config/example.yaml`.
- Tests in `config/ut`: defaults, every validation inequality, rejection of the old keys, an endpoint mapped to a pile, and a duplicate endpoint.

**C2. `runtime/state_file.{h,cpp}`.** Depends on nothing.
- Implement `TStateFile::Open(dir, clusterId)`, which takes the lock and loads or creates the file.
- Implement `Write(const TPersistentState&)`, an atomic replace that returns success or an error.
- Implement `ClearPending()`.
- Tests in `runtime/ut`:
  - round-trip;
  - mismatched `cluster_id` is rejected;
  - a second `Open` on the same directory fails;
  - a corrupt file fails closed;
  - a write failure, simulated with a read-only directory, returns an error and leaves the old file intact.

**C3. `runtime/ydb_clients.{h,cpp}`.** Depends on [admin](rework_plan-admin.md) A1.
- Endpoint-pinned clients, reusing `MakePinnedClientSettings` (exact authority, discovery off, empty database, inherited TLS and credentials).
- One function per RPC with the exact settings from [admin](rework_plan-admin.md) A7 and [healthcheck](rework_plan-healthcheck.md) H4.
- `TRedactor`.

**C4. `runtime/skipper.{h,cpp}` (`TSkipperActor`).** Depends on C2, C3 and the domain tasks.

Each tick does the following, in order:

1. Schedule the due state polls and health rounds.
2. Fold the completed replies into the per-endpoint latest-reply maps.
3. Select the view ([admin](rework_plan-admin.md) A3), and update and persist the high-water mark.
4. Compute verdicts ([healthcheck](rework_plan-healthcheck.md) H3).
5. Update confirmations and decide ([policy](rework_plan-policy.md) P1–P3).
6. In `auto` mode with a `Ready` decision, follow the mutation protocol ([admin](rework_plan-admin.md) A6).
7. Publish the snapshot and metrics.

The actor also resolves the pending mutation whenever a newer view arrives.

**C5. `main.cpp`.** Depends on C1–C4.
- Parse arguments, load configuration and credentials, then open the state file.
- Build the actor system (one pool), the driver, `TSkipperActor` and the HTTP actor.
- Install the signal handlers and follow the shutdown order above.

**C6. Delete the replaced code** (see Removals).
- Update `ya.make` (`RECURSE`/`PEERDIR`): drop `durable`, plus `contrib/libs/sqlite3` and `contrib/libs/openssl` if they become unused. Keep `acceptance`; [mock](rework_plan-mock.md) M5 rewrites its contents.
- Remove the tests that cover deleted code. The test lists are in each module plan.

## Acceptance criteria

- `ydb/apps/bridge_skipper` builds with no reference to `durable/`, SQLite, `TCompletionGate` or `TGatewayBudget`.
- `--check-config` rejects every removed key and accepts `config/example.yaml`.
- Restart and pending mutations:
  - After a simulated crash between the pending-record write and the RPC, a restarted skipper reloads the record.
  - If it then observes generation > G, it clears the record without operator action. This fixes C-G1 and C-G2.
- A restarted skipper that sees only a view below the high-water mark reports `STALE_HISTORY` and sends nothing.
- A hung `SelfCheck` on one endpoint (a mock silence fault) does not delay state polls, other endpoints or ticks. Tick age stays under 1 s.
- SIGTERM with a hung call outstanding exits within the driver stop time, with no use-after-free. Verify under ASan in one run.

## Focused regression tests (`runtime/ut`, fake `TYdbClients`)

1. A pending record at G, then a view at G+1: the record is cleared and the next decision proceeds.
2. A pending record at G, with the view still at G and a different plan: the decision is `PENDING_MUTATION`. With the identical plan, it is resent after the backoff.
3. A state-write failure before a mutation: the mutation is not sent and `STATE_WRITE_FAILED` is reported.
4. A view below the high-water mark: `STALE_HISTORY`. After `Success` at G, the persisted high-water is G+1, and a view still at G sends nothing.
5. Late replies after an endpoint's call has timed out do not overwrite a newer reply. Correlate replies by request sequence number.

The existing `controller_ut`, `gateway_ut`, `resources_ut` and `durable_ut` cases are deleted, except for these:

- `PinnedSdkClientsUseExactAuthorityAndInheritDriverTlsCredentials` (`gateway_ut.cpp:217`), moved to the `ydb_clients` tests;
- `DiagnosticLatency…` (`controller_ut.cpp:1641`), rewritten for the new actor.
