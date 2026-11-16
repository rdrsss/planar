# Evaluation workspace (T-044, diagnostic in review)

This workspace contains one bounded Harbor Codex diagnostic, not a passing
regression suite. The user authorized one diagnostic attempt; the current
implementation is awaiting independent review after the archive-stream fix.
The gate binds that review to the exact source, candidate, configuration and
image and prevents accidental reuse. It is an integrity check, not a request
for new human authorization or proof of reviewer identity.

Harbor 0.26.0 is locked and was installed locally with Python 3.15.2 and uv
0.13.1. Docker Engine 29.9.2 ran Linux arm64. The task pins the locally built
agent image `sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7`,
with Codex CLI 0.169.0; its Python 3.12.12 and Node 22.22.1 base images are
pinned by digest in the Dockerfile. The separate verifier image was rebuilt
without build networking. A real no-auth Harbor install-only preflight passed
plugin install, final config and complete cache inventory in the same container
before auth upload. See `reviews/harbor-t044-remediation-worker-20261116.md`
in the project directory for evidence and limitations.

From the repository root, these commands do not use a model or authentication:

```sh
uv sync --project evals --locked
uv run --project evals --locked harbor --version
uv run --project evals --locked python -m unittest discover -s evals/planar/adapters -p 'test_*.py' -v
python3 evals/planar/adapters/candidate.py plugins/planar --compare evals/planar/cases/plugin-smoke/candidate.json
uv run --project evals --locked python evals/planar/adapters/launch.py
uv run --project evals --locked python evals/planar/adapters/export_preflight.py
```

The plan prints the current digests without starting a subprocess. The export
preflight creates and removes a disposable no-secret container. No credentials
belong in this directory. A diagnostic, once the technical review clears,
produces evidence for investigation only: the verifier still reports an
infrastructure error for correct output and the host checker remains
inconclusive. Agent-writable native logs are non-adversarial evidence; an agent
able to read auth could intentionally write it into an allowed log. A durable
recovery marker blocks another attempt if watchdog cleanup is unconfirmed.

The first subscribed diagnostic recorded a real Codex exec and installed-file
reads but no child dispatch; the driver stopped because it could not determine
its configured model and effort. Its verifier did not start: this host's
Harbor Docker egress-control capability rejected `no-network`. Cleanup was
confirmed. See `reviews/harbor-t044-live-diagnostic-20261116.md` in the project.

The next candidate states the transport's configured driver model and effort
in its ordinary prompt, while keeping backend observations separate. The
separate verifier retains Harbor's `no-network` policy and uses a verifier-only
Docker Compose `network_mode: none`; the adapter checks the actual container
network mode before verification. A no-auth Harbor `nop` preflight started the
separate verifier, confirmed mode `none`, and retained its expected missing
fixture failure. Native dispatch and independent acceptance remain pending.
