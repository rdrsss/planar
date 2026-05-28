# Planar Architecture

Planar is a local-first task tracker and agent-operations infrastructure tool. It spans planning, tasking, scoping, durable agent handoff, vendor parity, and operational-plane integration with Jira and GitHub Issues.

This document describes the system as it stands today — for a new contributor or curious user who wants to understand how Planar works without reading the full source.

---

## System Layers

```
┌─────────────────────────────────────────────────────────────┐
│                    User / Agent Surface                     │
│  /orchestrator  /coder  /reviewer  /pl-spec-draft  ...     │
│  Skill files under commands/claude/, skills/codex/,         │
│  skills/copilot/ — installed to ~/.claude/commands/,        │
│  ~/.planar/codex-skills/, ~/.copilot/skills/                │
└────────────────────────┬────────────────────────────────────┘
                         │  invokes
┌────────────────────────▼────────────────────────────────────┐
│                   planar CLI binary                         │
│  Go binary, cobra-wired, ~25 subcommand domains             │
│  Built from src/ · installed to ~/.planar/bin/planar        │
└────────────────────────┬────────────────────────────────────┘
                         │  reads/writes
┌────────────────────────▼────────────────────────────────────┐
│                  SQLite database                            │
│  ~/.planar/planar.db · 21 application tables                │
│  Migrations: src/migrations/0001–0007                       │
│  Applied at startup via goose (library mode)                │
└─────────────────────────────────────────────────────────────┘
```

There are two layers that users and agents touch:

1. **The `planar` binary** — a Go CLI that owns all reads and writes to the SQLite database. No external process writes the database directly; the binary is the only supported access path for mutations.
2. **The skill and agent layer** — vendor-specific command surfaces (Claude slash commands, Codex skills, Copilot skills) that compose binary verbs. Most users invoke skills; skills invoke binary commands; binary commands operate on SQLite.

An LLM agent running a skill has no direct database access. It calls `planar` subcommands and reads their output.

---

## Storage

The database lives at `~/.planar/planar.db` by default. The `--db` global flag overrides the path.

### Schema management

Migrations are plain SQL files under `src/migrations/` with `-- +goose Up` / `-- +goose Down` markers. They are embedded into the binary at compile time via Go's `embed.FS` and applied at runtime by the `goose` library (not the CLI tool). The runtime calls `migrate.Apply` on every startup; already-applied migrations are skipped.

`schema_migrations` is the public schema-version contract. Every migration inserts one row with a version number and description. Read-side tools (e.g., a web viewer, an Obsidian bridge) must open the database read-only and query `schema_migrations` to verify they support the current version before operating.

### Application tables

| Migration | Tables |
|-----------|--------|
| 0001 foundation | `schema_migrations`, `config`, `projects`, `associations`, `project_associations` |
| 0002 planning | `plans`, `artifacts`, `decisions` |
| 0003 work items | `agents`, `active_scope`, `tasks`, `questions`, `test_scenarios`, `plan_steps` |
| 0004 entity links | `entity_links` |
| 0005 sessions | `sessions`, `session_entries`, `context_snapshots`, `handoffs` |
| 0006 external | `external_systems`, `external_links`, `sync_events` |
| 0007 workbench | `workbench_sync_state` |
| 0008 doc artifact kinds | (extends `artifacts.kind` CHECK with `research`, `getting_started`, `changelog_entry`, `glossary_term`) |
| 0015 agent activity (plan 85 M1) | `agent_work_claims`, `agent_actions` (each with locality columns: claims carry `repo_root`/`branch`/`head_sha_at_claim`/`dirty_at_claim` + optional worktree id/path; actions carry `head_sha`/`dirty`) |

Migration 0015 (`migrations/00015_agent_activity.up.sql`) lands the claim
+ action store that the agent-coordination feature is built on. `claim_token`
is generated in SQL via `lower(hex(randomblob(16)))` (32-char opaque
handle). Exclusivity of `(entity_kind, entity_id)` is enforced
transactionally in the engine store (`src/engine/runtime/agentactivity/`)
under `BEGIN IMMEDIATE` because SQLite cannot express the time-dependent
"unexpired" predicate in a partial unique index. WAL mode is enabled
per-connection in `src/runtime/runtime.zig` — load-bearing for the wake-
tier ladder behind `--follow` AND for cross-binary concurrency between
the operator and agent binaries (see Three-binary architecture below).

### Three-binary architecture (plan 85)

The agent-activity feature ships THREE binaries that share one schema,
one engine module, and one runtime library. The split is real: separate
`src/cmd/` source trees, separate `addExecutable` entries in
`build.zig`, separate `--help` surfaces, separate `bin/` artifacts
under `~/.planar/bin/`.

| Binary | Audience | Write surface | DB open mode |
|---|---|---|---|
| `planar` | Operator (human + scripts) | Planning entities (plans/tasks/decisions/etc.) + `tasks.status` on operator-driven transitions | Read-write; owns `init` and runs migrations. |
| `planar-agent` | Agent (vendor hook, orchestrator dispatch) + operator recovery | `agent_actions` + `agent_work_claims`; `tasks.status` ONLY as part of an atomic coordinated operation under a status-transition guard | Read-write; refuses startup with exit 7 if schema is older than the binary's embedded minimum. |
| `planar-watch` | Operator (live view) + scripts | None — the binary registers zero write verbs AND opens SQLite via `file:?mode=ro` URI as a second line of defense | Read-only; same schema-version handshake as `planar-agent`. |

Capability invariant — non-overlapping write surfaces enforced at
compile time by each binary's verb set, not by runtime ACLs:

- `planar` NEVER writes to `agent_actions` or `agent_work_claims`. The
  `planar agent` subcommand namespace does not exist; agent
  observability lives on `planar-watch`, agent-table mutation lives on
  `planar-agent`.
- `planar-agent` NEVER writes to plan / decision / question / scenario
  / artifact / annotation rows. A vendor hook configured with only
  `planar-agent` on its PATH has bounded blast radius — it cannot
  touch planning state.
- `planar-watch` is incapable of writing to the DB at all — both the
  verb set and the read-only DB handle are load-bearing.

The operator-recovery verbs `planar-agent reconcile` and
`planar-agent abort` live on `planar-agent` (not `planar`) because both
are `agent_*` table writers. The capability boundary tracks tables,
not audience.

Plan 85 M1 ships migration 0015 + the shared engine module
(`src/engine/runtime/agentactivity/`) + the `planar-agent` binary
scaffold (schema-version handshake + placeholder `version` verb). M2
adds the 13-verb `planar-agent` surface; M8 adds `planar-watch`; M9
upgrades `planar-watch <verb> --follow` to a Tier-2 event-driven wake
loop without changing the public contract.

### Live tail wake abstraction (plan 85 M9)

The `planar-watch <verb> --follow` family runs a poll loop: take an
initial snapshot, then re-query the watermark whenever new activity
might have landed. The TRANSPORT — how the loop knows when to wake
— is INTERNAL and evolves through a tier ladder. The public contract
(JSON event shape, watermark columns, `--interval` flag, SIGINT
exit-0) is preserved across tiers.

| Tier | Transport | Latency | Idle CPU | Status |
|---|---|---|---|---|
| 1 | Fixed-interval sleep (`--interval` cadence) | `--interval` floor (default 1s) | ~0 (sleeps) | Shipped in M8; remains the fallback. |
| 2 | kqueue (macOS/BSD) or inotify (Linux) on the SQLite `-wal` sibling | Sub-millisecond wake from any committed write | ~0 (kernel notification) | Shipped in M9. |
| 3 | Writer-side `update_hook` → sidecar notify socket | Same as Tier 2, plus per-row filtering | ~0 | Future. |

The abstraction lives at `src/engine/runtime/agentactivity/wake.zig`
behind a `Wake` struct with `init` / `waitNext` / `close`. The follow
loop in `src/cmd/planar-watch/handlers/follow.zig` calls
`Wake.waitNext(timeout_ns)` once per iteration; the wake source
returns `.wal_changed` when a kernel notification arrived, or
`.heartbeat` when the timeout elapsed without a notification. The
`--interval` flag is the HEARTBEAT cadence — a maximum fallback that
catches coalesced/missed wake events (laptop sleep, ENOMEM, etc.).

Backend selection is compile-time: macOS/BSD get kqueue with
`EVFILT_VNODE` (`NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME`)
on the `-wal` fd opened with `O_EVTONLY` (darwin's "watch but don't
hold a real reference" mode — required to avoid interfering with
SQLite's WAL coordination); Linux gets inotify with
`IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF`. Unsupported platforms
fall back to a plain timed sleep with a one-shot stderr warning.

WAL rotation (`PRAGMA wal_checkpoint(TRUNCATE)`, crash recovery) is
detected via `NOTE_DELETE`/`NOTE_RENAME` (kqueue) and
`IN_DELETE_SELF`/`IN_IGNORED` (inotify); the wake source re-opens
the watch transparently on the next `waitNext` call. Operators
never have to restart `planar-watch` after a checkpoint.

The wake transport is NOT part of the public contract — future
tiers (Tier 3 writer-side hook + sidecar; alternative IPC mechanisms)
can swap behind the same `Wake` interface without breaking
consumers.

### SQLite driver

Planar uses `modernc.org/sqlite` — a pure-Go SQLite port with no cgo dependency. There is no system SQLite requirement. Cross-compilation works without special toolchain setup.

---

## Internal Package Layout

All Go code lives under `src/internal/`. The packages are grouped into five entity buckets, a set of subsystem packages, utility packages, and orchestrator packages.

### Entity buckets

Each bucket maps to one domain of the data model and contains sub-packages per entity kind.

| Package | Contents |
|---------|----------|
| `src/internal/identity/` | Project, scope, association, and scope-argument parsing. The identity layer answers "who is asking and in what context." Sub-packages: `project/`, `scope/`, `association/`, `scopearg/`. |
| `src/internal/planning/` | Plans, tasks, questions, test scenarios, artifacts, and decisions. The core structured-intent and execution surfaces. Sub-packages: `plan/`, `task/`, `question/`, `scenario/`, `artifact/`, `decision/`. |
| `src/internal/external/` | External-system registration, external-link tracking, promote/demote surface. Sub-packages: `external/`, `promote/`. |
| `src/internal/runtime/` | Sessions, session entries, context snapshots, and handoffs. Durable agent-execution records. Sub-packages: `session/`, `snapshot/`, `handoff/`. |
| `src/internal/health/` | Database and handoff readiness checks. Sub-packages: `health/`, `render/`. |

### Subsystem packages

Packages that implement cross-cutting behavior or system-level features.

| Package | Role |
|---------|------|
| `src/internal/workbench/` | Bidirectional sync between the workbench filesystem and the database. Implements pull, push, sync, resolve, archive, restore, publish, and manifest management. |
| `src/internal/extsync/` | Ext-sync agent implementation: strategy selection (ADR-0006 logic), Jira propagation, GitHub propagation (parent-issue, Projects v2, zero-repo fallback), payload construction, and reporting. |
| `src/internal/templates/` | Template rendering for external-system payloads. Resolves templates through a three-level fallback chain (user set → default set → embedded defaults). |
| `src/internal/adapter/` | The `OperationalAdapter` interface and the data types that cross the adapter boundary. Jira and GitHub Issues implementations live here, behind the interface. |
| `src/internal/ingestor/` | Planning-document parser and ingestion engine. Reads `tech-spec.md` and `roadmap.md` from the workbench, computes a diff against the current DB state, and optionally applies it. |
| `src/internal/adopter/` | Transcription pipeline for `import`: classifier, parser, status-inference, diff, and apply stages. The apply layer is shared with `synthesize`. |
| `src/internal/bootstrap/` | Synthesis pipeline for `synthesize`: `codeprobe/` (per-FeatureArea EvidenceMap) and `synthesis/` (Request/Result, fingerprint cache, Validate, Merge). The LLM runs in the vendor skill; this package owns the deterministic floor and the skill-handoff cache contract. |
| `src/internal/tree/` | Hierarchical rendering for `planar tree` — walks the plan/task/artifact/decision/scenario/question graph and produces the indented tree output. |
| `src/internal/config/` | Configuration-plane reader: loads `~/.planar/config.toml`, applies the layered resolution order, validates, and exposes the resolved config to other packages. |
| `src/internal/syncengine/` | Operational-plane sync engine: drives pull (remote → local) and push (local → remote) cycles, records `sync_events`, detects conflicts. |

### Utility packages

| Package | Role |
|---------|------|
| `src/internal/db/` | `*sql.DB` factory: opens the SQLite file, sets PRAGMAs (foreign keys on, WAL mode), and returns the connection used by all other packages. |
| `src/internal/output/` | Output formatting helpers: human-text line formatting, JSON emission, scope display. |
| `src/internal/migrate/` | Thin wrapper around the goose library; embeds `src/migrations/` via `embed.FS` and applies them. |
| `src/internal/cperr/` | Structured error types for user-fixable vs. system errors; maps to exit codes 1/2/3/64. |
| `src/internal/entitylink/` | Cross-entity relationship management: creates, lists, and removes `entity_links` rows with typed relationships (`derives-from`, `blocks`, `addresses`, `verifies`, `cites`, `supersedes`, `touches`). |

### Orchestrator packages

| Package | Role |
|---------|------|
| `src/internal/resume/` | Resume-packet construction: assembles the structured context packet that a new agent session uses to resume a task from zero conversational context. |
| `src/internal/lifecycle/` | Lifecycle integration tests and state-machine helpers for plan and task status transitions. |

---

## CLI Binary

The binary is a cobra-wired Go application with a thin `main` in `src/cmd/planar/`. Each subcommand domain maps to one entity kind or system surface. Cobra handles flag parsing, help generation, and command routing.

### Command constructor layout

Command constructors are organized as seven subpackages under `src/cmd/planar/internal/` (Go's `internal/` path restriction ensures they are importable only from within `cmd/planar/`):

| Subpackage | Subcommands |
|------------|-------------|
| `cli/` | Shared helpers: `GlobalFlags`, `ChainPreRun`, `LogLevelPreRun`, `ResolveTemplatesRoot`, `ParseID`, `ResolveScope`, `ScopeDisplay`, `InitTestDB` |
| `identity/` | `init`, `scope`, `assoc`, `promote`, `demote` |
| `planning/` | `plan`, `task`, `artifact`, `question`, `scenario`, `decision`, `spec`, `templates` |
| `external/` | `ext`, `link`, `links`, `unlink`, `sync` |
| `runtime/` | `handoff`, `resume`, `capture`, `audit`, `session` |
| `workbench/` | `workbench`, `workspace` |
| `system/` | `health`, `config`, `tree` |

`main.go` calls each bucket's `AddCommands(root)` function. The `runtime` bucket name collides with Go's stdlib `runtime` package; `main.go` imports it as `cmdruntime`. `parity_test.go` stays at the top of `cmd/planar/` because it tests the compiled binary, not package internals.

### Subcommand domains (~25)

`artifact`, `assoc`, `audit`, `capture`, `config`, `decision`, `demote`, `ext`, `handoff`, `health`, `help`, `init`, `link`, `links`, `plan`, `promote`, `question`, `resume`, `scenario`, `scope`, `spec`, `sync`, `task`, `templates`, `tree`, `unlink`, `workbench`

Use `planar --help` for the current list. Use `planar <domain> --help` for subcommand detail.

### Global flags

| Flag | Default | Purpose |
|------|---------|---------|
| `--db <path>` | `~/.planar/planar.db` | Database path override |
| `--json` | off | Emit newline-delimited JSON instead of human text |
| `-q` / `--quiet` | off | Suppress informational output |
| `--v` | off | Debug-level tracing |
| `--vv` | off | Trace-level tracing |

### Output conventions

- Human mode (default): line-oriented text.
- Machine mode (`--json`): one JSON object per result row, snake_case keys matching schema column names.
- Mutating commands that produce no entity data emit `{"ok":true,"id":<id>}` under `--json`.
- Exit codes: `0` success, `1` user-fixable error, `2` system error, `3` sync conflict, `64` usage error.

---

## The Workbench

The workbench is a bidirectionally synced drafting filesystem at `$PLANAR_WORKBENCH_ROOT` (default `~/.planar/workbench/`). It is the primary surface where agents and users read and write planning documents.

Each anchor plan gets its own directory:

```
~/.planar/workbench/
  <assoc-slug>/
    p<plan-id>-<plan-slug>/
      product-spec.md      ← artifact (kind=product_spec)
      tech-spec.md         ← artifact (kind=tech_spec)
      roadmap.md           ← artifact (kind=roadmap)
      tasks/
        <id>-<slug>.md     ← task
      scenarios/
        <id>-<slug>.md     ← test_scenario
      decisions/
        <id>-<slug>.md     ← decision
      ...
```

Sync is explicit, not automatic:

- `planar workbench push <plan>` — DB → FS: write the current DB state to disk.
- `planar workbench pull <plan>` — FS → DB: read disk changes into DB.
- `planar workbench sync <plan>` — bidirectional: apply FS→DB and DB→FS in one pass; surface conflicts as `sync_events(outcome='conflict')` rows.
- `planar workbench status [<plan>]` — show FS-only, DB-only, and conflicting files without writing.
- `planar workbench resolve <event-id> --prefer fs|db` — settle a conflict.

`workbench_sync_state` tracks the per-file relationship (entity kind, entity id, content hash, last sync time) so the sync engine can detect changes without re-reading every file on every run.

When a feature is complete, `planar workbench archive <plan>` removes the on-disk tree. The database retains every entity row. `planar workbench restore <plan>` recreates the tree byte-identically from the DB.

`planar workbench publish <plan> --system <slug>` renders the workbench files for a plan and pushes the rendered content to a registered external operational system via the adapter layer. For full plan-subtree counterpart creation in an external system, `planar ext propagate <plan> --system <slug>` is the verb of record.

---

## Workspace State Directory Model

A workspace is an `associations` row of `kind=org` together with its `project_associations` members — a polyrepo grouping. Each workspace owns a state directory under `~/.planar/` that holds the canonical AGENTS.md surface and the structured routing table that drives it. There is no `workspaces` table; the state directory path is derived from the org's id and the convention is the contract.

### Layout

```
~/.planar/workspaces/<org_id>/
├── AGENTS.md                       # canonical, generated by workspace regenerate
├── routing-table.json              # structured project map, generated by routing build
├── config.toml                     # optional per-workspace settings (enrich_command, etc.)
├── routing-table-overrides.json    # optional manual overrides merged on every build
└── .manifest-docs                  # plan-96 drift manifest tracking the generated files
```

The state directory lives alongside the rest of Planar's state under `~/.planar/`: the database (`planar.db`), the workbench tree (`workbench/`), the templates layer (`templates/`), and the enrichment cache (`cache/workspace-enrichment/<org_id>/`). Keeping everything under one root makes backup, sync, and clean-slate operations a single-path operation.

### Symlink lifecycle at the workspace root

Two symlinks at the workspace root (the cwd of `planar workspace init` — typically `~/work/`) project the canonical content into the directory where agents naturally look:

```
<workspace-root>/AGENTS.md  →  ~/.planar/workspaces/<org_id>/AGENTS.md
<workspace-root>/CLAUDE.md  →  ~/.planar/workspaces/<org_id>/AGENTS.md
```

Both names point at the same target so Codex / Copilot (which read `AGENTS.md`) and Claude Code (which reads `CLAUDE.md`) see the same content. On filesystems that reject symlinks (Windows without developer-mode enabled), the installer falls back to a regular-file copy and records the degraded mode in the routing table so subsequent regenerations rewrite the copy. `planar workspace doctor` re-creates either form when it is missing or pointing at the wrong target; the operation is idempotent.

### Bare-init guardrail

`planar init` refuses when cwd has no `.git` of its own but contains one or more immediate child directories that do. Without the guardrail, a bare init in `~/work/` would register a semantically-wrong project row (`~/work/` is not a repo) and quietly skip the workspace-shaped intent the user clearly had. The refusal points at `planar workspace init` instead. The `--allow-no-repo` (alias `--force`) escape hatch exists for the rare standalone non-repo project; routine workflows should not need it. See [cli-reference.md § `planar init`](cli-reference.md#planar-init) and [concepts.md § Workspace](concepts.md#workspace).

---

## The Configuration Plane

Configuration lives in `~/.planar/config.toml`. The `planar config` domain manages it.

### Resolution order (highest wins)

1. Environment variables (`PLANAR_*` prefix).
2. Per-association overrides in `config.toml` under `[assoc.<slug>]`.
3. Top-level keys in `config.toml`.
4. Embedded defaults compiled into the binary.

### Config subcommands

| Command | Purpose |
|---------|---------|
| `planar config show` | Print the fully-resolved configuration as TOML. |
| `planar config validate` | Check `config.toml` for syntax and semantic errors. |
| `planar config edit` | Open `config.toml` in `$EDITOR`. |
| `planar config init` | Write a starter `config.toml` with documented defaults. |
| `planar config path` | Print the path to the active config file. |

Notable config keys: `github_lead_repo` (used by the GitHub zero-repo propagation strategy), `jira_base_url`, `jira_project_key`, freshness windows for sync, template set selection.

---

## The Templates Layer

Templates live under `~/.planar/templates/` and drive external-system payload rendering for propagation (`ext propagate`) and ext-sync.

The resolution chain for any template file:

1. The user-chosen template set (configured in `config.toml`).
2. The `default` set under `~/.planar/templates/default/`.
3. Embedded binary defaults (compiled into the binary from `src/internal/templates/defaults/`).

Templates use Go's `text/template` against a rendering context that exposes `.Task`, `.Plan`, `.Feature`, `.Scenario`, `.Touches`, `.Assoc`, `.ExternalKey`, and `.Children`.

`planar templates list` shows all available templates and their source level. `planar templates validate` checks them for syntax errors. `planar templates render <entity>` renders a template against a live entity for inspection.

---

## Operational Plane Adapters

The operational plane adapters connect Planar to external issue trackers. The adapter boundary is the `OperationalAdapter` interface in `src/internal/adapter/`:

```go
type OperationalAdapter interface {
    Fetch(ctx, externalID) (RemoteState, error)
    Create(ctx, local, opts) (CreatedEntity, error)
    Update(ctx, externalID, fields) (UpdateOutcome, error)
    Comment(ctx, externalID, body) (CommentID, error)
    Search(ctx, query) ([]string, error)
}
```

Two adapters are implemented: **Jira** (`src/internal/adapter/jira.go`) and **GitHub Issues** (`src/internal/adapter/github.go`). Both use stdlib `net/http` with a 30-second timeout. The sync engine and the ext-sync agent call the interface; per-vendor logic stays behind it.

### Strategy selection

For GitHub Issues, the propagation strategy is selected once at first propagation per feature and cached on `external_links.config_json` of the anchor plan:

| Condition | Strategy |
|-----------|---------|
| 0 repos touched by descendant tasks | `github-zero-repo` — parent issue in `github_lead_repo` |
| 1 repo touched | `github-parent-issue` — parent issue in the touched repo |
| 2+ repos touched | `github-projects-v2` — GitHub Projects v2, issues in their respective repos |

The strategy is sticky: subsequent re-propagations use the cached value. `--restrategize` forces fresh detection.

For Jira, the strategy is always the epic hierarchy: anchor plan → Epic, child plans → Stories, tasks → Sub-tasks.

### The ext-sync agent

Beyond propagation, the ext-sync agent (`src/internal/extsync/`) handles bidirectional sync: pulling remote state changes into `sync_events` and pushing local mutations to the remote. The sync engine (`src/internal/syncengine/`) orchestrates the pull/push cycle; adapters handle the per-system translation.

---

## Repo Onboarding Pipelines

Planar onboards an existing repository into the data model via two sibling verbs that share the same downstream Apply machinery but enter from different contracts.

### Transcription pipeline (`import`)

The `import` verb is a translator: it reads the repo's existing planning docs and emits them as Planar artifacts as-is. The pipeline lives in `src/internal/adopter/`:

```
adopter.Discover  → walk repo, classify .md files (frontmatter → filename → path)
adopter.Parse     → extract roadmap milestones, ADR decisions, deferred items
adopter.Infer     → status correlation from git log + branch list + checkbox state
adopter.Diff      → match against DB by fingerprint; emit additions / updates / removals
adopter.Apply     → commit additions, updates, and (with --apply-removals) soft-cancels
```

The optional `--interpret` pass writes a fingerprinted Request to `$PLANAR_HOME/cache/import-interpretation/<repo-slug>/_pending.json` and exits 0. The vendor skill produces a Result; the next invocation merges it with the deterministic Corpus before reaching the Diff/Apply stages.

### Synthesis pipeline (`synthesize`)

The `synthesize` verb is a generator: it reads docs *and* source, then produces fresh planning artifacts via an LLM pass. The pipeline lives in `src/internal/bootstrap/`:

```
bootstrap/codeprobe.Probe   → per-FeatureArea source / test / CI / commit signals
                              with SignalStrength scores; output is an EvidenceMap
adopter.Discover + Parse    → existing docs become Request context (not source-of-truth)
synthesis.Request           → fingerprinted package: docs + EvidenceMap + greenfield flag
                              written to $PLANAR_HOME/cache/bootstrap-synthesis/<slug>/_pending.json
[vendor skill runs LLM]     → reads Request, writes synthesis.Result to
                              <cache-dir>/<fingerprint>.json
synthesis.Validate          → hard-rejects Results that violate the code-evidence invariant
synthesis.Merge             → adapts Result to interpretation.Result, merges with deterministic baseline
adopter.Diff + Apply        → SHARED with import — same Diff/Apply stages
```

The LLM never runs in Go. The Go binary stays free of provider API keys, retries, and rate limits; the vendor skill (`commands/claude/pl-synthesize.md` etc.) is the LLM engine. The cache contract is the handoff: Go writes a Request, the skill writes a Result, Go validates and merges.

### Merge rules (synthesis)

`synthesis.Merge` honors four rules when combining the LLM Result with the deterministic baseline:

1. **Deterministic kind wins.** Frontmatter / filename / path classification of existing docs is locked; the LLM cannot reclassify them.
2. **LLM fills the qualitative output.** Phase decomposition, rich task bodies, status claims, decisions, deferred items, and forward specs all come from the LLM.
3. **Code-evidence outranks LLM-claimed-done.** A task with `status != "todo"` must cite a `code_evidence` path that exists in the deterministic EvidenceMap. The validator rejects any Result that violates the invariant; the LLM cannot lie about completion.
4. **Floor refusals propagate.** A deterministic confidence-floor refusal (50% threshold) and a greenfield invariant (no `status != "todo"` when codeprobe reports zero source evidence) cannot be rescued by good LLM data.

### Shared Apply machinery

The Diff/Apply stages (`src/internal/adopter/diff.go`, `src/internal/adopter/apply.go`) are shared between both verbs. After the synthesis-specific merge or the import-specific interpretation merge, both pipelines converge on the same idempotent diff (match by fingerprint; additions / updates / proposed-removals) and the same apply path (soft-cancel removed entities; preserve the audit trail).

The synthesis-vs-transcription split serves the same downstream pipeline: both verbs produce artifacts that flow through `/pl-spec-ingest` for task decomposition, then through the orchestrator's execution + propagation phases. The split is at the entry point only — what counts as the authoritative planning material.

See [docs/concepts.md § Transcription vs Synthesis](concepts.md#transcription-vs-synthesis) for the conceptual framing and [docs/cli-reference.md § Domain: synthesize](cli-reference.md#domain-synthesize) for the full CLI surface.

---

## The Agent Methodology

Planar defines vendor-neutral agent roles under `agents/`. Per-vendor command surfaces (Claude, Codex, Copilot) inherit the role spec and add vendor-specific invocation details.

### Roles

| Agent | Tier | Responsibility |
|-------|------|---------------|
| `orchestrator` | large | Receives a goal or task list; manages the full feature lifecycle across up to five phases; dispatches to coders; routes output through reviewers; enforces the iteration cap. |
| `coder` | medium | Implements one task (or task group) end-to-end; receives reviewer feedback and addresses it in the next iteration. |
| `reviewer` | large | Reviews coder output; returns `approve`, `request-changes`, `open-question`, or `abort`. |
| `planner` | large | Drafts planning documents (product spec, tech spec, roadmap) from a goal statement and registers them as workbench artifacts. |
| `ingestor` | large | Reads planning documents from the workbench and decomposes them into plans, tasks, decisions, and scenarios in the database. |
| `ext-sync` | large | Propagates the feature tree to the operational plane and syncs changes bidirectionally. |
| `importer` | large | Translates an existing repository's planning artefacts (specs, ADRs, roadmaps, backlog files, GitHub issues) into Planar's data model without a goal statement. |
| `synthesizer` | large | Produces fresh planning artifacts for a repo from existing docs + git log + source code via an LLM pass. Sibling of `importer`; shares the Apply machinery but enters from a synthesis contract (code-evidence invariant) rather than transcription. |

### Phases (orchestrator)

| Phase | Skill | Trigger |
|-------|-------|---------|
| 1 — Planning | `pl-spec-draft` | Goal given; no anchor plan or draft with no artifacts |
| 2 — Ingestion | `pl-spec-ingest` | Anchor plan draft with workbench artifacts present |
| 3 — Execution | coder + reviewer | Anchor plan active with todo/doing tasks |
| 4 — Propagation | `pl-ext-propagate` | User requests `--propagate` |
| 5 — Archive | `pl-workbench-archive` | Anchor plan done, user requests `--archive` |

The orchestrator gates Phases 2 and 3 on explicit user confirmation. Ingestion never auto-applies. The iteration cap is 5 per dispatch cycle.

### Vendor surfaces

| Vendor | Source dir | Installed to |
|--------|-----------|-------------|
| Claude | `commands/claude/` | `~/.claude/commands/` |
| Codex | `skills/codex/` | `~/.planar/codex-skills/` runtime, installed into `~/.codex/skills/` |
| Copilot | `skills/copilot/` | `~/.copilot/skills/` |

Agent role specs (vendor-neutral) live under `agents/`. The key files are `agents/methodology.md`, `agents/orchestrator.md`, `agents/planner.md`, `agents/ingestor.md`, `agents/extsync.md`, `agents/importer.md`, `agents/synthesizer.md`, `agents/coder.md`, `agents/reviewer.md`, and `agents/models.md` (tier-to-model resolution).

---

## Build and Test

The Go module root is `src/`. Build from the repo root via the Makefile or directly with `go -C src`.

```bash
# From repo root
make build              # builds ./bin/planar
make test               # unit tests
make test-integration   # builds the binary, sets PLANAR_BIN, runs CLI suites
make test-all           # unit + integration tests

# From src/ directly
go build ./...
go test ./...
go test -tags integration ./integration_tests/...
```

Planar runs a two-tier test model:

- **Unit tests** — `*_test.go` files colocated with the code under test under `src/internal/<pkg>/`. They import the package under test (plus `internal/migrate` when they need a DB) and run under `make test`. No build tag.
- **CLI integration tests** — `src/integration_tests/` is the only directory carrying the `//go:build integration` tag. Each test execs the compiled `planar` binary via the Suite harness (`harness_test.go`) against a fresh per-test SQLite database; nothing under `internal/` is imported. These suites lock the user-visible contract — flag names, JSON shapes, exit codes, status-transition rules. Always invoke them via `make test-integration` so `PLANAR_BIN` points at the freshly-built `./bin/planar` rather than falling back to per-call `go run`.

The binary produced by `make build` lands at `./bin/planar`. The installed binary (used by skills) is at `~/.planar/bin/planar`, built and staged by `install.sh`.

---

## Key Invariants

- **One `*sql.DB` per process.** Passed through a small `Store` struct; no global mutable state.
- **Migrations are append-only.** Never edit a released migration. Add a new file with the next sequence number.
- **The adapter boundary is clean.** The sync engine and ext-sync packages import `internal/adapter` for the interface; they do not import `jira.go` or `github.go` directly.
- **No cgo.** `modernc.org/sqlite` is a pure-Go port. `unsafe` is also forbidden.
- **Skills call the binary.** Agent skills do not write the database directly. They invoke `planar` subcommands and read stdout.
- **Schema is the contract.** Read-side tools must check `schema_migrations.version` before operating against the database.
