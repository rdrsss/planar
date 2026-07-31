# Planar Agent Guide

## Identity

- **Project:** Planar
- **Purpose:** Local agent-operations infrastructure spanning planning, tasking, scoping, durable agent handoff, vendor parity, and operational plane integration with Jira and GitHub Issues.
- **Stack:** Zig CLI binary plus a SQLite database (vendored SQLite amalgamation under `vendor/sqlite/`, compiled as a static library by `build.zig`, no system library dependency). Build via `zig build` from the repo root. Migrations live under `migrations/` in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`); build-time codegen (`tools/gen_migrations.zig`) reads them and emits a `migrations` Zig module that the runtime embeds and applies on startup. `schema_migrations` is the public schema-version contract. Propagation templates for external systems live under `templates/defaults/` and are read by a second codegen pass (`tools/gen_templates.zig`). CLI parsing is a hand-rolled comptime command tree vendored under `vendor/etcli/src/cli/` (cmd.zig, parser.zig, flag.zig, help.zig, completion.zig, validate.zig). HTTP via Zig's `std.http`. Logging via `std.log`. The workbench is a bidirectionally synced drafting filesystem under `$PLANAR_WORKBENCH_ROOT` (default `~/.planar/workbench/`). See [docs/architecture.md](docs/architecture.md) for the system overview.
- **Repo shape:** Feature-complete, ported from the original Go implementation to Zig (M1–M19 closed; archive remains at github.com:rdrsss/planar-go-archive.git). Five binaries (`planar`, `planar-agent`, `planar-watch`, `tabularium`, `planar-execute`), schema versioned across thirty-one migration files (`migrations/00001_foundation.up.sql` through `00031_dispatch_confirmation_tokens.up.sql`). (`planar-execute` is the deterministic, spawn-free Lua workflow engine revived in plan 633: an LLM caller invokes it to run a deterministic workflow — shelling `planar`/git/fs/control-flow — and collect results. It exposes NO model-spawning host function and holds no SQLite handle (it reaches state only by shelling `planar`). It is **not** a harness. An earlier `planar-execute` had grown re-entrant headless LLM spawning and became a harness in its own right, which is why it was extracted to a separate external project — the revival deliberately reigns that scope back in.) Full capability set: planning loop (spec drafting → task decomposition → execution → propagation), bidirectional workbench sync, configuration plane (`~/.planar/config.toml`), templates layer (`~/.planar/templates/`), three agent roles driving the feature lifecycle (planner, ingestor, ext-sync), and operational-plane adapters for Jira and GitHub Issues. 1,700+ unit tests, 570+ integration tests; all clean.
- **Binary name:** `planar`.

## Operating Rules

- Treat this file as this repo's true agent guide, not a template for other repos.
- Keep `AGENTS.md` equivalent to this file (symlink or byte-for-byte copy).
- Put installable Planar agents under top-level `agents/`. Do not scaffold them under vendor dot directories.
- Put unified skill source files under `skills/src/`. Do not author generated vendor surfaces directly.
- The per-vendor skill surfaces (`commands/claude/`, `skills/codex/`, `skills/copilot/`) are **not checked into the repo** — they are rendered at install time by scriptorium, invoked by `install.sh` after the binary is built (this replaced the retired in-tree renderer verb at plan 918 M5; see `scriptorium.yaml`). Source-of-truth lives under `skills/src/` only. `.gitignore` blocks the rendered dirs from re-entering the tree. The Tier Table for agent-role model routing is owned and hand-maintained by the armarium orchestration layer; Planar does not generate it.
- Copilot instructions/prompts stay under `copilot/`. Do not scaffold under `.copilot/` or `.github/`.
- Operator-machine-local skills and agents go under `~/.planar/local/{skills,agents}/`, not in the repo. They are user-machine-local state, never committed, and are linked into each vendor's surface via `planar local link` (which prefixes installs with `local-` to make them visibly user-authored). Promotion from sandbox to canonical is manual — copy the file into the repo and follow the normal contribution flow. There is no `planar local promote` shortcut. See `docs/workflows.md § Recipe 14` for the end-to-end walk-through and `docs/skill-reference.md § Personal sandbox` for the canonical-vs-sandbox boundary.
- User-facing reference docs (architecture, CLI reference, skill reference, concepts, workflows) live under `docs/` in the repo. Project-internal planning artifacts (tech specs, roadmaps, ADRs about Planar's own development) live as Planar artifacts under `~/.planar/`, accessible via `planar artifact` and the workbench. Operational state belongs in SQLite.
- Internal vs published docs. Project-internal planning artifacts (tech specs, roadmaps, decisions, ADRs, sessions) live as Planar artifacts under `~/.planar/` and the workbench, accessible via `planar artifact` and the workbench export. User-facing reference docs (architecture, CLI reference, skill reference, concepts, workflows, features, research, glossary, changelog) live under `docs/` in the repo. Published-doc coverage, drift detection, and linting are owned by the standalone `tabularium` tool and its machine-local database; Planar no longer ships a documentation binary. Don't blur the boundary: a tech-spec is internal authoring; a feature catalog entry is published prose derived from one or more tech-specs.
- Treat the `planar` CLI as the only supported access layer for workflows. Skills, agents, commands, and prompts must use current CLI commands and must not invent direct DB writes or repo-local context scaffolding.
- **Four-binary boundary and claim ritual.** Planar ships four binaries. Three are planning-state binaries with disjoint write surfaces over the shared SQLite DB: `planar` (operator — planning entities + `tasks.status` manual transitions), `planar-agent` (agent-callable — atomic writes to `agent_actions`, `agent_work_claims`, the `routing_dispatch_*` authorization tables, and `tasks.status` only as part of coordinated operations), and `planar-watch` (read-only viewer; opens SQLite via `file:?mode=ro`). The fourth, `planar-execute`, is the deterministic, spawn-free Lua workflow engine (plan 633): an LLM caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over allowlisted host functions (`cli`/`git`/`fs`/`flow`/`ctx`), and collects the JSON result. It holds **no SQLite handle** — it reaches Planar state only by shelling `planar`/`planar-agent`. It is **deliberately OUTSIDE the claim ritual** below: it is a workflow engine the caller invokes, not an agent-table writer, and exposes NO model-spawning host function (an earlier `planar-execute` grew re-entrant headless LLM spawning and became a harness, which is why it was extracted to a separate external project; the revival reigns that back in). The capability boundary is each binary's verb set, not runtime ACLs; the three planning-state binaries' boundaries are locked by `integration_tests/capability_boundary_test.zig`. There is no `planar agent <verb>` subcommand namespace — agent observability lives on `planar-watch`, agent-table writes live on `planar-agent`. Every code-writing agent dispatch (orchestrator → coder, hand-picked task dispatch, vendor-hook execution) MUST follow the canonical ritual: `planar-agent pull <plan-id>` (or `claim --entity task:<id>` for hand-picked) → `planar-agent heartbeat --claim <token>` at TTL/2 cadence → exactly one terminal verb of `planar-agent complete | fail | release | block`. The atomic terminal verbs flip claim status and `tasks.status` in one transaction; agents and skills MUST NOT split this into `planar task done` + `planar-agent release` (the intervening process death leaves the claim stranded). For orchestrator dispatch the orchestrator owns the terminal verb; coders heartbeat and return. For barrel-bypass and direct-claim dispatch the caller invokes the terminal verb. See the coordination-claims contract (owned by the armarium orchestration layer) and `docs/concepts.md § Binaries` for the full sequence and the capability invariants.
- Workspace `AGENTS.md` and `CLAUDE.md` at a polyrepo workspace root are symlinks (or, in degraded mode, copies) to the canonical generated file under `~/.planar/workspaces/<org_id>/`. Do not hand-edit them; they are regenerated by `planar workspace regenerate`. Operator overrides belong in `routing-table-overrides.json` next to the canonical target.
- Write verbs use the strict scope resolver: skills, agents, commands, and prompts must either run from a cwd inside the target project (relying on cwd derivation) or pass `--scope` explicitly. `--no-scope-check` is a legacy escape hatch and must not appear in routine workflow examples or new skill code. See `docs/concepts.md#scope` for the resolution algorithm.
- Every entity-targeted mutation verb verifies operator-vs-entity scope agreement before writing; cross-scope writes are explicit-only via `--scope <slug>` or `--no-scope-check`. Link verbs (`*_link`, `task touches add/remove`, `links add/remove`) are deliberately unguarded — they create entity_links edges that may legitimately cross scopes (the polyrepo touches/derives-from workflow). Read `docs/concepts.md#cross-scope-guard` for the full guarded/unguarded matrix.
- Use `planar import` to import an existing repo's planning content; it supports an optional `--interpret` LLM pass. The binary verb was trimmed from `pl-import` to `import` in plan 85; the slash command stays `/pl-import` because the `pl-` prefix namespaces it inside the vendor command tree.
- The data model is the contract. Schema changes flow through versioned migrations starting at `migrations/00001_foundation.up.sql`. Other binaries (read-side viewers, web servers, Obsidian bridges) must open the database read-only and verify schema version before operating.
- Architecture changes must update `docs/architecture.md` (and other affected reference docs) in the same change. The schema migration is the primary contract; docs are the human-readable annotation of it.
- External tool dependencies are tracked in two places that MUST stay in sync: `README.md` § Prerequisites (the human-readable inventory) and the `BUILD_DEPS` / `RUN_DEPS` manifests in `install.sh` (the machine-checked list the installer preflights). Whenever the binary, a bundled skill/agent, or the installer starts shelling out to a new program — or stops needing one — update both in the same change. `install.sh` fails fast on a missing build-tier tool and warns on a missing runtime-tier tool; an un-manifested dependency silently breaks for users who lack it.
- **Vendoring rule: vendored third-party dependencies MUST be sourced from a pinned release archive, not a git checkout.** Every entry in `vendor/<name>/` is declared in `vendor/manifest.zon` with a `url` (a versioned release archive — `.zip` / `.tar.gz`) and a `sha256` pin, materialized by `zig build vendor-sync`, and verified fail-fast at configure time (a drifted `vendor/<name>/VENDOR.toml` aborts the build). Do **not** vendor by `git clone`/submodule. The only sanctioned exception is a dependency that publishes **no** release archive at all; such an exception must be called out explicitly in that dep's `build.zig.zon` comment and `manifest`/`VENDOR.toml`, and is a last resort — prefer asking upstream to cut a tagged release. (For GitHub sources, pin the direct `codeload.github.com/.../tar.gz/refs/tags/<tag>` URL: the friendlier `…/archive/refs/tags/<tag>.tar.gz` form 302-redirects, which `vendor_sync`'s fetch does not follow.)

## Source Layout

The repo root IS the Zig package root: `build.zig` and `build.zig.zon` sit at the top level alongside the modules dir (`src/`), build-time codegen (`tools/`), the integration suite (`integration_tests/`), and the vendored SQLite amalgamation (`vendor/sqlite/`). Workflow surfaces, docs, templates, and bash tooling live as sibling top-level directories.

| Path | Role |
|------|------|
| `build.zig` | Zig build configuration. Compiles the vendored SQLite amalgamation as a static library, runs migrations + templates codegen against `migrations/` and `templates/defaults/`, and links the resulting modules into the `planar` executable. Declares `run`, `test`, and `test-integration` build steps. |
| `build.zig.zon` | Zig package manifest (name `planar`, version, fingerprint, minimum Zig version `0.16.0`, `.paths` listing the in-package directories). |
| `src/` | Zig modules — the runtime source tree. Imported by `build.zig` as `db`, `cli`, `engine`, `planar`, etc. |
| `src/cmd/planar/` | Executable entry point — `main.zig` plus runtime scaffolding (`editflow.zig`, `editor.zig`, `exit.zig`, `output.zig`, `runtime.zig`, `scope.zig`) and per-verb handlers under `handlers/`. |
| `vendor/etcli/` | Vendored CLI parser + help/completion renderer ([etcli](https://github.com/rdrsss/etcli) extracted from the former in-tree `src/cli/`). Declared as a path dependency in `build.zig.zon`; `build.zig` consumes its `cli` module via `b.dependency("etcli", ...).module("cli")` and every binary imports it as `cli`. Vendored in-tree from the **v0.2.0 release archive** (pinned by `sha256` in `vendor/manifest.zon`); re-vendor or bump via `zig build vendor-sync`. The `VENDOR.toml` is auto-generated — do not hand-edit. |
| `src/db/` | Database layer — `db.zig` (connection + transaction wrappers), `migrate.zig` (migration application against the embedded `migrations` module), `sqlite.zig` (C-API bindings against the vendored amalgamation). |
| `src/engine/` | Domain engine organized as bucket directories: `identity/`, `planning/`, `external/`, `runtime/`, plus subsystem modules (`config.zig`, `docs.zig`, `entitylink.zig`, `extsync.zig`, `health.zig`, `import.zig`, `ingestor.zig`, `init.zig`, `llm.zig`, `local.zig`, `policy.zig`, `promotion.zig`, `search.zig`) at top level. |
| `src/root.zig` | Package root (`pub` surface). |
| `tools/gen_migrations.zig` | Build-time codegen: scans `migrations/` and emits a `migrations` Zig module exposing `pub const all: []const Migration` for the runtime to apply. |
| `tools/gen_templates.zig` | Build-time codegen for embedded propagation templates (reads `templates/defaults/`). |
| `vendor/sqlite/` | Vendored SQLite amalgamation (`sqlite3.c`, `sqlite3.h`). Compiled into a static library by `build.zig` with `SQLITE_THREADSAFE=1`, `SQLITE_ENABLE_FTS5`, `SQLITE_ENABLE_JSON1`, `SQLITE_DQS=0`, `SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_USE_URI=1`. No external wrapper. |
| `integration_tests/` | End-to-end integration suites exercising the built binary via the `harness.zig` runner (`harness.smoke`, `harness.mustRun`, `harness.mustRunJSON`). Run via `zig build test-integration`. |
| `migrations/` | SQLite schema migrations in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`, 5-digit zero-padded prefix). Authoritative source — the Zig build picks them up automatically via `tools/gen_migrations.zig` codegen. See `migrations/README.md` for the file format and `schema_migrations` contract. |
| `templates/defaults/` | Propagation templates (JSON) for external operational systems (`github-issues/`, `github-projects/`, `jira/`). Embedded into the binary at build time via `tools/gen_templates.zig`; operator overrides land in `~/.planar/templates/defaults/`. |
| `templates/doc-prompts/`, `templates/entity/`, `templates/workspace-capabilities.toml` | Operator-editable template defaults staged under `~/.planar/templates/`. |
| `docs/architecture.md` | System overview — storage model, schema contract, context planes, workbench, adapters |
| `docs/cli-reference.md` | Full CLI surface — commands, flags, exit codes |
| `docs/skill-reference.md` | Skill and agent role overview |
| `docs/concepts.md` | Mental model — scope, association, plan, task, handoff |
| `docs/workflows.md` | End-to-end recipes |
| `docs/` | All user-facing reference documentation |
| `skills/src/` | Unified authored skill sources (`pl-*.md`); the only skills tree checked into the repo. Scriptorium (invoked by `install.sh`, driven by `scriptorium.yaml`) produces the per-vendor outputs at install time. |
| `~/.planar/commands/claude/` | (Generated at install) Claude slash commands, symlinked into `~/.claude/commands/` |
| `~/.planar/skills/codex/` | (Generated at install) Codex skills, materialized under `~/.planar/codex-skills/<slug>/SKILL.md` and installed into `~/.codex/skills/` |
| `~/.planar/skills/copilot/` | (Generated at install) Copilot skills, materialized under `~/.planar/copilot-skills/<slug>/SKILL.md` and installed into `~/.copilot/skills/` |
| `copilot/` | Source Copilot instructions and prompts installed to `~/.copilot/` |
| `agents/` | Installable Planar agents installed to `~/.planar/agents/` |
| `Makefile` | Thin wrapper around `zig build ...` so `make build` / `make test` work from the repo root. |
| `install.sh` | Source-checkout installer; builds the binary via `zig build --prefix ~/.planar` and stages workflow surfaces under `~/.planar`. |
| `scripts/` | Bash tooling (acceptance validators, session stats, git hooks) — independent of the Zig build. |
| `.sqlfluff` | sqlfluff linter config for `migrations/` — dialect `sqlite`, lowercase keywords, 2-space indent. |

## Migrations

Schema migrations live at repo root under `migrations/` in sqlx-cli format
(`NNNNN_<name>.up.sql` / `.down.sql`). Authoring rules — file naming,
`schema_migrations` insert/delete contract, sqlx-cli workflow,
`.sqlfluff` linting, and the up/down/up roundtrip test — are documented in
[`migrations/README.md`](migrations/README.md). New migrations are
created via `sqlx migrate add -r <name> --source migrations`; the Zig
build picks them up automatically via `tools/gen_migrations.zig`
codegen on the next `zig build`.

## Build And Test

`zig build` is the build system of record. The Zig package root IS the repo root (build.zig sits at the top); run `zig build` from the repo root or use the Makefile wrappers:

```bash
# Via Makefile (preferred for one-off scripts):
make build              # → ./bin/planar (ReleaseSafe)
make test               # unit tests
make test-integration   # runs the integration suite under integration_tests/
                        # (build.zig builds Debug test binaries and points the
                        # harness at them via PLANAR_BIN itself)
make test-all           # unit + integration + coverage + authored-surface gates

# Direct zig CLI from the repo root:
zig build                                     # default install (zig-out/bin/planar)
zig build test                                # unit tests
zig build test-integration                    # integration tests
zig build run -- <subcommand>                 # run from source
```

Optimize modes follow Zig conventions: `Debug` (default), `ReleaseSafe`, `ReleaseFast`, `ReleaseSmall`. Set via `zig build -Doptimize=<mode>` or `make build OPTIMIZE=<mode>`.

Two build-cache invariants keep iterative builds cheap; don't regress them:

- **Version metadata is opt-in.** `planar version` git metadata (sha, date,
  dirty flag) is embedded only when `-Dversion-meta=true` is passed; dev
  builds embed the stable sentinel `dev`. Auto-resolving it by default baked
  the live sha + dirty flag into a module every binary imports, so every
  commit and every clean↔dirty flip (untracked files count) invalidated the
  entire build graph — a full multi-minute rebuild with zero source changes.
  `install.sh` passes the flag; nothing else should.
- **Worktrees share the main checkout's Zig cache.** The Makefile exports
  `ZIG_LOCAL_CACHE_DIR` pointing at the main checkout's `.zig-cache` (derived
  via `git rev-parse --git-common-dir`), so builds in agent-dispatch worktrees
  start warm. When running bare `zig build` inside a worktree, prefer the
  `make` wrappers (or export the variable yourself) to avoid a from-scratch
  rebuild of the vendored C deps and module graph.

### Test stratification

Planar uses a two-tier test model. Both layers earn their keep and the
distinction is load-bearing — never collapse them.

- **Unit tests** — `test` blocks colocated with the code under test under
  `src/<module>/`. They exercise the module directly (plus the `db`
  module when they need a DB) and run under `zig build test`. These catch
  logic and SQL regressions.
- **CLI integration tests** — `integration_tests/` (at the repo root) holds
  black-box suites that exercise the compiled `planar` binary via the
  `harness.zig` runner (`harness.smoke`, `harness.mustRun`,
  `harness.mustRunJSON`, `harness.expectFailure`). The suite imports
  nothing from the engine modules. These lock the user-visible contract —
  flag names, JSON shapes, exit codes, status-transition rules — so
  internal refactors cannot silently break it.
- **Cross-binary parity gate — RETIRED.** The port was guarded by a
  `make parity-check` gate that diffed the zig binary against the archived Go
  reference. It is gone (planar task 5623): the reference is a frozen archive
  whose last migration is `00030`, so it and the current binary can no longer
  open the same database, and the audit's premise was that both operate on
  identical state. Giving each its own copy would not rescue it — the
  comparison becomes "Go at schema 30 vs zig at a later schema", where pure
  schema drift registers as parity gaps that are not parity gaps. The
  integration suite is the standing guard. The `parity_*` suites under
  `integration_tests/` remain LIVE and are not Go-dependent: they assert the
  current binary's own user-facing contract (help prose, exit codes, JSON
  shapes, render behaviour) that the audit originally surfaced.
  `scripts/parity-data/parity-triage.md` is retained as their rationale.

- **Authored-surface lint gate** — `make cli-usage-check` (also `zig build
  cli-usage-check`) runs two ordered validators over `agents/`, `skills/src/`,
  and `docs/`. First, `tools/cli_usage_lint.zig` dumps the three planning-state
  binaries' `schema` JSON catalogs and preserves the existing check that
  authored commands never reference an unexposed flag. `planar-execute` is
  intentionally excluded because it has no `schema` catalog; its frozen Lua
  host-function manifest is covered by unit tests. Lines containing
  `cli-lint-ignore` remain the narrow schema-lint escape hatch. Second,
  `tools/surface_lint.zig` checks repository-relative links, retired
  references, artifact-set agreement, read-only capabilities, and semantic
  command shapes. Feedback/recovery contract checks enforce the seven literal
  H2 sections for every unified skill unless its frontmatter declares the
  genuine-helper exemption `internal_only: true`. Run
  `make surface-lint` when only the semantic pass is wanted. `make test-all`
  depends on the composed `cli-usage-check` gate exactly once. This preserves
  schema-only diagnostics while also rejecting semantic drift.

#### Integration test methodology

Integration tests cover the user-visible CLI contract. Two test styles
co-exist; both earn their keep:

- **Focused per-verb tests** (`integration_tests/<verb>_test.zig`) pin
  one verb's contract — flags, JSON shape, exit code, error wording.
  They isolate a single surface so a regression there is easy to
  bisect.
- **Scenario tests** (`integration_tests/scenarios/*.zig`) walk a
  realistic operator workflow end-to-end through many verbs. A
  scenario is the kind of session an operator would actually run — a
  feature lifecycle, a polyrepo cross-scope edit, a handoff/resume,
  an external-plane propagation. They catch the "verb works in
  isolation but breaks in the actual flow" class of bug that pure
  per-verb tests miss.

**Scenario conventions:**

- One file per workflow theme, named for the workflow not the verb
  (e.g. `scenario_feature_lifecycle_test.zig`, not
  `scenario_task_done_test.zig`).
- Each `test` block walks a path through the workflow. Use
  `harness.registerProject` + `harness.addAssoc` to seed realistic
  scope state — never construct fixture state via raw SQL inside a
  test; go through the CLI so the test exercises the same code paths
  the operator would.
- After every mutating step, assert on the post-state via
  `*/show --json` (or `*/list --json` for collections). Exit code 0
  is not sufficient — a verb that silently no-ops is still exit 0.
  The JSON shape + the post-state is the contract.
- A scenario that needs to compare an entity's field across two steps
  should snapshot the JSON via `mustRunJSON(T, ...)` between them, not
  re-derive expectations from constants.
- Scenarios MAY (and often should) cross verb boundaries
  intentionally — e.g. add a question, link it to a plan, advance the
  plan, then assert the question still surfaces in `plan show`.

**Contribution policy:**

- A PR that adds a new top-level verb, subcommand, or flag MUST also
  add or extend an integration test that exercises it *in a
  realistic operator workflow*. A "verb exists and emits JSON" smoke
  test does not count; the test must call the verb the way an
  operator would, in a fixture that mirrors real on-disk state.
- A PR that changes a verb's JSON shape, exit-code mapping, or
  status-transition rules MUST update the corresponding focused test
  (or add one) in the same change, and re-run any scenarios that
  touch the verb.
- A PR that adds a new external-plane adapter or migration MUST add a
  scenario that walks the new code path end-to-end against a fixture
  external system (in-process `std.http.Server` for HTTP adapters).
- `make coverage` (see scripts/coverage-check.sh) reports the current
  `(verb, subcommand)` leaf-coverage ratio across `integration_tests/`
  and fails when the ratio drops below the recorded baseline. A new
  leaf added without a test trips the gate immediately.

**Bug-fix discipline: red-then-green, never silently work around:**

- When a scenario authoring run surfaces a real bug (verb fails,
  edge isn't written, status guard is missing, etc.), file a new
  test that **asserts the documented contract and fails** before
  touching the engine. This is the canonical red-test. Commit it
  as its own commit with a "Red test: …" subject so the discovery
  arc shows up in `git log`.
- Then land the engine / handler fix in the next commit. The same
  test goes green. The two commits together prove the fix actually
  closes the contract the test pins.
- **Do NOT silently route around the bug** by editing the scenario
  to assert only the working subset. That gives a false "tests
  passing" signal and the bug slips into the long tail. If the bug
  is truly out of scope for the current cycle, file it as a task
  on the anchor plan with a `TODO(plan:<id>, task:<id>)` comment
  in the test — but the red test still goes in first.
- This discipline is what surfaced the five plan-352 bugs (decision
  add --plan no-op, question wontfix from answered, templates
  validate --json, scenario verify --outcome). Each one had been
  papered over by the original scenario; the red-test pass on top
  exposed and fixed them.

**Harness fail-loudness invariant:**

- Every `must*` / `expectFailure*` helper in `integration_tests/
  harness.zig` panics on contract violation. A non-zero exit from
  `mustRun` / `mustRunWith` / `mustRunInDir` panics; a zero exit
  from `expectFailure*` panics; a JSON-decode failure in
  `mustRunJSON` panics. The diagnostic is printed via
  `std.debug.print` immediately before the panic so the test runner
  surfaces both.
- The earlier `std.testing.expect(false) catch {}` pattern was a
  silent swallow — the helper returned bogus stdout, the test
  continued, and downstream assertions passed against garbage.
  That pattern is **prohibited** in the harness; treat any future
  re-introduction as a bug.
- Tests that need to inspect a non-zero exit without panicking
  call `suite.execWith(...)` (or `exec`, `execWithInDir`)
  directly and assert on `res.term.exited` themselves. That's the
  documented escape hatch when both branches are operator-
  reachable contract paths (e.g. "verb may legitimately fail when
  contacting the network").

Run integration tests via `make test-integration`. The zig-level step
builds Debug `-Dtest-binary=true` binaries and points the harness at them
via `PLANAR_BIN` itself; the make wrapper adds the shared-worktree
`ZIG_LOCAL_CACHE_DIR` export and is the canonical entry point. (It no
longer pre-builds ReleaseSafe `./bin` binaries — the suite never executed
those.)

Migrations are plain SQL files under `migrations/` in sqlx-cli format
(`-r` reversible pairs). The runtime applies the embedded `migrations`
module at startup. To create a new migration:

```bash
sqlx migrate add -r <name> --source migrations
# Edit migrations/NNNNN_<name>.up.sql and migrations/NNNNN_<name>.down.sql.
zig build                                      # codegen picks up the new files
```

To smoke a migration as raw SQL against a scratch database:

```bash
sqlite3 /tmp/cp-smoke.db < migrations/00001_foundation.up.sql
```

**Never run a from-source binary against the real database.** `planar`
resolves `$PLANAR_DB` (NOT `$PLANAR_HOME`, which is a common and costly
mistake) and falls back to `~/.planar/planar.db`, and the runtime applies
pending migrations **automatically** on first use. So a single bare
`zig build run` / `./zig-out/bin/planar <verb>` from a branch carrying a new
migration silently migrates the operator's live database past the version
every installed binary supports — every other agent on the machine then fails
with `SchemaVersionAhead`, and the only remedy is rolling the migration back
by hand. Use `make smoke ARGS="..."` (throwaway DB under `.zig-cache/smoke/`)
for hand-run checks, and `make smoke-reset` to clear it. The automated suites
are already isolated: unit tests use in-memory SQLite and the integration
harness allocates a scratch DB under `.zig-cache/tmp/`, asserted on every
`Suite.init`.

When invoking the CLI to exercise documented behavior (running skills, composing verbs from agent docs, scripting workflows), use the bare `planar` command — it resolves via `$PATH` to the installed binary at `~/.planar/bin/planar`, which is what users and skills actually run. Reach for `./bin/planar` only when explicitly testing a fresh local build, and say so at the call site. Defaulting to `./bin/planar` risks running stale or branch-experimental code without realizing it.

## Zig Style

The defaults are mainstream Zig; deviations require justification.

- `zig fmt` (via `make fmt` or `zig fmt build.zig src tools integration_tests`) clean before merge. `zig build` clean (warnings are errors). `make fmt-check` enforces formatter cleanliness in CI.
- Identifiers: `camelCase` for functions and variables, `PascalCase` for types, `SCREAMING_SNAKE_CASE` for constants by convention. Module file names lowercase, short, no underscores when possible.
- Errors: Zig's error unions (`!T`) and error sets. Define module-local error sets where the surface is bounded; use `anyerror` only at boundaries that genuinely propagate everything. Wrap underlying errors via explicit `catch` blocks that map them to the module's error set; never silently swallow.
- Allocation: explicit allocator parameters at every API boundary that allocates. Prefer `std.heap.ArenaAllocator` for transient request-scoped work; long-lived state uses `std.heap.GeneralPurposeAllocator` (with leak detection on in tests). Never call `std.heap.page_allocator` from library code.
- Concurrency: explicit `std.Thread` and `std.Thread.Pool`. No async/await runtime (Zig's async is in flux); where I/O can run in parallel (orchestrator dispatch, parallel sync), use threads explicitly. Default to simple sequential code.
- Database access: the `db` module (`src/db/db.zig`) wraps `sqlite.zig` C-API bindings. One `*Db` per process, passed through context or a small `Store` struct. Use `db.beginTx` / `tx.commit` for atomic multi-statement operations. Migrations applied via the embedded `migrations` module — never write ad-hoc SQL migration code in the runtime.
- CLI parsing: vendored from [etcli](https://github.com/rdrsss/etcli) under `vendor/etcli/` (path dep in `build.zig.zon`). Same comptime-driven command-tree parser the project previously carried in-tree as `src/cli/`; extracted upstream for reuse. Subcommand-per-domain (init, scope, association, plan, task, question, scenario, decision, artifact, promote, workbench, workspace, ext, link, sync, resume, handoff, capture, audit, health, help, spec, config, templates, tree); `ext propagate` is the whole-feature propagation verb; `tree` is the hierarchical drill-down verb. Authoritative list lives in `docs/cli-reference.md`.
- HTTP: Zig stdlib `std.http`. Adapter implementations in their own modules behind the adapter boundary (see `docs/architecture.md`). Always set a sensible client timeout (e.g. 30s).
- JSON: Zig stdlib `std.json`. YAML/TOML are limited to surfaces that genuinely need them (templates, configs); prefer JSON where the input is internal-machine-readable.
- Logging: Zig stdlib `std.log`. Structured fields via `std.log.scoped(.<module>)`. JSON output for production; text for dev.
- C interop: bounded to the vendored SQLite amalgamation under `vendor/sqlite/`, called from `src/db/sqlite.zig`. No other C dependencies. Cross-compilation works out of the box because the SQLite C source is compiled by Zig itself.
- Module layout: `src/cmd/planar/main.zig` is thin — wires the parser, dispatches to handlers. Per-verb handlers live under `src/cmd/planar/handlers/`. Domain logic lives under `src/engine/<bucket>/` (buckets: `identity/`, `planning/`, `external/`, `runtime/`) with subsystem modules (`workbench`, `extsync`, `templates`, `adapter`, `ingestor`, `tree`, `config`, `syncengine`) at `src/engine/` top level. Build-time codegen tools live at the repo root under `tools/`.
- Tests: `test "<name>" { ... }` blocks colocated with the code under test are unit tests and run under `zig build test`. The integration suite at `integration_tests/` (repo root) exec's the compiled `planar` binary via `harness.zig`. Run with `make test-integration`; see Build And Test § "Test stratification".
- Test fixtures: per-test temp directories via `std.testing.tmpDir`. SQLite test DBs in-memory or under `tmpDir` for isolation.
- HTTP adapter tests: in-process test servers via `std.http.Server`. No external mock-server dependency.
- Output stability: machine-consumable commands use stable, parseable formats (line-oriented or JSON via `std.json`). Avoid decorative formatting in commands that scripts will read.
- No global mutable state. State threads through explicit `App` / `Store` parameters. Cancellation: explicit "shutdown" channels or atomics, not language-level constructs.
- Cross-platform from day one. Zig handles macOS/Linux/Windows transparently; do not gate features on host-OS conditionals without an explicit reason. The vendored SQLite amalgamation means no cross-compile gotchas.

## Zig Doc Comments

- Top-of-file `//!` comments describe the module's purpose and primary entry points (Zig convention).
- Item-level `///` comments document every exported declaration. Begin with a short summary sentence.
- Document the invariants on types that hold shared state. Document thread safety where applicable.
- Keep comments factual. Do not narrate obvious implementation steps.

## Documentation And Context

- User-facing reference docs live under `docs/`: `architecture.md`, `cli-reference.md`, `skill-reference.md`, `concepts.md`, `workflows.md`. Update the relevant doc in the same change as any behavior or architecture change.
- Project-internal planning artifacts (founding spec, roadmap, ADRs about Planar's own development) live as Planar artifacts under `~/.planar/`, accessible via `planar artifact list --plan <id>` or the workbench.
- The data model is the primary contract. Schema changes flow through versioned migrations starting at `migrations/00001_foundation.up.sql`; always define and review the schema change before writing application code.
- Workflow surfaces (Claude commands, Codex skills, Copilot skills/instructions/prompts, Planar agents) stay aligned automatically: render is the install step, so the only authored surface is `skills/src/`. Scriptorium is the sole renderer (plan 918 M5 retired the in-tree renderer verb); its own drift detection (`scriptorium check`/`status`, out-of-band via a merkle+xxhash manifest) is the CI / pre-merge gate for verifying a render independently of `install.sh` — not a planar verb.
- Build/render-time structured data belongs in YAML / JSON (for example the embedded vendor profile); operator-facing runtime settings stay in TOML (`~/.planar/config.toml`, templates, defaults).
- Skills, agents, commands, and prompts must route through `planar` CLI commands. They must not describe direct database writes or repo-local context scaffolding.

## Checklists

### Spec Change

1. Either add a new artifact via `planar artifact add` on a project-development plan, or update the existing one via `planar artifact update`. If the change is an ADR-level decision, register a `kind=adr` artifact.
2. Update the relevant user-facing reference doc under `docs/` (architecture, CLI reference, concepts, workflows) to reflect the change.
3. Verify any cross-references and anchor links in the updated docs remain valid.
4. If the change implies a non-goal or an unresolved question, capture it as a Planar artifact or decision on the appropriate plan.

### Schema Change

1. Review `docs/architecture.md` (schema contract section) to understand the current model before making changes.
2. Create a reversible migration pair via `sqlx migrate add -r <name> --source migrations`. Do not edit released migrations in place.
3. Place each table's indexes immediately after its `create table` statement.
4. Add a `-- <table>: …` doc comment immediately above every `create table` describing what the table is and why.
5. Add CHECK constraints for enum-shaped columns.
6. Do not declare `BEGIN`/`COMMIT` inside the migration — the runtime wraps each migration in its own transaction. Do not place `PRAGMA foreign_keys = ON` (or any PRAGMA that cannot run inside a transaction) in the file; the runtime sets that per-connection.
7. End the up migration with `insert into schema_migrations (version, description) values (<N>, '<short summary>');`, and mirror the deletion in the down migration. This is the public schema-version contract (see `docs/architecture.md` § schema contract).
8. Validate via the integration suite — `make test-integration` exercises real migrations on every run. For ad-hoc validation, `sqlite3 /tmp/cp-check.db < migrations/000<N>_<name>.up.sql` then inspect `schema_migrations`.
9. Update `docs/architecture.md` to reflect the new schema state.

### Workflow Surface Change

1. Update the single authored surface: `skills/src/<slug>.md`. The per-vendor outputs (`commands/`, `skills/codex/`, `skills/copilot/`) are generated at install time — do not check rendered files into the repo. `copilot/` and `agents/` are still hand-authored and stay in the tree.
2. Keep workflow names, descriptions, and command examples aligned across vendors via the unified source.
3. Workflow text must use the current `planar` CLI and the SQLite-backed model. It must not describe repo-local scaffolding or direct Markdown generation outside of the workbench.
4. If you want to verify renderer output independently of `install.sh`, use scriptorium's own render/check verbs against `scriptorium.yaml`. Plan 918 M5 retired the in-tree renderer and its `integration_tests/skills_render_test.zig` coverage along with it; scriptorium owns render-output verification now.

## Known Failure Modes

| Symptom | Cause | Fix |
|---------|-------|-----|
| Code written before the schema is locked | Skipped schema review step | Stop. Define the schema first, review end-to-end, then resume. |
| `docs/architecture.md` drifts from migrations | Schema change not reflected in docs | Update `docs/architecture.md` in the same change as the migration. |
| Agent or skill scaffolds repo-local context | Pre-context-plane template assumption | Keep context in `docs/`, Planar artifacts, and SQLite. |
| Workflow surfaces drift apart | Updated one vendor but not the others | Audit Claude, Codex, Copilot, and agent surfaces; run parity checks. |
| Re-install leaves an orphaned binary/file in `~/.planar/bin` | Stopped shipping an installed artifact without recording it | Add its `$PLANAR_HOME`-relative path to `install-cleanup.txt`; `install.sh` removes listed paths every run (`zig build --prefix` overwrites what it builds but never deletes a prior install's leftovers). Vendor-surface orphans outside `$PLANAR_HOME` are pruned by scriptorium's render step. |
