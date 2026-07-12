# Installing Planar

This guide covers three install paths, ordered from simplest to most flexible:

1. [Quick install (`zig build --prefix`)](#quick-install-zig-build---prefix) — just the binary, no vendor surfaces.
2. [Full install (`install.sh`)](#full-install-installsh) — binary + agent specs + vendor surfaces (Claude / Codex / Copilot).
3. [Build from source](#build-from-source) — for contributors.

Plus [uninstall](#uninstall), [troubleshooting](#troubleshooting), and the [install layout reference](#install-layout-reference).

## Prerequisites

- **Zig 0.16.0 or later.** Verify with `zig version`. Required for all install paths. Minimum version is declared in `build.zig.zon`.
- **Git.** Required for the full install (clones the source repo).
- *Optional:* **`gh` CLI** — only if you plan to authenticate against GitHub Issues via `--auth gh-cli` (instead of an env-var token).
- *Optional:* **`sqlx-cli`** — only if you want to author new migration pairs ad-hoc. `cargo install sqlx-cli --no-default-features --features sqlite`.

No system SQLite is needed. Planar vendors the SQLite amalgamation under `vendor/sqlite/`; `build.zig` compiles it into a static library that statically links into the binary — no platform-specific build flags, no system library dependency.

## Quick install (`zig build --prefix`)

The shortest path. Drops the `planar` binary at `~/.planar/bin/planar` and nothing else.

There is no zig equivalent to a remote `module@version` install, so clone the repo and build directly into your install prefix:

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
zig build --prefix "$HOME/.planar"
```

`zig build --prefix <root>` installs the binary at `<root>/bin/planar` because `build.zig` registers the executable as the default install step.

Add `~/.planar/bin` to your `$PATH`, then verify:

```bash
export PATH="$HOME/.planar/bin:$PATH"
planar health
```

This installs only the binary. The agent specs, slash commands, skills, and migration sources are *embedded* in the binary, so the CLI works in isolation. But none of the vendor surfaces (Claude `/pl-*` slash commands, Codex skills, Copilot skills) are wired up — for those, use the [full install](#full-install-installsh).

If you want a non-default optimize mode, pass `-Doptimize`:

```bash
zig build -Doptimize=ReleaseFast --prefix "$HOME/.planar"
```

Optimize modes follow Zig conventions: `Debug` (default), `ReleaseSafe`, `ReleaseFast`, `ReleaseSmall`.

## Full install (`install.sh`)

The recommended path. Installs the binary plus the canonical agent specs, vendor surfaces, workflow validation scripts, and migration sources into `~/.planar/`, then installs or symlinks the vendor surfaces into your harness directories (`~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/`). Codex source files are kept flat under `~/.planar/skills/codex/`; the installer materializes `~/.planar/codex-skills/<skill>/SKILL.md` runtime directories, then installs real Codex skill directories at `~/.codex/skills/<skill>`. Planar-owned agent role specs stay under `~/.planar/agents/`.

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
# or, equivalently:
make install                 # extra flags via: make install INSTALL_FLAGS="--link --force"
```

That's it. The script:

- Builds `planar` from source by running `zig build -Doptimize=ReleaseSafe --prefix "$HOME/.planar"` from the repo root, which writes `~/.planar/bin/planar`.
- Copies `agents/`, `commands/`, `skills/`, `migrations/`, `scripts/`, and (if present) `copilot/` into `~/.planar/` (migrations are staged at `~/.planar/migrations/` for ad-hoc `sqlx` use; the binary embeds them at build time via codegen).
- Symlinks 31 surfaces per vendor into the vendor harness dirs.
- Atomically writes `~/.planar/install-manifest.json` after the selected vendor
  wiring succeeds. The versioned file records only selected Planar-managed
  skill and agent projections; operator-authored destination files are not
  claimed.

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

- `/pl-orchestrator <task-id> [<task-id>...]` — drive a task list end-to-end through the 5-phase coder/reviewer lifecycle (planning / ingestion / execution / propagation / archive).
- `/pl-coder <task-id>` — implement one task.
- `/pl-reviewer <task-id> <iteration>` — review coder output.
- `/pl-init`, `/pl-scope`, `/pl-plan`, `/pl-task`, `/pl-question`, `/pl-scenario`, `/pl-promote`, `/pl-workbench`, `/pl-sync`, `/pl-ext-create`, `/pl-audit-trail`, `/pl-resume`, `/pl-handoff`, `/pl-help`, `/pl-health` — the core reference workflows wrapping `planar` subcommands.
- `/pl-spec-draft` — draft a feature spec and roadmap into the workbench from a goal statement.
- `/pl-spec-ingest` — decompose workbench Markdown into a fleshed-out task graph (preview by default; `--apply` commits).
- `/pl-workbench-sync` — bidirectional workbench sync (pull, push, status, resolve).
- `/pl-workbench-archive` — archive the workbench tree for a completed feature.
- `/pl-ext-propagate` — propagate a feature plan to a registered operational system (Jira or GitHub Issues/Projects).
- `/pl-templates` — list, show, render, and validate operational-plane templates.

In Codex, invoke the same Planar skills with `$` syntax, for example `$pl-task` or `$pl-orchestrator`. Slash syntax is for Claude commands.

### Install flags

| Flag | Purpose |
|------|---------|
| `--prefix DIR` | Install root (default `~/.planar`). |
| `--vendors LIST` | Comma-separated subset (e.g. `claude,codex` or just `claude`). Default `claude,codex,copilot`. |
| `--no-vendor` | Skip vendor symlinks entirely; install Planar core only. |
| `--link` | Symlink artifacts from the source repo into `~/.planar/` instead of copying. **Dev mode** — edits to the repo propagate immediately. |
| `--force` | Overwrite existing symlinks at the destinations. |
| `--optimize MODE` | Zig optimize mode passed to `zig build -Doptimize=<mode>` (default `ReleaseSafe`). |
| `--uninstall` | Tear down everything install.sh created. Preserves `~/.planar/planar.db` unless `--force` is also given. |

### Copy mode vs link mode

- **Copy mode (default).** `install.sh` *copies* the source artifacts into `~/.planar/`. After install, the source checkout can be deleted; `~/.planar/` is self-contained. Updates to the source require re-running `install.sh`.
- **Link mode (`--link`).** `install.sh` *symlinks* the artifacts from the source repo into `~/.planar/`. Edits to the source propagate immediately. Useful for development on Planar itself or for any contributor iterating on the surfaces. Requires the source repo to stay on disk at its original path.

The global mode describes how source artifacts are staged. The install
manifest also records each projection's actual install kind: Claude skills and
all vendor agent files are links, while Codex and Copilot directory-shaped
skills are copied from their staged `SKILL.md` files so runtimes that do not
follow directory links can discover them.

### Initialize the database

After install, create the local database:

```bash
planar init --name "my-project"
```

The default database lives at `~/.planar/planar.db`. Override it with the `PLANAR_DB` environment variable; there is no global `--db` flag.

## Build from source

If you want to develop on Planar or contribute back:

The Zig package root IS the repo root (`build.zig` and `build.zig.zon` sit at the top level), so all build invocations run from the repo root:

```bash
git clone https://github.com/rdrsss/planar.git
cd planar
zig build                          # writes zig-out/bin/planar
zig build test                     # 1,700+ unit tests
zig build test-integration         # 570+ integration tests
zig fmt --check build.zig src tools integration_tests
```

The repo also ships a `Makefile` with the common targets:

```bash
make build              # build the binary into ./bin/planar (ReleaseSafe)
make test               # run the unit test suite
make test-integration   # build + run integration suite (sets PLANAR_BIN)
make test-all           # unit + integration
make fmt                # zig fmt the source tree
make fmt-check          # CI gate — verify zig fmt is clean
```

For dev-mode install where edits to the source repo are picked up live by the binary's siblings (skills, agents, commands) and vendor surfaces, use `./install.sh --link`. Note that binary edits still require a rebuild (`make build` or re-running `install.sh`).

For authoring or inspecting migrations:

```bash
cargo install sqlx-cli --no-default-features --features sqlite
sqlx migrate add -r <name> --source migrations
# Edit migrations/NNNNN_<name>.up.sql and migrations/NNNNN_<name>.down.sql.
zig build                          # codegen picks up the new files
```

(The runtime applies migrations automatically from an embedded `migrations` Zig module produced by `tools/gen_migrations.zig` at build time; the sqlx CLI is only needed for ad-hoc developer work against an out-of-band database or for authoring new migration pairs.)

To smoke a single migration as raw SQL against a scratch database:

```bash
sqlite3 /tmp/scratch.db < migrations/00001_foundation.up.sql
```

## Uninstall

```bash
cd /path/to/planar     # the source repo, where install.sh lives
./install.sh --uninstall
# or:
make uninstall
```

This removes:
- All vendor symlinks under `~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/` that point into `~/.planar/`.
- Everything in `~/.planar/` *except* `planar.db` — your data is preserved.

To remove the database too:

```bash
./install.sh --uninstall --force
```

To remove only the database and keep the install:

```bash
rm -f ~/.planar/planar.db
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

**`zig: command not found` during install.**

Install Zig 0.16.0 or later. On macOS: `brew install zig`. On Linux, fetch the latest tarball from [ziglang.org/download](https://ziglang.org/download/). Verify with `zig version`.

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
planar sync status                   # see which links are in conflict
planar audit trail <link-id>         # see the sync_events log
planar sync resolve <event-id> --keep local    # or --keep remote
```

## Install layout reference

After a full install (`install.sh`), the layout under `~/.planar/` is:

```
~/.planar/
├── bin/
│   └── planar                          # the Zig binary
├── install-manifest.json               # versioned managed-projection authority
├── planar.db                           # SQLite database (after `planar init`)
├── migrations/
│   ├── 00001_foundation.up.sql         # canonical migration sources, sqlx-cli format
│   ├── 00001_foundation.down.sql
│   ├── 00002_planning.up.sql
│   ├── 00002_planning.down.sql
│   └── … (through 00013_test_spec_artifact_kind)
├── agents/                             # vendor-neutral agent role specs
│   ├── methodology.md
│   ├── models.md
│   ├── orchestrator.md
│   ├── coder.md
│   ├── reviewer.md
│   ├── planner.md
│   ├── ingestor.md
│   └── ext-sync.md
├── commands/claude/                    # Claude slash-command sources
│   ├── pl-orchestrator.md
│   ├── pl-coder.md
│   ├── pl-reviewer.md
│   ├── pl-init.md
│   ├── pl-scope.md
│   ├── pl-plan.md
│   ├── pl-task.md
│   └── … (31 total per vendor)
├── skills/codex/                       # Codex skill sources (same 31 file names)
├── codex-skills/                       # Codex runtime skill directories
│   ├── pl-orchestrator/
│   │   └── SKILL.md
│   └── … (31 total)
├── skills/copilot/                     # Copilot skill sources (same 31 file names)
├── copilot-skills/                     # Copilot runtime skill directories
├── copilot/                            # Copilot instructions/prompts (if any)
├── templates/                          # Operator-editable defaults
└── scripts/                            # Bash tooling staged from the repo
```

Symlinks the installer creates out of `~/.planar/`:

```
~/.claude/commands/pl-*.md       →  ~/.planar/commands/claude/pl-*.md      (31 links)
~/.codex/skills/pl-*/SKILL.md    real files copied from ~/.planar/codex-skills/pl-*/SKILL.md (31 skills)
~/.copilot/skills/pl-*/SKILL.md  real files copied from ~/.planar/copilot-skills/pl-*/SKILL.md (31 skills)
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
| `PLANAR_BIN` | Used by the integration test harness to point at a pre-built binary (set automatically by `make test-integration`). |
| `JIRA_USER`, `JIRA_TOKEN`, etc. | Whatever you point `planar ext register --auth-env VAR_NAME` at. Comma-separated `USER,TOKEN` form uses HTTP Basic auth. |

Set them in your shell rc or per-command:

```bash
PLANAR_VENDOR=claude-code planar capture session --task 1
```

## Next steps

- [README quickstart](README.md#quickstart) — a worked example of the full flow.
- [docs/cli-reference.md](docs/cli-reference.md) — every command, flag, and exit code.
- [docs/architecture.md](docs/architecture.md) — system design: storage model, three operational context planes, schema contract, workbench, and configuration.
- [docs/workflows.md](docs/workflows.md) — end-to-end recipes: planning pipeline, bidirectional workbench, ext-sync propagation.
- [agents/methodology.md](agents/methodology.md) — how all eight agent roles collaborate; the 5-phase orchestrator flow.
