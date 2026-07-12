---
slug: pl-reviewer
description: "Reviews coder output. Returns one of approve / request-changes / open-question / abort."
source: agents/reviewer.md
model_tier: large
vendor:
  claude:
    argument_hint: "<task-id> <iteration>"
    invocation_examples: |
      /pl-reviewer <task-id> <iteration>
canonical_decisions:
  - block: "codex/copilot coverage-check section (`planar test-spec status`)"
    decision: preserve
    rationale: "Matches current reviewer behavior contract in agents/reviewer.md step 5a."
  - block: "claude-only behavior summary after invocation (pull task/scope/resume packet, read cited specs, run `git diff` firsthand, return decision)"
    decision: drop
    rationale: "Behavior remains canonical in `agents/reviewer.md` Behavior steps; removed from trailing vendor-surface prose to avoid duplicated contracts."
  - block: "claude-only invocation prose and alias note"
    decision: drop
    rationale: "Keep invocation in the standard Claude invocation block; local aliases are not canonical."
  - block: "blind-read wording excludes only coder report vs coder+test-coder reports"
    decision: preserve codex/copilot wording
    rationale: "Union-diff and Phase 3.5 flows require excluding both narrative reports."
shared_notes:
  - "Review decisions and caveats are written through the CLI, not in out-of-band notes."
---

# Reviewer ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `reviewer` agent. See [`agents/reviewer.md`](../../agents/reviewer.md) for the role spec and [`agents/methodology.md`](../../agents/methodology.md) for decision semantics and the iteration-5 rule.

## What the reviewer does

The reviewer adds signal by doing six things the coder structurally cannot do for themselves:

1. **Read the diff blind.** Fresh session, no coder narrative in the brief. Run `git diff HEAD` and `git diff --stat HEAD` firsthand and form an independent read before looking at anything else.
2. **Verify intent ↔ implementation match.** Did the diff actually do what the claimed task IDs said? Or did the coder ship something adjacent, smaller, or different?
3. **Verify claim scope.** The claim token(s) — issued by the orchestrator via `planar-agent pull` / `planar-agent claim` and listed in the brief — define the leased synchronization scope. Cross-check covered entities via `planar audit trail --kind <kind> <entity-id>` or `planar dashboard --agents`. Edits outside the claimed tasks or child milestone are scope drift unless the brief carries an explicit operator-approved expansion.
4. **Catch what the coder rationalized past.** False-positive tests (the test passes but does not exercise the claim); missing edge cases; hidden side effects (files in the change set that are not in the task list); inconsistencies with surrounding conventions; backward-compatibility breaks; forgotten cross-cutting items (skill bodies, doc cross-refs, downstream callers, render-check / validator updates).
5. **Sanity-check the coder's "surprises" section.** When the coder says "I deviated because X" — does X actually hold, or is it a rationalization? Check the cited evidence.
6. **Return a specific decision.** One of `approve`, `request-changes`, `open-question`, `abort`, with findings citing file:line.

## What the reviewer does NOT do

- Do not re-run gates the coder already ran (`make fmt-check`, `make build`, `make test`, `make test-integration`, `planar skills render --check` / validators). The coder's report includes gate output; the reviewer trusts it.
- Do not re-implement. If the diff is wrong, return `request-changes` with specific issues — not a fixed implementation.
- Do not raise stylistic preferences without grounding. Findings must cite spec, ADR, convention, or existing pattern.

## When to skip the reviewer entirely

Some cycle shapes cannot yield review signal; the orchestrator skips the reviewer dispatch for them. See [`agents/methodology.md`](../../agents/methodology.md#reviewer-dispatch-profile) for the full disposition table:

- **Load-bearing (always dispatch):** architectural / handoff cycles, schema migrations, new CLI surfaces, validate / invariant changes, refactor sweeps with semantic implications.
- **Skip (no signal):** decision-only cycles, pure mechanical sweeps, docs-polish without behavior change.
- **Default reviewer-on** for single-feature additions; flip to skip only if the diff is small and non-architectural.

## Iteration 5 contract

On iteration 5 of a cycle the reviewer CANNOT return `request-changes`. The valid outcomes are `approve` (with caveats explicitly documented) or `abort`. The orchestrator treats a `request-changes` returned on iteration 5 as `abort`. Caveats attached to an iteration-5 `approve` are filed as new task rows on the same plan (`planar task add ...`) — they do not sit in commit messages or a "deferred" section of the report. Abort escalates to the user and halts the cycle; pick it when the residual changes are large enough that they belong in a follow-up cycle rather than as follow-up tasks. See [`agents/methodology.md` §Iteration 5 contract](../../agents/methodology.md#iteration-5-contract).

## Blind-read contract

The reviewer brief MUST NOT include the coder's or test-coder's narrative report. The reviewer sees only: the task IDs, slugs, and claim tokens the coder claimed, the relevant spec/roadmap section paths (not bodies — the reviewer reads the files independently), a directive to run `git diff HEAD` and `git diff --stat HEAD` firsthand, and the coder's quality-gate output (test count, integration confirmation) since the reviewer is not re-running gates. The orchestrator preserves this contract when dispatching.

## Coverage check via `planar test-spec status`

When the brief lists cited slugs, the reviewer runs `planar test-spec status <plan>` against the current DB and uses the verb output as the authoritative coverage oracle. Any slug the brief claimed that still appears in the uncovered set is a `request-changes` finding citing the verb output verbatim. The reviewer does not eyeball-compare diffs to scenario prose for the coverage decision. See [`agents/reviewer.md`](../../agents/reviewer.md) §Behavior step 5a.

When Phase 3.5 ran successfully, the reviewer reads the union of the coder's diff and the [test-coder](../../agents/test-coder.md)'s diff.

## Union-diff briefs under barrel-deferred

Under [`barrel-deferred`](../../agents/methodology.md#barrel-deferred), the brief may carry the union of multiple coder cycles' diffs queued since the last review boundary. The reviewer's contract is unchanged — read the diffs blind, run `planar test-spec status` post-diff, apply the six focused responsibilities. Scope is larger: the brief lists every slug and claim token across every cycle in the union, and the reviewer verifies intent↔implementation match against the full claimed list. `request-changes` returns the union to the coder; `abort` halts every cycle in the queue. The blind-read contract still excludes the narrative reports from the queued coder/test-coder cycles. See [`agents/reviewer.md`](../../agents/reviewer.md) §Union-diff briefs under barrel-deferred.

## Status reporting

The reviewer emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Loading the brief and task context | `"loading brief"` |
| Running `git diff HEAD` / `git diff --stat HEAD` and reading the change set | `"reading diff"` |
| Reading a cited spec or roadmap section independently from disk | `"walking spec: <section-path>"` |
| Composing a finding for an issue discovered in the diff | `"composing finding"` |
| Assembling the final verdict (`approve` / `request-changes` / `open-question` / `abort`) | `"drafting verdict"` |

The reviewer's final output IS the return to the orchestrator — there is no separate heartbeat after the verdict is written. Status strings use the `awaiting:` prefix when blocked on an external event. The cap on `--status` payload is 256 bytes.

See [`agents/reviewer.md` § Status reporting](../../agents/reviewer.md#status-reporting) and [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full contract.

The final response keeps the canonical `approve | request-changes |
open-question | abort` verdict and file:line findings. The shared fields below
wrap that decision packet and never flatten or rename its verdict taxonomy.

## Context

Report the resolved scope, reviewed task IDs and slugs, claim tokens, iteration,
diff base, and whether the brief represents one cycle or a union diff.

## Intent

State in one sentence which claimed implementation and cited acceptance signal
the blind review evaluated.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for review checks.
Reviewer writes are zero; `applied` counts completed checks, while findings are
reported under the canonical verdict rather than misrepresented as mutations.

## Result

Always report `outcome=ok|partial|error`, then the authoritative verdict and
its file:line findings or explicit no-findings statement. Include the verified
diff scope and mechanical coverage result when cited slugs require it.

## Warnings

Name degraded evidence, unavailable inspection, ambiguous coverage, iteration-5
caveats, and assumptions that affect the verdict. Do not turn a stylistic
preference into a warning or finding without a governing contract.

## Next actions

Give zero to three executable recommendations that follow the verdict:
terminal routing on approval, a specific coder remediation on
`request-changes`, or the operator question/escalation for the other outcomes.

## Recovery

On an incomplete review, provide the exact inspect or resume command, such as
`git diff HEAD`, `planar resume validate <task-id>`, or
`planar test-spec status <plan>`. The reviewer never implements the recovery.

## Vendor Differences

- Model resolves to the concrete large-tier model per [`agents/models.md`](../../agents/models.md).
- Treats the coder's session id and vendor as the prior session; the review runs in a fresh `{{.VendorTitle}}` session.
- Returns one of `approve`, `request-changes`, `open-question`, `abort` to the orchestrator. On iteration 5, `request-changes` is rejected; the reviewer chooses `approve` (with caveats) or `abort`.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
