# Support and production qualification

This checkout implements standalone controller machinery and observations
through existing public YDB APIs. It adds no YDB API/SDK or NodeWarden extension.
The current execution scope is local repository tests and the mock. Real YDB
scenario recordings and production load qualification remain pending. The
managed Bridge API is draft; pin both builds before any deployment assessment.

## Profile/build matrix

| Profile | Live adapter / domain model | Local/mock evidence | Real supported-build record | Production Auto |
| --- | --- | --- | --- | --- |
| `global_bsc_v1` | Domain parser/model for BSC-backed SelfCheck; repeated node answers share one source. The live SelfCheck adapter parses identity/issues but lacks pinned physical-group inventory and keeps assessments UNKNOWN | Parser/policy fixtures and modeled SDK flows; mock provenance must be declared explicitly | Pending live inventory integration and build recordings; no build certified | Blocked, unqualified |
| `static_whiteboard_v1` | Domain static/whiteboard parser/model. The live SelfCheck adapter does not derive expected physical-group inventory and keeps assessments UNKNOWN | Diagnostic fixtures establish modeled parsing, not complete live adapter coverage | Pending live inventory integration, coverage and failure recordings; no build certified | Blocked, unqualified |
| `nodewarden_local_v1` | Recognized profile name only; unavailable through the existing public API. There is no implemented collector, SDK extension or live diagnostic adapter | Pure independent-local policy/display fixtures test abstract behavior only; no matching live wire capability is exercised | Unsupported capability, with an explicit gate; no build certified | Blocked, unsupported and unqualified |

Recognizing a profile name does not establish capability. `IsProductionAutoQualified` currently
returns false for every profile. The durable Auto blockers must remain intact. An operator ownership or placement statement
cannot supply missing diagnostic capability, certify a release or open Auto.
For the two SelfCheck profiles, incomplete live inventory integration is an
implementation limitation in addition to the absence of production
qualification. Domain fixtures must not be presented as a working complete
release-qualified live assessment path.

## Diagnostic limits and unsupported conditions

* GLOBAL_BSC is one shared assessment, even when many piles return it. Complete
  contradictory vectors block action. Partial UNKNOWN replies cannot be stitched
  into invented completeness or treated as independent agreeing votes.
* On primary/BSC loss, ordinary SelfCheck may lack dynamic-storage coverage.
  Missing BRIDGE_GROUP is not evidence that a target failed. This path needs a
  separately qualified BSC-independent diagnostic before production Auto.
* Public NOT_SYNCHRONIZED hides the `_1`/`_2` distinction. An internally `_2`
  pile already participates in quorums. This checkout's SelfCheck evaluates
  stage-2 parts through BRIDGE_GROUP, with their logical-group contribution
  capped at ORANGE. Skipper still lacks a qualified live assessment path for
  these targets. Do not infer failure from omission or reissue NOT_SYNCHRONIZED
  to reset a hidden stage.
* No independently local NodeWarden diagnostic is exposed or collected by this
  implementation. The proposed I-02 extension is deferred and is not an
  installed prototype or measured capability. `nodewarden_local_v1` must remain
  unsupported. Missing responses and incomplete coverage remain UNKNOWN; a
  policy fixture cannot supply a real diagnostic or prove physical storage I/O.
* Skipper's local state fingerprint binds its observed complete public state;
  it is not a YDB mutation precondition or a server diagnostic receipt. The
  existing Bridge API supplies no diagnostic storage-config fingerprint for
  this application to substitute as an independent health signal.
* Unreachable SUSPENDED targets retain config-quorum obligations and are not
  a supported emergency Update target. YDB also constrains pending Add/Remove
  targets. A preview cannot promise that any proposed emergency can commit.
* Conversion is asynchronous. Do not admit Add/Remove until conversion is
  complete. Lifecycle work can progress without a generation increment;
  inspect complete named states and pending/error metadata, not generation alone.
* Add requires externally prepared resources. Remove irreversibly deletes a
  pile's storage-group parts. Directly forcing an unsynchronized primary,
  destructive split-brain repair, disk wiping and resource provisioning are
  operator procedures outside this controller.
* This checkout's UpdateClusterState API and SDK support the optional
  `expected_generation` precondition, but Skipper leaves it unset. Its current
  update path therefore has no server generation guard. The API has no
  request-ID deduplication, ownership lease or mutation-side fencing. One active
  mutation controller and disciplined handover remain deployment assumptions.
* Tablet drain and application recovery are separate checks. Long rejoin alone
  is not a failure, and no public synchronization percentage or reliable ETA
  is exposed.

## Qualification record template — pending

Create one reviewed record for each proposed supported profile/build/topology
combination. Empty cells are work remaining, not successful measurements.

| Field | Required evidence | Current record |
| --- | --- | --- |
| Identity | Skipper commit/build, YDB commit/release and feature capability/version | Pending |
| Environment | Exact topology, physical erasure groups, configured names including empty-name handling, active quorum piles, placement and clock assumptions | Pending |
| Diagnostic provenance | Redacted raw responses, responder identity, collection start/end, local/shared origins, complete inventory and coverage | Pending |
| Failure scenarios | Non-primary process/network/storage failure; primary/BSC loss; inter-pile split with third-site observer; conflicting complete views; internally `_2` failure; SUSPENDED and pending lifecycle targets | Pending |
| Outcome | Expected proposed action/blocker, exact request/state vector, RPC disposition, acceptance proof, observed postconditions and background completion | Pending |
| Missing data | BSC timeout, whiteboard fallback, truncated response, missing node/VDisk, stale sample, unsupported diagnostic and race cases | Pending |
| Scale | Positive pile counts, unequal node counts, sample sizes, zero-host inventory and capacity-budget limits | Pending |
| Load | CPU/RSS, response bytes, health/admin/diagnostic in-flight limits, per-round deadline/coverage, controller progress and service-manager restart behavior | Pending |
| Timing | Individual round-start/close, durable intent, SDK handoff and response timestamps; measured admission p50/p95/p99/max, stale-handoff misses and remaining evidence slack | Pending |
| Recovery | Crash before/after intent/SDK handoff, unknown HTTP receipt, replay with the same ID, restart with pending work and stop marker, external-recovery audit | Pending |
| Security | Verified TLS/admin certificate acceptance under degraded YDB; HTTP reachability boundary, plaintext-token policy and redaction checks | Pending |
| Review | Artifact hashes, procedures, reviewer, date, explicit supported/unsupported scope and release decision | Pending |

Record raw per-attempt timings to calculate percentiles. Counter sums/averages
cannot establish admission p99. Compare measured scheduling/admission worst
cases with observation TTL, round deadline, admission budget and dispatch
margin; keep the Auto gate closed if the required freshness bound is unproven.
All measurements must come from the pinned real environment; the mock's timing,
replication simulation and fault policy do not qualify physical storage.

Archive secret-free recordings alongside configuration/topology hashes and the
test procedure. Explicitly distinguish expected UNKNOWN from an explicit FAILED
assessment and record application checks independently of Bridge acceptance.
