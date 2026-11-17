# Evaluation workspace

This workspace contains model-free controls and review-gated Harbor diagnostics
for the subscription-backed Codex CLI. It is outside the installed plugin.
Each result applies to its frozen case, candidate and verifier; a cached
assessment does not replace an original online result. Claude and cross-vendor
runtime acceptance remain deferred.

The [Planar evaluation notes](planar/README.md#case-and-experiment-status)
distinguish historical smoke cases, the current-lineage snapshot, revision
controls and experimental schema-2 execution. A case name containing `current`
still refers to a frozen inventory, not the latest working tree.

## Historical first diagnostic

This workspace contains a bounded Harbor Codex diagnostic, not a passing
regression suite. A real Harbor execution observed installed skill enumeration,
a fresh native child dispatch and the expected result. An independent audit
and a separately reviewed cached narrow regrade support those bounded findings;
full TEST-S69 and T-044 acceptance remain pending. The original online
separate verifier reported `infrastructure_error` and reward 0 because native
dispatch evidence verification was not implemented. The cached regrade does
not change that outcome.

Harbor 0.26.0 is locked. The recorded execution used Docker on Linux arm64,
Codex CLI 0.169.0 and the pinned agent image
`sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7`.
The separate verifier ran without network access. Agent-writable native logs
are bounded diagnostic evidence, not adversarial attestation. No credentials
belong in this directory.

From the repository root, `make evals-setup` installs the locked dependencies,
`make evals-test` (or `make evals`) runs the adapter, recovery-case,
clean-revision, clean-work-review, retained-A/fresh-B and model-free experiment-control tests,
and `make evals-plan EVALS_PROJECT_DIR="/path/to/selected/project"` preflights
`CASE=fresh-interrupted-review` by default.
`make evals-test` uses a disposable Planar project and runs the portable
model-free suite. It reports how many retained-evidence tests are selected
separately; it fails on unexpected skips. Synthetic checks cover general
provenance types, path safety, publication faults and locking, but do not prove
that original historical evidence is intact.

For changes to historical consumers or account-release logic, also run:

```sh
make evals-evidence-test EVALS_PROJECT_DIR="/path/to/selected/project"
```

This read-only, no-model suite checks the explicitly selected project's 24
retained-evidence tests (21 adapter, one case and two experiment tests). It
fails when selection, retained inputs or any required check is unavailable;
it does not treat a skip as a pass. These tests include the original reviewed
trace, gate and marker checks. A passing portable suite alone does not establish
those historical results. Neither suite authorizes a live diagnostic.

Set `CASE` to select another supported case. A preflight can refuse a stale
frozen candidate or missing case-specific inputs; it never refreezes or approves
them. Specialized cases requiring paired snapshots and a fixed deadline use
the [direct launcher procedure](planar/README.md#clean-revision-inputs).
The registered [clean-work-review case](planar/README.md#clean-work-review)
also needs direct launcher inputs; its synthetic no-auth control is not a live pass.
The [retained-A/fresh-B procedure](planar/README.md#retained-a-to-fresh-b) also
requires its exact source packet, candidate, reserved ID and expiry; the generic
Make case wrapper does not supply those arguments.

A live diagnostic requires a matching independent gate and explicit paths:

```sh
make evals-run EVALS_PROJECT_DIR='/path/to/selected/project' AUTH_FILE='/path/to/auth.json' REVIEW_FILE='/path/to/review.json'
```

The launcher enforces the existing gate and returns its diagnostic exit status.
New project-owned plans and runs require an explicit selected project directory:
set `EVALS_PROJECT_DIR` for Make or `PLANAR_EVALS_PROJECT_DIR` for direct
launcher use. The directory must match Planar settings and the repository's
Git identity. Without an output override the destination is the selected
project's `scratch/scratch-evals`; `EVALS_OUTPUT_ROOT` (Make) or
`PLANAR_EVALS_OUTPUT_ROOT` (direct) explicitly selects a different owned root.
The plan records the selected context for a fresh matching review gate.
Historical recovery requires the exact previously recorded output root and
`PLANAR_EVALS_LAYOUT_KIND=legacy-reviews` or `override-jobs`; provide the
exact original gate with `PLANAR_EVALS_REVIEW_PATH`. The launcher record must
bind the project and jobs roots and the gate must match its hash and consumed
marker. Older records missing those bindings remain pending for review. Never
point recovery at a new scratch root. Confirm unresolved recovery markers at all previously
used roots before another run.
Paths and credentials should be kept private; no credentials belong in this tree.

These commands do not use a model or authentication:

```sh
uv sync --project evals --locked
uv run --project evals --locked harbor --version
make evals-test
python3 evals/planar/adapters/candidate.py plugins/planar --compare evals/planar/cases/plugin-smoke/candidate.json
PLANAR_EVALS_PROJECT_DIR="/path/to/selected/project" uv run --project evals --locked python evals/planar/adapters/launch.py
```

The comparison and launch-plan commands refuse when the current plugin differs
from the historical frozen candidate; they start no model or diagnostic job. A current launch
needs a separately reviewed freeze and gate. For historical snapshot replay, see
[Planar evaluations](planar/README.md#t-053-preflight-and-historical-replay). Project-local no-auth Docker integration controls and their
prerequisites are documented in `reviews/tools/harbor-t044/README.md` relative
to the project directory; they do not travel in a repository-only clone. A
durable recovery marker blocks another attempt if watchdog cleanup is
unconfirmed. Future live work requires a fresh gate for the current
implementation.

The project's `reviews/harbor-t044-native-audit-20261116.md`,
`reviews/harbor-t044-regrade-final-review-20261116.md` and
`reviews/harbor-t044-consolidation-review-20261116.md` record the evidence,
review scope and limitations. Complete plaintext role delivery, independent
backend model and effort identity, and automatic skill activation remain
unproved.

The separately versioned `native-lineage-smoke` case passed a live online
verification with reward 1; an independent audit corroborated that bounded
result. It checks recorded native lineage, host skill inventory and an
independently exported exact artifact. It does not prove reads, causal writes
or full TEST-S69 or T-044 acceptance. See [the Planar evaluation notes](planar/README.md)
for reproduction, live mechanics and limits.

The portable next-case mapping and offline historical snapshot replay procedure are in [Planar evaluations](planar/README.md). Local scratch evidence is provenance for the proposed recovery case, not a required clone dependency or a substitute for a reviewed fixture.
