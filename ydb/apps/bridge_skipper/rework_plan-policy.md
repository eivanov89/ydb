# Rework plan: confirmation, failover decision and modes

[Index](rework_plan.md). This plan replaces `EvaluatePolicy` (`domain/policy.cpp`), `TConfirmation` (`domain/evidence.cpp:322-341`) and the failover planner (`durable/workflow.cpp:84-120`). Its inputs are the view ([admin](rework_plan-admin.md) A3) and the per-pile verdicts ([healthcheck](rework_plan-healthcheck.md) H3). Its output is a `TDecision`.

## Current behaviour and evidenced gaps

`EvaluatePolicy` (`domain/policy.cpp:25-369`) works as follows:

- **Blocking.** It blocks on an unqualified profile (`:47`), which is always the case in production.
- **Global profile.** Any two determinate replies that disagree block the decision (`:99-101`), and one sample must cover the candidate and every target (`:179-211`).
- **Candidates.** The stored PRIMARY is tried first, then SYNCHRONIZED piles in name order (`:212-236`).
- **Confirmation.** It needs identical canonical decisions in consecutive rounds. The canonical decision includes the full epoch, so it resets on any readiness or metadata change (`domain/evidence.cpp:322-341`).

| ID | Class | Gap |
| --- | --- | --- |
| P-G1 | R | **Primary loss cannot reach a decision.** The candidate must be "NoFailureReported" by BSC (`domain/policy.cpp:160-163`, `219-236`), but BSC is gone with the primary (overview §3.2.2). |
| P-G2 | D | **Any `NOT_SYNCHRONIZED` target blocks the whole decision** (`domain/policy.cpp:320-326`), including failover of a failed primary in the same request (overview §3.2.1). |
| P-G3 | D | **"Stable" is reported while the primary's health is unknown.** It happens when a synchronized pile is clean and nothing has Failed (`domain/policy.cpp:219-236`, `327-330`). Overview §3.2.6 requires an explicit unknown state instead. |
| P-G4 | P | **A healthy PROMOTED pile is not preferred** as the successor, although it is already the effective target for tablets (overview §1.3, §3.2.3). |
| P-G5 | P | **A failed SUSPENDED pile returns a generic `OPERATOR_ACTION_REQUIRED`** (`domain/types.cpp:162-164`). It should be an explicit dead-end state (overview §3.2.7). |
| P-G6 | R | **Manual mode cannot show the plan** that Auto would execute (overview §3.3.4). The `apply_decision` path needs a qualified, confirmed decision (`runtime/controller.cpp:817-828`). |
| P-G7 | D | **One contradicting reply blocks the decision.** A blocked round then resets confirmation (`domain/policy.cpp:99-101`, `runtime/controller.cpp:657-662`). |

## Intended behaviour and design

### P1. Confirmation (decision D8): `TConfirmations`

A pure struct that the actor updates on every evaluation. For each pile it keeps:

```
{Generation, DownSince, NewestEvidenceSentAt}
```

**Down** means the verdict is `Failed` or `Lost`. Switching between the two keeps confirmation.

**Start and reset rules:**

- On the first Down evaluation at generation G, set `DownSince = now`.
- Reset on any evaluation where the pile is not Down.
- Reset all piles when G changes.
- UNKNOWN resets too. This is conservative and keeps the rule simple.

**Confirmed** requires both:

- `now - DownSince >= confirm_seconds`;
- the verdict's `NewestEvidenceSentAt > DownSince`.

The second condition guarantees that at least two separately sent observations agree (overview §3.1.2). Replies that only get older can never confirm a pile.

### P2. Decision: `Decide(view|blocker, verdicts, confirmations, pending, endpoints, now) → TDecision`

The steps run in order. The first one that produces a result ends the decision.

1. **View blocker.** `NO_VIEW`, `VIEW_CONFLICT` and `STALE_HISTORY` are returned as `Blocked`.
2. **SUSPENDED pile lost.** If any SUSPENDED pile is `Lost`, return `Blocked(SUSPENDED_PILE_LOST)`. No action is taken: every commit still needs that pile's quorum, and public recovery is postponed (overview §3.4).
3. **Collect piles.**
   - `D` is the set of participating piles whose Down status is confirmed.
   - `C` is the set of Down piles that are not yet confirmed.
   - If `C` is not empty, return `Confirming(C ∪ D)`. Skipper acts only when every Down pile is confirmed, so all of them go into one request. A sequence of single disconnects can fail, because each intermediate state still needs the quorum of the other failed piles (overview §3.3.3).
   - If `D` is empty:
     - every participating pile `Healthy` or `Replicating` → `Stable`;
     - otherwise → `Unknown`, listing the unknown piles. This addresses P-G3 and overview §3.2.6.
4. **Survivor eligibility.** Let `E` be the effective primary, which hosts BSC: the PROMOTED pile if one exists, otherwise the PRIMARY (overview §1.1, §1.3). A pile S is *survivor-eligible* in either of these cases:
   - S is `Healthy`; or
   - (decision D5) all of the following hold:
     - `E` is in `D` with verdict `Lost`;
     - the current evaluation has no valid BSC-mode reply (`BscAvailable` is false);
     - S has `SelfQuorum`;
     - S has no `FallbackRed`.
5. **Primary not in `D`.**
   - The primary must be survivor-eligible; otherwise return `Blocked(PRIMARY_UNKNOWN)`.
   - The update is `D → DISCONNECTED`.
   - A PROMOTED pile in `D` is simply disconnected, and the server falls back to the primary (`ydb/core/grpc_services/rpc_bridge.cpp:327-329`).
   - This branch also covers a dead PROMOTED pile during a switchover. BSC dies with it, so the stored primary can only be survivor-eligible through D5 (overview §3.2.3).
6. **Primary in `D`.**
   - Candidates are the PROMOTED pile, then SYNCHRONIZED piles, all outside `D`, in name order. Trying PROMOTED first addresses P-G4.
   - Choose the first survivor-eligible candidate X. If there is none, return `Blocked(NO_CANDIDATE)`.
   - The update is:
     - `D → DISCONNECTED`;
     - `X → PRIMARY`;
     - `P' → SYNCHRONIZED` for any other PROMOTED pile `P'` that is neither X nor in `D`.
7. **Survivor quorum.** Every pile that stays in the configuration quorum must have `has_node_quorum` in at least one valid connectivity report. Otherwise return `Blocked(SURVIVOR_QUORUM_UNKNOWN)`.
   - This avoids an ambiguous INTERNAL_ERROR that would pin the pending record.
   - These piles are PRIMARY, PROMOTED, SYNCHRONIZED, participating `NOT_SYNCHRONIZED` and SUSPENDED piles not in `D`.
8. **Validate the update.** Run `ValidateExactUpdate` ([admin](rework_plan-admin.md) A4). On failure return `Blocked(INVALID_UPDATE)`.
9. **Mutation endpoint.** Call `SelectMutationEndpoint` ([admin](rework_plan-admin.md) A5). If it finds none, return `Blocked(NO_MUTATION_ENDPOINT)`.
10. **Pending mutation.** If a pending mutation at G has a different update, return `Blocked(PENDING_MUTATION)`.
11. **Ready.** Return `Ready(TPlan{G, update, endpoint})` together with `CliCommand`.

**Partition (decision D6).** No special rule is needed, and the side that hosts BSC (that is, `E`) wins:
- BSC-mode replies from `E`'s side mark the far side `Failed`, while `E`'s side stays `Healthy`.
- Neither side is `Lost`, because each side's reports claim its own quorum.
- If no BSC evidence is available, the decision is `Unknown` or `PRIMARY_UNKNOWN`.

During a switchover `E` is the PROMOTED pile. If that pile is cut off from the stored primary, the far side that gets disconnected can include the stored PRIMARY. Step 6 then chooses the PROMOTED pile as the new primary.

**All failed piles in one request.** A single update covers all of them, and a role change does not count as a stored-state change (overview §3.3.3, §4.1).

### P3. Overlay construction

Move the failover vector logic from `durable/workflow.cpp:84-120` into `domain/policy.cpp`:

- the batch disconnect;
- the explicit PRIMARY;
- the explicit reset of an abandoned PROMOTED.

### P4. CLI rendering and modes (decision D2)

**CLI command.** `CliCommand` is:

```
ydb -e <endpoint> admin cluster bridge update --set <pile>:<STATE> … --expected-generation <G>
```

- Pile names are shell-quoted.
- An empty pile name cannot be expressed with `--set` (`ydb/public/lib/ydb_cli/commands/ydb_bridge.cpp` `Parse`). In that case, render the `--file` JSON form.
- Credential flags are left to the operator.

**Modes.**

- `manual`: publish the decision and the CLI command. Never send.
- `auto`: send `Ready` plans through the [admin](rework_plan-admin.md) A6 protocol.

The decision is computed the same way in both modes.

## Removals

Everything below is removed from `domain/policy.cpp`, `domain/evidence.cpp` and `runtime/controller.cpp`:

- `EvaluatePolicy`'s profile branches and qualification gate;
- the global "complete vector" requirement and the contradiction rule;
- local and mutual-preference rules;
- the placement acknowledgement (`policy.cpp:332`);
- the evidence-expiry and dispatch-margin calculation (`:357-365`);
- the canonical decision ID and suppression keys;
- `TryAutomatic` rebinding;
- the HTTP `apply_decision` path.

Freshness is enforced per reply ([healthcheck](rework_plan-healthcheck.md)). Staleness of the decision is enforced by the server compare-and-set.

**Retained safeguards:**

- confirmation over time (§3.1.2);
- no action on UNKNOWN;
- exclusion based on lifecycle state, never on failed connections (§3.3.3);
- the single-request batch;
- explicit role changes;
- `ValidateExactUpdate`.

## Ordered tasks

1. **P1** `TConfirmations`. Needs the [healthcheck](rework_plan-healthcheck.md) H3 output type, which includes `NewestEvidenceSentAt`.
2. **P3** Overlay helpers.
3. **P2** `Decide`. Depends on P1, P3, and [admin](rework_plan-admin.md) A4 and A5 (A5 includes `SelectMutationEndpoint`).
4. **P4** `RenderCliCommand`, then wire modes into `TSkipperActor` ([core](rework_plan-core.md) C4).

## Acceptance criteria

- Manual mode against the mock publishes these plans after `confirm_seconds`:
  - when a synchronized pile dies: `{b: DISCONNECTED}` at G;
  - when the primary dies (fallback replies plus connectivity): `{a: DISCONNECTED, b: PRIMARY}` at G.

  In both cases the CLI command is identical in content to the plan.
- Auto mode sends exactly that plan once.
- A one-round RED blip causes no action.
- Every reason in the [index](rework_plan.md) blocker enum is reachable by a unit test: `domain/ut` covers all of them except `STATE_WRITE_FAILED`, which is covered in `runtime/ut` ([core](rework_plan-core.md)).

## Focused regression tests (`domain/ut`)

1. A synchronized pile fails: disconnect only (`SoleGlobalSurvivorDisconnectsAllFailedInOneVector` rewritten).
2. Several piles fail, the primary among them: one request, with the candidate as PRIMARY.
3. A PROMOTED pile (the BSC host) becomes Lost during a switchover, and only fallback replies remain. The stored primary has `SelfQuorum` and no `FallbackRed`. Expected: `{promoted: DISCONNECTED}`, with no role change. With `FallbackRed` on the primary, the result is `PRIMARY_UNKNOWN`.
4. The primary fails while a healthy PROMOTED pile exists: PROMOTED is chosen. When a SYNCHRONIZED pile is chosen instead, the other PROMOTED pile is explicitly set to SYNCHRONIZED (`ExactBatchPreservesOmittedAndDemotesAbandonedPromotion`).
5. A stage-2 pile fails together with the primary: one request (P-G2).
6. A SUSPENDED pile is lost: `SUSPENDED_PILE_LOST`, with no plan.
7. Primary loss with no BSC:
   - a candidate with `SelfQuorum` is chosen;
   - a candidate with `FallbackRed` is not;
   - when BSC-mode replies exist, a candidate with an Unknown verdict is not.
8. The primary is Unknown and no pile is Down: `Unknown`, not `Stable` (P-G3). The primary is Unknown and another pile is Down: `PRIMARY_UNKNOWN`.
9. Confirmation:
   - a single sample does not confirm;
   - an UNKNOWN in the middle resets;
   - a generation change resets;
   - Failed→Lost keeps the confirmation;
   - aged evidence with no newer `SentAt` does not confirm.
10. One pile is confirmed Down and another is Down but not yet confirmed: the result is `Confirming` for both, with no plan. Once both are confirmed, a single request disconnects both.
11. A survivor has no quorum report: `SURVIVOR_QUORUM_UNKNOWN`.
12. CLI rendering: quoting, and the empty-name `--file` form.

**Tests deleted with the old policy:**

- `domain_ut` confirmation and selection tests bound to `CanonicalDecision`, epochs or profiles (`:494`, `:516` are rewritten as test 9);
- `controller_ut` `QualifiedTwoRoundsDriveAutomaticDurableAdmission` (`:686`), rewritten as the mock end-to-end "two rounds → one guarded mutation" ([mock](rework_plan-mock.md) M5).
