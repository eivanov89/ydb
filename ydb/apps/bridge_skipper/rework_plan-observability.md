# Rework plan: HTTP, metrics, logging and documentation

[Index](rework_plan.md). This plan merges the former HTTP and metrics modules. Under decision D2 the HTTP surface becomes read-only: one state document and one metrics page.

## Current behaviour and evidenced gaps

**HTTP API.** The API (`http/api.cpp:461-759`) has three groups of routes:

- **Mutation routes:** `POST /api/plans`, `/api/operations`, `/api/operations/{id}/{resume,cancel,resolve}`, `/api/mode` and `/api/local-stop[/release]`.
  - Previews are HMAC-signed with a per-process key (`http/api.cpp:398-427`).
  - Submissions are deduplicated by request UUID against durable receipts (`:616-641`).
- **Read routes:** `/api/state`, `/api/operations`, `/api/audit`, `/api/logs`, `/mon/*`, `/metrics`.
- **Metrics:** about 60 series, most of them projections of the snapshot taken at scrape time (`http/metrics.cpp:11-81`).

**Logging.**

- `TRuntimeResources::Log` (`runtime/resources.cpp:569-673`) applies a field allowlist, keyword redaction, a 5 s suppression window and an in-memory ring.
- The custom async writer (`runtime/log_writer.cpp`) carries the output.
- There are only 8 log call sites. The controller logs nothing.

**Gaps.**

| ID | Class | Gap |
| --- | --- | --- |
| O-G1 | D (security) | **Unauthenticated confused deputy.** Any client that can reach `http.listen` can call `POST /api/mode`, then `/api/plans`, then `/api/operations` with a failover of a healthy pile or `remove_pile`. These calls run with skipper's YDB admin credentials. The acknowledgements are fields the caller fills in, and there is no Host/Origin check (DNS rebinding). |
| O-G2 | D | **The preview binding is a local snapshot comparison, not a compare-and-set.** It cannot cover the gap between verification and RPC delivery (`http/api.cpp:681-684`; the RPC is unguarded). |
| O-G3 | D | **`read_rpc_microseconds_total` receives epoch-sized values.** It happens when `StartedAt` is zero (READ_DEADLINE, unsupported, SDK start error) (`runtime/gateway.cpp:151-155`, `215-220`, `270-274`, `432`). |
| O-G4 | D | **Metrics and alerts track the qualification gate, not cluster health.** `skipper_round_fresh_nodes` is always 0 and `SkipperRoundCoverageLost` fires permanently (`domain/evidence.cpp:142-143`, `docs/alerts.yaml:61-67`). |
| O-G5 | D | **The log field allowlist silently drops `reason`** and any new field (`runtime/resources.cpp:300` vs `:623`). Exact statuses are folded into `OTHER` in logs (`runtime/gateway.cpp:441,547`). |
| O-G6 | D | **`skipper_read_skipped` is a cumulative count exported as a gauge** (`runtime/controller.cpp:347,353`, `http/metrics.cpp:27`). |
| O-G7 | R | **No signal for unexplained state changes** (overview §3.3.5). There are also no per-pile state or verdict metrics. |
| O-G8 | D | **Every exception from `Run` is reported as `startup_failed`,** including failures at runtime (`main.cpp:145-148`). |

## Intended behaviour and design

### HTTP

`THttpActor` stays on the NActors HTTP proxy. It reads the published snapshot.

| Route | Response |
| --- | --- |
| `GET /api/state` | JSON, see below |
| `GET /metrics` | Prometheus, through the monlib encoder already in use |
| any other path | 404 |
| any non-GET method | 405 |

Every response sets `Cache-Control: no-store`. The default listen address stays `127.0.0.1:8080`. A non-loopback listen address is allowed: the surface is read-only, so the remaining risk is information disclosure.

The `/api/state` document is versioned with `"schema": 1` and contains:

- **Process:** `mode`, `now`, `loop_age_ms`, `cluster_id`.
- **View:** `generation`, `piles[{name, state}]`, `primary`, `promoted`, `pending_operation`, `conversion_in_progress`, `selected_from`, `age_ms`. Also `generation_high_water` and the view blocker, if any.
- **`endpoints[]`:** `{uri, pile, node_id, mismatch, state_generation, state_age_ms, selfcheck_mode, selfcheck_age_ms, last_error}`.
- **`piles[]` verdicts:** `{name, verdict, reasons[], fallback_red, self_quorum, connectivity[{vantage_pile, reporter_node_id, has_node_quorum, in_config_quorum, in_data_quorum}], clean_since, down_since, confirmed}`.
- **`decision`:** `{kind, reason, plan{expected_generation, update, endpoint}, cli_command}`.
- **`pending_mutation`** (from the state file) and **`recent_mutations`**: the last 16 invocations, each `{sent_at, endpoint, expected_generation, update, status, outcome, issues (redacted)}`.
- For each endpoint: `unattributed_red` and `observations` (the `PILE_*` issues).

### Metrics

All series use the `skipper_` prefix. Scraping never increments a counter.

**Gauges:**

| Metric | Labels |
| --- | --- |
| `skipper_loop_age_seconds` | — |
| `skipper_cluster_state_age_seconds` | — |
| `skipper_cluster_generation` | — |
| `skipper_generation_high_water` | — |
| `skipper_pile_state` (one-hot; series of removed piles are dropped) | `pile`, `state` |
| `skipper_pile_verdict` (one-hot) | `pile`, `verdict` |
| `skipper_mode` | `mode` |
| `skipper_decision` | `kind` |
| `skipper_blocked` | `reason` (bounded enum) |
| `skipper_pending_mutation` (0 or 1) | — |
| `skipper_pending_pile_operation` | — |

**Counters:**

| Metric | Labels |
| --- | --- |
| `skipper_rpc_total` | `method`, `status` (fixed allowlist, otherwise `OTHER`) |
| `skipper_mutation_total` | `outcome` (from [admin](rework_plan-admin.md) A5) |
| `skipper_decisions_total` | `kind`, counted on each transition |
| `skipper_unexplained_state_change_total` | — |

**Histograms:** `skipper_rpc_latency_milliseconds{method}`. A latency sample is recorded only when `SentAt` is set (fixes O-G3).

### Logging

**Backend.** Use the NActors logger (`LOG_*_S`) with a stderr backend: `ydb/library/actors/core/log.h`, `CreateStderrBackend`. Use a local component range registered through `NActors::NLog::TSettings::Append`, so that skipper does not depend on `ydb/core` service IDs.

**Info level** logs transitions only:

- startup summary;
- view generation change, with states and the `ExplainTransition` result;
- pile verdict change, with reasons;
- decision kind or plan change;
- mutation sent, with update, `expected_generation` and endpoint;
- mutation outcome, with the exact status and the redacted issues;
- pending record set or cleared;
- unexplained change (warning).

**Debug level** logs each RPC result.

**Redaction.** Only `TRedactor` is used: exact replacement of loaded secrets in RPC issue text. Credential material is never passed to the logger.

**Startup failure.** It stays one JSON line on stderr. Failures after startup are logged with their category: listen, state lock, state write or SDK (fixes O-G8).

## Removals

| Removed | Invariant and replacement |
| --- | --- |
| Signed previews, HMAC, `EffectsHash`, bindings, receipts, `Pending`, `ADMISSION_UNKNOWN`, submission credits, `/api/requests`, `/api/operations*`, `/api/mode`, `/api/plans`, Resume/Cancel/Resolve | **The applied change must be the one reviewed, applied only against the state it was reviewed on.** The server compare-and-set enforces this in the CLI command the operator runs, which is stronger than HMAC (O-G2). With no mutating routes, the confused-deputy problem goes away (O-G1). |
| `/api/local-stop[/release]`, `/mon/local-stop` | **Stop dispatch at once.** Stopping the process does this. There is no journal left to wedge (see [core](rework_plan-core.md)). |
| `/api/audit`, `/api/logs`, `TDiagnosticRing`, `THttpApi::Logs`/`Counters` (dead code, `http/api.cpp:765-818`), audit-read jobs | Replaced by stderr logs and the bounded `recent_mutations` list in the state document. |
| Strict JSON request parser, UUID and decimal schema helpers, query parser (`http/schema.cpp:16-149`) | There are no request bodies or queries. |
| `/mon/*` | Duplicated `/api/state`. |
| Liveness-gated admission and the `EvidenceExpired` and qualification masks (`http/schema.cpp:210-226`, `http/metrics.cpp`) | There is no admission left. Loop age is exported, and alerting is done in Prometheus. |
| Metrics for handoff, admission latency and slack, payload bytes, the duplicated `read_rpc_*`/`mutation_rpc_*`, storage/durable, constant limits, and round sample/fresh counts | Their producers are removed. The per-pile verdict and age gauges answer "is the skipper's picture current?". |
| `TAsyncLogWriter`, the field allowlist, keyword redaction, the suppression map, the ring | Replaced by the NActors logger and transition-only info logging. |

## Ordered tasks

- **O1. Snapshot.** Define `TSkipperSnapshot` (`runtime/skipper.h`) with the fields above. It depends on the domain types.
- **O2. HTTP.** Rewrite `http/` as `actor.cpp` (routing, 404 and 405) and `state_json.cpp` (snapshot to JSON). Delete `api.cpp`, `schema.cpp`, `projection.cpp` and `internal.h`.
- **O3. Metrics.** Rewrite `http/metrics.cpp`: counters are updated by the actor on events, and gauges are set from the snapshot at scrape time.
- **O4. Logging.** Set up NActors logging in `main.cpp`, add the transition log calls in `TSkipperActor`, and delete `runtime/log_writer.*` and the logging part of `runtime/resources.*`.
- **O5. Documentation.**
  - Rewrite `README.md`, `docs/operations.md`, `docs/monitoring.md` and `docs/alerts.yaml`.
  - The runbook covers: manual-mode workflow (copy the CLI command); switching to Auto; stopping; the `--clear-pending-mutation` and `state.json` overrides; lifecycle operations through `ydb admin cluster bridge` with `--expected-generation` and skipper in manual mode; and the minimum server version.
  - Replace `IMPLEMENTATION-*.md` with a guide to the new layout.
  - Delete `docs/qualification.md`, and record the validation gaps from the [index](rework_plan.md) in the README.

## Acceptance criteria

- Every `POST`, `PUT` or `DELETE` on any path returns 405. `GET /` returns 404.
- `/api/state` against the mock shows the view, verdicts, decision and CLI command. After an auto failover it also shows the mutation with its outcome.
- `/metrics` has no series whose value depends on how often it is scraped. Labels come from fixed sets only.
- Each decision transition produces exactly one info log line containing `kind`, `reason` and the plan.

## Focused regression tests (`http/ut`, `runtime/ut`)

1. Repeated scrapes leave every counter unchanged.
2. A read that completes with zero `SentAt` adds no latency sample (O-G3).
3. Gauges stay one-hot across state transitions, and a removed pile's series disappear.
4. Mutation outcome counters follow the [admin](rework_plan-admin.md) A5 table.
5. An unexplained transition increments the counter and logs once. An explained follow-up does neither.
6. In manual mode, the verdict and decision gauges reflect health. No gate masks them (O-G4).
7. JSON golden test of `/api/state` for a fixed snapshot.

**Deleted tests:**

- In `http_ut.cpp`: the preview, signature, receipt, acknowledgement, resume, local-stop, audit and mode tests (lines 252–838 as listed in the module review).
- In `controller_ut.cpp`: `AdmissionUnknown…` (`:605`), `DuplicateReceipt…` (`:642`), `…HttpResumeRebinds` (`:1065`) and `ChangedDecision…RejectsEarlierPreview` (`:1358`).
- In `resources_ut.cpp`: `RuntimeJsonRingBounds…` (`:178`) and `RepeatedDiagnosticsAreLimited…` (`:197`).

**Rewritten tests:**

- `MetricsUseActualCounters…` (`http_ut.cpp:903`) keeps its scrape-invariance and bounded-label assertions.
- `ReadTimeAges…` (`:453`) becomes the loop-age and state-age gauge test.
