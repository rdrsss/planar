# Planar

> AI coding agents lose context constantly: at compaction, at session end, at vendor switch, at every interruption. The user pays for that loss in re-explanation, lost decisions, and forgotten constraints. Planar is local infrastructure that keeps the user's accumulated context durable across agents, vendors, and sessions.

Planar is a local-first task tracker — Jira-shaped, but living next to the developer instead of inside the org's stack — for the structured intent and outputs of agent work: **tech specs, plans, tasks, decisions, questions, and test scenarios**, plus the **sessions** and **snapshots** that capture how the work happened. It scopes that state across repositories and organizations via associations, exports reviewable Markdown for team workbenches, and integrates with operational issue trackers (Jira, GitHub Issues) so org-level visibility and audit trails come for free.

It is **vendor-agnostic by design**: Claude, Codex, and Copilot are first-class today, with additional agent runtimes expected. The user's accumulated context — plans, ADRs, sessions, audit trails — outlives any particular agent.

## Status

Planar is a feature-complete, local-first tool built as five binaries (`planar`, `planar-agent`, `planar-watch`, `planar-doc`, and `planar-execute` — the deterministic Lua workflow engine, which holds no DB handle and reaches state only through a constrained CLI host surface). The schema has twenty-seven migrations, through `migrations/00027_external_sync_baseline.up.sql`; the runtime applies them automatically from an embedded `migrations` Zig module produced by build-time codegen. The suite contains 1,700+ unit tests and 570+ integration tests. Vendored SQLite is compiled by `build.zig`; `planar-doc` uses a separate DB-free module graph.

**History.** Repo split — the original Go implementation (M1–M19) is preserved at `github.com/rdrsss/planar-go-archive.git`; the current canonical Zig implementation lives at `github.com/rdrsss/planar.git`.

## Prerequisites

Planar shells out to a small set of external tools. On macOS, install them via Homebrew:

```bash
brew install zig git gh jq ripgrep
```

- `zig` — required to build the binary (see [Install](#install) and [Build from source](#build-from-source)). The minimum supported version is **zig 0.16.0 or later** (declared in `build.zig.zon`). The runtime statically links a vendored SQLite amalgamation compiled by `build.zig`; no system SQLite library dependency.
- `git` — required at runtime. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — required by the bundled agent skills (`pl-spec-draft`, `pl-spec-ingest`) to parse `planar … --json` output in their shell snippets. The Zig binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session below (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.

The full source-checkout installer also uses the base-system utilities declared
in `install.sh`'s `BUILD_DEPS` manifest (`awk`, `basename`, `cat`, `chmod`,
`cp`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mv`, `readlink`,
`rm`, `rmdir`, `sed`, and `tr`). These ship with supported Unix-like systems;
the installer preflights them before making changes.

### Optional / research tools

- `mtkahypar` — optional. The external [Mt-KaHyPar](https://github.com/kahypar/mt-kahypar) hypergraph partitioner backs the optimal arm of `planar groups recommend --solver=mtkahypar`. Planar shells out to it as a subprocess; it is **not vendored or compiled by `build.zig`** (its C++14/CMake/TBB/Boost toolchain is not a `build.zig`-compilable amalgam), so there is **no Homebrew formula** — build it from source per its README. When `mtkahypar` is absent, `groups recommend` degrades gracefully to the greedy arm and reports `optimal_available:false`; the greedy default never needs it.

`sqlx-cli` and `sqlite3` are only needed for ad-hoc developer workflows against a scratch database (see [Build from source](#build-from-source)); the runtime embeds migrations via build-time codegen and uses the vendored SQLite amalgamation, so neither CLI is a runtime dependency. Install the optional `sqlx-cli` for authoring new migration pairs:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
```

## Install

The Zig package root IS the repo root (`build.zig` and `build.zig.zon` sit at the top level). Clone the repo, then build directly into your install prefix:

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
zig build --prefix "$HOME/.planar"
```

This puts a single binary at `~/.planar/bin/planar`. The binary statically links the vendored SQLite amalgamation — no system library dependency.

Add `~/.planar/bin` to your `$PATH`, then verify:

```bash
export PATH="$HOME/.planar/bin:$PATH"
planar health
```

This installs only the binary. The agent specs, slash commands, skills, and migration sources are *embedded* in the binary, so the CLI works in isolation. But none of the vendor surfaces (Claude `/pl-*` slash commands, Codex skills, Copilot skills) are wired up — for those, use the [full install](INSTALL.md#full-install-installsh).

## Build from source

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
zig build                         # default install (zig-out/bin/planar)
zig build test                    # unit tests
zig build test-integration        # integration tests (or: make test-integration)
```

For ad-hoc migration work against a scratch database, install the optional sqlx CLI:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
sqlx migrate add -r <name> --source migrations
sqlite3 /tmp/scratch.db < migrations/00001_foundation.up.sql
```

## Quickstart

Initialize, scope, plan, work, capture, hand off, resume:

```bash
# Initialize the local database (default: ~/.planar/planar.db).
planar init --name "my-project"

# Group the project under an association you can scope work to.
planar assoc create org:my-org --kind org --name "My Org"
planar assoc add org:my-org "$(pwd)"
planar scope use org:my-org

# Plan, task, and capture work.
planar plan create "Migrate to v2" --summary "Cut the v1 endpoints"
planar plan step add 1 "Inventory v1 callers"
planar plan step add 1 "Document v2 contracts"
planar task add "Inventory v1 callers" --plan 1 --next-action "grep the monorepo"
planar task add "Document v2 contracts" --plan 1
planar question add "Do we deprecate before delete, or atomically?"

# Capture the working session. Vendor identity comes from $PLANAR_VENDOR.
export PLANAR_VENDOR=claude-code
planar capture session --task 1
planar capture note "found 7 callers under services/billing"
planar capture command "rg -l 'v1.client'"
planar capture snapshot "halfway through the inventory" --task 1 --next-action "audit services/payments"

# Record a decision tied to the active session.
planar decision add "Deprecate v1 over two releases" --body "@notes/deprecation-policy.md"

# Hand off — captures a snapshot, opens a handoff record, validates it,
# all atomically.
planar handoff 1 --vendor codex --note "halfway through; payments next"

# Resume on a fresh agent in a different vendor.
export PLANAR_VENDOR=codex
planar resume 1            # 8-section packet, ready to drop into context
planar resume validate 1   # exit 0 if resumable; 1 with remediation if not
```

Every command above writes to the local SQLite store. Nothing leaves the machine until you opt into operational-plane sync (next section).

## What's going on? — `planar tree`

The fastest answer to "what work do I have, where does it live, and what's the structure?" is `planar tree`:

```bash
planar tree                              # active scope: plans → tasks → derived artifacts
planar tree --all-scopes                 # every scope (global section always rendered)
planar tree --status todo --prune        # only open work, empty branches hidden
planar tree --kind plan --depth 2        # plan outline, two levels deep
planar tree --json | jq                  # nested machine-readable shape
```

The verb walks `plans.parent_plan_id` for plan hierarchy, `tasks.plan_id` / `parent_task_id` for tasks and subtasks, and `entity_links(derives-from)` for artifacts / decisions / scenarios / questions attached to each plan. Read-only — no schema writes, no external calls.

The flag surface mirrors Unix `tree(1)` wherever the semantic translates (`-L` / `--depth`, `-I` / `--ignore`, `-P` / `--match`, `--ignore-case`, `-r`, `-t` / `-c` / `-U`, `--dirsfirst`, `--noreport`, `--prune`, `--ascii`, `-J` / `--json`), so muscle memory transfers. Filesystem-specific flags that have no Planar analog (`-a`, `-d`, `-f`, `-s`, `-p`, `-u`, `-g`, `-D`, `--inodes`, `--device`, `-Q`, `-X`, `-H`, `-v`, `--filelimit`, `--matchdirs`, `-C`, `-o`) are rejected at parse time with a clean error pointing at the Planar alternative — see [docs/cli-reference.md § Domain: `tree`](docs/cli-reference.md#domain-tree).

## Operational plane sync

Link local entities to Jira tickets or GitHub Issues, then pull / push deltas explicitly:

```bash
# Register a system. Credentials come from env vars or the gh CLI.
planar ext register jira my-jira \
    --base-url https://acme.atlassian.net \
    --project PROJ \
    --auth-env JIRA_USER,JIRA_TOKEN          # email,api-token pair

planar ext register github my-gh \
    --project rdrsss/planar                   # omit --auth-env to shell out to `gh auth token`

# Link a task to a remote ticket.
planar link task:1 --to my-jira:PROJ-1234 --sync two-way

# Sync on demand.
planar sync pull 1
planar sync push 1

# Conflicts surface explicitly; never resolved silently.
planar sync status                            # last_sync_status per link
planar sync resolve 7 --keep local            # resolves sync_event id 7

# Audit trail from either direction.
planar audit trail 1                          # everything for link 1
planar audit publish-decision 5               # post decision 5 to all linked remotes
planar audit handoff-readiness --threshold 90 # CI gate
```

Auth methods supported today: `token-env` (single var → bearer, or `USER,TOKEN` pair → basic), `gh-cli` (shells out to `gh auth token`). OAuth-stored / OS keychain is deferred. Background daemons are explicitly out of scope — sync is on-demand only.

For whole-feature propagation (create all operational counterparts for a plan tree at once), use `planar ext propagate <plan>`. The per-feature GitHub strategy (parent-issue, Projects v2, or zero-repo fallback) is selected automatically and cached for subsequent syncs. Use `--restrategize` to change strategy for an existing propagated plan.

## Workbench

The workbench is a bidirectionally synced drafting filesystem under `$PLANAR_WORKBENCH_ROOT` (default `~/.planar/workbench/`). Each active feature plan gets its own directory; Markdown files there are the drafting surface for specs, roadmaps, and task scaffolding.

```bash
# Pull the current feature tree from the DB into the workbench.
planar workbench pull plan:7

# Edit the Markdown files in ~/.planar/workbench/ as needed, then push changes back.
planar workbench push plan:7

# Check for FS↔DB divergence; conflicts require explicit resolve.
planar workbench status plan:7
planar workbench resolve <event-id> --prefer fs

# Archive the FS tree when work is done (DB retains all entities).
planar workbench archive plan:7

# Restore the FS tree byte-identically from the DB.
planar workbench restore plan:7
```

See `docs/architecture.md` for the full workbench design and conflict semantics, and `docs/workflows.md` for end-to-end recipes.

## Planning pipeline

The planning loop goes from goal-statement through spec drafting, task decomposition, execution, and operational-plane propagation:

```bash
# 1. Draft a feature spec and roadmap into the workbench.
/pl-spec-draft "migrate billing service to v2 API"

# 2. Review the Markdown files under ~/.planar/workbench/, edit as needed, then ingest.
/pl-spec-ingest --apply plan:7   # decomposes roadmap bullets into tasks + scenarios

# 3. Execute through the standard coder/reviewer loop.
/pl-orchestrator plan:7

# 4. Propagate the feature to the operational plane (Jira or GitHub Issues/Projects).
/pl-ext-propagate plan:7         # per-feature strategy per ADR-0006

# 5. Archive the workbench tree when done (DB retains all entities).
/pl-workbench-archive plan:7
```

Each step corresponds to a vendor skill. Claude uses `/pl-*` slash commands; Codex uses `$pl-*` skills such as `$pl-spec-draft`. The `pl-orchestrator` runs 5 explicit phases (planning / ingestion / execution / propagation / archive) with user gates between Phase 1→2 and before `--apply`.

## Configuration

`~/.planar/config.toml` is the configuration file (TOML format, hand-edited or written by an agent). Resolution order: environment variables override per-association config, which overrides top-level config, which overrides embedded defaults. Validate with `planar config validate`; inspect the effective result with `planar config show --effective`.

```toml
[templates]
default_set = "default"

[associations."org:my-org"]
github_lead_repo = "owner/repo"
default_template_set = "my-custom-set"
```

`PLANAR_CONFIG_PATH` overrides the config file location. `PLANAR_WORKBENCH_ROOT` overrides the workbench directory (default `~/.planar/workbench/`). See `docs/architecture.md` for the full configuration reference.

## Templates

JSON template files under `~/.planar/templates/<set>/<system>/<kind>.json` control the payloads sent to Jira and GitHub when `planar ext propagate` creates or updates external counterparts. Ten defaults ship with the binary (5 github-issues + 1 github-projects + 4 jira) and are extracted to `~/.planar/templates/default/` on `planar init`. Inspect them with `planar templates list` and render a preview with `planar templates render --entity task:42`. Override per association via the `default_template_set` config key. See `docs/architecture.md` for the full template shape and rendering context.

## Concepts

Three threads run through everything:

- **Three orthogonal axes.** *Storage scope* (one SQLite DB per user; the workbench is a bidirectionally synced drafting filesystem at `$PLANAR_WORKBENCH_ROOT`). *Entity scope* (every entity carries `(scope_kind, scope_id)` ∈ `{repo, association, global}`). *Active scope* (a stack of associations that drives default query filters). See [docs/concepts.md](docs/concepts.md).
- **Three operational context planes.** *Local* (the SQLite store; working memory). *Workbench* (bidirectionally synced Markdown filesystem under `~/.planar/workbench/`; the drafting surface for active features). *Operational* (Jira / GitHub Issues; the org's system of record). Each plane has its own audience and its own source-of-truth rules. See [docs/concepts.md](docs/concepts.md).
- **From-zero handoff.** A new agent process — different vendor, no prior session memory — must be able to resume an in-flight task with one command. The combined `handoff <task-id>` ritual creates and validates the resume packet atomically. `resume validate` is the CI gate. See [docs/concepts.md](docs/concepts.md).

## The schema is the contract

Twenty-seven migration files (`migrations/00001_foundation.up.sql` through `migrations/00027_external_sync_baseline.up.sql`) define Planar's schema. The runtime applies them from an embedded `migrations` Zig module produced by build-time codegen (`tools/gen_migrations.zig`); the public schema-version tracker is `schema_migrations`.

Read-side tooling — viewers, query CLIs, Obsidian bridges, future binaries — opens `~/.planar/planar.db` with `PRAGMA query_only = 1`, reads `schema_migrations` to verify version compatibility, and operates without going through the binary. The contract is the schema, not the codebase. See [docs/architecture.md](docs/architecture.md) for the schema overview.

## Vendor surfaces

The bundled agent specs and reference workflows use unified source + generated vendor outputs:

| Vendor | Source path | Install destination |
|--------|-------------|---------------------|
| Unified skill source | `skills/src/` | Rendered into vendor surfaces by `planar skills render` |
| Claude | `commands/claude/` (generated) | `~/.claude/commands/` |
| Codex | `skills/codex/` (generated) | `~/.codex/skills/<skill>` installed directory |
| Copilot | `skills/copilot/` (generated), `copilot/` (authored prompts/instructions) | `~/.copilot/skills/`, `~/.copilot/` |
| Planar agents | `agents/` | `~/.planar/agents/` |

Eight agent roles total. Three drive task execution: `orchestrator` (large tier), `coder` (medium tier), `reviewer` (large tier). Three drive the feature lifecycle: `planner` (drafts spec/roadmap/scenario docs), `ingestor` (decomposes docs into rich tasks), `ext-sync` (propagates a feature to a registered operational system). Two drive repo onboarding: `importer` (classifies and ingests an existing repo's planning content) and `synthesizer` (re-synthesizes planning artifacts from docs + code + git history). See [`agents/methodology.md`](agents/methodology.md) for the orchestration flow.

31 vendor surfaces per vendor (`pl-*.md` files under `commands/claude/`, `skills/codex/`, and `skills/copilot/`). Three invoke agent roles directly (`pl-orchestrator`, `pl-coder`, `pl-reviewer`); the rest are workflow wrappers around the `planar` CLI — spec pipeline (`pl-spec-draft`, `pl-spec-ingest`), workbench (`pl-workbench`, `pl-workbench-sync`, `pl-workbench-archive`), external systems (`pl-ext-propagate`, `pl-ext-create`, `pl-templates`), repo onboarding (`pl-import`, `pl-local-import`, `pl-synthesize`, `pl-workspace-scan`), docs (`pl-doc-promote`, `pl-doc-regenerate`), and per-entity CLI wrappers (`pl-plan`, `pl-task`, `pl-question`, `pl-scenario`, `pl-promote`, `pl-status`, `pl-health`, `pl-init`, `pl-scope`, `pl-sync`, `pl-resume`, `pl-handoff`, `pl-audit-trail`, `pl-help`). Vendor-surface drift is gated by `planar skills render --check` against an out-of-tree staging directory.

## Repository layout

The repo root IS the Zig package root: `build.zig` and `build.zig.zon` sit at the top level alongside the modules dir (`src/`), build-time codegen (`tools/`), the integration suite (`integration_tests/`), and the vendored SQLite amalgamation (`vendor/sqlite/`).

| Path | Role |
|------|------|
| `build.zig`, `build.zig.zon` | Zig build configuration and package manifest (package name `planar`, minimum Zig `0.16.0`) |
| `src/` | Zig modules (the runtime source tree) |
| `src/cmd/planar/` | Executable entry point — `main.zig`, runtime scaffolding, and per-verb handlers |
| `vendor/etcli/` | Vendored CLI parser + help/completion renderer ([etcli](https://github.com/rdrsss/etcli) extracted from the former in-tree `src/cli/`). Declared as a path dependency in `build.zig.zon`; provides the `cli` module imported by every binary. |
| `src/db/` | Database layer — connection wrappers, migration application, vendored-SQLite C bindings |
| `src/engine/` | Domain engine organized into buckets (`identity/`, `planning/`, `external/`, `runtime/`) and subsystem modules |
| `tools/gen_migrations.zig` | Build-time codegen: scans `migrations/` and emits a `migrations` Zig module the runtime embeds |
| `tools/gen_templates.zig` | Build-time codegen for embedded propagation templates (reads `templates/defaults/`) |
| `vendor/sqlite/` | Vendored SQLite amalgamation (`sqlite3.c`, `sqlite3.h`); compiled by `build.zig` into a static library |
| `integration_tests/` | End-to-end integration suites exercising the built binary via the `harness.zig` runner |
| `migrations/` | SQLite migrations in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`) — single authoritative source |
| `templates/defaults/` | Propagation templates (JSON) for external systems (`github-issues/`, `github-projects/`, `jira/`); embedded at build time |
| `templates/doc-prompts/`, `templates/entity/`, `templates/workspace-capabilities.toml` | Operator-editable defaults staged into `~/.planar/templates/` on install |
| `skills/src/` | Unified authored skill sources (`pl-*.md`) |
| `commands/claude/` | Generated Claude slash commands |
| `skills/codex/` | Generated Codex skills |
| `skills/copilot/`, `copilot/` | Generated Copilot skills + authored instructions/prompts |
| `agents/` | Vendor-neutral Planar agent role specs |
| `docs/` | User-facing reference docs (architecture, CLI, skills, concepts, workflows) |
| `scripts/` | Bash tooling (acceptance validators, session stats, git hooks); independent of the Zig build |
| `Makefile` | Thin wrapper around `zig build ...` invocations |
| `install.sh` | Source-checkout installer |

## Documentation

- **[docs/architecture.md](docs/architecture.md)** — system overview: storage model, context planes, schema contract, workbench, operational adapters.
- **[docs/cli-reference.md](docs/cli-reference.md)** — full CLI surface (commands, flags, exit codes across all domains).
- **[docs/skill-reference.md](docs/skill-reference.md)** — skill and agent role overview; when to use each surface.
- **[docs/concepts.md](docs/concepts.md)** — mental model: scope, association, plan, task, handoff, and the three operational context planes.
- **[docs/workflows.md](docs/workflows.md)** — end-to-end recipes (feature planning, sync, handoff, propagation).
- **[examples/](examples/)** — copy-paste oriented examples for drafting specs, reviewing them, ingesting them, launching the orchestrator, authoring workflows, and propagating to external systems.
- **[agents/methodology.md](agents/methodology.md)** — how the orchestrator / coder / reviewer agents collaborate; the 5-phase orchestrator flow.
- **[CLAUDE.md](CLAUDE.md)** — agent guide for working in this repo (symlinked to `AGENTS.md`).

**Historical context.** Founding tech spec, roadmap, ADRs, and feature specs are preserved as Planar's own artifacts under `~/.planar/workbench/project_planar/p44-planar-founding-archive/` (accessible via `planar artifact list --plan 44` or `planar artifact show <id>`).

## License

To be determined. License selection is part of the first formal release.
