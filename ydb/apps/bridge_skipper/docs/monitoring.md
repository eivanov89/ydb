# Monitoring and audit

Scrape `GET /metrics` through the same HTTP reachability boundary as the HTTP API;
it has no bearer credential. Inspect `/api/state` response-time liveness and
`/mon/controller`, `/mon/storage` for bounded read-only context. A responding
HTTP worker does not prove the controller is progressing.

## Counters currently emitted

These names correspond to the current HTTP/runtime counters. Keep alert rules
aligned with the deployed build's actual scrape. Configured operational
assumptions and diagnostic profile names do not imply metric measurements.

| Name | Meaning |
| --- | --- |
| `skipper_controller_progress_age_milliseconds`, `skipper_controller_stalled` | Response-time age of immutable controller progress; stalled above the five-second liveness gate |
| `skipper_authority_ready`, `skipper_autonomy_ready` | Local authority and derived Auto readiness, including read-time liveness/evidence masks |
| `skipper_local_stop` | Current process dispatch latch |
| `skipper_requested_mode{mode="Observe\|Manual\|Auto"}` | One-hot requested mode; its value may differ from effective readiness |
| `skipper_blockers`, `skipper_evidence_expired` | Present blocker count and read-time evidence expiry |
| `skipper_unresolved_operation` | Unresolved SDK dispatch uncertainty; separate from a slow accepted workflow |
| `skipper_ordinary_workflow`, `skipper_background_work` | Ordinary owner activity and incomplete accepted background obligations |
| `skipper_long_running_work` | Uncompleted operations beyond configured attention duration; default rejoin duration is disabled |
| `skipper_health_inflight`, `skipper_read_inflight`, `skipper_health_limit`, `skipper_admin_limit` | Active health/read requests and configured/runtime admission limits |
| `skipper_admission_depth`, `skipper_admission_rejected_total` | Pending command work and rejected HTTP admission |
| `skipper_storage_pending`, `storage_errors_total`, `storage_jobs_total`, `storage_microseconds_total` | Storage queue/activity/errors and cumulative job elapsed time |
| `durable_log_rows`, `durable_log_bytes`, `durable_snapshot_bytes` | Durable journal/snapshot logical payload size; monitor filesystem size/free space separately |
| `durable_checkpoints_skipped_total` | Checkpoints not written because the state exceeds the 256 MiB record limit; the previous snapshot and the complete log remain, so restart replays a longer tail |
| `inflight_reads`, `read_rpc_total`, `read_rpc_bytes_total`, `read_rpc_errors_total`, `read_rpc_microseconds_total` | Gateway read concurrency, counts, returned bytes, errors and cumulative elapsed time |
| `inflight_mutations`, `mutation_rpc_total`, `mutation_rpc_errors_total` | Gateway mutation concurrency, attempts and unsuccessful outcomes; failure alone does not prove rejection/nonacceptance |
| `dispatch_evidence_slack_microseconds_total` | Cumulative remaining evidence slack at mutation handoff; not a percentile or per-attempt minimum |
| `skipper_round_closed_total`, `skipper_round_deadline_total` | Closed rounds and rounds still unfinished at their configured deadline |
| `skipper_round_sampled_nodes`, `skipper_round_fresh_nodes` | Last closed round's frozen sample and fresh contributing node counts |
| `skipper_evidence_rejected_total{reason}` | Rejected decisions under a fixed reason allowlist; other reasons fold into `OTHER` |
| `skipper_admission_budget_overruns_total`, `skipper_handoff_freshness_misses_total` | Actual round-close-to-handoff budget overruns and refused stale handoffs |
| `skipper_round_close_to_handoff_microseconds`, `skipper_handoff_slack_microseconds`, `skipper_handoff_slack_observed` | Last measured admission interval and remaining evidence slack; presence is explicit |
| `skipper_admission_latency_milliseconds`, `skipper_handoff_slack_milliseconds` | Fixed-bucket histograms collected once per real evidence-bound SDK handoff, rather than at scrape time |
| `skipper_rpc_total{method,status}`, `skipper_rpc_latency_milliseconds{method,status}` | Completed SDK calls and measured callback latency under fixed method/status labels; unrecognized status folds into `OTHER` |
| `skipper_rpc_returned_payload_bytes_total`, `skipper_rpc_payload_measurements_total` | Size of returned diagnostic/config payloads where the SDK exposes the payload; samples distinguish unavailable size from a measured payload |
| `mutation_rpc_microseconds_total`, `mutation_rpc_retained_issue_bytes_total` | Measured mutation callback time and retained redacted issue text bytes; the status-only mutation SDK does not expose transport response-wire size |
| `skipper_pending_lifecycle_errors` | Current coherent pending Add/Remove entries carrying an error |
| `skipper_log_suppressed_total`, `skipper_log_sink_dropped_total` | Rate-limited repeated diagnostics and bounded stdout sink overflow/unavailability |

`mode` is a bounded label, not a regex value literally emitted by the endpoint;
each row has one of the three listed values. Counter sums permit interval
averages when divided by counts. They cannot produce p99 latency, a slack
minimum, a per-target cause or a reliable application recovery time. Preserve
raw measurement artifacts for [qualification](qualification.md). Do not invent
missing latency histograms or assume the journal byte counter includes SQLite
page/WAL overhead.

The new fixed-bucket histograms support approximate `histogram_quantile` queries
over a selected interval; they are not raw timing artifacts or release
qualification. SDK completion latency includes client/transport time. Returned
payload byte counters do not claim TLS/gRPC framing bytes or measurements for
SDK status-only calls. Zero payload bytes with zero measurement samples means
unavailable, rather than a zero-byte wire response.

[alerts.yaml](alerts.yaml) is an optional Prometheus rule example using the
actual exported names. Its windows and thresholds require deployment calibration.
It does not page on the currently unqualified production Auto gate.

## Suggested operator alerts

* Page on a sustained unreachable HTTP boundary or
  `skipper_controller_stalled > 0`. Correlate with progress age and process,
  CPU/storage symptoms; HTTP can remain alive while control is stalled.
* Page on `skipper_unresolved_operation > 0` and follow the ambiguity runbook.
  Never automate a resend based on the alert.
* Page on an increase in `storage_errors_total`, exhausted filesystem capacity
  or sustained loss of `skipper_authority_ready`. Preserve state and inspect
  persistence/controller blockers before recovery.
* Alert on unexpected `skipper_local_stop > 0`; confirm marker durability and
  service restart policy before deciding whether explicit release is safe.
* Warn on sustained `skipper_evidence_expired > 0`, read RPC errors, skipped
  reads or admission queue growth. Inspect configured sample/deadline budgets,
  source coverage and missing nodes. Absence is UNKNOWN, not proof of failure.
* Interpret `skipper_autonomy_ready == 0` together with requested mode and
  blockers. It is expected for all current unqualified production profiles;
  avoid paging continually on that known release gate.
* Warn on `skipper_long_running_work > 0` with operation kind and current
  lifecycle/background context. Do not page merely because
  `skipper_background_work > 0` or a healthy rejoin has taken a long time.

Choose sustained windows and error-rate thresholds from measured deployment
behavior; none are certified by the local mock. Server progress has an explicit
five-second stall gate; workflow attention defaults are configuration values,
not estimated completion deadlines.

## Three different histories

`/api/audit` is the durable command chronology with decimal sequence cursors.
It records fixed caller `local`, request/operation correlation, intent/outcome,
mode/ownership and recovery context. Do not infer a separate authenticated user.

`/mon/local-stop?limit=50` exposes a bounded recent tail (at most 100 records)
of the separately fsynced `local-stop.audit` emergency chronology. It remains
independent of ordinary authority admission, and `/api/audit` links it. Its
process/revision context is not a command-audit sequence: indexes have no
combined ordering and cannot share cursors. Inspect `truncated` and preserve
the underlying file for complete incident history.

`/api/logs` is a bounded in-memory diagnostic ring, scoped to process incarnation.
It can reset or overflow and is not a receipt/audit substitute.
Do not store secrets, credentials or unbounded RPC payloads in incident exports.

Runtime logs also go to stdout as redacted JSON through a separate bounded
writer. Actor/SDK/storage producers enqueue owned records and never write the
output stream themselves. `logging.level` filters severity; repeated read
diagnostics with the same method/status/endpoint are limited to one per five
seconds. Mutation, admission, stop and persistence transitions bypass that rate
limit. The stdout queue holds at most 512 records, prioritizing transitions over
queued diagnostics. Output backpressure can still drop records, counted by
`skipper_log_sink_dropped_total`; the process ring and durable audits retain
their independent semantics. The coordinator bounds writer shutdown and a
stalled sink keeps only its own queue/descriptor alive, without actor references.
