#!/usr/bin/env bash
#
# install.sh — install Planar from a source checkout.
#
# Layout produced:
#
#   ~/.planar/
#     bin/planar                      # the operator binary
#     bin/planar-agent                # the agent-callable coordination binary
#     bin/planar-watch                # the human-facing read-only viewer
#                                     # (opens DB read-only, zero write verbs)
#     bin/planar-execute              # deterministic spawn-free Lua workflow
#                                     # engine (run <wf.lua> --phase; shells
#                                     #  planar for state, holds no DB handle)
#     bin/planar-ext                  # operational-plane binary (Jira, GitHub
#                                     # Issues adapters; read-only on planning
#                                     # tables, read-write on external_links /
#                                     # external_systems / sync_events)
#     bin/scriptorium                 # in-tree skill + agent renderer
#     bin/centuriond                  # stock Centurion workflow daemon
#     share/centurion/                # centuriond migrations + build identity
#     install-manifest.json           # managed vendor projections
#     planar.db                       # created on first `planar init` (0600;
#                                     # the install root is 0700)
#     agent.db                        # agent-state database (override with
#                                     # PLANAR_AGENT_DB); created 0600 on first use
#     queue-logs/                     # detached queue-run output, beside agent.db
#     migrations/00001_foundation.up.sql  # canonical migration sources (also
#                                     # embedded into the binary at configure
#                                     # time via CMake codegen)
#     agents/                         # vendor-neutral agent role specs, plus
#                                     # rendered agents/{claude,codex,copilot,gemini}/
#     skills/src/pl-*.md              # unified authored skill sources
#     commands/claude/pl-*.md         # rendered Claude slash commands
#     skills/codex/pl-*/SKILL.md      # rendered Codex skills
#     codex-skills/pl-*/SKILL.md      # Codex runtime skill directories
#     skills/copilot/pl-*.md          # rendered Copilot skills
#     copilot-skills/pl-*/SKILL.md    # Copilot runtime skill directories
#     skills/gemini/pl-*.md           # rendered Gemini skills
#     gemini-skills/pl-*/SKILL.md     # Gemini runtime skill directories
#     copilot/                        # Copilot instructions/prompts (only when
#                                     # the checkout has a copilot/ directory)
#     templates/                      # operator-editable defaults
#     workflows/                      # Lua workflows
#     scripts/validate-{barrel-modes,plan-status}-acceptance
#
# Then installs into the vendor harness dirs (only when --vendors selects
# them; default is all four):
#
#   ~/.claude/commands/pl-*.md   ->   ~/.planar/commands/claude/pl-*.md
#   ~/.codex/skills/pl-*         # real Codex skill dirs copied from ~/.planar/codex-skills/pl-*
#   ~/.copilot/skills/pl-*       # real Copilot skill dirs copied from ~/.planar/copilot-skills/pl-*
#   ~/.gemini/antigravity-cli/skills/pl-*  # real Gemini skill dirs copied from ~/.planar/gemini-skills/pl-*
#   ~/.claude/agents/<name>.md   ->   ~/.planar/agents/claude/<name>.md
#   ~/.codex/agents/<name>.toml  ->   ~/.planar/agents/codex/<name>.toml
#   ~/.copilot/agents/<name>.agent.md -> ~/.planar/agents/copilot/<name>.agent.md
#   ~/.gemini/antigravity-cli/agents/<name>  ->  ~/.planar/agents/gemini/<name>
#
# The binary lives at ~/.planar/bin/planar. Add ~/.planar/bin to your PATH:
#
#   # bash / zsh
#   export PATH="$HOME/.planar/bin:$PATH"
#
#   # fish
#   fish_add_path ~/.planar/bin
#
# Usage:
#   ./install.sh                      # full install with all four vendors
#   ./install.sh --no-vendor          # install Planar core only; skip vendor symlinks
#   ./install.sh --vendors claude     # install + symlink Claude only
#   ./install.sh --vendors claude,codex
#   ./install.sh --link               # symlink from this repo instead of copying
#                                     #   (dev mode — edits to repo propagate)
#   ./install.sh --prefix /opt/planar # override ~/.planar
#   ./install.sh --force              # overwrite existing symlinks
#   ./install.sh --uninstall          # tear down everything install.sh created
#   ./install.sh --preset debug       # CMake preset (default release)
#   ./install.sh --with-solver        # link the Mt-KaHyPar solver (needs tbb)
#   ./install.sh --dry-run            # preview planned actions without changing anything
#   ./install.sh --verbose            # per-file detail (default prints a summary)
#   ./install.sh --version            # print installer version and exit
#
# Run ./install.sh --help for the full option list.
# Output is colorized on a TTY; set NO_COLOR=1 (or pipe stdout) for plain text.

# -E (errtrace) propagates the ERR trap into subshells/functions so a failure
# inside the CMake build subshell is reported, not swallowed.
set -eEuo pipefail

# ---------- defaults ----------

PLANAR_HOME="${PLANAR_HOME:-$HOME/.planar}"
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
VENDORS="claude,codex,copilot,gemini"
MODE="copy"                   # copy | link
FORCE=0
UNINSTALL=0
NO_PRUNE=0                    # set with --no-prune to skip stale-vendor-file removal
BUILD_PRESET="release"        # CMake preset
# The installer builds in its OWN directory, never the developer's
# build/<preset> (task 6537). Reusing it meant the install inherited whatever
# flags the cache happened to hold -- a `make test-parity-cpp` run leaves
# PLANAR_WITH_MTKAHYPAR=ON there, which silently produced a solver-linked
# install -- and left PLANAR_VERSION_META=ON behind afterwards, invalidating
# the whole build graph on every subsequent commit.
BUILD_DIR=""                  # resolved below; override with --build-dir
WITH_SOLVER=0                 # set with --with-solver (Mt-KaHyPar; needs tbb)
VERBOSE=0                     # set with --verbose/-v for per-file detail
DRY_RUN=0                     # set with --dry-run/-n to preview without changes
INSTALLER_VERSION="1.0.0"     # install.sh's own version (see --version)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$REPO_ROOT/scripts/install-manifest.sh"
# The queue upgrade steps (plan 1089): probe and migrate the prefix planar.db.
# Sourced so the installer-manifest test and a post-build ctest case drive the
# exact functions this script runs.
source "$REPO_ROOT/scripts/install-lib/queue-retire.sh"

# usage — the canonical help text. Defined before arg parsing so -h/--help and
# the unknown-flag path can both reach it. (Replaces the old header-comment sed
# scrape, which broke whenever the header format drifted.)
usage() {
  cat <<'EOF'
install.sh — install Planar from a source checkout.

Usage:
  ./install.sh [options]

Options:
  --prefix DIR       Install root (default: ~/.planar)
  --vendors LIST     Comma-separated vendors to wire: claude,codex,copilot,gemini (default: all)
  --no-vendor        Install Planar core only; skip vendor surfaces
  --link             Symlink from this repo instead of copying (dev mode)
  --force            Overwrite existing symlinks / adopt a non-Planar prefix
  --no-prune         Skip removal of stale vendor files
  --preset NAME      CMake build preset: debug|release (default: release)
  --build-dir DIR    Where to configure and build (default:
                     build/install-<preset>). The installer never builds in
                     the developer's build/<preset>.
  --with-solver      Link the Mt-KaHyPar solver (needs tbb). Off by default;
                     without it `groups recommend --solver mtkahypar`
                     degrades to greedy and reports optimal_available:false.
  --dry-run, -n      Show what would happen without making any changes
  --verbose, -v      Per-file detail (default prints a summary)
  --uninstall        Tear down everything install.sh created
  --version          Print the installer version and exit
  -h, --help         Show this help and exit

Output is colorized on a TTY; set NO_COLOR=1 (or pipe stdout) for plain text.
EOF
}

# ---------- parse args ----------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)     PLANAR_HOME="$2"; shift 2 ;;
    --vendors)    VENDORS="$2"; shift 2 ;;
    --no-vendor)  VENDORS=""; shift ;;
    --link)       MODE="link"; shift ;;
    --force)      FORCE=1; shift ;;
    --uninstall)  UNINSTALL=1; shift ;;
    --no-prune)   NO_PRUNE=1; shift ;;
    --preset)     BUILD_PRESET="$2"; shift 2 ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
    --with-solver) WITH_SOLVER=1; shift ;;
    --verbose|-v) VERBOSE=1; shift ;;
    --dry-run|-n) DRY_RUN=1; shift ;;
    --version)
      _sha="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
      printf 'install.sh (Planar installer) %s — repo %s\n' "$INSTALLER_VERSION" "$_sha"
      exit 0
      ;;
    -h|--help)    usage; exit 0 ;;
    *) printf 'install.sh: unknown flag: %s\n\n' "$1" >&2; usage >&2; exit 64 ;;
  esac
done

# The installer's own build directory. Deliberately NOT build/<preset>: that
# one belongs to the developer, and sharing it is how an install picked up a
# parity lane's solver flag and left version-metadata stamping switched on
# behind it (task 6537). `--build-dir` overrides for callers that need to
# place it elsewhere (the installer integration tests do).
[[ -n "$BUILD_DIR" ]] || BUILD_DIR="$REPO_ROOT/build/install-$BUILD_PRESET"

# ---------- output helpers ----------

# TTY-aware color. Honors NO_COLOR (https://no-color.org) and a non-tty stdout
# (pipes, CI logs) by emitting no escape codes.
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'
else
  C_RESET=''; C_BOLD=''; C_DIM=''
  C_CYAN=''; C_GREEN=''; C_YELLOW=''; C_RED=''
fi

CURRENT_STEP="starting up"     # updated by title(); named by the ERR trap on failure
WARN_COUNT=0                   # warnings emitted; re-surfaced in the final summary

# title  — a bold section header; also records the step name for the ERR trap.
# log    — an always-shown detail line.
# vlog   — a per-file detail line, shown only under --verbose (dimmed).
# ok     — a green success line.
# warn   — a yellow non-fatal note (counted, sent to stderr).
# err    — a red fatal message; exits 1.
title() { CURRENT_STEP="$*"; printf '\n%s==>%s %s%s%s\n' "$C_CYAN$C_BOLD" "$C_RESET" "$C_BOLD" "$*" "$C_RESET"; }
log()   { printf '  %s\n' "$*"; }
vlog()  { [[ "$VERBOSE" -eq 1 ]] && printf '  %s%s%s\n' "$C_DIM" "$*" "$C_RESET"; return 0; }
ok()    { printf '  %s✓%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn()  { WARN_COUNT=$((WARN_COUNT + 1)); printf '  %s!%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
err()   { printf '\n%sinstall.sh: %s%s\n' "$C_RED" "$*" "$C_RESET" >&2; exit 1; }

# harden_planar_home — make the install root private (decision 1210).
# planar.db and agent.db both store task claim tokens, which authorise
# heartbeats and terminal verbs on a claim, so the install root is 0700 and
# the databases (with their -wal/-shm sidecars) are 0600. Creates the root
# when absent, tightens an existing install in place, and touches nothing
# else: queue-logs/ is already created 0700 with 0600 logs by `queue run`, and
# PLANAR_DB / PLANAR_AGENT_DB overrides outside the root are the operator's.
# A symlinked database is left alone rather than chmodding its target.
harden_planar_home() {
  mkdir -p "$PLANAR_HOME"
  # A chmod that fails is a warning, never an abort: a live -wal/-shm can be
  # checkpointed away between the test and the chmod, and a file the user can
  # write but does not own cannot be chmodded. Under `set -e` and the ERR trap
  # either would end the install.
  chmod 700 "$PLANAR_HOME" 2>/dev/null || warn "could not restrict $PLANAR_HOME to mode 700"
  local db
  for db in planar.db planar.db-wal planar.db-shm agent.db agent.db-wal agent.db-shm; do
    if [[ -f "$PLANAR_HOME/$db" && ! -L "$PLANAR_HOME/$db" ]]; then
      if chmod 600 "$PLANAR_HOME/$db" 2>/dev/null; then
        vlog "$db: mode 600"
      else
        warn "could not restrict $PLANAR_HOME/$db to mode 600"
      fi
    fi
  done
  vlog "$PLANAR_HOME: mode 700"
}

# set -e + this ERR trap turn a raw mid-script failure (a bad CMake build, a
# failed `cp`) into a framed message naming the phase that died, instead of a
# bare non-zero exit the user has to reverse-engineer. Explicit err() exits and
# `cmd || …` guarded failures never reach here.
on_err() {
  local code=$1 line=$2
  printf '\n%s==> install failed%s during: %s%s%s (line %s, exit %s)\n' \
    "$C_RED$C_BOLD" "$C_RESET" "$C_BOLD" "$CURRENT_STEP" "$C_RESET" "$line" "$code" >&2
  printf '  The failing command'\''s output is above. Re-run with %s--verbose%s for per-step detail.\n' \
    "$C_BOLD" "$C_RESET" >&2
  exit "$code"
}
trap 'on_err $? $LINENO' ERR

# The D13 install freeze ended at the M9 parity gate, and the M10 cutover
# (task 6045, decisions 963/982) deleted `zig/`: the CMake build is the only
# build, and the binaries this script installs are its output.
# check_deps "<tier label>" <fatal:0|1> "cmd|brewpkg|what it's for" …
# Checks every entry and reports ALL missing tools at once (not one-at-a-time),
# with a `brew install …` hint built from the entries that have a Homebrew
# package. Fatal tier aborts; non-fatal tier warns (counted) and continues.
# Keep the BUILD_DEPS / RUN_DEPS manifests below in sync with README.md
# § Prerequisites.
check_deps() {
  local label="$1" fatal="$2"; shift 2
  local entry cmd rest pkg desc m
  local -a missing=() brew=()
  for entry in "$@"; do
    cmd="${entry%%|*}"; rest="${entry#*|}"; pkg="${rest%%|*}"; desc="${rest#*|}"
    if command -v "$cmd" >/dev/null 2>&1; then
      vlog "dep ok: $cmd"
    else
      missing+=("$cmd — $desc")
      [[ -n "$pkg" ]] && brew+=("$pkg")
    fi
  done
  [[ ${#missing[@]} -eq 0 ]] && return 0
  if [[ "$fatal" -eq 1 ]]; then
    printf '\n%sinstall.sh: missing required %s tool(s):%s\n' "$C_RED$C_BOLD" "$label" "$C_RESET" >&2
    for m in "${missing[@]}"; do printf '  %s✗%s %s\n' "$C_RED" "$C_RESET" "$m" >&2; done
    [[ ${#brew[@]} -gt 0 ]] && printf '  macOS: brew install %s\n' "${brew[*]}" >&2
    exit 1
  fi
  printf '  %s!%s %s tool(s) missing — install proceeds, but some workflows will degrade:\n' \
    "$C_YELLOW" "$C_RESET" "$label" >&2
  for m in "${missing[@]}"; do WARN_COUNT=$((WARN_COUNT + 1)); printf '    %s-%s %s\n' "$C_YELLOW" "$C_RESET" "$m" >&2; done
  [[ ${#brew[@]} -gt 0 ]] && printf '    macOS: brew install %s\n' "${brew[*]}" >&2
  return 0
}

# version_ge "1.2.3" "1.2.0" → true if the first is >= the second. Pure bash,
# no `sort -V` (BSD sort on macOS lacks it). Callers strip pre-release suffixes.
version_ge() {
  local IFS=.; local -a a=($1) b=($2); local i x y
  for i in 0 1 2; do
    x=${a[i]:-0}; y=${b[i]:-0}
    ((10#$x > 10#$y)) && return 0
    ((10#$x < 10#$y)) && return 1
  done
  return 0
}

# count_glob — how many of the given paths exist (a non-matching glob passes its
# literal pattern, which fails the -e test, so the count is 0).
count_glob() { local n=0 f; for f in "$@"; do [[ -e "$f" ]] && n=$((n + 1)); done; printf '%s' "$n"; }

# Place a file (or directory) at $2 from $1, either by copy or symlink.
place() {
  local src="$1" dst="$2"
  if [[ "$MODE" == "link" ]]; then
    ln -sfn "$src" "$dst"
  else
    if [[ -d "$src" ]]; then
      mkdir -p "$dst"
      # Copy contents, preserving timestamps. Use cp -R to mirror dirs.
      cp -R "$src/." "$dst/"
    else
      cp -f "$src" "$dst"
    fi
  fi
}

# Symlink $1 -> $2, creating parent dirs and handling existing targets.
symlink_to() {
  local src="$1" dst="$2"
  mkdir -p "$(dirname "$dst")"
  if [[ -e "$dst" || -L "$dst" ]]; then
    if [[ "$FORCE" -eq 1 ]]; then
      rm -f "$dst"
    else
      # Already the right link?
      local current
      current="$(readlink "$dst" 2>/dev/null || true)"
      if [[ "$current" == "$src" ]]; then
        return 0
      fi
      err "$dst already exists and points elsewhere (rerun with --force to overwrite)"
    fi
  fi
  ln -s "$src" "$dst"
}

# ---------- uninstall path ----------

if [[ "$UNINSTALL" -eq 1 ]]; then
  title "Uninstalling Planar"

  for vendor_root in "$HOME/.claude/commands" "$HOME/.codex/skills" "$HOME/.copilot/skills" "$HOME/.gemini/antigravity-cli/skills"; do
    [[ -d "$vendor_root" ]] || continue
    while IFS= read -r -d '' link; do
      target="$(readlink "$link" 2>/dev/null || true)"
      if [[ "$target" == "$PLANAR_HOME"/* ]]; then
        log "removing symlink: $link"
        rm -f "$link"
      fi
    done < <(find "$vendor_root" -maxdepth 1 -name 'pl-*.md' -print0)
  done

  for codex_root in "$CODEX_HOME/skills"; do
    [[ -d "$codex_root" ]] || continue
    while IFS= read -r -d '' link; do
      target="$(readlink "$link" 2>/dev/null || true)"
      if [[ "$target" == "$PLANAR_HOME"/* ]]; then
        log "removing Codex skill link: $link"
        rm -f "$link"
      fi
    done < <(find "$codex_root" -maxdepth 1 -name 'pl-*' -type l -print0)
  done

  if [[ -d "$CODEX_HOME/skills" ]]; then
    while IFS= read -r -d '' link; do
      skill_dir="$(dirname "$link")"
      target="$(readlink "$link" 2>/dev/null || true)"
      marker="$skill_dir/.planar-source"
      marker_target=""
      if [[ -f "$marker" ]]; then
        marker_target="$(cat "$marker" 2>/dev/null || true)"
      fi
      if [[ "$target" == "$PLANAR_HOME"/* || "$marker_target" == "$PLANAR_HOME"/* ]]; then
        log "removing Codex skill: $skill_dir"
        rm -f "$link"
        rm -f "$marker"
        rmdir "$skill_dir" 2>/dev/null || true
      fi
    done < <(find "$CODEX_HOME/skills" -mindepth 2 -maxdepth 2 -path '*/pl-*/SKILL.md' -print0)
  fi

  if [[ -d "$HOME/.copilot/skills" ]]; then
    while IFS= read -r -d '' link; do
      skill_dir="$(dirname "$link")"
      target="$(readlink "$link" 2>/dev/null || true)"
      marker="$skill_dir/.planar-source"
      marker_target=""
      if [[ -f "$marker" ]]; then
        marker_target="$(cat "$marker" 2>/dev/null || true)"
      fi
      if [[ "$target" == "$PLANAR_HOME"/* || "$marker_target" == "$PLANAR_HOME"/* ]]; then
        log "removing Copilot skill: $skill_dir"
        rm -f "$link"
        rm -f "$marker"
        rmdir "$skill_dir" 2>/dev/null || true
      fi
    done < <(find "$HOME/.copilot/skills" -mindepth 2 -maxdepth 2 -path '*/pl-*/SKILL.md' -print0)
  fi

  if [[ -d "$PLANAR_HOME/codex-skills" ]]; then
    log "removing Codex runtime skills: $PLANAR_HOME/codex-skills"
    rm -rf "$PLANAR_HOME/codex-skills"
  fi

  if [[ -d "$PLANAR_HOME/copilot-skills" ]]; then
    log "removing Copilot runtime skills: $PLANAR_HOME/copilot-skills"
    rm -rf "$PLANAR_HOME/copilot-skills"
  fi

  # Remove agent symlinks installed by symlink_vendor_agents. Mirrors the
  # prune_stale_vendor_agents safety semantics: only remove files whose symlink
  # target points into $PLANAR_HOME; regular files and operator-authored links
  # are left untouched.
  for agent_dir in "$HOME/.claude/agents" "$CODEX_HOME/agents" "$HOME/.copilot/agents" "$HOME/.gemini/antigravity-cli/agents"; do
    [[ -d "$agent_dir" ]] || continue
    while IFS= read -r -d '' link; do
      target="$(readlink "$link" 2>/dev/null || true)"
      if [[ "$target" == "$PLANAR_HOME"/* ]]; then
        log "removing agent symlink: $link"
        rm -f "$link"
      fi
    done < <(find "$agent_dir" -maxdepth 1 -type l -print0)
  done

  if [[ -d "$PLANAR_HOME" ]]; then
    # Prefix ownership guard, uninstall side. The --uninstall branch returns
    # (exit 0) above the INSTALL-side ownership guard further down this file,
    # so without this check a mistyped --prefix/PLANAR_HOME reaches the
    # unconditional `rm -rf "$PLANAR_HOME"` below with NO ownership check at
    # all — `PLANAR_HOME=$HOME ./install.sh --uninstall --force` deletes the
    # operator's home directory outright, and even without --force the
    # find-and-delete two lines down removes every top-level entry of
    # $PLANAR_HOME except the preserved data entries (planar.db, agent.db,
    # their SQLite sidecars, queue-logs/).
    #
    # Two checks, in order:
    #   1. $PLANAR_HOME normalized (plain `realpath`, not `-m` — GNU's `-m`
    #      tolerates a nonexistent path but BSD/macOS realpath has no `-m`
    #      flag at all and errors out, which silently fell through to the
    #      raw-string fallback below and defeated the whole normalization on
    #      macOS when this was first written; `-m` is unneeded here anyway,
    #      since we are already inside `-d "$PLANAR_HOME"`, so the path is
    #      guaranteed to exist) so a trailing slash or a symlink cannot
    #      defeat the comparison — the D13 normalization hazard tasks
    #      6051/6052/6053 described for a different, now-gone code path,
    #      applied to the real one — must not equal $HOME itself. $HOME is
    #      never a valid Planar prefix, so this refuses even under --force:
    #      there is no legitimate reason to override it.
    #   2. Absent that, the SAME ownership signals the install-side guard
    #      uses (stamp / bin/planar / planar.db / agent.db) must be present,
    #      unless --force overrides — mirroring the install-side guard
    #      exactly rather than inventing a second policy.
    _planar_home_real="$(realpath "$PLANAR_HOME" 2>/dev/null || echo "$PLANAR_HOME")"
    _home_real="$(realpath "$HOME" 2>/dev/null || echo "$HOME")"
    if [[ "$_planar_home_real" == "$_home_real" ]]; then
      err "refusing to uninstall: PLANAR_HOME ($PLANAR_HOME) resolves to \$HOME ($HOME) — this would delete your home directory. Set --prefix to the actual Planar install root."
    fi
    if [[ ! -e "$PLANAR_HOME/.planar-install" && ! -x "$PLANAR_HOME/bin/planar" && ! -e "$PLANAR_HOME/planar.db" && ! -e "$PLANAR_HOME/agent.db" && "$FORCE" -ne 1 ]]; then
      err "$PLANAR_HOME does not look like a Planar install (no bin/planar, no planar.db, no agent.db, no .planar-install stamp). Refusing to remove it. Pass the correct --prefix, or re-run with --force to remove it anyway."
    fi

    log "removing install root: $PLANAR_HOME"
    log "(planar.db, agent.db, their -wal/-shm sidecars and queue-logs/ are preserved if you have data — re-run with --force or rm manually)"
    if [[ "$FORCE" -eq 1 ]]; then
      rm -rf "$PLANAR_HOME"
    else
      # Preserve the databases and the agent database's detached-run output;
      # remove everything else. The SQLite sidecars (-wal / -shm) of both
      # databases hold committed data not yet checkpointed into the main file
      # (both run in WAL mode, so an unclean exit leaves them behind), so they
      # stay with their database. Both planar.db and agent.db default to this directory;
      # PLANAR_DB / PLANAR_AGENT_DB overrides live wherever the operator put
      # them and are never touched here.
      find "$PLANAR_HOME" -mindepth 1 -maxdepth 1 \
        ! -name 'planar.db' ! -name 'planar.db-wal' ! -name 'planar.db-shm' \
        ! -name 'agent.db' ! -name 'agent.db-wal' ! -name 'agent.db-shm' \
        ! -name 'queue-logs' \
        -exec rm -rf {} +
    fi
  fi

  title "Uninstall complete."
  exit 0
fi

# ---------- preflight ----------

title "Planar — install from $REPO_ROOT"

# Dependency manifest — keep in sync with README.md § Prerequisites and the
# CLAUDE.md "external tool dependencies" rule. Format: "cmd|brewpkg|what for"
# (empty brewpkg = base system tool, no Homebrew hint).
#
# build_deps: the installer itself shells these; a miss aborts the install.
# run_deps:   Planar (the binary + bundled agent skills) needs these at run
#             time; a miss only warns — the install still produces a binary.
BUILD_DEPS=(
  "cmake|cmake|configures, builds, and installs the five Planar binaries and Scriptorium"
  "ninja|ninja|C++26 module dependency scanning"
  "/opt/homebrew/opt/llvm/bin/clang|llvm|pinned LLVM C compiler required by CMakePresets.json"
  "/opt/homebrew/opt/llvm/bin/clang++|llvm|pinned LLVM C++ compiler required by CMakePresets.json"
  "python3|python|Centurion's configure; install.sh's queue store probe classifier (scripts/install-lib/queue_retire.py)"
  "shasum||digest centuriond and verify a Centurion release asset"
  "mktemp||stage centuriond release downloads"
  "tar||unpack a Centurion release asset"
  "uname||select the Centurion release asset for this platform"
  "cp||copy install artifacts into place"
  "ln||symlink vendor surfaces"
  "mkdir||create the install tree"
  "rm||replace prior-install artifacts"
  "mv||atomically replace the install manifest"
  "find||walk vendor + template source trees"
  "head||take the first Mt-KaHyPar smoke result"
  "rmdir||remove emptied vendor skill directories"
  "awk||read the build id from 'planar version'"
  "grep||validate the Planar package manifest"
  "cat||read help text + vendor ownership markers"
  "ls||detect a non-empty / foreign install prefix"
  "readlink||resolve existing vendor symlinks safely"
  "basename||derive install target names"
  "dirname||resolve parent directories"
  "tr||normalize version / PATH strings"
  "chmod||mark shipped scripts executable"
)
RUN_DEPS=(
  "cmp||checks installed projection bytes in scripts/check-self-installed.sh"
  "git|git|repo discovery + 'planar import' (required at runtime)"
  "jq|jq|bundled agent skills parse 'planar … --json' output"
  "gh|gh|GitHub adapter auth + issue import (degrades gracefully)"
  "rg|ripgrep|agent-workflow code-search recipes (ripgrep)"
  "tabularium||bundled documentation-maintenance workflows"
)

check_deps "build" 1 "${BUILD_DEPS[@]}"

# TBB (decision 1006, task 6459) — cmake/dependencies.cmake now vendors
# Mt-KaHyPar unconditionally, and Mt-KaHyPar's own CMakeLists.txt
# find_package(TBB)s the Homebrew `tbb` formula (TBB does not support static
# linking; decision 1006 accepts this as the one dynamic runtime dependency
# the vendored solver needs). CMake configure FATAL_ERRORs without it, so
# this fails the install here too rather than deep into the CMake build.
# check_deps() above only probes command-line tools (`command -v`); TBB
# ships no CLI binary of its own, so it is checked directly the same way
# cmake/dependencies.cmake's own probe resolves it.
if command -v brew >/dev/null 2>&1 && [[ -d "$(brew --prefix tbb 2>/dev/null)" ]]; then
  vlog "dep ok: tbb ($(brew --prefix tbb))"
else
  printf '\n%sinstall.sh: missing required build tool(s):%s\n' "$C_RED$C_BOLD" "$C_RESET" >&2
  printf '  %s✗%s tbb — Mt-KaHyPar (`groups recommend --solver mtkahypar`) TBB runtime dependency\n' "$C_RED" "$C_RESET" >&2
  printf '  macOS: brew install tbb\n' >&2
  exit 1
fi

# Check that we are in a CMake Planar source checkout.
[[ -f "$REPO_ROOT/CMakeLists.txt" ]] || err "CMakeLists.txt not found in $REPO_ROOT (run install.sh from the Planar source repo)"
[[ -f "$REPO_ROOT/CMakePresets.json" ]] || err "CMakePresets.json not found in $REPO_ROOT"


# Runtime tools — non-fatal; the install still produces a working binary, but
# Planar's git-backed verbs and the bundled agent skills need these to work.
check_deps "Planar runtime" 0 "${RUN_DEPS[@]}"

# git version floor — non-fatal, matching the RUN_DEPS tier above (git is
# already in RUN_DEPS; this adds the *version* check check_deps' presence-only
# probe can't express). >= 2.31 is required for `git rev-parse
# --path-format=absolute --git-common-dir` (docs/toolchain-parity.md's git
# row) — below that floor, worktree detection can misclassify a primary
# checkout nested two or more levels below the repo root as a secondary
# worktree. This check stays live after the C++ cutover.
MIN_GIT="2.31.0"
if command -v git >/dev/null 2>&1; then
  HAVE_GIT="$(git --version | awk '{print $3}')"
  HAVE_GIT_CORE="${HAVE_GIT%%-*}"
  if [[ -n "$HAVE_GIT_CORE" ]] && ! version_ge "$HAVE_GIT_CORE" "$MIN_GIT"; then
    warn "git $MIN_GIT or newer required for correct worktree detection, found $HAVE_GIT ($(command -v git))"
  fi
fi
log "PLANAR_HOME = $PLANAR_HOME"
log "mode        = $MODE"
log "vendors     = ${VENDORS:-(none)}"
log "preset      = $BUILD_PRESET"
log "build dir   = $BUILD_DIR"
log "solver      = $( ((WITH_SOLVER)) && echo "mtkahypar (linked)" || echo "off (greedy only)" )"
[[ "$DRY_RUN" -eq 1 ]] && log "dry-run     = yes (no changes will be made)"

# Writability — the install writes into $PLANAR_HOME (or creates it). Fail with
# a clear message now rather than a raw mkdir error mid-build.
if [[ -e "$PLANAR_HOME" ]]; then
  [[ -w "$PLANAR_HOME" ]] || err "cannot write to $PLANAR_HOME (permissions?)"
else
  _parent="$(dirname "$PLANAR_HOME")"
  [[ -d "$_parent" && -w "$_parent" ]] || err "cannot create $PLANAR_HOME — $_parent is not writable (permissions?)"
fi

# Prefix ownership guard — the install wipes subdirs of $PLANAR_HOME (skills/,
# commands/, agents/, …). A mis-typed --prefix (say ~/dev or $HOME) would wipe
# the wrong tree. Refuse unless the prefix is empty, already a Planar install
# (carries a bin/planar or the .planar-install stamp), or --force overrides.
PLANAR_STAMP="$PLANAR_HOME/.planar-install"
if [[ -d "$PLANAR_HOME" && -n "$(ls -A "$PLANAR_HOME" 2>/dev/null)" ]]; then
  # Ownership signals: the stamp, an existing binary, or a preserved planar.db
  # or agent.db (left behind by a non-force uninstall). Any one of them means
  # "our tree".
  if [[ ! -e "$PLANAR_STAMP" && ! -x "$PLANAR_HOME/bin/planar" && ! -e "$PLANAR_HOME/planar.db" && ! -e "$PLANAR_HOME/agent.db" && "$FORCE" -ne 1 ]]; then
    err "$PLANAR_HOME exists and does not look like a Planar install (no bin/planar, no planar.db, no agent.db, no .planar-install stamp). Pass a clean --prefix, remove it, or re-run with --force to adopt it."
  fi
fi

# Dry run — everything above is read-only preflight; stop here and print the
# plan rather than mutating anything.
if [[ "$DRY_RUN" -eq 1 ]]; then
  _cleanup_n=0
  [[ -f "$REPO_ROOT/install-cleanup.txt" ]] && \
    _cleanup_n="$(grep -cE '^[[:space:]]*[^#[:space:]]' "$REPO_ROOT/install-cleanup.txt" || true)"
  title "Dry run — planned actions"
  log "build 5 Planar binaries and scriptorium → $PLANAR_HOME/bin  [preset=$BUILD_PRESET]"
  log "run cleanup manifest: $_cleanup_n path(s) checked for removal"
  if [[ -e "$PLANAR_HOME/planar.db" ]]; then
    log "probe $PLANAR_HOME/planar.db with the new planar-agent; migrate it with planar init only when behind"
  fi
  log "wipe + re-place: agents/, scripts/, skills/, commands/, migrations/$([[ -d "$REPO_ROOT/copilot" ]] && echo ', copilot/')"
  log "render per-vendor skill + agent outputs into $PLANAR_HOME"
  log "place templates/ (missing-only; --force overwrites)"
  if [[ -n "$VENDORS" ]]; then
    log "wire vendor surfaces: $VENDORS → ~/.claude, ~/.codex, ~/.copilot, ~/.gemini/antigravity-cli"
  else
    log "vendor surfaces: skipped (--no-vendor)"
  fi
  printf '\n'
  log "Re-run without --dry-run to apply."
  exit 0
fi

# ---------- build the binary ----------

title "Building the Planar binaries"

harden_planar_home
mkdir -p "$PLANAR_HOME/bin"
# CMake configures, builds, and installs all FIVE executable targets:
#
#   planar         — operator surface
#   planar-agent   — agent-callable coordination (atomic claim ops,
#                    nested actions, ingest, operator recovery)
#   planar-watch   — human-facing read-only viewer (no write verbs,
#                    strict SQLITE_OPEN_READONLY handle)
#   planar-execute — deterministic spawn-free Lua workflow engine
#                    (run <wf.lua> --phase; shells planar for state,
#                    holds no DB handle, no model-spawn host fn)
#   planar-ext     — operational-plane binary (Jira, GitHub Issues adapters;
#                    read-only on planning tables, read-write on exactly
#                    external_links / external_systems / sync_events)
#
# `PLANAR_VERSION_META=ON` stamps the install's git metadata. It stays off
# for ordinary dev builds so commit/dirty changes do not invalidate the tree.
#
# EVERY option this build depends on is pinned explicitly. A configure that
# omits a flag does NOT reset it -- the cache wins -- so an unpinned
# PLANAR_WITH_MTKAHYPAR was inherited from whatever last configured the
# directory (task 6537). `--with-solver` is the only way to turn it on here,
# and it requires the tbb preflight above.
#
# First-party dependencies (Centurion, a private repository) are fetched into
# the gitignored external/ on the first configure, not committed under
# vendor/ (task 6495). That download needs a GitHub token; borrow gh's when
# the operator has not exported one. A later configure reuses external/ and
# needs neither.
if [[ -z "${GITHUB_TOKEN:-}" ]] && command -v gh >/dev/null 2>&1; then
  # An unauthenticated gh is normal when Centurion is already cached. Its
  # failed token lookup must not fire the inherited ERR trap inside $().
  trap - ERR
  _planar_gh_token="$(gh auth token 2>/dev/null)" || _planar_gh_token=""
  trap 'on_err $? $LINENO' ERR
  if [[ -n "$_planar_gh_token" ]]; then
    export GITHUB_TOKEN="$_planar_gh_token"
  fi
  unset _planar_gh_token
fi
( cd "$REPO_ROOT" \
  && cmake --preset "$BUILD_PRESET" -B "$BUILD_DIR" \
       -DPLANAR_VERSION_META=ON \
       -DPLANAR_WITH_MTKAHYPAR="$( ((WITH_SOLVER)) && echo ON || echo OFF )" \
  && cmake --build "$BUILD_DIR" \
  && cmake --install "$BUILD_DIR" --prefix "$PLANAR_HOME" )
vlog "wrote $PLANAR_HOME/bin/planar"
vlog "wrote $PLANAR_HOME/bin/planar-agent"
vlog "wrote $PLANAR_HOME/bin/planar-watch"
vlog "wrote $PLANAR_HOME/bin/planar-execute"
vlog "wrote $PLANAR_HOME/bin/planar-ext"
[[ -x "$PLANAR_HOME/bin/scriptorium" ]] || err "the in-tree scriptorium tool was not installed"
SCRIPTORIUM_BIN="$PLANAR_HOME/bin/scriptorium"
vlog "wrote $SCRIPTORIUM_BIN"
[[ "$("$SCRIPTORIUM_BIN" version 2>/dev/null)" == scriptorium\ * ]] || \
  err "installed $SCRIPTORIUM_BIN but it failed its version smoke check"

# Smoke check — a build can succeed yet produce a binary that won't run. Confirm
# it executes now (and capture the build id) rather than discovering it broken
# at `planar init`.
PLANAR_VERSION_LINE="$("$PLANAR_HOME/bin/planar" version 2>/dev/null || true)"
[[ -n "$PLANAR_VERSION_LINE" ]] || \
  err "built $PLANAR_HOME/bin/planar but it failed to run ('planar version' produced no output)"
PLANAR_BUILD_ID="$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $2}')"
ok "built 5 Planar binaries and scriptorium → $PLANAR_HOME/bin  ${C_DIM}($PLANAR_VERSION_LINE)${C_RESET}"

# Stock centuriond from the same pinned Centurion archive planar-execute's
# client links (plan 1033 M1, task 6709; tech-spec D8/D13). Built as a
# SEPARATE CMake project — a Centurion release binary is preferred when the
# pinned tag publishes one — and installed with its migrations and build
# identity under $PLANAR_HOME. The Planar configure above wrote the pin to
# $BUILD_DIR/centurion-pin.env; the daemon's build tree is kept under build/
# so a re-install only recompiles what changed.
"$REPO_ROOT/scripts/install-centuriond.sh" \
  --pin "$BUILD_DIR/centurion-pin.env" \
  --prefix "$PLANAR_HOME" \
  --build-dir "$REPO_ROOT/build/centuriond-$BUILD_PRESET" \
  --toolchain "$REPO_ROOT/cmake/llvm-toolchain.cmake" \
  || err "could not install centuriond (see the output above)"
[[ -x "$PLANAR_HOME/bin/centuriond" ]] || err "centuriond was not installed"
ok "installed centuriond → $PLANAR_HOME/bin  ${C_DIM}($(grep -o '"source": "[^"]*"' "$PLANAR_HOME/share/centurion/build-identity.json"))${C_RESET}"

# CMake install only writes the targets it builds — it never removes files a
# PRIOR install left behind. Iterate the cleanup manifest and delete
# any $PLANAR_HOME-relative artifact current Planar no longer ships (e.g. a
# binary dropped in a refactor) so a re-install over an older tree is clean.
# See install-cleanup.txt.
CLEANUP_LIST="$REPO_ROOT/install-cleanup.txt"
if [[ -f "$CLEANUP_LIST" ]]; then
  while IFS= read -r _raw; do
    _line="${_raw%%#*}"                  # strip an inline comment
    read -r _relpath _ <<< "$_line"      # trim whitespace; first token = path
    [[ -z "$_relpath" ]] && continue
    _target="$PLANAR_HOME/$_relpath"
    if [[ "$_relpath" == */ ]]; then
      [[ -d "$_target" ]] && { rm -rf "$_target"; log "removed stale dir  $_target"; }
    else
      [[ -e "$_target" ]] && { rm -f "$_target"; log "removed stale file $_target"; }
    fi
  done < "$CLEANUP_LIST"
fi

# ---------- queue store ----------

# The queue lives in planar.db (plan 1089). Ask the planar-agent just
# installed whether the prefix's planar.db is usable, read-only and from /,
# and migrate it with `planar init` only when it is behind (decision 1008).
# An ahead planar.db is never refused: an incompatible or foreign queue schema
# only warns. Any other answer stops the install here, with the new binaries
# installed and nothing retired. See scripts/install-lib/queue-retire.sh.
title "Checking the queue store"
queue_probe_migrate || exit 1

# ---------- place artifacts ----------

title "Placing source artifacts into $PLANAR_HOME"

# These directories are the canonical Planar artifacts that get installed.
# In copy mode we mirror them under $PLANAR_HOME; in link mode we symlink the
# whole tree so edits to the repo propagate.
#
# `agents/` and `skills/` are special-cased because each mixes canonical
# authored input with rendered vendor output. Their prefix roots must remain
# real directories so the renderer never writes through a link into the source
# checkout.
for d in scripts workflows; do
  if [[ -d "$REPO_ROOT/$d" ]]; then
    rm -rf "$PLANAR_HOME/$d"
    place "$REPO_ROOT/$d" "$PLANAR_HOME/$d"
    log "$d/ → $PLANAR_HOME/$d ($MODE)"
  fi
done

# Stage only the canonical top-level agent sources. Vendor subdirectories are
# renderer output and are recreated below. Link mode keeps authored role/docs
# live, except models.md: the renderer patches its tier table at install time,
# so that file must be prefix-owned to preserve the canonical checkout.
rm -rf "$PLANAR_HOME/agents"
mkdir -p "$PLANAR_HOME/agents"
if [[ -d "$REPO_ROOT/agents" ]]; then
  while IFS= read -r -d '' f; do
    dst="$PLANAR_HOME/agents/$(basename "$f")"
    if [[ "$MODE" == "link" && "$(basename "$f")" != "models.md" ]]; then
      ln -s "$f" "$dst"
    else
      cp -f "$f" "$dst"
    fi
  done < <(find "$REPO_ROOT/agents" -maxdepth 1 -type f -print0)
  log "agents/ → $PLANAR_HOME/agents (canonical sources: $MODE; rendered outputs: copy)"
fi

# Stage skills/src — the unified renderer input — and prepare a real
# $PLANAR_HOME/skills/ directory the renderer can write into without
# touching the repo. Wipe the whole skills/ subtree first so previous-
# install rendered files don't linger.
rm -rf "$PLANAR_HOME/skills" "$PLANAR_HOME/commands"
mkdir -p "$PLANAR_HOME/skills"
if [[ -d "$REPO_ROOT/skills/src" ]]; then
  place "$REPO_ROOT/skills/src" "$PLANAR_HOME/skills/src"
  log "skills/src/ → $PLANAR_HOME/skills/src ($MODE)"
fi

# Render the per-vendor outputs (commands/claude, skills/codex,
# skills/copilot, skills/gemini, agents/{claude,codex,copilot,gemini})
# directly into $PLANAR_HOME by shelling the CMake-installed in-tree
# scriptorium binary, driven by the committed scriptorium.yaml (repo root).
# This replaces Planar's retired in-tree renderer verb (tech-spec.md §
# Architecture "How Planar shells scriptorium"). Note: `agents/models.md`'s
# `## Tier Table` is NOT patched by this step, and is not patched anywhere —
# it is hand-maintained in agents/models.md, so
# models.md installs as ordinary committed content.
title "Rendering per-vendor skill outputs (scriptorium)"
SCRIPTORIUM_CONFIG="$REPO_ROOT/scriptorium.yaml"
[[ -f "$SCRIPTORIUM_CONFIG" ]] || err "scriptorium.yaml not found at $SCRIPTORIUM_CONFIG (required to render skill/agent sources)"
# scriptorium lists every file it writes on stdout; that per-file detail is
# verbose-only. Keep stderr (warnings/errors) so the ERR trap still fires.
if [[ "$VERBOSE" -eq 1 ]]; then
  ( cd "$PLANAR_HOME" && "$SCRIPTORIUM_BIN" render --config "$SCRIPTORIUM_CONFIG" )
else
  ( cd "$PLANAR_HOME" && "$SCRIPTORIUM_BIN" render --config "$SCRIPTORIUM_CONFIG" ) >/dev/null
fi
log "rendered via scriptorium: commands/claude, skills/codex, skills/copilot, skills/gemini, agents/{claude,codex,copilot,gemini}"

# Migrations live at repo root in sqlx-cli format and are read by the CMake
# build via configure-time codegen. We also stage them under $PLANAR_HOME for
# ad-hoc tooling (e.g. operators running `sqlx migrate` against scratch DBs).
if [[ -d "$REPO_ROOT/migrations" ]]; then
  rm -rf "$PLANAR_HOME/migrations"
  place "$REPO_ROOT/migrations" "$PLANAR_HOME/migrations"
  log "migrations/ → $PLANAR_HOME/migrations ($MODE)"
fi

# copilot/ is optional and may not exist yet.
if [[ -d "$REPO_ROOT/copilot" ]]; then
  rm -rf "$PLANAR_HOME/copilot"
  place "$REPO_ROOT/copilot" "$PLANAR_HOME/copilot"
  log "copilot/ → $PLANAR_HOME/copilot ($MODE)"
fi

# templates/ ships operator-editable defaults (workspace-capabilities.toml,
# doc prompts). Unlike the surfaces above we do NOT rm -rf first: the
# install drops individual files into place only when missing, so hand
# edits survive `install.sh` re-runs. Use --force to overwrite.
if [[ -d "$REPO_ROOT/templates" ]]; then
  mkdir -p "$PLANAR_HOME/templates"
  while IFS= read -r -d '' f; do
    rel="${f#$REPO_ROOT/templates/}"
    dst="$PLANAR_HOME/templates/$rel"
    mkdir -p "$(dirname "$dst")"
    if [[ -e "$dst" && "$FORCE" -ne 1 ]]; then
      vlog "templates/$rel: kept existing (use --force to overwrite)"
      continue
    fi
    if [[ "$MODE" == "link" ]]; then
      ln -sfn "$f" "$dst"
    else
      cp -f "$f" "$dst"
    fi
    vlog "templates/$rel → $dst ($MODE)"
  done < <(find "$REPO_ROOT/templates" -type f -print0)
fi

# Make sure copied script tooling is executable. In link mode the prefix path
# resolves into the source checkout; chmod there would mutate canonical source
# modes (including non-executable data manifests).
if [[ "$MODE" != "link" && -d "$PLANAR_HOME/scripts" ]]; then
  chmod +x "$PLANAR_HOME/scripts/"* 2>/dev/null || true
fi

# ---------- vendor symlinks ----------

if [[ -n "$VENDORS" ]]; then
  title "Symlinking vendor surfaces"

  # Map vendor name → (planar source dir, harness target dir).
  symlink_vendor() {
    local name="$1" src_dir="$2" dst_dir="$3"
    if [[ ! -d "$src_dir" ]]; then
      warn "$name: source $src_dir not found, skipping"
      return 0
    fi
    mkdir -p "$dst_dir"
    local count=0
    while IFS= read -r -d '' f; do
      symlink_to "$f" "$dst_dir/$(basename "$f")"
      count=$((count + 1))
    done < <(find "$src_dir" -maxdepth 1 -name 'pl-*.md' -print0)
    log "$name: linked $count file(s) into $dst_dir"
  }

  # Codex discovers skills as directories that contain SKILL.md. Scriptorium's
  # built-in Codex profile already stages its render output that way — one
  # `pl-<slug>/SKILL.md` directory per skill under $src_dir (docs/format.md §
  # 2.1/2.4's `layout: dir`), unlike Claude/Copilot/Gemini's flat `pl-*.md`
  # staging — so this reads the STAGED SKILL.md directly rather than a flat
  # source file. (Deliberately not overridden to `layout: file` in
  # scriptorium.yaml: that field also gates whether `kind: doc` sources
  # render for this vendor — docs/format.md § 2.5 — and Codex's TOML agent
  # surface has no markdown-link consumer, so it should keep skipping them.)
  # Materialize Planar-owned runtime skill directories, then install real
  # Codex skill directories into CODEX_HOME so discovery works even when the
  # loader does not follow symlinked directories.
  install_codex_vendor() {
    local src_dir="$1" runtime_dir="$2" dst_dir="$3"
    if [[ ! -d "$src_dir" ]]; then
      warn "codex: source $src_dir not found, skipping"
      return 0
    fi
    rm -rf "$runtime_dir"
    mkdir -p "$runtime_dir"
    mkdir -p "$dst_dir"
    local count=0
    while IFS= read -r -d '' f; do
      local skill_name legacy runtime_skill_dir dst_skill_dir marker
      skill_name="$(basename "$(dirname "$f")")"
      legacy="$dst_dir/${skill_name}.md"
      runtime_skill_dir="$runtime_dir/$skill_name"
      dst_skill_dir="$dst_dir/$skill_name"
      marker="$dst_skill_dir/.planar-source"

      if [[ -L "$legacy" ]]; then
        local legacy_target
        legacy_target="$(readlink "$legacy" 2>/dev/null || true)"
        if [[ "$legacy_target" == "$src_dir/"* || "$legacy_target" == "$PLANAR_HOME/skills/codex/"* ]]; then
          rm -f "$legacy"
        fi
      fi

      if [[ -L "$dst_skill_dir" ]]; then
        local dir_target
        dir_target="$(readlink "$dst_skill_dir" 2>/dev/null || true)"
        if [[ "$dir_target" == "$runtime_dir/"* || "$dir_target" == "$PLANAR_HOME/codex-skills/"* ]]; then
          rm -f "$dst_skill_dir"
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -f "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      if [[ -d "$dst_skill_dir" && -f "$marker" ]]; then
        local marker_target
        marker_target="$(cat "$marker" 2>/dev/null || true)"
        if [[ "$marker_target" == "$f" || "$marker_target" == "$PLANAR_HOME/skills/codex/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rm -f "$marker"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        fi
      fi

      if [[ -d "$dst_skill_dir" && ( -e "$dst_skill_dir/SKILL.md" || -L "$dst_skill_dir/SKILL.md" ) ]]; then
        local skill_target
        skill_target="$(readlink "$dst_skill_dir/SKILL.md" 2>/dev/null || true)"
        if [[ "$skill_target" == "$f" || "$skill_target" == "$PLANAR_HOME/skills/codex/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -rf "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      mkdir -p "$runtime_skill_dir"
      if [[ "$MODE" == "link" ]]; then
        symlink_to "$f" "$runtime_skill_dir/SKILL.md"
      else
        cp -f "$f" "$runtime_skill_dir/SKILL.md"
      fi
      mkdir -p "$dst_skill_dir"
      cp -f "$runtime_skill_dir/SKILL.md" "$dst_skill_dir/SKILL.md"
      rm -rf "$runtime_skill_dir/references" "$dst_skill_dir/references"
      mkdir -p "$runtime_skill_dir/references/agents" "$dst_skill_dir/references/agents"
      while IFS= read -r -d '' agent_ref; do
        cp -f "$agent_ref" "$runtime_skill_dir/references/agents/$(basename "$agent_ref")"
        cp -f "$agent_ref" "$dst_skill_dir/references/agents/$(basename "$agent_ref")"
      done < <(find -L "$PLANAR_HOME/agents" -maxdepth 1 -type f -name '*.md' -print0)
      printf '%s\n' "$f" > "$marker"
      count=$((count + 1))
    done < <(find "$src_dir" -mindepth 2 -maxdepth 2 -type f -name 'SKILL.md' -print0)
    log "codex: installed $count skill directories into $dst_dir"
  }

  # Copilot discovers skills as directories that contain SKILL.md (same layout as
  # Codex). Keep Planar's source-of-truth files flat, materialize runtime skill
  # directories, then install into ~/.copilot/skills/ so discovery works.
  install_copilot_vendor() {
    local src_dir="$1" runtime_dir="$2" dst_dir="$3"
    if [[ ! -d "$src_dir" ]]; then
      warn "copilot: source $src_dir not found, skipping"
      return 0
    fi
    rm -rf "$runtime_dir"
    mkdir -p "$runtime_dir"
    mkdir -p "$dst_dir"
    local count=0
    while IFS= read -r -d '' f; do
      local skill_name legacy runtime_skill_dir dst_skill_dir marker
      skill_name="$(basename "$f" .md)"
      legacy="$dst_dir/${skill_name}.md"
      runtime_skill_dir="$runtime_dir/$skill_name"
      dst_skill_dir="$dst_dir/$skill_name"
      marker="$dst_skill_dir/.planar-source"

      # Clean up legacy flat symlinks from previous installs.
      if [[ -L "$legacy" ]]; then
        local legacy_target
        legacy_target="$(readlink "$legacy" 2>/dev/null || true)"
        if [[ "$legacy_target" == "$src_dir/"* || "$legacy_target" == "$PLANAR_HOME/skills/copilot/"* ]]; then
          rm -f "$legacy"
        fi
      fi

      if [[ -L "$dst_skill_dir" ]]; then
        local dir_target
        dir_target="$(readlink "$dst_skill_dir" 2>/dev/null || true)"
        if [[ "$dir_target" == "$runtime_dir/"* || "$dir_target" == "$PLANAR_HOME/copilot-skills/"* ]]; then
          rm -f "$dst_skill_dir"
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -f "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      if [[ -d "$dst_skill_dir" && -f "$marker" ]]; then
        local marker_target
        marker_target="$(cat "$marker" 2>/dev/null || true)"
        if [[ "$marker_target" == "$f" || "$marker_target" == "$PLANAR_HOME/skills/copilot/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rm -f "$marker"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        fi
      fi

      if [[ -d "$dst_skill_dir" && ( -e "$dst_skill_dir/SKILL.md" || -L "$dst_skill_dir/SKILL.md" ) ]]; then
        local skill_target
        skill_target="$(readlink "$dst_skill_dir/SKILL.md" 2>/dev/null || true)"
        if [[ "$skill_target" == "$f" || "$skill_target" == "$PLANAR_HOME/skills/copilot/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -rf "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      mkdir -p "$runtime_skill_dir"
      if [[ "$MODE" == "link" ]]; then
        symlink_to "$f" "$runtime_skill_dir/SKILL.md"
      else
        cp -f "$f" "$runtime_skill_dir/SKILL.md"
      fi
      mkdir -p "$dst_skill_dir"
      cp -f "$runtime_skill_dir/SKILL.md" "$dst_skill_dir/SKILL.md"
      printf '%s\n' "$f" > "$marker"
      count=$((count + 1))
    done < <(find "$src_dir" -maxdepth 1 -name 'pl-*.md' -print0)
    log "copilot: installed $count skill directories into $dst_dir"
  }


  # Gemini discovers skills as directories that contain SKILL.md (same layout as
  # Codex). Keep Planar's source-of-truth files flat, materialize runtime skill
  # directories, then install into ~/.gemini/antigravity-cli/skills/ so discovery works.
  install_gemini_vendor() {
    local src_dir="$1" runtime_dir="$2" dst_dir="$3"
    if [[ ! -d "$src_dir" ]]; then
      warn "gemini: source $src_dir not found, skipping"
      return 0
    fi
    rm -rf "$runtime_dir"
    mkdir -p "$runtime_dir"
    mkdir -p "$dst_dir"
    local count=0
    while IFS= read -r -d '' f; do
      local skill_name legacy runtime_skill_dir dst_skill_dir marker
      skill_name="$(basename "$f" .md)"
      legacy="$dst_dir/${skill_name}.md"
      runtime_skill_dir="$runtime_dir/$skill_name"
      dst_skill_dir="$dst_dir/$skill_name"
      marker="$dst_skill_dir/.planar-source"

      # Clean up legacy flat symlinks from previous installs.
      if [[ -L "$legacy" ]]; then
        local legacy_target
        legacy_target="$(readlink "$legacy" 2>/dev/null || true)"
        if [[ "$legacy_target" == "$src_dir/"* || "$legacy_target" == "$PLANAR_HOME/skills/gemini/"* ]]; then
          rm -f "$legacy"
        fi
      fi

      if [[ -L "$dst_skill_dir" ]]; then
        local dir_target
        dir_target="$(readlink "$dst_skill_dir" 2>/dev/null || true)"
        if [[ "$dir_target" == "$runtime_dir/"* || "$dir_target" == "$PLANAR_HOME/gemini-skills/"* ]]; then
          rm -f "$dst_skill_dir"
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -f "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      if [[ -d "$dst_skill_dir" && -f "$marker" ]]; then
        local marker_target
        marker_target="$(cat "$marker" 2>/dev/null || true)"
        if [[ "$marker_target" == "$f" || "$marker_target" == "$PLANAR_HOME/skills/gemini/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rm -f "$marker"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        fi
      fi

      if [[ -d "$dst_skill_dir" && ( -e "$dst_skill_dir/SKILL.md" || -L "$dst_skill_dir/SKILL.md" ) ]]; then
        local skill_target
        skill_target="$(readlink "$dst_skill_dir/SKILL.md" 2>/dev/null || true)"
        if [[ "$skill_target" == "$f" || "$skill_target" == "$PLANAR_HOME/skills/gemini/"* ]]; then
          rm -f "$dst_skill_dir/SKILL.md"
          rmdir "$dst_skill_dir" 2>/dev/null || true
        elif [[ "$FORCE" -eq 1 ]]; then
          rm -rf "$dst_skill_dir"
        else
          err "$dst_skill_dir already exists and was not installed by Planar (rerun with --force to overwrite)"
        fi
      fi

      mkdir -p "$runtime_skill_dir"
      if [[ "$MODE" == "link" ]]; then
        symlink_to "$f" "$runtime_skill_dir/SKILL.md"
      else
        cp -f "$f" "$runtime_skill_dir/SKILL.md"
      fi
      mkdir -p "$dst_skill_dir"
      cp -f "$runtime_skill_dir/SKILL.md" "$dst_skill_dir/SKILL.md"
      printf '%s\n' "$f" > "$marker"
      count=$((count + 1))
    done < <(find "$src_dir" -maxdepth 1 -name 'pl-*.md' -print0)
    log "gemini: installed $count skill directories into $dst_dir"
  }

  # symlink_vendor_agents links every file in the rendered agents/<vendor>/
  # directory into the vendor's agents/ harness directory. Mirrors symlink_vendor
  # but uses a wildcard pattern that covers all file extensions (.md, .toml,
  # .agent.md) rather than the pl-*.md skill pattern.
  symlink_vendor_agents() {
    local name="$1" src_dir="$2" dst_dir="$3"
    if [[ ! -d "$src_dir" ]]; then
      warn "$name agents: source $src_dir not found, skipping"
      return 0
    fi
    mkdir -p "$dst_dir"
    local count=0
    while IFS= read -r -d '' f; do
      symlink_to "$f" "$dst_dir/$(basename "$f")"
      count=$((count + 1))
    done < <(find "$src_dir" -maxdepth 1 -type f -print0)
    log "$name agents: linked $count file(s) into $dst_dir"
  }

  symlink_agent_references() {
    local dst_dir="$1"
    mkdir -p "$dst_dir"
    while IFS= read -r -d '' f; do
      local dst="$dst_dir/$(basename "$f")"
      [[ -e "$dst" || -L "$dst" ]] && continue
      symlink_to "$f" "$dst"
    done < <(find -L "$PLANAR_HOME/agents" -maxdepth 1 -type f -name '*.md' -print0)
  }

  # prune_stale_vendor_agents removes destination entries that look like
  # Planar-installed agent symlinks but whose source counterpart no longer
  # exists. Mirrors prune_stale_vendor but covers all file extensions.
  prune_stale_vendor_agents() {
    local name="$1" src_dir="$2" dst_dir="$3"
    [[ "$NO_PRUNE" -eq 1 ]] && return 0
    [[ -d "$dst_dir" ]] || return 0
    local removed=0
    while IFS= read -r -d '' link; do
      local target
      target="$(readlink "$link" 2>/dev/null || true)"
      [[ -z "$target" ]] && continue                 # not a symlink — operator file
      [[ "$target" == "$src_dir/"* ]] || continue    # target outside Planar source — leave alone
      [[ -e "$target" ]] && continue                 # source still exists — keep
      vlog "$name agents: pruning stale symlink $(basename "$link") (source removed)"
      rm -f "$link"
      removed=$((removed + 1))
    done < <(find "$dst_dir" -maxdepth 1 -type f -print0)
    if [[ "$removed" -gt 0 ]]; then
      log "$name agents: pruned $removed stale entr$([[ $removed -eq 1 ]] && echo y || echo ies)"
    fi
  }

  # prune_stale_vendor removes destination entries that look like Planar-installed
  # symlinks but whose source counterpart no longer exists. Catches the rename
  # case: a previous `make install` created dst/pl-adopt.md → src/pl-adopt.md,
  # the source was later renamed to pl-import.md, and a fresh install creates
  # dst/pl-import.md alongside the now-orphaned dst/pl-adopt.md. Without pruning
  # both files persist; with pruning the orphan goes.
  #
  # Safety: only removes entries whose symlink target points into the Planar-
  # owned source directory ($src_dir/...). Regular files and symlinks pointing
  # elsewhere (operator-authored skills) are left alone. Pass --no-prune to
  # skip this pass.
  prune_stale_vendor() {
    local name="$1" src_dir="$2" dst_dir="$3"
    [[ "$NO_PRUNE" -eq 1 ]] && return 0
    [[ -d "$dst_dir" ]] || return 0
    local removed=0
    while IFS= read -r -d '' link; do
      local target
      target="$(readlink "$link" 2>/dev/null || true)"
      [[ -z "$target" ]] && continue                 # not a symlink — operator file
      [[ "$target" == "$src_dir/"* ]] || continue    # target outside Planar source — leave alone
      [[ -e "$target" ]] && continue                 # source still exists — keep
      vlog "$name: pruning stale symlink $(basename "$link") (source removed)"
      rm -f "$link"
      removed=$((removed + 1))
    done < <(find "$dst_dir" -maxdepth 1 -name 'pl-*.md' -print0)
    if [[ "$removed" -gt 0 ]]; then
      log "$name: pruned $removed stale entr$([[ $removed -eq 1 ]] && echo y || echo ies)"
    fi
  }

  # prune_stale_codex is the codex equivalent: each skill is a directory with
  # a .planar-source marker file (written by install_codex_vendor) recording
  # the source path. If the recorded source no longer exists, the directory
  # is stale and gets removed.
  prune_stale_codex() {
    local dst_dir="$1"
    [[ "$NO_PRUNE" -eq 1 ]] && return 0
    [[ -d "$dst_dir" ]] || return 0
    local removed=0
    while IFS= read -r -d '' skill_dir; do
      local marker="$skill_dir/.planar-source"
      [[ -f "$marker" ]] || continue                  # no marker — operator skill
      local marker_target
      marker_target="$(cat "$marker" 2>/dev/null || true)"
      [[ -n "$marker_target" ]] || continue
      [[ -e "$marker_target" ]] && continue           # source still exists — keep
      vlog "codex: pruning stale skill dir $(basename "$skill_dir") (source removed)"
      rm -rf "$skill_dir"
      removed=$((removed + 1))
    done < <(find "$dst_dir" -maxdepth 1 -name 'pl-*' -type d -print0)
    if [[ "$removed" -gt 0 ]]; then
      log "codex: pruned $removed stale skill director$([[ $removed -eq 1 ]] && echo y || echo ies)"
    fi
  }

  # prune_stale_copilot is the copilot equivalent: same directory-based layout
  # as codex, with .planar-source marker files for ownership tracking.
  prune_stale_copilot() {
    local dst_dir="$1"
    [[ "$NO_PRUNE" -eq 1 ]] && return 0
    [[ -d "$dst_dir" ]] || return 0
    local removed=0
    while IFS= read -r -d '' skill_dir; do
      local marker="$skill_dir/.planar-source"
      [[ -f "$marker" ]] || continue                  # no marker — operator skill
      local marker_target
      marker_target="$(cat "$marker" 2>/dev/null || true)"
      [[ -n "$marker_target" ]] || continue
      [[ -e "$marker_target" ]] && continue           # source still exists — keep
      vlog "copilot: pruning stale skill dir $(basename "$skill_dir") (source removed)"
      rm -rf "$skill_dir"
      removed=$((removed + 1))
    done < <(find "$dst_dir" -maxdepth 1 -name 'pl-*' -type d -print0)
    if [[ "$removed" -gt 0 ]]; then
      log "copilot: pruned $removed stale skill director$([[ $removed -eq 1 ]] && echo y || echo ies)"
    fi
  }

  prune_stale_gemini() {
    local dst_dir="$1"
    [[ "$NO_PRUNE" -eq 1 ]] && return 0
    [[ -d "$dst_dir" ]] || return 0
    local removed=0
    while IFS= read -r -d '' skill_dir; do
      local marker="$skill_dir/.planar-source"
      [[ -f "$marker" ]] || continue                  # no marker — operator skill
      local marker_target
      marker_target="$(cat "$marker" 2>/dev/null || true)"
      [[ -n "$marker_target" ]] || continue
      [[ -e "$marker_target" ]] && continue           # source still exists — keep
      vlog "gemini: pruning stale skill dir $(basename "$skill_dir") (source removed)"
      rm -rf "$skill_dir"
      removed=$((removed + 1))
    done < <(find "$dst_dir" -maxdepth 1 -name 'pl-*' -type d -print0)
    if [[ "$removed" -gt 0 ]]; then
      log "gemini: pruned $removed stale skill director$([[ $removed -eq 1 ]] && echo y || echo ies)"
    fi
  }

  IFS=',' read -r -a vendor_list <<< "$VENDORS"
  for v in "${vendor_list[@]}"; do
    case "$v" in
      claude)
        symlink_vendor "claude" "$PLANAR_HOME/commands/claude" "$HOME/.claude/commands"
        prune_stale_vendor "claude" "$PLANAR_HOME/commands/claude" "$HOME/.claude/commands"
        symlink_vendor_agents "claude" "$PLANAR_HOME/agents/claude" "$HOME/.claude/agents"
        symlink_agent_references "$HOME/.claude/agents"
        prune_stale_vendor_agents "claude" "$PLANAR_HOME/agents/claude" "$HOME/.claude/agents"
        ;;
      codex)
        install_codex_vendor "$PLANAR_HOME/skills/codex" "$PLANAR_HOME/codex-skills" "$CODEX_HOME/skills"
        prune_stale_codex "$CODEX_HOME/skills"
        symlink_vendor_agents "codex" "$PLANAR_HOME/agents/codex" "$CODEX_HOME/agents"
        symlink_agent_references "$CODEX_HOME/agents"
        prune_stale_vendor_agents "codex" "$PLANAR_HOME/agents/codex" "$CODEX_HOME/agents"
        ;;
      copilot)
        install_copilot_vendor "$PLANAR_HOME/skills/copilot" "$PLANAR_HOME/copilot-skills" "$HOME/.copilot/skills"
        prune_stale_copilot "$HOME/.copilot/skills"
        symlink_vendor_agents "copilot" "$PLANAR_HOME/agents/copilot" "$HOME/.copilot/agents"
        prune_stale_vendor_agents "copilot" "$PLANAR_HOME/agents/copilot" "$HOME/.copilot/agents"
        # Also link the copilot/ instructions+prompts root if present.
        if [[ -d "$PLANAR_HOME/copilot" ]]; then
          while IFS= read -r -d '' f; do
            symlink_to "$f" "$HOME/.copilot/$(basename "$f")"
          done < <(find "$PLANAR_HOME/copilot" -maxdepth 1 -type f -print0)
        fi
        ;;
      gemini)
        install_gemini_vendor "$PLANAR_HOME/skills/gemini" "$PLANAR_HOME/gemini-skills" "$HOME/.gemini/antigravity-cli/skills"
        prune_stale_gemini "$HOME/.gemini/antigravity-cli/skills"
        symlink_vendor_agents "gemini" "$PLANAR_HOME/agents/gemini" "$HOME/.gemini/antigravity-cli/agents"
        prune_stale_vendor_agents "gemini" "$PLANAR_HOME/agents/gemini" "$HOME/.gemini/antigravity-cli/agents"
        ;;
      *) warn "unknown vendor: $v (skipping)" ;;
    esac
  done
fi

# ---------- install authority ----------

# The versioned manifest is written only after every selected vendor has been
# wired successfully. Its rows are the complete managed-ownership set; files
# found only in vendor destinations (including personal local-* extensions)
# are deliberately excluded.
install_manifest_begin "${PLANAR_BUILD_ID:-unknown}" "$MODE"
if [[ -n "$VENDORS" ]]; then
  IFS=',' read -r -a vendor_list <<< "$VENDORS"
  for v in "${vendor_list[@]}"; do
    case "$v" in
      claude|codex|copilot|gemini)
        install_manifest_record_vendor "$v" "$PLANAR_HOME" "$HOME" "$CODEX_HOME"
        ;;
    esac
  done
fi
install_manifest_write "$PLANAR_HOME/install-manifest.json"
vlog "wrote $PLANAR_HOME/install-manifest.json (${#INSTALL_MANIFEST_ROW_VENDOR[@]} managed projections)"

# ---------- ownership stamp ----------

# Mark $PLANAR_HOME as a Planar-managed install. The prefix ownership guard
# reads this on re-install to distinguish "our tree" from a mis-typed --prefix.
printf 'planar-install %s\nbuild %s\n' "$INSTALLER_VERSION" "${PLANAR_BUILD_ID:-unknown}" > "$PLANAR_STAMP"

# A database the install itself created or replaced is private too.
harden_planar_home

# ---------- summary ----------

title "Install complete"

# Count the rendered role set (agents/claude/), not the top-level agents/ dir —
# the latter mixes in shared docs (doctrine, methodology, models). Both the
# rendered skills and agent roles are produced by the scriptorium render step
# above, so these reflect what actually got installed regardless of --vendors.
skills_n="$(count_glob "$PLANAR_HOME"/commands/claude/pl-*.md)"
agents_n="$(count_glob "$PLANAR_HOME"/agents/claude/*.md)"

ok "Planar ${PLANAR_BUILD_ID:-installed} → $PLANAR_HOME  ${C_DIM}(${SECONDS}s, $MODE mode)${C_RESET}"
log "binaries:   planar, planar-agent, planar-watch, planar-execute, planar-ext, scriptorium"
log "surfaces:   $skills_n skills · $agents_n agents · vendors: ${VENDORS:-none}"
if [[ "$WARN_COUNT" -gt 0 ]]; then
  printf '  %s!%s %s warning(s) above — review before first run\n' "$C_YELLOW" "$C_RESET" "$WARN_COUNT"
fi
printf '\n'
log "next:       planar init"
log "uninstall:  ./install.sh --uninstall"

# Print PATH instructions only when ~/.planar/bin is not already on PATH.
PLANAR_BIN_DIR="$PLANAR_HOME/bin"
if ! printf '%s' "$PATH" | tr ':' '\n' | grep -qx "$PLANAR_BIN_DIR"; then
  printf '\n'
  printf '  Add %s to your PATH so the planar command is available:\n' "$PLANAR_BIN_DIR"
  printf '\n'
  printf '    # bash / zsh — add to ~/.bashrc or ~/.zshrc:\n'
  printf '    export PATH="%s:$PATH"\n' "$PLANAR_BIN_DIR"
  printf '\n'
  printf '    # fish — run once:\n'
  printf '    fish_add_path %s\n' "$PLANAR_BIN_DIR"
  printf '\n'
  printf '  Then reload your shell (or open a new terminal) and run: planar init\n'
fi
