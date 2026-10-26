# Standalone operator runbook

The web UI has been removed pending a rewrite. Use the HTTP API for the
operations below.

## Prepare and launch

1. Pin the Skipper and managed YDB build IDs and save the configuration and
   topology under change control. Consult [qualification](qualification.md)
   before interpreting a selected parser profile as a supported Auto scenario.
2. Choose a stable nonempty `cluster_id` and `member_id`. Use exactly one state
   directory and active controller for the cluster. The local directory lock
   only excludes another process using that directory; it cannot exclude an
   external CLI, another host or a different state directory.
3. Copy [example.yaml](../config/example.yaml), set explicit `grpc://` or
   `grpcs://` endpoints and the evidence profile, and arrange YDB credentials.
   All seed endpoints must use the same scheme. CA and certificate/key files
   configure the SDK connection; certificate and key must be supplied together.
   Verify administrator certificate authorization while YDB is healthy. A
   password/login service that disappears during an outage is unsuitable as
   the only emergency credential path.
4. Select `http.listen`. It is a plain HTTP, fully authorized surface for anyone
   reaching the port. Default loopback binding supports a local administration
   boundary. The application supplies no HTTP authentication or TLS listener.
   A plaintext YDB token requires explicit `allow_plaintext_token:true`; `/api/state`
   exposes that diagnostic. This flag does not protect the HTTP listener.
5. Make the state directory's parent available to the service account. The
   state directory must be an owned real directory with no group/other access
   (mode 0700); startup can create the final directory if its parent exists.
   Use local durable storage with working SQLite WAL, locking and fsync.
6. Run `--check-config` with the same configuration, member and state-dir
   arguments used for launch. Start the binary under a service manager with
   explicit restart policy, capture its version and inspect `/api/state` on the configured HTTP listener.

```text
bridge_skipper --config /etc/skipper/config.yaml --state-dir /var/lib/skipper/cluster-a --member-id skipper-a --check-config
bridge_skipper --config /etc/skipper/config.yaml --state-dir /var/lib/skipper/cluster-a --member-id skipper-a
```

Initial Manual mode permits no mutation until ownership is acknowledged.
Inspect cluster binding, exact names, public generation, lifecycle metadata,
topology confirmation, local authority, server progress and evidence provenance.
An unreachable or zero-host configured pile must remain visible. UNKNOWN and
stale are neither healthy nor failed; DISCONNECTED and INACTIVE are distinct
YDB states. Generation is a uint64 decimal string and must never be rounded
through JavaScript Number or a spreadsheet numeric cell.

## Ownership, modes and evidence

Use `POST /api/mode` with an audited reason to establish exclusive
administration for Manual actions. Confirm that no other administrator or
controller mutates YDB and supply a deployment-plan reference. Independent
failure-domain placement is a separate operational statement required for
P-05-dependent Auto policies; it is not a discovered lock or certification.

`GET /api/state` reports requested/effective state, handover, evidence and gates.
Selecting Auto stores the request, but all profiles currently remain
unqualified: effective readiness stays blocked until the real capability and
measurement release gates are fulfilled in a supported implementation. Do not
modify a parser name, acknowledgement or mock fixture to bypass this gate.

GLOBAL_BSC replies corroborate one shared assessment. They are not independent
pile votes. Contradictory complete shared assessments block action. A qualified
independently local profile would show separate reporter/target assessments,
node denominators and shared-origin grouping. Missing/truncated replies remain
UNKNOWN. Compute-only faults do not establish a storage failure.
That independently local behavior currently exists only in pure policy/display
fixtures. `nodewarden_local_v1` has no live capability through the existing
public API and reports unsupported; this app does not install a collector or
extend YDB/SDK contracts to provide one.

## Eight workflows

`POST /api/plans` produces a signed, expiring preview for an action. Review exact targets, RPC steps,
current/intermediate/terminal states, side effects, blockers and acknowledgements.
The server rechecks its bindings and state before admission and checks again
before SDK handoff. A preview is not a YDB generation compare-and-swap. Manual
previews expire within 120 seconds independently of evidence TTL; Apply decision
also binds the displayed decision/evidence. Refresh a stale preview explicitly.

| Workflow | Preparation and intended completion |
| --- | --- |
| Failover | Approve every failed target as one batch. If the primary is among them, choose an already synchronized successor. Verify the exact final role map and all requested disconnections. Keeping another unavailable mandatory quorum pile may still prevent commit. |
| Make primary | Preview switchover to an already synchronized pile. YDB first records PROMOTED; completion is observed when the intended pile is PRIMARY and no unwanted promotion remains. |
| Take down | Use a reachable synchronized non-primary pile. YDB first enters SUSPENDED, later DISCONNECTED. SUSPENDED remains in config quorum; this is unsuitable for an unreachable target. |
| Primary maintenance | Select a synchronized successor. The workflow moves primary first, observes completion, then takes down the former primary. It is not one combined suspend/promote call. |
| Rejoin | Repair and prepare a DISCONNECTED pile, then explicitly request NOT_SYNCHRONIZED. Observe later synchronization. Do not repeatedly reissue rejoin; the hidden `_2` phase already participates in quorum. No reliable percentage or ETA is available. |
| Convert | Operators deploy compatible single-pile BridgeConfig first. Accepted conversion starts BSC work; completion requires `conversion_in_progress:false`. Add/remove must wait. |
| Add | Operators provision hosts/config/resources first; target starts INACTIVE. Accepted work installs parts, and completion leaves DISCONNECTED. Rejoin is a separate action. |
| Remove | Target must be DISCONNECTED with no conflicting lifecycle work. Removal deletes its storage-group parts including data and finishes INACTIVE. Remove the configured name/hosts only after completion. |

For Remove, type the exact target name, preserving whitespace, case and Unicode.
For the valid empty-string name, select the explicit empty-name statement;
the request sends `typed_pile_name:""`. Independently acknowledge permanent
data deletion and every required statement. Server admission validates the
actual typed value, empty-name flag and deletion flag; selecting a target alone
does not authorize destruction. An empty name is never a missing target.

Accepted SDK/YDB work can outlive its HTTP response or client. Inspect the
operation's original RPC outcome, acceptance proof, observed postconditions and
continuing background guards separately. O-07 exclusive-history inference
requires its recorded assumptions and distinguishing marker; it is not a
server-side request-ID proof. Completion of role/state transitions does not
establish tablet drain or application readiness; validate those separately.

## Observe and external handover

Select Observe with a reason before giving an external administrator control.
It durably stops new dispatch/steps, revokes ownership acknowledgement, cancels
eligible pre-intent queued work and continues reconciling admitted work. Wait
for `controller.handover.complete:true` and inspect unresolved/background work
before beginning external changes. Merely selecting Observe is not complete
handover and does not cancel accepted YDB work.

Returning to Manual or Auto requires a fresh explicit ownership acknowledgement
and every ordinary gate. A paused ordinary workflow does not resume merely
because the mode changes: preview remaining steps and resume the same operation.
Preempted workflows cannot resume; inspect their emergency successor and
continuing background obligations. Use a fresh plan if the old goal is wanted
again. **Cancel remaining steps** is available only while no step is executing,
every earlier attempt is closed and no background obligation is pending. It
stops steps without durable intent; it never unsends or reverts earlier steps.
Automatic work closed with proof that it was never dispatched requeues in Auto
mode and rebinds to fresh confirmed evidence without operator action.

## Local emergency stop and release

Use `POST /api/local-stop` to close this process's dispatch gate even
when ordinary command admission or durable authority is unavailable. The
independent path can act while a command submission's receipt is unresolved.
It cannot unsend a queued/in-flight SDK call or establish that ambiguous work
stopped. It is separate from Observe and from external handover.

Inspect the receipt and current `/api/state` stop latch:

* `receipt_durability:"durable"` means the stop marker/recording succeeded;
  an existing marker is loaded before dispatch is enabled on restart.
* `receipt_durability:"volatile"` means the live gate is latched but persistence
  failed or is uncertain. Stop the service and inhibit its restart if a
  persistent stop is required. Do not depend on that latch surviving restart.

The explicit release request binds the displayed `incarnation_id` and
`stop_revision`. A stale release cannot clear a newer stop. Failed marker
removal keeps the gate closed, and all normal mode/readiness gates still apply.
Mode changes cannot release the stop. Compare the separate bounded recent
emergency chronology at `/mon/local-stop` with command history `/api/audit` by
recorded context/time; their indexes have no combined cursor ordering.

## Unknown receipt or ambiguous SDK outcome

For an HTTP timeout or `ADMISSION_UNKNOWN`, retain the original request ID and
unchanged body/envelope. Use `/api/requests/{id}` or replay exactly the same
request. Do not issue a replacement ID while the old command may commit. A
lookup 404 is not proof of rejection. Retain the ID and unchanged request body
in the operator incident record.

An unresolved SDK attempt is a different uncertainty. Do not retry the Bridge
mutation. Keep reconciling, inspect the operation and pending server work, and
use external recovery only when the required procedure is actually complete:

1. Stop the old executor and prevent delayed invocation/completion from it.
2. Reconcile accepted YDB work, lifecycle metadata, relevant background work
   and a coherent fresh complete observed state.
3. Record the recovery procedure/evidence and reason. Acknowledge the absence
   of mutation-side fencing and the current exact state fingerprint.
4. Submit the operation's external-resolution checklist. A changed fingerprint
   requires a new review; resolution is not a resend or cancellation.

An increased generation alone does not prove the original action occurred.
An unchanged generation does not prove nonacceptance. Never edit the journal,
delete an operation, discard the state directory or relax a generation high
watermark as a recovery shortcut.

## Restart, retention and backup

The state directory contains `skipper.sqlite3`, its SQLite WAL/SHM sidecars when
present, `controller.lock`, and optional `local-stop` and `local-stop.audit`.
It binds schema 1 to cluster ID, member ID, local backend and membership
incarnation. A changed process incarnation on each start invalidates previews;
durable request receipts and accepted operations survive. Ownership for the
same binding survives restart, but fresh state/evidence/readiness is required
before dispatch. Restored stop markers keep dispatch latched.

The database uses WAL and synchronous FULL commits. Startup validates database,
snapshot, journal sequence/integrity and binding before exposing authority.
Corruption, incompatible schema or binding mismatch must fail closed; preserve
the directory for diagnosis. Graceful SIGTERM/SIGINT closes admission and drains
bounded local work; it does not cancel an SDK invocation already handed off.

Runtime checkpoints after journal progress reduce replay of the applied suffix;
they do not delete the authoritative log, durable receipt index or audit.
There is no operator journal-compaction/retention API in this version. Monitor
disk use and journal growth. `local-stop.audit` is also append-only; its HTTP
projection is a bounded recent tail, not a claim of file rotation. Display logs
are a bounded, restart-local diagnostic ring and cannot replace durable audit.

For a simple consistent backup, establish Observe handover, stop the service
and verify no executor remains, then copy the complete owner-protected state
directory together with version/configuration metadata to protected storage.
Include WAL sidecars if present; copying only a live database can omit committed
state. Retain SDK credentials separately under their own secret-management
policy. Rehearse restore against an isolated cluster without enabling mutations.

A stale backup lacks newer receipts, high-watermarks and attempts. Before
allowing a restored controller to dispatch, reconcile all activity after the
backup and prevent every old executor from acting. Never run the original and
restored directory as concurrent controllers. An on-disk backup is not YDB data
backup, HA, a mutation fence or permission to replay historical intents.
