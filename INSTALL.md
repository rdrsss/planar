# Installing Planar

This guide covers three install paths, ordered from simplest to most flexible:

1. [Quick install (`make install`)](#quick-install-make-install) — just the executables, no vendor surfaces.
2. [Full install (`install.sh`)](#full-install-installsh) — binary + the `planar` skill + the `planar-<role>` agents, placed into each vendor harness found (Claude Code / Codex / Copilot / Gemini CLI / Antigravity / OpenCode).
3. [Build from source](#build-from-source) — for contributors.

Plus [uninstall](#uninstall), [troubleshooting](#troubleshooting), and the [install layout reference](#install-layout-reference).

## Prerequisites

Planar shells out to a small set of external tools. On macOS, install them via Homebrew:

```bash
brew install cmake ninja llvm python git gh jq ripgrep
```

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries, on every install path. Both presets resolve the toolchain through `cmake/llvm-toolchain.cmake`, which discovers the prefix (an explicit `-DPLANAR_LLVM_PREFIX` first, then `brew --prefix llvm`, then apt.llvm.org's versioned prefixes and `PATH`) and refuses a candidate that lacks a modules-enabled `libc++`. `install.sh` additionally preflights the Homebrew paths `/opt/homebrew/opt/llvm/bin/clang` and `clang++` before invoking CMake. See [toolchain parity](docs/toolchain-parity.md) for the pinned versions and non-Homebrew-ARM-macOS resolution.
- `python3` — **required to configure**. The configure step registers Python test runners (`scripts/install-lib/queue_retire.test.py`, `scripts/queue-logs-after-reset.test.py`) and `find_package(Python3)` is `REQUIRED`. `install.sh` also runs it as its old-queue-database retirement reader (`scripts/install-lib/queue_retire.py`, standard library and `ctypes` only; it runs no other program), and `migrations/README.md`'s counter-reset recipe runs the log helper `scripts/queue-logs-after-reset.py` with it.
- No network access and no token are needed to configure or build: every dependency is committed under `vendor/` as a pinned release archive.
- `docker` — optional, developer-only. `make linux-gate` builds and tests the tree on Debian trixie in a container (see [docs/testing.md](docs/testing.md#the-linux-gate)). It is not an installer dependency.
- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree. The full install also needs it to clone the source repository.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — used by the bundled agent specs (`planar-planner`, and the procedures in `agents/methodology.md`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session in [docs/getting-started.md](docs/getting-started.md#8-capture-hand-off-and-resume) (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.

The full source-checkout installer also uses the base-system utilities declared
in `install.sh`'s `BUILD_DEPS` / `RUN_DEPS` manifests (`awk`, `basename`, `cat`, `chmod`, `cmp`, `diff`,
`cp`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mktemp`, `mv`,
`readlink`, `rm`, `rmdir`, `sort`, and `tr`) alongside CMake, Ninja, and the exact pinned LLVM compiler paths above. These ship with supported Unix-like systems;
the installer preflights them before making changes.

No system SQLite is needed. Planar vendors the SQLite amalgamation under `vendor/sqlite/`; the CMake build compiles it into a static library that statically links into every binary but `planar-execute` (which holds no SQLite handle at all) — no platform-specific build flags, no system library dependency.

### Optional / research tools

- `sqlx-cli` and `sqlite3` — only needed for ad-hoc developer workflows against a scratch database (see [Build from source](#build-from-source)); the runtime embeds migrations via build-time codegen and uses the vendored SQLite amalgamation, so neither CLI is a runtime dependency. Install the optional `sqlx-cli` for authoring new migration pairs:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
```

## Quick install (`make install`)

The shortest path. Builds and installs Planar's five executables into
`~/.local/bin` and nothing else.

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
local database. The `planar` skill and the role agents are **not** embedded —
they are source files that `install.sh` stages and places — so neither is wired
into any vendor harness by a `make install` or a `cmake --install` alone; for
those, use the [full install](#full-install-installsh).

The five binaries are installed. The four
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

This puts the five binaries in `~/.planar/bin/`; add that
directory to your `$PATH` and run `planar health` as above. For a debug build
instead, configure and build by hand:

```bash
cmake --preset debug
cmake --build build/debug
cmake --install build/debug --prefix ~/.local
```

## Full install (`install.sh`)

The recommended path. Installs the binary plus the canonical agent specs, the `planar` skill, workflow validation scripts, and migration sources into `~/.planar/`, then places the skill and the agents into each vendor harness found on the host (Claude Code, Codex, Copilot, Gemini CLI, Antigravity, OpenCode; see the vendor table under [Install layout](#install-layout-reference) and the presence rule there).

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
# or, equivalently:
make install-full       # extra flags via: make install-full INSTALL_FLAGS="--link --force"
```

That's it. The script:

- Builds all five binaries from source in its own build directory, `build/install-release/` (never the developer's `build/release/`), by running `cmake --preset release -B build/install-release -DPLANAR_VERSION_META=ON`, `cmake --build`, and `cmake --install … --prefix "$HOME/.planar"`, which writes `~/.planar/bin/{planar,planar-agent,planar-watch,planar-execute,planar-ext}`.
- Stages `skills/planar/` and `agents/*.md` into `~/.planar/skills/planar/` and `~/.planar/agents/`, and derives the Codex agent TOML files from `agents/` into `~/.planar/codex-agents/` (never under `agents/codex/`), before any vendor placement. The staged paths are recorded in the `extras` list of `install-manifest.json`.
- Stages `scripts/`, `workflows/`, `migrations/`, and `templates/` into `~/.planar/` (migrations are staged at `~/.planar/migrations/` for ad-hoc `sqlx` use; the binary embeds them at build time via codegen). It runs no renderer for the skill; the Codex agent TOML is derived by `scripts/render-codex-agents.py`.
- Places the staged skill and agents into each vendor whose presence marker exists (the nine targets in the [layout reference](#install-layout-reference)), and prints the vendors found and skipped.
- Atomically writes `~/.planar/install-manifest.json` after every placement
  succeeds. Each placed vendor path is recorded in its `extras` list;
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

The skill and the agents are now in place. Check the skill in any vendor that reads it by asking the harness to list its skills: `planar` appears once, and the `planar-<role>` agents appear in the harness's agent list. No slash commands are installed; role work goes to agents dispatched by name, for example `planar-orchestrator` or `planar-coder`. The first step in any session is `planar --help`; the [skill and agent reference](docs/skill-reference.md) describes what each piece does.

### Install flags

| Flag | Purpose |
|------|---------|
| `--prefix DIR` | Install root (default `~/.planar`). See [The install root guard](#the-install-root-guard). |
| `--vendors LIST` | Comma-separated filter over the vendors found on the host: `claude`, `codex`, `copilot`, `gemini`, `antigravity`, `opencode`. Default is all six. Naming an absent vendor warns. |
| `--no-vendor` | Skip vendor surfaces entirely; install Planar core only. |
| `--link` | Symlink the staged skill and the Markdown, Copilot and Codex agent files from the source repo instead of copying. **Dev mode** — edits to the repo propagate immediately. The OpenCode agents are always derived regular files. |
| `--force` | Overwrite existing symlinks at the destinations, and adopt a non-empty install root that carries no Planar sign. It does **not** override the install root guard for `$HOME`, `/` or an empty root, and does **not** bypass the live-queue guard on an old queue database (see the upgrade note below). |
| `--ignore-live-queue` | Retire (or uninstall) the old queue database even while its queue has live entries, or when `python3` cannot check it. The cost is an orphaned old queue; see the upgrade note below. It never bypasses the old-range checks of the retire step. |
| `--no-prune` | Skip removal of stale vendor files. |
| `--preset NAME` | CMake build preset: `debug`\|`release` (default `release`). |
| `--build-dir DIR` | Where to configure and build (default `build/install-<preset>`). |
| `--dry-run`, `-n` | Show the planned actions without changing anything. |
| `--verbose`, `-v` | Per-file detail (default prints a summary). |
| `--version` | Print the installer version and exit. |
| `--uninstall` | Tear down everything install.sh created. Preserves `~/.planar/planar.db` (with its `-wal`/`-shm` sidecars) and `~/.planar/queue-logs/` unless `--force` is also given. Removes the retired old queue database, behind the same live-queue guard as an install. |

### Copy mode vs link mode

- **Copy mode (default).** `install.sh` *copies* the source artifacts into `~/.planar/`, then copies the staged skill into each skill root as a real directory and each agent into each agent directory as a regular file. After install, the source checkout can be deleted; `~/.planar/` and the vendor directories are self-contained. Updates to the source require re-running `install.sh`.
- **Link mode (`--link`).** The skill directories and the Markdown, Copilot and Codex agent files placed into the vendor directories are *symlinks* into the staged tree under `~/.planar/`, which in turn is linked to the source repo, so source edits show up without reinstalling. Requires the source repo to stay on disk at its original path.
- **OpenCode agents** are derived from the Markdown source (frontmatter reduced to `description` and `mode: subagent`), so they are regular files in both modes.

The install manifest records each placed target's kind (link or copy), so health and uninstall know which one they are looking at. An existing destination that no Planar manifest records stops the install, naming the path.

### Initialize the database

After install, create the local database:

```bash
planar init --name "my-project"
```

The default database lives at `~/.planar/planar.db`. Override it with the `PLANAR_DB` environment variable; there is no global `--db` flag.

The host build and test queue lives in the same database: `queue_entries`, `queue_history` and `queue_schema` are tables of `planar.db`, so there is no second database to create or move. The `planar-agent queue` verbs open `planar.db` as an existing file and never create or migrate it, so run `planar init` first. Detached queue runs write their output files under `queue-logs/` beside the database file.

`planar.db` holds task claim tokens, which authorise heartbeats and terminal verbs on a claim, and the queue stores a submitter's token while its entry is live, so the database is private to your user. `install.sh` makes `~/.planar` mode `0700` and, on an existing install, tightens `planar.db` (with its `-wal`/`-shm` sidecars) to `0600`; re-running it is how an install that predates this is tightened. `queue-logs/` is created `0700` with `0600` logs. An install root shared by several users (a `--prefix` such as `/opt/planar`) is unsupported under the `0700` rule; use one install per user. If `install.sh` cannot change a mode it warns and continues. See [operations.md](docs/operations.md#5-the-host-build-and-test-queue) for the full rule.

### Upgrade note: the previous skill and agent projections

Earlier installs rendered a per-vendor skill and command set and linked or
copied it into the vendors' directories. `install.sh` now places one `planar`
skill and the `planar-<role>` agents instead, and under "Retiring the previous
skill and agent projections" it removes what the old layout left, before it
deletes the staged trees that prove the copies are Planar's. It removes only
what it can prove Planar made, and prints each removal:

- The slash-command symlinks in `~/.claude/commands/` that point into
  `~/.planar/` or `~/.planar/local/`.
- The unprefixed agent symlinks into `~/.planar/agents/<vendor>/`, in every
  vendor agent directory, dangling ones under `~/.config/opencode/agents/`
  included.
- The old per-skill directories (named with the retired skill prefix) under
  `~/.codex/skills`, `~/.copilot/skills`,
  `~/.gemini/antigravity-cli/skills` and `~/.config/opencode/skills` that are
  symlinks into `~/.planar/` or byte-identical to the staged
  `~/.planar/<vendor>-skills/<name>/`. A directory with no such proof is
  left and reported as "could not prove ownership".

It then removes the retired `~/.planar` paths listed in `install-cleanup.txt`
(`commands/`, `skills/<vendor>/`, `<vendor>-skills/` including
`opencode-skills/`, `agents/<vendor>/`). The retired renderer binary under `bin/` is listed too, so an install over an older tree removes it.

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
make build              # cmake --preset release; copies the 5 binaries into ./bin/
make test               # cmake --preset debug; cmake --build --target all planar_tests; ctest
make test-all           # unit (ctest) + registry check + coverage + cli-usage-check + surface/exit-code/eval contracts + cpp-lint-gate
make cpp-lint           # pinned clang-format --dry-run --Werror + clang-tidy + doxygen
```

On a machine where several agents build at once, send builds and tests through
the host queue instead of running them directly, for example `planar-agent
queue run --detach -- make test` (see [docs/testing.md](docs/testing.md)).

For dev-mode install where edits to the source repo are picked up live by the installed skill and agents, use `./install.sh --link`. Note that binary edits still require a rebuild (`make build` or re-running `install.sh`).

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
- Every target `install.sh` placed and recorded in `~/.planar/install-manifest.json` (the `planar` skill under `~/.claude/skills`, `~/.agents/skills` and `~/.gemini/antigravity-cli/skills`, and the `planar-<role>` agents under the six agent directories), while it is still what Planar placed: a symlink into `~/.planar/` or the staged bytes. A recorded path someone replaced, and any other `planar` or `planar-*` entry there, is left and reported. No vendor directory and not `~/.agents/skills` itself is removed.
- Everything in `~/.planar/` *except* your data: `planar.db` (with its SQLite sidecars, `-wal` and `-shm`) and the `queue-logs/` directory of detached queue-run output. A retired old queue database left from before the upgrade is removed too; the upgrade note above covers the guard that protects a live old queue.

A prefix that holds only a preserved `planar.db` still counts as a Planar install: a later `--uninstall` or re-install accepts it without `--force`.

### The install root guard

`install.sh` and `install.sh --uninstall` run one guard, `scripts/install-lib/prefix-guard.sh`, on the install root (`--prefix`, else `PLANAR_HOME`, else `~/.planar`) before anything is removed, created or built:

- A root that is the empty string (`--prefix ""` or `PLANAR_HOME=`), `/`, or resolves to `$HOME` exits **2**, naming the path and the rule. The path is canonicalised first, so `$HOME/.`, `$HOME/../<user>`, a trailing slash and a symlink to `$HOME` or `/` are all caught. `--force` makes no difference.
- An existing non-empty root is accepted without `--force` when it carries the install stamp (`.planar-install`), an executable `bin/planar`, `planar.db`, or a valid installer recovery journal (`.planar-journal`, so an interrupted first install is adopted). Installs made before the stamp existed carry `bin/planar` or `planar.db`. A bare `.staging-*` directory or any other marker file is not evidence.
- Any other non-empty root exits **1** naming the path, unless `--force` adopts it.

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

**An agent sees `DatabaseUnreadable`, or `QueryFailed` with "read-only to this process".**

Every Planar binary needs read and write access to `~/.planar/planar.db` and to
the `~/.planar/` folder itself. SQLite keeps its `-wal` and `-shm` files beside
the database and creates them even for a reader, so a process that may read the
file but not write in the folder cannot open it. An agent harness that runs
commands in a sandbox must allow writes under `~/.planar/` (or under the folder
of `$PLANAR_DB`). The database is not damaged and needs no `planar init`; check
from an unsandboxed terminal with `planar health`.

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

**The `planar` skill or the agents do not appear in a harness.**

Check the manifest and the placed targets. `planar health` compares every recorded target with the staged copy and names a missing one; a vendor whose presence marker was absent at install time was skipped, and the installer's summary named it. Install the vendor's harness, then rerun for just that vendor:

```bash
planar health
ls -la ~/.claude/skills/planar ~/.claude/agents/planar-*.md
./install.sh --vendors claude
```

Harnesses read skills and agents when a session starts, so start a new session after installing. The nine targets are in the [layout reference](#install-layout-reference).

**`install.sh` says a destination already exists.**

By default, install.sh refuses to overwrite a destination that no Planar install manifest records, and names the path. Inspect it:

```bash
ls -la ~/.claude/skills/planar
```

If it is yours and you want it gone, move it aside and rerun `./install.sh`. `--uninstall` likewise leaves a recorded path that someone replaced, and reports it.

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
│   └── planar-ext                      # operational-plane binary (Jira, GitHub Issues)
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
│   ├── planar-orchestrator.md
│   ├── planar-coder.md
│   ├── planar-reviewer.md
│   ├── planar-planner.md
│   ├── planar-ingestor.md
│   ├── planar-ext-sync.md
│   └── … (the remaining role specs and shared docs)
├── skills/planar/                      # staged planar skill (SKILL.md + references/)
├── codex-agents/                       # Codex custom agents (planar-*.toml) derived from agents/
├── templates/                          # Operator-editable defaults
├── workflows/                          # Lua workflows staged from the repo
└── scripts/                            # Bash tooling staged from the repo
```

Vendor targets the installer places into, only for a vendor whose presence
marker exists (six vendors, nine targets):

| Vendor | Presence marker | Skill | Agents |
|--------|-----------------|-------|--------|
| Claude Code | `~/.claude/` | `~/.claude/skills/planar` | `~/.claude/agents/planar-<role>.md` |
| Codex | `$CODEX_HOME` set, else `~/.codex/` | `~/.agents/skills/planar` (shared) | `$CODEX_HOME/agents/planar-<role>.toml` (default `~/.codex`) |
| Copilot | `~/.copilot/` | `~/.agents/skills/planar` (shared) | `~/.copilot/agents/planar-<role>.agent.md` |
| Gemini CLI | `~/.gemini/settings.json` | `~/.agents/skills/planar` (shared) | `~/.gemini/agents/planar-<role>.md` |
| Antigravity | `~/.gemini/antigravity-cli/` | `~/.gemini/antigravity-cli/skills/planar` | `~/.gemini/antigravity-cli/agents/planar-<role>.md` |
| OpenCode | `~/.config/opencode/` | none: it reads `~/.agents/skills` and `~/.claude/skills` | `~/.config/opencode/agents/planar-<role>.md`, frontmatter reduced to `description` and `mode: subagent` |

The shared `~/.agents/skills/planar` is placed once when any of Codex, Copilot,
Gemini CLI or OpenCode is present. Gemini CLI and Antigravity are detected
independently. Skills and Markdown agents are symlinks into `~/.planar/` with
`--link` and copies otherwise; the OpenCode agents are derived files, so they are
always copied. The installer prints the vendors it found and the ones it skipped.
A destination that exists and that no Planar manifest records stops the install,
naming the path.

The binary is **not** symlinked anywhere. Add `~/.planar/bin` to your `$PATH` (see [above](#full-install-installsh)).

## Environment variables

Planar respects these env vars when set:

| Variable | Purpose |
|----------|---------|
| `PLANAR_HOME` | Override the install root used by `install.sh` (defaults to `~/.planar`). The runtime binary does not consult this variable. |
| `CODEX_HOME` | Override the Codex home used by `install.sh`: its presence marker, and the Codex agent directory `$CODEX_HOME/agents` (defaults to `~/.codex`). Codex skills go to the shared `~/.agents/skills`, not under `$CODEX_HOME`. |
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
