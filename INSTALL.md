# Installing Planar

This guide covers three install paths, ordered from simplest to most flexible:

1. [Quick install (`make install`)](#quick-install-make-install) — just the executables, no vendor surfaces.
2. [Full install (`install.sh`)](#full-install-installsh) — binary + agent specs + vendor surfaces (Claude / Codex / Copilot / Gemini).
3. [Build from source](#build-from-source) — for contributors.

Plus [uninstall](#uninstall), [troubleshooting](#troubleshooting), and the [install layout reference](#install-layout-reference).

## Prerequisites

Planar shells out to a small set of external tools. On macOS, install them via Homebrew:

```bash
brew install cmake ninja llvm python git gh jq ripgrep tbb
```

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries, on every install path. Both presets resolve the toolchain through `cmake/llvm-toolchain.cmake`, which discovers the prefix (an explicit `-DPLANAR_LLVM_PREFIX` first, then `brew --prefix llvm`, then apt.llvm.org's versioned prefixes and `PATH`) and refuses a candidate that lacks a modules-enabled `libc++`. `install.sh` additionally preflights the Homebrew paths `/opt/homebrew/opt/llvm/bin/clang` and `clang++` before invoking CMake. See [toolchain parity](docs/toolchain-parity.md) for the pinned versions and non-Homebrew-ARM-macOS resolution.
- `tbb` (>= 2021.5) — **required to build**, since `cmake/dependencies.cmake` vendors Mt-KaHyPar (decision 1006, task 6459) as a pinned CPM source block and Mt-KaHyPar's own CMake `find_package(TBB)`s it. Unlike every other third-party dependency this tree takes, TBB is **not** vendored as source: upstream states TBB does not support static linking, so this is a deliberately accepted dynamic system dependency rather than a hermetic one. `install.sh` preflights `brew --prefix tbb` and fails fast if it is missing, matching CMake's own `find_package(TBB)` failure.
- `python3` — **required to configure**. The configure step registers Python test runners (`src/tools/scriptorium/core.test.py`, `scripts/install-lib/queue_retire.test.py`, `scripts/queue-logs-after-reset.test.py`) and `find_package(Python3)` is `REQUIRED`. `install.sh` also runs it as its old-queue-database retirement reader (`scripts/install-lib/queue_retire.py`, standard library and `ctypes` only; it runs no other program), and `migrations/README.md`'s counter-reset recipe runs the log helper `scripts/queue-logs-after-reset.py` with it.
- Scriptorium is built from `src/tools/scriptorium/` and installed by CMake; no external Scriptorium executable is required. It needs no dependency beyond Glaze, which the rest of the tree already vendors.
- No network access and no token are needed to configure or build: every dependency is committed under `vendor/` as a pinned release archive.
- `docker` — optional, developer-only. `make linux-gate` builds and tests the tree on Debian trixie in a container (see [docs/testing.md](docs/testing.md#the-linux-gate)). It is not an installer dependency.
- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree. The full install also needs it to clone the source repository.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — required by the bundled agent skills (`pl-spec-draft`, `pl-spec-ingest`, `pl-orchestrator`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session in [docs/getting-started.md](docs/getting-started.md#8-capture-hand-off-and-resume) (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.

The full source-checkout installer also uses the base-system utilities declared
in `install.sh`'s `BUILD_DEPS` / `RUN_DEPS` manifests (`awk`, `basename`, `cat`, `chmod`, `cmp`,
`cp`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mktemp`, `mv`,
`readlink`, `rm`, `rmdir`, and `tr`) alongside CMake, Ninja, and the exact pinned LLVM compiler paths above. These ship with supported Unix-like systems;
the installer preflights them before making changes.

No system SQLite is needed. Planar vendors the SQLite amalgamation under `vendor/sqlite/`; the CMake build compiles it into a static library that statically links into every binary but `planar-execute` (which holds no SQLite handle at all) — no platform-specific build flags, no system library dependency.

### Optional / research tools

- `mtkahypar` — the external [Mt-KaHyPar](https://github.com/kahypar/mt-kahypar) hypergraph partitioner backs the optimal arm of `planar groups recommend --solver=mtkahypar`. As of decision 1006 (tasks 6459/6460) it is **vendored from source** via `cmake/dependencies.cmake` and linked directly into the C++ tree (`libmtkahypar`) — the earlier `--with-mtkahypar` Python-wheel adapter (`bin/mtkahypar`, `opt/mtkahypar/<version>/venv/`) is retired and no `python3` step is needed for this feature any more. The linkage is **off by default**: `mtkahypar` is an `EXCLUDE_FROM_ALL` CMake target, so a plain `cmake --build` never compiles it. Configure with `-DPLANAR_WITH_MTKAHYPAR=ON` to build and link the real seam (requires `tbb`, see above), or pass `--with-solver` to `install.sh`; without it `groups recommend --solver mtkahypar` degrades gracefully to greedy and reports `optimal_available:false`. This fallback is a deliberate, permanent contract, not a placeholder for missing Mt-KaHyPar support.
- `sqlx-cli` and `sqlite3` — only needed for ad-hoc developer workflows against a scratch database (see [Build from source](#build-from-source)); the runtime embeds migrations via build-time codegen and uses the vendored SQLite amalgamation, so neither CLI is a runtime dependency. Install the optional `sqlx-cli` for authoring new migration pairs:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
```

## Quick install (`make install`)

The shortest path. Builds and installs Planar's five executables and the
in-tree `scriptorium` renderer into `~/.local/bin` and nothing else.

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
make install
```

Override the conventional prefix when needed:

```bash
make install PREFIX=/opt/planar
```

Add `~/.local/bin` to your `$PATH`, then verify:

```bash
export PATH="$HOME/.local/bin:$PATH"
planar health
```

This installs only the executables. Runtime migrations and default propagation
templates are *embedded* at build time, so the CLI works standalone against a
local database. Agent specs, slash commands, and skills are **not** embedded —
they are separate source files rendered and staged by `install.sh` — so none of
the vendor surfaces (Claude `/pl-*` slash commands, Codex, Copilot, and Gemini
skills) are wired up by a `make install` or a `cmake --install` alone; for those,
use the [full install](#full-install-installsh).

The five binaries and the in-tree `scriptorium` renderer are installed. The four
that open a database (all but `planar-execute`, which holds no SQLite handle at
all) statically link the vendored SQLite amalgamation — no system library
dependency.

This build does not include the Centurion workflow engine. `planar-execute`'s
engine verbs (`submit`, `status`, `cancel`, `follow`, `host`) require a
Centurion-enabled build (the `dev/centurion-integration` branch); this build
refuses them with `planar-execute was built without the Centurion engine` and
exit `1`. `planar-execute run` is unaffected.

`make install` always builds the `release` CMake preset with version metadata
stamped in (`-DPLANAR_VERSION_META=ON`). To run the same release-preset build by
hand and install under `~/.planar` instead:

```bash
cmake --preset release -DPLANAR_VERSION_META=ON
cmake --build build/release
cmake --install build/release --prefix "$HOME/.planar"
```

This puts the five binaries and `scriptorium` in `~/.planar/bin/`; add that
directory to your `$PATH` and run `planar health` as above. For a debug build
instead, configure and build by hand:

```bash
cmake --preset debug
cmake --build build/debug
cmake --install build/debug --prefix ~/.local
```

## Full install (`install.sh`)

The recommended path. Installs the binary plus the canonical agent specs, vendor surfaces, workflow validation scripts, and migration sources into `~/.planar/`, then installs or symlinks the vendor surfaces into your harness directories (`~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/`, `~/.gemini/antigravity-cli/skills/`). Codex skills are rendered as `~/.planar/skills/codex/<skill>/SKILL.md`; the installer materializes `~/.planar/codex-skills/<skill>/SKILL.md` runtime directories, then installs real Codex skill directories at `~/.codex/skills/<skill>`. Planar-owned agent role specs stay under `~/.planar/agents/`, with rendered per-vendor copies under `~/.planar/agents/<vendor>/` linked into each vendor's agents directory.

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
# or, equivalently:
make install-full       # extra flags via: make install-full INSTALL_FLAGS="--link --force"
```

That's it. The script:

- Builds all five binaries and `scriptorium` from source in its own build directory, `build/install-release/` (never the developer's `build/release/`), by running `cmake --preset release -B build/install-release -DPLANAR_VERSION_META=ON -DPLANAR_WITH_MTKAHYPAR=OFF`, `cmake --build`, and `cmake --install … --prefix "$HOME/.planar"`, which writes `~/.planar/bin/{planar,planar-agent,planar-watch,planar-execute,planar-ext,scriptorium}`.
- Stages `agents/`, `skills/src/`, `scripts/`, `workflows/`, `migrations/`, and `templates/` into `~/.planar/` (migrations are staged at `~/.planar/migrations/` for ad-hoc `sqlx` use; the binary embeds them at build time via codegen), then renders the per-vendor outputs there with `scriptorium render`.
- Installs the 40 rendered skills, and the rendered agents, into each selected vendor's harness dirs.
- Atomically writes `~/.planar/install-manifest.json` after the selected vendor
  wiring succeeds. The versioned file records selected Planar-managed skill
  and agent projections plus explicitly selected installer extras;
  operator-authored destination files are not claimed.

After the script finishes, add `~/.planar/bin` to your PATH so the `planar` command is available:

```bash
# bash / zsh — add to ~/.bashrc or ~/.zshrc, then reload your shell:
export PATH="$HOME/.planar/bin:$PATH"

# fish — run once:
fish_add_path ~/.planar/bin
```

Verify:

```bash
planar health
```

And in Claude Code, the slash commands should now resolve:

- `/pl-orchestrator <task-id> [<task-id>...]` — drive a goal, plan, or task list end-to-end through the phased lifecycle (planning / spec review / ingestion / execution / finalization / propagation / archive).
- `/pl-coder <task-id>` — implement one task.
- `/pl-reviewer <task-id> <iteration>` — review coder output.
- `/pl-init`, `/pl-scope`, `/pl-plan`, `/pl-task`, `/pl-question`, `/pl-scenario`, `/pl-promote`, `/pl-workbench`, `/pl-sync`, `/pl-ext-create`, `/pl-audit-trail`, `/pl-resume`, `/pl-handoff`, `/pl-help`, `/pl-health` — the core reference workflows wrapping `planar` subcommands.
- `/pl-spec-draft` — draft a feature spec and roadmap into the workbench from a goal statement.
- `/pl-spec-ingest` — decompose workbench Markdown into a fleshed-out task graph (preview by default; `--apply` commits).
- `/pl-workbench-sync` — bidirectional workbench sync (pull, push, status, resolve).
- `/pl-workbench-archive` — archive the workbench tree for a completed feature.
- `/pl-ext-propagate` — propagate a feature plan to a registered operational system (Jira or GitHub Issues).
- `/pl-templates` — list, show, render, and validate operational-plane templates.

In Codex, invoke the same Planar skills with `$` syntax, for example `$pl-task` or `$pl-orchestrator`. Slash syntax is for Claude commands.

### Install flags

| Flag | Purpose |
|------|---------|
| `--prefix DIR` | Install root (default `~/.planar`). |
| `--vendors LIST` | Comma-separated subset (e.g. `claude,codex` or just `claude`). Default `claude,codex,copilot,gemini`. |
| `--no-vendor` | Skip vendor symlinks entirely; install Planar core only. |
| `--link` | Symlink artifacts from the source repo into `~/.planar/` instead of copying. **Dev mode** — edits to the repo propagate immediately. |
| `--force` | Overwrite existing symlinks at the destinations. It does **not** bypass the live-queue guard on an old queue database (see the upgrade note below). |
| `--ignore-live-queue` | Retire (or uninstall) the old queue database even while its queue has live entries, or when `python3` cannot check it. The cost is an orphaned old queue; see the upgrade note below. It never bypasses the old-range checks of the retire step. |
| `--no-prune` | Skip removal of stale vendor files. |
| `--preset NAME` | CMake build preset: `debug`\|`release` (default `release`). |
| `--build-dir DIR` | Where to configure and build (default `build/install-<preset>`). |
| `--with-solver` | Link the Mt-KaHyPar solver (needs `tbb`). Off by default; without it `groups recommend --solver mtkahypar` degrades to greedy. |
| `--dry-run`, `-n` | Show the planned actions without changing anything. |
| `--verbose`, `-v` | Per-file detail (default prints a summary). |
| `--version` | Print the installer version and exit. |
| `--uninstall` | Tear down everything install.sh created. Preserves `~/.planar/planar.db` (with its `-wal`/`-shm` sidecars) and `~/.planar/queue-logs/` unless `--force` is also given. Removes the retired old queue database, behind the same live-queue guard as an install. |

### Copy mode vs link mode

- **Copy mode (default).** `install.sh` *copies* the source artifacts into `~/.planar/`. After install, the source checkout can be deleted; `~/.planar/` is self-contained. Updates to the source require re-running `install.sh`.
- **Link mode (`--link`).** `install.sh` *symlinks* the artifacts from the source repo into `~/.planar/`. Edits to the source propagate immediately. Useful for development on Planar itself or for any contributor iterating on the surfaces. Requires the source repo to stay on disk at its original path.

The global mode describes how source artifacts are staged. The install
manifest also records each projection's actual install kind: Claude skills and
all vendor agent files are links, while Codex, Copilot, and Gemini
directory-shaped skills are copied from their staged `SKILL.md` files so runtimes that do not
follow directory links can discover them.

### Initialize the database

After install, create the local database:

```bash
planar init --name "my-project"
```

The default database lives at `~/.planar/planar.db`. Override it with the `PLANAR_DB` environment variable; there is no global `--db` flag.

The host build and test queue lives in the same database: `queue_entries`, `queue_history` and `queue_schema` are tables of `planar.db`, so there is no second database to create or move. The `planar-agent queue` verbs open `planar.db` as an existing file and never create or migrate it, so run `planar init` first. Detached queue runs write their output files under `queue-logs/` beside the database file.

`planar.db` holds task claim tokens, which authorise heartbeats and terminal verbs on a claim, and the queue stores a submitter's token while its entry is live, so the database is private to your user. `install.sh` makes `~/.planar` mode `0700` and, on an existing install, tightens `planar.db` (with its `-wal`/`-shm` sidecars) to `0600`; re-running it is how an install that predates this is tightened. `queue-logs/` is created `0700` with `0600` logs. An install root shared by several users (a `--prefix` such as `/opt/planar`) is unsupported under the `0700` rule; use one install per user. If `install.sh` cannot change a mode it warns and continues. See [operations.md](docs/operations.md#5-the-host-build-and-test-queue) for the full rule.

<!-- retired-ref: agent.db upgrade note -->
### Upgrade note: unlinking `agent.db` under a live queue submitter

Removing `agent.db` (and its `-wal` and `-shm` sidecars) while an older
`planar-agent queue run` is still waiting or running does not stop that
process. It was measured on the vendored SQLite 3.53.3 (macOS, APFS), first
with a bare connection and then with two real `planar-agent queue run`
submitters at 595510e6 in a scratch home:

- The old process keeps reading and writing the unlinked WAL database through
  its open file descriptors. No statement fails, and no new file appears at
  the old path from that process.
- A waiting old submitter does not exit 125. Its polls succeed against the
  orphaned file, and it runs its command when the entry ahead of it ends.
- A `queue run` from the upgraded binaries uses `planar.db` and never opens
  the old path, so it does not join the old queue and does not see its
  entries. It takes a turn in the new queue at once, outside the old queue's
  slot count, and both queues are active until every old submitter drains. (A
  binary from before the upgrade, started after the unlink, opens a fresh
  database at the old path in the same way.)

So retire `agent.db` only when no old submitter is live, or accept that the
old queue and the new one run side by side until it drains.

### Upgrade note: what install.sh does with the old queue (plan 1089)

The queue now lives in `planar.db`. On an install over a prefix that still
holds the old `agent.db`, `install.sh` (through
`scripts/install-lib/queue-retire.sh`):

1. **Checks the old queue before building anything.**
   `python3 scripts/install-lib/queue_retire.py live` reads `agent.db`
   read-only and judges every entry the way the queue's own liveness does.
   An entry blocks when its submitter, or a running entry's command group, is
   provably alive on this boot, or when it cannot be proven dead (another pid
   namespace, an unreadable identity or start time, an out-of-range id). An
   entry written on an earlier boot of this machine, or on another machine,
   is dead. A store it cannot read blocks too. It refuses with the blocking
   entries and the remedies: let them finish, cancel them with the old
   `planar-agent queue cancel <seq>` if it is still installed, or re-run with
   `--ignore-live-queue`.
2. **Builds and installs the binaries.**
3. **Probes `planar.db`** with the new `planar-agent queue status 1 --json`,
   read-only and from `/`, and migrates it with
   `planar init --skip-project --allow-no-repo` only when it is behind. An
   ahead `planar.db` is never refused: if its queue tables are newer than
   this build, or differ under the same migration number, it warns and
   continues. Any other probe result stops the install before anything is
   retired. It never creates `planar.db`.
4. **Checks the old queue again**, immediately before removing anything.
5. **Retires `agent.db`**: it reads the old store's highest sequence number,
   refuses when it cannot or when that number reaches 1,000,000 (the new
   queue's floor), and otherwise removes `agent.db`, `agent.db-wal`,
   `agent.db-shm` and the old logs `queue-logs/<n>.log` numbered at or below
   it. Logs of the new queue (numbered above 1,000,000) and every other file
   are kept.

`--force` does not bypass steps 1 and 4; `--ignore-live-queue` turns their
refusal into a warning and nothing else. What it costs: the old queue is
orphaned. Its submitters keep running their commands outside the new queue's
slot count, and nothing reads the old database afterwards.

`--uninstall` runs the same live-queue guard first whenever `agent.db`
exists, with or without `--force`; `--ignore-live-queue` is the only override.
It needs `python3` for that and refuses without it; with no `agent.db`,
`python3` is not needed. A prefix that holds only an `agent.db` does not count
as a Planar install.

`PLANAR_AGENT_DB` was the variable that relocated the old database. It is no
longer read: a value left exported is ignored, never refused, and a store it
named is never touched by the installer. Delete it by hand once no old queue
command is running.
<!-- retired-ref: agent.db upgrade note -->

## Build from source

If you want to develop on Planar or contribute back:

The repo root IS the CMake project root (`CMakeLists.txt` and `CMakePresets.json` sit at the top level), so all build invocations run from the repo root:

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
cmake --preset debug                                    # or --preset release
cmake --build build/debug --target all planar_tests   # test binaries are not in `all`
ctest --test-dir build/debug --output-on-failure         # Catch2 unit tests
```

The repo also ships a `Makefile` with the common targets:

```bash
make build              # cmake --preset release; copies the 5 binaries and scriptorium into ./bin/
make test               # cmake --preset debug; cmake --build --target all planar_tests; ctest
make test-all           # unit (ctest) + registry check + coverage + cli-usage-check + surface/exit-code/eval contracts + cpp-lint-gate
make cpp-lint           # pinned clang-format --dry-run --Werror + clang-tidy + doxygen
```

On a machine where several agents build at once, send builds and tests through
the host queue instead of running them directly, for example `planar-agent
queue run --detach -- make test` (see [docs/testing.md](docs/testing.md)).

For dev-mode install where edits to the source repo are picked up live by the binary's siblings (skills, agents, commands) and vendor surfaces, use `./install.sh --link`. Note that binary edits still require a rebuild (`make build` or re-running `install.sh`).

**Developer-only note.** The Zig implementation this project ported from is GONE. It lived under `zig/` as the port's parity oracle and was deleted at the M10 cutover (task 6045) once decision 963/982's evidence conditions were met. Nothing here builds, tests, installs, or lints against it; its history is in `git log`.

For authoring or inspecting migrations:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
sqlx migrate add -r <name> --source migrations
# Edit migrations/NNNNN_<name>.up.sql and migrations/NNNNN_<name>.down.sql.
cmake --build build/debug          # cmake/generate_migrations.cmake re-runs configure
                                    # automatically and picks up the new files
```

(The runtime applies migrations automatically from a generated `planar.db.migrations` module, `#embed`-produced at CMake configure time by `cmake/generate_migrations.cmake`; the sqlx CLI is only needed for ad-hoc developer work against an out-of-band database or for authoring new migration pairs.)

To smoke a single migration as raw SQL against a scratch database:

```bash
sqlite3 /tmp/scratch.db < migrations/00001_foundation.up.sql
```

## Uninstall

```bash
cd /path/to/planar     # the source repo, where install.sh lives
./install.sh --uninstall
# or:
make uninstall-full
```

This removes:
- All vendor symlinks under `~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/`, `~/.gemini/antigravity-cli/skills/` that point into `~/.planar/`.
- Everything in `~/.planar/` *except* your data: `planar.db` (with its SQLite sidecars, `-wal` and `-shm`) and the `queue-logs/` directory of detached queue-run output. A retired old queue database left from before the upgrade is removed too; the upgrade note above covers the guard that protects a live old queue.

A prefix that holds only a preserved `planar.db` still counts as a Planar install: a later `--uninstall` or re-install accepts it without `--force`.

Only the default file names directly under `~/.planar/` are preserved. A database relocated with `PLANAR_DB` to a path outside `~/.planar/` is never touched by the uninstall; one relocated to another file name *inside* `~/.planar/` is not preserved, and a non-force uninstall deletes it.

To remove the database and the queue logs too:

```bash
./install.sh --uninstall --force
```

`make uninstall` is the counterpart of `make install`: it removes only the five Planar executables from `PREFIX/bin` (default `~/.local/bin`).

To remove only the data and keep the install:

```bash
rm -f ~/.planar/planar.db ~/.planar/planar.db-wal ~/.planar/planar.db-shm
rm -rf ~/.planar/queue-logs
```

## Troubleshooting

**`planar: command not found` after install.**

Check that the binary directory is on your `$PATH`:

```bash
echo "$PATH" | tr ':' '\n' | grep -E '\.planar/bin'
```

You want `~/.planar/bin` on PATH. Add to your shell rc as needed:

```bash
# .zshrc / .bashrc
export PATH="$HOME/.planar/bin:$PATH"

# fish — run once:
fish_add_path ~/.planar/bin
```

**A pinned LLVM tool is not found, or CMake fails to configure.**

`install.sh` preflights an exact LLVM toolchain path, and the CMake presets discover and validate one — see [docs/toolchain-parity.md](docs/toolchain-parity.md) for the pinned versions and prefixes. On macOS: `brew install cmake ninja llvm`. Verify the pinned compilers resolve:

```bash
/opt/homebrew/opt/llvm/bin/clang++ --version
cmake --version   # must be >= 4.3
```

On a non-Homebrew-ARM-macOS host, see `docs/toolchain-parity.md`'s "Platform prefixes" table for the equivalent path and pass it as `-DPLANAR_LLVM_PREFIX=<path>`.

**Slash commands not appearing in Claude Code.**

Verify the symlinks exist:

```bash
ls -la ~/.claude/commands/pl-*.md
```

Each one should point at `~/.planar/commands/claude/pl-*.md`. If they're missing, rerun:

```bash
./install.sh --force --vendors claude
```

Claude Code picks up new slash commands the next time you start a session or invoke any command.

**`install.sh` says a vendor surface already exists.**

By default, install.sh refuses to overwrite a destination symlink that points somewhere unexpected. Inspect:

```bash
readlink ~/.claude/commands/pl-orchestrator.md
```

If it's not what you want, rerun with `--force`:

```bash
./install.sh --force
```

**Database operations fail with "no such table".**

You probably skipped `planar init`. Run it once:

```bash
planar init --name "my-project"
```

**Conflicts on `sync pull` / `sync push`.**

By design — Planar never resolves operational-plane conflicts silently. Inspect the event and pick a side:

```bash
planar-ext sync status                      # see which links are in conflict
planar audit trail --link <link-id>         # see the sync_events log
planar-ext sync resolve <event-id> --keep local \
    --evidence-token <token> \
    --expected-local-updated-at <timestamp>  # or --keep remote
```

## Install layout reference

After a full install (`install.sh`), the layout under `~/.planar/` is:

```
~/.planar/
├── bin/
│   ├── planar                          # the C++26 operator binary
│   ├── planar-agent                    # agent-callable coordination binary
│   ├── planar-watch                    # human-facing read-only viewer
│   ├── planar-execute                  # deterministic spawn-free Lua workflow engine
│   ├── planar-ext                      # operational-plane binary (Jira, GitHub Issues)
│   └── scriptorium                     # in-tree skill and agent renderer
├── install-manifest.json               # versioned managed-projection authority
├── planar.db                           # SQLite database (after `planar init`; mode 0600)
├── queue-logs/                         # detached queue-run output (`<seq>.log`)
├── migrations/
│   ├── 00001_foundation.up.sql         # canonical migration sources, sqlx-cli format
│   ├── 00001_foundation.down.sql
│   ├── 00002_planning.up.sql
│   ├── 00002_planning.down.sql
│   └── … (through 00041_contextual_annotation_threads)
├── agents/                             # vendor-neutral agent role specs
│   ├── methodology.md
│   ├── models.md
│   ├── orchestrator.md
│   ├── coder.md
│   ├── reviewer.md
│   ├── planner.md
│   ├── ingestor.md
│   ├── ext-sync.md
│   ├── … (the remaining role specs and shared docs)
│   └── claude/ codex/ copilot/ gemini/ # rendered per-vendor agent files
├── commands/claude/                    # Claude slash-command sources
│   ├── pl-orchestrator.md
│   ├── pl-coder.md
│   ├── pl-reviewer.md
│   ├── pl-init.md
│   ├── pl-scope.md
│   ├── pl-plan.md
│   ├── pl-task.md
│   └── … (40 total per vendor)
├── skills/src/                         # unified authored skill sources
├── skills/codex/                       # rendered Codex skills (pl-*/SKILL.md, same 40 names)
├── codex-skills/                       # Codex runtime skill directories
│   ├── pl-orchestrator/
│   │   └── SKILL.md
│   └── … (40 total)
├── skills/copilot/                     # rendered Copilot skills (same 40 file names)
├── copilot-skills/                     # Copilot runtime skill directories
├── skills/gemini/                      # rendered Gemini skills (same 40 file names)
├── gemini-skills/                      # Gemini runtime skill directories
├── templates/                          # Operator-editable defaults
├── workflows/                          # Lua workflows staged from the repo
└── scripts/                            # Bash tooling staged from the repo
```

Symlinks the installer creates out of `~/.planar/`:

```
~/.claude/commands/pl-*.md       →  ~/.planar/commands/claude/pl-*.md      (40 links)
~/.codex/skills/pl-*/SKILL.md    real files copied from ~/.planar/codex-skills/pl-*/SKILL.md (40 skills)
~/.copilot/skills/pl-*/SKILL.md  real files copied from ~/.planar/copilot-skills/pl-*/SKILL.md (40 skills)
~/.gemini/antigravity-cli/skills/pl-*/SKILL.md  real files copied from ~/.planar/gemini-skills/pl-*/SKILL.md (40 skills)
```

The binary is **not** symlinked anywhere. Add `~/.planar/bin` to your `$PATH` (see [above](#full-install-installsh)).

## Environment variables

Planar respects these env vars when set:

| Variable | Purpose |
|----------|---------|
| `PLANAR_HOME` | Override the install root used by `install.sh` (defaults to `~/.planar`). The runtime binary does not consult this variable. |
| `CODEX_HOME` | Override the Codex home used by `install.sh` for Codex skills (defaults to `~/.codex`; skills install into `$CODEX_HOME/skills`). |
| `PLANAR_VENDOR` | Vendor identity recorded on every session and snapshot (e.g. `claude-code`, `codex`, `copilot`). Falls back to `cli` if unset. |
| `PLANAR_VENDOR_SESSION_ID` | Vendor's session id, recorded alongside the vendor name. Falls back to NULL if unset. |
| `PLANAR_WORKBENCH_ROOT` | Override the workbench drafting filesystem root (default `~/.planar/workbench/`). Useful for pointing multiple Planar instances at the same workbench directory. |
| `PLANAR_CONFIG_PATH` | Override the config file location (default `~/.planar/config.toml`). |
| `PLANAR_DB` | Override the database path (default `~/.planar/planar.db`). The host queue lives in this database, and detached queue-run output goes to `queue-logs/` next to this file. |
| `PLANAR_BIN` | Used by `scripts/coverage-check.sh` (`make coverage`) to point at a pre-built `planar` binary (default `./bin/planar`). |
| `JIRA_USER`, `JIRA_TOKEN`, etc. | Whatever you point `planar-ext ext register … --auth-env VAR_NAME` at. Comma-separated `USER,TOKEN` form uses HTTP Basic auth. |

Set them in your shell rc or per-command:

```bash
PLANAR_VENDOR=claude-code planar capture session --task 1
```

## Next steps

- [docs/getting-started.md](docs/getting-started.md) — a worked example of the full flow, from `planar init` through plans, tasks, capture, handoff, and resume.
- [docs/cli-reference.md](docs/cli-reference.md) — every command, flag, and exit code.
- [docs/architecture.md](docs/architecture.md) — system design: storage model, three operational context planes, schema contract, workbench, and configuration.
- [docs/workflows.md](docs/workflows.md) — end-to-end recipes: planning pipeline, bidirectional workbench, ext-sync propagation.
- [agents/methodology.md](agents/methodology.md) — how the agent roles collaborate; the phased orchestrator flow.
