# HTTP API and acceptance tooling

[Implementation index](IMPLEMENTATION-index.md)

Read this module when changing API schemas, signed previews, receipt replay,
projections or the mock/executable acceptance boundary. Bridge requests belong
to [admin](IMPLEMENTATION-admin.md); durable effects and recovery belong to
[operations](IMPLEMENTATION-operations.md).

## Source map

| Source | Entry points and responsibility |
| --- | --- |
| [http/actor.cpp](http/actor.cpp) | `THttpActor` adapts NHttp events, controller submissions and asynchronous audit reads. |
| [http/api.cpp](http/api.cpp) | `THttpApi::Handle`, `PlanAction`, `Bindings`, `EffectsHash` and signed envelopes. |
| [http/schema.cpp](http/schema.cpp) | Strict JSON, field schemas, UUID/decimal checks, response headers and read-time liveness. |
| [http/projection.cpp](http/projection.cpp) | State, operation, receipt and stop-receipt wire projections. |
| [acceptance/lib/harness.py](acceptance/lib/harness.py) | `Controller` and `MockEnvironment`: owned processes, signed plans and receipt workflows. |
| [acceptance/lib/contracts.py](acceptance/lib/contracts.py) | Shared direct Bridge wire assertions and mock fixture control. |
| [acceptance/main.py](acceptance/main.py), [report.py](acceptance/lib/report.py) | Mock/real recorder, provenance hashes, case results and explicit unexecuted gates. |
| [ydbd_mock/](ydbd_mock/) | Synthetic Bridge/SelfCheck/config/discovery services, replication, faults and supervisor controls. |

## Transport and input boundary

`main.cpp` registers the HTTP actor in a separate executor pool. Its handler
prefixes are `/`, `/api/`, `/metrics` and `/mon/`. The catch-all registration
does not serve a product page: `/`, former JS/CSS routes and unknown routes
return 404. Static assets and browser-test targets have been removed.

The listen address defaults to `127.0.0.1:8080`. Every client reaching this port
has authority for every route. There is no HTTP authentication, user identity,
Origin/Host authorization or separate privileged monitor port. YDB TLS/client
credentials protect the outgoing SDK connection independently.

Both the actor and `ParseStrictJson` reject bodies above 1 MiB, after the stock
listener has buffered the request. Strict JSON requires UTF-8, an object root,
at most 32 nesting levels and unique decoded map keys. Mutation schemas require
exact field sets and `application/json` (media type matched without case).
UUIDs have the 36-character hex/hyphen shape; this validator does not constrain
UUID version. Large indexes/generations use canonical decimal strings with
overflow checking, preserving uint64 values through JSON clients.

Request targets are bounded to 2,048 bytes. Query parsing rejects duplicate or
unknown keys and percent-encoded fields. API GET bodies and mutation query fields
are rejected; exact `GET /metrics` is handled directly by the actor adapter.
`JsonResponse` sets no-store and restrictive browser headers;
diagnostic errors are bounded/redacted.

## Routes and data ownership

| Route | Behavior |
| --- | --- |
| `GET /api/state` | Published cluster/topology/decision/diagnostic/authority projection plus response-time liveness. |
| `GET /api/requests/{request_id}` | Process-local stop receipt, durable command receipt or pending/absent result. Absence does not prove rejection. |
| `GET /api/operations`, `GET /api/operations/{id}` | Created-index pagination or one operation, including attempts and background obligations. |
| `GET /api/audit`, `GET /mon/local-stop` | Asynchronous storage reads of separate command/emergency histories. |
| `GET /api/logs` | Bounded process-incarnation diagnostic ring. |
| `GET /metrics`, `GET /mon/`, `GET /mon/controller`, `GET /mon/storage` | Prometheus counters or allowlisted read-only diagnostics; see [metrics](IMPLEMENTATION-metrics.md). |
| `POST /api/plans` | Pure preview of a manual workflow, `apply_decision` or `resume_workflow`; returns a signed envelope and blockers. |
| `POST /api/operations`, `POST /api/operations/{id}/resume` | Verify the preview and acknowledgements, then submit admission/Resume. |
| `POST /api/mode`, `POST /api/operations/{id}/cancel`, `POST /api/operations/{id}/resolve` | Durable control commands with mode/revision/recovery checks. |
| `POST /api/local-stop`, `POST /api/local-stop/release` | Independent process latch and marker protocol. |

`THttpApi` does CPU-only parsing/planning/projection work. It returns a
`TControllerSubmission` or an audit-read descriptor to the actor adapter.
Controller snapshots are immutable and contain only an installed durable
prefix. Projection adds served-at/age context without modifying that prefix.

Operation details and lists include `state_index`, the durable prefix that
produced the projection. Operation `revision`, controller revision, snapshot
revision and durable index have separate meanings; clients must not order all
responses solely by HTTP completion order.

## Preview to durable receipt

1. `POST /api/plans` calls `PlanAction` against the published state and stop
   latch. Planning uses the same `PlanWorkflow` as controller admission.
2. The signed canonical payload binds process incarnation, readiness generation,
   topology/controller/policy revisions, public-state fingerprint, normalized
   action, remaining effects and required acknowledgements. It uses HMAC-SHA256
   with a random process-local key of at least 32 bytes; envelope size is at
   most 64 KiB.
3. Ordinary Manual previews expire after 120 seconds. Evidence-bound previews
   additionally bind Decision ID, evidence/request IDs, candidate endpoint and
   original evidence expiry; the earlier expiry wins. Ordinary previews do not
   bind rotating health evidence.
4. On submission, UUID and method/target/canonical-body hash are checked against
   known durable/local receipts and pending requests before preview freshness
   checks. An unchanged duplicate returns its existing result; a changed body
   with the same UUID returns `REQUEST_ID_REUSED`.
5. A new request verifies signature, canonical encoding, expiry, current
   bindings, recomputed effects and acknowledgements. Remove requires the exact
   typed pile name and deletion acknowledgement, including explicit empty-name
   acknowledgement when applicable. Resume retains the original operation and
   remaining arguments; evidence-bound Resume requires matching fresh evidence.
6. The actor reserves a shared submission credit, records pending correlation
   and sends `TEvController::TEvSubmit`. The controller replans/rechecks current
   authority before proposing the durable command.

HTTP response waiting is limited to ten seconds. A timed-out or unknown
admission returns 503 `ADMISSION_UNKNOWN`; the waiter retains correlation and
credit until final completion. Replay means the same UUID and unchanged body,
not a new operation or an SDK resend. A committed operation receipt normally
returns 202; it acknowledges durable admission, not completion of the workflow.

Audit reads have a separate limit of 16 concurrent jobs and a ten-second
response deadline. Timeout returns `AUDIT_READ_PENDING` and retains the job
until settlement. Ordinary submissions share the runtime's 256-credit bound.
Local stop bypasses ordinary authority admission; its persistence and release
rules are described in [operations](IMPLEMENTATION-operations.md).

## Mock and acceptance layers

One mock process represents a pile with several per-node gRPC listeners.
`lib/model.py`, `validator.py`, `lifecycle.py` and `followups.py` model public
roles, hidden sync stages, overlays and asynchronous lifecycle transitions.
`cluster.py`/`replica.py` model peer snapshots, quorum preparation, stale or
detached replicas and persistence. `services.py`, `faults.py` and `health.py`
provide RPC envelopes, delay/silence/lost-reply controls, raw response overrides
and synthetic health. `supervisor.py` owns processes, manifests and finite
control actions. The model does not reproduce physical storage I/O or distconf
consensus; use the [mock contract](ydbd_mock/README.ai.md) for exact controls.

[acceptance/tests](acceptance/tests/) runs the actual C++ executable and SDK
against owned mock processes. Cases cover all eight workflow kinds, signed
admission/UUID replay, lost replies, real client timeout and no duplicate
delivered mutation. Rejoin deliberately retains its unqualified health
obligation after the mock reaches SYNCHRONIZED. Controller tests inject
qualified evidence to exercise the completion path separately.

The shared recorder runs eight direct public-wire contracts by default,
including omitted internal state, batch disconnect, stale receiver generation,
unsupported enum, abandoned promotion, lifecycle metadata, empty-name rejoin
and listener closure. These direct gRPC cases are separate from executable
HTTP-to-SDK workflows. Real mode requires explicit build/config/fixture and
recording provenance; hashes and per-case timing go into the report. Missing
cases are `not_run`. `Report` always emits `production_auto_qualified: false`.
See [acceptance/README.md](acceptance/README.md) for fixture/recorder schemas.

The local launcher [scripts/skipper.py](ydbd_mock/scripts/skipper.py) starts
owned mocks, supervisor and Skipper, advertises `/api/state` and control links,
and cleans up its processes. `ydbd_mock/e2e` is a separate LARGE compatibility
tier; retained CLI/demo classes need optional real CLI/interpreter prerequisites
and explicitly skip when those prerequisites are absent.

## Review and validation

1. Trace API changes through schema, preview bindings, canonical request hash,
   controller submission and durable replay. A stale preview must not hide an
   already committed receipt.
2. Keep HTTP timeout, admission outcome and external mutation outcome separate.
   Retain credits until final durable settlement even if the HTTP waiter left.
3. Keep production capability gates intact when adding mock fixtures. A passing
   fixture establishes the exercised path only.

Focused targets are `http/ut`, `runtime/ut`, `acceptance/tests`,
`ydbd_mock/tests` and, when compatibility/launcher behavior changes,
`ydbd_mock/e2e`. Use the active checkout's `ydb-build-test` policy. This guide
records no new test execution or real-cluster qualification.
