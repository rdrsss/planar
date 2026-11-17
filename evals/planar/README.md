# Planar evaluations

## Case and experiment status

Run `make evals-test` from the repository root for the model-free adapter,
recovery, revision, retained-A/fresh-B and experiment-control suites. Docker/native integration
controls are separate; synthetic reports and rollouts do not prove a model
performed the workflow. Live execution needs the exact matching independent
one-use gate and confirmed cleanup.

| Case or path | Available evidence and scope |
| --- | --- |
| `cases/plugin-smoke` | Historical first diagnostic; original online infrastructure error/reward 0 and separate narrower cached assessment. |
| `cases/native-lineage-smoke` | Historical frozen version-1 lineage/artifact contract with an independently audited online reward 1. |
| `cases/native-lineage-current` | Separate frozen successor with one independently audited online reward 1 for its bounded lineage/artifact contract. Its name does not select an arbitrary current package. |
| `cases/fresh-interrupted-review` | Recovery fixture, independent verifier and retained-export assessments; these do not establish every recovery scenario or a final-byte live pass. |
| `cases/clean-revision` | Two fresh native parents in a persistent workspace, controlled post-worker STATE fault and separate verifiers. Actual trials have stopped at A with reward 0; corrected, versioned retained-A assessments do not establish B, repair or revision success. |
| `cases/retained-a-fresh-b` | Separate source-bound continuation case. Real no-auth Harbor/Docker restore, sole fault and network-none verifier handoff are observed with synthetic target logs. No real B has run. Structural-positive remains incomplete/reward 0 pending independent raw audit. |
| `cases/native-lineage-portable` | Opt-in schema-2 template/materializer with explicit inventory/execution data. The template's unbound placeholders are invalid inputs, not a runnable case. |
| `experiments/cli.py` | Schema-1 controls and separately gated Codex execution; experimental schema-2 planning, image preparation, exact reservation/gate, execution, reconciliation and reporting routes. Schema-2 source/control review is complete; a gated native run remains unproven. |

The current-lineage audit is project-local
`reviews/t046-live-baseline-audit-20261117.md`; the retained-A semantic audit is
`reviews/t045-b-alias-live-audit-20261117.md`. These reports are not included in
a repository-only clone. Full TEST-S69/S70/S71/S72 acceptance, complete profile
and session-audit matrices, and Claude/cross-vendor runtime proof remain open.
Fresh cases and runtime integrations under development are not authorized by
this inventory.

## Historical smoke evidence

The diagnostic is one Harbor 0.26.0 Docker job using Codex CLI 0.169.0, one
attempt and concurrency one. It installs the frozen package in an isolated
Codex home. The requested worker output is `RESULT.txt` with exact bytes
`32 0a`; the retained execution produced those bytes.

`cases/plugin-smoke/candidate.json` inventories 37 files from candidate commit
`1ef833747ae46fc58b8ecfd0ec46231b301b8185`. Its canonical inventory SHA-256 is
`2e229df68784143220d6550bed33719016b1547689ac5a25d5d9f1e4cb1f3368`.
The inventory hashes each file, then hashes the compact sorted-key JSON array
of path/hash objects. It rejects symlinks and empty trees. The installed cache
inventory was checked against the frozen candidate.

The native audit found host enumeration of the installed skill root and seven
Planar skills before the first assistant action. It also found an actual fresh
native child, matching parent and child lineage, requested model and effort,
installed resource reads, and the correct exported result. Enumeration does
not establish automatic skill activation. Encrypted spawn and child task
payloads do not prove complete plaintext role delivery; requested settings and
runtime context do not independently prove backend model or effort identity.
Agent-writable native logs are bounded diagnostic evidence, not adversarial
attestation.

The original online separate verifier ran without networking and reported
`infrastructure_error`, reward 0, because native dispatch evidence verification
was not implemented. A separately reviewed, network-isolated cached regrade
passed its narrower dispatch scope against the retained execution. It does not
change the online outcome or establish TEST-S69 or full T-044 acceptance.
This original smoke does not establish recovery, cross-vendor, quota or
experiment acceptance. Later bounded work is distinguished above.

The project's `reviews/harbor-t044-native-audit-20261116.md` and
`reviews/harbor-t044-regrade-final-review-20261116.md` detail the evidence and
its limits. Repository unit controls are run from the repository root with
`uv run --project evals --locked python -m unittest discover -s evals/planar/adapters -p 'test_*.py'`.
Project-local no-auth Docker integration controls and their prerequisites are
documented in `reviews/tools/harbor-t044/README.md` relative to the project
directory; they do not travel in a repository-only clone.

`cases/native-lineage-smoke` is a separate version 1 case for recorded native
lineage and an independent exported artifact. The launcher verifies the frozen
candidate inventory, and its installation hook checks the installed inventory;
the install hook requires an empty `/workspace` before model execution; the
separate verifier requires recorded host skill enumeration, one linked
fresh direct child with requested pins and completion, and the exact regular
file. Its verdict does not parse command scripts, prove resource reads or their
order, or prove that the child caused the file. The separate verifier cannot
independently check launcher, installed-package provenance, or the initial
empty workspace. Retained preflight records are cross-checked by the cached
replay when available; older runs without that record report it unavailable. These limits,
role delivery, automatic activation, backend identity and full TEST-S69 remain
unverified or pending; native events are agent-writable.

Attempt to print the frozen, no-auth launch plan with:

```sh
uv run --project evals --locked python evals/planar/adapters/launch.py --case native-lineage-smoke
```

The launcher refuses before printing a plan if the current plugin differs from
the historical frozen inventory. A current launch needs a reviewed freeze and gate.

Reassess a retained execution without a model using the [historical snapshot
procedure](#t-053-preflight-and-historical-replay). This assessment cross-checks retained host
candidate/install records; it is not a new live execution and does not alter
the original verifier result. A fresh live attempt requires a new one-use
review gate bound to the printed case, candidate, configuration, implementation
and image identities.

An independently audited live `native-lineage-smoke` attempt passed the
version 1 recorded-lineage-and-artifact contract with reward 1, one trial and
no retries. The separate verifier ran without networking. The project-local
`reviews/harbor-t044-lineage-live-audit-20261116.md` records the evidence and
independent cleanup checks. This result does not establish full TEST-S69 or
T-044 acceptance. The pass applies to the frozen implementation used in that
attempt; changes to this integration candidate require fresh review and a new
gate before any subsequent live run.

For live execution, prepare the one-use project-local review gate from the
current plan, then run:

```sh
uv run --project evals --locked python evals/planar/adapters/launch.py --case native-lineage-smoke --execute --auth-file <subscription-file> --review-file <review-json>
```

 The launcher checks
the gate, pinned local Docker image and recovery state before accessing the
subscription file; it uses one attempt, an isolated container home and a
network-isolated verifier. It retains execution and cleanup records under the
project reviews directory. Its printed pending diagnostic is not the verifier
verdict; inspect the trial's verifier status and reward. Never put credentials
in this repository.

### T-053 preflight and historical replay

Run the launcher from the repository root. Its secret-free plan checks the repository, package, case, project, jobs and consumed-review destinations (or their existing writable ancestors), and frozen candidate, and reports the checker, runtime, selected configuration source, authentication route, fresh-only lifetime, install/discovery steps, fixed remote home, config, cache and workspace paths, budgets and cleanup owner. Requested versions are distinct from observed local Harbor and Python; installation and verdict are pending at planning time. Execution checks local executables, gate, image and recovery before reading subscription authentication. Launcher termination is not a verifier verdict; consult the last observed event and retained evidence. Missing required evidence is infrastructure error, absent completion is incomplete, and usable false predicates are behavioral failure. Optional unavailable observations remain unavailable.

The historical online execution `planar-t044-03b5a69016f645e8b07d821b8df35049` had reward 1 with candidate `2e229df68784143220d6550bed33719016b1547689ac5a25d5d9f1e4cb1f3368` and implementation `e2513f8c8fbdeb83a25059fe74b891bbf020440a79fbfb90afdf983e53eae26a`; it is not a live result for later code. For offline replay, extract `plugins/planar` from Git commit `afda641` into a separate directory and pass that directory as the second argument to `replay_native_lineage.py`. Replay checks its full inventory against the frozen manifest and recorded host provenance. It reports the current checker and case digests and `new_live_execution: false`; it does not rewrite historical grades. A changed current candidate requires a separately reviewed freeze and gate before launch.

From the repository root, with the historical commit and a retained execution available:

```sh
snapshot_dir=$(mktemp -d)
git archive afda641 plugins/planar | tar -x -C "$snapshot_dir"
python3 evals/planar/adapters/replay_native_lineage.py /path/to/retained-execution "$snapshot_dir/plugins/planar"
```

The retained execution is project-local evidence, not included in a clone.
Remove the temporary snapshot when finished.

The [fresh recovery mapping](cases/fresh-recovery-plan.md) describes the T-045 case; it is not an acceptance result.

The [interrupted-review oracle](cases/fresh-interrupted-review/README.md)
provides a bounded fresh-session recovery case. Its synthetic and no-auth
controls are not a live pass; execution requires independent review and a new
matching one-use gate. The case README states its evidence and limitations.

### Clean revision inputs

`clean-revision` requires paired frozen package/case snapshots and a
1,050-second whole-job deadline. Each parent is capped at 400 seconds and each
separate verifier at 60 seconds. The four expected children are graded after
execution; they are not an enforced runtime child cap. A passing first verifier
is required before injection and the second parent. Independent semantic audit
of checks, findings, repair and fresh review is required in addition to reward.

The Make plan/run wrappers pass the case selection, authentication and review
paths; they do not supply specialized snapshot/deadline arguments. To inspect
this case without authentication or model work, use reviewed snapshots with the
direct launcher:

```sh
PLANAR_EVALS_OUTPUT_ROOT="$PWD/evals/results" \
  uv run --project evals --locked python evals/planar/adapters/launch.py \
  --case clean-revision --trial-deadline 1050 \
  --snapshot-candidate /path/to/frozen/plugins/planar \
  --snapshot-case /path/to/frozen/clean-revision
```

This command is only a plan. Live execution additionally needs its exact
reviewed execution ID, absolute expiry, existing one-use gate and explicit
subscription authentication path. Use the independently reviewed launch packet;
do not generate approval or refreeze inputs during a run. Keep original online
rewards and every versioned retained assessment separately. The
[case README](cases/clean-revision/README.md) describes capture and verifier
requirements.

### Experimental deterministic controls

`experiments/cli.py` provides a model-free version-1 plan, run/continue and
report prototype. Each execution is a fresh Harbor job using the real `nop`
agent and a test-only fake environment; verification is disabled, so every
execution is incomplete rather than a plugin pass. A native START hook records
actual Harbor job and trial IDs before environment setup. Raw jobs are retained.
The absolute job deadline includes time paused between continuations. Run the
controls with `evals/.venv/bin/python -m unittest discover -s
evals/planar/experiments -v`. CLI help is available with
`evals/.venv/bin/python evals/planar/experiments/cli.py --help`.

The experimental Codex path requires a separate matching one-use review gate
for each execution. `codex-plan` prints the review inputs and `codex-run`
launches only one selected execution. `codex-batch` accepts a JSON mapping of
each unreserved logical trial to a distinct existing matching review gate,
checks the whole mapping, and stops after an account-limit result. Incomplete
executions require an explicit `codex-run --retry-of` selection and a fresh
gate; retries retain their predecessor. It uses frozen snapshots, the existing
guarded launcher and cleanup checks, and classifies native typed limit errors.
An account stop persists across invocations until `resolve-account-stop` records
an explicit reason; unresolved interrupted executions also block dispatch.
Serial admission and stops are shared by experiments using the same configured
evaluation output root. Separately configured output roots do not share this
protection; use one root for the same account. Standalone T-044 diagnostic
launches do not participate in this experiment admission lock and must not run
concurrently against that account.
Independent review of the exact integration, inputs and gate is required for
each live execution; a prior baseline does not authorize another run.
After verified native identity and cleanup, a behavioral outcome requires a
valid separate-verifier binary reward, its retained reward file and matching
structured `status.json`. Missing, conflicting or infrastructure evidence stays
incomplete; typed native errors take precedence. This narrow grading does not
establish full test acceptance. Live crash reconciliation, verifier-versioned
regrade and cross-vendor acceptance remain pending. No-model controls are not
plugin behavior evidence.

### Retained A to fresh B

This separate case resumes the durable boundary from one explicitly bound real
worker A in a fresh target B. Source A's original online reward 0 is preserved;
its corrected unchanged-byte assessment is separate. The target receives only
allowlisted durable project/work files, with Git rebuilt from the pinned base.
Source conversation, native logs and cache remain outside the target and are
handed to the separate verifier only after the target stops.

The source packet is a closed, private input validated by the case policy; it
is not included in a repository-only clone. The first slice requires the exact
source candidate inventory `02a5932125696d1c1cbe586f010d59c33b74a1b8b2c765c6c9b911adb6d317d3`.
A newer plugin checkout cannot substitute for it. See the case
[contract](cases/retained-a-fresh-b/CONTRACT.md) for source/target distinctions,
allowed artifacts, evidence and limits.

Use the direct launcher; the Make case wrapper does not supply these mandatory
inputs. With all placeholder paths and identities replaced, secret-free
planning is:

```sh
PLANAR_EVALS_OUTPUT_ROOT=/path/to/reviews \
uv run --project evals --locked python evals/planar/adapters/launch.py \
  --case retained-a-fresh-b \
  --snapshot-candidate /path/to/reviews/source-candidate \
  --snapshot-case /path/to/reviews/retained-case \
  --source-packet /path/to/reviews/source-packet \
  --execution-id 'planar-t044-<32 lowercase hex>' \
  --absolute-deadline '<reviewed UTC expiry>' \
  --trial-deadline 1050 --effort medium
```

Keep the same output root for planning, execution and resolution. Source,
gate and independent release inputs must satisfy the validated root rules.
Execution adds `--execute`, `--auth-file` and `--review-file` to the same frozen
invocation after independent exact gate review. The gate binds source packet,
original outcome/assessment, candidate/case/implementation, effective native
configuration, images, account/output roots, reserved ID and finite expiry.
One exact validated gate is consumed before auth under the shared account
lock. The target retains the reviewed 1050-second envelope, B400 seconds,
verifier60 and cleanup15; preparation and pauses consume the fixed deadline.

Native no-auth controls prove restoration, the fixed STATE-only injection,
separate pinned verifier with network `none`, no build/pull fallback and
cleanup. Synthetic target logs do not prove genuine review/repair behavior.
A real B has not yet run. Automatic structural-positive is deliberately
`incomplete` with reward 0 and `semantic_audit: required`. Supported host
captures are restored-pre, post-injection and final. Intermediate record
writes are cooperative evidence; complete actual native calls and outputs
need independent raw audit for timely starts, full packets before dispatch,
genuine finding, repair/check order and fresh final review. A final file or
child read after dispatch does not prove a pre-dispatch write.

Admission stays pending until explicit reviewed release. The case-specific
operator tool supports the structurally positive, audit-pending incomplete
outcome with confirmed terminal cleanup and no exact-label resources:

```sh
PLANAR_EVALS_OUTPUT_ROOT=/path/to/reviews \
uv run --project evals --locked python evals/planar/adapters/retained_b_resolve.py \
  /path/to/reviews/jobs/planar-t044-EXECUTION_ID \
  /path/to/reviews/retained-case /path/to/reviews/source-candidate \
  /path/to/reviews/independent-release.json
```

The independently prepared release JSON binds execution, consumed gate,
source, verifier status and reward hashes plus an explicit reason. It is an
operator assertion; the tool cannot authenticate its author or establish
semantic truth. It rechecks source/config/gate authority and cleanup, publishes
a complete release atomically, and preserves behavior as incomplete. It does
not rewrite rewards, approve workflow semantics or retry a model. Other
failure shapes or ambiguous cleanup remain pending for independent disposition;
never fabricate a resolution marker. Keep source/code/gate inputs frozen
through audit and account resolution. New attempts require fresh review and a
new one-use gate; none of these controls establishes full S62/S63/S70 or
cross-vendor acceptance.

### Experimental schema-2 execution

`plan-v2`, `preflight-v2`, `reserve-v2`, `check-gate-v2` and `report-v2`
retain the offline planning/comparison routes. Planning freezes explicit
candidate and case-instance inputs; a reservation alone does not establish
that a native job started. Old opaque schema-2 records remain offline-only
and cannot be upgraded in place or made runnable by adding a hash.

New plans explicitly using `runnable_contract: planar-schema2-runnable-v1`
can use the experimental runnable routes below. Inspect their exact arguments
with the locked environment:

```sh
uv run --project evals --locked python evals/planar/experiments/cli.py --help
```

| Command | Boundary |
| --- | --- |
| `prepare-runnable-v2` | Build and inspect the separate verifier image without authentication or a model; retain fresh preparation evidence. |
| `reserve-runnable-v2` | Bind one future execution to that preparation, frozen inputs, concrete JobConfig, images, account scope and deadline. |
| `check-runnable-v2` | Compare an independently supplied exact gate without consuming it or authorizing a dispatch. |
| `execute-runnable-v2` | Recheck under existing account/controller locks, consume one exact reviewed gate before authentication and run the guarded launcher once. Requires auth file, gate and preparation evidence. |
| `reconcile-runnable-v2` | Assess retained native/cleanup evidence for an existing attempt without replaying authentication or starting another job. Missing or contradictory evidence stays blocked. |
| `resolve-account-stop-v2` | Record an explicit reasoned resolution after matching the retained gate, account stop and confirmed cleanup. It preserves original stop/results and does not itself retry. |

The source and model-free controls are independently reviewed. Actual no-auth
Harbor/Docker evidence proves separate pinned verifier selection, network
`none`, Compose no-build/no-pull and cleanup; its Nop result is an
infrastructure nonpass, not a behavior pass. A gated schema-2 native model run
is still unproven. Source, candidate, configuration or documentation changes
require a fresh freeze, preparation, reservation and independently reviewed
gate before such a run; historical reservations are not reusable approval.

Planning bounds the immutable absolute deadline by `caps.job_seconds`;
preparation and pauses consume that budget. Runtime activation requires enough
remaining time for the reviewed trial. Quota/auth failures retain account
stops; a confirmed clean native timeout is incomplete without an account stop.
Cleanup ambiguity cannot be resolved automatically. Reports distinguish
planned, started, reconciled and unknown outcomes, and keep retries separate
from primary attempts. Schema-1 and historical online results remain separate;
these controls do not establish full experiment or cross-vendor acceptance.
