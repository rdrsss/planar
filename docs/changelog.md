# Changelog

A changelog of shipped milestones and plans, newest first.

## 2026-10-11

### Plan 1142, milestone 2: verbs that are not mechanism left master

Verbs that prescribed a method, or that nothing used, are gone, with the engines and role agents behind them.

- **Moved to a branch.** `planar closure`, `planar groups`, `planar bench` and `planar run`, the closure, grouping and runs engines, the vendored tree-sitter, and the three research documents live on the branch `dev/grouping-solvers`.
- **Archived at tags.** `planar import`, `planar synthesize` and the importer and synthesizer agents: tag `archive/import-synthesize`. `planar report`, the introspection engines, the `introspection.transcripts.*` config keys and the introspector agent: tag `archive/introspection`. `planar feedback triage` and the feedback-triager agent: tag `archive/feedback-triage`.
- **Removed with no replacement.** `planar explore`, `planar skills`, `planar scope use`, `pop` and `clear`; `plan edit`, `plan diff`, `plan review`, `question diff`, `question review`, `scenario diff` and `decision diff`; `document validate-range`, `audit publish-decision`, `assoc detect` and `config init`.
- **Changed.** `plan recommend-strategy` reads declared touches only, so `--closure-source` is gone and the JSON key `closure_source` is always `"declared"`. The cross-scope guard now covers seven verbs: `spec ingest --apply`, `task update`, `decision accept`, `decision withdraw`, and `planar-ext sync push`, `sync pull` and `sync resolve`.
- **Kept.** The CLI usage log (`cli_invocations`, `introspection.cli_log`, `introspection.retention_days`) and the `feedback_triage` table stay. Eleven role agents remain under `agents/`.

## 2026-10-10

### Plan 1142, milestone 1: Planar ships mechanism only

Planar's code and release bundle now hold mechanism only. The workflow runner and the model-routing surface left master.

- **Moved to a branch.** `planar-execute`, the Lua workflows under `workflows/` and the Centurion client verbs live on the branch `dev/planar-execute`.
- **Removed with no replacement on master.** `planar workflow`; `planar-agent dispatch`, `planar models`, the routing engine and `agents/models.md`; `planar-agent run`, `planar-agent context` and `planar-watch run`; `planar-agent claim-associate` and the engine-supervision flags on `planar-agent`; and the evals candidate-spawn and rollout harnesses. The routing tables stay in the schema until a later milestone.
- **Config.** The `[execute]` table is gone, and `planar config validate` now rejects it.
- **Release bundle.** The bundle ships four binaries: `planar`, `planar-agent`, `planar-watch` and `planar-ext`. The installer removes `bin/planar-execute`, `workflows/` and `agents/models.md` from an older install through `install-cleanup.txt`.

## 2026-10-04

### Plan 1104: one `planar` skill replaces the forty `pl-*` skills and the renderer

Planar now ships one skill and fifteen role agents, installed directly from the authored tree. The CLI carries the detail the skills used to repeat.

- **Skills.** The forty `pl-*` skills under `skills/src/` are replaced by one `skills/planar/` skill in the Agent Skills layout, with nine references. Agents are renamed `planar-<role>`.
- **Renderer.** The `scriptorium` renderer, `scriptorium.yaml` and the 232 rendered projections are gone. The installer places the authored tree for six vendors (Claude Code, Codex, Copilot, Gemini CLI, Antigravity, OpenCode) from a nine-target table. `planar health` reports each recorded target as `fresh`, `stale` or `missing`.
- **Removed install paths.** `~/.claude/commands/pl-*`, `~/.codex/skills/pl-*`, `~/.copilot/skills/pl-*`, `~/.gemini/antigravity-cli/skills/pl-*`, and under `$PLANAR_HOME`: `commands`, `skills/codex`, `skills/copilot`, `skills/gemini`, `codex-skills`, `copilot-skills`, `gemini-skills`, `opencode-skills` and `agents/<vendor>`. The installer removes them on upgrade when it can prove Planar made them, and reports what it leaves.
- **Local skills and agents.** `planar local link` now projects by copy, as `planar-local-<name>`, under a name rule the verb enforces. The `shadow` key is retired.
- **`planar skills`.** Kept for one release as a pointer to the new layout.
- **CLI help.** Every flag carries help and every leaf verb ends with `Examples:` and `Exit codes:` sections. `schema --command` and `schema --compact` narrow the catalog, and `planar help` prints the root page.

The first step with or without the skill is `planar --help`. See [skill-reference.md](skill-reference.md) and [getting-started.md](getting-started.md).

## 2026-10-01

### Plan 1089: the host queue moves into planar.db; agent.db removed

Planar uses a single SQLite database again. The host queue's tables (`queue_entries`, `queue_history`, and a new `queue_schema` marker) now live in `planar.db`, created by migration 00040, and every `planar-agent queue` verb and every `planar-watch queue` view reads them there. Queue entry numbers start above 1,000,000.

The `planar-agent queue` verbs refuse a `planar.db` that is behind the binary, and still run against one that is ahead when the queue's own compatibility check passes. Each refusal exits 125 with one tag: `schema_version_behind`, `queue_schema_incompatible` or `queue_schema_foreign`. Claims keep the exact-version rule. The `planar-watch` queue views follow that binary's usual rule and exit 7 on a version mismatch in either direction. Detached logs go to `queue-logs/` next to `planar.db`, or `<stem>.queue-logs/` for any other database name.

On upgrade, `install.sh` asks the new `planar-agent` whether `planar.db` is usable, migrates it only when it is behind, and retires the old `~/.planar/agent.db` and its logs. A fail-closed live-queue guard runs first; `--ignore-live-queue` is the only override. Uninstall now removes `agent.db`, behind the same guard. The separate agent migration stream (`migrations-agent/`), the agent-database module, and the `PLANAR_AGENT_DB` override are gone. A set `PLANAR_AGENT_DB` is ignored, not refused. `surface_lint` now rejects new references to the retired names outside this changelog and INSTALL.md's marked upgrade note.

## 2026-05-18

### Plan 96 M4–M6: promote, regenerate, and query verbs

The synthesis pipeline landed across three milestones. M4 added
`planar doc promote` (synthesise a new doc from named source
entities; the LLM call lives in the `pl-doc-promote` vendor skill).
M5 added `planar doc regenerate` (re-synthesise an existing doc
when sources have drifted; hand-edit policy with `--force` /
`--merge`). M6 added the coverage queries: `planar doc backlinks`
(every doc citing an entity), `planar doc orphans` (artifacts of a
kind with zero backlinks), and `planar doc coverage` (done plans
with no published doc).

> **Port status.** These `planar doc promote/regenerate/backlinks/orphans/coverage`
> CLI verbs are from the Go implementation and were **never ported**: the
> current C++ `planar` binary has no `doc` domain at all (this includes the
> `planar doc lint` verb named in the M1–M2 entry below). Published-documentation
> drift and coverage are delegated to a separate documentation tool.

### Plan 135: workspace AGENTS.md generation

Plan 135 shipped the workspace AGENTS.md generation pipeline. A
workspace is an `associations` row of `kind=org`. Each workspace
owns a state directory at `~/.planar/workspaces/<id>/` holding the
canonical `AGENTS.md`, the routing-table JSON, and per-workspace
config. The repo workspace root holds symlinks back to those
canonical files (with a degraded copy-mode fallback).
`planar workspace init` scaffolds the org association and writes
the first routing table; `planar workspace routing build` refreshes
static signals; `planar workspace regenerate` re-renders AGENTS.md;
`planar workspace doctor` inspects symlink lifecycle health.

### Plan 88: strict scope resolution shipped

The strict scope resolver landed on 2026-05-18 (plan 88, M1–M7).
Every write verb now resolves to exactly one scope via a four-step
algorithm: `--scope` explicit override → cwd derivation →
stack-top fallback → strict tiebreak (refuse when cwd and stack
disagree). The `--no-scope-check` escape hatch is available for
legacy scripts but is excluded from routine workflow examples.
New: `planar health` surfaces cwd-vs-stack disagreement;
cwd-not-on-top mismatches print a non-blocking warning
(suppressible with `--quiet`).

> **Status update (2026-09-09, task 6140).** The active scope stack this
> entry describes was itself dropped in plan 153 M5 (see
> [`concepts.md#removed-the-active-scope-stack`](concepts.md#removed-the-active-scope-stack)),
> and `--no-scope-check` is not implemented by the current binary at
> all — `planar schema` declares no such flag on any command, and
> passing it fails at parse time with exit 2 (`error: <cmd>: The
> following argument was not expected: --no-scope-check`). This paragraph is preserved as
> the historical record of plan 88's ship date; it is not current
> guidance. The only present-day remedy for a cross-scope-guard
> refusal is `--scope <slug>` or `cd` into the entity's owning repo —
> see [`concepts.md#cross-scope-guard`](concepts.md#cross-scope-guard).

### Plan 96 M1–M2: doc kinds and manifest layer landed

The outward-facing docs system's foundation shipped in two
milestones. M1 (migration 0008) extended `artifacts.kind` with
`research`, `getting_started`, `changelog_entry`, and
`glossary_term`. M2 added a Merkle drift index
(xxh3-keyed, two-level; since removed from Planar), the citation linter (`planar doc lint`)
covering GFM footnote citations plus a structured `references:`
front-matter block, the URL HEAD validator with on-disk cache and
30-day re-check policy, and the per-entry planar-entity resolver.
A pre-commit hook for the docs checks shipped alongside it.
