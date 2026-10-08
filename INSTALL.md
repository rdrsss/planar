# Installing Planar

Planar installs from a published release with one command. Building from
source is the contributor path.

1. [Install](#install): the one-line install, a pinned install, and the [platforms](#supported-platforms) the releases cover.
2. [Prerequisites](#prerequisites) of the release install.
3. [Update](#update-planar-update) with `planar update`, and [uninstall](#uninstall) with `planar-uninstall`.
4. [Installer reference](#installer-reference): flags, the order of an install, recovery and the upgrade notes.
5. [Install from source](#install-from-source-contributors): for contributors.

Plus [troubleshooting](#troubleshooting), the [install layout reference](#install-layout-reference) and the [environment variables](#environment-variables).

## Install

```sh
curl -fsSL https://github.com/rdrsss/planar/releases/latest/download/get-planar.sh | sh
```

The bootstrap, `get-planar.sh`, is a POSIX `sh` script that needs no Python and no toolchain. It:

1. Refuses a platform that has no bundle (see [Supported platforms](#supported-platforms)).
2. Resolves the latest release from `<base>/latest/download/VERSION`, then fetches `SHA256SUMS` and the platform bundle `planar-<os>-<arch>.tar.gz` from that one release's `<base>/download/<tag>/`, so a release published mid-run cannot mix two versions.
3. Verifies the bundle against its one `SHA256SUMS` record before it extracts anything.
4. Runs the bundle's `install.sh --prebuilt` in the foreground, which installs into `~/.planar/` and places the `planar` skill and the `planar-<role>` agents into each vendor harness it finds. The order of that install is under [Installer reference](#installer-reference).
5. Removes its temporary directory when the installer exits, and prints a warning when a different `planar` earlier on `PATH` shadows the installed one.

Then add `~/.planar/bin` to your `PATH` and verify:

```bash
# bash / zsh: add to ~/.bashrc or ~/.zshrc, then reload your shell:
export PATH="$HOME/.planar/bin:$PATH"

# fish: run once:
fish_add_path ~/.planar/bin

planar version
planar health
```

`planar health` reports the install. The skill and the agents are in place once a new harness session lists `planar` among its skills and the `planar-<role>` agents among its agents; the first step in any session is `planar --help`. The [skill and agent reference](docs/skill-reference.md) describes each piece. If a run is interrupted, run the same command again: the bootstrap finishes the pending release before it considers a newer one.

### Pin a release

To install a specific release, fetch that release's bootstrap and set `PLANAR_VERSION` on the shell that reads it, not on `curl`:

```sh
curl -fsSL https://github.com/rdrsss/planar/releases/download/vX.Y.Z/get-planar.sh | PLANAR_VERSION=vX.Y.Z sh
```

Replace `vX.Y.Z` with a tag from the [releases page](https://github.com/rdrsss/planar/releases). `PLANAR_VERSION` must match `vMAJOR.MINOR.PATCH`; any other value is refused. A pinned run never reads `latest/download/VERSION`. A release mirror or a local fixture is selected with `PLANAR_RELEASE_URL` (`https://...`, a `file:///...` directory, or `http://127.0.0.1` / `http://localhost`); `planar update` reads the same variable. Trailing slashes on the release base are ignored by the bootstrap, `install.sh` and the uninstaller alike, so a base written with or without them names the same interrupted install. A failed download names its cause: only a connection that could not be made says the release server cannot be reached; a transfer cut short and a failed write to the temporary directory (a full disk) say so instead.

## Supported platforms

| Bundle | Host | Floor |
|--------|------|-------|
| `planar-macos-arm64.tar.gz` | Apple silicon macOS | macOS 26.0 or later |
| `planar-linux-x86_64.tar.gz` | x86_64 Linux on glibc | glibc 2.36 or later (Debian 12 "bookworm" has 2.36) |

The binaries link the C++ runtime statically, so no compiler or LLVM install is needed on either host. The macOS floor is the binaries' deployment target; the Linux floor is the glibc the bundle was built against, which the bootstrap and `planar update` read from `ldd --version`. Any other platform (Intel macOS, Linux on another architecture, a musl libc) and any older host is refused before anything is installed, with a pointer to [Install from source](#install-from-source-contributors). Windows is not supported.

## Prerequisites

The release install needs only tools a Unix host already has:

- `bash`: the installer and `planar-uninstall` run under the `/bin/bash` 3.2 that stock macOS ships, with `set -u`; Homebrew bash is not needed.
- `curl` or GNU `wget`: the bootstrap downloads with whichever it finds, `curl` first. BusyBox `wget` is refused because it cannot be told not to follow redirects.
- `shasum` or `sha256sum`: the bootstrap verifies the bundle with whichever it finds.
- `tar`: unpacks the bundle (the bootstrap, and [`planar update`](#update-planar-update) with the system `tar`).
- The base system tools (`cp`, `mv`, `rm`, `mkdir`, `find`, `sed` and the like), which the installer preflights on every install and which any supported host ships; on Linux also `ldd`.
- On macOS, `/usr/sbin/ioreg` (stock on every Mac): the [install lock](#ownership-recovery-and-the-order-of-an-install) names the Mac by its hardware UUID with it. Without it the lock falls back to the host name and the installer warns. On Linux the lock reads `/etc/machine-id` instead, which needs no tool.

At runtime Planar uses these tools when they are present, and the installer warns when one is missing:

- `git` — required at runtime, **>= 2.31**. Planar runs `git remote get-url origin` for repo discovery (association/project registration) and walks `git log` / `git branch` / `git ls-files` during `planar import` and codeprobe. The 2.31 floor is load-bearing: worktree detection's authoritative fallback (`git rev-parse --path-format=absolute --git-common-dir`) needs the `--path-format=absolute` flag introduced in git 2.31 (see `docs/toolchain-parity.md`'s git row) — below that floor a primary checkout nested two or more levels below the repo root can be misclassified as a secondary worktree. A source install also needs it to clone the repository; a release install needs no clone.
- `gh` — optional but recommended. Used by the default `gh-cli` auth method for the GitHub adapter (`planar-ext ext register github <slug> --project <owner>/<repo>` with `--auth-env` omitted) and by `planar import` to enumerate existing GitHub Issues. Planar degrades gracefully when `gh` is absent.
- `jq` — used by the bundled agent specs (`planar-planner`, and the procedures in `agents/methodology.md`) to parse `planar … --json` output in their shell snippets. The binary itself does not depend on `jq`, but skipping it will break those workflows. No `yq` is needed; Planar handles YAML and TOML internally.
- `ripgrep` (`rg`) — recommended. Planar's agent workflows and the example session in [docs/getting-started.md](docs/getting-started.md#10-capture-hand-off-and-resume) (`planar capture command "rg -l 'v1.client'"`) prefer `rg` over `grep` for fast, gitignore-aware codebase search. Not a hard dependency, but the documented recipes assume it is available.

No Python, CMake, Ninja or LLVM is needed to install or run a release, and no system SQLite: the binaries carry the vendored SQLite amalgamation. A source install needs a toolchain and `python3` as well (`python3` also runs the old-queue-database retirement reader there); those are listed under [Contributor prerequisites](#contributor-prerequisites).

## Update (`planar update`)

`planar update` replaces an installation with a published release, from the
installed binary itself; the [bootstrap](#install) stays the first-install path
and the fallback when the binary cannot run.

```bash
planar update --check            # prints "installed v1.2.3 latest v1.3.0" and changes nothing
planar update                    # install the latest release
planar update --version v1.2.3   # install that release
```

`planar update --check` exits:

| Exit | Meaning |
|------|---------|
| `0` | The installed release equals the latest one. |
| `10` | The installed release differs from the latest one, so an update is available. Not a failure; `--check` is the only verb that returns it. |
| `1` | A fault: the release server cannot be reached, or an interrupted install or uninstall left the installation incomplete (the retry command is printed). |

A malformed argument or `PLANAR_RELEASE_URL` exits `2`. A source install records the version `dev` in `release.json`, so `--check` always reports an update there. `installed none` means no `release.json`.

It updates `$PLANAR_HOME` when set, else `~/.planar`, from
`https://github.com/rdrsss/planar/releases` or `PLANAR_RELEASE_URL` (the
bootstrap's grammar, `file://` included). It takes the installation's
[mutation lock](#ownership-recovery-and-the-order-of-an-install), so a running
install, update or uninstall makes it exit 1 naming the holder; downloads
`SHA256SUMS` and the platform bundle into `~/.planar/.planar-update/<name>/`;
verifies the one checksum record for the bundle; unpacks it with the system
`tar`; refuses an older glibc or a bundle whose database schema is older than
its own; and then execs the bundle's `install.sh --prebuilt` with
[the handoff](#the-update-handoff-and---cleanup), which keeps the lock without
a gap and removes the download directory when the install ends. An interrupted
install or uninstall is reported (exit 1, with its retry command) instead of
any verdict about the installed release; `planar update` never replays it
itself. It opens no database: migration is the installer's, and so is the
warning that a different `planar` earlier on `PATH` (for example the retired
`~/.local/bin/planar`) shadows the installed one; a `PATH` entry that is a
symlink to the installed binary does not. See
[`planar update`](docs/cli-reference.md#domain-update) for the full step list
and messages.

## Uninstall

```bash
~/.planar/bin/planar-uninstall            # remove Planar, keep the data paths
~/.planar/bin/planar-uninstall --purge    # remove the data paths too (database, workbench, config, templates)
# equivalently, from a checkout or an unpacked bundle:
./install.sh --uninstall [--purge] [--yes]
```

`planar-uninstall` is `scripts/uninstall.sh`, installed into `~/.planar/bin/`
by every install and shipped at the root of every release bundle as
`uninstall.sh`. It is a standalone bash script that needs only the
[base tools](#dependency-tiers) (no `python3`, and no working Planar binary), so it
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
- The retired old queue database and its sidecars, left from before the [queue upgrade](#upgrade-note-what-installsh-does-with-the-old-queue-plan-1089), which it never reads.
- Every other entry it does not know, including `.staging-*` and `.old` entries no journal owns: reported, never removed.

**`--purge`** also removes the preserved paths under the install root and the retired old queue database, naming each first (the files under `retired/` one by one), then the install root when nothing is left in it. A data path relocated by `PLANAR_DB`, `PLANAR_CONFIG_PATH`, `PLANAR_WORKBENCH_ROOT`, `PLANAR_LOCAL_HOME`, `PLANAR_TEMPLATES_DIR` or `config.toml` (`workbench.root`, `templates.dir`) is named and left where it is, and nothing outside the install root is removed. Unknown entries are still kept.

**`~/.local/bin`.** When `~/.local/bin` holds `planar` or a sibling binary (`planar-agent`, `planar-watch`, `planar-execute`, `planar-ext`) left by the retired Makefile install, the uninstaller names them and asks whether to remove them; `--yes` removes them without asking. With no terminal on standard input it asks nothing, leaves them and says to re-run with `--yes`.

**Ownership and interruption.** The uninstall takes the same [mutation lock](#ownership-recovery-and-the-order-of-an-install) as an install, `--purge` included: a running install, update or uninstall makes it exit 1 naming the holder, before anything is removed, and it holds the lock until every removal has finished. The lock directory beside the root (`~/.planar.lock`) is never removed. A recovery journal that does not validate stops it, kept. Before its first removal it records **uninstalling** in `~/.planar/.planar-journal` and flushes it to disk, which ends any interrupted install for good: no later install resumes or restores it, and an install refuses while that journal exists. It also prints the durable retry command:

- for a release install (one whose `release.json` records no `source_checkout`), the matching release's bundled uninstaller, downloaded afresh (the installed copy may already be gone). The retry downloads `SHA256SUMS` and the archive, verifies the archive against its one record with `sha256sum -c` or `shasum -a 256 -c` as the bootstrap does, and extracts only after the check passes. A mismatch or a missing or duplicate record refuses with `checksum verification failed for planar-<os>-<arch>.tar.gz`, naming that asset; with neither `sha256sum` nor `shasum` it refuses saying so; either way nothing is extracted. For an `https` release base both downloads add `--proto-redir =https`, so a redirect to plain http is refused as in the bootstrap. The scratch directory is removed when the command ends, however it ends. It is one parenthesised line (`PLANAR_RELEASE_URL` honoured, `--prefix`, `--purge` and `--yes` repeated, the release base shell-quoted):
  `(d="$(mktemp -d "${TMPDIR:-/tmp}/planar-uninstall.XXXXXX")" || exit 1; trap 'rm -rf "$d"' EXIT; a=planar-<os>-<arch>.tar.gz && u=<base>/download/<tag> && curl -fsSL "$u/SHA256SUMS" -o "$d/SHA256SUMS" && curl -fsSL "$u/$a" -o "$d/$a" && <verify "$a" against SHA256SUMS in "$d"> && tar -xzf "$d/$a" -C "$d" && bash "$d/planar-<os>-<arch>/uninstall.sh")`;
- for a source install, `cd <checkout> && ./install.sh --uninstall` with the checkout path shell-quoted. The installed copy has no checkout beside it, so a source install records the absolute checkout path as `"source_checkout"` in `release.json` (omitted for a path holding a quote, a backslash or a control character); `planar-uninstall` reads it, and names the checkout only while it is absolute and still holds `install.sh` and `scripts/uninstall.sh`. A usable recorded checkout takes precedence over the release form, so a source build of a tagged commit (whose `version` is the tag) gets the checkout command and never a release download. Run from the checkout itself, the uninstaller names the checkout it runs from;
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

A variable that is unset, empty, or points at the default location relocates nothing. The installer reads each variable the way the runtime does: `PLANAR_DB` and `PLANAR_LOCAL_HOME` literally, a relative value against the current directory, and a leading `~/` expanded only for `PLANAR_CONFIG_PATH`, `PLANAR_WORKBENCH_ROOT` and `PLANAR_TEMPLATES_DIR`. `workbench/` and `templates/` can also be relocated by `config.toml`: `[workbench] root` and `[templates] dir`. The installer reads them when the variable is unset or empty (the runtime's order: variable, then config file, then default), from the file `PLANAR_CONFIG_PATH` names or else `~/.planar/config.toml`, with quoted string values, trailing comments and a leading `~/` handled as the runtime handles them; a missing or malformed file relocates nothing. It reports such a relocation as `relocated by config.toml (workbench.root)`. `PLANAR_LOCAL_HOME` stands in for `$HOME`, so `local/` is then `$PLANAR_LOCAL_HOME/.planar/local`. A data path inside a tree the installer refreshes (for example a `PLANAR_TEMPLATES_DIR` under `~/.planar/scripts/`) stops the install, before anything is changed, with a message naming it, rather than being removed. The install is refused as a whole, not run with that subtree skipped, because the subtree is replaced as a unit alongside the binaries that belong to it and a half-refreshed install would be worse than none; move the data path out of the installer's trees and re-run. An uninstall never stops: it keeps the subtree that holds the data path, names it, and removes the rest.

To remove the data by hand, delete the paths in the list above (the commands below cover the default locations; a relocated path is wherever its variable points, and anything you keep outside `~/.planar/` is not touched):

```bash
rm -f ~/.planar/planar.db ~/.planar/planar.db-wal ~/.planar/planar.db-shm ~/.planar/config.toml
rm -rf ~/.planar/queue-logs ~/.planar/retired ~/.planar/workbench ~/.planar/local ~/.planar/workspaces ~/.planar/models ~/.planar/execute ~/.planar/templates
```

The by-hand commands name the same twelve paths as the list.

## Installer reference

Everything below describes `install.sh`, which the bundle carries and the bootstrap and `planar update` run with `--prebuilt`, and which a source install runs from a checkout. The [bootstrap](#install) is the operator entry point; run `install.sh` by hand only from an unpacked bundle or a checkout.

### Dependency tiers

`install.sh` declares its tools in three tiers in its `TOOLCHAIN_DEPS`, `BASE_DEPS` and `RUN_DEPS` manifests:

- The **toolchain tier** (`cmake`, `ninja`, the pinned LLVM compilers and `python3`, under [Contributor prerequisites](#contributor-prerequisites)) is checked only on a source install.
- The **base tier** is checked on every install path, `--prebuilt` included: `awk`, `basename`, `cat`, `chmod`, `cmp`, `cp`, `cut`, `date`, `diff`, `dirname`, `find`, `grep`, `head`, `ln`, `ls`, `mkdir`, `mktemp`, `mv`, `od`, `ps`, `readlink`, `realpath`, `rm`, `rmdir`, `sed`, `sleep`, `sort`, `sync`, `tr`, `uname` and `wc`. These ship with supported Unix-like systems; the installer preflights them before making changes.
- The **runtime tier** (`git`, `jq`, `gh`, `rg`, `tar`, and on macOS `/usr/sbin/ioreg`) only warns.

No system SQLite is needed. Planar vendors the SQLite amalgamation under `vendor/sqlite/`; the CMake build compiles it into a static library that statically links into every binary but `planar-execute` (which holds no SQLite handle at all) — no platform-specific build flags, no system library dependency.

### Install flags

| Flag | Purpose |
|------|---------|
| `--prefix DIR` | Install root (default `~/.planar`). See [The install root guard](#the-install-root-guard). |
| `--vendors LIST` | Comma-separated filter over the vendors found on the host: `claude`, `codex`, `copilot`, `gemini`, `antigravity`, `opencode`. Default is all six. Naming an absent vendor warns. |
| `--no-vendor` | Skip vendor surfaces entirely; install Planar core only. |
| `--prebuilt DIR` | Install an unpacked release bundle instead of building: see [Prebuilt install](#prebuilt-install---prebuilt). Refused together with `--link`, `--ignore-live-queue` or `--uninstall` (exit 2, naming both flags); without a directory it is a usage error (exit 64). |
| `--link` | Symlink the staged skill and the Markdown, Copilot and Codex agent files from the source repo instead of copying. **Dev mode** — edits to the repo propagate immediately. The OpenCode agents are always derived regular files. |
| `--force` | Overwrite existing symlinks at the destinations (never a preserved path: shipped `templates/` files are still placed only where missing), and adopt a non-empty install root that carries no Planar sign. It does **not** override the install root guard for `$HOME`, `/` or an empty root, and does **not** bypass the live-queue guard on an old queue database (see the upgrade note below). |
| `--ignore-live-queue` | Source install only (refused with `--prebuilt`, exit 2): retire the old queue database even while its queue has live entries, or when `python3` cannot check it. The cost is an orphaned old queue; see the upgrade note below. It never bypasses the old-range checks of the retire step. |
| `--no-prune` | Skip removal of stale vendor files. |
| `--preset NAME` | CMake build preset: `debug`\|`release` (default `release`). |
| `--build-dir DIR` | Where to configure and build (default `build/install-<preset>`). |
| `--dry-run`, `-n` | Show the planned actions without changing anything. |
| `--verbose`, `-v` | Per-file detail (default prints a summary). |
| `--version` | Print the installer version and exit. |
| `--cleanup DIR` | Updater only: remove the updater's temporary directory when the install ends. Accepted only with `--prebuilt` and the updater's handoff; see [the handoff](#the-update-handoff-and---cleanup). |
| `--uninstall` | Run the standalone uninstaller, `planar-uninstall` (the bundle's `uninstall.sh`, or the checkout's `scripts/uninstall.sh`); see [Uninstall](#uninstall). `--prefix`, `--purge` and `--yes` are passed on; every other install option is ignored. `--uninstall --force` exits 2 naming `--purge`, `--uninstall --prebuilt` exits 2 naming both flags, and `--uninstall --dry-run` exits 64. |
| `--purge` | With `--uninstall` only: remove the [preserved paths](#preserved-paths) too, except a relocated one. |
| `--yes`, `-y` | With `--uninstall` only: remove Planar binaries left in `~/.local/bin` without asking. |

### Prebuilt install (`--prebuilt`)

`install.sh --prebuilt <dir>` installs a release bundle that was unpacked into `<dir>` (the layout is `scripts/dist.sh`'s: `bin/`, `skills/planar/`, `agents/`, `codex-agents/`, `templates/`, `workflows/`, `migrations/`, `scripts/install-lib/`, `install.sh`, `uninstall.sh`, `get-planar.sh`, `install-cleanup.txt` and `release.json`). Nothing is built, so only the [base tier](#dependency-tiers) of tools is needed: no CMake, Ninja, LLVM or `python3`.

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
process id, that process's start time and the host it ran on. A second install,
update or uninstall exits 1 naming the holder's operation and pid and changes
nothing. A holder that was killed is reclaimed by the next run; a pid that was
reused by another process is recognized from its start time.

The host is named by its machine identity, not its host name: `/etc/machine-id`
on Linux (with the process's pid namespace, so a container and its host, or two
containers sharing a machine id, are different hosts), and the hardware UUID
(`IOPlatformUUID`, read with `/usr/sbin/ioreg`) on macOS. Renaming the host, by
hand, with `scutil` or through DHCP, does not change it, so a crashed install's
record is still reclaimed after a rename. A process's pid is checked only on the
host that recorded it: another machine sharing the filesystem (a network home)
never judges an owner dead. A host with neither identity falls back to its host
name, as earlier builds did; there a rename refuses (it never reclaims).

**When ownership cannot be judged**, the run exits 1, names the cause and keeps
every file:

- a record that is malformed, not a regular file, or names another generation,
  or a release marker that is not a link to its record;
- a record from another host: another machine identity (or another pid
  namespace), even under the same host name;
- a record from a host with no identity at all (no machine id and no host
  name), even on that host;
- a record from an earlier build, which named hosts by host name, under a name
  that is not this host's: another machine, or this one before a rename;
- a record whose process still exists but whose start time cannot be read;
- a record from an earlier build whose start time is in local time (`ps:`)
  while its process still exists: the time zone it was written in is unknown,
  so a reused pid cannot be told apart (once that process is gone, the record
  is reclaimed);
- a pid whose existence cannot be checked.

Remove the named `owner.<n>` record by hand only after making sure no Planar
install, update or uninstall is running. The run also refuses a `<root>.lock`
that is a symlink, is not owned by you or is writable by its group or by
others, and a filesystem where a record cannot be hard-linked, naming the
reason (an install root whose path contains `exists` is no exception). The
permission check reads the mode bits only, never an access control list: on
macOS an ACL entry (`chmod +a`) that lets another user write `<root>.lock` is
not detected, so keep ACLs off it (on Linux, a POSIX ACL that grants write
shows in the group bits and is refused). Nothing removes `<root>.lock`;
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
   command that completes the pending one. A resume also refuses, naming the
   reason and changing nothing, when this run is in the other copy or link
   mode than the interrupted one (`--link` or not), and, for a source install,
   when the checkout has uncommitted changes, tracked or untracked (or git
   cannot read it): only a clean checkout at the recorded commit is the tree
   the interrupted run was building. Stash the changes
   (`git stash --include-untracked`) and run the printed command. An
   **uninstalling** journal refuses: an interrupted uninstall is finished by
   uninstalling, never undone.
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
names the installer's own process (exec keeps the pid and start time) on this
host, and only once; a forged or replayed handoff exits 1. The record was
written by the updater, which is older than the bundle it downloaded, so the
installer also accepts an earlier build's record (named by host name, with a
local-time start), comparing its own start time and host in that record's
form. `--cleanup` is accepted only with
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

## Install from source (contributors)

Build and install from a checkout to develop on Planar or to run a platform that has no release bundle. A source install is the same `install.sh` the bundle carries, plus a build.

### Contributor prerequisites

On macOS, install the toolchain and the runtime tools via Homebrew:

```bash
brew install cmake ninja llvm python git gh jq ripgrep
```

- `cmake` (>= 4.3), `ninja`, and the pinned LLVM toolchain — required to configure and build the C++ binaries, on a source install. A [prebuilt install](#prebuilt-install---prebuilt) needs none of them. Both presets resolve the toolchain through `cmake/llvm-toolchain.cmake`, which discovers the prefix (an explicit `-DPLANAR_LLVM_PREFIX` first, then `brew --prefix llvm`, then apt.llvm.org's versioned prefixes and `PATH`) and refuses a candidate that lacks a modules-enabled `libc++`. `install.sh` additionally preflights the Homebrew paths `/opt/homebrew/opt/llvm/bin/clang` and `clang++` before invoking CMake. See [toolchain parity](docs/toolchain-parity.md) for the pinned versions and non-Homebrew-ARM-macOS resolution.
- `python3` — required to build from source, not to install a release bundle. The configure step registers Python test runners (`scripts/install-lib/queue_probe.test.py`, `scripts/install-lib/queue_retire.test.py` and `scripts/queue-logs-after-reset.test.py`) and `find_package(Python3)` is `REQUIRED`. A source `install.sh` also runs it for two things: the old-queue-database retirement reader (`scripts/install-lib/queue_retire.py`, standard library and `ctypes` only; it runs no other program) and the Codex agent TOML renderer (`scripts/render-codex-agents.py`). The queue-store probe of `planar.db` (`planar-agent queue status 1 --json`) is classified in shell on every install path and needs no Python. `migrations/README.md`'s counter-reset recipe runs the log helper `scripts/queue-logs-after-reset.py` with it.
- GNU `wget` — required only to run the `bootstrap.release` test (`scripts/get-planar-test.sh`, part of `make test`), which exercises the bootstrap's `wget` fallback with `curl` removed from `PATH`. It is a test-only dependency of a source checkout: no install path, `install.sh` manifest tier or release bundle needs it, so it is deliberately absent from `install.sh`'s `TOOLCHAIN_DEPS`, `BASE_DEPS` and `RUN_DEPS` (a source install does not run the tests). The test fails, never skips, when GNU `wget` is missing.
- No network access and no token are needed to configure or build: every dependency is committed under `vendor/` as a pinned release archive.
- `docker` — optional, developer-only. `make linux-gate` builds and tests the tree on Debian trixie in a container (see [docs/testing.md](docs/testing.md#the-linux-gate)). It is not an installer dependency.

The runtime tools (`git`, `gh`, `jq`, `rg`, `tar`) are described under [Prerequisites](#prerequisites).

#### Optional / research tools

- `sqlx-cli` and `sqlite3` — only needed for ad-hoc developer workflows against a scratch database (see [Build from source](#build-from-source)); the runtime embeds migrations via build-time codegen and uses the vendored SQLite amalgamation, so neither CLI is a runtime dependency. Install the optional `sqlx-cli` for authoring new migration pairs:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
```

### Source install (`install.sh`)

The contributor path, and the fallback where no release bundle exists for the platform. It builds the binaries from a checkout, installs them with the canonical agent specs, the `planar` skill, the workflows and the migration sources into `~/.planar/`, then places the skill and the agents into each vendor harness found on the host (Claude Code, Codex, Copilot, Gemini CLI, Antigravity, OpenCode; see the vendor table under [Install layout](#install-layout-reference) and the presence rule there).

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
```

That's it. The script:

- Takes the installation's [mutation lock](#ownership-recovery-and-the-order-of-an-install) and records a recovery journal, so a second install, update or uninstall of the same root is refused and an interrupted run can be completed by running it again.
- Builds all five binaries from source in its own build directory, `build/install-release/` (never the developer's `build/release/`), by running `cmake --preset release -B build/install-release -DPLANAR_VERSION_META=ON`, `cmake --build`, and `cmake --install … --prefix ~/.planar/.staging-<token>/.cmake-install`, and takes `bin/{planar,planar-agent,planar-watch,planar-execute,planar-ext}` from there.
- Stages `skills/planar/` and `agents/*.md`, derives the Codex agent TOML files from `agents/` into `codex-agents/` (never under `agents/codex/`), and stages `scripts/`, `workflows/` and `migrations/` (for ad-hoc `sqlx` use; the binary embeds them at build time via codegen), all under the same `~/.planar/.staging-<token>/`. It runs no renderer for the skill; the Codex agent TOML is derived by `scripts/render-codex-agents.py`. The staged paths are recorded in the `extras` list of `install-manifest.json`.
- Probes the database with the staged binaries, then swaps `bin/`, `skills/`, `agents/`, `codex-agents/`, `workflows/`, `scripts/` and `migrations/` into `~/.planar/` whole, and creates a missing database or migrates a behind one with the installed `planar init --skip-project --allow-no-repo`. `templates/` is placed missing-only.
- Places the staged skill and agents into each vendor whose presence marker exists (the nine targets in the [layout reference](#install-layout-reference)), and prints the vendors found and skipped.
- Writes `~/.planar/release.json`, the record of which release is installed. A source install writes it with `version` set to the sixth token of `planar version` (the release tag, `dev` for a build without one), so a source install is never mistaken for a release, and adds `"source_checkout"`, the absolute path of the checkout it ran from, for the uninstaller's retry command; a prebuilt install copies the bundle's file.
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

### Binaries only (`cmake --install`)

The shortest path from a clone: build the `release` preset and install Planar's
five executables and nothing else. The Makefile has no install or uninstall target
(the retired ones left binaries in `~/.local/bin` that shadow the installed
ones, which is why `install.sh` now names a shadowing `~/.local/bin/planar`).

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
cmake --preset release -DPLANAR_VERSION_META=ON
cmake --build build/release
cmake --install build/release --prefix "$HOME/.planar"
```

Add `~/.planar/bin` to your `$PATH`, then verify:

```bash
export PATH="$HOME/.planar/bin:$PATH"
planar health
```

This installs only the executables. Runtime migrations and default propagation
templates are *embedded* at build time, so the CLI works standalone against a
local database. The `planar` skill and the role agents are **not** embedded —
they are source files that `install.sh` stages and places — so neither is wired
into any vendor harness by a `cmake --install` alone; for
those, use the [source install](#source-install-installsh).

The five binaries are installed. The four
that open a database (all but `planar-execute`, which holds no SQLite handle at
all) statically link the vendored SQLite amalgamation — no system library
dependency.

This build does not include the Centurion workflow engine. `planar-execute`'s
engine verbs (`submit`, `status`, `cancel`, `follow`, `host`) require a
Centurion-enabled build (the `dev/centurion-integration` branch); this build
refuses them with `planar-execute was built without the Centurion engine` and
exit `1`. `planar-execute run` is unaffected.

For a debug build instead, configure and build by hand:

```bash
cmake --preset debug
cmake --build build/debug
cmake --install build/debug --prefix "$(mktemp -d)"
```

The prefix is a scratch directory on purpose: `~/.local/bin` is a retired install location, so installing there would leave binaries the installer no longer manages. A real install goes through `install.sh`.

### Build from source

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

After an install, the layout under `~/.planar/` is (the
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
└── scripts/                            # install-lib/ only, from a release bundle; a source install stages the repository's scripts/
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

The binary is **not** symlinked anywhere. Add `~/.planar/bin` to your `$PATH` (see [Install](#install)).

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
