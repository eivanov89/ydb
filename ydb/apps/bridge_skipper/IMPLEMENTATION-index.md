# Skipper implementation guide

Skipper is a standalone external controller for one YDB bridge cluster. It
observes public APIs, plans exact pile-state changes and journals operations
before handing mutations to the SDK. One process owns local authority; there
is no HA controller, lease or server-side fencing integration.

## Modules

| Read when working on | Guide |
| --- | --- |
| Process architecture, main loop, policy, configuration and timing | [Core](IMPLEMENTATION-core.md) |
| SelfCheck requests, identity, evidence parsing and qualification | [Healthcheck](IMPLEMENTATION-healthcheck.md) |
| Cluster/config discovery, topology and Bridge API requests | [Admin](IMPLEMENTATION-admin.md) |
| Durable state, attempts, ambiguity, recovery and local stop | [Operations](IMPLEMENTATION-operations.md) |
| Counters, liveness, logs and audit histories | [Metrics](IMPLEMENTATION-metrics.md) |
| HTTP schemas, signed previews, receipts, mock and acceptance tooling | [HTTP and acceptance](IMPLEMENTATION-http.md) |

## Architecture and reading order

1. Start with [main.cpp](main.cpp) and the core guide. `TControllerCore` owns
   deterministic controller state; its actor adapter publishes snapshots and
   routes effects to the SDK gateway and serialized storage executor.
2. Follow observations through admin and healthcheck. Configuration and responder
   identity establish topology; corroborated admin views and bracketed health
   rounds feed policy. A proposed decision is separate from permission to send.
3. Follow a change through HTTP, operations and admin: signed preview, durable
   admission, same-node preflight, committed intent, one-use live permission,
   final gateway checks, RPC outcome and background reconciliation.
4. Use metrics to check what each stage measures and which histories survive a
   restart. For a change spanning boundaries, read both modules and their focused
   test maps before delegating implementation.

## Current capability limits

The controller and HTTP API are implemented. Embedded UI/static assets and
browser-test targets have been removed. The two live SelfCheck profiles parse
identity/issues but lack expected physical-group inventory integration, so
assessments remain UNKNOWN. Production evidence qualification, live decoders
and qualified observation-acceptance capabilities are unset; Auto is blocked.
Qualified unit tests inject these capabilities. Rejoin retains its final health
obligation after SYNCHRONIZED. `nodewarden_local_v1` has no live capability.
Runtime identity binding also leaves hosts without configured pile labels
unresolved; see the admin guide's implicit-mapping limit.
Production Auto requires implementation work and pinned real-build qualification;
see [Support and qualification](docs/qualification.md).

The checkout's Bridge API/SDK/mock support `expected_generation`, but Skipper
leaves it unset. Its `UpdateClusterState` requests use the unguarded overlay
path. Application commit `dfe2861ba4d` is confined to this directory relative
to parent `4c65619e191`; the original task baseline `70498f8555f` predates other
server/SDK changes in this checkout.

## Review and validation context

These guides describe source inspected on 2026-10-31. They replace the old
execution diary; its implementation/review/test chronology remains in Git at
`dfe2861ba4d`, path `ydb/apps/bridge_skipper/IMPLEMENTATION.md`. Recorded passes
cover different historical checkpoints, including the removed UI. The diary's
final endpoint-binding regression had no recorded run; these documentation
changes add no build, test or production-qualification evidence.

Each module names relevant tests. Execute selected targets using the active
checkout's personal instructions and `ydb-build-test` skill; these guides do not
replace that policy. Operator procedures remain in the
[runbook](docs/operations.md), emitted metric names in
[monitoring](docs/monitoring.md), and fixture schemas in the
[acceptance guide](acceptance/README.md) and [mock contract](ydbd_mock/README.ai.md).
