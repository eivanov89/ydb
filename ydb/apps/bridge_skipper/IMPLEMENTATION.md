# Skipper standalone v1 implementation record

Authoritative design: `/Users/eivanov89/repos/ai_snippets/skipper.md` (sections
4–10, plus the existing YDB/mock contracts in sections 1–3). Lab and HA are
excluded. This record tracks implementation and evidence; an unchecked item is
not a claim of completion.

**Current status:** the embedded web UI, static routes and browser tests have
been removed pending a rewrite. The HTTP API and controller remain available.
UI implementation and browser validation entries below are historical.

**Historical status: v1 implementation complete (no lab, no HA).** The 2026-10-22
completion review fixed the round-3 P2 automatic liveness issue and the defects
listed under "Completion review" at the end of this record, removed every change
outside `ydb/apps/bridge_skipper`, and simplified the durable codec and storage
protocol. Production Auto qualification remains an external gate.

User scope amendment (2026-10-21): implement against the existing YDB API.
The additive I-02 prototype is excluded; API, SDK, NodeWarden and RPC changes
are removed. Unsupported evidence stays explicitly unavailable. Audit every
change outside `ydb/apps/bridge_skipper` and retain only necessary, justified
shared-library work.

Baseline: `70498f8555f`. At the start of this task the working tree was clean and
`bridge_skipper` contained only `ydbd_mock`; the production controller was absent.

## Changes outside the application

None. Everything outside `ydb/apps/bridge_skipper` matches the baseline,
including the Bridge public API/protobufs, SDK, NodeWarden, RPC handlers, NHttp,
shared time provider, YDB docs and historical `bridge_skipper_demo`.

The earlier opt-in NHttp listener body limit and its contributor docs were
reverted. Skipper still rejects bodies over 1 MiB with 413 `BODY_TOO_LARGE` in
`http/actor.cpp` and `ParseStrictJson`, but only after the stock listener has
buffered the body. Every client that reaches the port already has full
authority (README), so the remaining exposure is memory use by an authorized
client; this is accepted for v1.

## Plan and ownership

All implementation steps use Sol 6.1 High agents. Each implementation agent must
self-review its changes. Builds run only in coordinated windows with no source
editing, using the active YDB build/test skill and direct `yamake` wrapper. Test
execution belongs to a Luna agent selected immediately before it is launched.

1. **Configuration and identity** — strict configuration, URI and argument
   parsing, startup credential loading, timing validation, examples and tests
   (`config/`; C-01–C-07, D-01–D-02, E-05). Implemented/self-reviewed by
   `config` agent; included in the passing foundation release test run below.
2. **Domain, topology, evidence and policy** — deterministic public types,
   topology corroboration, fixed samples/rounds, source provenance, parser
   invalidity, admin selection, confirmations, arbitration and exact vectors
   (`domain/`; A-02, A-09, D-01–D-02, T-01–T-06, E-01–E-15, P-01–P-09a).
   Implemented/self-reviewed by `evidence`; included in passing foundation run.
3. **Durable operations and storage** — pure Apply, log/snapshot/dedup,
   admission/effect protocol, operation planning, outcome classification,
   background guards, recovery, modes/ownership and tests (`durable/`;
   A-06–A-07, O-01–O-20). Implemented/self-reviewed by `durable`; included in
   passing foundation run. Asynchronous runtime pieces remain in step 4.
4. **Runtime and SDK integration** — NActors process, single SDK driver,
   endpoint-pinned reads/mutations, bounded jobs and futures, callback lifetime
   gates, serialized storage executor, controller integration, local stop,
   orderly shutdown and runtime tests (A-01, A-03–A-05, A-08, A-10, C-01,
   E-01–E-07a, P-09–P-09a, O-19–O-20). Implemented and self-reviewed;
   the existing-API and app-owned clock adjustments pass regular unit tests.
5. **HTTP, previews and monitoring** — NHttp routes, strict JSON, signed
   stateless plans, receipts and unknown admission, independent liveness,
   metrics, logs/audit and API tests (H-01–H-14, I-08a). Executable/API checkpoint
   passes; measured projections/telemetry pass regular unit tests.
6. **Embedded production UI** — responsive Cluster/Operations/diagnostics,
   evidence provenance, all actions and recovery/ownership controls,
   race-resistant polling, accessibility and browser verification
   (U-01–U-13). Assets/self-review completed by `ui`; all 26 browser logic cases
   passed in Chrome. DOM/live browser cases and new casing/zoom geometry checks
   pass. Coordinator inspected the corrected wide table and zoomed header,
   card facts and diagnostic provenance screenshots.
7. **Signal qualification and acceptance tooling** — use the existing API and
   record unavailable diagnostic capabilities
   explicitly, build shared mock/real conformance harness, exercise direct SDK
   endpoints/failpoints, document measured versus unmeasured behavior, support
   matrix, alerts, ownership/deployment assumptions and recovery runbooks
   (E-04, E-11–E-13a, I-01, I-06–I-11; I-02 excluded by user). Existing-API
   executable conformance and all 53 retained CLI/demo tests pass; `ui`
   authored operational/support documentation. Real execution and
   production certification require actual supported-build fixtures/cluster
   access; synthetic fixtures never satisfy that gate.
8. **Integration validation** — compile executable/libraries, deterministic
   tests for every reasonable unit of logic, mock integration and browser
   acceptance. The current regular checkpoint passes all 930 tests, including
   Astra round-two corrections. All eight browser tests pass. The completion
   review fixes the Astra round-3 P2 with a recovery regression.

## Required review sequence

Do not skip, reorder, or silently replace requested reviewers.

- [ ] Astra Ultra implementation review, findings checked/fixed, re-review up
      to three total rounds. If issues remain after round three, stop.
- [ ] Cursor `grok high` review, findings checked/fixed, up to three total
      rounds with the same stop condition.
- [ ] Astra Ultra test-gap audit; implement missing coverage; Luna executes
      tests and returns completed results.
- [ ] Cursor `opus high` once; check/fix findings; no second Opus review.
- [ ] Astra Ultra final implementation-versus-specification analysis.
- [ ] Final requirement-by-requirement completion audit with current evidence.

Execution stopped after Astra implementation rounds **1, 2 and 3**. The final
round confirmed one remaining P2; there is no fourth implementation review.
Cursor Grok rounds: **0**; separate Astra test-gap audit: **not started**;
Cursor Opus: **0**; final Astra specification analysis: **not started**.
Final regular `--tests-retries 10` validation: **not run**. These stages were
not silently replaced by the preparatory scope audit or owner self-reviews.

## Evidence and open gates

- The latest release checkpoint passes 930 tests in 21 regular suites,
  including all executable workflows, HTTP/NHttp and the medium mock suite.
  All 53 retained CLI/demo tests and the browser target's eight tests pass,
  including 26 browser logic assertions and the live UI/SDK/mock workflow.
  Astra round-two corrections pass. The final browser rerun passes all eight
  tests, including page-wide containment at 390 px with 200% CSS zoom. The
  coordinator inspected representative screenshots; bounded full-page capture
  metadata explicitly marks incomplete coverage when its 12-tile cap is reached.
  The completion review's counts and regressions are recorded at the end.
  Production Auto qualification remains external.
- Recognized evidence profiles are not certified merely because their parsers
  exist. I-03 permits Manual diagnostics while I-01/I-02 remain open; this does
  not waive the rest of the controller, operation, HTTP or UI implementation.
- No independent local-vantage capability, primary-loss fallback, resynchronizing
  `_2` signal, exclusive-history acceptance marker, or no-delivered-request-replay
  qualification will be invented from mock output.
- User confirmed: use the local repository/mock for now. Real-cluster execution
  and production Auto certification are deferred external qualification gates;
  implementation and local/mock coverage remain required.
- The supplied Linux skill/wrapper paths are absent on this Mac. Located active
  equivalents are `/Users/eivanov89/.agents/skills/ydb-build-test/SKILL.md` and
  `/Users/eivanov89/repos/my-env/bin/yamake`; the latter has an existing approval.

## Execution log

2026-10-21: inspected source/specification and clean baseline; created plan;
started three independent foundation agents with explicit file ownership,
shared-interface coordination, unit-test and self-review requirements.

Configuration implementation/self-review completed; runtime SDK/actor APIs
inspected read-only. Started independent embedded UI implementation while domain
and durable unit coverage is being completed. Coordinator integration review
identified promotion preservation, exact maintenance target, hung-node round
continuation and Observe handover reconciliation cases; owners are fixing and
covering them before the first build.

Foundation validation completed by selected `gpt-6-luna` test executor:

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -t --test-size-timeout=medium=100 ydb/apps/bridge_skipper/config/ut ydb/apps/bridge_skipper/domain/ut ydb/apps/bridge_skipper/durable/ut
```

Initial two invocations exited 1 before tests: a copied range-loop variable in
config tests, then missing-field initializers in durable tests. Owners fixed and
self-reviewed both. Third invocation exited 0: **3/3 suites and 83/83 tests GOOD**.
All source writers paused during each build. This is initial foundation
validation; final repeated release validation remains pending.

Resumed UI work and started runtime gateway/resources/bootstrap and controller
integration in two Sol 6.1 High agents. HTTP implementation begins when a worker
slot becomes available. Review sequence above has not begun.

UI assets and self-review completed. Began HTTP integration with the `durable`
agent. The agreed acknowledgement wire object contains `confirmed` IDs,
`typed_pile_name`, `empty_name` and `deletion`; the server checks removal values
independently. Product `/app.js` concatenates the two embedded JS resources;
test harness resources are not product routes.

Runtime/HTTP integration review is in progress before the first runtime build.
Coordinator identified and assigned corrections for installed-state naming,
post-intent workflow revision checks, actual SDK start timestamps, stale config
timestamps, fair bounded endpoint polling and canonical UUID generation.
HTTP input bounding includes an opt-in NHttp listener parser limit so the 1 MiB
API limit also bounds buffering. Owners will test and self-review these changes.

The local/mock clarification defers real execution and qualification, not the
I-02 read-only prototype. Traced the existing cheap GetClusterState path through
NodeWardenQueryStorageConfig and physical VStatus query/quorum machinery; the
prototype must collect independently of a quorate distconf root and keep
unobserved storage UNKNOWN. Its implementation and mock contract remain pending.

Gateway/resources/bootstrap authoring reached self-review. Actor-runtime tests
now cover a registered gateway/fake SDK, actual callback-gate attachment and
closure, queued closure versus handoff, and independent stop before dispatch.
These tests have not run. Controller integration is adding injected test-only
qualification/decoder seams so Auto admission, qualified acceptance and healthy
rejoin completion are exercised without enabling unqualified production Auto.
Coordinator assigned outcome-slot reservation through durable completion,
actual config-fetch freshness, readiness-epoch loss/recovery, identity mismatch
invalidation, and checkpoint scheduling. HTTP remains under implementation.

Initial runtime integration validation completed by selected `gpt-6-luna`:

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -t --test-size-timeout=medium=100 ydb/apps/bridge_skipper/config/ut ydb/apps/bridge_skipper/domain/ut ydb/apps/bridge_skipper/durable/ut ydb/apps/bridge_skipper/runtime/ut
```

First two invocations exited 1: qualified `EventSpaceBegin` macro uses, then
SDK `TIssues::empty()` versus `Empty()`. Third compiled and ran all 116 tests,
but four gateway actor tests crashed because the fixture used immediate test
runtime delivery before queued bootstrap. The owner fixed real mailbox ordering
with queued delivery and an observed bootstrap, without changing production
actor behavior. Fourth invocation exited 0: **4/4 suites, 116/116 tests GOOD**.
All source writers paused for every invocation. This covers the current runtime
checkpoint; additional controller acceptance cases, HTTP/NHttp, executable,
mock/browser and final repeated validation remain pending.

Resumed HTTP/NHttp work and remaining controller acceptance tests. Assigned the
I-02 additive proto/SDK/local NodeWarden collector implementation to `config`,
with explicit root-independent UNKNOWN behavior and bounded physical VStatus
probes. VStatus does not perform fresh PDisk I/O: the prototype must expose its
scope and cannot establish production qualification from actor readiness alone.
The controller adapter is coordinated with `evidence`; mock/conformance tooling
follows the published schema. Required external review counters are still zero.

The additive diagnostic schema and owned SDK settings/result seam are now in
the workspace. Ordinary GetClusterState remains opt-out by default. The local
collector, strict controller decoder and their tests are in progress. HTTP
review also separated evidence-bound manual decision application from the Auto
source flag, corrected readiness/prebridge presentation and preserved exact
workflow effects in previews. These changes are not yet build-validated.

Remaining integration work is explicit: finish the HTTP/NHttp and executable
build, complete the local diagnostic collector tests, model the new capability
in the mock, run controller-to-mock/contract cases and browser acceptance, and
publish support/measurement/runbook documentation. The local-stop emergency
journal remains independent of authority so stop can survive journal stalls;
its diagnostic exposure must not fabricate a shared ordering with command-log
indexes.

Intermediate executable/HTTP validation completed by `gpt-6-luna`. The initial
`-t` command reached only small tests; subsequent invocations used `-tt` to
include the medium mock suite:

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -tt --test-size-timeout=medium=100 ydb/apps/bridge_skipper ydb/library/actors/http/ut
```

Four initial invocations stopped before full completion: missing Base64 header
and dependency (configuration exit 8), SDK fluent-setting getter mismatch, then
HTTP string-stream/iterator compilation errors across two invocations. Owners
fixed each issue between frozen build windows. The fifth invocation exited 0:
**12/12 suites, 855/855 tests GOOD**, including the executable build, HTTP/NHttp
and medium mock tests. This is a checkpoint, not final repeated validation.

Released the freeze for I-02 collector actor tests/server completion, explicit
mock diagnostic capability and executable acceptance, UI/server wire fixes,
and operational documentation. Read-only integration checks found the native
storage-config fingerprint is SHA1 (20 bytes), distinct from the new public-state
SHA256 fingerprint (32 bytes); collector, decoder and mock must agree. UI fixes
include accepting an explicitly unknown/null generation on startup and correctly
displaying null ages and structured assessments. External review counters remain
zero.

Post-checkpoint review found an admission-budget-derived dispatch deadline that
contradicted E-05/P-09a. The controller owner removed that second validity limit:
evidence-bound dispatch retains the original diagnostic TTL, while measured
round-close-to-handoff overruns are telemetry only. New deterministic cases
cover exceeding the configured admission p99 while retaining valid TTL slack,
expiry followed by negative closure, and selecting only necessary fresh local
quorums. The owner also corrected the native 20-byte storage fingerprint and
exposed the actual corroborated global vector for HTTP. These additions await
the next validation window.

UI/docs handoff now includes 26 browser logic cases, null initial-generation and
age fixes, structured matrix rendering, operational runbooks/support matrix,
monitoring and browser verification instructions, plus delegated EN/RU
NodeWarden contract documentation for collector-owner review. The mock agent is
adding shared executable contract cases and repository-supported retained
CLI/demo validation; the local CLI and demo interpreter are available. Collector
actor test authoring includes missing/dead/malformed VDisk replies, no-root
primary loss, asymmetric coverage, configuration races and exact empty names.

Collector/server/SDK, controller and mock/acceptance owners reached self-review
checkpoints. The next validation includes the collector actor suite, NodeWarden
and gRPC service compilation, SDK settings/result tests, and actual executable
workflow/shared-contract tests. HTTP metrics and bounded JSON logging are being
completed before that build window. No new test result is implied yet.

Luna attempted browser verification through the connected browser service. It
reported no browsers and could not open the in-app browser; zero assertions
executed. A local Chrome binary exists, so the UI owner is adding an explicit
repository browser test target with isolated profiles and retained screenshots.
This fallback will also run through the prescribed build/test wrapper.

The Chrome fallback target is authored and self-reviewed: three LARGE tests
cover the 26-case logic harness, explicit read-only UI fixtures with keyboard,
polling and screenshot checks, and live UI/C++ SDK/mock empty-name removal.
The shared acceptance and retained CLI/demo targets now split into four
independent chunks. None of these new cases has executed yet.

The collector checkpoint contains 16 actor tests and three SDK compatibility
tests. Controller coverage includes queued evidence-bound recovery, stale
decisions, preemption, readiness epochs and fair polling, with actual RPC
latency and once-per-handoff histogram samples. HTTP/runtime observability and
focused tests are finishing before the coordinated source freeze.

The next validation attempt exited 8 during configuration: both new LARGE
targets needed `TAG(ya:fat)`. The owner fixed those declarations. A retry was
then interrupted at the user's scope amendment; Luna confirmed terminal exit
130 and no remaining build processes. Neither attempt produced new passing
test evidence, and no server/SDK validation stage ran.

The user requested existing-API implementation and justification for changes
outside the app. Removed all I-02 public proto/SDK/NodeWarden/RPC additions and
their new tests/docs; those tracked paths now match baseline. Runtime and mock
adapters are being returned to ordinary SelfCheck/GetClusterState, retaining
all general workflow, TTL, recovery and evidence tests. A Skipper-owned clock
will replace the unnecessary shared time-provider API exposure. The remaining
shared HTTP listener limit is being audited separately: the stock proxy
buffers a full request before app admission and offers no per-listener body
cap, so its opt-in limit enforces the required 1 MiB bound before buffering.
External review counters remain zero.

Outside-app audit after the amendment: public Bridge API, SDK, NodeWarden,
RPC and shared time-provider files have no remaining diff. The original mock
service/model/control behavior also matches baseline; only its repository test
integration is extended. Runtime/mock/browser acceptance now uses existing
public APIs. Retained NHttp changes are limited to an optional request-body
bound, propagation to the listener parser, safe error/connection handling and
behavioral tests; the default listener behavior remains unchanged.

Existing-API release validation by Luna completed (exit 10):

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -tt --test-size-timeout=medium=100 ydb/apps/bridge_skipper ydb/library/actors/http/ut
```

Compilation succeeded. The run reported 21 suites (14 GOOD, six FAIL, one
TIMEOUT) and 900 tests (882 GOOD, 14 FAIL, one NOT_LAUNCHED, three TIMEOUT).
Failures include executable acceptance topology-startup timeouts, one queued
maintenance/preemption controller assertion, new Python lint issues and an
e2e import-check initialization error. Owners are investigating/fixing those;
large CLI/browser execution and final repeated validation remain pending.
The acceptance process logs show every SDK read failing immediately before
topology confirmation, so endpoint handling is being traced before changing
topology or timeout expectations. Review counters remain zero.

The SDK boundary used a URI where per-client DiscoveryEndpoint requires bare
host:port. Corrected the app adapter while retaining exact IPv6/port, disabled
discovery and inherited TLS/credentials, with focused regression coverage.
Emergency Manual preemption now requests its mandatory fresh same-node
preflight without requiring an already-current cached admin view. The stale
maintenance callback still cannot create an intent or dispatch. Fixed Python
lint/import-time initialization and retained bounded startup-state diagnostics.
All owners self-reviewed before the next source freeze.

The regular rerun exited 10: **20/21 suites and 898/901 tests GOOD**, with two
failures and one test interrupted by its chunk's 100-second limit. All unit,
HTTP/NHttp, mock, lint and import suites passed. SDK executable workflows now
reach the mock. Remaining failures are the shared stale-generation contract
(received a modeled config-quorum rejection) and conversion completion; the
empty-name Add case was cut off by the shared chunk timeout. Owners are tracing
these before the next validation. External review counters remain zero.

The 898/901 run also included an unintended trailing `.` target; its successor
used the exact two intended targets. That successor exited 10 with **20/21
suites and 903/904 tests GOOD**. Raw NOT_FOUND preflights are now retained for
the explicitly guarded prebridge Convert path, with positive and negative
gateway-actor regressions. The stale-generation fixture now models a genuinely
stale disconnected receiver instead of rejecting its own local prepare through
detach; its exact expected generation error and authoritative-state invariant
pass. Independent test chunks preserve the existing 100-second budget.

The only remaining regular failure is Add after an expanded configuration:
old confirmed membership and new matching admin/config replies mutually block
reconciliation. The owner is implementing atomic reconciliation of the matching
candidate while preserving the original fixed quorum and freshness checks.
Large compatibility/browser execution has not started. Review counters are zero.

Atomic membership reconciliation and its four arrival-order/generation cases
passed the next regular release run: **21/21 suites and 905/905 tests GOOD**.
The unchanged executable empty-name Add test also passes. Source remained
frozen while Luna continued with the explicit LARGE compatibility/browser run:

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -ttt --test-size-timeout=medium=100 --test-param=ydbd_mock_real_ydb=/Users/eivanov89/ydb/bin/ydb --test-param=ydbd_mock_work=/Users/eivanov89/ydbd_mock_work '--test-param=skipper_browser_binary=/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' ydb/apps/bridge_skipper/ydbd_mock/e2e ydb/apps/bridge_skipper/ui/browser_tests
```

That command exited 10: **5/6 suites, 64/66 tests GOOD**. All 53 retained
CLI/demo compatibility tests passed. Chrome 155 executed and passed all 26 UI
logic cases. The two DOM/live-UI browser tests failed at the navigation URL
wait after recorded load events; their assertions/screenshots remain pending.
The UI owner is correcting the strict origin-versus-trailing-slash comparison.

Astra Ultra implementation review **round 1 of at most 3** ran read-only against
the frozen implementation while LARGE validation finished. It found seven
substantive issues, accepted for correction:

1. Separate current dispatch readiness from new-admission readiness: queued
   Manual sends must see current conflicts, and Auto must permit its own intent.
2. Preserve actual lifecycle configuration and admin-quorum expiry through
   preflight, intent commit and final gateway handoff.
3. Preserve an explicit Observe pause across late RPC/observation acceptance.
4. Retain continuing Add/Remove obligations after a disjoint emergency.
5. Require a fresh ownership acknowledgement after external recovery.
6. Select another verified Manual endpoint after failed/busy preflight.
7. Recover HTTP planning/display after unsupported admin observations are
   superseded by a coherent supported quorum.

Runtime and durable owners are fixing these with focused regressions and
self-review. No additional shared HTTP compatibility issue was substantiated.
Cursor Grok, the separate Astra test-gap audit, Cursor Opus and final Astra
specification analysis have not begun. Final regular 10-retry validation also
remains pending; no LARGE test will use 10 retries.

All seven round-one findings are implemented with focused regressions and
owner self-review. The first validation attempt stopped at compilation (exit
1): a newly added permission epoch was assigned after an aggregate initializer,
triggering `-Wmissing-field-initializers`. Both aggregate sites now initialize
it explicitly. The unchanged HTTP/2 suite also reported two crashes in that
interrupted run; their diagnostics are being located before the rerun. No
round-one fix is claimed validated yet, and Astra round two has not started.

The rerun compiled successfully and passed **919/920 tests in 20/21 suites**
(exit 10). Both HTTP/2 tests passed on this rerun; earlier hidden logs show
thread-cleanup and duplicate-PassAway aborts, but their cause is unconfirmed.
The sole current failure is the new integrated qualified Auto test producing
no controller dispatch. Its required placement/setup conditions are being
checked without weakening the exactly-one-SDK-call assertion. Browser rerun and
Astra round two remain gated on the regular pass.

The integrated Auto regression fixture lacked independent placement in its
ownership acknowledgement. Supplying that required fixture condition preserved
the production gate and the exactly-one-SDK-call assertion. The regular rerun
exited 0: **21/21 suites and 920/920 tests GOOD**. Both previously failing
HTTP/2 cases passed again. Luna then completed browser-only LARGE validation:

```text
/Users/eivanov89/repos/my-env/bin/yamake --build=release -DCFLAGS=-fno-omit-frame-pointer -ttt --test-size-timeout=medium=100 '--test-param=skipper_browser_binary=/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' ydb/apps/bridge_skipper/ui/browser_tests
```

It exited 0: **3/3 suites and 8/8 tests GOOD**, including real Chrome logic,
DOM fixtures and live UI → C++ SDK → mock empty-name removal. Coordinator
screenshot inspection found table row names visually uppercased and narrow
200% CSS zoom facts wrapping into single-character columns. The UI owner
scoped uppercase styling to column headers, stacked narrow-screen facts and
added computed-style/geometry assertions; those changes await validation.

Astra Ultra implementation review **round 2 of at most 3** confirmed the seven
round-one corrections and identified two further issues in qualified evidence
workflows: confirmation reset must revoke already-bound dispatch permissions,
and HTTP Resume must bind a fresh matching decision for an evidence-bound
operation. Runtime and HTTP owners are correcting both with regression tests
and self-review. Ordinary primary-maintenance Resume was independently traced
and found sound; domain/durable/config re-review found no additional issue.
The third implementation review will be the final permitted round. Later
requested review stages and final regular 10-retry validation remain pending.

Round-two corrections add a confirmation revocation generation checked through
preflight, intent commit and both gateway validation points. Evidence bindings
are installed only after successful durable admission/Resume, retaining their
captured generation and original expiry. Signed evidence-bound Resume previews
select a current matching confirmed decision, preserve the existing operation
and exact remaining update, and bind decision/evidence/endpoint/expiry. Ordinary
Manual previews remain independent of rotating evidence. New regressions cover
contradictory and unreserved rounds, excessive confirmation gaps, old grants
after recovery, and HTTP preview/submission through controller admission and
exactly one SDK mutation. Owner self-review and validation are tracked below;
authoring these regressions is not a passing result.

The first round-two regular validation compiled and exited 10: **20/21 suites
and 929/930 tests GOOD**. The sole failure was the new HTTP-to-controller test's
initial preview receiving `LOCAL_STOP`: its helper supplied a default receipt,
whose safe default is stopped, instead of the harness's current latch state.
The runtime owner is correcting that fixture; browser validation did not start.

The fixture now projects the current controller stop state instead of using the
receipt's safe stopped default. Its regular rerun exited 0: **21/21 suites and
930/930 tests GOOD**. Browser-only LARGE then exited 10: **2/3 suites and 7/8
tests GOOD**. Logic and live UI/SDK/mock tests pass; the display fixture fails
at Chrome screenshot capture at 200% CSS zoom before its new geometry assertions
execute. Saved geometry shows full-width stacked values, UNKNOWN on one line
and the full uint64 on two lines. The coordinator confirmed exact east/East
visual casing in the new table image; the UI owner is fixing bounded viewport
capture so the zoomed layout can be inspected. No product assertion is waived.

The browser capture helper now enforces viewport-sized PNGs and captures longer
pages as bounded scroll tiles with offsets/completeness metadata. An initial
double-animation-frame scrolling wait timed out in headless Chrome; synchronous
scrolling with a bounded position check replaced it. The browser rerun then
passed **3/3 suites and 8/8 tests**. Coordinator inspection confirms readable
zoomed facts; the full-page zoom capture explicitly records its 12-tile limit
and incomplete coverage. The viewport inspection also exposed horizontal
overflow in the zoomed header's Theme/Stop controls; a minimal wrap correction
and page-level overflow assertion are being added before the final visual pass.

Header wrapping made the emergency Stop control fully visible, while the new
page-wide assertion exposed another overflow in a long diagnostic provenance
token. Added ordinary word wrapping for that text, bounded offending-element
diagnostics and a dedicated provenance viewport screenshot. The final browser
command exited 0: **3/3 suites and 8/8 tests GOOD**. Geometry reports a 390 px
document in a 390 px viewport; Stop is enabled and lies between x=30 and x=360.
Coordinator inspected header, card facts and provenance at 200% CSS zoom, plus
the earlier exact-case table screenshot. All assertions remain enabled.

Astra Ultra implementation review **round 3 of 3** confirmed the earlier fixes
and independently substantiated one remaining **P2**: if confirmation is revoked
after an automatic operation is durably queued but before any attempt exists,
fresh identical confirmation does not recover it. `TryAutomatic()` suppresses
replacement using the queued owner's unchanged suppression key, while
`ScheduleWorkflow()` rejects its obsolete confirmation generation. There is no
automatic retirement/replanning transition, and HTTP Resume requires Paused.
The operation therefore requires operator cancellation despite having no
ambiguous external effect. No additional final core/UI finding was substantiated.

Future correction must durably retire/replan or explicitly admit a fresh binding
only for work with no mutation attempt, without reviving old permissions or
changing restrictions on ambiguous, explicitly paused or preempted operations.
Extend the existing pre-intent revocation regression through two recovered
matching rounds and require a fresh preflight/intent without operator action.
Per the user's explicit limit, implementation work stops here: no fourth Astra
round, no Cursor or later audit stages, no final ten-retry run. The goal remains
unfinished. All test commands are terminal and all source owners are paused.

## Completion review (2026-10-22)

A later user request superseded the stop above: review against the design,
fix, finish v1 and simplify, keeping changes inside the application. Fixes:

- **P2 automatic liveness.** `TryAutomatic` still suppresses a duplicate plan
  when a queued automatic owner has the same suppression key. It now rebinds
  that owner to the current confirmed Decision when the binding is missing,
  stale, from another epoch or expired. A `NeverDispatched` closure requeues
  executing automatic work in Auto mode instead of pausing it; manual work and
  work closed outside Auto still pause for an explicit resume. Covered by an
  extension of `RevokedConfirmationCannotAdmitPreflightOrDispatchDelayedIntent`
  (two clean rounds, then a fresh dispatch without operator action) and an
  updated recovery test.
- **Cancel.** "Cancel remaining steps" is allowed when no step is executing,
  every attempt is closed and no background work is pending. It is reported as
  `ALREADY_DISPATCHED`, `OPERATION_TERMINAL` or `BACKGROUND_WORK_PENDING`
  otherwise. O-10's "before intent" is read per step: steps already settled stay
  applied. `CancelError` is shared by Apply and the projection.
- **Terminal status.** One `IsTerminal` predicate replaces three local lists.
  The metrics copy had counted Preempted and ExternallyResolved operations as
  active.
- **Gateway preflight race.** An ordinary poll completing after a named
  preflight no longer replaces that preflight's result.
- **Startup diagnostics.** Startup failures print one bounded JSON
  `startup_failed` line. Configuration errors no longer carry source locations.
- **Outside-app changes removed.** See "Changes outside the application".

Simplifications: table-driven field lists replace 15 hand-written durable
struct codecs (`durable/codec.cpp` 708 → 466 lines, identical wire format). The
redundant full-state `Candidate` comparison in storage requests is removed:
Apply is deterministic, and the controller already checks the applied index,
revision and result. The HTTP actor's duplicate pending-request scan is removed.

Validation used the direct release `yamake` wrapper. The regular suite with
`--tests-retries 10` exited 0: **20/20 suites, 680/680 tests GOOD**, with no
retries consumed. The LARGE e2e and browser command above (`-ttt`, real `ydb`
CLI, Chrome) exited 0: **6/6 suites, 66/66 tests GOOD**. The `grok-4.7-medium`
model selected for the test runner was unavailable, so tests ran directly
rather than through a delegated runner. The Cursor review stages listed under
"Required review sequence" were not run.

### Astra Ultra completion re-review

The read-only re-review confirmed the queued-Auto fix and the outside-app
cleanup, and raised four findings. All are fixed:

- **P1 state-directory durability.** After the startup transaction, the store
  fsyncs the state directory and its parent before it loads or accepts work, so
  the directory, lock, database and WAL entries survive power loss.
  `SyncDirectory` is shared with the local-stop marker code.
- **P2 stale cached endpoints.** `Corroborate` keeps the previously corroborated
  configuration. A verified cache entry is restored only if that node's
  configured host and URI are unchanged; otherwise it is dropped and the node is
  verified again at its new address.
- **P2 oversized checkpoints.** `MaxDurableRecordBytes` (256 MiB) is shared by
  the codec and the store. A larger checkpoint is skipped without failing
  storage: the previous snapshot and the complete log remain readable.
  `durable_checkpoints_skipped_total` and a `checkpoint_skipped` log record
  report the skip. Oversized commands are rejected before they are written.
- **P2 UI pending request.** A definitive 4xx rejection of a replay now
  releases the retained request, using the same rule as the first submission
  (`SkipperLogic.definitelyRejected`).

New regressions: `OversizedCheckpointKeepsPreviousSnapshotAndStoreAvailable`,
`StoreCreatesAndSyncsNestedStateDirectory`,
`TopologyDropsVerifiedEndpointWhenConfiguredAddressChanges`, and a 27th browser
logic case. The regular suite with `--tests-retries 10` exited 0: **20/20 suites,
683/683 tests GOOD**. The LARGE e2e and browser command exited 0: **6/6 suites,
66/66 tests GOOD**.

### Astra Ultra third re-review: invariants
Four P2 findings were symptoms of duplicated state and ordering by request
sequence. They are fixed by the invariants in skipper.md §4.5:
- INV-01: replies are stored only by endpoint and attributed to nodes through
  the tracker's current binding; probes are consumed once. Fixes stale aliases
  after readdress.
- INV-02: acceptance proof uses `ExclusiveHistory`, not the active ownership
  acknowledgement. Observe no longer blocks qualified acceptance.
- INV-03: release receipts no longer copy latch errors.
- INV-04: operation projections carry `state_index`; the UI keeps the highest
  index and a reset removes only older absent entries.
New regressions: `SuccessfulReleaseAfterVolatileStopReportsNoError`,
`ConfiguredReaddressDropsEveryVerifiedAliasOfNode`,
`ObserveKeepsQualifiedAcceptanceOfAdmittedMutation`, `state_index` checks in
`http_ut`, an updated UI ordering case. Not built or run in this round.

### Recovery, launcher validation and invariant follow-up (2026-10-22)

The preceding section records the implementation round before validation.
This record was recovered after an accidental `PLACEHOLDER` overwrite by
replaying 35 successful recorded patches and comparing with later recorded
file reads. One 41-line historical section was truncated in later reads and
retains its exact earlier patch version; no content was invented.

Added `ydbd_mock/scripts/skipper.sh` to launch isolated mock piles, the mock
supervisor and the new Skipper with free loopback ports. It prints HTTP links
for the Skipper console and mock state/events/control endpoints, generates
configuration from the mock manifest, and cleans up its services on Ctrl+C
or startup failure. Logs and state remain in the printed run directory.
All changes stay under `ydb/apps/bridge_skipper`.

Luna ran the direct release `yamake` wrapper with
`-DCFLAGS=-fno-omit-frame-pointer` and `--test-size-timeout=medium=100`:

- Focused LARGE mock e2e selection `-F '*SkipperLauncherTest*'`: **3/3 GOOD**.
  The initial compile failed while the controller refactor was in progress;
  the completed source built and all launcher tests passed on the next run.
- Regular `runtime/ut` and `http/ut`, with `--tests-retries 10`:
  **2/2 suites, 108/108 tests GOOD**.
- LARGE `ui/browser_tests`, with explicit
  `skipper_browser_binary=/Applications/Google Chrome.app/Contents/MacOS/Google Chrome`:
  **3/3 suites, 8/8 tests GOOD**, including all three browser methods and the
  **28 JavaScript logic cases**. The earlier run without the browser parameter
  skipped those three methods and is not counted as browser validation.

Astra Ultra's independent read-only follow-up found two remaining issues:

- **P1 endpoint identity collision.** If an endpoint changes from node 1 to
  another configured node 2, `ResolveIdentity` can leave both nodes bound to
  that endpoint. `AdminByNode` and `ConfigReplies` then count its reply twice.
  Frozen samples and expired config corroboration can preserve this collision
  long enough to satisfy an admin quorum or qualified acceptance proof.
- **P2 operation version forgotten on removal.** A list reset can remove a
  later-page operation and its version. An older outstanding detail response
  then recreates that operation because the current map entry is absent.
  Retain a version watermark when removing entries and enforce it on details.

These findings were verified by source tracing; the triggering sequences were
not executed. No fixes were made in this read-only review. The passing tests
above do not establish coverage of either sequence. Production Auto remains
unqualified.

Fixes, by making the two invariants explicit in skipper.md §4.5:

- **P1, one-to-one binding (INV-01).** `TTopologyTracker::RememberVerified`
  reverts any other node bound to the same endpoint to its configured,
  unverified form, so the derived views count each endpoint once. Before that,
  `ResolveIdentity` treats a different logical node at a bound endpoint as an
  identity change and forgets the endpoint's replies and the previous node's
  observations (`RESPONDER_IDENTITY_CHANGED`). `Corroborate` already restores
  at most one node per endpoint from `KnownEndpoints`.
- **P2, removal floor (INV-04).** A list reset records its `state_index` as
  `operationFloor`; `operationNewer` rejects any response below the floor, so a
  delayed older detail cannot recreate a removed entry.

New regressions: `EndpointBindsAtMostOneVerifiedNode` (domain tracker, checked
before and after corroboration) and floor assertions in the existing 28th UI
logic case. Not built or run in this round.
