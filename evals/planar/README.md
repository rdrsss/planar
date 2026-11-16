# Planar smoke — bounded diagnostic

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
Claude, recovery, cross-vendor, quota and experiment capabilities also remain
pending.

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

An [offline interrupted-review oracle draft](cases/fresh-interrupted-review/README.md)
provides a bounded fresh-session recovery case. Its synthetic and no-auth
controls are not a live pass; execution requires independent review and a new
matching one-use gate. The case README states its evidence and limitations.
