# Planar Agent Guide

## Identity

- **Project:** Planar
- **Purpose:** Local agent-operations infrastructure spanning planning, tasking, scoping, durable agent handoff, vendor parity, and operational plane integration with Jira and GitHub Issues.
- **Stack:** C++26 binaries (modules-only, no headers in first-party code) plus a SQLite database (vendored SQLite amalgamation under `vendor/sqlite/`, compiled as a static library, no system library dependency). Build via `cmake --preset debug` (or `release`) then `cmake --build build/<preset>` from the repo root, or the `make build` / `make test` wrappers. Migrations live under `migrations/` in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`); configure-time CMake codegen (`cmake/generate_migrations.cmake`) `#embed`s each pair into a generated `planar.db.migrations` module that the runtime applies on startup. `schema_migrations` is the public schema-version contract. Propagation templates for external systems live under `templates/defaults/` and are embedded the same way via `cmake/generate_templates.cmake`. CLI parsing wraps vendored CLI11 (`vendor/cli11/`, tokenization and value coercion only — decision 948) behind Planar's own help renderer, schema-catalog emitter, exit-code mapping, and completion generator in `src/lib/cliapp/`, so the oracle-pinned help text, parse-error wording, and JSON schema catalog survive the parser swap unchanged. HTTP via vendored libcurl (`vendor/curl/`). Logging via vendored spdlog (`vendor/spdlog/`). JSON/TOML via vendored Glaze (`vendor/glaze/`). The workbench is a bidirectionally synced drafting filesystem under `$PLANAR_WORKBENCH_ROOT` (default `~/.planar/workbench/`). See [docs/architecture.md](docs/architecture.md) for the system overview.
- **Repo shape:** Ported from the original Go implementation to Zig (M1–M19 closed; archive remains at github.com:rdrsss/planar-go-archive.git), and now mid-port from Zig to C++26 on branch `rewrite/cpp26` (plan 996; M10 cutover in progress). Five binaries (`planar`, `planar-agent`, `planar-watch`, `planar-execute`, `planar-ext`), schema versioned across thirty-three migration files (`migrations/00001_foundation.up.sql` through `00033_rename_blocks_to_depends_on.up.sql`). `planar-execute` is the deterministic, spawn-free Lua workflow engine revived in plan 633: an LLM caller invokes it to run a deterministic workflow — shelling `planar`/git/fs/control-flow — and collect results. It exposes NO model-spawning host function and holds no SQLite handle (it reaches state only by shelling `planar`). It is **not** a harness. An earlier `planar-execute` had grown re-entrant headless LLM spawning and became a harness in its own right, which is why it was extracted to a separate external project — the revival deliberately reigns that scope back in. `planar-ext` is the newest binary (decisions 995–1001): it owns the operational-plane adapters (Jira, GitHub Issues) that `planar` used to host, opens SQLite directly with read-only access to planning tables and read-write access to exactly `external_links` / `external_systems` / `sync_events`, and no longer auto-applies pulled remote values into planning entities — it emits `remote_title`/`remote_status` proposals for an agent to verify and write back through `planar`. Full capability set: planning loop (spec drafting → task decomposition → execution → propagation), bidirectional workbench sync, configuration plane (`~/.planar/config.toml`), templates layer (`~/.planar/templates/`), three agent roles driving the feature lifecycle (planner, ingestor, ext-sync), and the Jira/GitHub Issues operational-plane adapters above. The Zig implementation remains buildable under `zig/` strictly as the parity oracle for the port (see the M0 split note under Source Layout) — it is not part of the shipped toolchain and is not required to build or install Planar.
- **Binary name:** `planar`.

## Operating Rules

- **`etcli` and `etcli-zig` are two different libraries. Do not conflate them.**
  - **`etcli-zig`** ([github.com/rdrsss/etcli-zig](https://github.com/rdrsss/etcli-zig))
    is the Zig comptime CLI parser this project extracted from its own former
    `src/cli/`, vendored at `zig/vendor/etcli-zig/` and consumed by the Zig
    tree as the `cli` module. It is what every "etcli" reference in this
    repository used to mean.
  - **`etcli`** ([github.com/rdrsss/etcli](https://github.com/rdrsss/etcli))
    is a separate, newer **C++26** library — modules-only, allocation-free,
    reflection-shaped — and is the intended CLI layer for the `rewrite/cpp26`
    tree. Planning documents that say "etcli" mean this one.
  - The vendored package still calls *itself* `etcli` inside
    `zig/vendor/etcli-zig/build.zig.zon`. That is upstream content at tag
    v0.2.0, not ours to rewrite; the surrounding wiring (manifest entry,
    dependency key `etcli_zig`, directory name) says `etcli-zig`.
  - **`etcli` (the C++26 library) was never adopted.** Decision 948 swapped
    the plan mid-port: the C++ tree vendors CLI11 (`vendor/cli11/`) for
    tokenization and value coercion only, wrapped by Planar's own help
    renderer, schema-catalog emitter, exit-code mapping, and completion
    generator under `src/lib/cliapp/` — the layer that carries the
    oracle-pinned parity surface (help text, parse-error wording, exit
    codes, the `schema` JSON catalog). `import cli11;` in
    `src/lib/cliapp/args.cppm` is ground truth for this; decision 948's
    status is still recorded as `proposed` even though the code already
    ships it — that status/code mismatch is open as a question, not
    silently reconciled here.

- Treat this file as this repo's true agent guide, not a template for other repos.
- Keep `AGENTS.md` equivalent to this file (symlink or byte-for-byte copy).
- **All project tooling lives under `src/tools/<tool-name>/`** — one directory per tool (decision 1000). This is the standing home for every tool this project builds, not only the current ones. Pre-existing Zig tooling under `zig/tools/` is not retrofitted: `gen_migrations` and `gen_templates` are already superseded by configure-time CMake codegen (`cmake/generate_migrations.cmake`), and `vendor_sync` is Zig-only because the C++ build vendors via CPM.
- Put installable Planar agents under top-level `agents/`. Do not scaffold them under vendor dot directories.
- Put unified skill source files under `skills/src/`. Do not author generated vendor surfaces directly.
- The per-vendor skill surfaces (`commands/claude/`, `skills/codex/`, `skills/copilot/`) are **not checked into the repo** — they are rendered at install time by scriptorium, invoked by `install.sh` after the binary is built (this replaced the retired in-tree renderer verb at plan 918 M5; see `scriptorium.yaml`). Source-of-truth lives under `skills/src/` only. `.gitignore` blocks the rendered dirs from re-entering the tree. The Tier Table for agent-role model routing is hand-maintained in `agents/models.md`; Planar does not generate it.
- Copilot instructions/prompts stay under `copilot/`. Do not scaffold under `.copilot/` or `.github/`.
- Operator-machine-local skills and agents go under `~/.planar/local/{skills,agents}/`, not in the repo. They are user-machine-local state, never committed, and are linked into each vendor's surface via `planar local link` (which prefixes installs with `local-` to make them visibly user-authored). Promotion from sandbox to canonical is manual — copy the file into the repo and follow the normal contribution flow. There is no `planar local promote` shortcut. See `docs/workflows.md § Recipe 14` for the end-to-end walk-through and `docs/skill-reference.md § Personal sandbox` for the canonical-vs-sandbox boundary.
- User-facing reference docs (architecture, CLI reference, skill reference, concepts, workflows) live under `docs/` in the repo. Project-internal planning artifacts (tech specs, roadmaps, ADRs about Planar's own development) live as Planar artifacts under `~/.planar/`, accessible via `planar artifact` and the workbench. Operational state belongs in SQLite.
- Internal vs published docs. Project-internal planning artifacts (tech specs, roadmaps, decisions, ADRs, sessions) live as Planar artifacts under `~/.planar/` and the workbench, accessible via `planar artifact` and the workbench export. User-facing reference docs (architecture, CLI reference, skill reference, concepts, workflows, features, research, glossary, changelog) live under `docs/` in the repo. Published-doc coverage, drift detection, and linting are owned by the standalone `tabularium` tool and its machine-local database; Planar no longer ships a documentation binary. Don't blur the boundary: a tech-spec is internal authoring; a feature catalog entry is published prose derived from one or more tech-specs.
- Treat the `planar` CLI as the only supported access layer for workflows. Skills, agents, commands, and prompts must use current CLI commands and must not invent direct DB writes or repo-local context scaffolding.
- **Five-binary boundary and claim ritual.** Planar ships five binaries, each with a disjoint write surface over the shared SQLite DB (decision 995 gives the fifth its own explicit doctrinal position rather than leaving it implied):
  - `planar` — operator surface; writes planning entities plus `tasks.status` manual transitions.
  - `planar-agent` — agent-callable; atomic writes to `agent_actions`, `agent_work_claims`, the `routing_dispatch_*` authorization tables, and `tasks.status` only as part of coordinated operations.
  - `planar-watch` — read-only viewer; opens SQLite via `file:?mode=ro`. No writes at all.
  - `planar-execute` — the deterministic, spawn-free Lua workflow engine (plan 633): an LLM caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over allowlisted host functions (`cli`/`git`/`fs`/`flow`/`ctx`), and collects the JSON result. It holds **no SQLite handle at all** — it reaches Planar state only by shelling `planar`/`planar-agent`. It is **deliberately OUTSIDE the claim ritual** below: it is a workflow engine the caller invokes, not an agent-table writer, and exposes NO model-spawning host function (an earlier `planar-execute` grew re-entrant headless LLM spawning and became a harness, which is why it was extracted to a separate external project; the revival reigns that back in).
  - `planar-ext` — the operational-plane binary (decisions 995–1001; extracted from `planar` this session, tasks 6418–6421/6428/6430). It opens SQLite directly rather than shelling back to `planar`: **read-only** on planning tables (`plans`, `tasks`, `questions`, `artifacts`), **read-write** on exactly three tables — `external_links`, `external_systems`, `sync_events` — enforced at the SQLite layer by a `sqlite3_set_authorizer` allowlist that fires on the parsed table name, not by convention alone. It owns both operational adapters, Jira and GitHub Issues together (decision 997 — they sit behind one `external_adapter` interface and splitting them would put that interface across two binaries). `planar-ext sync pull` no longer auto-applies remote values into planning entities (decision 996, a deliberate divergence from the Zig oracle): it fetches and emits `remote_title`/`remote_status`; an agent verifies, synthesizes, and calls `planar` to make the actual planning write. `planar-ext schema` exposes the same deterministic flat JSON catalog as the other three planning-state binaries so `cli-usage-check` polices its verbs the same way (decision 998) — it must NOT inherit `planar-execute`'s lint exemption, because unlike `planar-execute` it carries authored operator-facing verbs.

  The capability boundary is each binary's verb set, not runtime ACLs; the four non-`planar-execute` binaries' boundaries are locked by `integration_tests/capability_boundary_test.zig` (Zig oracle) / the equivalent C++ boundary test. There is no `planar agent <verb>` subcommand namespace — agent observability lives on `planar-watch`, agent-table writes live on `planar-agent`. Every code-writing agent dispatch (orchestrator → coder, hand-picked task dispatch, vendor-hook execution) MUST follow the canonical ritual: `planar-agent pull <plan-id>` (or `claim --entity task:<id>` for hand-picked) → `planar-agent heartbeat --claim <token>` at TTL/2 cadence → exactly one terminal verb of `planar-agent complete | fail | release | block`. The atomic terminal verbs flip claim status and `tasks.status` in one transaction; agents and skills MUST NOT split this into `planar task done` + `planar-agent release` (the intervening process death leaves the claim stranded). For orchestrator dispatch the orchestrator owns the terminal verb; coders heartbeat and return. For barrel-bypass and direct-claim dispatch the caller invokes the terminal verb. See `agents/methodology.md` § Coordination claims and `docs/concepts.md § Binaries` for the full sequence and the capability invariants.
- Workspace `AGENTS.md` and `CLAUDE.md` at a polyrepo workspace root are symlinks (or, in degraded mode, copies) to the canonical generated file under `~/.planar/workspaces/<org_id>/`. Do not hand-edit them; they are regenerated by `planar workspace regenerate`. Operator overrides belong in `routing-table-overrides.json` next to the canonical target.
- Write verbs use the strict scope resolver: skills, agents, commands, and prompts must either run from a cwd inside the target project (relying on cwd derivation) or pass `--scope` explicitly. `--no-scope-check` is a legacy escape hatch and must not appear in routine workflow examples or new skill code. See `docs/concepts.md#scope` for the resolution algorithm.
- Every entity-targeted mutation verb verifies operator-vs-entity scope agreement before writing; cross-scope writes are explicit-only via `--scope <slug>` or `--no-scope-check`. Link verbs (`*_link`, `task touches add/remove`, `links add/remove`) are deliberately unguarded — they create entity_links edges that may legitimately cross scopes (the polyrepo touches/derives-from workflow). Read `docs/concepts.md#cross-scope-guard` for the full guarded/unguarded matrix.
- Use `planar import` to import an existing repo's planning content; it supports an optional `--interpret` LLM pass. The binary verb was trimmed from `pl-import` to `import` in plan 85; the slash command stays `/pl-import` because the `pl-` prefix namespaces it inside the vendor command tree.
- The data model is the contract. Schema changes flow through versioned migrations starting at `migrations/00001_foundation.up.sql`. Other binaries (read-side viewers, web servers, Obsidian bridges) must open the database read-only and verify schema version before operating.
- Architecture changes must update `docs/architecture.md` (and other affected reference docs) in the same change. The schema migration is the primary contract; docs are the human-readable annotation of it.
- External tool dependencies are tracked in two places that MUST stay in sync: `README.md` § Prerequisites (the human-readable inventory) and the `BUILD_DEPS` / `RUN_DEPS` manifests in `install.sh` (the machine-checked list the installer preflights). Whenever the binary, a bundled skill/agent, or the installer starts shelling out to a new program — or stops needing one — update both in the same change. `install.sh` fails fast on a missing build-tier tool and warns on a missing runtime-tier tool; an un-manifested dependency silently breaks for users who lack it.
- **Vendoring rule: vendored third-party dependencies MUST be sourced from a pinned release archive, not a git checkout.** Every dependency the C++ tree needs is declared as a single `CPMAddPackage(...)` block in `cmake/dependencies.cmake`, pinned by `URL` (a versioned release archive — `.zip` / `.tar.gz`) plus `URL_HASH SHA256=...`, cached under root `vendor/` via `CPM_SOURCE_CACHE` and committed, so a configured build never touches the network again. Do **not** vendor by `GIT_REPOSITORY`, submodules, `FetchContent`, or `find_package` for application dependencies. This supersedes the Zig-era `vendor/manifest.zon` + `zig build vendor-sync` mechanism (same philosophy — pinned archive, checked-in sources — CPM is the new tool); that mechanism still governs `zig/vendor/` while the Zig tree is buildable as the parity oracle. (For GitHub sources, pin the direct `codeload.github.com/.../tar.gz/refs/tags/<tag>` URL: the friendlier `…/archive/refs/tags/<tag>.tar.gz` form 302-redirects.)

## Source Layout

**The CMake/C++26 tree at the repo root is Planar's build (plan 996, cutover in progress on branch `rewrite/cpp26`).** `CMakeLists.txt` and `CMakePresets.json` at the repo root are the project root; `src/`, `cmake/`, and root `vendor/` (CPM-cached, committed) hold the C++ implementation. **The Zig implementation lives entirely under `zig/`** — `zig/build.zig`, `zig/build.zig.zon`, `zig/src/`, `zig/tools/`, `zig/integration_tests/`, `zig/vendor/` — and stays buildable there strictly as the **parity oracle** (decisions 963/980/982): it is not part of the shipped toolchain, is not built or installed by `install.sh`, and reaching this milestone does not by itself delete it. Deletion is evidence-gated, not milestone-gated — see decision 963/982's three conditions (a clean state differential across the eleven in-scope ported leaves, `explore` recorded as deferred-by-decision-980 rather than pending, and every oracle-conditional parity skip removed in the same change that deletes `zig/`). Workflow surfaces, docs, templates, and bash tooling stay at the repo root regardless (language-agnostic, shared by both trees while both exist).

| Path | Role |
|------|------|
| `CMakeLists.txt` | Top-level CMake project (`project(planar ...)`, C++26, modules). Rejects in-source builds, wires the pinned-toolchain presets, and adds `src/` and `src/tools/`. |
| `CMakePresets.json` | `debug` / `release` configure presets, both pinning the exact Homebrew LLVM `clang`/`clang++` paths and `libc++` flags (see `docs/toolchain-parity.md`). CMake `>= 4.3` required (module-aware `file(CODEGEN)`/import-std support). |
| `cmake/dependencies.cmake` | Every third-party dependency as one `CPMAddPackage(...)` block each, pinned by `URL` + `URL_HASH SHA256=...`, cached under root `vendor/`. |
| `cmake/generate_migrations.cmake`, `cmake/generate_templates.cmake` | Configure-time codegen: `#embed`s `migrations/*.sql` and `templates/defaults/` into generated C++ modules the runtime applies/reads at startup. Replaces the Zig-era `tools/gen_migrations.zig` / `tools/gen_templates.zig` build-time codegen. |
| `cmake/module.cmake` | `planar_module()` — the project's CMake helper for declaring a modules-only C++ library/target (Catch2 test wiring, warnings-as-errors, `SYSTEM`/`EXCLUDE_FROM_ALL` third-party isolation). |
| `src/cmd/` | One directory per binary: `planar/`, `planar-agent/`, `planar-watch/`, `planar-execute/`, `planar-ext/`. Each is its own CMake target wired through `add_subdirectory(src/cmd)`'s guarded registration helper (never a bare `add_executable()` — see `src/cmd/CMakeLists.txt`). |
| `src/lib/` | Shared C++ modules: `db/` (SQLite connection + migrations), `core/`, `cliapp/` (CLI11-backed parser wrapper — help, schema catalog, exit codes, completion), `engine/` (domain logic, bucketed the same way the Zig tree was: `identity/`, `planning/`, `external/`, `runtime/`, plus subsystem dirs like `extsync/`, `workbench/`, `templates/`, `routing/`), `adapter/`, `http/`, `git/`, `process/`, `json_dom/`, `json_text/`, `log/`, `policy/`, `scope_ref/`, `sha256/`, `docs_manifest/`, `installed_surface/`, `introspection_preview/`. |
| `src/tools/` | Project tooling, one directory per tool (decision 1000): `cli_usage_lint/` and `surface_lint/` (ported from `zig/tools/*.zig` at task 6402 — no `zig build-exe` remains in `cli-usage-check`). `gen_migrations`/`gen_templates` are superseded by the `cmake/generate_*.cmake` codegen above and `vendor_sync` is Zig-only (the C++ build vendors via CPM); none of the three needed a C++ port. |
| `vendor/` | CPM's source cache — committed, pinned release archives only (`catch2`, `sqlite`, `lua`, `glaze`, `curl`, `spdlog`, `xxhash`, `cli11`, `tree_sitter`, `tree_sitter_zig`). No `git clone`/submodule vendoring. |
| `zig/` | The retired-in-progress Zig implementation, kept buildable as the parity oracle. Same internal shape it always had (`zig/src/`, `zig/tools/`, `zig/integration_tests/`, `zig/vendor/`) — see git history pre-M0 for its own source-layout description if you need to read it. |
| `integration_tests/` | The Zig-oracle's black-box integration suite; still exercises whichever binary `PLANAR_BIN` points at. Run via `make test-integration` (`zig build test-integration`). The C++ tree's own tests are Catch2 unit tests colocated as `*.t.cpp`, run via `ctest` (see Build And Test). |
| `migrations/` | SQLite schema migrations in sqlx-cli format (`NNNNN_<name>.up.sql` / `.down.sql`, 5-digit zero-padded prefix). Authoritative source — both the C++ `cmake/generate_migrations.cmake` codegen and the Zig `tools/gen_migrations.zig` codegen pick these up automatically. See `migrations/README.md` for the file format and `schema_migrations` contract. |
| `templates/defaults/` | Propagation templates (JSON) for external operational systems (`github-issues/`, `jira/`). Embedded at build time (`cmake/generate_templates.cmake` for C++, `tools/gen_templates.zig` for the oracle); operator overrides land in `~/.planar/templates/defaults/`. |
| `templates/doc-prompts/`, `templates/defaults/`, `templates/workspace-capabilities.toml` | Operator-editable template defaults staged under `~/.planar/templates/`. |
| `docs/architecture.md` | System overview — storage model, schema contract, context planes, workbench, adapters |
| `docs/cli-reference.md` | Full CLI surface — commands, flags, exit codes |
| `docs/skill-reference.md` | Skill and agent role overview |
| `docs/concepts.md` | Mental model — scope, association, plan, task, handoff |
| `docs/workflows.md` | End-to-end recipes |
| `docs/toolchain-parity.md` | The pinned CMake/LLVM toolchain contract — exact compiler paths, CMake floor, git floor, and why each is load-bearing. |
| `docs/` | All user-facing reference documentation |
| `skills/src/` | Unified authored skill sources (`pl-*.md`); the only skills tree checked into the repo. Scriptorium (invoked by `install.sh`, driven by `scriptorium.yaml`) produces the per-vendor outputs at install time. |
| `~/.planar/commands/claude/` | (Generated at install) Claude slash commands, symlinked into `~/.claude/commands/` |
| `~/.planar/skills/codex/` | (Generated at install) Codex skills, materialized under `~/.planar/codex-skills/<slug>/SKILL.md` and installed into `~/.codex/skills/` |
| `~/.planar/skills/copilot/` | (Generated at install) Copilot skills, materialized under `~/.planar/copilot-skills/<slug>/SKILL.md` and installed into `~/.copilot/skills/` |
| `copilot/` | Source Copilot instructions and prompts installed to `~/.copilot/` |
| `agents/` | Installable Planar agents installed to `~/.planar/agents/` |
| `Makefile` | Thin wrapper around `cmake --preset` / `cmake --build` / `ctest` so `make build` / `make test` work from the repo root; retains a handful of Zig-oracle-only targets (`make test-integration`, `make fmt`) that still shell `zig build` because the oracle is still Zig. |
| `install.sh` | Source-checkout installer; configures and builds the C++ binaries via CMake, installs them with `cmake --install`, then shells the `scriptorium` binary to render and stage the vendor skill/agent surfaces under `~/.planar`. Does not build or touch `zig/`. |
| `scripts/` | Bash tooling (acceptance validators, session stats, git hooks, the installer's dependency/manifest tests) — independent of either build. |
| `.sqlfluff` | sqlfluff linter config for `migrations/` — dialect `sqlite`, lowercase keywords, 2-space indent. |
| `.clang-format`, `.clang-tidy`, `Doxyfile.lint` | Pinned C++ formatting, static-analysis, and doc-comment lint configs (`make cpp-lint`; see `docs/toolchain-parity.md`). |

## Migrations

Schema migrations live at repo root under `migrations/` in sqlx-cli format
(`NNNNN_<name>.up.sql` / `.down.sql`). Authoring rules — file naming,
`schema_migrations` insert/delete contract, sqlx-cli workflow,
`.sqlfluff` linting, and the up/down/up roundtrip test — are documented in
[`migrations/README.md`](migrations/README.md). New migrations are
created via `sqlx migrate add -r <name> --source migrations`; the next
CMake configure (`cmake/generate_migrations.cmake`) `#embed`s the new pair
into the generated `planar.db.migrations` module automatically — no manual
codegen step. (The Zig oracle picks up the same new files via its own
`tools/gen_migrations.zig` codegen on the next `zig build`, as long as `zig/`
remains buildable.)

## Build And Test

**CMake is the build system of record.** The repo root IS the CMake project root (`CMakeLists.txt` sits at the top). Configure with a preset, then build and test:

```bash
# Via Makefile (preferred for one-off scripts):
make build              # cmake --preset release; copies planar/planar-agent/
                        # planar-watch/planar-execute into ./bin/ (see task 6431 —
                        # this target does not yet also copy planar-ext)
make install            # cmake --preset release -DPLANAR_VERSION_META=ON;
                        # cmake --install into PREFIX (default ~/.local/bin)
make test               # cmake --preset debug; cmake --build; ctest
make test-integration   # runs the Zig-oracle's black-box suite under
                        # integration_tests/ against whichever binaries
                        # $PLANAR_BIN et al. point at (defaults to the Zig
                        # build; `make test-parity-cpp` points it at the C++
                        # binaries instead)
make test-all           # unit (ctest) + integration + coverage + cli-usage-check

# Direct CMake/ctest from the repo root:
cmake --preset debug                          # or --preset release
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Presets `debug` and `release` both pin the exact Homebrew LLVM `clang`/`clang++` paths and `libc++` flags declared in `CMakePresets.json`; see [docs/toolchain-parity.md](docs/toolchain-parity.md) for why the pin is load-bearing and how to resolve it on a non-Homebrew-ARM-macOS host. CMake `>= 4.3` is required.

**The Zig tree (`zig/`) still builds and tests independently** as the parity oracle, via its own `zig build` / `zig build test` / `zig build test-integration` (see `zig/` in Source Layout). It is not part of `make build`/`make install`/`make test`; `make test-integration` and `make fmt`/`fmt-check` are the Makefile targets that still shell into it.

One build-cache invariant carries over from the Zig era and still matters for the oracle:

- **Version metadata is opt-in.** `planar version` git metadata (sha, date,
  dirty flag) is embedded only when explicitly requested (`-DPLANAR_VERSION_META=ON`
  for the C++ build, `-Dversion-meta=true` for the Zig oracle); dev builds embed
  the stable sentinel `dev`. Auto-resolving it by default bakes the live sha +
  dirty flag into a module every binary imports, so every commit and every
  clean↔dirty flip (untracked files count) invalidates the entire build graph —
  a full rebuild with zero source changes. `install.sh` / `make install` pass
  the flag; `make build` (dev-facing) does not, and nothing else should.

### Test stratification

Planar uses a two-tier test model. Both layers earn their keep and the
distinction is load-bearing — never collapse them.

- **Unit tests** — Catch2 `TEST_CASE`s in `*.t.cpp` files colocated with the
  module under test throughout `src/lib/` and `src/cmd/`. They exercise the
  module directly (plus the `db` module when they need one) and run under
  `ctest` (`make test`). These catch logic and SQL regressions. `make
  test-cpp-report` runs the same ctest suite and reports its skip tally — a
  skipped case asserted nothing, so a rising skip count is itself a signal;
  `make test-cpp-strict` (`PLANAR_PARITY_STRICT=1`) turns an absent Zig oracle
  from a skip into a hard failure for the differential cases that need it.
- **CLI integration tests** — `integration_tests/` (at the repo root) holds
  black-box suites, still driven by the Zig-era `harness.zig` runner
  (`harness.smoke`, `harness.mustRun`, `harness.mustRunJSON`,
  `harness.expectFailure`), that exercise whichever compiled binary
  `$PLANAR_BIN` (and its `PLANAR_AGENT_BIN` / `PLANAR_WATCH_BIN` /
  `PLANAR_EXECUTE_BIN` siblings) points at. The suite imports nothing from the
  engine modules, so it grades the C++ binaries exactly as it graded the Zig
  ones — `make test-parity-cpp` runs it against the CMake build's output.
  These lock the user-visible contract — flag names, JSON shapes, exit codes,
  status-transition rules — so internal refactors (including the language
  rewrite itself) cannot silently break it. This is decision 932/D6: the
  integration suite is the parity oracle for the port.
- **Cross-binary parity gate against the Go archive — RETIRED.** An earlier
  `make parity-check` gate diffed the Zig binary against the archived Go
  reference. It is gone (planar task 5623): the reference is a frozen archive
  whose last migration is `00030`, so it and the current binary can no longer
  open the same database, and the audit's premise was that both operate on
  identical state. The integration suite is the standing guard. The
  `parity_*` suites under `integration_tests/` remain LIVE and are not
  Go-dependent: they assert the current binary's own user-facing contract
  (help prose, exit codes, JSON shapes, render behaviour) that the audit
  originally surfaced. `scripts/parity-data/parity-triage.md` is retained as
  their rationale.
- **C++ format/tidy/doc-comment lint** — `make cpp-lint` runs the pinned
  LLVM's `clang-format --dry-run --Werror`, `clang-tidy`, and a Doxygen
  doc-comment pass over every first-party `.cppm`/`.cpp` file (see
  `docs/toolchain-parity.md`). Of the three, only `clang-format --Werror` and
  the Doxygen pass (`WARN_AS_ERROR`) actually gate the recipe — a nonzero
  exit from either stops `make` before the next step runs. **`clang-tidy` is
  advisory only**: the recipe invokes it with no `--warnings-as-errors` and
  `.clang-tidy` declares no `WarningsAsErrors` key, so any number of
  clang-tidy warnings still exits 0 and the recipe proceeds (task 6438).
  `.clang-tidy` enables exactly one check, `readability-identifier-naming`;
  task 6438 fixed one config/codebase disagreement inherited wholesale from
  tabula (a `PublicMemberSuffix: '_'` rule that matched none of this
  codebase's public struct members and accounted for 2925 of 3030 measured
  warnings) but left 105 genuine residual findings (task 6439) that block
  turning `--warnings-as-errors` on for now. It requires `build/debug`
  already configured and built (clang-tidy needs the module BMIs
  materialized) and is deliberately **not** composed into `make test-all`
  yet — see the Makefile's own comment on why, and revisit once the C++ tree
  is the sole implementation.

- **Authored-surface lint gate** — `make cli-usage-check` runs two ordered
  C++ validators, ported from the Zig oracle's `tools/cli_usage_lint.zig` and
  `tools/surface_lint.zig` at task 6402 (decision 1000; both now live under
  `src/tools/`, so **no `zig build-exe` remains in this gate**) over `agents/`,
  `skills/src/`, and `docs/`. First, `cli_usage_lint` dumps the **four**
  planning-state binaries' `schema` JSON catalogs — `planar`, `planar-agent`,
  `planar-watch`, and `planar-ext` (decision 998: `planar-ext` must expose a
  catalog and be added to this invocation, precisely because its extraction
  is the surface most likely to go stale) — and checks that authored commands
  never reference an unexposed flag. `planar-execute` is intentionally
  excluded because it has no `schema` catalog; its frozen Lua host-function
  manifest is covered by unit tests, and it must NOT inherit that exemption
  onto `planar-ext`, which carries authored operator-facing verbs. Lines
  containing `cli-lint-ignore` remain the narrow schema-lint escape hatch.
  Second, `surface_lint` checks repository-relative links, retired
  references, artifact-set agreement, read-only capabilities, and semantic
  command shapes. Feedback/recovery contract checks enforce the seven literal
  H2 sections for every unified skill unless its frontmatter declares the
  genuine-helper exemption `internal_only: true`. Run `make surface-lint`
  when only the semantic pass is wanted. `make test-all` depends on the
  composed `cli-usage-check` gate exactly once. This preserves schema-only
  diagnostics while also rejecting semantic drift.

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
cmake --build build/debug                      # cmake/generate_migrations.cmake re-runs configure
                                                # automatically (file(GLOB CONFIGURE_DEPENDS)) and
                                                # picks up the new files
zig build                                      # (only if you also need the Zig oracle to see it)
```

To smoke a migration as raw SQL against a scratch database:

```bash
sqlite3 /tmp/cp-smoke.db < migrations/00001_foundation.up.sql
```

**Never run a from-source binary against the real database.** `planar`
resolves `$PLANAR_DB` (NOT `$PLANAR_HOME`, which is a common and costly
mistake) and falls back to `~/.planar/planar.db`, and the runtime applies
pending migrations **automatically** on first use. So a single bare
run of a freshly-built binary from a branch carrying a new migration
silently migrates the operator's live database past the version every
installed binary supports — every other agent on the machine then fails
with `SchemaVersionAhead`, and the only remedy is rolling the migration back
by hand. Use `PLANAR_DB=<scratch-path> build/debug/bin/planar <verb>`
(or `PLANAR_DB=<scratch-path> zig-out/bin/planar <verb>` against the Zig
oracle) for hand-run checks against the C++ build. `make smoke` /
`make smoke-reset` are still Zig-oracle-only (`make run`/`make smoke` shell
`zig build run`, isolated under `zig/.zig-cache/smoke/`); there is no C++
equivalent Makefile target yet, so build the scratch-DB invocation above by
hand when smoking the CMake binaries. The automated suites are already
isolated: Catch2 unit tests use in-memory or per-test-tmpdir SQLite and the
`integration_tests/` harness allocates its own scratch DB per suite run,
regardless of which binary it is pointed at.

When invoking the CLI to exercise documented behavior (running skills, composing verbs from agent docs, scripting workflows), use the bare `planar` command — it resolves via `$PATH` to the installed binary at `~/.planar/bin/planar`, which is what users and skills actually run. Reach for `./bin/planar` only when explicitly testing a fresh local build, and say so at the call site. Defaulting to `./bin/planar` risks running stale or branch-experimental code without realizing it.

## C++ Style

The defaults follow centurion's proven conventions (decision D4, adopted wholesale); deviations require justification.

- `make cpp-lint` (pinned LLVM `clang-format --dry-run --Werror` + `clang-tidy` + a Doxygen doc-comment pass, configs `.clang-format` / `.clang-tidy` / `Doxyfile.lint` — decision D16, adopting tabula's configs) clean before merge for the two steps that actually gate: `clang-format --Werror` and the Doxygen pass. **`clang-tidy` is advisory only** — see Build And Test § Test stratification for why and its current warning count. `cmake --build` clean with `PLANAR_WARNINGS_AS_ERRORS=ON` (the `debug` preset's default). `cpp-lint` is not yet composed into `make test-all` (see Build And Test); run it explicitly.
- **Modules only, no headers in first-party code** (decision D1/D4): every translation unit is a named module (`.cppm` interface + `.cpp` implementation), imported via `import planar.<dotted.path>;`. `import std;` is the only way the standard library is visible — no `#include <...>` for anything the standard library provides.
- **Strictly downward module dependencies** (decision D15): a module may only import modules beneath it in the dependency graph (`cmd` → `engine`/`cliapp` → `lib` leaves). No cycles, no sideways coupling between sibling command binaries.
- Identifiers: `snake_case` throughout — functions, variables, namespaces, and types alike (e.g. `struct parsed_args`, `class connection`, `struct db_error`); `SCREAMING_SNAKE_CASE` for macros/constants by convention. Module file names lowercase, short, no underscores when possible. Checked by `.clang-format` (enforced) and `.clang-tidy`'s `readability-identifier-naming` (advisory only — see Build And Test § Test stratification), not restated as a separate house style here.
- Errors: `std::expected<T, E>` at API boundaries, not exceptions, for expected-failure paths (decision-driven; see `domain_error_kind` and the per-module `*_error` enums under `src/lib/`). Exceptions are reserved for genuinely exceptional/unrecoverable conditions. Wrap underlying library errors (SQLite, curl, Lua) at the boundary into the module's own error enum; never let a raw vendor error code leak past the module surface.
- Database access: `src/lib/db/` (`db.cppm`/`db.cpp` connection + transaction wrappers, `migrate.cppm`/`migrate.cpp` migration application against the generated `planar.db.migrations` module). One connection per process, threaded through context/handler structs. Migrations are applied via the embedded module — never write ad-hoc SQL migration code in the runtime.
- CLI parsing: CLI11 (vendored, `vendor/cli11/`) wrapped by Planar's own `src/lib/cliapp/` for tokenization/value-coercion only (decision 948) — help text, the `schema` JSON catalog, exit-code mapping, and completion are Planar's own, not CLI11's, because they are the oracle-pinned parity surface. Subcommand-per-domain (init, scope, association, plan, task, question, scenario, decision, artifact, promote, workbench, workspace, ext, link, sync, resume, handoff, capture, audit, health, help, spec, config, templates, tree); `ext propagate` is the whole-feature propagation verb (now on `planar-ext`); `tree` is the hierarchical drill-down verb. Authoritative list lives in `docs/cli-reference.md`.
- HTTP: vendored libcurl (`vendor/curl/`) behind `src/lib/http/`. Adapter implementations (Jira, GitHub Issues) live under `src/lib/engine/external/` behind the `external_adapter` interface, now compiled into `planar-ext`. Always set a sensible client timeout (e.g. 30s).
- Serialization: Glaze (vendored, `vendor/glaze/`) owns both JSON and TOML (decisions D8/D12) — `src/lib/json_dom/` and `src/lib/json_text/` wrap it for the DOM-shaped and text-emission use cases respectively. Prefer JSON where the input is internal-machine-readable; TOML stays limited to operator-facing config/templates.
- Logging: spdlog (vendored, `vendor/spdlog/`) behind `src/lib/log/`. Structured fields via named loggers per module. JSON output for production; text for dev.
- C interop: bounded to the vendored SQLite amalgamation under `vendor/sqlite/` (called from `src/lib/db/`) and the vendored Lua runtime under `vendor/lua/` (the `planar-execute` sandbox). No other C dependencies beyond what curl/spdlog/Glaze themselves need.
- Module layout: `src/cmd/<binary>/` holds one thin entry point per binary that wires the CLI app and dispatches to handlers under its own `handlers/`. Domain logic lives under `src/lib/engine/<bucket>/` (buckets: `identity/`, `planning/`, `external/`, `runtime/`, plus subsystem dirs like `extsync/`, `workbench/`, `templates/`, `routing/`, `config/`) — the buckets carry forward 1:1 from the Zig tree's `src/engine/` layout (ADR-0007/0008). Project tooling lives under `src/tools/<tool-name>/` (decision 1000), not under `cmd/` or `lib/`.
- Tests: Catch2 `TEST_CASE`/`SECTION` blocks in `*.t.cpp` files colocated with the code under test (decision D14, tabula's test convention) run under `ctest` (`make test`). The Zig-oracle integration suite at `integration_tests/` (repo root) still exec's whichever compiled binary `$PLANAR_BIN` points at via `harness.zig`; run with `make test-integration` (against the Zig build) or `make test-parity-cpp` (against the CMake build's binaries); see Build And Test § "Test stratification".
- Test fixtures: per-test temp directories (Catch2 + a small RAII tmpdir helper). SQLite test DBs in-memory or under a temp dir for isolation.
- HTTP adapter tests: in-process fixture server (`src/lib/http/fixture_server.hpp`). No external mock-server dependency.
- Output stability: machine-consumable commands use stable, parseable formats (line-oriented or JSON via Glaze). Avoid decorative formatting in commands that scripts will read.
- No global mutable state. State threads through explicit context/handler parameters. Cancellation: explicit signals/atomics, not language-level constructs.
- Cross-platform: macOS + Linux parity first (matching centurion's toolchain-parity model); Windows is a post-cutover concern (spec non-goal). The vendored SQLite amalgamation, compiled from source, avoids cross-compile gotchas on the platforms it targets.

## C++ Doc Comments

- Doxygen-style `///` comments, enforced by the pinned `Doxyfile.lint` (part of `make cpp-lint`). Every `.cppm` interface unit opens with `/// @file <name>` and `/// @brief` describing the module's purpose, primary entry points, and error-boundary contract (e.g. "every fallible boundary surfaces `std::expected<T, E>`; no exceptions cross the module boundary").
- Item-level `///` comments (with `@param` / `@return` where non-obvious) document every exported declaration. Begin with a short summary sentence.
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
| Re-install leaves an orphaned binary/file in `~/.planar/bin` | Stopped shipping an installed artifact without recording it | Add its `$PLANAR_HOME`-relative path to `install-cleanup.txt`; `install.sh` removes listed paths every run (`cmake --install` overwrites what it builds but never deletes a prior install's leftovers). Vendor-surface orphans outside `$PLANAR_HOME` are pruned by scriptorium's render step. |
