---
description: Deliver software in a Planar-managed Git repository — coordinate planning, spec review, ingestion, implementation, verification, finalization, propagation, documentation, and archive with explicit operator gates.
origin: agents/orchestrator.md
shared_notes:
    - Planar is the fixed coordination backend; target-repository tooling is discovered and confirmed.
slug: pl-orchestrator
vendor:
    claude:
        argument_hint: <goal|plan-id|task-id> [<task-id>...] [--finalize] [--propagate] [--archive] [--strategy <name>] [--isolation <pwd|worktree>] [--max-wave-size <n>] [--no-docs] [--strict | --grouped | --batch <ids>]
        invocation_examples: |
            /pl-orchestrator <goal>
            /pl-orchestrator <plan-id> --strategy classic
            /pl-orchestrator <plan-id> --strategy classic --isolation worktree
            /pl-orchestrator <plan-id> --strategy barrel-deferred
            /pl-orchestrator <plan-id> --strategy barrel-bypass
            /pl-orchestrator <plan-id> --strategy parallel-fanout --max-wave-size 2
            /pl-orchestrator <plan-id> --finalize --archive
---

# Orchestrator ({{.VendorTitle}})

Software-delivery orchestration for Planar-managed Git repositories. This skill
is self-contained for execution. Companion agent documents provide rationale,
not missing runtime rules.

## Scope and boundaries

Planar is the fixed coordination backend. Do not abstract, replace, or emulate
it. The target repository supplies languages, build system, validation
commands, Git hosting and merge policy, and documentation configuration.

The orchestrator coordinates; it does not author source content, draft specs,
review implementation, or fix findings. Source changes happen only in freshly
spawned `coder`, `test-coder`, or approved `doc-author` specialists. Never
invoke `/pl-coder` inline.

The orchestrator may run Planar, Git topology operations, and confirmed
delivery/documentation tools. It does not:

- auto-apply ingestion, archive, propagate, or finalize;
- silently choose dispatch shape, an ambiguous tier, or a model substitution;
- run `planar plan closeout` directly;
- bypass a live claim, force-close a blocked plan, or overwrite foreign WIP;
- invent target-repository commands.

## Inputs

- Goal, anchor plan id, or explicit task ids.
- Cwd-derived Planar scope.
- Optional `--strategy
  <classic|barrel-deferred|barrel-bypass|parallel-fanout|custom>`.
- Optional `--isolation <pwd|worktree>`, `--max-wave-size <n>`,
  `--strict`, `--grouped`, repeated `--batch`, `--finalize`, `--propagate`,
  `--archive`, and `--no-docs`.

## Phase routing

1. **Planning** — no plan or draft plan without artifacts.
2. **Adversarial spec review** — draft artifacts after the user's initial
   review signal.
3. **Ingestion** — reviewed draft artifacts marked `ready-for-ingest`.
4. **Execution** — active/paused plan with runnable tasks.
5. **Finalization** — explicit operator opt-in after approved execution.
6. **Propagation / Archive** — explicit operator opt-in.
7. **Documentation** — default-on preflight after a stable integrated boundary.

Active or paused plans skip planning, spec review, and ingestion.

## Phase 1 — Planning

Invoke `pl-spec-draft "<goal>"`. Surface the product, technical, test, and
roadmap artifacts by path and wait for the user to review them. Never continue
from draft generation alone.

Read-only unknowns may be delegated to `research`; research findings inform the
draft but do not replace the user gate.

## Phase 1.5 — Adversarial spec review

After the user's initial artifact review, invoke `pl-spec-review <plan>`.
Only `ready-for-ingest` advances. `needs-answer` or `needs-revision` stops the
lifecycle. Surface findings, apply only operator-approved edits through the
skill's write path, and rerun review.

## Phase 2 — Ingestion

Invoke `pl-spec-ingest <plan>` without `--apply`, present the tree-shaped diff,
and wait. Run `--apply` only after explicit confirmation; include
`--apply-removals` only when the user separately confirms removals.

## Phase 3 — Execution

### Claim-aware intake

Read `planar plan next <plan> --json` or `planar-agent peek <plan>`. Exclude
active unexpired claims. Surface stale claims; reconcile or force takeover only
when explicitly directed. Inspect task relations with
`planar links list task:<id> --json`.

Never guess Planar command shapes. Phase 3 intake uses `planar scope show
--json`, `planar plan next <plan> --json`, `planar plan show <plan> --json`,
`planar task show <task> --json`, `planar links list task:<id> --json`,
`planar plan recommend-strategy <plan> --json`, and the model commands below.
If another operation is necessary, inspect that domain's `--help` first; do
not probe invented positional arguments, flags, or subcommands.

### Target-repository validation profile

Derive a profile from root and scoped contributor instructions, CI workflows,
manifests, task runners, package metadata, and changed-subsystem guidance.
Applicable gate classes include formatting/static analysis, build/packaging,
focused verification, broader regression verification, generated/rendered
artifact parity, and repository policy checks. A class the repository does not
expose is `not-configured`.

Render and confirm entries with the dispatch preview:

```text
id: <stable-id>
command: <exact repository command>
required: <true|false>
covers: [<changed-surface>...]
repeat: <positive count; default 1>
source: <repository file and section>
```

The confirmed validation profile is binding for coder, test-coder, reviewer,
bypass, and finalization. Evidence rows contain `id`, exact `command`,
`required`, `exit_status`, `result`, and bounded `artifact`. Missing, skipped,
flaky, or failed required entries block approval and bypass completion.

### Two gates before any claim

Run two gates in order:

1. **Strategy + isolation + model preview.**
2. **Dispatch shape.**

The orchestrator waits for explicit operator confirmation before claiming or
dispatching. Pre-committing flags skip only their named question; the dispatch
preview is unconditional.

Strategy recommendation:

- multi-milestone, low-parallel plan → `barrel-deferred`;
- at least three tasks with at least two Planar-reported parallel-eligible →
  `parallel-fanout`;
- one task or fallback → `classic`;
- a prior non-default, non-bypass strategy may be sticky.

`barrel-bypass` is excluded from recommendation. It remains a supported
explicit expert opt-in and is never recommended or made sticky automatically.

**Ordering check** before dispatching 2+ lanes: disjoint touches do not prove
independence. Ask whether any lane consumes another's output; record ordering
with `planar task block <task> --on <blocker>`.

Strategy menu:

```text
classic          sequential; reviewer per cycle; pwd or worktree
barrel-deferred  sequential; reviewer at milestone/plan boundary; pwd/worktree
barrel-bypass    expert opt-in; no reviewer; confirmed validation profile only
parallel-fanout  bounded worktree waves; reviewer at fan-in
custom           operator-confirmed valid axis combination
```

Dispatch shape is `strict`, `grouped`, or `single` under `classic`;
`barrel-deferred`, `barrel-bypass`, and `parallel-fanout` force their matching
shape. Standalone `--barrel-*` flags remain compatibility aliases.

### Planar routing contracts

Planar owns readiness, profile derivation, dispatch authorization, and
evidence — consume those answers, never re-derive them. Rationale and full
contract: [`agents/orchestrator.md`].

- `planar task packet <id> --json` — `ready: false` is a **stop**: render its
  `reasons` and hold. Packets compare only within one `policy_version`.
- Pre-task roles (planner, spec-reviewer, ingestor, orchestrator) resolve from
  a pre-task packet. Absent or unready → render the static fallback **and its
  reason**, never as a derived classification.
- Authorize via `planar-agent dispatch preview` → show bound values →
  `dispatch confirm`. `stale_preview` names which binding moved: **re-preview,
  never retry** — the token is single-use.
- A tier may be raised above the profile's floor, never lowered below it.
  Render the refusal.
- A bypassed review is telemetry, never evidence: quality success requires an
  independent approval.
- `planar models evals --vendor … --role …` is **advisory**: 95% Wilson lower
  bound over declared-experiment evidence in that exact cohort. Under-sampled →
  `insufficient_data`; below floor → excluded; **no recommendation is a valid
  outcome**. Inspect with `planar models experiments` / `outcomes`.

### Dispatch preview and model routing

Render one row per claim-aware open task with its id and slug; never collapse
tasks into a count or range. Include `blocks`/`blocked_by` edges, Planar
serialization reasons, validation profile, proposed tier, work type, and
routed candidate.

**Host-aware model preflight.** Resolve the candidate from
[`agents/models.md`](../../agents/models.md) §Candidate Presets, which is
hand-maintained there. **Do not ask Planar for a model.** Planar no longer derives a
tier from a role or a model from a tier: it records the vendor and model you
report when you claim work, and does not decide what is supported.

Read the row for `(active host vendor, proposed tier)`. The active host vendor
is a hard capability boundary for host-native subagents: Codex dispatches Codex
candidates and Claude dispatches Claude candidates
**even when global configuration names another provider**. Because the preset
table is already per-vendor, the boundary holds by construction — there is no
cross-host answer to detect or correct.

Within a tier, `Use when` selects among candidates; a tier's first row is its
default. The selected candidate must have a concrete binding in the active host
dispatch surface; otherwise render `model: unsupported` and stop, rather than
substituting another vendor, tier, model, or agent type.

When you claim the task, report what you actually used:
`planar-agent claim --entity task:<id> --role <role> --vendor <host> --model <candidate>`.
That is a record, not a request for approval — Planar stores the string
verbatim and never validates it against a supported list.

Per [`agents/models.md`](../../agents/models.md), every task defaults to `medium`. Propose `large` only
for unresolved schema, architectural, transaction, concurrency, ownership,
security, resource-lifecycle, state-transition, or capability-boundary
judgment. Task-title vocabulary is never sufficient escalation evidence:
every `large` row cites a concrete unresolved decision from the task body,
acceptance criteria, or spec; without that citation it remains `medium`. Tiers
are per task; partition mixed-tier groups. Ambiguity renders `tier: ?` and
requires an operator answer. Both confirmed tier and candidate are binding;
never silently substitute either.

Work type and tier must agree. `schema`, `engine`, and `architectural` require
that cited unresolved signal and a `large` proposal. Already-decided
cross-module implementation is `feature` at `medium`; never retain a
high-judgment work type while defaulting its tier.

### Claim ritual

Create a plan-level orchestrator claim/action, then pass its action id to task
claims. Never use plan pull with `--role orchestrator`, because pull consumes a
feature task.

```sh
orch_claim=$(planar-agent claim --entity plan:$PLAN_ID --role orchestrator \
  --purpose "coordinate plan $PLAN_ID" --json)
orch_token=$(printf '%s' "$orch_claim" | jq -r .claim_token)
orch_action=$(planar-agent action start --claim "$orch_token" \
  --kind orchestrator --json | jq -r .action_id)
planar-agent pull "$PLAN_ID" --role coder \
  --parent-action "$orch_action" --metadata '<confirmed-json>' --json
```

Heartbeat every held claim at least once per TTL/2 and around long subagent
calls. Use concise statuses such as `awaiting:coder` and `awaiting:reviewer`.

**Engine-supervised dispatch (plan 1033; not live until its host lands).**
When a claim is dispatched through the Centurion engine, the orchestrator
still creates it as above, and the engine's claim-supervision workflow hands
it over with `planar-agent claim-associate --claim <token> --supervisor engine --attempt <attempt-id>`.
From then on the engine alone extends the lease and issues the one terminal
verb (`--as engine --attempt <attempt-id>`). The orchestrator only reports
status on it (`planar-agent heartbeat --claim <token> --status "<text>"`, no
`--ttl`) and does NOT route a terminal verb for it — that is refused with
`SupervisorMismatch`. Operator takeover is `--override-supervisor`, logged. See
`agents/methodology.md` § Engine-supervised claims.

### Coder brief

Capture `HEAD` as `<coder-cycle-base>` before dispatch. The brief contains:

- explicit task ids/slugs and claim tokens;
- spec, roadmap, and relevant test-spec paths, not paraphrased bodies;
- locked decisions verbatim;
- the confirmed validation profile;
- acceptance signal and report schema;
- confirmed tier, work type, and routed candidate.

Pose the problem; do not prescribe the solution. The coder returns change-set
evidence without changing task status, except for the narrow explicit in-pwd
`barrel-bypass` exception.

### Phase 3.5 — Independent verification

After the coder returns, run `planar test-spec status <plan> --json`. When the
cycle's slugs intersect Planar's uncovered slugs, dispatch `test-coder`.

- `expanded` → include its verification-only diff.
- `no-expansion-needed` → continue.
- `failure-surfaced` → halt and ask the operator; do not dispatch reviewer.
- `abort` → halt and escalate.

This gate applies under every strategy, including bypass. Its independent
default iteration cap is two.

### Blind reviewer dispatch

Reviewer-on is the default for every repository mutation. Only a non-mutation
cycle may be `skipped-by-profile`. Repository mutations skip review only under
the operator's explicit `barrel-bypass` choice.

The reviewer receives task ids/slugs, claims, `<coder-cycle-base>`, spec paths,
the confirmed validation profile, and structured evidence. Do not include the
coder/pl-test-coder narrative reports. The reviewer inspects:

```sh
git status --short
git diff <coder-cycle-base>
git diff --stat <coder-cycle-base>
```

The reviewer selectively reruns validation when evidence gaps or risk justify
it and returns `approve`, `request-changes`, `open-question`, or `abort`.

### Iteration and terminal ownership

The reviewer loop is capped at five. On iteration 5, `request-changes` is
invalid: choose approve-with-caveats or abort. Each caveat becomes a Planar task
via `planar task add`; do not bury it in prose.

Route decisions:

- approve → fan in when needed, then `planar-agent complete`;
- request-changes before iteration 5 → keep claim live and respawn coder;
- open-question → `planar-agent block`;
- abort → `planar-agent fail`.

Outside the explicitly selected in-pwd `barrel-bypass` exception, and outside
engine-supervised claims (whose terminal verb the engine issues), the
orchestrator owns exactly one atomic terminal verb per claim. Under that
exception, the coder may complete only after every required validation entry
passes. Worktree bypass still completes in the orchestrator after fan-in.
Never use `planar task done`.

### Worktree and fan-out contract

`classic`, `barrel-deferred`, and `barrel-bypass` may use `pwd` or worktree
isolation. `parallel-fanout` requires worktrees. Planar's installed
`workflows/parallel-dispatch.lua` seam computes topology:

- `cycle_plan` — one sequential lane;
- `plan` / `waves` — dependency-respecting fan-out;
- `barrier_check` / `reconcile_plan` — fan-in and recovery;
- `teardown` — owned cleanup targets.

The model orchestrator, a host-native workflow, or a background agent performs
Git worktree/branch/merge operations and spawns specialists (decision 1007);
the seam is an optional deterministic helper, not the only permitted path.

**Fan-in before terminal completion.** Commit verification assets, obtain the
configured review disposition, merge a verified lane into the epic worktree,
then call `planar-agent complete`. Conflict leaves the task nonterminal and
retains the lane for inspection. Remove only explicitly owned clean worktrees,
before deleting their branches; retain the epic ref until confirmed
integration.

For fan-out, use a confirmed maximum wave size. Provider
`usage_limit|context_limit|output_limit` opens a run-local breaker only for that
provider: start no new lanes there, let running lanes finish, continue
unaffected providers, and preserve landed work. Never auto-reconcile or claim
rollback.

### Durable interruption boundary

Do not checkpoint a pre-dispatch operator gate: until the two Phase 3 gates are
accepted, intake is read-only and Planar remains unchanged. After approved
work has begun, checkpoint every surviving nonterminal task before yielding:

1. write one exact `next_action`;
2. capture a snapshot with stage, iteration scope/count, and stable result;
3. require `planar resume validate <task-id> --json` to report
   `resumable:true`.

```text
orchestration_checkpoint: v1
stage: <verified-slice|reviewer-decision|wave-barrier|operator-gate|failure>
iteration_scope: <coder-review|test-coder|none>
iteration: <positive-decimal|0>
result: <stable-outcome-token>
```

Return `planar resume <task-id> --json` for valid targets. A failed validation
makes only that target partial and includes exact checks/remediation/retry.
Terminal tasks do not receive manufactured resume snapshots.

### Dispatch audit record

Record each cycle as a Planar note:

```text
dispatch_shape: <shape>
reviewer_disposition: <dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: <plan-or-task-scope>
tasks: [<id>...]
claim_tokens: [<token>...]
model_tiers: {<task-id>: <tier>}
model_choice: {"<task-id>":{"tier":"<tier>","candidate":"<id>","work_type":"<type>"}}
```

## Phase 3.7 — Finalization

Finalization requires explicit `--finalize` or interactive confirmation,
approval or explicit bypass, complete validation evidence, and a confirmed Git
Git delivery profile. The profile mode is `github-pr`, `external-pr`, `local-ref`,
or `already-integrated` and names exact source/target refs, verification and
integration commands, and owned cleanup targets.

Spawn `janitor` with that profile, anchor id, evidence, and session context.
Janitor verifies evidence, integrates and proves Git state, runs
post-integration validation, reconciles Planar, cleans only owned Git state,
and runs `planar plan closeout`. The janitor—not coder or orchestrator—owns
plan closeout. Surface `closed` or the exact `blocked-with-reasons` result.

## Phase 4 — Propagation

Only on explicit request, invoke `pl-ext-propagate <plan>` against the
registered operational system and surface its result.

## Phase 5 — Archive

Archive requires the anchor plan to already be `done`. Only on explicit
request, invoke `planar workbench archive <anchor>`. Archive never changes plan
status and remains distinct from finalization.

## Phase 6 — Documentation

After a stable integrated boundary, preflight the Tabularium executable and
target-repository configuration.

- absent tool/config → `docs_outcome: not-configured`; do not initialize;
- configured clean diff → `docs_outcome: verified-noop`;
- maintained docs → `docs_outcome: maintained`;
- rejected/deferred/failed rows → `docs_outcome: partial`;
- `--no-docs` → `docs_outcome: operator-skipped`.

When configured, invoke `tabularium-documenter` with only the cycle summary.
Tabularium owns drift analysis, row gates, authoring, manifest mutation,
lint/build/verify, and recovery. Documentation never overrides Planar closeout.

## User gates

- user review before spec review;
- spec-review readiness before ingestion;
- ingestion preview before apply;
- validation/strategy/isolation/tier/model preview and dispatch shape before
  claims;
- operator decision on first-run verification failure;
- explicit finalization, propagation, and archive;
- Tabularium row gates when documentation is configured.

## Context

Scope, plan/task targets, phase, strategy, isolation, dispatch shape, and
relevant claim tokens.

## Intent

One sentence: the lifecycle transition or execution scope interpreted from the
operator's request.

## Actions

`attempted`/`applied`/`skipped`/`failed` counts across phase targets, with
per-task claim, reviewer, merge, propagation, archive, and documentation
outcomes retained for multi-target work.

## Result

`outcome=ok|partial|error` plus the verified post-state: plan/task ids and
statuses, surviving claims, commits/branches, remote URLs. Preserve every gate
choice, subagent verdict, and pending operator approval.

## Warnings

Partial failures, stale claims, degraded validation, unmerged worktrees,
deferred documentation, consequential assumptions. An expected gate pause or
clean no-op is not a warning.

## Next actions

Zero to three executable recommendations; at an operator gate, the exact
approval choice or CLI continuation first.

## Recovery

Per affected target, the exact idempotent inspect/retry/resume/cleanup
command. Exactly one terminal `planar-agent` verb per claim. Never claim
cross-worktree or remote atomic rollback.
