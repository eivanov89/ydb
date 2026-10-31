# Healthcheck requests and evidence

[Implementation index](IMPLEMENTATION-index.md)

This page traces SelfCheck from endpoint discovery to policy input. Read
[core](IMPLEMENTATION-core.md) for scheduling/configuration and the decision
algorithm, and [admin](IMPLEMENTATION-admin.md) for the surrounding Bridge reads.

## Source map

| Source | Relevant symbols and responsibility |
| --- | --- |
| [runtime/controller.cpp](runtime/controller.cpp) | `Poll`, `StartRound`, `ContinueChain`, `ReadCompleted`, `Diagnostic`, `CloseRound` |
| [runtime/events.h](runtime/events.h) | `TReadRequest`, `TReadResult`; owned RPC correlation and timestamps |
| [runtime/gateway.cpp](runtime/gateway.cpp) | `TSdkGateway::Read`, `TYdbGatewayActor::Read`/`ReadComplete`, `TGatewayBudget` |
| [domain/scheduler.cpp](domain/scheduler.cpp) | `VerifiedInventory`, `TBoundedScheduler`; fixed samples and outstanding chain capacity |
| [domain/evidence.h](domain/evidence.h) | Physical-group inventory, diagnostic input and observation contracts |
| [domain/evidence.cpp](domain/evidence.cpp) | `IsQualified`, `ParseDiagnostic`, `ValidateObservation`, `AggregatePile` |
| [domain/types.h](domain/types.h) | `TObservation`, `TProfileQualification`, assessment/source enums |
| [docs/qualification.md](docs/qualification.md) | Live integration limits and supported-build qualification requirements |

## Current live capability

`global_bsc_v1` and `static_whiteboard_v1` select domain evidence models. The
production SelfCheck adapter extracts identity and issue records but does not
populate expected physical-group inventory or normalized physical groups.
Its pile assessments remain UNKNOWN. `RuntimeOptions` supplies neither a
qualified profile record nor an `EvidenceDecoder` or `ProvenanceDecoder`.
`IsProductionAutoQualified` in [config/config.cpp](config/config.cpp) returns
false for every profile. Configuration and operator acknowledgement cannot
enable production Auto.

`nodewarden_local_v1` has no live collector or wire capability. Ordinary Health
requests for that profile return `UNSUPPORTED_DIAGNOSTIC_CAPABILITY` without a
SelfCheck call. Bootstrap can queue a follow-up identity-only Health request
that invokes SelfCheck for Manual topology discovery. Its response is labeled
`selfcheck_identity_v1` / `IDENTITY_ONLY_NOT_DECISION_EVIDENCE`.

Finishing the live assessment path requires both inventory/decoder integration
and real supported-build qualification. Qualified tests inject decoded
assessments and provenance; they validate controller behavior under those
assumptions. Rejoin's final health-dependent background obligation remains
pending in the shipped executable even after the pile becomes SYNCHRONIZED;
see [operations](IMPLEMENTATION-operations.md).

## Bootstrap identity probes

1. `Poll` combines configured seeds, supplementary discovery endpoints and
   topology endpoints. At each bootstrap interval it rotates through endpoints
   that lack a known node binding, offering at most `GlobalAdminConcurrency`
   Health requests. This interval is `RoundInterval`, initially 15 seconds.
2. The request uses `HealthClientTimeout`, initially 9 seconds, and carries a
   fresh request ID, process ID, current epoch and absolute deadline. Bootstrap
   probes do not produce a before/after-bracketed policy observation.
3. `Diagnostic` reads SelfCheck's top-level `location.id`, `host` and optional
   `pile.name`. Present empty pile names retain their identity; absent pile
   fields cannot be invented from a list position.
4. `ResolveIdentity` reconciles that identity with inventory, verifies the
   binding, and gives it to `TTopologyTracker::RememberVerified`. Identity
   changes discard endpoint replies and the previous node's observations.
   Endpoint/node uniqueness and readdress invalidation are detailed in
   [admin](IMPLEMENTATION-admin.md).

## A health observation round

`StartRound` requires confirmed topology and fresh coherent admin state. The
scheduler freezes sampled node IDs and the majority denominator separately for
each pile. Defaults are up to three sampled nodes per pile and three concurrent
chains per pile. Sample rotation changes the next round, not an active round's
denominator. A missing or busy sampled node stays in the sample.

Each launched node follows this sequence on its verified endpoint:

1. **Before:** Bridge `GetClusterState`, with `BracketAdminTimeout` (1 second).
2. **Health:** Monitoring `SelfCheck`, with the configured Health client and
   operation budgets (9 and 8 seconds).
3. **After:** Bridge `GetClusterState`, with the bracket budget again.

`ContinueChain` caps each timeout by the round's remaining time. Once a Health
start is known, it also caps continuation by that observation's original TTL.
The absolute round deadline is retained in each bracket request. The default
round deadline is 12 seconds, the interval 15 seconds and observation TTL
20 seconds; configuration validates their ordering in
[core](IMPLEMENTATION-core.md).

`ReadCompleted` checks request/process/endpoint/method correlation, rejects
obsolete read epochs, and stops chains on failure, lateness or epoch change.
The Health response must identify the exact sampled node and pile. The After
reply completes the observation only while its original round remains active.
Admin polling and configuration refresh continue independently of health chains.

`CloseRound` evaluates at the deadline. It supplies all completed observations
and the original frozen samples to policy. Unfinished chains are counted as
deadline misses; available surviving nodes can participate in later rounds
without waiting for a blackholed node. Scheduler reservations for an unsettled
chain persist until its completion is processed.

## SDK settings, timestamps and ownership

`TSdkGateway` reuses endpoint-pinned clients on one driver. Client settings use
an empty database, the exact bare host:port authority and discovery disabled;
driver TLS and startup credentials are inherited.

The SelfCheck call explicitly sets `ReturnVerboseStatus(true)`,
`ClientTimeout(timeout)` and `OperationTimeout(min(request.OperationTimeout,
timeout))`. Here `timeout` is the lesser of the request's relative timeout and
remaining absolute deadline. The adapter does **not** explicitly set merge,
cache-bypass or filtering options; their SDK/server defaults apply. Do not
document desired request flags as implemented flags.

`StartedAt` is captured immediately before the SDK call, after client lookup.
`FinishedAt` is captured when its future settles. Successful Health results
retain an owned protobuf copy and `ByteSizeLong` as `ResponseBytes`. Failed
SDK results preserve bounded/redacted status information. `Diagnostic` records
actual elapsed RPC latency, including failed responses, rather than batch age.

`TGatewayBudget` allows at most one outstanding `(endpoint, method)` request,
32 Health requests and 128 reads overall. Bracket admin reads have their own
32-slot class, separate from ordinary/admin-operation capacity. The controller
and per-pile scheduler impose additional bounds before the gateway.

Expiry does not release a gateway read slot while the underlying SDK future is
unsettled. Foreign callbacks own their result and send through
`TCompletionGate`; they do not retain actors or activation contexts. Gate
closure serializes with sends during shutdown. The control loop never waits
for a Health future. See [core](IMPLEMENTATION-core.md) for process teardown.

## Parsing and evidence validity

`Diagnostic` currently extracts issue ID, type, RED status, reason IDs and the
optional storage/group pile name. RED `HEALTHCHECK_STATUS` marks truncation;
the overall SelfCheck enum must be in the supported GOOD through EMERGENCY
range. Overall EMERGENCY alone is not a pile failure verdict.

The domain parser's stronger input contract is separate from that partial live
extraction. `ParseDiagnostic` requires release-qualified physical group IDs,
their expected pile mapping, complete coverage and known shapes. Duplicate or
unexpected group IDs, absent expected groups and attribution mismatches cannot
establish complete health. Red BRIDGE_GROUP/STORAGE_GROUP issue attribution
follows reason links, detects missing/cyclic references, and retains correctly
attributed child failures under a multi-child aggregate.

The parser starts every pile at UNKNOWN. Unqualified profiles, unsuccessful or
unsupported responses, truncation, mandatory unknown shapes and missing storage
data return UNKNOWN. A missing BRIDGE_GROUP issue is neither a failure report
nor proof of health. Fallback/GREY/missing-data normalization and complete
physical coverage must be supplied by a live decoder before that path can be
qualified; the current live adapter does not infer them from verbose groups.
NOT_SYNCHRONIZED failure assessments additionally require qualified resync
coverage and are never certified by the global BSC profile.

`IsQualified` checks profile/source agreement, capability presence, qualification,
supported-build and fixture records. Local-source profiles also require
independent origins. `ValidateObservation` then checks:

- Current process/epoch, verified responder node/pile and nonempty request ID.
- Both bracket fingerprints equal the current admin fingerprint.
- Before-to-Health delay is within `BeforeMaxAge` (the bracket timeout).
- Ordered timestamps, completion by the round deadline and freshness measured
  from the individual Health start, with TTL never extended by later delivery.
- For local sources, direct-local provenance and nonempty origin/vantage.

`AggregatePile` counts unique sampled responders against the frozen majority.
Missing and UNKNOWN replies do not shrink that denominator. Repeated BSC-backed
answers describe one shared source; the global policy must not count them as
independent local votes or stitch partial vectors into invented completeness.
The decision and confirmation rules belong to [core](IMPLEMENTATION-core.md).

## Review and validation map

When changing collection or parsing, trace the request, SDK settings, owned
result, identity binding, normalized coverage, bracket checks and policy input.
Preserve UNKNOWN and original timestamps through every layer. Update the
qualification matrix when capability or live decoder coverage changes.

| Test source / target | Relevant coverage |
| --- | --- |
| [domain/ut/domain_ut.cpp](domain/ut/domain_ut.cpp), `domain/ut` | `UnknownAndUnqualifiedNeverCertify`, physical coverage/issue attribution, bracket identity/TTL, fixed denominator, unsettled credits and surviving majority |
| [runtime/ut/controller_ut.cpp](runtime/ut/controller_ut.cpp), `runtime/ut` | Bootstrap identity, unavailable NodeWarden profile, identity/readdress invalidation, qualified two-round fixtures, production Auto blocking and Rejoin's qualified self-health requirement |
| [runtime/ut/gateway_ut.cpp](runtime/ut/gateway_ut.cpp), `runtime/ut` | Pinned clients, timeout capacity retained until settlement, bracket/admin capacity separation and callback-gate closure |

Metrics and HTTP diagnostic projections are described in
[metrics](IMPLEMENTATION-metrics.md) and [HTTP/acceptance](IMPLEMENTATION-http.md).
Use the active `ydb-build-test` skill for test execution; this guide records no
new test run or production qualification.
