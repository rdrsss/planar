# Evaluation workspace (T-044)

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
`make evals-test` (or `make evals`) runs the adapter and recovery-case tests,
and `make evals-plan` preflights `CASE=fresh-interrupted-review` by default.
Set `CASE` to select another supported case. A preflight can refuse a stale
frozen candidate; it never refreezes or approves it.

A live diagnostic requires a matching independent gate and explicit paths:

```sh
make evals-run AUTH_FILE='/path/to/auth.json' REVIEW_FILE='/path/to/review.json'
```

The launcher enforces the existing gate and returns its diagnostic exit status.
Make places jobs under ignored `evals/results`; set `EVALS_OUTPUT_ROOT` to choose
another directory. Direct launcher, smoke hook, and manual recovery use
`PLANAR_EVALS_OUTPUT_ROOT`; when unset they retain the previous project reviews
jobs directory. Supply the same root to manual recovery as the launch, and check
all previously used roots for unresolved recovery markers before another run.
With an override, review files and one-use gate reservations reside under that
output root; without it they remain in the project reviews directory.
Paths and credentials should be kept private; no credentials belong in this tree.

These commands do not use a model or authentication:

```sh
uv sync --project evals --locked
uv run --project evals --locked harbor --version
uv run --project evals --locked python -m unittest discover -s evals/planar/adapters -p 'test_*.py' -v
python3 evals/planar/adapters/candidate.py plugins/planar --compare evals/planar/cases/plugin-smoke/candidate.json
uv run --project evals --locked python evals/planar/adapters/launch.py
```

The comparison and launch-plan commands refuse when the current plugin differs
from the historical frozen candidate; they start no subprocess. A current launch
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
