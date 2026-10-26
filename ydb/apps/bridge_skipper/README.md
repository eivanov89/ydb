# YDB Bridge Skipper

Skipper is a standalone external Bridge mutation controller with an HTTP API,
public YDB SDK clients and a local durable command journal.
The web UI has been removed pending a rewrite.
One active mutation controller owns a managed cluster. There is no HA mode.

To look around locally after building Skipper and `ydbd_mock`, run:

```bash
ydb/apps/bridge_skipper/ydbd_mock/scripts/skipper.sh
```

The launcher starts an isolated mock cluster and Skipper, prints HTTP links for
the Skipper state and mock control APIs, and stops its processes on Ctrl+C.
See the [mock quick start](ydbd_mock/README.md#skipper-with-local-mocks) for options.

The application uses existing public YDB APIs: Bridge `GetClusterState`,
`UpdateClusterState`, `ConvertToBridge`, `AddPile` and `RemovePile`, plus
SelfCheck, discovery and configuration fetch. It does
not add a YDB API, SDK method, NodeWarden collector or server-side diagnostic
extension. `nodewarden_local_v1` is a recognized but unavailable profile and
reports an explicit unsupported-capability gate. Pure local-policy fixtures
exercise model behavior without establishing a live diagnostic capability.

**Production Auto remains unqualified for every configured evidence profile.**
The current validation scope is this repository and its local mock. Successful
unit, mock or browser checks do not establish production storage correctness,
supported-release conformance, latency qualification or availability guarantees.
Manual operations also require operator preparation, exclusive administration,
fresh coherent state and the server's admission checks.

* [Operator runbook](docs/operations.md): launch, ownership, workflows, handover,
  local stop, ambiguity, restart and backup.
* [Support and qualification record](docs/qualification.md): actual diagnostic
  limits, unsupported cases and the unfilled production measurement template.
* [Monitoring](docs/monitoring.md): emitted metrics, suggested alerts and
  independent command/emergency-stop audit histories.
* [Configuration example](config/example.yaml) and
  [implemented mock](ydbd_mock/README.md).
* [Implementation status](IMPLEMENTATION.md) records the coordinating validation
  results and remaining implementation gates.

The binary accepts:

```text
bridge_skipper --help
bridge_skipper --version
bridge_skipper --config FILE --state-dir DIR --member-id ID --check-config
bridge_skipper --config FILE --state-dir DIR --member-id ID
```

`--check-config` parses configuration and reads credential files without opening
listeners, contacting YDB or changing the state directory. It does not prove
that credentials are authorized, a YDB endpoint works or the chosen directory
can be opened. Configuration is loaded once; changing it requires restart.

The listener defaults to `127.0.0.1:8080`. Every client that reaches its port
has authority for every HTTP route and mutation. There is no login, token,
session, role, Origin/Host authorization or separate privileged monitor port.
Choose the deployment's network boundary accordingly. Request bodies over
1 MiB are rejected with 413 `BODY_TOO_LARGE`; the check runs after the stock
NHttp listener has buffered the body, so it bounds parsing, not reachable-client
memory use. YDB SDK TLS/client
credentials are a separate connection boundary and never enter the browser.

Build/test execution follows the repository's active personal instructions and
the `ydb-build-test` skill. Use the prescribed `yamake` wrapper directly with a
release build, without editing source during compilation. This README does not
override those rules or prescribe a separate uncoordinated test runner.
