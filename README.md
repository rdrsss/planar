# Planar

> AI coding agents lose context constantly: at compaction, at session end, at vendor switch, at every interruption. The user pays for that loss in re-explanation, lost decisions, and forgotten constraints. Planar is local infrastructure that keeps the user's accumulated context durable across agents, vendors, and sessions.

Planar is a local-first task tracker — Jira-shaped, but living next to the developer instead of inside the org's stack — for the structured intent and outputs of agent work: **tech specs, plans, tasks, decisions, questions, and test scenarios**, plus the **sessions** and **snapshots** that capture how the work happened. It scopes that state across repositories and organizations via associations, exports reviewable Markdown for team workbenches, and integrates with operational issue trackers (Jira, GitHub Issues) so org-level visibility and audit trails come for free.

It is **vendor-agnostic by design**: Claude, Codex, and Copilot are first-class today, with additional agent runtimes expected. The user's accumulated context — plans, ADRs, sessions, audit trails — outlives any particular agent.

## Status

Planar is a local-first tool built as five binaries: `planar` (operator surface), `planar-agent` (agent-coordination writes), `planar-watch` (read-only viewer), `planar-execute` (the deterministic Lua workflow engine, which holds no DB handle and reaches state only through a constrained CLI host surface), and `planar-ext` (the operational-plane binary — Jira and GitHub Issues adapters; opens SQLite directly, read-only on planning tables and read-write on exactly `external_links` / `external_systems` / `sync_events`). The schema has thirty-three migrations, through `migrations/00033_rename_blocks_to_depends_on.up.sql`; the C++ runtime embeds and applies them at startup. Vendored SQLite is compiled from its pinned amalgamation; no system SQLite library is required.

**History.** Repo split — the original Go implementation (M1–M19) is preserved at `github.com/rdrsss/planar-go-archive.git`. Planar then ported from Go to Zig, and from Zig to C++26 (branch `rewrite/cpp26`). **The M10 cutover has landed:** the Zig tree under `zig/`, kept buildable as the port's parity oracle, was deleted once its state-differential evidence came back clean, and C++26 is now the only implementation (see [docs/architecture.md](docs/architecture.md)).

## Prerequisites

Planar shells out to a small set of external tools. On macOS, install them via Homebrew:

```bash
brew install cmake ninja llvm python git gh jq ripgrep tbb
```

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries. `install.sh` preflights the exact preset compilers, `/opt/homebrew/opt/llvm/bin/clang` and `/opt/homebrew/opt/llvm/bin/clang++`, before it invokes CMake. Use the repository's `debug` and `release` presets; see [toolchain parity](docs/toolchain-parity.md).
- `tbb` (>= 2021.5) — **required to build**, since `cmake/dependencies.cmake` vendors Mt-KaHyPar (decision 1006, task 6459) as a pinned CPM source block and Mt-KaHyPar's own CMake `find_package(TBB)`s it. Unlike every other third-party dependency this tree takes, TBB is **not** vendored as source: upstream states TBB does not support static linking, so this is a deliberately accepted dynamic system dependency rather than a hermetic one. `install.sh` preflights `brew --prefix tbb` and fails fast if it is missing, matching CMake's own `find_package(TBB)` failure.
- `python3` — **required to configure**. Centurion (added as a CMake subdirectory by `cmake/centurion.cmake`, plan 1033) generates a minimized Botan amalgamation at configure time by running Botan's `configure.py`. It is the only host program Centurion's stack adds: `protoc` and the gRPC C++ plugin are built in-tree from Centurion's own vendored protobuf and gRPC, not taken from the host.
- Scriptorium is built from `src/tools/scriptorium/` and installed by CMake; no external Scriptorium executable is required. Its Inja and nlohmann dependencies are pinned under `vendor/` and private to the tool.
- Network access plus a GitHub token on the **first** configure of a checkout — `cmake/dependencies.cmake` fetches first-party dependencies (currently Centurion, a private repository) into the gitignored `external/` directory rather than committing them under `vendor/`. Run `GITHUB_TOKEN=$(gh auth token) cmake --preset debug` once; later configures reuse `external/` offline. Third-party dependencies remain committed under `vendor/` and never need the network.
- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — required by the bundled agent skills (`pl-spec-draft`, `pl-spec-ingest`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session below (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.
- `tabularium` — required only by the bundled documentation-maintenance
  workflows. It is a separate project and is not built or installed by Planar;
  install it from `locumipsum/tabularium` when using those workflows.

The full source-checkout installer also uses the base-system utilities declared
in `install.sh`'s `BUILD_DEPS` / `RUN_DEPS` manifests (`awk`, `basename`, `cat`, `chmod`, `cmp`,
`cp`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mktemp`, `mv`,
`readlink`, `rm`, `rmdir`, `shasum`, `tar`, `tr`, and `uname`) alongside CMake, Ninja, and the exact pinned LLVM compiler paths above. These ship with supported Unix-like systems;
the installer preflights them before making changes.

### Optional / research tools

- `mtkahypar` — the external [Mt-KaHyPar](https://github.com/kahypar/mt-kahypar) hypergraph partitioner backs the optimal arm of `planar groups recommend --solver=mtkahypar`. As of decision 1006 (tasks 6459/6460) it is **vendored from source** via `cmake/dependencies.cmake` and linked directly into the C++ tree (`libmtkahypar`) — the earlier `--with-mtkahypar` Python-wheel adapter (`bin/mtkahypar`, `opt/mtkahypar/<version>/venv/`) is retired and no `python3` step is needed for this feature any more. The linkage is **off by default**: `mtkahypar` is an `EXCLUDE_FROM_ALL` CMake target and its debug build is ~330MB, so a plain `cmake --build` never compiles it. Configure with `-DPLANAR_WITH_MTKAHYPAR=ON` to build and link the real seam (requires `tbb`, see above); without that flag — including every `install.sh` build today — `groups recommend --solver mtkahypar` degrades gracefully to greedy and reports `optimal_available:false`. This fallback is a deliberate, permanent contract, not a placeholder for missing Mt-KaHyPar support.

`sqlx-cli` and `sqlite3` are only needed for ad-hoc developer workflows against a scratch database (see [Build from source](#build-from-source)); the runtime embeds migrations via build-time codegen and uses the vendored SQLite amalgamation, so neither CLI is a runtime dependency. Install the optional `sqlx-cli` for authoring new migration pairs:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
```

## Install

The C++ project root is the repo root. Clone the repo, then build and install with the release preset:

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
cmake --preset release -DPLANAR_VERSION_META=ON
cmake --build build/release
cmake --install build/release --prefix "$HOME/.planar"
```

This puts the five Planar binaries and the in-tree `scriptorium` renderer in `~/.planar/bin/`. The four that open a database (all but `planar-execute`, which holds no SQLite handle at all) statically link the vendored SQLite amalgamation — no system library dependency.

Add `~/.planar/bin` to your `$PATH`, then verify:

```bash
export PATH="$HOME/.planar/bin:$PATH"
planar health
```

This installs the five Planar binaries and the Scriptorium renderer. Migrations and propagation templates are *embedded* at build time, so the CLI works standalone against a local database. Agent specs, slash commands, and skills are **not** embedded — they are separate source files rendered and staged by `install.sh` — so none of the vendor surfaces (Claude `/pl-*` slash commands, Codex skills, Copilot skills) are wired up by a `cmake --install` alone; for those, use the [full install](INSTALL.md#full-install-installsh).

The full install also puts a stock `centuriond` — the Centurion workflow daemon `planar-execute` is becoming a client of (plan 1033) — at `~/.planar/bin/centuriond`, with its migrations and a `build-identity.json` under `~/.planar/share/centurion/`. It is built from the same pinned Centurion archive as a separate CMake project (or taken from a checksum-verified Centurion release binary once the pinned tag publishes one) by `scripts/install-centuriond.sh`; the first build compiles Centurion's gRPC stack and takes several minutes. A bare `cmake --install` does not install it.

## Build from source

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
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
planar tree --status todo                # only open work
planar tree --kind plan --depth 2        # plan outline, two levels deep
planar tree --json | jq                  # nested machine-readable shape
```

The verb walks `plans.parent_plan_id` for plan hierarchy, `tasks.plan_id` / `parent_task_id` for tasks and subtasks, and `entity_links(derives-from)` for artifacts / decisions / scenarios / questions attached to each plan. Read-only — no schema writes, no external calls.

The flag surface is exactly seven flags — `--scope`, `--all-scopes`, `--depth`, `--kind`, `--status`, `--sort`, `--json` — and `--kind` / `--status` each take a single value. It does not mirror Unix `tree(1)`: none of the `tree(1)` flags an earlier edition of this README listed (`-L`, `-I`, `-P`, `--prune`, `--noreport`, `--dirsfirst`, `-J`, …) are accepted, and every one of them fails at parse time with exit 2. See [docs/cli-reference.md § Domain: `tree`](docs/cli-reference.md#domain-tree).

## Operational plane sync

Link local entities to Jira tickets or GitHub Issues, then pull / push deltas explicitly:

`ext` and `sync` verbs run on **`planar-ext`**, the operational-plane binary (decisions 995–1001): it opens SQLite directly with read-only access to planning tables and read-write access to exactly `external_links` / `external_systems` / `sync_events`, and owns both adapters, Jira and GitHub Issues together. `link`/`unlink` and `audit` stay on `planar` (the operator binary still records and reads external links directly).

```bash
# Register a system. Credentials come from env vars or the gh CLI.
planar-ext ext register jira my-jira \
    --base-url https://acme.atlassian.net \
    --project PROJ \
    --auth-env JIRA_USER,JIRA_TOKEN          # email,api-token pair

planar-ext ext register github my-gh \
    --project rdrsss/planar                   # omit --auth-env to shell out to `gh auth token`

# Link a task to a remote ticket.
planar link task:1 --to my-jira:PROJ-1234 --sync two-way

# Sync on demand. planar-ext no longer writes remote values into planning
# entities (decision 996): it fetches and emits remote_title/remote_status;
# an agent verifies and calls `planar` to make the actual planning write.
planar-ext sync pull 1
planar-ext sync push 1

# Conflicts surface explicitly; never resolved silently.
planar-ext sync status                            # last_sync_status per link
planar-ext sync resolve 7 --keep local            # resolves sync_event id 7

# Audit trail from either direction.
planar audit trail 1                          # everything for link 1
planar audit publish-decision 5               # post decision 5 to all linked remotes
planar audit handoff-readiness --threshold 90 # CI gate
```

Auth methods supported today: `token-env` (single var → bearer, or `USER,TOKEN` pair → basic), `gh-cli` (shells out to `gh auth token`). OAuth-stored / OS keychain is deferred. Background daemons are explicitly out of scope — sync is on-demand only.

For whole-feature propagation (create all operational counterparts for a plan tree at once), use `planar-ext ext propagate <plan>`. GitHub uses the single-repo parent-issue strategy when the feature touches exactly one repo — the only strategy this verb executes today (task 6421 scoped the C++ port to this arm; Jira's epic-hierarchy strategy is not yet wired into this whole-tree verb, see [docs/cli-reference.md](docs/cli-reference.md#planar-ext-ext-propagate-plan)). The multi-repo `projects-v2` strategy is permanently cut (decision 1001) and will not exist — it is recognized only to be refused with a pointer at the alternative. Use `--restrategize` to change strategy for an existing propagated plan.

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

JSON template files under `~/.planar/templates/<set>/<system>/<kind>.json` control the payloads sent to Jira and GitHub when `planar-ext ext propagate` creates or updates external counterparts. Ten defaults ship with the binary (5 github-issues + 1 github-projects + 4 jira) and are extracted to `~/.planar/templates/default/` on `planar init`. Inspect them with `planar templates list` and render a preview with `planar templates render --entity task:42`. Override per association via the `default_template_set` config key. See `docs/architecture.md` for the full template shape and rendering context.

## Concepts

Three threads run through everything:

- **Three orthogonal axes.** *Storage scope* (one SQLite DB per user; the workbench is a bidirectionally synced drafting filesystem at `$PLANAR_WORKBENCH_ROOT`). *Entity scope* (every entity carries `(scope_kind, scope_id)` ∈ `{repo, association, global}`). *Active scope* (a stack of associations that drives default query filters). See [docs/concepts.md](docs/concepts.md).
- **Three operational context planes.** *Local* (the SQLite store; working memory). *Workbench* (bidirectionally synced Markdown filesystem under `~/.planar/workbench/`; the drafting surface for active features). *Operational* (Jira / GitHub Issues; the org's system of record). Each plane has its own audience and its own source-of-truth rules. See [docs/concepts.md](docs/concepts.md).
- **From-zero handoff.** A new agent process — different vendor, no prior session memory — must be able to resume an in-flight task with one command. The combined `handoff <task-id>` ritual creates and validates the resume packet atomically. `resume validate` is the CI gate. See [docs/concepts.md](docs/concepts.md).

## The schema is the contract

Thirty-three migration files (`migrations/00001_foundation.up.sql` through `migrations/00033_rename_blocks_to_depends_on.up.sql`) define Planar's schema. The runtime applies them from a generated `planar.db.migrations` module, `#embed`-produced at CMake configure time (`cmake/generate_migrations.cmake`); the public schema-version tracker is `schema_migrations`.

Read-side tooling — viewers, query CLIs, Obsidian bridges, future binaries — opens `~/.planar/planar.db` with `PRAGMA query_only = 1`, reads `schema_migrations` to verify version compatibility, and operates without going through the binary. The contract is the schema, not the codebase. See [docs/architecture.md](docs/architecture.md) for the schema overview.

## Vendor surfaces

The bundled agent specs and reference workflows use unified source + generated vendor outputs:

| Surface | Canonical or staged path | Install destination |
|--------|-------------|---------------------|
| Unified skill source | `skills/src/` | Rendered by `scriptorium` at install time (`planar skills` is a retirement notice with no subcommands); never installed directly |
| Claude | `$PLANAR_HOME/commands/claude/` (generated stage) | `~/.claude/commands/` |
| Codex | `$PLANAR_HOME/codex-skills/` (generated stage) | `$CODEX_HOME/skills/` (normally `~/.codex/skills/`) |
| Copilot | `$PLANAR_HOME/copilot-skills/` (generated stage), `copilot/` (authored prompts/instructions) | `~/.copilot/skills/`, `~/.copilot/` |
| Planar agents | `agents/` | `~/.planar/agents/` |

Sixteen agent roles cover orchestration and review, planning and ingestion,
external propagation, repo adoption, introspection and feedback triage,
documentation classification/authoring, guarded sync reconciliation, testing,
and closeout. The specialist boundaries are deliberate: `documenter` is
read-only, `doc-author` writes only approved prose, `feedback-triager` and
`sync-reconciler` coordinate preview-gated changes, and planning-state writes
still go through the owning CLI binary. See
[`agents/methodology.md`](agents/methodology.md) for orchestration and
[`docs/skill-reference.md`](docs/skill-reference.md) for the role inventory.

Forty-one unified `pl-*` skill sources render for each selected vendor. In
addition to the planning, workbench, external, onboarding, and entity
workflows, the inventory includes intent-oriented status/help, durable
knowledge, operational observation, the complete local lifecycle (with
`pl-local-import` retained as a compatibility wrapper), preview/apply
introspection and feedback triage, guarded sync reconciliation, and gated
documentation maintenance. Repo-relative vendor trees are not checked in;
canonical edits belong in `skills/src/`, and drift is gated by semantic lint
plus the in-tree `scriptorium check` / `scriptorium status` against an
out-of-tree staging directory (`planar skills render` no longer exists).

## Repository layout

The repo root IS the CMake project root: `CMakeLists.txt` and `CMakePresets.json` sit at the top level alongside the C++ module tree (`src/`), build-support CMake modules (`cmake/`), the CPM-cached vendor sources (`vendor/`), and the tool sources (`src/tools/`). There is no second implementation tree — `zig/` was deleted at the M10 cutover.

| Path | Role |
|------|------|
| `CMakeLists.txt`, `CMakePresets.json` | Top-level CMake project (C++26, modules) and the pinned `debug`/`release` presets |
| `src/cmd/` | One directory per binary — `planar/`, `planar-agent/`, `planar-watch/`, `planar-execute/`, `planar-ext/` — each its own CMake target |
| `src/lib/` | Shared C++ modules: `db/` (connection + migrations), `cliapp/` (CLI11-backed parser wrapper), `engine/` (domain logic, bucketed as `identity/`, `planning/`, `external/`, `runtime/` plus subsystem dirs), `adapter/`, `http/`, and other leaf libraries |
| `src/tools/` | Project tooling, one directory per tool (`cli_usage_lint/`, `surface_lint/`, `scriptorium/`) |
| `cmake/dependencies.cmake` | Every third-party dependency as a pinned `CPMAddPackage(...)` (release archive + SHA256, cached under `vendor/`) |
| `cmake/generate_migrations.cmake`, `cmake/generate_templates.cmake` | Configure-time codegen: `#embed`s `migrations/` and `templates/defaults/` into generated modules the runtime embeds |
| `vendor/` | CPM's committed source cache — pinned release archives only, no `git clone`/submodule vendoring |
| `src/cmd/parity_harness.hpp` | The cross-process (black-box) test harness — `run_pinned()` execs a built binary over fixed argv in a scratch environment with its own `PLANAR_DB` and `HOME`. Cases live in `src/cmd/*/parity.t.cpp` and `src/cmd/planar/cross_process.t.cpp` |
| `migrations/` | SQLite migrations in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`) — single authoritative source |
| `templates/defaults/` | Propagation templates (JSON) for external systems (`github-issues/`, `jira/`); embedded at build time |
| `templates/doc-prompts/`, `templates/defaults/`, `templates/workspace-capabilities.toml` | Operator-editable defaults staged into `~/.planar/templates/` on install |
| `skills/src/` | Unified authored skill sources (`pl-*.md`) |
| `$PLANAR_HOME/commands/claude/` | Generated Claude staging tree (install output, not checked in) |
| `$PLANAR_HOME/codex-skills/` | Generated Codex staging tree (install output, not checked in) |
| `$PLANAR_HOME/copilot-skills/`, `copilot/` | Generated Copilot staging tree + checked-in authored instructions/prompts |
| `agents/` | Vendor-neutral Planar agent role specs |
| `docs/` | User-facing reference docs (architecture, CLI, skills, concepts, workflows) |
| `scripts/` | Bash tooling (acceptance validators, session stats, git hooks); independent of the build |
| `Makefile` | Thin wrapper around `cmake --preset` / `cmake --build` / `ctest` invocations |
| `install.sh` | Source-checkout installer; builds and installs the C++ binaries via CMake |

## Documentation

- **[docs/architecture.md](docs/architecture.md)** — system overview: storage model, context planes, schema contract, workbench, operational adapters.
- **[docs/cli-reference.md](docs/cli-reference.md)** — full CLI surface (commands, flags, exit codes across all domains).
- **[docs/skill-reference.md](docs/skill-reference.md)** — skill and agent role overview; when to use each surface.
- **[docs/concepts.md](docs/concepts.md)** — mental model: scope, association, plan, task, handoff, and the three operational context planes.
- **[docs/workflows.md](docs/workflows.md)** — end-to-end recipes (feature planning, sync, handoff, propagation).
- **[docs/lifecycles.md](docs/lifecycles.md)** — every state machine and workflow as a diagram: transition matrices, verb-to-edge maps, engine roll-ups, the claim ritual, sync and propagation flows.
- **[examples/](examples/)** — copy-paste oriented examples for drafting specs, reviewing them, ingesting them, launching the orchestrator, authoring workflows, and propagating to external systems.
- **[agents/methodology.md](agents/methodology.md)** — how the orchestrator / coder / reviewer agents collaborate; the 5-phase orchestrator flow.
- **[CLAUDE.md](CLAUDE.md)** — agent guide for working in this repo (symlinked to `AGENTS.md`).

**Historical context.** Founding tech spec, roadmap, ADRs, and feature specs are preserved as Planar's own artifacts under `~/.planar/workbench/project_planar/p44-planar-founding-archive/` (accessible via `planar artifact list --plan 44` or `planar artifact show <id>`).

## License

To be determined. License selection is part of the first formal release.
