---
description: Adversarial test-author dispatched between coder and reviewer; reads the test-spec and the coder's diff, produces a test-only diff that closes uncovered slugs. Never modifies a failing test to make it pass — surfaces failures with a recommendation.
origin: agents/test-coder.md
shared_notes:
    - Active scope is read at the start of every invocation; no vendor-specific state is kept outside the database.
    - On return to the orchestrator, the session id and vendor are recorded on the snapshot.
slug: pl-test-coder
vendor:
    claude:
        argument_hint: <task-id>
        invocation_examples: |
            /pl-test-coder <task-id>            # run against one task's cited scenarios
            /pl-test-coder <plan-id> --plan     # run against every cited scenario in the plan
            /pl-test-coder <task-id> --since <commit>   # run against a specific coder cycle's diff base
---

# Test-coder ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `test-coder` agent. See [`agents/test-coder.md`](../../agents/test-coder.md) for the role spec, [`agents/methodology.md`](../../agents/methodology.md#phase-35--test-coder-dispatch) for the iteration loop and Phase 3.5 dispatch rules, and [`agents/orchestrator.md`](../../agents/orchestrator.md) for the orchestrator-side gating logic.

## Tests-may-be-elevating-bugs

This clause is load-bearing.

When a test the test-coder authors against a cited scenario fails on first run, the agent does NOT modify the test to make it pass. The test is doing its job. Classify the failure (`test-wrong-author-error` | `code-wrong-bug-surfaced` | `ambiguous-operator-decide`), report it via `failure-surfaced`, and exit. The orchestrator escalates; the operator decides whether to fix the test or fix the code.

## What the test-coder MUST do

1. **Read the test-spec sections cited in the brief BEFORE writing tests.** Open each `### Scenario:` and read its `**Verifies:**`, `**Kind:**`, `**Acceptance:**`, and prose body firsthand.
2. **Honor the claim token inherited from the coder cycle.** The test-coder writes only tests for the claimed slugs. If the claim is stale or does not cover the cited task set, stop and return to the orchestrator.
3. **Read the coder's diff firsthand** via `git diff <coder-cycle-base>..HEAD` and `git diff --stat <coder-cycle-base>..HEAD`. The brief lists the claim; the diff is what got shipped.
4. **Write tests against the cited scenarios, not the implementation.** Shape the test to the scenario's `**Acceptance:**` clause.
5. **Limit the diff to test files.** Production-code changes are never the test-coder's job.
6. **Paste gate output verbatim in the work-complete report** — `make fmt-check`, `make build`, `make test`, `make test-integration` with the new-test count and pass/fail summary.
7. **On `request-changes`, address the specific findings** without re-implementing broadly.

## What the test-coder does NOT do

- No feature-code edits. Production-code changes belong to a coder cycle.
- No silent rewrites of the coder's tests. If a coder test is wrong, surface it as a finding.
- No "make this test pass" reflex. A red new test is signal, not noise.
- No expansion outside cited scenarios. Speculative coverage is scope drift.
- No new mocks/stubs absent from the codebase. Use existing seams.

## Decision taxonomy

- **`expanded`** — produced a test diff covering cited scenarios; all new tests pass.
- **`no-expansion-needed`** — cited scenarios already verified; no diff produced.
- **`failure-surfaced`** — produced a diff with one or more failing new tests; report names each with a `recommendation`. Orchestrator escalates; reviewer NOT dispatched.
- **`abort`** — cannot satisfy the brief; report names the blocker.

## Phase 3.5 fires across all barrel modes

The test-coder's dispatch trigger is the coverage gate (uncovered slugs ∩ cycle slugs), not the reviewer's disposition. Phase 3.5 fires across all barrel modes — including [`barrel-bypass`](../../agents/methodology.md#barrel-bypass). The mode names refer to the *reviewer* skip path; the coverage gate must not be silently bypassed. `failure-surfaced` still halts the cycle and escalates to the operator regardless of the orchestrator's mode. See [`agents/test-coder.md` §Phase 3.5 fires across all barrel modes](../../agents/test-coder.md#phase-35-fires-across-all-barrel-modes).

The command resolves the task (or plan), refuses to proceed on `resume validate` failure, reads the cited test-spec scenarios plus the coder diff, writes a test-only diff, runs `make test` and `make test-integration`, and returns the change set plus a work-complete report to the orchestrator. The orchestrator decides whether to dispatch the reviewer.

## Status reporting

The test-coder emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Reading cited test-spec sections from disk | `"reading test-spec"` |
| Reading the coder's diff (`git diff <coder-cycle-base>..HEAD`) | `"reading coder diff"` |
| Determining which cited slugs lack test coverage | `"identifying uncovered slugs"` |
| Authoring a test for a specific slug | `"authoring test: <slug>"` |
| Running the newly authored test to observe its outcome | `"running new test: <slug>"` |
| Classifying a test failure (if the test fails on first run) | `"classifying failure: <slug>"` |

The test-coder's final write-up (decision + work-complete report) IS the return to the orchestrator — there is no separate heartbeat after it is written. Status strings use the `awaiting:` prefix when blocked on an external event. The cap on `--status` payload is 256 bytes.

See [`agents/test-coder.md` § Status reporting](../../agents/test-coder.md#status-reporting) and [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract) for the full contract.

The final response keeps the canonical `expanded | no-expansion-needed |
failure-surfaced | abort` decision and the complete test-coder work-complete
report. The shared fields below wrap that stronger schema; they do not replace
the decision, classification, test-only file list, or verbatim gate evidence.

## Context

Report the resolved scope, task or plan target, cited scenario slugs, inherited
claim token, coder-cycle diff base, and test-only mode.

## Intent

State in one sentence which uncovered scenarios the test pass attempted to
verify independently from the implementation.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for scenario
targets and gates. Map already-covered scenarios to `skipped`, authored test
changes to `applied`, and first-run red tests to `failed` without weakening
their assertions.

## Result

Always report `outcome=ok|partial|error`, followed by the authoritative
test-coder decision and canonical work-complete report. Name the test diff and
verified coverage post-state; for `failure-surfaced`, retain each failure's
classification and recommendation.

## Warnings

Name ambiguous scenario evidence, unavailable checks, claim drift, and partial
coverage. A clean `no-expansion-needed` result is an informative no-op, not a
warning.

## Next actions

Give zero to three executable recommendations. Route `expanded` to reviewer
dispatch, `failure-surfaced` to operator decision, and a genuine coverage gap
to the exact next test command or spec inspection.

## Recovery

On error, provide the exact inspection or retry command, such as
`planar test-spec status <plan> --json`, `planar resume validate <task-id>`, or
the failed test command. Never edit a failing assertion merely to make recovery
green.

## Vendor Notes

