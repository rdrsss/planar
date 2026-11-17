# Clean revision case — offline review candidate

This case uses two fresh native Harbor steps in one isolated workspace. It
installs a clean projection of the approved revision-fault inputs, stops after
the first worker, takes host-owned snapshots, changes only STATE from
`correct\n` to `fault-injected\n`, and continues through a real review and
revision. The first separate verifier gates the second step. The final verifier
checks bounded structure and native lineage; a separate semantic audit of the
actual review, repair and approval is mandatory for a success claim. This case
is not a live TEST-S62 result and does not establish full S62, S63 or S70.

The projected inputs and base bundle are under `fixture/`. They contain no
historical T-001 reports or packets. `projection.json` records the initial
allowlist and original portable hashes. The installer clones the pinned base
with a clean index and absent output files. Source corpus and past evidence
remain unchanged.

The task selects a single prebuilt verifier image for both steps. The offline
build uses the local tag `planar-t044-agent-preflight:latest`, whose required
image identity is `sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7`;
the built image identity is pinned in `task.toml`. Verify the base and built
image identities, build with `--network none --pull=false`, and review the
resulting image and source before creating a gate. A matching image is required
at launch and each actual verifier container must match it and use network
mode `none`. A rebuild is not assumed to reproduce the same image identity.

```sh
PYTHONPATH=evals/planar/adapters PYTHONDONTWRITEBYTECODE=1 \
  evals/.venv/bin/python -m unittest discover -s evals/planar/cases/clean-revision -p 'test_*.py'
```

The launcher accepts this case only with paired frozen candidate and case
snapshots, a 1050-second whole-job budget, the configured 400-second step and
60-second verifier caps, and a matching independent one-use gate with finite
expiry and the exact verifier image ID. The gate is consumed before the auth
file is accessed. No live execution is authorized by this README or by the
synthetic controls. Earlier no-model controls used synthetic reports and
rollouts, so their reward does not prove a model performed the revision.

The recorded native lineage, selected model/effort and process census are
bounded evidence, not hostile attestation. Backend identity, exact role
delivery, complete source-reading order and causal authorship are not proven.
The verifier must keep missing evidence incomplete and must not substitute a
structural finding check for the independent semantic audit.
