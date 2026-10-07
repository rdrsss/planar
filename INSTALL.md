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

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries, on a source install. A [prebuilt install](#prebuilt-install---prebuilt) needs none of them. Both presets resolve the toolchain through `cmake/llvm-toolchain.cmake`, which discovers the prefix (an explicit `-DPLANAR_LLVM_PREFIX` first, then `brew --prefix llvm`, then apt.llvm.org's versioned prefixes and `PATH`) and refuses a candidate that lacks a modules-enabled `libc++`. `install.sh` additionally preflights the Homebrew paths `/opt/homebrew/opt/llvm/bin/clang` and `clang++` before invoking CMake. See [toolchain parity](docs/toolchain-parity.md) for the pinned versions and non-Homebrew-ARM-macOS resolution.
- `python3` — required to build from source, not to install a release bundle. The configure step registers Python test runners (`scripts/install-lib/queue_probe.test.py`, `scripts/install-lib/queue_retire.test.py` and `scripts/queue-logs-after-reset.test.py`) and `find_package(Python3)` is `REQUIRED`. A source `install.sh` also runs it for two things: the old-queue-database retirement reader (`scripts/install-lib/queue_retire.py`, standard library and `ctypes` only; it runs no other program) and the Codex agent TOML renderer (`scripts/render-codex-agents.py`). The queue-store probe of `planar.db` (`planar-agent queue status 1 --json`) is classified in shell on every install path and needs no Python. `migrations/README.md`'s counter-reset recipe runs the log helper `scripts/queue-logs-after-reset.py` with it.
- `bash` — `install.sh`, `planar-uninstall` and the scripts they source run under the `/bin/bash` 3.2 that stock macOS ships, with `set -u`; Homebrew bash is not needed. Possibly empty arrays are expanded as `${a[@]+"${a[@]}"}`, and no bash-4-only construct is used. `scripts/install-bash32-test.sh` (ctest label `install_bash32`) lints every sourced script for an unguarded expansion (a provably non-empty array carries `# bash32: nonempty`) and runs a prebuilt install and an uninstall under `/bin/bash`.
- No network access and no token are needed to configure or build: every dependency is committed under `vendor/` as a pinned release archive.
- `docker` — optional, developer-only. `make linux-gate` builds and tests the tree on Debian trixie in a container (see [docs/testing.md](docs/testing.md#the-linux-gate)). It is not an installer dependency.
- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree. The full install also needs it to clone the source repository.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — used by the bundled agent specs (`planar-planner`, and the procedures in `agents/methodology.md`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session in [docs/getting-started.md](docs/getting-started.md#8-capture-hand-off-and-resume) (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.

`install.sh` declares its tools in three tiers in its `TOOLCHAIN_DEPS`, `BASE_DEPS` and `RUN_DEPS` manifests:

- The **toolchain tier** (`cmake`, `ninja`, the pinned LLVM compilers and `python3`, above) is checked only on a source install.
- The **base tier** is checked on every install path, `--prebuilt` included: `awk`, `basename`, `cat`, `chmod`, `cmp`, `cp`, `cut`, `date`, `diff`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mktemp`, `mv`, `od`, `ps`, `readlink`, `realpath`, `rm`, `rmdir`, `sed`, `sleep`, `sort`, `sync`, `tr`, `uname` and `wc`. These ship with supported Unix-like systems; the installer preflights them before making changes.
- The **runtime tier** (`git`, `jq`, `gh`, `rg`) only warns.

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

- Takes the installation's [mutation lock](#ownership-recovery-and-the-order-of-an-install) and records a recovery journal, so a second install, update or uninstall of the same root is refused and an interrupted run can be completed by running it again.
- Builds all five binaries from source in its own build directory, `build/install-release/` (never the developer's `build/release/`), by running `cmake --preset release -B build/install-release -DPLANAR_VERSION_META=ON`, `cmake --build`, and `cmake --install … --prefix ~/.planar/.staging-<token>/.cmake-install`, and takes `bin/{planar,planar-agent,planar-watch,planar-execute,planar-ext}` from there.
- Stages `skills/planar/` and `agents/*.md`, derives the Codex agent TOML files from `agents/` into `codex-agents/` (never under `agents/codex/`), and stages `scripts/`, `workflows/` and `migrations/` (for ad-hoc `sqlx` use; the binary embeds them at build time via codegen), all under the same `~/.planar/.staging-<token>/`. It runs no renderer for the skill; the Codex agent TOML is derived by `scripts/render-codex-agents.py`. The staged paths are recorded in the `extras` list of `install-manifest.json`.
- Probes the database with the staged binaries, then swaps `bin/`, `skills/`, `agents/`, `codex-agents/`, `workflows/`, `scripts/` and `migrations/` into `~/.planar/` whole, and creates a missing database or migrates a behind one with the installed `planar init --skip-project --allow-no-repo`. `templates/` is placed missing-only.
- Places the staged skill and agents into each vendor whose presence marker exists (the nine targets in the [layout reference](#install-layout-reference)), and prints the vendors found and skipped.
- Writes `~/.planar/release.json`, the record of which release is installed. A source install writes it with `version` set to the sixth token of `planar version` (the release tag, `dev` for a build without one), so a source install is never mistaken for a release; a prebuilt install copies the bundle's file.
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
| `--prebuilt DIR` | Install an unpacked release bundle instead of building: see [Prebuilt install](#prebuilt-install---prebuilt). Refused together with `--link` (exit 2). |
| `--link` | Symlink the staged skill and the Markdown, Copilot and Codex agent files from the source repo instead of copying. **Dev mode** — edits to the repo propagate immediately. The OpenCode agents are always derived regular files. |
| `--force` | Overwrite existing symlinks at the destinations (never a preserved path: shipped `templates/` files are still placed only where missing), and adopt a non-empty install root that carries no Planar sign. It does **not** override the install root guard for `$HOME`, `/` or an empty root, and does **not** bypass the live-queue guard on an old queue database (see the upgrade note below). |
| `--ignore-live-queue` | Retire the old queue database even while its queue has live entries, or when `python3` cannot check it. The cost is an orphaned old queue; see the upgrade note below. It never bypasses the old-range checks of the retire step. |
| `--no-prune` | Skip removal of stale vendor files. |
| `--preset NAME` | CMake build preset: `debug`\|`release` (default `release`). |
| `--build-dir DIR` | Where to configure and build (default `build/install-<preset>`). |
| `--dry-run`, `-n` | Show the planned actions without changing anything. |
| `--verbose`, `-v` | Per-file detail (default prints a summary). |
| `--version` | Print the installer version and exit. |
| `--cleanup DIR` | Updater only: remove the updater's temporary directory when the install ends. Accepted only with `--prebuilt` and the updater's handoff; see [the handoff](#the-update-handoff-and---cleanup). |
| `--uninstall` | Run the standalone uninstaller, `planar-uninstall` (the bundle's `uninstall.sh`, or the checkout's `scripts/uninstall.sh`); see [Uninstall](#uninstall). `--prefix`, `--purge` and `--yes` are passed on; every other install option is ignored. `--uninstall --force` exits 2 naming `--purge`, and `--uninstall --dry-run` exits 64. |
| `--purge` | With `--uninstall` only: remove the [preserved paths](#preserved-paths) too, except a relocated one. |
| `--yes`, `-y` | With `--uninstall` only: remove Planar binaries left in `~/.local/bin` without asking. |

### Prebuilt install (`--prebuilt`)

`install.sh --prebuilt <dir>` installs a release bundle that was unpacked into `<dir>` (the layout is `scripts/dist.sh`'s: `bin/`, `skills/planar/`, `agents/`, `codex-agents/`, `templates/`, `workflows/`, `migrations/`, `scripts/install-lib/`, `install.sh`, `uninstall.sh`, `install-cleanup.txt` and `release.json`). Nothing is built, so only the [base tier](#prerequisites) of tools is needed: no CMake, Ninja, LLVM or `python3`.

```bash
./install.sh --prebuilt /path/to/planar-macos-arm64
```

It is the same installer as the source path from the build onward: the vendor sweep, the `~/.planar` cleanup, the queue store, staging, the vendor table, placement and the manifest. The differences:

- **Inputs.** `bin/` is copied from `<dir>/bin/` and every staged input is read from `<dir>`. `codex-agents/` is copied from the bundle, where it was rendered when the bundle was cut, and `release.json` is copied from the bundle.
- **A bundle must be whole.** A directory missing any of the five binaries (or `release.json`, `uninstall.sh`, the skill or `codex-agents/`) is refused at exit 1, naming what is missing, before anything is created: `~/.planar` does not exist afterwards.
- **`--link` is refused.** `--link --prebuilt` exits 2 naming both flags, again before anything is created, because link mode would symlink into a directory the caller deletes after the install.
- **The old queue database is moved aside, never read.** There is no `python3` on this path, so the live-queue check and the retirement reader do not run. After the `planar.db` probe passes, the retired queue database, its sidecars and the old numbered queue logs are moved into `~/.planar/retired/<YYYY-MM-DD>/` and each move is printed. Nothing is deleted, and nothing already in `retired/` is replaced. The [upgrade note on the old queue](#upgrade-note-what-installsh-does-with-the-old-queue-plan-1089) has the exact rules.
- **The probe is shared.** The database probe that stands in front of that move is the same as on the source path (see [the order of an install](#ownership-recovery-and-the-order-of-an-install)): a queue answer that is not exactly one JSON object, or an error tag outside the `error` object, refuses the install before anything changes, with the old queue database left in place.
- **The updater's handoff.** `planar update` hands its lock to the bundled installer with `--cleanup <dir>`; see [the handoff](#the-update-handoff-and---cleanup).

### Ownership, recovery and the order of an install

Every install, update and uninstall of one installation takes the same lock
first, and every install records what it is doing in a journal, so an
interrupted install is completed by running the same command again, never by
`--force` (tech spec 677, "Order of an install"; decisions 1324-1328).

**The mutation lock.** The lock lives *beside* the install root, at
`<root>.lock` (`~/.planar.lock` for the default root), keyed by the root's
canonical path, so no spelling of the root, and no removal or `--purge` of it,
can split it. It holds small ownership records naming the operation, the
process id and that process's start time. A second install, update or
uninstall exits 1 naming the holder's operation and pid and changes nothing. A
holder that was killed is reclaimed by the next run; a pid that was reused by
another process is recognized from its start time. When ownership cannot be
judged (a malformed record, another host's record, a start time that cannot be
read), the run refuses and keeps every file. It also refuses a `<root>.lock`
that is a symlink, is not owned by you or is writable by its group or by
others, and a filesystem where a record cannot be hard-linked, naming the
reason. Nothing removes `<root>.lock`;
remove it by hand only when no Planar install, update or uninstall can be
running. The protocol is specified in the header of
`scripts/install-lib/mutation-lock.sh`.

**The order of an install.**

1. Resolve the root and the relocated data paths, apply the
   [install root guard](#the-install-root-guard), check the tools, and take the
   lock.
2. Read `~/.planar/.planar-journal` when it exists. A journal that does not
   validate stops the install and is kept. A **prepared** or
   **aborted-before-mutation** attempt never changed anything: its staging is
   removed and the install starts over. A **complete** one only left backups
   and staging: they are removed. A **mutating** one is resumed when this run
   installs the same release (the same bundle version, commit and schema, or
   the same checkout and commit); a different release refuses, naming the
   command that completes the pending one. An **uninstalling** journal refuses:
   an interrupted uninstall is finished by uninstalling, never undone.
3. When resuming, a subtree whose live name is missing is restored from its
   journal-owned `<name>.old` before anything new is staged. `.staging-*` and
   `*.old` entries no journal owns are reported and kept.
4. Stage everything under an exclusively created `.staging-<token>/`, recorded
   in the journal first (phase **prepared**).
5. Probe the database with the **staged** binaries, read-only and from `/`:
   - missing: it is created after the swap; no project is registered;
   - behind: it is migrated after the swap;
   - current: nothing to do;
   - ahead: the install exits 1 naming both schema versions, and nothing
     changes (an older release is never installed over a newer database);
   - unreadable, corrupt, a foreign or incompatible queue schema at the same
     version, or an answer the probe cannot classify: the install exits 1 with
     the probe's diagnostic, before any vendor cleanup or swap.
   The main schema is judged by `planar-watch`'s read-only startup check; the
   queue tables never decide it.
6. Name running and waiting host-queue entries with the still-installed
   `planar-watch queue --json`: a running command keeps its old binary, and a
   claim lease renewed by `queue run --claim` can lapse while the database is
   migrated. Without a working `planar-watch` it prints one line saying the
   queue could not be checked. It never blocks.
7. Record **mutating** (flushed to disk with `sync`), then run the vendor sweep and `install-cleanup.txt`
   (never a data path, never through a symlink), then swap each managed
   subtree: the live `<name>` is renamed to `<name>.old`, the staged one to
   `<name>`, the journal updated around each rename.
   The managed subtrees are `bin/`, `skills/`, `agents/`, `codex-agents/`,
   `workflows/`, `scripts/` and `migrations/`; the installer owns each outright
   and replaces it whole. A subtree that is a symlink (a `--link` install) is
   renamed aside, never followed, so a checkout behind it is never touched, and
   switching between link and copy mode replaces the subtree's form. Every
   managed subtree is always shipped: a prebuilt bundle or a checkout that lacks
   one is refused before anything is written, naming the missing path, never
   treated as a retirement. `commands/` and `copilot/` are retired paths removed
   through `install-cleanup.txt`.
   Shipped `templates/` files are placed only where missing, `--force`
   included; a template an operator edited, or one an older release shipped
   under another name, is never overwritten or removed, even when
   `install-cleanup.txt` lists it.
8. Create or migrate the database with the installed
   `planar init --skip-project --allow-no-repo` (against `PLANAR_DB` and
   `PLANAR_CONFIG_PATH` when they are set), and require a current probe.
9. Retire the old queue store, place the vendor surfaces and write the
   manifest, then `release.json`, then the install stamp last. Record
   **complete** (flushed with `sync`), remove the backups and the staging, and
   remove the journal.

**When it fails.** A failure before step 7 removes only that attempt's staging
and journal: the previous install, its stamp and `release.json` are untouched.
A failure after it (a failed migration, say) exits non-zero, keeps the
journal, the `*.old` backups and every data path, says the binaries may
already have changed, and prints the command that completes the install: for a
release, the version-pinned bootstrap
(`curl -fsSL <base>/download/<tag>/get-planar.sh | PLANAR_VERSION=<tag> sh`);
for a source install, `cd <checkout> && ./install.sh <the same flags>`. Binaries
are never rolled back across a schema change. Fix the cause (a continuing
fault, such as an unreadable database or a full disk, keeps failing), then run
that command.

### The update handoff and `--cleanup`

`planar update` (plan 1122 M3) downloads a bundle into an exclusively created
directory `~/.planar/.planar-update/<name>/`, records it in its ownership
record, and `exec`s the bundled `install.sh --prebuilt <dir> --cleanup <dir>`
with `PLANAR_MUTATION_HANDOFF=<generation>:<nonce>`. The installer adopts the
lock only when that record is the current owner, belongs to the updater, and
names the installer's own process (exec keeps the pid and start time), and only
once; a forged or replayed handoff exits 1. `--cleanup` is accepted only with
`--prebuilt` (exit 2 otherwise) and the handoff, and only for exactly the
directory the updater recorded: the install root, a data path, a managed
subtree, a symlink, a path through a symlink or an unrelated directory exits 1
and nothing is removed. The directory is removed when the installer exits,
after success or failure; after a KILL, the next owner removes the killed
updater's recorded directory and nothing else under `.planar-update/`.

### Copy mode vs link mode

- **Copy mode (default).** `install.sh` *copies* the source artifacts into `~/.planar/`, then copies the staged skill into each skill root as a real directory and each agent into each agent directory as a regular file. After install, the source checkout can be deleted; `~/.planar/` and the vendor directories are self-contained. Updates to the source require re-running `install.sh`.
- **Link mode (`--link`).** The skill directories and the Markdown, Copilot and Codex agent files placed into the vendor directories are *symlinks* into the staged tree under `~/.planar/`, which in turn is linked to the source repo, so source edits show up without reinstalling. Requires the source repo to stay on disk at its original path.
- **OpenCode agents** are derived from the Markdown source (frontmatter reduced to `description` and `mode: subagent`), so they are regular files in both modes.

The install manifest records each placed target's kind (link or copy), so health and uninstall know which one they are looking at. An existing destination that no Planar manifest records stops the install, naming the path.

### Initialize the database

The install creates the database when it is missing (with
`planar init --skip-project --allow-no-repo`, which registers no project) and
migrates it when it is behind. Register a project from its checkout:

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
(`commands/`, `copilot/`, `skills/<vendor>/`, `<vendor>-skills/` including
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
2. **Builds and stages the binaries.**
3. **Probes `planar.db`** with the staged binaries and, after the swap,
   creates or migrates it, as [the order of an install](#ownership-recovery-and-the-order-of-an-install)
   describes. An ahead `planar.db`, or one whose queue tables are foreign or
   incompatible at the same migration number, refuses the install before
   anything changes (decision 1326 replaces the earlier warn-and-continue).
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

**On a prebuilt install** (`install.sh --prebuilt`) steps 1, 4 and 5 differ,
because there is no `python3` to read the old store. Step 3 runs unchanged,
with the same shell classifier. Instead of the live-queue checks and the
retirement, `agent.db`, `agent.db-wal`, `agent.db-shm` and the old numbered
logs (`queue-logs/<n>.log`, `n` below 1,000,000) are moved, unread, into
`~/.planar/retired/<YYYY-MM-DD>/` (the logs under `queue-logs/` there), and
each move is printed. An existing `retired/<date>/` is reused and nothing in it
is overwritten. The database and its sidecars keep SQLite's pairing: when any
of the three names is taken, all three move under the first free shared name
(`agent.db.1`, `agent.db.1-wal`, `agent.db.1-shm`, then `agent.db.2`, ...); a
taken log name gets the first free suffix. `retired/` is a preserved path, so no later install or
uninstall removes it; only `planar-uninstall --purge` does, naming each file first. An old submitter still running keeps using the moved
file, as in the note above, so there is no `--ignore-live-queue` decision to
make.

The uninstaller never reads `agent.db` (it needs no `python3`): an uninstall
keeps `agent.db` and its sidecars and names them, and `--purge` removes them
after naming them. A prefix that holds only an `agent.db` does not count as a
Planar install.

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
~/.planar/bin/planar-uninstall            # keep the data paths
~/.planar/bin/planar-uninstall --purge    # remove them too
# equivalently, from a checkout or an unpacked bundle:
./install.sh --uninstall [--purge] [--yes]
```

`planar-uninstall` is `scripts/uninstall.sh`, installed into `~/.planar/bin/`
by every install and shipped at the root of every release bundle as
`uninstall.sh`. It is a standalone bash script that needs only the
[base tools](#prerequisites) (no `python3`, and no working Planar binary), so it
works when the binaries are broken, schema-locked or already gone.
`./install.sh --uninstall` runs it. Options: `--prefix DIR` (the install root;
default `PLANAR_HOME`, else `~/.planar`), `--purge`, `--yes`. There is no
`--force`: it exits 2 naming `--purge`, and nothing is changed.

It removes, printing each removal:
- Every vendor path recorded in `~/.planar/install-manifest.json` (the `planar` skill under `~/.claude/skills`, `~/.agents/skills` and `~/.gemini/antigravity-cli/skills`, and the `planar-<role>` agents under the six agent directories), each only while it is still what Planar placed: a symlink to the staged entry or into `~/.planar/`, or the staged bytes (an OpenCode agent: its derived form). A recorded path someone replaced, and any other `planar` or `planar-*` entry there, is left and reported. No vendor directory and not `~/.agents/skills` itself is removed. The manifest is read with `sed`, one projection per line; a line it cannot read is reported by its line number and its target is left in place. **Without a manifest, or with a version 1 manifest, nothing under the vendor directories is removed**, and it says so.
- The managed subtrees (`bin/`, `skills/`, `agents/`, `codex-agents/`, `workflows/`, `scripts/`, `migrations/`), never following a symlink: a link-mode subtree is unlinked and the checkout behind it is untouched. Also the paths `install-cleanup.txt` lists as retired, and `install-cleanup.txt`, `release.json`, the install stamp and the manifest.
- The staging directories and `<name>.old` backups that an interrupted install's journal owns, and an abandoned `planar update`'s recorded download directory.

It keeps, and names:
- Every [preserved path](#preserved-paths): `planar.db` with its `-wal` and `-shm` sidecars, `queue-logs/`, `retired/` (old queue databases and logs), `workbench/`, `config.toml`, `local/`, `workspaces/`, `models/`, `execute/` and every file in `templates/`. A relocated one is named and left where it is.
- A legacy `agent.db` and its sidecars, which it never reads.
- Every other entry it does not know, including `.staging-*` and `.old` entries no journal owns: reported, never removed.

**`--purge`** also removes the preserved paths under the install root and the legacy `agent.db`, naming each first (the files under `retired/` one by one), then the install root when nothing is left in it. A data path relocated by `PLANAR_DB`, `PLANAR_CONFIG_PATH`, `PLANAR_WORKBENCH_ROOT`, `PLANAR_LOCAL_HOME` or `PLANAR_TEMPLATES_DIR` is named and left where it is, and nothing outside the install root is removed. Unknown entries are still kept.

**`~/.local/bin`.** When `~/.local/bin` holds `planar` or a sibling binary (`planar-agent`, `planar-watch`, `planar-execute`, `planar-ext`) left by the retired `make install`, the uninstaller names them and asks whether to remove them; `--yes` removes them without asking. With no terminal on standard input it asks nothing, leaves them and says to re-run with `--yes`.

**Ownership and interruption.** The uninstall takes the same [mutation lock](#ownership-recovery-and-the-order-of-an-install) as an install, `--purge` included: a running install, update or uninstall makes it exit 1 naming the holder, before anything is removed, and it holds the lock until every removal has finished. The lock directory beside the root (`~/.planar.lock`) is never removed. A recovery journal that does not validate stops it, kept. Before its first removal it records **uninstalling** in `~/.planar/.planar-journal` and flushes it to disk, which ends any interrupted install for good: no later install resumes or restores it, and an install refuses while that journal exists. It also prints the durable retry command:

- for a release install, the matching release's bundled uninstaller, downloaded afresh (the installed copy may already be gone):
  `d="$(mktemp -d)" && curl -fsSL https://github.com/rdrsss/planar/releases/download/<tag>/planar-<os>-<arch>.tar.gz | tar -xzf - -C "$d" && bash "$d/planar-<os>-<arch>/uninstall.sh"` (with `--prefix`, `--purge` and `--yes` repeated, and `PLANAR_RELEASE_URL` honoured);
- for a source install run from a checkout, `cd <checkout> && ./install.sh --uninstall`;
- otherwise the bundle's `uninstall.sh` it ran from.

Running that command, or `planar-uninstall` again while it exists, finishes the removals; it never brings binaries back. The journal is removed last. When the uninstall keeps the install root (preserved data, or entries it does not know), it writes `~/.planar/.planar-uninstalled`, a two-line record naming the root, so the next install into that root, `--prefix` included, needs no `--force`; that install removes it. A fresh install after a finished uninstall proceeds normally, with a relocated `PLANAR_DB` still the database it uses.

### The install root guard

`install.sh` and `planar-uninstall` (also as `install.sh --uninstall`) run one guard, `scripts/install-lib/prefix-guard.sh`, on the install root (`--prefix`, else `PLANAR_HOME`, else `~/.planar`) before anything is removed, created or built:

- A root that is the empty string (`--prefix ""` or `PLANAR_HOME=`), `/`, or resolves to `$HOME` exits **2**, naming the path and the rule. The path is canonicalised first, so `$HOME/.`, `$HOME/../<user>`, a trailing slash and a symlink to `$HOME` or `/` are all caught. `--force` makes no difference.
- An existing non-empty root is accepted without `--force` when it carries the install stamp (`.planar-install`), an executable `bin/planar`, `planar.db`, a valid installer recovery journal (`.planar-journal`, so an interrupted first install is adopted), a valid uninstall record (`.planar-uninstalled`, which a finished uninstall writes into a root it keeps and which names that root, so a copy from another root does not count), or, only when the root is `~/.planar` itself however it is spelled (a symlink to it, or a case variant on a case-insensitive volume), any [preserved path](#preserved-paths) (what an uninstall leaves behind once it has removed the stamp and `bin/`; it counts with or without unknown files beside it, while unknown files alone never do). In any other directory a preserved-path name such as `models/` or `templates/` is not evidence, so a project that holds one is refused without `--force`. Installs made before the stamp existed carry `bin/planar` or `planar.db`. A bare `.staging-*` directory or any other marker file is not evidence.
- Any other non-empty root exits **1** naming the path, unless `--force` adopts it.
- The root's parent directory must be writable, even when the root already exists: the [mutation lock](#ownership-recovery-and-the-order-of-an-install) is created beside the root as `<root>.lock` (for `--prefix /opt/planar`, `/opt/planar.lock`). When it cannot be created the run exits **1** before any change with `cannot create the mutation lock directory <root>.lock (is <parent> writable?)`. Create `<root>.lock` yourself (`mkdir -m 700`, owned by you) if the parent must stay read-only.

### Preserved paths

The data paths are listed once, in `scripts/install-lib/data-paths.sh`, which `install.sh` sources. The installer never removes one: a re-install, an `install-cleanup.txt` entry and a managed-tree refresh all skip them, and a cleanup entry that names one is skipped with a note. Two writes are allowed: a `planar.db` that is behind the binary is migrated forward, and shipped files are placed into `templates/` only where missing, `--force` included, so an edited template is never overwritten. `planar-uninstall` (and `install.sh --uninstall`) preserves them too and names each; only `planar-uninstall --purge` removes them, and never a relocated one (see [Uninstall](#uninstall)). You can also remove the data by hand (below). Each lives under `~/.planar/` unless one of five variables relocates it, and the installer protects the path under both the install root and `~/.planar/` (they differ when `--prefix` or `PLANAR_HOME` points elsewhere). It names a relocation and leaves the file where it is:

<!-- data-paths:begin (generated by `bash scripts/install-lib/data-paths.sh --markdown`; checked by scripts/install-data-paths-test.sh) -->
- `planar.db`: the SQLite database; relocated by `PLANAR_DB`
- `planar.db-wal`: its write-ahead log, which holds committed data not yet checkpointed; relocated by `PLANAR_DB`
- `planar.db-shm`: its shared-memory index; relocated by `PLANAR_DB`
- `queue-logs/`: output of detached queue runs
- `retired/`: databases and logs retired by an upgrade
- `workbench/`: the planning workbench; relocated by `PLANAR_WORKBENCH_ROOT`
- `config.toml`: the operator configuration; relocated by `PLANAR_CONFIG_PATH`
- `local/`: operator-local skills and agents; relocated by `PLANAR_LOCAL_HOME`
- `workspaces/`: workspace definitions
- `models/`: model catalogs
- `execute/`: execute profiles
- `templates/`: operator-editable templates; relocated by `PLANAR_TEMPLATES_DIR`
<!-- data-paths:end -->

A variable that is unset, empty, or points at the default location relocates nothing. The installer reads each variable the way the runtime does: `PLANAR_DB` and `PLANAR_LOCAL_HOME` literally, a relative value against the current directory, and a leading `~/` expanded only for `PLANAR_CONFIG_PATH`, `PLANAR_WORKBENCH_ROOT` and `PLANAR_TEMPLATES_DIR`. A relocation set in `config.toml` (`workbench.root`, `templates.dir`) is not read. `PLANAR_LOCAL_HOME` stands in for `$HOME`, so `local/` is then `$PLANAR_LOCAL_HOME/.planar/local`. A data path inside a tree the installer refreshes (for example a `PLANAR_TEMPLATES_DIR` under `~/.planar/scripts/`) stops the install with a message naming it, rather than being removed.

`make uninstall` is the counterpart of `make install`: it removes only the five Planar executables from `PREFIX/bin` (default `~/.local/bin`).

To remove the data by hand, delete the paths in the list above (the commands below cover the default locations; a relocated path is wherever its variable points, and anything you keep outside `~/.planar/` is not touched):

```bash
rm -f ~/.planar/planar.db ~/.planar/planar.db-wal ~/.planar/planar.db-shm ~/.planar/config.toml
rm -rf ~/.planar/queue-logs ~/.planar/retired ~/.planar/workbench ~/.planar/local ~/.planar/workspaces ~/.planar/models ~/.planar/execute ~/.planar/templates
```

The by-hand commands name the same twelve paths as the list; `scripts/install-data-paths-test.sh` checks that they match.

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

If it is yours and you want it gone, move it aside and rerun `./install.sh`. `planar-uninstall` likewise leaves a recorded path that someone replaced, and reports it.

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

After a full install (`install.sh`), the layout under `~/.planar/` is (the
[mutation lock](#ownership-recovery-and-the-order-of-an-install) is beside it,
at `~/.planar.lock/`):

```
~/.planar/
├── bin/
│   ├── planar                          # the C++26 operator binary
│   ├── planar-agent                    # agent-callable coordination binary
│   ├── planar-watch                    # human-facing read-only viewer
│   ├── planar-execute                  # deterministic spawn-free Lua workflow engine
│   ├── planar-ext                      # operational-plane binary (Jira, GitHub Issues)
│   └── planar-uninstall                # the standalone uninstaller (scripts/uninstall.sh)
├── install-manifest.json               # versioned managed-projection authority
├── release.json                        # the installed release (source: version = the build's sixth version token; prebuilt: the bundle's file)
├── planar.db                           # SQLite database (created by the install; mode 0600)
├── .planar-install                     # the install stamp, written last
├── .planar-journal                     # recovery journal; present only while an install or uninstall is unfinished
├── .planar-uninstalled                 # left by a finished uninstall that kept the root; the next install removes it
├── .staging-<token>/, <name>.old       # staging and backups of an unfinished install (journal-owned)
├── .planar-update/<name>/              # `planar update`'s download directory, removed by the installer it hands off to
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
