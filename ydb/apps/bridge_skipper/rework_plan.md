# Skipper rework plan

Handoff for the implementing orchestrator. It covers the reasons for the
rework, the target design, the order of work and the open decisions. The
module plans linked below hold the concrete tasks.

**Analysis basis**
- Checkout `7dd5fe9aa7a` (branch `skipper_single_overengineered`). The
  application commit is `dfe2861ba4d`; the server/SDK baseline it sits on is
  `4c65619e191`.
- Inputs: the bridge control-plane overview
  (`my-env-private/ai/snippets/skipper_overview.md`, cited as "overview §N"),
  the `IMPLEMENTATION-*.md` guides, the source and tests of this directory,
  and the server/SDK code listed in each plan.
- Nothing was built or run. Every claim was checked by reading the source.
- Unless a path is given in full, it is relative to `ydb/apps/bridge_skipper/`.

## Executive summary

### Main problems

1. **Automatic mode can never act, and manual mode has no plan to show.**
   - Every live health assessment is UNKNOWN. Three separate gates cause this:
     - no evidence qualification is injected (`runtime/controller.cpp:1924-1932`, `217-234`);
     - `ExpectedGroups` is never filled (`domain/evidence.cpp:120-124`);
     - the policy blocks first with `UNQUALIFIED_EVIDENCE_PROFILE` (`domain/policy.cpp:47`).
   - Overview §3.3.4 requires that manual mode show the plan automatic mode would execute. Today there is no such plan to show.
2. **Mutations are unguarded.**
   - `UpdateClusterState` is sent without `expected_generation` (`runtime/gateway.cpp:294`).
   - About 3k lines of journal, intent, grant and acceptance-proof machinery compensate for this.
   - Even so, in production every non-SUCCESS outcome becomes Ambiguous: the qualifiers and acceptance proofs are never enabled (`durable/durable.cpp:467-530`, `runtime/controller.cpp:936-937`).
   - An Ambiguous outcome blocks all later mutations, including emergency failover, until an operator runs `ResolveExternal`.
   - A restart with an admitted intent has the same permanent effect (`runtime/ut/controller_ut.cpp:947-962`).
3. **The most important failures are not detectable, even in principle:**
   - **Silent primary loss (overview §3.2.2, §4.2.1).** BSC dies with the primary, and the survivors' SelfCheck falls back to GREY "no data". The GetClusterState connectivity report would show the loss, but the C++ SDK does not expose it.
   - **Failed stage-2 `NOT_SYNCHRONIZED` piles (§3.2.1)** are skipped (`domain/evidence.cpp:125-127`), and their presence blocks the whole decision (`domain/policy.cpp:320-326`).
   - **The BSC fallback mode, GREY, BLUE replication and the post-restart GREEN window** (§3.1.3, §3.1.4, §3.2.5) are not recognised.
4. **Implicit-pile clusters are blind.**
   - Hosts without `bridge_pile_name` are never bound (`runtime/controller.cpp:1333`, `domain/scheduler.cpp:81-84`).
   - In a converted cluster this means no samples, no admin view and no actions.
5. **The HTTP surface is an unauthenticated confused deputy.**
   - Any client that reaches the port can disconnect or remove piles using skipper's YDB admin credentials.
   - The signed previews do not change this (see [observability](rework_plan-observability.md)).
6. **The machinery is out of proportion to the job:**
   - about 19k lines of C++ (including tests), a SQLite command journal, an HMAC preview protocol, three audit histories, a custom log writer;
   - a 3-RPC bracketed health chain per node, majority voting over a source that is in fact single (BSC);
   - eight workflows and about 25 timing keys.

### Intended behaviour

Skipper is a single external **failover** controller for one bridge cluster.

**Each health round**, it reads two things from configured endpoints in every pile:
- `GetClusterState`, including the pile connectivity report;
- `SelfCheck`, with `verbose=false`, `NoMerge` and `NoCache`.

From these it classifies each participating pile as HEALTHY, FAILED, LOST, REPLICATING or UNKNOWN.

**A failover is planned** once these conditions hold:
- FAILED or LOST has been confirmed over time, at one cluster-state generation G;
- the survivor (or new primary) is healthy.

**The plan** is a single overlay:
- every failed pile set to DISCONNECTED;
- an explicit new PRIMARY and an explicit PROMOTED reset if needed;
- `expected_generation=G`.

**Modes:**
- `manual` displays the plan and the equivalent `ydb admin cluster bridge update ... --expected-generation G` command.
- `auto` sends the plan.

**Safety rests on four things:**
- the server compare-and-set;
- a persisted generation high-water mark;
- one persisted pending-mutation record, with the rule "only the identical update may be resent at G";
- the operational single-controller assumption (overview §3.3.5).

### Major removals

| Area | What goes |
| --- | --- |
| Durable journal | All of `durable/`: SQLite store, codec, operations, attempts, receipts, acceptance proofs, background obligations |
| Lifecycle workflows | Switchover, takedown, primary maintenance, rejoin, convert, add and remove. Operators use the YDB CLI for these |
| HTTP | Previews, HMAC signing, receipts and every mutating route |
| Local stop | Latch, marker and audit |
| Evidence model | Evidence profiles and qualification records; bracketed chains; frozen samples and majority denominators |
| Inventory | Config-majority topology corroboration and the readdress machinery |
| Runtime | Gateway actor, completion gate, storage executor and custom log writer |
| Acceptance tooling | Real-mode recorder |

Production qualification against real YDB builds stays a release activity and
is not a runtime flag.

## Target architecture

```
main.cpp ─ config/ ─ NActors system (one pool) ─┬─ TSkipperActor (runtime/skipper.*)
                                                  │    owns all mutable state, 250 ms tick
                                                  │    ├─ TYdbClients (runtime/ydb_clients.*): pinned SDK clients, request settings,
                                                  │    │    futures → Send(TEvReply) to the actor
                                                  │    ├─ TStateFile (runtime/state_file.*): flock + state.json
                                                  │    └─ pure domain/: view.*, health.*, policy.*, mutation.*
                                                  └─ HTTP actor (http/*): GET /api/state, GET /metrics, from a published immutable snapshot
```

**Responsibility boundaries**

| Layer | Owns | Must not |
| --- | --- | --- |
| `domain/` (pure; `now` passed explicitly) | View selection and conflict detection, SelfCheck and connectivity classification, per-pile verdicts, confirmation, decision, update validation, outcome classification, CLI rendering | Perform I/O, read clocks, use the SDK |
| `runtime/ydb_clients` | Endpoint-pinned SDK clients and the exact request settings for every RPC | Contain decision logic |
| `runtime/skipper` | Scheduling, at most one outstanding call per (endpoint, method), latest replies, invoking `domain`, sending the mutation, state-file writes, publishing the snapshot, logs and metrics | Block on a future |
| `runtime/state_file` | Persisting `{cluster_id, generation_high_water, pending_mutation}` atomically (fsync) | — |
| `http/` | Read-only rendering of the snapshot and counters | Mutate anything |

**Plan files.** The existing module boundaries map onto the plans as follows:
- core and operations → [core](rework_plan-core.md);
- admin → [admin](rework_plan-admin.md);
- healthcheck → [healthcheck](rework_plan-healthcheck.md);
- the decision part of core → [policy](rework_plan-policy.md);
- metrics and HTTP → [observability](rework_plan-observability.md);
- mock and acceptance → [mock](rework_plan-mock.md).

There is no separate operations plan. Its only surviving duties are the
pending-mutation record and the high-water mark (core) and outcome
classification (admin).

## Shared interface decisions

These are fixed across all plans. They live in `domain/types.h`, which is kept
but trimmed.

- `EPileState`, `TNamedPileStates` and `TExactUpdate` stay as they are (`domain/types.h:15-28`).
- `TClusterView {Generation, Piles, Pending (optional {Kind, Pile, Error}), ConversionInProgress, Endpoint, SentAt}`.
  - Remove `Metadata`, `Converted`, `PileOrder` and `Pending.Progress`.
- `TConnectivity {VantagePile, ReporterNodeId, ReporterIsRoot, BindingInProgress, Generation, SentAt, map<pile, {StaticNodes, ConnectedStaticNodes, HasNodeQuorum, InConfigQuorum, InDataQuorum}>}`.
  - `VantagePile` is the configured pile of the endpoint that was queried.
  - `runtime/ydb_clients` maps the SDK report into this struct ([admin](rework_plan-admin.md) A2, after A1).
- `EPileVerdict {Healthy, Failed, Lost, Replicating, Unknown}`, each with a list of reasons. "Down" means `Failed` or `Lost`.
- `TPlan {ExpectedGeneration, TExactUpdate Update, Endpoint}`.
- `TDecision {Kind: Stable | Unknown | Confirming | Blocked | Ready, Reason, optional TPlan, CliCommand}`. `Unknown` means that no pile is Down but some participating pile has no positive verdict (overview §3.2.6).
- `EMutationOutcome {Success, GenerationMoved, NotAppliedHere, Rejected, Ambiguous}`. The mapping from status is defined in [admin](rework_plan-admin.md) A5.
- State file `state.json`:

  ```json
  {"cluster_id": "...", "generation_high_water": "N", "pending_mutation": null | {"expected_generation": "G", "update": {"pile": "STATE"}, "first_sent_at": "..."}}
  ```

- Blocker reasons form a closed enum used by `/api/state`, metrics and logs:

  `NO_VIEW, VIEW_CONFLICT, STALE_HISTORY, PENDING_MUTATION, PRIMARY_UNKNOWN,
  NO_CANDIDATE, SUSPENDED_PILE_LOST, SURVIVOR_QUORUM_UNKNOWN,
  NO_MUTATION_ENDPOINT, STATE_WRITE_FAILED, INVALID_UPDATE`.

  There is no blocker for a pending Add or Remove. Add needs an INACTIVE pile and Remove a DISCONNECTED one, and state changes are refused while either is pending (`distconf_invoke_bridge.cpp:79-85`, `451-452`, `481`, `534`). So a participating pile can never be such a target.

## Implementation order

Write each step test-first: add the tests, see them fail, then implement.

1. **Prerequisites outside this directory.** Each is a separate commit.
   - a. **C++ SDK pile connectivity** ([admin](rework_plan-admin.md) A1). Blocks the policy work on primary loss and stage 2.
   - b. **Mock extensions** ([mock](rework_plan-mock.md) M1–M4): connectivity report, SelfCheck fallback mode, suspend follow-up fix, contract tests for the generation guard. Blocks the end-to-end tests only.
   - c. *(Optional)* `--expected-generation` on the named CLI commands ([admin](rework_plan-admin.md) A8).
2. **Pure domain layer.** Can run in parallel with 1b.
   - [admin](rework_plan-admin.md) A3–A5: view, update validation, outcome classification, mutation-endpoint selection.
   - [healthcheck](rework_plan-healthcheck.md) H1–H3: SelfCheck classifier, connectivity verdicts, per-pile verdicts.
   - [policy](rework_plan-policy.md) P1–P4: confirmation, decision, CLI rendering.
3. **Runtime** ([core](rework_plan-core.md) C1–C5, [admin](rework_plan-admin.md) A2, A6, A7, [healthcheck](rework_plan-healthcheck.md) H4): config, state file, `TYdbClients` and decoding, `TSkipperActor`, main and shutdown. Needs 1a and 2.
4. **Observability** ([observability](rework_plan-observability.md) O1–O4): snapshot schema, read-only HTTP, metrics, logging. Needs 3.
5. **Deletion** ([core](rework_plan-core.md) C6): remove the replaced directories and files, the dependencies and the `ya.make` entries. Do this as soon as nothing references them.
6. **End-to-end tests** ([mock](rework_plan-mock.md) M5–M6). Needs 1b and 4.
7. **Documentation** ([observability](rework_plan-observability.md) O5): rewrite the README, runbook, monitoring page, alerts and the `IMPLEMENTATION-*` guides. Delete `docs/qualification.md`.

## Policy decisions

These are unspecified by the overview. Each has a recommended default; the
module plans assume these defaults.

| # | Decision | Recommendation (consequence) | Plan |
| --- | --- | --- | --- |
| D1 | Scope | Failover only. Lifecycle operations go through `ydb admin cluster bridge`. Skipper tolerates INACTIVE, pending and converting piles. (The planned two-step primary maintenance becomes two operator commands.) | [policy](rework_plan-policy.md) |
| D2 | Operator surface | Read-only HTTP. `mode` comes from configuration and changes on restart. Manual mode shows the CLI command. (The emergency stop is "stop the process". Operators act with their own YDB identity.) | [observability](rework_plan-observability.md) |
| D3 | One RED group part ⇒ pile failed | Yes, after confirmation. (A single-group outage costs a full pile resynchronisation. Disconnecting is the only public remedy.) | [healthcheck](rework_plan-healthcheck.md) |
| D4 | Group RED only because of BLUE replication | REPLICATING, which acts like UNKNOWN. (No failover on replication alone.) | [healthcheck](rework_plan-healthcheck.md) |
| D5 | Survivor when the BSC host (PROMOTED, otherwise PRIMARY) is Lost and no BSC-mode reply exists | A synchronized pile, or the stored primary, is accepted when both hold: reports from its own pile show it has node quorum, and no fallback RED is attributed to it. (Its disk health is not verified until BSC restarts there.) | [policy](rework_plan-policy.md) |
| D6 | Partition (each side reports the other) | The side hosting BSC (PROMOTED during a switchover, otherwise PRIMARY) wins if its BSC-mode replies show the other side RED. Otherwise skipper blocks. | [policy](rework_plan-policy.md) |
| D7 | Same-generation replacement | While a pending mutation at G is unresolved, only the identical update may be resent. A different plan at G is `PENDING_MUTATION` until a generation above G is observed or the operator clears the record. `Success` or `ABORTED` raises the high-water mark to G+1, so no view at G can drive a second mutation. Skipper acts only when every Down pile is confirmed, so all failed piles are in one request. | [admin](rework_plan-admin.md), [policy](rework_plan-policy.md) |
| D8 | Confirmation and warm-up | Down must persist for at least `confirm_seconds` (20), with fresh evidence after the start of that window. Any non-Down evaluation or a generation change resets it. HEALTHY requires a pile to be clean in BSC mode continuously for `bsc_warmup_seconds` (30). Process start, a PRIMARY/PROMOTED change and any non-clean evaluation restart that window. | [policy](rework_plan-policy.md), [healthcheck](rework_plan-healthcheck.md) |
| D9 | Endpoint inventory | Endpoints are configured per pile name, and each one is verified against the SelfCheck `location.pile.name` on every reply. FetchConfig and discovery are dropped. (Inventory changes need a restart.) | [admin](rework_plan-admin.md) |
| D10 | Changes skipper cannot explain (§3.3.5) | Logged and counted, not blocking. The generation guard already handles correctness. | [observability](rework_plan-observability.md) |
| D11 | Server nodes that ignore `expected_generation` | Treated as a deployment requirement (minimum server version). No runtime probe. | [admin](rework_plan-admin.md) |
| D12 | SelfCheck `return_verbose_status` | `false`. This contradicts overview §4.2 (see below). | [healthcheck](rework_plan-healthcheck.md) |

## Disagreements between the overview, the guides and the source

- **Overview §3.3.2 says a replacement at the same generation "names it too, so only one of them wins".** That holds only within one coherent history.
  - The commit uses only the new configuration's quorum (`distconf_invoke_bridge.cpp:195`, `mindPrev=false`) and does not require the scepter (`:29`).
  - So two *different* updates at G, each disconnecting the other side, can both commit on opposite sides of a partition.
  - D7 is the client-side mitigation.
- **Overview §4.2 recommends verbose status.**
  - `RemoveUnrequestedEntries` clears `database_status` only when verbose is off, and it runs before truncation (`ydb/core/health_check/health_check.cpp:79-82`, `3982`, `3998`).
  - So verbose output pushes RED issues towards truncation. The classifier needs only `issue_log`.
- **Overview §4.1 says the connectivity report is requested with `include_pile_connectivity`.**
  - That is true on the wire, but neither the C++ SDK (`ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_bridge.h:24`, `:63-105`), the CLI nor the mock exposes it.
  - `main` at `4bb0978cfff` has the server side but also no SDK support.
- **Overview §4.1 cites the CLI for `--expected-generation`.**
  - The option exists only on the hidden `update` command (`ydb/public/lib/ydb_cli/commands/ydb_bridge.cpp:73`, `:96-101`).
  - `switchover`, `failover` and the other named commands send unguarded requests. They also do not clear an existing PROMOTED pile.
- **Overview §4.3 names FetchConfig as the source for node→pile mapping.**
  - Config hosts carry no gRPC port (`domain/topology.cpp:107-115`), so FetchConfig cannot supply endpoints.
  - The responder's own SelfCheck location resolves the implicit pile (`health_check.cpp:3990-3993`).
- **The guides call `IsProductionAutoQualified` the Auto gate.** It is read only by the HTTP projection (`http/projection.cpp:421,499`). The real gate is `TImpl::Qualification()` (`runtime/controller.cpp:217-234`).
- **The guides list "qualified definitive rejection" as a live outcome.**
  - The only recognised literal is "Unsupported pile state", which the server returns only for out-of-enum values that skipper never sends (`ydb/core/grpc_services/rpc_bridge.cpp:316-317`).
  - The other qualifiers are never set outside tests.
- **The server commits this rework depends on are not on `main`.** These are `expected_generation` (`ab94a1660dd`, `bab4be64648`), the fallback "no data" handling (`47c54ff9176`) and stage-2 visibility (`6d0f8ee7a75`). Merging them is external work.

## Material validation gaps

None of these can be closed by code in this directory.

- **Fallback behaviour during primary loss and partitions.** There is no recording from a real cluster of SelfCheck in fallback mode or of the connectivity report in these cases. The mock models both synthetically (M2). Policies D5 and D6 need such recordings before production Auto is enabled.
  - **Release gate.** This is external, release-time work.
    - Record SelfCheck and connectivity replies on a real cluster for four cases: primary loss, PROMOTED loss during a switchover, an A|B partition with BSC on each side, and a dead stage-2 pile.
    - Add the recordings as `domain/ut/fixtures` and run them through `ClassifySelfCheck`, `EvaluatePiles` and `Decide` ([healthcheck](rework_plan-healthcheck.md) H1, [policy](rework_plan-policy.md) P2).
    - It is complete when the fixtures are checked in, the tests pass and the README release checklist ([observability](rework_plan-observability.md) O5) records the build they came from.
- **BSC warm-up window.** The source gives a 20 s minimum: BSC's 15 s status expiry plus its 5 s sysview cycle. The 30 s default adds 10 s for publication and observation delay. Neither value has been measured.
- **Unversioned issue texts.** The classifier matches `"System tablet BSC didn't provide information"`, `"Replication in progress"` and `"Group has disks with no data"`. These are not a versioned contract (overview §4.2), so they must be pinned by fixtures for each supported release.
- **Timeout values.** The SelfCheck operation timeout (12 s, of which BSC gets half) and the confirmation timing have not been measured on a loaded cluster.
- **Exclusive controller.** Single-controller operation and placement in a third failure domain are operational requirements that skipper cannot verify (overview §3.3.5, §3.4).
