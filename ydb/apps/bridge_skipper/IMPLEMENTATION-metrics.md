# Metrics, liveness and diagnostic histories

[Implementation index](IMPLEMENTATION-index.md)

Read this module when changing instrumentation, readiness presentation, incident
exports or alerts. The emitted-name catalog and operator interpretation live in
[Monitoring and audit](docs/monitoring.md); example rules are in
[alerts.yaml](docs/alerts.yaml).

## Source map

| Source | Entry points and responsibility |
| --- | --- |
| [http/actor.cpp](http/actor.cpp) | `THttpActor::Metrics` updates scrape-time gauges and encodes the shared counter tree as Prometheus text. |
| [http/metrics.cpp](http/metrics.cpp) | `UpdateHttpMetrics` projects the published snapshot, live stop latch and storage depth. |
| [http/schema.cpp](http/schema.cpp) | `ReadLiveness` computes controller age and displayed decision expiry independently of the controller actor. |
| [runtime/controller.cpp](runtime/controller.cpp) | `CloseRound`, `OnMutation` and actor `Flush` own round/confirmation/handoff measurements. |
| [runtime/gateway.cpp](runtime/gateway.cpp) | `ReadComplete` and `MutationComplete` measure actual SDK completions. |
| [runtime/resources.cpp](runtime/resources.cpp) | Storage counters, bounded process logs and emergency-stop audit. |
| [runtime/log_writer.cpp](runtime/log_writer.cpp) | Bounded asynchronous stdout writer and shutdown ownership. |

## Scrape and response-time liveness

`TRuntimeResources::Counters` is shared by runtime producers and the HTTP actor.
`GET /metrics` calls `UpdateHttpMetrics` before the Prometheus encoder visits
that tree. SDK/storage counters advance on completion events; scraping must
not increment invocation counts or collect new latency samples.

`ReadLiveness` compares `TSkipperClock::Now()` with the immutable snapshot's
`GeneratedAt`. Missing progress time or age greater than 5,000 ms means stalled.
For a displayed failover Decision, reaching `EvidenceExpiry` also marks evidence
expired. This check needs no response from the controller actor.

`skipper_authority_ready` masks snapshot authority readiness with the stall
check. `skipper_autonomy_ready` additionally masks expired decision evidence and
the current stop latch. `/api/state` uses the same liveness helper, while the
gateway separately enforces absolute dispatch validity; see
[core](IMPLEMENTATION-core.md) and [admin](IMPLEMENTATION-admin.md).

Requested mode, effective state and readiness have different meanings. Auto
mode can be requested while `skipper_autonomy_ready` remains zero. All current
production evidence profiles are unqualified, so this is an expected blocker.
HTTP responsiveness and a successful metrics scrape do not establish controller
progress, healthy storage or authority to dispatch.

## Measurements and their owners

| Family | Update point and limits |
| --- | --- |
| Round coverage and rejection | `CloseRound` records closed/deadline rounds, frozen sampled nodes, fresh contributors and decision rejection reasons. Counts are published in `TControllerSnapshot`. |
| Admission interval and evidence slack | A nonterminal gateway handoff event reaches `OnMutation`; the first handoff for an attempt produces one `THandoffSample`. `Flush` collects the millisecond histograms. |
| SDK reads | Gateway completion records count, status, elapsed time and returned payload bytes, including failed responses with a valid start time. |
| SDK mutations | Gateway completion records count/status and handoff-to-completion time. Redacted issue bytes are retained payload size, not transport response size. |
| Storage | The serialized executor records jobs, errors and elapsed time. Store statistics expose logical log/snapshot bytes; they exclude SQLite page/WAL overhead. |
| Workflow attention | Scrape-time projection counts incomplete operations beyond configured `attention_after_seconds`. Rejoin has no default attention duration. |
| Logging | Suppressed diagnostics and sink drops have separate counters. Neither is a durable audit sequence. |

Admission latency is round-close-to-SDK-handoff for evidence-bound work, not
HTTP request latency or journal commit latency. Remaining slack is the original
health evidence expiry minus actual handoff time. The controller records a
handoff once even if duplicate notifications arrive. `AdmissionBudget` overruns
are measurements; they do not create a second evidence lease or extend TTL.

The gateway's `method` and `status` metric labels use fixed sets, with unknown
statuses folded into `OTHER`. HTTP projection similarly bounds rejection-reason
labels. Endpoint URLs, node IDs, operation IDs and arbitrary RPC issue text
belong in bounded diagnostic context, not new metric labels.

RPC latency histograms use fixed buckets; mutation buckets extend farther than
read buckets. Controller admission/slack histograms have their own fixed
buckets. Keep units explicit: histogram names end in `milliseconds`, while
several cumulative/last-value counters use `microseconds`.

`skipper_rpc_payload_measurements_total` advances only for a nonzero exposed
payload size. A zero counter with no measurements means size was unavailable;
it does not establish a zero-byte gRPC response. Histograms permit approximate
interval quantiles. Sums divided by counts permit averages, not p99 or minimum
slack. Qualification needs raw per-attempt artifacts and pinned real execution.

## Logs and three separate histories

| History | Storage and cursor semantics |
| --- | --- |
| `/api/audit` | Durable command chronology, read through the storage executor; cursor is the applied command index. Caller identity is fixed `local`. |
| `/mon/local-stop` | Recent tail of separately fsynced `local-stop.audit`; process/revision context has no shared ordering with command indexes. |
| `/api/logs` | In-memory process-incarnation ring, with sequence, reset, overflow and pagination indicators. It is lost on restart. |

`TRuntimeResources::Log` accepts a bounded structured field allowlist, filters
severity, redacts suspicious values and repairs truncated UTF-8. Repeated
`read_rpc_result` or debug/trace diagnostics with the same event/method/status/
endpoint key are suppressed within five seconds. Transition records bypass
that rate limiter. The process ring uses `logging.display_ring_lines` (default
1,000); runtime log pages are capped at 200 records.

`TAsyncLogWriter` accepts owned JSON lines up to 8 KiB into a 512-record queue.
A transition may evict a queued diagnostic when full; a full transition-only
queue still drops new records. The writer owns a duplicated stdout descriptor.
The coordinator waits at most one second for writer shutdown, then detaches a
blocked writer whose shared state contains no actors. SDK and actor callbacks
enqueue records rather than writing stdout directly.

Startup failure is a separate bounded JSON line on stderr in
[main.cpp](main.cpp), emitted before or outside the ordinary runtime log path.

## Review and validation

1. Follow a new measurement from its producer through snapshot/counter ownership
   to scrape output. Keep event counts independent of scrape frequency.
2. Preserve the five-second read-time stall check and the live stop mask even
   when the controller pool is stalled.
3. Keep response bytes, retained issue text, wire bytes, averages and histogram
   quantiles distinct. Add a measured-presence signal for unavailable data.
4. Update the [metric catalog](docs/monitoring.md) and
   [alert examples](docs/alerts.yaml) when emitted names or meanings change.

Focused coverage is in [HTTP tests](http/ut/http_ut.cpp) for liveness, metrics,
fixed labels and projections; [controller tests](runtime/ut/controller_ut.cpp)
for once-per-handoff timing and freshness; and
[resource tests](runtime/ut/resources_ut.cpp) for log limits, redaction and
suppression. Select `http/ut` and `runtime/ut` through the active checkout's
`ydb-build-test` policy. This guide records no new test execution.
