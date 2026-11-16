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

Print the frozen, no-auth launch plan with:

```sh
uv run --project evals --locked python evals/planar/adapters/launch.py --case native-lineage-smoke
```

Reassess a retained execution without a model using:

```sh
python3 evals/planar/adapters/replay_native_lineage.py /path/to/retained-execution
```
 This new assessment cross-checks retained host
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
