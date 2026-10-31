# Rework plan: Bridge API, cluster view, endpoints and mutations

[Index](rework_plan.md). This plan covers:
- the SDK extension for pile connectivity;
- reading and selecting the cluster view;
- the endpoint inventory;
- building and validating the update;
- the guarded mutation protocol and how its outcome is classified.

The decision itself is in [policy](rework_plan-policy.md).

## Current behaviour and evidenced gaps

| ID | Class | Finding |
| --- | --- | --- |
| A-G1 | R/D | **`expected_generation` is never set** (`runtime/gateway.cpp:294`), contrary to overview §3.3.2 and §4.1. A timed-out failover can apply later on top of newer state, for example after the pile has been rejoined. |
| A-G2 | D | **No outcome other than SUCCESS can be told apart.** `GenerationMismatchQualified` and `PinnedLifecycleErrorQualified` are never set, and observation acceptance is disabled (`controller.h:131-134`, `controller.cpp:935-990`). ABORTED and the lagging-node UNAVAILABLE both become Ambiguous, then NeedsAttention. |
| A-G3 | R/D | **Implicit-pile hosts are always rejected.** The binding check is passed `implicitMappingQualified=false` (`runtime/controller.cpp:1333`, `domain/scheduler.cpp:81-84`). In a converted cluster no node is ever bound. The server resolves the implicit pile in SelfCheck `location.pile.name` (`ydb/core/health_check/health_check.cpp:3990-3993`, `ydb/core/base/bridge.cpp` `ResolvePile`). |
| A-G4 | D | **Ordinary config-rollout skew blocks emergency dispatch.** Any two differing sampled configs set `CompetingChange` (`domain/topology.cpp:189-191`, `324-326`). One fresh reply with a different pile list blocks the admin view (`domain/evidence.cpp:262`). |
| A-G5 | D | **Pending-operation and conversion changes at the same generation block the admin view** (`LIFECYCLE_METADATA_RESAMPLE`, `domain/evidence.cpp:279`). Overview §4.1 calls these normal progress. |
| A-G6 | D | **A newer reply from a single node blocks a majority view** (`UNCORROBORATED_HIGHER_GENERATION`, `domain/evidence.cpp:317`). This is unnecessary once the generation guard is in place: a node that lags is answered with UNAVAILABLE, and one at a newer generation with ABORTED. |
| A-G7 | R | **The C++ SDK cannot request or return the connectivity report.** The proto has it (`ydb/public/api/protos/draft/ydb_bridge.proto:20,37-81`) and the server implements it (`ydb/core/grpc_services/rpc_bridge.cpp:455-490`), but `TGetClusterStateSettings` and `TGetClusterStateResult` do not expose it (`ydb_bridge.h:24,63-105`). |
| A-G8 | R | **`ValidateExactUpdate` accepts a new PRIMARY while another pile stays PROMOTED** (`domain/types.cpp:140-187`). The server also accepts this: `finalPromoted` keeps the old promoted pile (`ydb/core/grpc_services/rpc_bridge.cpp:327-329`). The pending promotion then later overrides the chosen primary (overview §4.1). The current failover planner avoids this (`durable/workflow.cpp:108-114`), but the validator should enforce it. |
| A-G9 | P | **Mutations disable the server operation timeout** (`UseClientTimeoutForOperation(false)`, `runtime/gateway.cpp:294-306`), against the overview §3.3.1 direction. |
| A-G10 | D | **The identity check binds by host name** when config hosts lack `node_id`. A dynamic node on the same host then alternates with the static one under the same logical id (`runtime/controller.cpp:1318-1322`, `domain/topology.cpp:263-270`). |

## Intended behaviour and design

### Endpoint inventory (decision D9)

- **Source.** Endpoints come only from configuration, keyed by pile name ([core](rework_plan-core.md) C1).
- **Verification.** Every SelfCheck reply's `location.pile.name` is checked against the endpoint's configured pile. A mismatch excludes the endpoint (`ENDPOINT_PILE_MISMATCH` in the snapshot) until a later reply matches. `location.id` is recorded for display only, because the configuration has no node IDs to compare it with. The implicit pile is handled by configuring the pile name that SelfCheck reports.
- **Vantage pile.** An endpoint's configured and verified pile is also the *vantage pile* of every connectivity report requested through it. The report is produced by the top of the binding component that contains this endpoint (`distconf_invoke_common.cpp:859-875`), so it describes what that endpoint's side can reach. `reporter_node_id` does not identify a pile, and the plan does not use it to derive one.
- **Reconciling with the cluster view.**
  - A pile that appears in the view but has no configured endpoint is shown as `no_endpoints`. It can still be a target (via other piles' evidence) and a candidate (BSC-mode health is a shared source).
  - Configured endpoints of piles that are absent from the view are ignored, with a warning.
  - Piles are matched by name only (overview §4.1).
- **FetchConfig and ListEndpoints are dropped.** FetchConfig cannot supply gRPC endpoints (`domain/topology.cpp:107-115`). ListEndpoints is scoped to a database and lists database nodes only (overview §4.3).

### Cluster view (A3)

Every `state_poll_interval_seconds`, a plain `GetClusterState` is sent to each configured endpoint that has no such call in flight. The latest reply per endpoint is kept, with the time its request was sent; the connectivity call's reply also updates it. A reply is fresh while `SentAt >= now - evidence_ttl`. If there are no fresh valid replies, the result is `NO_VIEW`. Among fresh replies:

1. **Validate each reply.**
   - Pile names are unique.
   - Exactly one pile is PRIMARY, and at most one is PROMOTED.
   - Every state is known.
   - An invalid reply is excluded and reported.
2. **Choose the generation.** `G = max(generation)`.
3. **Detect conflicts.** If two fresh replies at G disagree on any named state, the result is `VIEW_CONFLICT`. Pending-operation and conversion differences at G are not a conflict; take them from the newest reply at G.
4. **Check the high-water mark.** If `G < high_water`, the result is `STALE_HISTORY`. Otherwise raise `high_water` to G and persist it.
5. **Record first sight of G.** Track `FirstSeen(G)`, the time this process first selected G. [Healthcheck](rework_plan-healthcheck.md) H3 uses it.
6. **Report unexplained changes.** When a new G replaces the previous one, classify the transition for decision D10 (unexplained-change reporting). The following are explained:
   - the pending mutation's update, or the update of the mutation that last returned `Success` (kept in memory);
   - PROMOTED→PRIMARY together with the old PRIMARY→SYNCHRONIZED;
   - SUSPENDED→DISCONNECTED;
   - NOT_SYNCHRONIZED→SYNCHRONIZED;
   - no change in states.

   Anything else is "unexplained". A jump of more than one generation is "unverifiable".

`GetClusterState` with `include_pile_connectivity` is a separate call, made once per health round per sampled endpoint. Its failure does not affect the plain poll (`rpc_bridge.cpp:468-472`).

### Update validation (A4)

Keep `ValidateExactUpdate` (`domain/types.cpp:140-187`) and add the rule from A-G8: a request naming a new PRIMARY must explicitly set every other PROMOTED pile. The validator reduces requests that distconf would reject, but it cannot remove INTERNAL_ERROR, which also signals lost quorum (overview §4.5).

### Outcome classification (A5): `ClassifyUpdateOutcome(TStatus) → EMutationOutcome`

The meanings below were checked against `rpc_bridge.cpp:197-403`, `distconf_invoke_bridge.cpp:33-45` and `distconf_invoke_common.cpp:28-46,133-143`.

| SDK status | Outcome | Meaning |
| --- | --- | --- |
| `SUCCESS` | `Success` | The state has reached G+1 or later. The server may synthesise success after a forwarding hop disconnects, so this does not prove that this invocation caused the change. |
| `ABORTED` | `GenerationMoved` | The generation is no longer G. No invocation at G can commit any more. |
| `UNAVAILABLE` (server status, not `TRANSPORT_UNAVAILABLE`), `PRECONDITION_FAILED` | `NotAppliedHere` | Specific to the receiving node: it lags behind, has no bridge config (`rpc_bridge.cpp:202-206`, `229-233`) or has no cluster state (`:197-199`). This invocation was not applied. |
| `BAD_REQUEST`, `UNAUTHORIZED` | `Rejected` | Rejected before the invoke. Deterministic; needs an operator, for example to fix credentials or administrator SIDs. |
| `INTERNAL_ERROR`, `TIMEOUT`, `CLIENT_*`, `TRANSPORT_*`, `CANCELLED`, anything else | `Ambiguous` | The request may still commit. |

### Guarded mutation protocol (A6, decision D7)

The protocol runs in `auto` mode only, in `TSkipperActor`.

1. **Build the plan.** The [policy](rework_plan-policy.md) step yields `TPlan{G, update, endpoint}`. The endpoint comes from the pure function `SelectMutationEndpoint(view, endpoints, update, exclude)` (A5). It picks a configured endpoint that meets all of these:
   - it is in a pile that stays in the config quorum after the update, preferring the new primary pile (or the current one when the primary does not change);
   - its latest `GetClusterState` reply reports exactly G;
   - it is not mismatched and not in `exclude`.

   If none qualifies, the decision is `NO_MUTATION_ENDPOINT`.
2. **Check for a pending record at G.**
   - If its update differs from the plan, block with `PENDING_MUTATION`. Do not send.
   - If it is identical, resend according to the rules in step 5. The last send time and last outcome are kept in memory. After a restart, one immediate resend is allowed.
3. **Persist before sending.** Write `pending_mutation={G, update, first_sent_at}` before the first send. If the write fails, report `STATE_WRITE_FAILED` and do not send.
4. **Send.** Call `UpdateClusterState(updates, {}, ExpectedGeneration(G).OperationTimeout(mutation_operation).ClientTimeout(mutation_client))`.
5. **Handle the outcome.**
   - `Success` or `GenerationMoved`: the cluster has reached G+1 or later in this history. Raise `generation_high_water` to at least G+1 and clear the record, in one state-file write. Keep `{G, update}` in memory as the last success, for `ExplainTransition`. Views still at G are then `STALE_HISTORY`. This prevents both a duplicate send and an opposite failover via a side that has not caught up.
   - `NotAppliedHere`: keep the record (earlier invocations may still be in flight). Resend the identical update at once through `SelectMutationEndpoint` with this endpoint excluded.
   - `Ambiguous`: keep the record. Resend the identical update no earlier than `mutation_client_timeout` after the last send.
   - `Rejected`: keep the record, stop automatic resends until restart, and raise an error-level log and a metric. A different plan at G stays `PENDING_MUTATION`.
6. **Resolve from views.** On any view with generation greater than the record's G, clear the record. The guarded request can then never commit (overview §4.1).

**Operator paths.**
- `Rejected`: fix the cause, then restart. The restart permits one resend of the identical update.
- A record that can never commit, because its survivors lack quorum: verify the cluster state, then restart with `--clear-pending-mutation` ([core](rework_plan-core.md)).
- Both paths are logged.

**Why D7 is needed.** The overview's claim (§3.3.2) that a same-generation replacement means "only one wins" fails across a partition. A switch commits with the new configuration's quorum only (`distconf_invoke_bridge.cpp:195`, `mindPrev=false`) and does not require the scepter (`:29`). Resending the identical update is safe: every copy needs the same survivors' quorum.

### Request settings (A7)

- **Plain `GetClusterState`:** client timeout and operation timeout both `read_timeout_seconds`.
- **`GetClusterState` with connectivity:** the same timeouts, plus `IncludePileConnectivity(true)`.
- **`UpdateClusterState`:** as in A6, with an empty `quorum_piles` list. Do not call `UseClientTimeoutForOperation(false)`.
- **Removed RPCs:** `ConvertToBridge`, `AddPile`, `RemovePile`, `FetchConfig` and `ListEndpoints` are no longer called (decisions D1 and D9).

### Version requirement (decision D11)

Nodes without server commit `ab94a1660dd` ignore `expected_generation` (`ydb_bridge.proto:104`). The minimum version is a documented deployment requirement. Skipper does not probe for it.

## Removals

All of the following are deleted:

- **Topology corroboration:**
  - `domain/topology.*`: `ParseMainConfig`, `Corroborate`, `RememberVerified`, `TopologyRevision`, `CompetingChange`;
  - `ReconcileMembership`, `ResolveIdentity`, `VerifyBinding`;
  - the alias and readdress handling, `EBinding`;
  - the identity probes from bootstrap `SelfCheck` (`runtime/controller.cpp:1599-1691`, partly).
- **Admin view machinery:**
  - `SelectAdminView`'s majorities over frozen samples, `UNCORROBORATED_HIGHER_GENERATION`, `LIFECYCLE_METADATA_RESAMPLE`, `MEMBERSHIP_RECONCILIATION_REQUIRED`;
  - the quorum-timestamp aging;
  - `UnsupportedObservations` replay (`runtime/controller.cpp:478-516`);
  - the dead `Metadata` and `Progress` fields.
- **Preflight and synthesized prebridge state:**
  - the same-node preflight read and its rotation (`ScheduleWorkflow`, `CompletePreflight`, `PreflightTried`);
  - the raw `NOT_FOUND` prebridge synthesis (`runtime/controller.cpp:1433-1445`, `gateway.cpp:243-254`).
  - The receiving node's generation check and the root's re-check replace the preflight.
- **Workflows (decision D1):** `PlanWorkflow`'s seven non-failover workflows, the membership timeout, and the deletion and empty-name acknowledgements.

**Kept:**
- the failover overlay construction, moved to [policy](rework_plan-policy.md) P3;
- `ValidateExactUpdate`;
- `MakePinnedClientSettings`;
- `TRedactor`;
- the public-state enum handling for unknown values.

## Ordered tasks

**A1. C++ SDK connectivity.** A separate commit outside this directory, in the same style as `bab4be64648`.
- In `ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_bridge.h`:
  - `TGetClusterStateSettings::IncludePileConnectivity` (`FLUENT_SETTING_DEFAULT(bool, …, false)`);
  - the structs `TPileConnectivity{PileName, StaticNodes, ConnectedStaticNodes, HasNodeQuorum, InConfigQuorum, InDataQuorum}` and `TPileConnectivityReport{ReporterNodeId, ReporterIsRoot, BindingInProgress, Generation, Piles}`;
  - `const std::optional<TPileConnectivityReport>& TGetClusterStateResult::GetPileConnectivity() const`.
- Serialisation goes in `ydb/public/sdk/cpp/src/client/draft/ydb_bridge.cpp`.
- Add an entry to `ydb/public/sdk/cpp/CHANGELOG.md`.
- **Done when:** a round trip against the mock (after [mock](rework_plan-mock.md) M1) or against `rpc_bridge_ut` shows that the flag is sent and the report is parsed. If the report is absent, the result is `std::nullopt`.

**A2. Decoding.** In `runtime/ydb_clients`, after A1:
- `TGetClusterStateResult` → `TClusterView` (`SentAt` comes from the caller);
- `TPileConnectivityReport` → `TConnectivity`.
- Duplicate names or unknown states make the reply invalid. They are never silently fixed.

**A3. `domain/view.{h,cpp}`.**
- `SelectView(replies, highWater, now, ttl) → {view | blocker}`.
- `ExplainTransition(prev, next, pending) → {Explained, Unexplained, Unverifiable}`.

**A4.** Extend `ValidateExactUpdate` with the A-G8 rule.

**A5. `domain/mutation.{h,cpp}`.**
- `ClassifyUpdateOutcome`.
- `TPendingMutation` rules as pure functions:
  - `MayResend(pending, plan, lastSendAt, lastOutcome, now)`;
  - `ResolvedBy(pending, view)`;
  - `ApplyOutcome(state, outcome) → {new high-water, cleared?}`.
- `SelectMutationEndpoint(view, endpoints, update, exclude)`.

**A6.** Wire the mutation protocol into `TSkipperActor` ([core](rework_plan-core.md) C4).

**A7.** Apply the request settings in `runtime/ydb_clients` ([core](rework_plan-core.md) C3).

**A8. (Optional, separate CLI commit.)** Add `--expected-generation` to `switchover`, `failover`, `takedown` and `rejoin` in `ydb/public/lib/ydb_cli/commands/ydb_bridge.cpp`, with a changelog entry. Make `switchover` and `failover` clear an existing PROMOTED pile. Until this lands, manual mode renders the hidden `update` command.

## Acceptance criteria

- Every `UpdateClusterState` that skipper sends carries `expected_generation` equal to the generation of the view the decision used, and has an empty `quorum_piles`. Verified against the mock's request log ([mock](rework_plan-mock.md) M5).
- ABORTED and the lagging-node UNAVAILABLE never leave skipper in a state that needs an operator.
- After `Success` at G, `state.json` holds `generation_high_water >= G+1` and no pending record, and no further mutation is sent while views are at G.
- A timeout at G followed by an observed G+1 clears the pending record within one poll.
- A converted (implicit-pile) cluster with configured endpoints is observed and can fail over.
- Same-generation pending or conversion differences do not block. A same-generation state difference blocks with `VIEW_CONFLICT`.

## Focused regression tests

**`domain/ut`:**
1. **View selection:**
   - a newer reply from a single node is selected;
   - a same-generation state conflict blocks;
   - a same-generation pending-operation difference does not block;
   - a generation below the high-water mark blocks;
   - an invalid reply (two PRIMARY piles, duplicate name, unknown state) is excluded.
2. **`ExplainTransition`:** each automatic follow-up is classified as explained, an operator rejoin as unexplained, and a jump of two or more generations as unverifiable.
3. **`ValidateExactUpdate`:**
   - rejects a new PRIMARY with an uncleared PROMOTED;
   - rejects a mention of a SUSPENDED or INACTIVE pile;
   - rejects two stored changes unless all of them are DISCONNECTED.
   - Keep `ExactBatchPreservesOmittedAndDemotesAbandonedPromotion` (`domain_ut.cpp:241`) and `EmptyNameAndUnsupportedStateRemainDistinct` (`:386`).
4. **`ClassifyUpdateOutcome`:** every row of the A5 table, including `TRANSPORT_UNAVAILABLE` (Ambiguous) versus `UNAVAILABLE` (NotAppliedHere).
5. **`MayResend`, `ResolvedBy` and `ApplyOutcome`:**
   - a different update at G is refused;
   - an identical update is resent after the backoff following `Ambiguous`, and at once to another endpoint after `NotAppliedHere`;
   - `Rejected` stops resends;
   - any view above G resolves the record;
   - `Success` and `GenerationMoved` raise the high-water mark to G+1.
6. **`SelectMutationEndpoint`:** it never picks an endpoint whose latest reply is not at G, an endpoint in a pile being disconnected, or an excluded endpoint, and it prefers the new primary pile.

**Deleted tests:**
- topology, preflight, raw `NOT_FOUND`, membership and readdress tests: `domain_ut`, `controller_ut`, `gateway_ut`, as listed in each test file around the symbols removed above;
- all workflow tests in `durable_ut` except the failover vector cases, which move to the `policy` tests.
