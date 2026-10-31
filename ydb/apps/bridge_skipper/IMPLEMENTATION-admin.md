# Bridge administration and topology

[Implementation index](IMPLEMENTATION-index.md)

This module follows inventory, Bridge reads and exact maintenance requests.
The [core guide](IMPLEMENTATION-core.md) owns scheduling and policy;
[durable operations](IMPLEMENTATION-operations.md) owns intent, ambiguity and
recovery. Health evidence is described in [healthcheck](IMPLEMENTATION-healthcheck.md).

## Source map

| Source | Entry points and responsibility |
| --- | --- |
| [runtime/controller.cpp](runtime/controller.cpp) | `Poll`, `ReadCompleted`, `ResolveIdentity`, `SelectAdmin`, `ReconcileMembership`, `ScheduleWorkflow`, `CompletePreflight`, `DispatchCommitted` |
| [runtime/gateway.cpp](runtime/gateway.cpp) | `TSdkGateway::Read`/`Mutate`, `MakePinnedClientSettings`, `ValidateLiveDispatch`, `ValidateDispatch` |
| [domain/topology.cpp](domain/topology.cpp) | `ParseMainConfig`, `TTopologyTracker::Corroborate`, `RememberVerified` |
| [domain/evidence.cpp](domain/evidence.cpp) | `SelectAdminView` |
| [domain/scheduler.cpp](domain/scheduler.cpp) | `VerifyBinding` and verified inventory selection |
| [domain/types.cpp](domain/types.cpp) | Public-state fingerprints, `ValidateClusterView`, `ValidateExactUpdate` |
| [durable/workflow.cpp](durable/workflow.cpp) | `PlanWorkflow`: eight workflows and exact request vectors |
| [durable/durable.cpp](durable/durable.cpp) | `BackgroundDone`, pending-work guards and completion postconditions |
| [Bridge request schema](../../public/api/protos/draft/ydb_bridge.proto) | Wire contract, including optional `expected_generation` |

## Inventory and responder identity

`Poll` starts independent configuration/discovery work at startup and each
`config_interval_seconds` (default 30 s). It rotates over at most four seed or
verified endpoints each time; in-flight capacity prevents a catch-up queue.

1. `FetchAllConfigs` retrieves YAML, with client and operation timeouts capped
   by the read request's remaining deadline (normally 10 s). The adapter rejects
   more than 64 MiB of returned configuration text.
2. `ParseMainConfig` reads `config.bridge_config.piles` and `config.hosts`.
   Pile names are exact strings, including the empty name. `node_id`, when
   present, is an inventory expectation. Otherwise host/interconnect-port
   information distinguishes logical hosts. The interconnect port is never
   converted into a gRPC port. A host containing a complete gRPC URI supplies
   an explicit endpoint; ordinary hostnames require discovery/seed probes.
3. `ListEndpoints` adds supplementary, scheme-checked endpoints, retaining at
   most 4,096 entries. It does not establish a node/pile binding or replace the
   configured inventory. The SDK call sets a client timeout.
4. Bootstrap health probes run at the round interval on unbound seed,
   discovered and configured endpoints. `Diagnostic` uses SelfCheck's own
   location to supply node ID, host and optional pile. `ResolveIdentity`
   requires exactly one matching inventory node and successful `VerifyBinding`.
   A missing pile differs from an explicitly empty pile name. The unavailable
   NodeWarden profile uses an identity-only SelfCheck fallback.
5. `RememberVerified` binds each endpoint to at most one verified logical node.
   A different responder at an occupied endpoint causes the old replies and
   observations to be forgotten. A configured address change discards every
   retained alias for that node. Replies remain keyed by endpoint and are
   attributed to node IDs only through the tracker's current bindings.

`ResolveIdentity` currently passes `implicitMappingQualified=false` to
`VerifyBinding`. Hosts without `location.bridge_pile_name` therefore remain
`IMPLICIT_PILE_UNRESOLVED`, even when SelfCheck returns a pile name. Domain
support for a qualified implicit binding is not enabled by the runtime.

The first parseable YAML provisions tentative topology. Confirmation requires
matching normalized configurations from a strict majority of one frozen pile
sample, matching Bridge membership, and no competing fresh sampled reply.
`Corroborate` uses a fixed 60 s configuration freshness limit. Its timestamp is
the newest necessary quorum's oldest reply timestamp; reusing cached replies
does not refresh it. Configuration timestamps currently use RPC completion time.

Lifecycle operations require confirmed, fresh, unconflicted configuration.
Emergency failover may use stale previously confirmed configuration while it
remains uncontradicted; it still requires fresh coherent admin evidence.

## Reading the Bridge state

`Poll` schedules `GetClusterState` at `admin_interval_seconds` (default 1 s).
It queries frozen sample endpoints and endpoints associated with unsettled
attempts or nonterminal operations. Admin reads also bracket health requests
and run as operation preflights. The SDK supplies both client and operation
timeouts. Ordinary polls leave the optional pile-connectivity report unset;
Skipper does not consume that report.

The decoder records generation, named pile states, display order, conversion
flag and pending Add/Remove metadata. Duplicate names or unsupported public
states/metadata cannot authorize mutation. `SelectAdminView` then:

1. Rejects stale/future timestamps, duplicate responder IDs and mismatched
   membership. Admin reply age currently starts at RPC completion.
2. Blocks different named states at the same generation (`STATE_CONFLICT`) and
   differing lifecycle metadata (`LIFECYCLE_METADATA_RESAMPLE`).
3. Requires one exact full fingerprint to have a fixed-sample majority in at
   least one pile, at or above the durable generation high-water mark.
4. Selects the highest corroborated generation and retains the necessary
   quorum's original validity timestamp. A fresh higher singleton blocks an
   older majority with `UNCORROBORATED_HIGHER_GENERATION`.

Membership expansion/removal uses `ReconcileMembership`: the original frozen
sample must corroborate both the candidate configuration and matching admin
view before either is installed. A previously unsupported admin observation
requires a newer coherent supported quorum before its blocker is cleared.

For an attested single-pile prebridge inventory only, Bridge `NOT_FOUND` is
represented locally as generation 0 with that pile PRIMARY. The gateway retains
the raw correlated `NOT_FOUND` preflight and permits it only for Convert.

## Workflow requests and observed completion

`PlanWorkflow` creates narrow overlays; it never sends a full state vector as a
refresh. Every plan requires external-prerequisite acknowledgement. Conversion
in progress blocks other plans. Add/Remove and promotion background obligations
also constrain otherwise disjoint plans; see the operations guide.

| Workflow | Request and principal guards | Background completion |
| --- | --- | --- |
| Failover | All failed mutable piles to DISCONNECTED in one update. If PRIMARY failed, a synchronized survivor to PRIMARY; explicitly demote any abandoned PROMOTED role. SUSPENDED targets are unsupported. Automatic NOT_SYNCHRONIZED targets require qualified coverage. | Exact requested public update is observed. |
| Switchover | Distinct SYNCHRONIZED successor to PROMOTED; explicitly clear another promotion. No pending membership work. | Successor PRIMARY and no PROMOTED pile. |
| Takedown | SYNCHRONIZED non-primary target to SUSPENDED. | Target DISCONNECTED after YDB's follow-up. |
| Primary maintenance | Current PRIMARY is the exact target. First promote a distinct SYNCHRONIZED successor; then suspend the former primary after the first postcondition and a fresh preflight. | First successor PRIMARY, target SYNCHRONIZED, no PROMOTED; then target DISCONNECTED. |
| Rejoin | DISCONNECTED target to NOT_SYNCHRONIZED. | Target SYNCHRONIZED plus fresh qualified self-health for that exact pile. |
| Convert | `ConvertToBridge` for an attested single-pile prebridge cluster. | Converted and `conversion_in_progress=false`. |
| Add pile | `AddPile` for an INACTIVE exact target; resources are prepared externally. | No pending operation and target DISCONNECTED. |
| Remove pile | `RemovePile` for a DISCONNECTED exact target; deletion acknowledgement, exact typed name and additional empty-name acknowledgement when applicable. | No pending operation and target INACTIVE. |

Only failover has automatic planning. The production evidence path is currently
unqualified, so Auto cannot dispatch. The live SelfCheck adapter cannot yet
establish complete physical inventory/health; Rejoin therefore retains its
final background obligation even after synchronization.

## From a planned step to one SDK call

1. `ScheduleWorkflow` selects a verified endpoint outside the piles being
   disconnected. Ordinary Manual preflights may rotate endpoints after a
   failed/busy read, before any mutation intent exists. Evidence-bound steps
   retain the decision's pinned endpoint and original evidence expiry.
2. `CompletePreflight` requires the exact current admin fingerprint, same
   endpoint, workflow revision, epoch, readiness and fresh lifecycle config
   where applicable. It reserves outcome capacity and proposes `AdmitAttempt`.
3. Only a committed and installed intent permits `DispatchCommitted` to issue
   an in-memory permission. It names the exact preflight, process, arguments,
   intent index, epoch and expiry. Expiry is the original preflight-start TTL
   for ordinary Manual work or the original diagnostic TTL for evidence-bound
   work. Admission-budget telemetry does not extend or shorten that validity.
4. The gateway validates current readiness and installed immutable attempt,
   baseline, ownership, local stop, workflow revision, confirmation generation,
   absolute config/admin expiry and dispatch margin. It rechecks immediately
   before SDK handoff. An ordinary poll cannot replace the named preflight.
5. The gateway consumes the attempt once. A failed pre-handoff check produces
   a designated-gateway `NeverDispatched` proof; an SDK call or synchronous SDK
   exception after consumption remains subject to conservative outcome handling.

The SDK uses one driver and per-endpoint clients. `MakePinnedClientSettings`
supplies the exact bare host:port authority, preserves IPv6 brackets, disables
discovery and inherits driver TLS/credentials. The database is empty. Startup
loads CA, client certificate/key and optional token once; credential contents
are excluded from durable state and redacted from issues.

`UpdateClusterState` sends only the planned updates and empty `quorum_piles`.
It leaves `ExpectedGeneration` unset, although this checkout's server, SDK and
mock support the optional guard. The preflight fingerprint is therefore a local
binding, not a server compare-and-set. Update/Convert use the configured 120 s
client timeout; Add/Remove use 600 s by default. Mutations explicitly disable
deriving a server operation timeout from that client timeout. A client timeout
does not establish cancellation, rejection or permission to resend.

## Review and focused coverage

For a changed request, trace `PlanWorkflow` → immutable attempt/hash →
`CompletePreflight` → installed intent → both gateway checks → SDK serialization
→ outcome and background postcondition. Preserve omitted internal states,
exact names, explicit role replacement and the original evidence expiry.

Start with [domain tests](domain/ut/domain_ut.cpp) for coherent admin selection,
fixed quorums, readdressing and `EndpointBindsAtMostOneVerifiedNode`;
[durable tests](durable/ut/durable_ut.cpp) for narrow workflow vectors and
deletion guards; and [runtime tests](runtime/ut/controller_ut.cpp) plus
[gateway tests](runtime/ut/gateway_ut.cpp) for membership reconciliation,
preflight rotation, raw prebridge `NOT_FOUND`, expiry and one-use handoff.
The corresponding targets are `domain/ut`, `durable/ut` and `runtime/ut` under
this directory. Follow the active `ydb-build-test` skill for execution.
