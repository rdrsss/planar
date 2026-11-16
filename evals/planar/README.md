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
