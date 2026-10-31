# Rework plan: health observation and per-pile verdicts

[Index](rework_plan.md). This plan covers the health round, the SelfCheck request, how SelfCheck replies and connectivity reports are classified, and the per-pile verdict that [policy](rework_plan-policy.md) consumes.

## Current behaviour and evidenced gaps

**Request.**
- `runtime/gateway.cpp:223` sets only `ReturnVerboseStatus(true)`, a client timeout and an operation timeout (8 s / 9 s).
- The live adapter `Diagnostic` (`runtime/controller.cpp:1350-1430`) extracts identity, the RED flag and the pile for each issue, and the RED `HEALTHCHECK_STATUS`.
- It never sets `MissingStorageData`, GREY, BLUE, `PILE_*`, or the expected groups.

**Rounds.**
- Each round freezes a sample of 3 nodes per pile.
- Each node runs Before-`GetClusterState` → `SelfCheck` → After-`GetClusterState` (`runtime/controller.cpp:558-620`, `1557-1596`).
- Replies are then aggregated by majority over a fixed denominator (`domain/evidence.cpp:166-237`), using profile-specific rules (`domain/policy.cpp:90-163`).

| ID | Class | Gap |
| --- | --- | --- |
| H-G1 | R/D | **Every live assessment is UNKNOWN.** No qualification or decoder is injected, and `ExpectedGroups` is empty (`domain/evidence.cpp:30-31`, `120-124`). |
| H-G2 | R | **A silently dead primary cannot be detected** (overview §3.2.2, §4.2.1). The survivors run in fallback mode with GREY disks, and connectivity is not read. |
| H-G3 | R | **Stage-2 `NOT_SYNCHRONIZED` parts are skipped** (`domain/evidence.cpp:125-127`). The server reports them as RED `BRIDGE_GROUP` (`ydb/core/health_check/health_check.cpp:3565-3591`), and stage-1 parts are excluded there (`:3569-3578`). |
| H-G4 | D | **One RED issue without a pile hides every pile.** Any RED group issue that resolves to no pile turns the whole reply UNKNOWN (`domain/evidence.cpp:108-111`). Non-bridged groups under non-uniform storage produce such issues (`health_check.cpp:3534-3539`), and so does an unknown pile name, which falls back to a numeric id (`:3556`). |
| H-G5 | R | **Fallback mode is not recognised.** The RED `STORAGE` issue "System tablet BSC didn't provide information" (`health_check.cpp:3720`) is ignored. Fallback replies are responder-dependent and not filtered by pile state (overview §3). |
| H-G6 | R | **BLUE "Replication in progress" counts as failure.** The group checker counts it as failed (`health_check.cpp:2711-2715`, `2987-2996`), and skipper cannot tell this apart (overview §3.1.4). |
| H-G7 | R | **No HEALTHY suppression after BSC returns** or after a primary or promoted change (overview §3.2.5). |
| H-G8 | P | **`return_verbose_status=true` brings RED issues closer to truncation.** `database_status` is cleared only when verbose is off, and that happens before truncation (`health_check.cpp:79-82`, `3982`, `3998`). |
| H-G9 | S | **`NoMerge` and `NoCache` are not set.** This is harmless today: the proto defaults give no merge and the cache is unused without a database (`ydb/core/grpc_services/rpc_monitoring.cpp:46`). Correctness should not depend on those defaults. |
| H-G10 | S | **Confirmation resets on unrelated metadata.** Brackets and confirmation bind to the full admin fingerprint, including pending error text and conversion (`domain/types.cpp:54-79`). A flapping `error_reason` could prevent confirmation indefinitely. |

## Intended behaviour and design

### Health round (H4, runs in `TSkipperActor`)

Every `round_interval_seconds`, for each pile with configured endpoints:

1. **Choose endpoints.** Pick up to `health_endpoints_per_pile` of them.
   - Rotate through the pile's endpoints.
   - Skip mismatched endpoints and endpoints that still have a call in flight for that method.
2. **Send two calls to each chosen endpoint, concurrently:**
   - `SelfCheck(ReturnVerboseStatus(false), NoMerge(true), NoCache(true), OperationTimeout(selfcheck_operation), ClientTimeout(selfcheck_client))`, with an empty database;
   - `GetClusterState(IncludePileConnectivity(true))`, with the read timeouts.
3. **Store replies.** Keep the latest reply per (endpoint, method), with `SentAt` captured immediately before the SDK call.

There are no Before/After brackets, no frozen samples and no round deadline. Evaluation runs on every tick over the latest valid replies.

### Validity of a reply for a decision at generation G

A reply counts only if all of the following hold:

- **Fresh:** `SentAt >= now - evidence_ttl`. Age is measured from the time the call was sent, never from delivery (overview §3.1.1).
- **After G was first seen:** `SentAt >= FirstSeen(G)` ([admin](rework_plan-admin.md) A3).
- **Responder at G:** the same endpoint's latest `GetClusterState` reply (plain or connectivity) reports G. This keeps the responder's state filter consistent with the view.
- **SelfCheck only:** `location.pile.name` matches the configured pile.
- **Connectivity only:** `report.generation == G` and `!binding_in_progress`.

**Why this replaces the brackets.**
- A decision at G is sent with `expected_generation=G`.
- If the cluster moved past G before the commit, the server returns ABORTED.
- Otherwise the state stayed at G throughout `[FirstSeen(G), commit]`, so every valid reply was produced while the state was G.

### SelfCheck classifier (H1): `ClassifySelfCheck(const Ydb::Monitoring::SelfCheckResult&)`

Output: `{Mode, Truncated, SizeWarning, map<pile, EPart>, UnattributedRed, Observations}`.

**Mode.**
- `Fallback` if any issue has type `STORAGE`, status RED and message exactly `System tablet BSC didn't provide information`.
- `Bsc` otherwise.
- The other "BSC didn't provide …" messages (`health_check.cpp:2566`, `2679`) do not select fallback.

**Truncation.**
- RED `HEALTHCHECK_STATUS` → `Truncated`.
- YELLOW or ORANGE `HEALTHCHECK_STATUS` → `SizeWarning` only.

**Per-pile parts.** For each RED `BRIDGE_GROUP`, or RED `STORAGE_GROUP` that carries `location.storage.pool.group.pile.name`, using the issue's own pile location (the server already copies single-reason piles to the parent, `health_check.cpp:3216-3237`):

| Mode | Result for pile P |
| --- | --- |
| `Bsc` | `RedPart(P)`, unless every non-GREEN VDISK reason, followed one level down, is BLUE "Replication in progress". Then it is `ReplicatingPart(P)`. |
| `Fallback` | `FallbackRed(P)`. A GREY `Group has disks with no data` with pile P gives `NoData(P)`. |

**Never pile verdicts.** These are kept only as observations:
- RED issues without a pile, or with a pile name absent from the view: counted in `UnattributedRed`;
- `PILE_STATE_STORAGE`, `PILE_SCHEME_BOARD`, `PILE_BOARD` and `BRIDGE_PILE`;
- the overall `self_check_result`.

### Connectivity summary (H2)

Map the report to `TConnectivity` ([admin](rework_plan-admin.md) A2). Each report is attributed to its **vantage pile**: the configured, SelfCheck-verified pile of the endpoint it was requested through ([admin](rework_plan-admin.md), endpoint inventory). "Inside P" means vantage pile P; "outside P" means any other vantage pile.

A pile counts as participating if either of these holds:
- its view state is PRIMARY, PROMOTED or SYNCHRONIZED;
- its view state is `NOT_SYNCHRONIZED` and some valid report shows it `in_config_quorum`. This is stage 2.

SUSPENDED piles are in the configuration quorum too (`ydb/core/base/bridge.h:88`), but they are not participating. They are evaluated only for the Lost rule below.

### Per-pile verdict (H3): `EvaluatePiles(view, replies, now, params) → map<pile, TPileVerdict>`

**Scope.** Evaluate participating piles with all the rules below. For a SUSPENDED pile, evaluate only rule 2 (Lost); otherwise its verdict is Unknown. BSC excludes suspended parts, so the other rules do not apply to it. A Lost SUSPENDED pile leads to `SUSPENDED_PILE_LOST` in [policy](rework_plan-policy.md). DISCONNECTED, INACTIVE and non-participating `NOT_SYNCHRONIZED` piles get no verdict.

**Inputs.** The valid SelfCheck replies are split by mode:
- `B` is the set of BSC-mode replies;
- `F` is the set of fallback replies.

Each reply is classified independently.

**Verdicts, applied in order (the first that matches wins):**

1. **Failed.** `B` is non-empty and every reply in `B` has `RedPart(P)`.
   - Unanimity is used because BSC is a single shared source (overview §3), not because it adds independence.
2. **Lost.** There is at least one valid connectivity report from outside P, and:
   - every such report shows P `in_config_quorum` (or `in_data_quorum`) with `!has_node_quorum`;
   - no valid report from inside P shows P with `has_node_quorum`.

   This covers a silently dead primary and a dead stage-2 pile. A partitioned pile that still reports its own quorum is not Lost.
3. **Replicating.** `B` is non-empty, every reply has `RedPart(P)` or `ReplicatingPart(P)`, and at least one has `ReplicatingPart(P)`.
4. **Healthy.** P is *clean* now and has been clean continuously for at least `bsc_warmup_seconds` (`now - CleanSince(P) >= bsc_warmup_seconds`). P is clean in an evaluation when:
   - `B` is non-empty;
   - no reply in `B` has a part for P;
   - no reply in `B` is `Truncated`.
5. **Unknown** otherwise. This includes mixed `B` results, an empty `B` (fallback only), and `NoData`.

**Extra outputs.** Each verdict carries:
- `FallbackRed`: true if any reply in `F` has `FallbackRed(P)`, used by policy decision D5;
- `SelfQuorum`: true if a valid report from inside P shows `has_node_quorum`;
- `NewestEvidenceSentAt`: the latest `SentAt` among the replies that support the verdict, used for confirmation ([policy](rework_plan-policy.md) P1);
- reason strings for display.

The evaluation as a whole also returns `BscAvailable`, which is true when `B` contains at least one reply.

**`CleanSince`** (decision D8) is per pile.
- It is set at the first clean evaluation and cleared by any evaluation where the pile is not clean.
- An evaluation with only fallback replies is not clean, so a visible BSC outage resets it.
- It is cleared for all piles when PRIMARY or PROMOTED changes in the view, because BSC moves then. It also starts empty at process start.

This covers a BSC restart that is never observed as fallback, for example one that completes between rounds:
- a pile whose disks are down was not clean before the restart;
- after the restart it looks clean only while BSC still reports missing statuses as GREEN;
- BSC turns those statuses into ERROR after about 15–20 s, below the 30 s default (overview §3.2.5).

A partition in which one side keeps answering in BSC mode does not reset the piles that side reports as clean.

**One RED group part is enough** (decision D3): a single `RedPart(P)` in every reply in `B` makes P Failed, as stated explicitly in overview §3.1.2.

## Removals

All of the following go:

- **Evidence profiles and qualification:**
  - profiles: `EEvidenceProfile`, `IsRecognizedProfile`, the `evidence.profile` key;
  - `ESourceKind`, `TProfileQualification`, `IsQualified`, `IsProductionAutoQualified`;
  - the `EvidenceDecoder`/`ProvenanceDecoder` hooks;
  - `Origin`, `Vantage`, `SnapshotId`, `DirectLocalOrigin`;
  - local-profile voting and `mutualPreference`;
  - `nodewarden_local_v1`.
- **Inventory and attribution:**
  - the physical-group inventory: `TPhysicalGroup`, `ExpectedGroups`, coverage checks;
  - the reason-graph walk with cycle detection (`domain/evidence.cpp:77-119`).
- **Round machinery:**
  - Before/After brackets, `BeforeMaxAge`, the bracket budget class;
  - `TBoundedScheduler`, frozen samples, majority thresholds;
  - `AggregatePile`, `CONTRADICTORY_GLOBAL_ASSESSMENT`.
- **Bootstrap identity probes.** Identity comes from each round's replies ([admin](rework_plan-admin.md)).

**Why each invariant still holds:**

| Invariant | How it is kept now |
| --- | --- |
| Never certify health on an unverified parser | One built-in classifier, pinned by fixture tests of real `SelfCheckResult` shape, plus explicit operator opt-in to `mode: auto`. |
| Healthy only with complete coverage | BSC mode enumerates every BSC group (`health_check.cpp:3646-3660`). Fallback mode never yields Healthy, and truncation blocks Healthy. |
| Missing replies must not shrink the evidence | Unanimity over `B` together with the Unknown default. An absent reply never yields Failed or Healthy on its own. |

**Kept:**
- the rule that `SentAt` is taken before the SDK call;
- one outstanding call per (endpoint, method), never released on timeout before the future settles;
- the replies' owned protobuf copies.

## Ordered tasks

- **H1.** `domain/health.{h,cpp}`: `ClassifySelfCheck`. Fixtures are protobuf text files under `domain/ut/fixtures/`, derived from `ydb/core/health_check/health_check_ut.cpp` scenarios: BSC RED part, stage-2 part, fallback with GREY no-data, fallback RED static part, BLUE-only group, truncation, unattributed RED. Pin the three message texts.
- **H2.** `TConnectivity` validity and participation helpers, in `domain/health.*`.
- **H3.** `EvaluatePiles` and the per-pile `CleanSince` tracking state, as a pure struct updated by the actor.
- **H4.** Round scheduling and SelfCheck settings in `TSkipperActor` and `ydb_clients` ([core](rework_plan-core.md) C3–C4). Depends on [admin](rework_plan-admin.md) A1.

## Acceptance criteria

- Against the mock with all piles healthy (after warm-up), every participating pile is Healthy.
- A dead synchronized pile becomes Failed, from BSC-mode RED. A dead primary becomes Lost, from connectivity, while the survivors report fallback.
- A dead stage-2 pile becomes Failed or Lost. A stage-1 pile gets no verdict.
- A fallback-only round never produces Healthy. An unattributed RED leaves the other piles' verdicts unchanged.
- The SelfCheck request carries `return_verbose_status=false`, `merge_records=false` and `do_not_cache=true`, with operation timeout < client timeout. Verified through the mock request log or a fake client.

## Focused regression tests (`domain/ut`)

1. Fallback plus GREY means Unknown, not Healthy. Fallback `FallbackRed` alone means Unknown (H-G5).
2. BSC RED `BRIDGE_GROUP` for b: b Failed, a Healthy. A single-reason ORANGE or RED parent with an inherited pile counts once.
3. An unattributed RED, or a numeric pile name, does not affect the other piles (H-G4).
4. A RED part that is BLUE-only gives Replicating. With one RED VDisk among the BLUE ones, it gives Failed (H-G6).
5. RED `HEALTHCHECK_STATUS`: no Healthy, but RED parts still count. YELLOW: a warning only.
6. `NOT_SYNCHRONIZED`:
   - BSC RED gives Failed;
   - `in_config_quorum && !has_node_quorum` gives Lost;
   - `!in_config_quorum` gives no verdict.
7. Lost requires that no report from inside P claims quorum, where a report is inside P when its endpoint's configured pile is P. A symmetric partition gives no Lost verdict on either side. A SUSPENDED pile is evaluated for Lost only.
8. Validity:
   - a reply sent before `FirstSeen(G)` is ignored;
   - a responder whose latest plain reply is at G−1 is ignored;
   - an expired reply is ignored even if it was delivered late;
   - a connectivity report with `binding_in_progress` or a different generation is ignored.
9. Warm-up:
   - Healthy needs `bsc_warmup_seconds` of continuous cleanliness.
   - A PRIMARY or PROMOTED change resets it, and so does a fallback-only evaluation.
   - A pile that turns from RED to clean, as after a BSC restart that was never observed, is not Healthy until the full window has passed.
   - A one-sided BSC partition does not reset the clean side.

**Deleted tests:**
- In `domain_ut`: `LocalStrictMajorityRetainsUnknownInDenominator`, `LocalMajorityChoosesBytewiseHealthyCandidate`, `TwoActiveLocalMutualPreferenceRequiresCompleteTopology`, `SharedLocalOriginCannotCountAsIndependentReporters`, `FixedTwoNodeSampleNeverShrinksDenominator`, `GlobalDoesNotStitchPerTargetMajorities`, `GlobalUsesNewestNecessaryQuorumForExpiry`, `LocalUsesFreshNecessaryQuorumsWithoutOlderExtraResponseExpiry`, `UnknownAndUnqualifiedNeverCertify`.
- In `controller_ut`: `UnavailableNodewardenProfile…`, `ProductionUnqualifiedProfileNeverDispatchesAuto`.

**Rewritten tests:**

| Existing test | Becomes |
| --- | --- |
| `PhysicalCoverageAndOrangeAggregatePreserveRedChildEvidence` | Test 2 |
| `ObservationBracketIdentityAndTtlAreHardGates` | Test 8 |
| `BlackholedNodeDoesNotSuppressFreshSurvivingMajority` | A hung endpoint does not stall evaluation |
| `SuspendedPendingAndResyncFailuresRequireOperator` | Test 6, plus [policy](rework_plan-policy.md) SUSPENDED |
