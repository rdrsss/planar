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
#     install-manifest.json           # placed vendor paths (extras) and staged paths
#     planar.db                       # created on first `planar init` (0600;
#                                     # the install root is 0700); also holds the
#                                     # host queue (plan 1089)
#     queue-logs/                     # detached queue-run output, beside planar.db
#     migrations/00001_foundation.up.sql  # canonical migration sources (also
#                                     # embedded into the binary at configure
#                                     # time via CMake codegen)
#     agents/                         # vendor-neutral agent role specs, staged
#                                     # (planar-*.md roles plus the shared docs)
#     skills/planar/                  # staged planar skill (SKILL.md + references/)
#     codex-agents/planar-*.toml      # Codex custom agents rendered from agents/
#     skills/src/pl-*.md              # unified authored skill sources
#     copilot/                        # Copilot instructions/prompts (only when
#                                     # the checkout has a copilot/ directory)
#     templates/                      # operator-editable defaults
#     workflows/                      # Lua workflows
#     scripts/validate-{barrel-modes,plan-status}-acceptance
#
# Then places the staged skill and agents into each vendor that is present on
# the host (a presence marker per vendor; --vendors narrows the set):
#
#   skills   ~/.claude/skills/planar                 Claude Code
#            ~/.agents/skills/planar                 Codex, Copilot, Gemini CLI, OpenCode (once)
#            ~/.gemini/antigravity-cli/skills/planar Antigravity
#   agents   ~/.claude/agents/planar-<role>.md
#            $CODEX_HOME/agents/planar-<role>.toml   (default ~/.codex)
#            ~/.copilot/agents/planar-<role>.agent.md
#            ~/.gemini/agents/planar-<role>.md
#            ~/.gemini/antigravity-cli/agents/planar-<role>.md
#            ~/.config/opencode/agents/planar-<role>.md  (frontmatter reduced)
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
#   ./install.sh                      # full install, every vendor found on this host
#   ./install.sh --no-vendor          # install Planar core only; skip vendor surfaces
#   ./install.sh --vendors claude     # install + place for Claude only (if present)
#   ./install.sh --vendors claude,codex
#   ./install.sh --link               # symlink from this repo instead of copying
#                                     #   (dev mode — edits to repo propagate)
#   ./install.sh --prefix /opt/planar # override ~/.planar
#   ./install.sh --force              # overwrite existing symlinks
#   ./install.sh --ignore-live-queue  # retire agent.db despite live old queue entries
#   ./install.sh --uninstall          # tear down everything install.sh created
#   ./install.sh --preset debug       # CMake preset (default release)
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
CODEX_HOME_EXPLICIT="${CODEX_HOME:-}"   # non-empty when the operator set CODEX_HOME: Codex's presence marker
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
VENDORS="claude,codex,copilot,gemini,antigravity,opencode"
VENDORS_EXPLICIT=0            # set with --vendors: naming an absent vendor then warns
MODE="copy"                   # copy | link
FORCE=0
UNINSTALL=0
IGNORE_LIVE_QUEUE=0           # set with --ignore-live-queue; the ONLY override of
                              # the agent.db live-queue guard (--force is not one)
# shellcheck disable=SC2034  # accepted for compatibility; stale-file pruning moves to the cleanup task
NO_PRUNE=0                    # set with --no-prune to skip stale-vendor-file removal
BUILD_PRESET="release"        # CMake preset
# The installer builds in its OWN directory, never the developer's
# build/<preset> (task 6537). Reusing it meant the install inherited whatever
# flags the cache happened to hold, and left PLANAR_VERSION_META=ON behind
# afterwards, invalidating the whole build graph on every subsequent commit.
BUILD_DIR=""                  # resolved below; override with --build-dir
VERBOSE=0                     # set with --verbose/-v for per-file detail
DRY_RUN=0                     # set with --dry-run/-n to preview without changes
INSTALLER_VERSION="1.0.0"     # install.sh's own version (see --version)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$REPO_ROOT/scripts/install-manifest.sh"
# The queue upgrade steps (plan 1089): probe and migrate the prefix planar.db,
# guard and retire the old agent.db. Sourced so the installer-manifest test
# and the post-build ctest cases drive the exact functions this script runs.
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
  --vendors LIST     Comma-separated filter over the vendors found on this host:
                     claude,codex,copilot,gemini,antigravity,opencode (default: all
                     of them; a vendor is placed only when its presence marker exists)
  --no-vendor        Install Planar core only; skip vendor surfaces
  --link             Symlink from this repo instead of copying (dev mode)
  --force            Overwrite existing symlinks / adopt a non-Planar prefix.
                     It does NOT bypass the agent.db live-queue guard.
  --ignore-live-queue
                     Retire (or uninstall) the old agent.db even while its
                     queue has live entries, or python3 cannot check it. The
                     old submitters keep an orphaned queue running outside
                     the new queue's slot count until they drain.
  --no-prune         Skip removal of stale vendor files
  --preset NAME      CMake build preset: debug|release (default: release)
  --build-dir DIR    Where to configure and build (default:
                     build/install-<preset>). The installer never builds in
                     the developer's build/<preset>.
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
    --vendors)    VENDORS="$2"; VENDORS_EXPLICIT=1; shift 2 ;;
    --no-vendor)  VENDORS=""; shift ;;
    --link)       MODE="link"; shift ;;
    --force)      FORCE=1; shift ;;
    --ignore-live-queue) IGNORE_LIVE_QUEUE=1; shift ;;
    --uninstall)  UNINSTALL=1; shift ;;
    --no-prune)   NO_PRUNE=1; shift ;;
    --preset)     BUILD_PRESET="$2"; shift 2 ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
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
# parity lane's build flags and left version-metadata stamping switched on
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

# harden_planar_home — make the install root private (plan 1089, tech spec
# 656 "File modes", which replaces the withdrawn decision 1210). planar.db
# stores task claim tokens, which authorise heartbeats and terminal verbs on a
# claim, so the install root is 0700 and the database (with its -wal/-shm
# sidecars) is 0600. Creates the root when absent, tightens an existing
# install in place, and touches nothing else: queue-logs/ is already created
# 0700 with 0600 logs by `queue run`, and a PLANAR_DB override outside the
# root is the operator's. A symlinked database is left alone rather than
# chmodding its target. The retired agent.db is not tightened: install
# removes it (queue_retire_store).
harden_planar_home() {
  mkdir -p "$PLANAR_HOME"
  # A chmod that fails is a warning, never an abort: a live -wal/-shm can be
  # checkpointed away between the test and the chmod, and a file the user can
  # write but does not own cannot be chmodded. Under `set -e` and the ERR trap
  # either would end the install.
  chmod 700 "$PLANAR_HOME" 2>/dev/null || warn "could not restrict $PLANAR_HOME to mode 700"
  local db
  for db in planar.db planar.db-wal planar.db-shm; do
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

  # The live-queue guard runs before anything is removed, --force or not:
  # uninstall removes agent.db (it is no longer preserved), so it must not
  # pull the store out from under a live old queue. This branch runs before
  # the dependency preflight, so the guard checks for python3 itself, and
  # only when agent.db exists. --ignore-live-queue is the only override.
  queue_live_guard uninstall || exit 1

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
    # $PLANAR_HOME except the preserved data entries (planar.db, its SQLite
    # sidecars, queue-logs/).
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
    #      uses (stamp / bin/planar / planar.db) must be present, unless
    #      --force overrides — mirroring the install-side guard exactly
    #      rather than inventing a second policy. The retired agent.db is no
    #      longer a signal (plan 1089): a prefix holding only it is refused.
    _planar_home_real="$(realpath "$PLANAR_HOME" 2>/dev/null || echo "$PLANAR_HOME")"
    _home_real="$(realpath "$HOME" 2>/dev/null || echo "$HOME")"
    if [[ "$_planar_home_real" == "$_home_real" ]]; then
      err "refusing to uninstall: PLANAR_HOME ($PLANAR_HOME) resolves to \$HOME ($HOME) — this would delete your home directory. Set --prefix to the actual Planar install root."
    fi
    if [[ ! -e "$PLANAR_HOME/.planar-install" && ! -x "$PLANAR_HOME/bin/planar" && ! -e "$PLANAR_HOME/planar.db" && "$FORCE" -ne 1 ]]; then
      err "$PLANAR_HOME does not look like a Planar install (no bin/planar, no planar.db, no .planar-install stamp). Refusing to remove it. Pass the correct --prefix, or re-run with --force to remove it anyway."
    fi

    log "removing install root: $PLANAR_HOME"
    log "(planar.db, its -wal/-shm sidecars and queue-logs/ are preserved if you have data — re-run with --force or rm manually; the retired agent.db is removed)"
    if [[ "$FORCE" -eq 1 ]]; then
      rm -rf "$PLANAR_HOME"
    else
      # Preserve the database and the queue's detached-run output; remove
      # everything else, the retired agent.db and its sidecars included (plan
      # 1089; the live-queue guard above already ran). planar.db's SQLite
      # sidecars (-wal / -shm) hold committed data not yet checkpointed into
      # the main file (it runs in WAL mode, so an unclean exit leaves them
      # behind), so they stay with it. A PLANAR_DB override lives wherever
      # the operator put it and is never touched here.
      find "$PLANAR_HOME" -mindepth 1 -maxdepth 1 \
        ! -name 'planar.db' ! -name 'planar.db-wal' ! -name 'planar.db-shm' \
        ! -name 'queue-logs' \
        -exec rm -rf {} +
    fi
  fi

  title "Uninstall complete."
  exit 0
fi

# ---------- preflight ----------

title "Planar — install from $REPO_ROOT"

# Live-queue preflight (plan 1089, tech spec 656 step 1). When the prefix
# still holds the retired agent.db, refuse before anything is built while its
# old queue has a live entry, or when the store cannot be read. It runs again
# immediately before agent.db is removed (step 4). --ignore-live-queue is the
# only override; --force is not one.
queue_live_guard preflight || exit 1

# Dependency manifest — keep in sync with INSTALL.md § Prerequisites and the
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
  "python3|python|CMake configure (the Python test runners); install.sh's agent.db retirement reader (scripts/install-lib/queue_retire.py); the counter-reset log helper (scripts/queue-logs-after-reset.py); the Codex agent TOML renderer (scripts/render-codex-agents.py)"
  "mktemp||capture the queue retirement probe's stderr (scripts/install-lib/queue-retire.sh)"
  "cp||copy install artifacts into place"
  "ln||symlink vendor surfaces"
  "mkdir||create the install tree"
  "rm||replace prior-install artifacts"
  "mv||atomically replace the install manifest"
  "find||walk vendor + template source trees"
  "head||take the first match in scripts/install-manifest.sh and scripts/install-lib/queue-retire.sh"
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
  "cmp||checks installed projection bytes in scripts/check-self-installed.sh (not run by the installer; the manifest-health task replaces it)"
  "git|git|repo discovery + 'planar import' (required at runtime)"
  "jq|jq|bundled agent skills parse 'planar … --json' output"
  "gh|gh|GitHub adapter auth + issue import (degrades gracefully)"
  "rg|ripgrep|agent-workflow code-search recipes (ripgrep)"
)

check_deps "build" 1 "${BUILD_DEPS[@]}"

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
  # (left behind by a non-force uninstall). Any one of them means "our tree".
  # The retired agent.db is not one (plan 1089).
  if [[ ! -e "$PLANAR_STAMP" && ! -x "$PLANAR_HOME/bin/planar" && ! -e "$PLANAR_HOME/planar.db" && "$FORCE" -ne 1 ]]; then
    err "$PLANAR_HOME exists and does not look like a Planar install (no bin/planar, no planar.db, no .planar-install stamp). Pass a clean --prefix, remove it, or re-run with --force to adopt it."
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
  if [[ -e "$PLANAR_HOME/agent.db" ]]; then
    log "re-check $PLANAR_HOME/agent.db for live queue entries, then retire it and its old numbered queue logs"
  fi
  log "wipe + re-place: agents/, scripts/, skills/, commands/, migrations/$([[ -d "$REPO_ROOT/copilot" ]] && echo ', copilot/')"
  log "stage skills/planar/ and agents/*.md; render the Codex agent TOML into $PLANAR_HOME/codex-agents"
  log "place templates/ (missing-only; --force overwrites)"
  if [[ -n "$VENDORS" ]]; then
    log "place the planar skill and agents for each vendor found among: $VENDORS (presence markers decide; see the vendor table in install.sh)"
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
# option was inherited from whatever last configured the directory (task
# 6537).
( cd "$REPO_ROOT" \
  && cmake --preset "$BUILD_PRESET" -B "$BUILD_DIR" \
       -DPLANAR_VERSION_META=ON \
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
# installed and nothing retired. Then re-check the old agent.db's queue
# immediately before removing anything (an entry may have appeared during the
# build), and retire it with its old numbered logs. See
# scripts/install-lib/queue-retire.sh.
title "Checking the queue store"
queue_probe_migrate || exit 1
queue_live_guard re-check || exit 1
queue_retire_store || exit 1

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

# ---------- stage the planar skill and agents ----------

# Stage the vendor-neutral skill and agents once, before any vendor placement:
# skills/planar/ (SKILL.md + references/) and the nineteen agents/*.md files
# (the fifteen planar-*.md roles and the four doctrine documents) are copied
# or linked per $MODE, and the Codex custom-agent TOML files are rendered from
# agents/ into codex-agents/. The TOML never goes under agents/codex/, which
# belongs to the retired render. agents/<vendor>/ subdirectories are left to
# the cleanup task.
title "Staging the planar skill and agents"
[[ -f "$REPO_ROOT/skills/planar/SKILL.md" ]] || err "skills/planar/SKILL.md not found in $REPO_ROOT (required to stage the planar skill)"
[[ -d "$REPO_ROOT/agents" ]] || err "agents/ not found in $REPO_ROOT (required to stage the planar agents)"

rm -rf "$PLANAR_HOME/skills/planar"
mkdir -p "$PLANAR_HOME/skills"
place "$REPO_ROOT/skills/planar" "$PLANAR_HOME/skills/planar"
log "skills/planar/ → $PLANAR_HOME/skills/planar ($MODE)"

mkdir -p "$PLANAR_HOME/agents"
_staged_agents=0
for f in "$REPO_ROOT"/agents/*.md; do
  [[ -f "$f" ]] || continue
  rm -f "$PLANAR_HOME/agents/$(basename "$f")"
  place "$f" "$PLANAR_HOME/agents/$(basename "$f")"
  vlog "agents/$(basename "$f") → $PLANAR_HOME/agents ($MODE)"
  _staged_agents=$((_staged_agents + 1))
done
log "agents/*.md → $PLANAR_HOME/agents ($_staged_agents files, $MODE)"

# The renderer exits 1 naming a malformed file and writes nothing. Render into
# a sibling directory and move it into place only on success, so a failure
# leaves no codex-agents/ behind and a prior good one intact.
_codex_new="$PLANAR_HOME/codex-agents.new"
rm -rf "$_codex_new"
if ! _codex_out="$(python3 "$REPO_ROOT/scripts/render-codex-agents.py" "$REPO_ROOT/agents" "$_codex_new" 2>&1)"; then
  rm -rf "$_codex_new"
  err "rendering the Codex agents failed: $_codex_out"
fi
rm -rf "$PLANAR_HOME/codex-agents"
mv "$_codex_new" "$PLANAR_HOME/codex-agents"
log "codex-agents/ ← agents/ ($(count_glob "$PLANAR_HOME"/codex-agents/*.toml) TOML files)"

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

# ---------- vendor surfaces ----------

# Six vendors, nine targets (plan 1104, M2). A vendor is placed only when its
# presence marker exists; the sources are the staged trees above:
#   skill   $PLANAR_HOME/skills/planar/            (a directory)
#   md      $PLANAR_HOME/agents/planar-<role>.md   (the fifteen role files only,
#                                                    never the doctrine documents)
#   toml    $PLANAR_HOME/codex-agents/planar-<role>.toml
#
# Presence markers (VENDOR_NAMES order is the order they are reported in):
#   claude       ~/.claude/ exists
#   codex        $CODEX_HOME is set, else ~/.codex/ exists
#   copilot      ~/.copilot/ exists
#   gemini       ~/.gemini/settings.json exists
#   antigravity  ~/.gemini/antigravity-cli/ exists
#   opencode     ~/.config/opencode/ exists
# Gemini CLI and Antigravity are detected independently. OpenCode has no skill
# row on purpose: it reads ~/.agents/skills and ~/.claude/skills, and Planar
# never writes ~/.config/opencode/skills.
VENDOR_NAMES=(claude codex copilot gemini antigravity opencode)

# The target table: owners|format|destination directory. A row is placed when at
# least one of its owners is present and selected, so the shared skill is placed
# once however many of its owners are found. The comment line directly above each
# row names the vendor documentation the target was checked against and the date;
# scripts/install-surface-test.sh fails when a row loses it.
VENDOR_TARGETS=(
  # https://code.claude.com/docs/en/skills (checked 2026-10-04)
  "claude|skill|$HOME/.claude/skills"
  # https://learn.chatgpt.com/docs/build-skills ; https://docs.github.com/en/copilot/concepts/agents/about-agent-skills ; https://geminicli.com/docs/cli/skills/ ; https://opencode.ai/docs/skills (checked 2026-10-04)
  "codex,copilot,gemini,opencode|skill|$HOME/.agents/skills"
  # https://antigravity.google/docs/skills (checked 2026-10-04)
  "antigravity|skill|$HOME/.gemini/antigravity-cli/skills"
  # https://code.claude.com/docs/en/sub-agents (checked 2026-10-04)
  "claude|md|$HOME/.claude/agents"
  # https://learn.chatgpt.com/docs/agent-configuration/subagents (checked 2026-10-04)
  "codex|toml|$CODEX_HOME/agents"
  # https://docs.github.com/en/copilot/reference/custom-agents-configuration (checked 2026-10-04)
  "copilot|copilot|$HOME/.copilot/agents"
  # https://geminicli.com/docs/core/subagents/ (checked 2026-10-04)
  "gemini|md|$HOME/.gemini/agents"
  # https://antigravity.google/docs/skills (checked 2026-10-04)
  "antigravity|md|$HOME/.gemini/antigravity-cli/agents"
  # https://opencode.ai/docs/agents (checked 2026-10-04)
  "opencode|opencode|$HOME/.config/opencode/agents"
)

# vendor_present <vendor> — the presence marker for one vendor.
vendor_present() {
  case "$1" in
    claude)      [[ -d "$HOME/.claude" ]] ;;
    codex)       [[ -n "$CODEX_HOME_EXPLICIT" || -d "$HOME/.codex" ]] ;;
    copilot)     [[ -d "$HOME/.copilot" ]] ;;
    gemini)      [[ -f "$HOME/.gemini/settings.json" ]] ;;
    antigravity) [[ -d "$HOME/.gemini/antigravity-cli" ]] ;;
    opencode)    [[ -d "$HOME/.config/opencode" ]] ;;
    *)           return 1 ;;
  esac
}

# vendor_marker <vendor> — the marker, for the skipped line.
vendor_marker() {
  # shellcheck disable=SC2088,SC2016  # a display string: the literal ~ and $CODEX_HOME are intended
  case "$1" in
    claude)      printf '~/.claude/' ;;
    codex)       printf '$CODEX_HOME or ~/.codex/' ;;
    copilot)     printf '~/.copilot/' ;;
    gemini)      printf '~/.gemini/settings.json' ;;
    antigravity) printf '~/.gemini/antigravity-cli/' ;;
    opencode)    printf '~/.config/opencode/' ;;
  esac
}

# prev_manifest_records <path> — true when the previous install manifest (still
# on disk: it is rewritten after placement) records <path> as a whole JSON string.
prev_manifest_records() {
  [[ -f "$PLANAR_HOME/install-manifest.json" ]] || return 1
  grep -Fq -- "$(install_manifest_json_quote "$1")" "$PLANAR_HOME/install-manifest.json"
}

# check_destination <path> — refuse a destination that cannot be proven Planar's:
# absent, a symlink into $PLANAR_HOME, or a path the previous manifest records.
# Never removes anything. Task 7218 formalizes this against the manifest.
check_destination() {
  local dst="$1" target
  if [[ -L "$dst" ]]; then
    target="$(readlink "$dst" 2>/dev/null || true)"
    [[ "$target" == "$PLANAR_HOME"/* ]] && return 0
  elif [[ ! -e "$dst" ]]; then
    return 0
  fi
  prev_manifest_records "$dst" && return 0
  err "$dst already exists and no Planar manifest records it, so Planar will not replace it (move or remove it, then re-run)"
}

# opencode_derive <staged agent .md> — print the OpenCode form of an agent: the
# frontmatter reduced to `description` and `mode: subagent`, the body unchanged.
# A description YAML would misread as a mapping or comment is double-quoted.
opencode_derive() {
  awk '
    NR == 1 { if ($0 != "---") exit 2; infm = 1; next }
    infm && $0 == "---" {
      if (desc == "") exit 3
      if (desc !~ /^["\047]/ && (desc ~ /: / || desc ~ / #/ || desc ~ /:$/ || desc ~ /^[-?:,\[\]{}#&*!|>%@`]/)) {
        gsub(/\\/, "\\\\", desc); gsub(/"/, "\\\"", desc); desc = "\"" desc "\""
      }
      print "---"; print "description: " desc; print "mode: subagent"; print "---"
      infm = 0; next
    }
    infm { if ($0 ~ /^description:/) { desc = $0; sub(/^description:[ \t]*/, "", desc) } ; next }
    { print }
    END { if (infm) exit 4 }
  ' "$1"
}

# place_one <src> <dst> <format> — put one file at <dst>. A Markdown agent and a
# Copilot agent follow $MODE (link = a symlink to the staged file, which for
# Copilot carries the .agent.md name; copy = a copy). An OpenCode agent is a
# derived file: it is always a regular file.
place_one() {
  local src="$1" dst="$2" fmt="$3"
  mkdir -p "$(dirname "$dst")"
  rm -f "$dst"   # never write through a link into $PLANAR_HOME
  if [[ "$fmt" == opencode ]]; then
    opencode_derive "$src" > "$dst" || err "could not derive the OpenCode agent from $src (frontmatter lacks a description?)"
  elif [[ "$MODE" == "link" ]]; then
    ln -s "$src" "$dst"
  else
    cp -f "$src" "$dst"
  fi
}

# Vendors that are selected by --vendors, and of those the ones found.
VENDORS_FOUND=()
VENDORS_SKIPPED=()
PLACED_PATHS=()          # every placed path, recorded in the manifest's extras
PLAN_SRC=(); PLAN_DST=(); PLAN_FMT=()

if [[ -n "$VENDORS" ]]; then
  title "Placing vendor surfaces"

  IFS=',' read -r -a _requested <<< "$VENDORS"
  for _v in "${_requested[@]}"; do
    [[ " ${VENDOR_NAMES[*]} " == *" $_v "* ]] || warn "unknown vendor: $_v (skipping)"
  done
  for _v in "${VENDOR_NAMES[@]}"; do
    if [[ ",$VENDORS," != *",$_v,"* ]]; then
      VENDORS_SKIPPED+=("$_v (not in --vendors)")
    elif vendor_present "$_v"; then
      VENDORS_FOUND+=("$_v")
      log "found $_v ($(vendor_marker "$_v"))"
    else
      VENDORS_SKIPPED+=("$_v (no $(vendor_marker "$_v"))")
      [[ "$VENDORS_EXPLICIT" -eq 1 ]] && warn "--vendors names $_v but it is not present ($(vendor_marker "$_v") is missing)"
    fi
  done
  log "vendors found:   ${VENDORS_FOUND[*]:-none}"
  _skipped_txt=""
  for _s in ${VENDORS_SKIPPED[@]+"${VENDORS_SKIPPED[@]}"}; do _skipped_txt+="${_skipped_txt:+, }$_s"; done
  log "vendors skipped: ${_skipped_txt:-none}"

  # Plan every placement first, so a foreign destination stops the install before
  # anything is written.
  for _row in "${VENDOR_TARGETS[@]}"; do
    IFS='|' read -r _owners _fmt _dir <<< "$_row"
    _active=0
    IFS=',' read -r -a _owner_list <<< "$_owners"
    for _o in "${_owner_list[@]}"; do
      [[ " ${VENDORS_FOUND[*]:-} " == *" $_o "* ]] && _active=1
    done
    [[ "$_active" -eq 1 ]] || continue
    case "$_fmt" in
      skill)
        PLAN_SRC+=("$PLANAR_HOME/skills/planar"); PLAN_DST+=("$_dir/planar"); PLAN_FMT+=(skill)
        ;;
      md|copilot|opencode)
        for _f in "$PLANAR_HOME"/agents/planar-*.md; do
          [[ -f "$_f" ]] || continue
          _name="$(basename "$_f")"
          [[ "$_fmt" == copilot ]] && _name="${_name%.md}.agent.md"
          PLAN_SRC+=("$_f"); PLAN_DST+=("$_dir/$_name"); PLAN_FMT+=("$_fmt")
        done
        ;;
      toml)
        for _f in "$PLANAR_HOME"/codex-agents/planar-*.toml; do
          [[ -f "$_f" ]] || continue
          PLAN_SRC+=("$_f"); PLAN_DST+=("$_dir/$(basename "$_f")"); PLAN_FMT+=(toml)
        done
        ;;
    esac
  done

  for ((_i = 0; _i < ${#PLAN_DST[@]}; _i++)); do
    check_destination "${PLAN_DST[$_i]}"
  done

  for ((_i = 0; _i < ${#PLAN_DST[@]}; _i++)); do
    _src="${PLAN_SRC[$_i]}"; _dst="${PLAN_DST[$_i]}"; _fmt="${PLAN_FMT[$_i]}"
    if [[ "$_fmt" == skill ]]; then
      # A skill is a directory: a link to the staged directory, or a copy of it.
      mkdir -p "$(dirname "$_dst")"
      rm -rf "$_dst"   # proven Planar's by check_destination
      place "$_src" "$_dst"
      PLACED_PATHS+=("$_dst")
      while IFS= read -r -d '' _f; do
        PLACED_PATHS+=("$_dst/${_f#"$_src"/}")
      done < <(find -L "$_src" -type f -print0)
      vlog "skill → $_dst ($MODE)"
    else
      place_one "$_src" "$_dst" "$_fmt"
      PLACED_PATHS+=("$_dst")
      vlog "agent → $_dst ($([[ "$_fmt" == opencode ]] && echo 'derived copy' || echo "$MODE"))"
    fi
  done
  log "placed ${#PLAN_DST[@]} target path(s): skill dirs and agent files follow $MODE; OpenCode agents are derived regular files"
fi

# ---------- install authority ----------

# The versioned manifest is written only after every placement has succeeded.
# Placed vendor paths are recorded in `extras` (absolute paths), not as
# `projections` rows: the installed-surface reader still validates projection
# rows against the four retired vendors, and the surface-reader task retargets it.
# `vendors` and `projections` stay empty until then. Files found only in vendor
# destinations (including personal local-* extensions) are deliberately excluded.
install_manifest_begin "${PLANAR_BUILD_ID:-unknown}" "$MODE"
install_manifest_record_staged "$PLANAR_HOME" "$REPO_ROOT"
for _p in ${PLACED_PATHS[@]+"${PLACED_PATHS[@]}"}; do install_manifest_add_extra "$_p"; done
install_manifest_write "$PLANAR_HOME/install-manifest.json"
vlog "wrote $PLANAR_HOME/install-manifest.json (${#INSTALL_MANIFEST_EXTRAS[@]} recorded paths)"

# ---------- ownership stamp ----------

# Mark $PLANAR_HOME as a Planar-managed install. The prefix ownership guard
# reads this on re-install to distinguish "our tree" from a mis-typed --prefix.
printf 'planar-install %s\nbuild %s\n' "$INSTALLER_VERSION" "${PLANAR_BUILD_ID:-unknown}" > "$PLANAR_STAMP"

# A database the install itself created or replaced is private too.
harden_planar_home

# ---------- summary ----------

title "Install complete"

# The staged role set (planar-*.md), not the top-level agents/ dir — the latter
# mixes in shared docs (doctrine, methodology, models).
agents_n="$(count_glob "$PLANAR_HOME"/agents/planar-*.md)"

ok "Planar ${PLANAR_BUILD_ID:-installed} → $PLANAR_HOME  ${C_DIM}(${SECONDS}s, $MODE mode)${C_RESET}"
log "binaries:   planar, planar-agent, planar-watch, planar-execute, planar-ext, scriptorium"
log "surfaces:   planar skill · $agents_n agents · vendors found: ${VENDORS_FOUND[*]:-none}"
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
