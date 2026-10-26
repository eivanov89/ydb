# Standalone acceptance

`tests` is a repository PY3TEST target. Its `DEPENDS` builds `bridge_skipper`
and `ydbd_mock`; `yatest.common.binary_path` supplies both executables. The
controller therefore invokes the actual C++ SDK against the supervisor
manifest's direct dial endpoints. No external virtualenv, Node, CDN or YDB CLI
is required. Use the active checkout's build/test instructions to run it.

The test starts independently owned mock processes and a supervisor HTTP
listener, uses signed HTTP plans and exact UUID receipt reconciliation, and
checks RPC/failpoint events plus replica state. It covers all eight workflow
kinds, simultaneous batch disconnect, empty-name Add/Remove, an intentionally
discarded HTTP admission reply, lost mutation replies before/after commit,
actual client timeout, and no delivered mutation replay. Deterministic
persistence-unknown admission is tested at the controller/storage seam; no
production persistence fault endpoint is added here.

Rejoin on the production executable deliberately retains its background
obligation after the mock reaches SYNCHRONIZED: a recognized profile is still
unqualified and cannot supply the fresh healthy evidence needed to finish.
Qualified modeled completion belongs to deterministic controller tests. The
production executable test also verifies that supplying synthetic SelfCheck health
does not enable production Auto.

The helper uses the existing public Bridge, SelfCheck, discovery and config
APIs with `global_bsc_v1`. The mock's synthetic per-pile blame is diagnostic
test data and never establishes an independently sourced or qualified BSC
assessment. No public API or SDK extension is required by this tooling.

The retained `ydbd_mock/e2e/test_cli.py` and `test_demo_compat.py` are a separate
compatibility tier with their original prerequisites. This target reports that
tier as not run; it does not silently claim an unavailable real CLI ran.

## Shared recorder

`skipper_acceptance` writes a versioned JSON report. Mock mode requires paths
to the two repository-built executables:

```text
skipper_acceptance --mode mock --controller-binary PATH --mock-binary PATH --build-id SOURCE_OR_BUILD --report REPORT.json
```

Real mode requires an explicit controller binary, config, build identity and
fixture manifest. It starts only Skipper; provisioning, failures, application
workload and captured recordings are supplied externally:

```text
skipper_acceptance --mode real --controller-binary PATH --config CONFIG.yaml --fixtures FIXTURES.json --build-id SUPPORTED_BUILD --report REPORT.json
```

The default mock recorder executes the eight named shared public-wire
contracts: omitted internal `_2`, batch disconnect, stale receiving-node
generation rejection, exact unsupported enum error, explicit abandoned
PROMOTED demotion, Add/Remove error/completion generations, empty-name rejoin,
and listener closure. These assertions live in `lib/contracts.py`; fixture
preparation is separate from assertions. Production controller workflows run
in the PY3TEST tier described above.

The existing public API projects `_1` and `_2` together as
NOT_SYNCHRONIZED. Its omitted-state case records the fixture's known internal
`_2` stage separately, checks ordinary public responses before/after the
unrelated update, and retains that internal stage as fixture evidence only.
The built-in adapter inspects mock control state; a real fixture must supply
its own release-pinned internal evidence. This does not create a public
storage-health signal. Empty-name rejoin reads ordinary GetClusterState.

The stale-generation fixture uses an authoritative disconnected pile and a
separate stale connected receiver. It leaves the receiver attached so its
local prepare can succeed, and preserves the authoritative exclusion so
ordinary anti-entropy cannot silently catch it up. The assertion requires the
exact generation-rejection issue and unchanged authoritative public state;
cleanup restores the receiver's authoritative generation and vector.

The fixture manifest has `schema_version: 1`, `source_build` equal to the CLI
build identity, `endpoints` exactly equal to the config's `ydb_endpoints`, a
nonempty `recordings` array of paths relative to the manifest, and `cases`.
Every recording and executable is hashed into provenance. No real run has
been performed by authoring this tool.

Each case contains `id` and either `shared_contract:true`, an HTTP workflow `action` object or a wire
`method`, exact configured `endpoint`, protobuf JSON `request`, and
`expect_operation` object. Wire expectations compare the supplied fields
recursively; list expectations compare exactly. Wire cases issue one generated
gRPC call with no retry. Workflow cases use the production C++ SDK controller,
signed previews, receipt reconciliation and expected completion. Optional
`failure_injection` records what the external operator/fixture injected.

Shared real cases require three `piles:[{name,endpoint},...]` entries, including
an exact empty name, and an explicit `fixture_control_url`. The external test
fixture actuator receives finite `/prepare`, `/followups`, `/stale_receiver`,
`/restore_receiver` and `/await_internal_state` requests and returns `{ok:true}` only after the
requested condition is established. `/prepare` receives the case ID and must
provide its known baseline; Add/Remove starts with the empty pile INACTIVE.
`/await_internal_state` also returns `fixture_evidence` containing the exact
pile, `stored`, `requires_data_quorum`, `requires_config_quorum` and a source
scope; these fields describe fixture evidence, not a Bridge API response.
This is an external test-infrastructure contract, not a production Skipper or
YDB API. The built-in mock adapter implements it through supervisor controls.
The shared adapter currently uses plaintext direct gRPC; certificate/TLS cases
use the credential-aware generated wire runner. Real implicit unlabeled-host
resolution requires separate recordings; empty-name success does not qualify it.

```json
{
  "schema_version": 1,
  "source_build": "release-pinned-build",
  "endpoints": ["grpcs://node.example:2135"],
  "recordings": ["capture/cluster-and-timeline.json"],
  "cases": [{
    "id": "exact_enum_error",
    "method": "UpdateClusterState",
    "endpoint": "grpcs://node.example:2135",
    "request": {"updates": [{"pile_name": "target", "state": 999}]},
    "expect_operation": {"ready": true, "status": "INTERNAL_ERROR", "issues": [{"message": "Unsupported pile state"}]}
  }]
}
```

Only provide mutation cases for a cluster prepared for those exact changes.
The recorder reports each I-09 contract ID absent from the scenario as
`not_run`. A scenario label alone is not a fixture qualification or evidence
of independent application/storage correctness. Reports always retain
`production_auto_qualified: false` and enumerate the external gates.

Report schema version 1 contains `execution_mode`, build/executable identity,
commands, hardware, topology, hashed fixtures, timed case results, explicit
failure injections, metrics, versioned differences and external gates.
Empty metrics mean no measurements were captured. Build/test command results
must be appended from the actual execution; this README claims no execution.

The retained CLI/demo/supervisor tier is registered at `ydbd_mock/e2e` as
LARGE. It uses the built standalone mock and explicit optional local
parameters `ydbd_mock_real_ydb` and `ydbd_mock_work`. The latter supplies the
existing `venv/bin/python` for the guarded shim and unchanged demo; the PY3TEST
executable is never mistaken for a Python interpreter. Missing CLI/interpreter
prerequisites skip the corresponding compatibility classes explicitly.
