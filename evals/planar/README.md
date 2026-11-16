# Planar smoke — diagnostic in review

The bounded diagnostic is one Harbor 0.26.0 Docker job using Codex CLI
0.169.0, one attempt and concurrency one. It installs the frozen package in the
final isolated Codex home. The requested worker output is `RESULT.txt` with
exact bytes `32 0a`. The installed role instructions must be delivered to a
fresh child with the minimal profile's explicit model and effort. Requested
and observed pins are separate evidence; skill injection or app-server does not
prove native exec discovery and dispatch.

`cases/plugin-smoke/candidate.json` inventories 37 files from candidate commit
`1ef833747ae46fc58b8ecfd0ec46231b301b8185`. Its canonical inventory SHA-256 is
`2e229df68784143220d6550bed33719016b1547689ac5a25d5d9f1e4cb1f3368`.
The inventory hashes each file, then hashes the compact sorted-key JSON array
of path/hash objects. It rejects symlinks and empty trees. A no-auth Harbor
install-only run checked the installed cache against it, but did not establish
runtime skill discovery or dispatch.

The agent image is pinned to
`sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7`
(Linux arm64). The separate verifier is network-disabled. Guarded exports admit
only the result and bounded native logs, but those logs remain agent-writable.
The verifier deliberately cannot return a pass for the dispatch assertion;
synthetic controls establish checker behavior, not live capability.

Subscription authentication, full package discovery, fresh dispatch and
end-to-end isolated verification remain **pending**. Claude, recovery,
cross-vendor, quota and experiment capabilities also remain pending. This case
does not establish TEST-S69 acceptance. The project's
`reviews/harbor-t044-remediation-review-20261116.md` identifies the archive
stream correction awaiting fresh independent review; the related worker report
records the no-auth preflights and their limits.

The first live diagnostic completed a Codex model turn and read the installed
skill, profile, host reference and worker role, but stopped without spawning a
child because the driver could not establish its configured model and effort.
Those explicit reads demonstrate resource access, not automatic skill
discovery. The initial prompt now states the actual configured transport
request; it does not assert observed backend identity or name an installed
skill path. The verifier remains fail-closed for missing role-delivery proof.
The separate verifier's static Docker `network_mode: none` is checked on the
actual verifier container; its no-auth Harbor preflight passed with the expected
missing-fixture result. A fresh independent review precedes the next authorized
diagnostic.

Repository unit controls are run with `uv run --project evals --locked python
-m unittest discover -s evals/planar/adapters -p 'test_*.py'`. Project-local
no-auth Docker integration controls and their prerequisites are documented in
`reviews/tools/harbor-t044/README.md` relative to the project directory.
