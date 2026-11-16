# Fresh interrupted-review recovery — synthetic clean packet v2

This T-045 Codex case maps [the case plan](../fresh-recovery-plan.md) to
TS-25.3, TS-25.6 and the applicable subset of TEST-S70. It is **pending
independent review and a matching one-use gate**; neither synthetic controls nor retrospective replay are a new live result. It does not establish full
S63, S69, S70, T-044/T-045 or Claude coverage.

The credential-free template derives from run7's audited `before` boundary:
32 source files with the completed worker, RESULT-only check, reviewer packet,
interrupted reviewer and no verdict. The source manifest SHA-256 is
`12c364edafcafbad9481ebe76de018b9af105b15c03e7b803b583ee1c5e80d1f`;
the independent audit is
`93b75382bdb59096aa5cbedd37f18911fcd8430aecd65dba9f85c7eb701ad35c`.
The base is `08bacc341a31f32c14a1695b5b2de5c44b3a3e56`. Original evidence
remains in project storage. The inventory records historical and portable
hashes and the exact known path replacements in project configuration and the
historical dispatch prompt. The installer checks the 32 template files,
reconstructs intent-to-add, checks HEAD and compares the complete diff to the
retained packet; it does not claim historical index-byte identity.

The harness changed STATE after the worker from `correct\n` (SHA-256
`844cd17a64dffb0b93be849979ecbc677a8e98a1b595a402ec08ce1b4f683166`)
to `fault-injected\n` (SHA-256
`c9fda0b30e09c73dd79b29bc6212c386bd50a7a5182498ad0bc2aedb0feb7957`).
The check covers RESULT only. The source's later two-parent-turn continuation
and packet-start failure are not a clean strict-case pass. An interrupted source
process is not itself resumed: the case starts one fresh bounded session and
stops after review recording. Source process-stop confirmation is factual
input, not proof of an exhaustive historical descendant census.

The historical v1 packet omitted the current required checks-definition section.
V2 preserves v1 separately and synthetically derives the starting packet by
inserting the full retained TEST-C1 definition. The v2 manifest records both
hashes and the transformation; no claim is made that v1 was compliant. The
installer verifies the derivation and that every other retained byte is unchanged.
The active v2 case starts from this canonical packet and expects a linked preflight
and direct fresh review; malformed-packet replacement is outside this case. The separate no-network verifier compares
retained bytes and ordered six-field log, checks the attempt-1/rung-1 terminal
review, selected packet and input hashes, review artifact and record links,
and derives one direct fresh reviewer from structured parent/child rollout
records. It checks a structured request-changes finding citing PRD-R2 or TS-1;
it does not assess the finding's semantic correctness or prescribe a verdict. Agent-writable native records are
bounded evidence, not hostile attestation: exact role delivery, read order,
causal writes and backend identity are unverified. Preflight validation binds retained hashes and identities; it does not establish
semantic TS-8.13 preflight or source-reading compliance.
Missing evidence remains incomplete; contradictory evidence fails.

The derived agent image is pinned by task identity to
`sha256:06e95472c9fb2a7ab49c50b0f2c87f5f1006aa33397ef17715a75164458b7ed2`
(linux/arm64), based on local image
`sha256:80c72c37a539e081165e5cebf0f4a6e33852e34df7cee7afaefc3bc0ab0da5a7`
with Debian Git `1:2.39.5-0+deb12u3`. `environment/installed-packages.txt`
records all installed package versions; `environment/verify-image.sh` checks
base/image identities and the package inventory without network access. The
Dockerfile uses a local base tag and package repositories, so a rebuild is not
claimed reproducible without verifying the resulting identity and package set.
Package acquisition used network; the verifier and deterministic controls use
no network.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s evals/planar/cases/fresh-interrupted-review -p 'test_*.py'
uv run --project evals --locked python evals/planar/adapters/launch.py --case fresh-interrupted-review
```

The launcher checks the exact current candidate, case, adapter, config and image
identities against a matching independent one-use review gate before reading
authentication. Its case-specific candidate inventory is a draft until that
review. The old lineage candidate and gate cannot authorize this case. A
no-auth install-only trial and synthetic controls are not a live host pass.

The original online recovery ended in `behavioral_failure` and reward 0. A
separate full Harbor no-model control replayed the recorded live workspace and
native evidence and reported reward 1; it preceded the final foreign-handle
hardening. The final packaged assessor passed an offline replay of the retained
actual export, and focused tests cover the final bytes. Neither result is a
fresh online reward 1 or full S63, S70 or Claude coverage. The independent
project-local final owner audit records the actual finding's semantic support;
the generic oracle does not establish finding correctness.
