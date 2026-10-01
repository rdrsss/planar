# Planar Agent Guide

This file holds the rules an agent needs on every task in this repo. Detail
and rationale live in the docs it links to; read the linked doc before
working in that area. `AGENTS.md` is a symlink to this file.

## Identity

- **Project:** Planar. Local agent-operations infrastructure: planning,
  tasking, scoping, durable agent handoff, vendor parity, and
  operational-plane integration with Jira and GitHub Issues.
- **Stack:** C++26, modules only, built with CMake (`>= 4.3`) and a pinned
  LLVM with `libc++`. State is one SQLite database. Every third-party
  dependency is vendored and compiled from source.
- **Binaries:** `planar`, `planar-agent`, `planar-watch`, `planar-execute`,
  `planar-ext`, plus the `scriptorium` skill and agent renderer.
- **History:** Planar was written in Go, ported to Zig, then ported to C++26.
  Neither earlier tree exists here. Nothing builds, tests or lints against
  them, and "check it against the oracle" is not an answerable question. In
  older planning documents, "etcli" names a CLI library that was never
  adopted; the CLI layer is CLI11 wrapped by `src/lib/cliapp/`.
- System overview: [docs/architecture.md](docs/architecture.md).

## Binaries and their write surfaces

Each binary has a disjoint write surface over the shared database. The boundary is each binary's verb set,
not a runtime ACL.

| Binary | Role | Writes |
|--------|------|--------|
| `planar` | Operator surface | Planning entities, and manual `tasks.status` transitions. |
| `planar-agent` | Agent-callable | `agent_actions`, `agent_work_claims`, the `routing_dispatch_*` tables, and `tasks.status` as part of a coordinated operation. Also the host-queue tables `queue_entries`, `queue_history` and `queue_schema`, through `queue run`. |
| `planar-watch` | Read-only viewer | Nothing. Opens SQLite with `mode=ro`. |
| `planar-ext` | Jira and GitHub Issues adapters | `external_links`, `external_systems`, `sync_events` only, enforced by a `sqlite3_set_authorizer` allowlist. Read-only on planning tables. |
| `planar-execute` | Workflow entry point and client | Nothing. Holds no SQLite handle; reaches state by shelling `planar` and `planar-agent`. |

- `planar-agent`, `planar-watch` and `planar-ext` each have a
  `capability.t.cpp` that locks the boundary. `planar`'s surface is pinned by
  `src/cmd/planar/parity.t.cpp` and the `schema` catalog.
- **`planar-agent` executes commands and owns the host queue (plan 1080,
  accepted by the operator on 2026-09-28; folded into `planar.db` by plan
  1089).** `planar-agent queue run -- <command>` is the host-wide build and
  test queue: it waits in a queue shared by every project on the host, runs
  the command it was given in the caller's directory with the caller's
  environment, and exits with the command's status. Its state lives in
  `planar.db` (migration `00040_host_queue`), in its own tables
  `queue_entries`, `queue_history` and `queue_schema`, behind its own
  module, so the queue SQL stays separable from the planning SQL. Only
  `planar-agent` writes those tables. The installer migrates `planar.db` and
  retires the old `~/.planar/agent.db`.
  - **Schema tolerance.** `queue` verbs refuse a `planar.db` that is BEHIND
    the binary at exit 125. They run against one that is AHEAD when the
    queue's own `queue_schema` check passes (tables and marker present, the
    marker's `compat` not above this build's, every column the binary reads
    present). If the check fails the verb refuses at 125 with
    `queue_schema_incompatible` (ahead) or `queue_schema_foreign` (same
    version number, different migration). Neither is the exit-7
    `schema_version_ahead` that every other verb gives. So `queue run` keeps
    working while the main schema is ahead of this install.
  - **Claims keep the exact-version rule.** Claims and heartbeats, including
    the renewal `queue run --claim` performs, still require `planar.db` to
    be at exactly the binary's version. Against an ahead database
    `queue run --claim` prints a warning that it cannot renew the claim, the
    command still runs, and the lease can lapse while it waits or runs. In a
    migration cycle use the freshly built head binary
    (`./bin/planar-agent`) for `queue run --claim`, as the rest of the claim
    ritual already does, or leave out `--claim` and heartbeat separately
    with the head binary.
  - `planar-watch` reads the queue views from `planar.db` read-only.
  - The queue is a coordination aid and not a security boundary; `queue` is
    a domain of `planar-agent` and never of `planar`.
  - The agent-database module, the `migrations-agent/` stream and the
    `PLANAR_AGENT_DB` test pins are gone. A set `PLANAR_AGENT_DB` is ignored,
    never refused; only the installer still knows `agent.db`, to retire it.
- There is no `planar agent <verb>` namespace. Agent observability is on
  `planar-watch`; agent-table writes are on `planar-agent`.
- The `ext` and `sync` verb domains are on `planar-ext`, not `planar`.
  `planar-ext sync pull` does not apply remote values to planning entities.
  It emits `remote_title` and `remote_status`; an agent verifies them and
  writes through `planar`.
- All five binaries expose a `schema` JSON catalog, and `make
  cli-usage-check` lints authored surfaces against all five.

### `planar-execute` is mid-transition

`planar-execute run <wf.lua> --phase <name>` runs a deterministic Lua
workflow locally over allowlisted host functions (`cli`, `git`, `fs`, `flow`,
`ctx`). It exposes no model-spawning host function and is outside the claim
ritual. The binary also carries the Centurion client verbs: `submit`,
`status`, `cancel`, `follow`, `host` and `profile`.

Decision 1007 and plan 1033 make Centurion the workflow engine, with
`planar-execute` as its configuration, bootstrap and client entry point. The
local `run` path stays until plan 1033's cutover milestone (M5). Until then,
every guard and boundary test that pins the embedded runner stays in force.
Do not relax one ahead of the milestone that replaces it. Planar itself still
never shells a headless model.

### Claim ritual

Every code-writing agent dispatch follows this sequence:

1. `planar-agent pull <plan-id>`, or `planar-agent claim --entity task:<id>`
   for a hand-picked task.
2. `planar-agent heartbeat --claim <token>` at half the TTL.
3. Exactly one terminal verb: `planar-agent complete`, `fail`, `release` or
   `block`.

The terminal verbs flip the claim and `tasks.status` in one transaction.
Never split that into `planar task done` plus `planar-agent release`; a
process death between the two strands the claim. In orchestrator dispatch the
orchestrator owns the terminal verb, and coders heartbeat and return. In
direct-claim dispatch the caller runs it. Full sequence:
`agents/methodology.md` § Coordination claims.

## Operating rules

- The `planar` CLI is the only supported access layer for workflows. Skills,
  agents, commands and prompts use current CLI commands. They never describe
  direct database writes or repo-local context scaffolding.
- To exercise documented behaviour, run the bare `planar` from `$PATH`. Use
  `./bin/planar` or `build/debug/bin/planar` only to test a fresh build, and
  say so at the call site.
- **Never run a from-source binary against the real database.** The runtime
  resolves `$PLANAR_DB` (not `$PLANAR_HOME`), falls back to
  `~/.planar/planar.db`, and applies pending migrations automatically. Use
  `make smoke ARGS="<verb>"`, or set `PLANAR_DB` and `HOME` to scratch paths.
  `make run` uses the real database. See [docs/testing.md](docs/testing.md).
- Write verbs use the strict scope resolver. Run from a directory inside the
  target project, or pass `--scope`. There is no `--no-scope-check` flag.
- The data model is the contract. Define and review a schema change before
  writing application code. Other readers of the database open it read-only
  and verify the schema version first.
- A behaviour or architecture change updates the affected doc under `docs/`
  in the same change.
- Every commit carries a `Signed-off-by` line (`git commit -s`).

### Where things go

| Content | Location |
|---------|----------|
| Project tooling, one directory per tool | `src/tools/<tool-name>/` |
| Installable agents | `agents/` |
| Authored skill sources | `skills/src/` |
| Agent-role model routing table, hand-maintained | `agents/models.md` |
| User-facing reference docs | `docs/` |
| Planning artifacts: specs, roadmaps, ADRs | Planar artifacts under `~/.planar/`, through `planar artifact` and the workbench |
| Operational state | SQLite |
| Operator-local skills and agents | `~/.planar/local/{skills,agents}/`, linked with `planar local link` |

- The per-vendor surfaces (claude, codex, copilot, gemini) are rendered at
  install time by `scriptorium`, driven by `scriptorium.yaml`. They are not
  checked in, and `.gitignore` blocks them. Do not author a rendered file.
- Do not scaffold under vendor dot directories such as `.github/`.
- Workspace-root `AGENTS.md` and `CLAUDE.md` in a polyrepo workspace are
  generated by `planar workspace regenerate`. Do not hand-edit them.
- Published-doc coverage and drift detection belong to the standalone
  `tabularium` tool. Planar ships no documentation binary.

### Cross-scope guard

The guard runs on ten verbs, not on every mutation: `spec ingest --apply`,
`feedback triage set`, `audit publish-decision`, `decision accept`, `decision
withdraw`, `task update`, `closure compute`, and `planar-ext sync push`,
`sync pull` and `sync resolve`. Every other mutation writes without comparing
scopes. `grep -rn guard_with_membership src/cmd` is the measurement.

- The comparison is membership-aware. Operator scope `assoc:<org>` covers an
  entity at `repo:<member>`. The reverse direction refuses.
- A mismatch exits 5 and has no flag-based bypass. An unresolvable `--scope`
  slug exits 1.
- `--scope` means different things on different verbs. On `plan update`,
  `artifact update` and `annotate update` it is a patch field that moves the
  entity. On `list`, `search`, `tree`, `dashboard` and `health` it is a read
  filter.
- Link verbs and `ext propagate` are unguarded on purpose.

Do not claim the guard is universal without measuring. Matrix:
[docs/concepts.md](docs/concepts.md#cross-scope-guard). Per-verb listing:
[docs/cli-reference.md](docs/cli-reference.md#cross-scope-guard).

### Dependencies

- Each dependency is one `CPMAddPackage(...)` block in
  `cmake/dependencies.cmake`, pinned by `URL` to a versioned release archive
  plus `URL_HASH SHA256=...`. Never vendor by `GIT_REPOSITORY`, submodule,
  `FetchContent` or `find_package`. For GitHub sources, pin the
  `codeload.github.com/.../tar.gz/refs/tags/<tag>` URL; the `archive/` form
  redirects.
- Third-party sources are cached under `vendor/` and committed, so a
  configured build does not touch the network.
- First-party dependencies (Centurion) are pinned the same way but cached
  under the gitignored `external/`. A fresh checkout's first configure
  fetches them, and a private repo needs a token: `GITHUB_TOKEN=$(gh auth
  token) cmake --preset debug`. Do not move a third-party dependency to
  `external/` to save space.
- External programs that the binaries, a bundled skill or agent, or the
  installer shell out to are listed in both `README.md` § Prerequisites and
  the `BUILD_DEPS` / `RUN_DEPS` manifests in `install.sh`. Update both in the
  same change.
- When an installed artifact stops shipping, add its `$PLANAR_HOME`-relative
  path to `install-cleanup.txt`.

## Source layout

| Path | Role |
|------|------|
| `CMakeLists.txt`, `CMakePresets.json` | Project root. Presets `debug` and `release` inherit a `base` preset that names `cmake/llvm-toolchain.cmake`. |
| `cmake/` | Toolchain discovery, `dependencies.cmake`, the `planar_module()` helper (`module.cmake`), and configure-time codegen that embeds `migrations/` and `templates/defaults/` into generated modules. |
| `src/cmd/<binary>/` | One thin entry point per binary, dispatching to `handlers/<family>/`. `internal/` holds the shared invocation context and database holder. `integration_tests/` holds multi-command lifecycle scenarios. |
| `src/cmd/parity_harness.hpp` | The black-box test harness: `make_arena()` and `run_pinned()`. |
| `src/engine/` | Domain logic and state transitions, one `engine_*` target per directory. |
| `src/lib/` | Shared base modules: `db`, `core`, `cliapp`, `http`, `git`, `process`, `json_dom`, `json_text`, `log` and others. |
| `src/tools/` | Project tooling: `cli_usage_lint`, `surface_lint`, `cli_docs_coverage`, `scriptorium`, `centurion_client_proof`. |
| `vendor/` | Committed CPM cache of third-party release archives. |
| `external/` | Gitignored CPM cache of first-party dependencies. |
| `migrations/` | Schema migrations in sqlx-cli format. See `migrations/README.md`. |
| `templates/` | Propagation templates and operator-editable defaults. |
| `workflows/` | Lua workflows and `command-policy.json`. |
| `agents/`, `skills/src/` | Authored agent and skill sources. |
| `evals/` | Eval harnesses, fixtures and the results ledger. |
| `docs/` | User-facing reference documentation. |
| `scripts/` | Bash and Python tooling: gates, validators, git hooks. |
| `install.sh` | Builds and installs the binaries, then renders the vendor surfaces with `scriptorium`. |

## Build and test

```bash
make build      # release build; copies the binaries into ./bin/
make install    # release build with version metadata; installs into PREFIX
make test       # debug build, then ctest
make test-all   # the full gate; run it before a pull request
make cpp-lint   # clang-format, clang-tidy and the Doxygen pass
make fmt        # reformat first-party C++ with the pinned clang-format
make help       # every target

cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

- Version metadata is opt-in (`-DPLANAR_VERSION_META=ON`). Only `make
  install` and `install.sh` pass it. Embedding the live sha and dirty flag by
  default invalidates the whole build graph on every commit.
- `make test` alone is not the merge gate; `make test-all` is.
- A green ctest run does not prove the suite ran. `make
  ctest-registry-check` does. A `ctest -R` filter that matches nothing is
  also green, so check the matched count.
- The expected skip tally is zero.
- A no-change build runs zero steps. A full rebuild every time means a
  damaged `.ninja_deps`; see [docs/testing.md](docs/testing.md).
- Do not run two builds in the same build directory at once.
- Agents in this repo send builds and tests through the host queue, not
  directly: `planar-agent queue run --detach --vendor <vendor> --role <role>
  -- make test`, then poll `planar-agent queue status <seq>` every 30 seconds.
  `planar-agent queue rule` prints the full rule, including what to do when
  the queue refuses (exit 125: stop and report, never run the command
  directly). The queue does not replace any gate; see
  [docs/testing.md](docs/testing.md).
- `clang-tidy` is advisory. `clang-format --Werror` and the Doxygen pass
  gate.
- A bug fix lands as a failing "Red test: ..." commit, then the fix. Never
  narrow a test to the subset that works.
- A new verb, subcommand or flag needs a test that exercises it in a
  realistic operator workflow.

Test layers, harness, conventions and rationale:
[docs/testing.md](docs/testing.md). Toolchain:
[docs/toolchain-parity.md](docs/toolchain-parity.md).

## C++ style

- **Modules only.** Every translation unit is a named module: a `.cppm`
  interface and a `.cpp` implementation, imported as `import
  planar.<dotted.path>;`. The standard library is visible only through
  `import std;`. First-party code has no headers, apart from test harness
  headers under `src/cmd/`.
- **Dependencies point downward.** `cmd` imports `engine` and `cliapp`, which
  import `lib` leaves. No cycles, and no edge between sibling command
  binaries.
- **Naming.** `snake_case` for functions, variables, namespaces and types.
  Private and protected data members take a leading underscore (`_timeout`),
  never a trailing one. Public members, including every field of a plain
  `struct`, have no underscore at either end. Template parameters are
  CamelCase.
- **Errors.** `std::expected<T, E>` at API boundaries for expected failures.
  Wrap SQLite, curl and Lua errors into the module's own error enum at the
  boundary. Exceptions are for unrecoverable conditions.
- **Invariants.** Use `planar::core::check()`, never `assert()`. The
  `release` preset defines `NDEBUG`, so an `assert` is absent from the binary
  operators run. `check()` is for a condition believed impossible; it is not
  error handling. `static_assert` is unaffected.
- **Formatting text.** Append with `out += std::format(...)`. Never use
  `std::format_to(std::back_inserter(out), ...)`: the pinned `libc++`
  overflows a stack buffer when an argument's byte length is a non-zero
  multiple of 256.
- **Database.** One connection per process, passed through context structs.
  Migrations apply through the embedded module. Never write ad-hoc migration
  SQL in the runtime.
- **CLI.** CLI11 does tokenization and value coercion only. Help text, the
  `schema` catalog, exit-code mapping and completion are Planar's own, in
  `src/lib/cliapp/`, and are pinned by tests. `planar-execute` keeps a manual
  parser. Register a binary through the helper in `src/cmd/CMakeLists.txt`,
  never a bare `add_executable()`.
- **HTTP.** libcurl behind `src/lib/http/`, always with a client timeout.
  Adapters live under `src/engine/external/` behind `external_adapter`.
- **Serialization.** Glaze for JSON and TOML. JSON for machine-read data;
  TOML for operator-facing config and templates.
- **Logging.** spdlog behind `src/lib/log/`, with a named logger per module.
- **State.** No global mutable state. Cancellation uses explicit signals or
  atomics.
- **Output.** Commands that scripts read emit stable line-oriented text or
  JSON, without decoration.
- **Platforms.** macOS and Linux.
- **Tests.** Catch2 cases in `*.t.cpp` files beside the code, with per-test
  temp directories. Test files open with plain `//` comments, not `///`.

### Doc comments

- Doxygen `///` comments, enforced by `Doxyfile.lint`.
- Every `.cppm` opens with `/// @file <name>` and a `/// @brief` that covers
  the module's purpose, entry points and error-boundary contract.
- Every exported declaration has a `///` comment that begins with a summary
  sentence, with `@param` and `@return` where they are not obvious.
- Document the invariants and thread safety of types that hold shared state.
- State facts. Do not narrate implementation steps.

## Checklists

### Schema change

1. Read the schema contract section of `docs/architecture.md`.
2. Create a reversible pair: `sqlx migrate add -r <name> --source
   migrations`. Never edit a released migration.
3. Put each table's indexes directly after its `create table`.
4. Put a `-- <table>: ...` comment above every `create table`.
5. Add CHECK constraints for enum-shaped columns.
6. Do not write `BEGIN`, `COMMIT` or any `PRAGMA` in the file. The runtime
   wraps each migration in a transaction.
7. End the up migration with `insert into schema_migrations (version,
   description) values (<N>, '<summary>');` and delete that row in the down
   migration.
8. Run `make test`. The suite applies the embedded migrations and includes an
   up, down, up roundtrip. The next build picks up new files without a manual
   codegen step.
9. Update `docs/architecture.md`.

### Spec change

1. Add or update the artifact with `planar artifact add` or `planar artifact
   update`. Register an ADR-level decision as a `kind=adr` artifact.
2. Update the affected reference doc under `docs/`.
3. Check that cross-references and anchor links in the changed docs resolve.
4. Record an implied non-goal or open question as an artifact or decision on
   the plan.

### Workflow surface change

1. Edit `skills/src/<slug>.md` or the file under `agents/`. Do not check in
   rendered output.
2. Use the current CLI and the SQLite-backed model in all workflow text.
3. Run `make cli-usage-check`.
4. To check rendering without installing, run `make eval-render`.
