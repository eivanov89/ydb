# Rework plan: mock, acceptance and end-to-end tests

[Index](rework_plan.md). This plan covers the `ydbd_mock/` changes the reworked skipper needs, and replaces `acceptance/` with a small executable end-to-end suite. Decision logic is tested in C++ `domain/ut` with protobuf fixtures (see each module plan). The mock is used only for behaviour that needs real gRPC, timing and processes.

## Current behaviour and evidenced gaps

**Provenance.**
- The mock predates skipper. It is on `main` (`2437f770408`, `5d4aca28e39`, `1e8ec4f4880`) plus `4c65619e191`.
- The skipper commit `dfe2861ba4d` added three things:
  - the whole `acceptance/` directory;
  - `ydbd_mock/scripts/skipper.{py,sh}` and `e2e/test_launcher.py`;
  - `e2e/ya.make` and README text.

**Faithfully modelled**, verified against the server:
- the `expected_generation` check: ABORTED when current > expected, UNAVAILABLE when current < expected, rechecked on each commit retry (`ydbd_mock/lib/services.py:194`, `cluster.py:259-268,588-594`);
- `UpdateClusterState` validation (`validator.py:90-199`);
- the PROMOTED, SUSPENDED, connect and sync follow-ups (`followups.py`);
- listener closure;
- lagging replicas;
- lost replies.

**Gaps.**

| ID | Class | Gap |
| --- | --- | --- |
| M-G1 | R | **No pile connectivity report.** `include_pile_connectivity` is ignored (`services.py:166-189`), so the primary-loss path cannot be exercised. |
| M-G2 | D | **The mock makes primary loss look easy.** With `auto_blame_dead_piles=true` (`topology.py:64`), survivors emit a BSC-backed RED `BRIDGE_GROUP` for a dead primary (`health.py:37-42`). Real YDB loses BSC together with the effective primary and falls back: a RED `STORAGE` "BSC didn't provide information" issue, plus GREY no-data parts (overview §3.2.2, §4.2.1). |
| M-G3 | D | **SUSPENDED → DISCONNECTED always completes.** The commit requires only `required_set(new)`, which excludes the suspended pile (`followups.py:62-63`, `cluster.py:392`). Real YDB needs the scepter and the global quorum including that pile (overview §3.2.7), so the dead end skipper must surface is hidden. |
| M-G4 | R | **Several SelfCheck behaviours are not modelled:** fallback mode, GREY, BLUE replication reasons, the `HEALTHCHECK_STATUS` size issues, and the post-BSC-restart GREEN window. |
| M-G5 | S | **The acceptance real-mode recorder cannot run.** It needs an external `fixture_control_url` actuator that does not exist (`acceptance/README.md:89`), and three cases are permanently `not_run` (`acceptance/lib/report.py:10-14`). |
| M-G6 | D | **The automatic path has no executable coverage.** Auto is unqualified, so acceptance only checks that Auto does nothing (`acceptance/tests/test_acceptance.py:146-163`). |

## Intended behaviour and design

### Mock changes

Each change is its own commit, separate from the skipper changes, with mock tests in `ydbd_mock/tests`.

**M1. Connectivity report.**
- When `include_pile_connectivity` is set, fill `pile_connectivity` from the responder's `Reachability` (`lib/connectivity.py`) and per-node up/down state:
  - the static node counts;
  - `has_node_quorum`, from a per-pile `static_quorum` count (default: a majority of the pile's static nodes). The server applies the static-group erasure quorum (`ydb/core/blobstorage/nodewarden/distconf_fsm.cpp:90-93`). The count is a documented approximation, and the tests choose fault sets that are unambiguous under both rules;
  - `in_config_quorum` and `in_data_quorum`, from the hidden internal state: stage 1 has neither, stage 2 has both, SUSPENDED has config only;
  - `generation`, `reporter_is_root` and `binding_in_progress`, the last set by a control knob.
- INACTIVE piles report no quorum.
- Add a fault option that rejects the optional query, as an older forwarding node does.

**M2. SelfCheck modes.**
- **When BSC-mode replies are used.** The mock answers in BSC mode only when the responder can reach the pile that hosts BSC (the PROMOTED pile if set, otherwise PRIMARY) and that pile's `static_quorum` is met.
- **Fallback reply.** Otherwise the reply contains:
  - the RED `STORAGE` issue "System tablet BSC didn't provide information";
  - static `STORAGE_GROUP` parts with the pile name: RED for piles whose nodes are "refused" (the existing unavailable fault), GREY "Group has disks with no data" for silent ones;
  - overall status EMERGENCY;
  - no `BRIDGE_GROUP` issues.
- **BSC-mode RED.** A BSC-mode reply marks RED the parts of piles that the BSC host pile cannot reach. This keeps the partition asymmetry.
- **Removed option.** `auto_blame_dead_piles` no longer emits BSC-style RED when the dead pile is the BSC host.
- **New control knobs:**
  - BLUE-only replicating VDisk reasons for a chosen pile's group;
  - a RED `HEALTHCHECK_STATUS`;
  - a GREEN window after the BSC host changes, with a configurable length (overview §3.2.5).

**M3. Suspend follow-up.** SUSPENDED → DISCONNECTED commits only while the suspended pile still meets its `static_quorum`, approximating the scepter and global-quorum requirement. Otherwise the pile stays SUSPENDED.

**M4. Contract tests and documentation.**
- Move the direct wire contracts from `acceptance/lib/contracts.py` into `ydbd_mock/tests/test_contracts.py`.
- Add contract tests for the generation guard:
  - ABORTED when current > expected;
  - UNAVAILABLE on a lagging receiver;
  - a commit produces generation = expected + 1.
- Update `ydbd_mock/README.ai.md`: the reviewed revision, `expected_generation`, connectivity and fallback.

### Skipper end-to-end tests (M5)

Rewrite `acceptance/` as `acceptance/tests/test_e2e.py` plus a slim `acceptance/lib/harness.py`. The harness:
- starts mock piles and skipper from a generated config with endpoints keyed by pile;
- reads `/api/state`;
- inspects the mock's request log.

Delete `acceptance/main.py`, `lib/report.py`, `lib/contracts.py` (moved by M4), `tests/test_report.py` and the recorder sections of `acceptance/README.md`. Replace `acceptance/ya.make`, which is currently a `PY3_PROGRAM` around `main.py`, with `RECURSE(lib)` and `RECURSE_FOR_TESTS(tests)`. Update `tests/ya.make` `TEST_SRCS` to `test_e2e.py`.

The scenarios run against the real C++ binary and SDK. Every mutation assertion checks two things in the mock request log: `expected_generation` equals the view generation, and `quorum_piles` is empty.

| # | Scenario | Expected |
| --- | --- | --- |
| E1 | A dead SYNCHRONIZED non-primary, `mode: auto` | One `{b: DISCONNECTED}` after `confirm_seconds`, no earlier |
| E2 | Primary loss with no PROMOTED pile: the primary (the BSC host) is down, survivors answer in fallback, and connectivity shows the primary without quorum | `{a: DISCONNECTED, b: PRIMARY}` |
| E3 | A one-round RED blip | No mutation |
| E4 | Lost reply after commit (failpoint) | No different update sent. Pending record cleared once G+1 is observed |
| E5 | Mutation held `after_acceptance` while another actor changes state | ABORTED, replan, no duplicate |
| E6 | Lagging replica behind one endpoint (mock stale replica) | That endpoint is not selected, because its reply is below G, and the update goes to an endpoint at G. With a forced UNAVAILABLE fault on the first endpoint, the identical update goes to another endpoint at the same G |
| E7 | A dead PROMOTED pile (the BSC host, with the promotion follow-up held): survivors answer in fallback | `{promoted: DISCONNECTED}` through decision D5, with no role change |
| E8 | A dead stage-2 `NOT_SYNCHRONIZED` pile | Disconnect |
| E9 | Closed listeners in DISCONNECTED and stage-1 piles | No verdict, no action |
| E10 | A dead SUSPENDED pile (needs M3) | `SUSPENDED_PILE_LOST`, no mutation |
| E11 | `mode: manual` with the E1 fault | Plan and CLI command published, no mutation sent |
| E12 | Skipper restart with the pending record at G and the view still at G, after the evidence changes | `PENDING_MUTATION`, no different update |
| E13 | Restart in which skipper sees only a lower-generation side | `STALE_HISTORY` |
| E14 | HTTP | Every POST returns 405 |

### Launcher (M6)

Update `ydbd_mock/scripts/skipper.py` and its test `e2e/test_launcher.py` for the new config schema. Leave the rest of `ydbd_mock/e2e` untouched; it tests the mock and the CLI, not skipper.

## Removals

- From `acceptance/`: the real-mode recorder, the provenance hashes and `Report`, which always says `production_auto_qualified: false`.
  - Real-build qualification is a release activity outside this repository's tests (see the validation gaps in the [index](rework_plan.md)).
- The `acceptance` workflow cases for switchover, takedown, maintenance, rejoin, convert, add and remove, which are out of scope under decision D1. The mock's own tests still cover these server behaviours.

## Ordered tasks

1. **M1–M4.** These can be done in parallel with the domain work.
2. **M5.** Depends on [core](rework_plan-core.md) C5 and [observability](rework_plan-observability.md) O2.
3. **M6.**

## Acceptance criteria

- The `ydbd_mock/tests` suites pass, including the new connectivity, fallback, suspend and contract tests.
- E1–E14 pass against the release build of the new binary. Run them with the `ydb-build-test` workflow; they are MEDIUM-size Python tests under `acceptance/tests/ya.make`.
- No mock end-to-end test depends on BSC-backed RED for a dead BSC host pile.
