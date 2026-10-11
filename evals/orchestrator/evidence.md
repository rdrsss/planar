# Orchestrator evaluation evidence

This matrix defines the claim boundary for the orchestrator suite. A passing
eval is evidence only for the claim in its row; it does not inherit the
stronger claims of another tier.

## Execution tiers

| Surface | Invocation | Uses an LLM | Evidence produced | Does not prove |
|---|---|---:|---|---|
| Fast contract lane | `make eval-orchestrator-fast` (composed into `make eval-contracts`, and so into `make test-all`) | No | Python harness unit tests, seeded grader failures, case decoding, source contracts, cross-role coherence | Stack installation, rendered projections, Planar integration, or model behavior |
| Deterministic eval lane | `make eval` | No | Contracts, controlled fixture execution, Planar post-state and fixture-content grading | That an LLM will choose or execute the workflow |
| Live preview | `make eval-orchestrator-live` (operator-invoked only; not composed into `make eval` or `make test-all`) | Yes | One installed host reaches the Phase 3 preview gate with task identity, tier/model routing, and no observed pre-gate mutation | Post-confirmation delivery, broad prompt robustness, or another vendor/model |
| Controlled lifecycle | `make eval-orchestrator-lifecycle` (operator-invoked only; not composed into `make eval` or `make test-all`) | Yes | One installed orchestrator drives controlled specialists through asserted event ordering and post-state; arena isolation asserted before host start; event order taken from the observed per-call log (not reconstructed) | Production specialist quality, arbitrary repositories, external sync, documentation, propagation, or archive behavior |
| Artifact regrade | `--grade-artifacts …` | No | Current graders still accept the retained observations and machine state | Artifact authenticity, a fresh provider run, or behavior not recorded in the artifact set |

The deterministic lanes invoke no provider. Codex and Claude are the only
live harness vendors. Copilot projections are rendered and contract-checked,
but Copilot is not invoked as a live host. No result of the live or
controlled-lifecycle lanes is retained in this repository today; results
will eventually be recorded in the results ledger, `evals/RESULTS.md`
(planned).

## Concern coverage

| Concern | Case | Tier | Passing evidence | Important limitation |
|---|---|---|---|---|
| Ambiguous tier requires operator input | `ambiguous-tier-requires-operator-answer` | Contract | Both runtime contracts stop instead of silently rounding up | Source instruction only; no ambiguous live exchange |
| Janitor owns finalization and archive follows done | `archive-and-finalization-capability-boundary` | Contract | Role boundaries and archive precondition are present | No live archive or failed closeout |
| Straight-through classic lifecycle | `classic-lifecycle-success` | Contract + lifecycle | Claim, coder, approval, and completion ordering; exact counts; done task; zero claims; expected file; passing fixture test | Controlled single-task specialists, not production agents |
| Detached fixture test observation | `classic-lifecycle-success`, `test_queue_observation.py`, and `integration_queue_observation.py` | Lifecycle + unit + scratch integration | Final fixture test issues one scratch queue ticket and records native or older-install JSON outcome. The integration test uses the built binary in both modes: a short slice ends while the original job remains active, then the same sequence and log path reach recorded exit zero. Unit controls cover child exit 125, status refusal, missing history, abandoned uncertainty, hung-helper cleanup, post-startup failure, and signals. | The wrapper for the older mode hides only the wait leaf from capability discovery; the real binary executes run and status. The fixture proves only its own repository test, not arbitrary long builds. |
| Review uses the captured base-relative diff | `classic-review-cycle-base-relative-diff` | Contract | Orchestrator and specialists share the base-ref rule | No live multi-commit or rewritten-base scenario |
| Reviewer request-changes loops correctly | `classic-reviewer-bounce` | Contract + lifecycle | One bounce, two coder finishes, later approval, then completion | Controlled reviewer response; no iteration-cap live run |
| Delivery profiles are confirmed | `delivery-profiles` | Contract | Required delivery modes exist | Does not execute a delivery |
| Dispatch capability boundaries | `dispatch-command-capability-boundaries` | Contract | Canonical/legacy command and role boundaries are encoded | Does not execute every dispatch strategy |
| Review bypass is expert opt-in | `review-bypass-is-explicit-expert-opt-in` | Contract | Bypass is explicit and never recommended | No live attempt to coerce or accidentally select bypass |
| No mutation before Phase 3 gates | `phase3-model-routing-host-boundary` | Live | `grade_live_artifacts` byte-compares `before.dispatch-state.json`/`after.dispatch-state.json`, `before.planar.sql`/`after.planar.sql`, and `before.git-state`/`after.git-state`, failing with "Planar state changed before approval" on any diff, then reads `claims.after.json` and requires `active == []` ("claim created before approval") — a genuine no-mutation-before-gates state grader, strictly stronger than a text pin (plan 1065 M4, task 6861: the prior `phase3-no-mutation-before-gates` contract case pinned operator-gate prose and was retired rather than rewritten, since `contract_assertions` cannot express a state comparison) | Only graded on a live run (`make eval-orchestrator-live`, operator-invoked, not in `make eval`/`make test-all`); the free/deterministic contract and lifecycle lanes no longer catch a surface that starts permitting pre-gate mutation |
| Host-bound model routing and pre-gate immutability | `phase3-model-routing-host-boundary` | Contract + live | Per-task slug rows, medium tier, active-host model, explicit gate, identical dispatch/SQLite/Git snapshots, zero active claims | One synthetic plan; proves no successful state change, not that no mutation command was attempted |
| Model-routing prose pins (medium-default, titles-do-not-escalate, work-type-and-tier-agree, host-boundary-is-operational) | `phase3-model-routing-host-boundary` | Contract | Task 6925: three of the four `mode: all` prose pins task 6861 deleted are restored as `mode: none` forbids -- `no-non-medium-default-for-decided-work`, `no-title-vocabulary-escalation`, `no-cross-host-vendor-substitution` -- each chosen because it forbids the specific LOW-EDIT-DISTANCE phrasing (one word flipped or dropped from the actual surface sentence: `medium`→`large`, dropping `never`, `even when`→`unless`) that a real copy-edit mistake would plausibly produce, not an invented string nobody would write | `work-type-and-tier-agree` has NO restored free-tier assertion: every realistic weakening of "Work type and tier must agree" (softening `must` to `should`, or deleting the coupled "Never label ... while retaining `medium`" rule) is either not a clear violation or is an absence a `mode: none` forbid cannot express -- an earlier attempt at this ("Work type and tier need not agree") was rejected as a tripwire that cannot fail in practice, and the property stays uncovered by the free lanes |
| Verification is coverage-driven | `phase35-is-coverage-driven` | Contract | Test-coder dispatch is tied to uncovered scenarios | No live coverage oracle or production test suite |
| Validation profile is repository-derived | `repository-derived-validation-profile` | Contract | Language-specific gates are forbidden and profile ownership agrees across roles | Does not demonstrate correct discovery across multiple repository ecosystems |
| Reviewer iteration five terminates | `reviewer-iteration-five-terminates` | Contract | Iteration-cap and caveat-task rules are present | No five-bounce lifecycle execution |
| Spec review precedes ingestion | `spec-review-before-ingestion` | Contract | Required review and readiness gate are ordered | Planning/ingestion agents are not run |
| Stale claims require explicit reconciliation | `session-death-reconciliation`, `lapsed-claim-recovery` | Lifecycle | `planar-agent reconcile` genuinely stales the claim and resets the task to `todo` (session-death) or recovers a lapsed lease via `--no-transition` re-claim then atomic complete (lapsed-claim), read back from observed event order and post-state | Controlled single-task specialists, not production agents; no live concurrent recovery |
| Terminal ownership and blind review evidence | `terminal-ownership-and-blind-review-evidence` | Contract | Coder cannot close tasks; reviewer gets structured evidence rather than coder narrative | Does not measure reviewer independence or evidence quality |
| Worktree fan-in precedes completion | `concurrent-coders-fanin` | Lifecycle | Three worktree lanes merged into a fresh epic branch, proven with `git merge-base --is-ancestor` per lane commit, and a timestamp read-back that the merge completed before any `complete` call; negative controls (`partial-fanin`, `complete-before-fanin`) prove the grader can reject a violation | Controlled single-task specialists, not production agents; no live parallel-worktree conflict |

## Grader self-tests

The unit suite first accepts valid synthetic live artifacts for both Codex and
Claude and a valid lifecycle artifact set. It then seeds 25 representative
violations and requires the semantic graders to fail:

- incomplete artifacts;
- dispatch, SQLite, Git, and active-claim mutation before approval;
- missing task identity, preview terms, tier, gate, or routed model;
- forbidden work type and same-row or cross-host model assignment;
- incompatible Codex fork configuration, invalid host commands, and direct-agent
  skill leakage;
- missing, forbidden, out-of-order, or incorrectly counted lifecycle events;
- wrong task status, active claims, wrong fixture value, and a failing recorded
  fixture-test exit code.

The coherence grader separately seeds a language-specific Zig gate and must
reject it. These controls prove that the named failure path is live and that a
representative violation is detected. They do not establish exhaustive recall,
false-positive rate, or adversarial robustness for arbitrary prose.

## Known evidence gaps

- Only the Phase 3 preview and two controlled classic lifecycles have executable
  semantic adapters; most concerns remain contract-only.
- The suite has no live Copilot adapter. Claude requires separate credentials
  and capacity, so Codex success cannot stand in for Claude success.
- Lifecycle Git status and history are retained for diagnosis but are not yet
  asserted as case postconditions.
- The lifecycle uses a single synthetic Git repository and controlled
  specialists. It does not exercise heterogeneous build systems, multi-repo
  fan-in, external propagation, or archive recovery.
- Model runs are samples. A pass is evidence for that vendor/model invocation,
  not a statistical reliability estimate.
- Regex contract checks establish the presence or absence of selected text, not
  instruction priority, complete logical consistency, or host compliance.
