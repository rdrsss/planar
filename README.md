# Planar

Planar is a durable context plane and synchronization mechanism for
long-running agent orchestrations. It holds the state an orchestration
accumulates — tech specs, plans, tasks, decisions, questions, test scenarios,
and the sessions and handoff snapshots that record how the work happened — in a
local SQLite database whose schema, versioned through ordered migrations, is the
contract every participant writes against. That state is addressed by scope
(repository, association, global) rather than by process, so it survives
compaction, session end, and vendor switch, and is readable by any agent that
joins the orchestration later.

Synchronization operates on three planes. **Coordination:** leases and claims
serialize concurrent agents against the same task — exactly one holds a work
claim at a time, and an abandoned claim expires on its TTL instead of
deadlocking the task. **Drafting:** a bidirectionally synced workbench
filesystem projects planning state to reviewable Markdown and ingests operator
edits back into the database. **Operational:** adapters reconcile with issue
trackers (Jira, GitHub Issues) through explicit link records and
proposal-shaped pulls, so org-level visibility and audit trails come for free
without a remote system silently overwriting local intent.

**Project continuity belongs to the context plane and remains owned by the
user, independent of the provider or harness running the next task.** You can
move between providers and harnesses while keeping the same context plane:
recorded decisions, task ownership, dependencies, and handoff context remain
available to the next agent. Each participating harness uses Planar's CLI and
coordination protocol to continue from that shared state.

Planar is local-first and Jira-shaped, but it lives next to the developer
rather than inside the org's stack, and it is **provider- and harness-agnostic
by design**: Claude, Codex, Copilot, and Gemini are first-class today, with
additional agent runtimes expected.

## Status

Planar ships as five binaries over one shared SQLite database, each with a
disjoint write surface. The capability boundary is each binary's verb set, not a
runtime ACL, and three of the five have capability-boundary tests that pin it.
The schema has thirty-nine migrations, through
`migrations/00039_workflow_runs_lease.up.sql`; the C++ runtime embeds and
applies them at startup. Vendored SQLite is compiled from its pinned
amalgamation, so no system SQLite library is required.

- **`planar`** — the operator surface. Writes planning entities (plans, tasks,
  questions, scenarios, decisions, artifacts, annotations) and manual
  `tasks.status` transitions. This is the only supported access layer for
  workflows: skills and agents compose its verbs rather than writing to the
  database directly.
- **`planar-agent`** — the agent-callable coordination surface. Atomic writes to
  `agent_actions`, `agent_work_claims`, the routing-dispatch authorization
  tables, and `tasks.status` only as part of a coordinated operation. Its
  terminal verbs (`complete` / `fail` / `release` / `block`) flip the claim and
  the task status in a single transaction, which is why the claim ritual must
  not be split across two commands.
  It also hosts the **host build and test queue**: `planar-agent queue run --
  <command>` waits its turn in a queue shared by every project on the machine,
  so two agents do not build at once, and keeps its state in `planar.db`, the
  same database as everything else. `planar-agent queue rule` prints the rule
  that tells an agent to queue its builds and tests, for pasting into a
  project's own agent guide; `planar-watch queue` shows the queue.
- **`planar-watch`** — the read-only viewer. Opens SQLite via `file:?mode=ro`
  and performs no writes at all. Agent observability — action topology, live
  claims, feeds — lives here.
- **`planar-execute`** — the deterministic Lua workflow engine. Runs a workflow
  over allowlisted host functions (`cli` / `git` / `fs` / `flow` / `ctx`),
  holds no SQLite handle at all, and reaches Planar state only by shelling
  `planar` / `planar-agent`. It exposes no model-spawning host function: it is
  a workflow engine a caller invokes, not a harness.
- **`planar-ext`** — the operational plane. Owns both external adapters, Jira
  and GitHub Issues, behind one `external_adapter` interface. Opens SQLite
  directly: read-only on planning tables, read-write on exactly
  `external_links` / `external_systems` / `sync_events`, enforced by a
  `sqlite3_set_authorizer` allowlist on the parsed table name. `sync pull`
  emits `remote_title` / `remote_status` proposals for an agent to verify
  rather than applying remote values to planning entities itself.

## Prerequisites

Planar shells out to a small set of external tools. On macOS, install them via Homebrew:

```bash
brew install cmake ninja llvm python git gh jq ripgrep tbb
```

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries. Both presets resolve the toolchain through `cmake/llvm-toolchain.cmake`, which discovers the prefix (an explicit `-DPLANAR_LLVM_PREFIX` first, then `brew --prefix llvm`, then apt.llvm.org's versioned prefixes and `PATH`) and refuses a candidate that lacks a modules-enabled `libc++`. `install.sh` additionally preflights the Homebrew paths `/opt/homebrew/opt/llvm/bin/clang` and `clang++` before invoking CMake. See [toolchain parity](docs/toolchain-parity.md).
- `tbb` (>= 2021.5) — **required to build**, since `cmake/dependencies.cmake` vendors Mt-KaHyPar (decision 1006, task 6459) as a pinned CPM source block and Mt-KaHyPar's own CMake `find_package(TBB)`s it. Unlike every other third-party dependency this tree takes, TBB is **not** vendored as source: upstream states TBB does not support static linking, so this is a deliberately accepted dynamic system dependency rather than a hermetic one. `install.sh` preflights `brew --prefix tbb` and fails fast if it is missing, matching CMake's own `find_package(TBB)` failure.
- `python3` — **required to configure**. Centurion (added as a CMake subdirectory by `cmake/centurion.cmake`, plan 1033) generates a minimized Botan amalgamation at configure time by running Botan's `configure.py`. It is the only host program Centurion's stack adds: `protoc` and the gRPC C++ plugin are built in-tree from Centurion's own vendored protobuf and gRPC, not taken from the host. `install.sh` also runs it as its old-queue-database retirement reader (`scripts/install-lib/queue_retire.py`, standard library and `ctypes` only; it runs no other program), and `migrations/README.md`'s counter-reset recipe runs the log helper `scripts/queue-logs-after-reset.py` with it.
- Scriptorium is built from `src/tools/scriptorium/` and installed by CMake; no external Scriptorium executable is required. It needs no dependency beyond Glaze, which the rest of the tree already vendors.
- Network access plus a GitHub token on the **first** configure of a checkout — `cmake/dependencies.cmake` fetches first-party dependencies (currently Centurion, a private repository) into the gitignored `external/` directory rather than committing them under `vendor/`. Run `GITHUB_TOKEN=$(gh auth token) cmake --preset debug` once; later configures reuse `external/` offline. Third-party dependencies remain committed under `vendor/` and never need the network.
- `docker` — optional, developer-only. `make linux-gate` builds and tests the tree on Debian trixie in a container (see [docs/testing.md](docs/testing.md#the-linux-gate)). It is not an installer dependency.
- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — required by the bundled agent skills (`pl-spec-draft`, `pl-spec-ingest`, `pl-orchestrator`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
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

- `mtkahypar` — the external [Mt-KaHyPar](https://github.com/kahypar/mt-kahypar) hypergraph partitioner backs the optimal arm of `planar groups recommend --solver=mtkahypar`. As of decision 1006 (tasks 6459/6460) it is **vendored from source** via `cmake/dependencies.cmake` and linked directly into the C++ tree (`libmtkahypar`) — the earlier `--with-mtkahypar` Python-wheel adapter (`bin/mtkahypar`, `opt/mtkahypar/<version>/venv/`) is retired and no `python3` step is needed for this feature any more. The linkage is **off by default**: `mtkahypar` is an `EXCLUDE_FROM_ALL` CMake target, so a plain `cmake --build` never compiles it. Configure with `-DPLANAR_WITH_MTKAHYPAR=ON` to build and link the real seam (requires `tbb`, see above), or pass `--with-solver` to `install.sh`; without it `groups recommend --solver mtkahypar` degrades gracefully to greedy and reports `optimal_available:false`. This fallback is a deliberate, permanent contract, not a placeholder for missing Mt-KaHyPar support.

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

Migrations and propagation templates are *embedded* at build time, so the CLI works standalone against a local database. Agent specs, slash commands, and skills are **not** embedded — they are separate source files rendered and staged by `install.sh` — so none of the vendor surfaces (Claude `/pl-*` slash commands, Codex, Copilot, and Gemini skills) are wired up by a `cmake --install` alone; for those, use the [full install](INSTALL.md#full-install-installsh).

The full install also puts a stock `centuriond` — the Centurion workflow daemon `planar-execute` is becoming a client of (plan 1033) — at `~/.planar/bin/centuriond`, with its migrations and a `build-identity.json` under `~/.planar/share/centurion/`. It is built from the same pinned Centurion archive as a separate CMake project (or taken from a checksum-verified Centurion release binary once the pinned tag publishes one) by `scripts/install-centuriond.sh`; the first build compiles Centurion's gRPC stack and takes several minutes. A bare `cmake --install` does not install it.

## Build from source

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

On a machine where several agents build at once, send builds and tests through
the host queue instead of running them directly, for example `planar-agent
queue run --detach -- make test` (see [docs/testing.md](docs/testing.md)).

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
planar decision add "Deprecate v1 over two releases" --body "Warn in release N, remove in N+2"

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
planar-ext sync resolve 7 --keep local \
    --evidence-token <token> \
    --expected-local-updated-at <timestamp>       # resolves sync_event id 7

# Audit trail from either direction.
planar audit trail --link 1                   # everything for link 1
planar audit publish-decision 5               # post decision 5 to all linked remotes
planar audit handoff-readiness --threshold 90 # CI gate
```

Auth methods supported today: `token-env` (single var → bearer, or `USER,TOKEN` pair → basic), `gh-cli` (shells out to `gh auth token`). OAuth-stored / OS keychain is deferred. Background daemons are explicitly out of scope — sync is on-demand only.

For whole-feature propagation (create all operational counterparts for a plan tree at once), use `planar-ext ext propagate <plan>`. GitHub uses the single-repo parent-issue strategy when the feature touches exactly one repo — the only strategy this verb executes today (task 6421 scoped the C++ port to this arm; Jira's epic-hierarchy strategy is not yet wired into this whole-tree verb, see [docs/cli-reference.md](docs/cli-reference.md#planar-ext-ext-propagate-plan)). The multi-repo `projects-v2` strategy is permanently cut (decision 1001) and will not exist — it is recognized only to be refused with a pointer at the alternative. Use `--restrategize` to change strategy for an existing propagated plan.

## Workbench

The workbench is a bidirectionally synced drafting filesystem under `$PLANAR_WORKBENCH_ROOT` (default `~/.planar/workbench/`). Each active feature plan gets its own directory; Markdown files there are the drafting surface for specs, roadmaps, and task scaffolding.

```bash
# Push the current feature tree from the DB into the workbench (DB→FS).
planar workbench push 7

# Edit the Markdown files in ~/.planar/workbench/ as needed, then pull changes back (FS→DB).
planar workbench pull 7

# Check for FS↔DB divergence; conflicts require explicit resolve.
planar workbench status 7
planar workbench resolve <event-id> --prefer fs

# Archive the FS tree when work is done (DB retains all entities).
planar workbench archive 7

# Restore the FS tree byte-identically from the DB.
planar workbench restore 7
```

See `docs/architecture.md` for the full workbench design and conflict semantics, and `docs/workflows.md` for end-to-end recipes.

## Planning pipeline

The planning loop goes from goal-statement through spec drafting, task decomposition, execution, and operational-plane propagation:

```bash
# 1. Draft a feature spec and roadmap into the workbench.
/pl-spec-draft "migrate billing service to v2 API"

# 2. Review the Markdown files under ~/.planar/workbench/, edit as needed, then ingest.
/pl-spec-ingest 7 --apply        # decomposes roadmap bullets into tasks + scenarios

# 3. Execute through the standard coder/reviewer loop.
/pl-orchestrator 7

# 4. Propagate the feature to the operational plane (Jira or GitHub Issues).
/pl-ext-propagate 7

# 5. Archive the workbench tree when done (DB retains all entities).
/pl-workbench-archive archive plan:7
```

Each step corresponds to a vendor skill. Claude uses `/pl-*` slash commands; Codex uses `$pl-*` skills such as `$pl-spec-draft`. The `pl-orchestrator` runs explicit phases (planning / spec review / ingestion / execution / finalization / propagation / archive / documentation) with user gates after drafting, before ingestion `--apply`, and before dispatch.

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

JSON template files under `~/.planar/templates/<set>/<system>/<kind>.json` control the payloads sent to Jira and GitHub when `planar-ext ext propagate` creates or updates external counterparts. Ten defaults ship with the binary (5 github-issues + 1 github-projects + 4 jira) and are extracted to `~/.planar/templates/default/` by `planar templates init`. Inspect them with `planar templates list` and render a preview with `planar templates render default github-issues issue --entity task:42`. Override per association via the `default_template_set` config key. See `docs/architecture.md` for the full template shape and rendering context.

## Concepts

Three threads run through everything:

- **Three orthogonal axes.** *Storage scope* (one SQLite DB per user; the workbench is a bidirectionally synced drafting filesystem at `$PLANAR_WORKBENCH_ROOT`). *Entity scope* (every entity carries `(scope_kind, scope_id)` ∈ `{repo, association, global}`). *Active scope* (derived from the current working directory, or passed per verb with `--scope`; there is no scope stack). See [docs/concepts.md](docs/concepts.md).
- **Three operational context planes.** *Local* (the SQLite store; working memory). *Workbench* (bidirectionally synced Markdown filesystem under `~/.planar/workbench/`; the drafting surface for active features). *Operational* (Jira / GitHub Issues; the org's system of record). Each plane has its own audience and its own source-of-truth rules. See [docs/concepts.md](docs/concepts.md).
- **From-zero handoff.** A new agent process — different vendor, no prior session memory — must be able to resume an in-flight task with one command. The combined `handoff <task-id>` ritual creates and validates the resume packet atomically. `resume validate` is the CI gate. See [docs/concepts.md](docs/concepts.md).

## The schema is the contract

Thirty-nine migration files (`migrations/00001_foundation.up.sql` through `migrations/00039_workflow_runs_lease.up.sql`) define Planar's schema. The runtime applies them from a generated `planar.db.migrations` module, `#embed`-produced at CMake configure time (`cmake/generate_migrations.cmake`); the public schema-version tracker is `schema_migrations`.

Read-side tooling — viewers, query CLIs, Obsidian bridges, future binaries — opens `~/.planar/planar.db` with `PRAGMA query_only = 1`, reads `schema_migrations` to verify version compatibility, and operates without going through the binary. The contract is the schema, not the codebase. See [docs/architecture.md](docs/architecture.md) for the schema overview.

## Vendor surfaces

The bundled agent specs and reference workflows use unified source + generated vendor outputs:

| Surface | Source or output | Installed skill target |
|--------|------------------|------------------------|
| Unified skill source | `skills/src/` in this repository | Never installed directly (`planar skills` is a retirement notice with no subcommands) |
| Claude | `$PLANAR_HOME/commands/claude/pl-*.md` | Symlinked as flat slash commands in `~/.claude/commands/` |
| Codex | `$PLANAR_HOME/skills/codex/pl-*/SKILL.md` | Copied into `$CODEX_HOME/skills/pl-*/SKILL.md` (normally `~/.codex/skills/`); also staged under `$PLANAR_HOME/codex-skills/` |
| Copilot | `$PLANAR_HOME/skills/copilot/pl-*.md` | Materialized as `~/.copilot/skills/pl-*/SKILL.md`, via `$PLANAR_HOME/copilot-skills/` |
| Gemini | `$PLANAR_HOME/skills/gemini/pl-*.md` | Materialized as `~/.gemini/antigravity-cli/skills/pl-*/SKILL.md`, via `$PLANAR_HOME/gemini-skills/` |

Claude consumes flat command files, while Codex consumes skill directories
directly. Copilot and Gemini also consume skill directories; the installer
converts their flat rendered files into `SKILL.md` directories. Installed Codex,
Copilot, and Gemini skills are real copies so their loaders can discover them
without following directory symlinks. Planar agent sources live in `agents/` and
have separate vendor-specific outputs and install targets.

Fifteen agent roles cover orchestration and review, planning and ingestion,
external propagation, repo adoption, introspection and feedback triage,
guarded sync reconciliation, research, testing, and closeout. The
documentation roles — `documenter` and `doc-author` — live in tabularium,
which owns the doc-system tool they drive. The specialist boundaries are
deliberate: `feedback-triager` and `sync-reconciler` coordinate preview-gated
changes, `research` is read-only, and planning-state writes still go through
the owning CLI binary. See
[`agents/methodology.md`](agents/methodology.md) for orchestration and
[`docs/skill-reference.md`](docs/skill-reference.md) for the role inventory.

Forty unified `pl-*` skill sources render for each selected vendor. In
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
| `src/engine/` | Domain logic and state transitions, bucketed as `identity/`, `planning/`, `external/`, `runtime/`, plus subsystem dirs (`extsync/`, `workbench/`, `templates/`, `routing/`, `config/`) |
| `src/lib/` | Shared base modules: `db/` (connection + migrations), `cliapp/` (CLI11-backed parser wrapper), `adapter/`, `http/`, `git/`, `process/`, `log/`, and the other leaf libraries |
| `src/tools/` | Project tooling, one directory per tool (`cli_usage_lint/`, `cli_docs_coverage/`, `surface_lint/`, `scriptorium/`, `centurion_client_proof/`) |
| `cmake/dependencies.cmake` | Every third-party dependency as a pinned `CPMAddPackage(...)` (release archive + SHA256, cached under `vendor/`) |
| `cmake/generate_migrations.cmake`, `cmake/generate_templates.cmake` | Configure-time codegen: `#embed`s `migrations/` and `templates/defaults/` into generated modules the runtime embeds |
| `vendor/` | CPM's committed source cache for third-party dependencies — pinned release archives only, no `git clone`/submodule vendoring |
| `external/` | CPM's source cache for first-party dependencies (Centurion) — pinned the same way but gitignored, populated by the first configure |
| `src/cmd/parity_harness.hpp` | The cross-process (black-box) test harness — `run_pinned()` execs a built binary over fixed argv in a scratch environment with its own `PLANAR_DB` and `HOME`. Cases live in `src/cmd/*/parity.t.cpp` and `src/cmd/planar/cross_process.t.cpp` |
| `migrations/` | SQLite migrations in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`) — single authoritative source |
| `templates/defaults/` | Propagation templates (JSON) for external systems (`github-issues/`, `jira/`); embedded at build time |
| `templates/doc-prompts/`, `templates/defaults/`, `templates/workspace-capabilities.toml` | Operator-editable defaults staged into `~/.planar/templates/` on install |
| `skills/src/` | Unified authored skill sources (`pl-*.md`) |
| `$PLANAR_HOME/commands/claude/` | Generated Claude staging tree (install output, not checked in) |
| `$PLANAR_HOME/codex-skills/` | Generated Codex staging tree (install output, not checked in) |
| `$PLANAR_HOME/copilot-skills/`, `$PLANAR_HOME/gemini-skills/` | Generated Copilot and Gemini staging trees (install output, not checked in) |
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
- **[docs/testing.md](docs/testing.md)** — test layers, the gates and what each proves, the black-box harness, and the rules for adding tests.
- **[examples/](examples/)** — copy-paste oriented examples for drafting specs, reviewing them, ingesting them, launching the orchestrator, authoring workflows, and propagating to external systems.
- **[agents/methodology.md](agents/methodology.md)** — how the orchestrator / coder / reviewer agents collaborate; the 5-phase orchestrator flow.
- **[CLAUDE.md](CLAUDE.md)** — agent guide for working in this repo (`AGENTS.md` is a symlink to it).

**Historical context.** Founding tech spec, roadmap, ADRs, and feature specs are preserved as Planar's own artifacts under `~/.planar/workbench/project_planar/p44-planar-founding-archive/` (accessible via `planar artifact list --plan 44` or `planar artifact show <id>`).

## License

MIT — see [LICENSE](LICENSE).
