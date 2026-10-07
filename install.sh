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
#   ./install.sh --prebuilt DIR       # install an unpacked release bundle: no build,
#                                     #   no cmake/ninja/clang/python3 needed
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

# `${VAR-default}`, not `:-`: PLANAR_HOME set to the empty string must reach the
# prefix guard and be refused, never be silently replaced by the default.
PLANAR_HOME="${PLANAR_HOME-$HOME/.planar}"
CODEX_HOME_EXPLICIT="${CODEX_HOME:-}"   # non-empty when the operator set CODEX_HOME: Codex's presence marker
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
VENDORS="claude,codex,copilot,gemini,antigravity,opencode"
VENDORS_EXPLICIT=0            # set with --vendors: naming an absent vendor then warns
MODE="copy"                   # copy | link
PREBUILT_DIR=""               # set with --prebuilt DIR: the unpacked release bundle
PREBUILT=0                    # 1 when --prebuilt was passed
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
source "$REPO_ROOT/scripts/install-lib/install-manifest.sh"
# The queue upgrade steps (plan 1089): probe and migrate the prefix planar.db,
# guard and retire the old agent.db. Sourced so the installer-manifest test
# and the post-build ctest cases drive the exact functions this script runs.
source "$REPO_ROOT/scripts/install-lib/queue-retire.sh"
# The install-root guard shared with the uninstaller (plan 1122).
source "$REPO_ROOT/scripts/install-lib/prefix-guard.sh"
# The single list of data paths the installer never removes (plan 1122).
source "$REPO_ROOT/scripts/install-lib/data-paths.sh"
# The common mutation lock, the recovery journal's state machine and the
# database probe (plan 1122, "Order of an install").
source "$REPO_ROOT/scripts/install-lib/mutation-lock.sh"
source "$REPO_ROOT/scripts/install-lib/install-state.sh"
source "$REPO_ROOT/scripts/install-lib/db-probe.sh"

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
  --link             Symlink from this repo instead of copying (dev mode).
                     Refused with --prebuilt (exit 2).
  --prebuilt DIR     Install the unpacked release bundle in DIR instead of
                     building: take bin/, the skill, the agents, codex-agents/
                     and release.json from DIR. Needs only base system tools;
                     cmake, ninja, clang and python3 are not checked. An old
                     agent.db is moved, unread, to retired/<date>/.
  --cleanup DIR      Updater only: remove the updater's temporary directory DIR
                     when the install ends. Accepted only with --prebuilt and
                     the updater's ownership handoff (PLANAR_MUTATION_HANDOFF),
                     and only for exactly the directory that handoff recorded.
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
  --uninstall        Tear down everything install.sh created, except data
                     paths (planar.db, queue-logs/, workbench/, ...); --force
                     does not remove those either
  --version          Print the installer version and exit
  -h, --help         Show this help and exit

Output is colorized on a TTY; set NO_COLOR=1 (or pipe stdout) for plain text.
EOF
}

# ---------- parse args ----------

ORIG_ARGS=("$@")              # for the durable retry command
CLEANUP_DIR=""                # set with --cleanup DIR (the updater's handoff only)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)     PLANAR_HOME="$2"; shift 2 ;;
    --vendors)    VENDORS="$2"; VENDORS_EXPLICIT=1; shift 2 ;;
    --no-vendor)  VENDORS=""; shift ;;
    --link)       MODE="link"; shift ;;
    --prebuilt)
      if [[ $# -lt 2 ]]; then printf 'install.sh: --prebuilt needs a bundle directory\n' >&2; exit 64; fi
      PREBUILT=1; PREBUILT_DIR="$2"; shift 2 ;;
    --cleanup)
      if [[ $# -lt 2 ]]; then printf 'install.sh: --cleanup needs a directory\n' >&2; exit 64; fi
      CLEANUP_DIR="$2"; shift 2 ;;
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

# ---------- prebuilt bundle ----------

# SRC_ROOT is where every staged input is read from: the source checkout, or,
# with --prebuilt, the unpacked release bundle (tech spec 677, "Prebuilt install
# mode"). Everything below that reads skills/, agents/, migrations/,
# templates/, workflows/, scripts/ or install-cleanup.txt reads SRC_ROOT.
# The bundle is validated here, before the prefix guard and before anything is
# created: a refusal leaves no ~/.planar behind.
SRC_ROOT="$REPO_ROOT"
PLANAR_BINARIES=(planar planar-agent planar-watch planar-execute planar-ext)
if [[ "$PREBUILT" -eq 0 && ( -n "$CLEANUP_DIR" || -n "${PLANAR_MUTATION_HANDOFF-}" ) ]]; then
  printf 'install.sh: --cleanup and the updater handoff (PLANAR_MUTATION_HANDOFF) are accepted only with --prebuilt\n' >&2
  exit 2
fi
if [[ "$PREBUILT" -eq 1 ]]; then
  if [[ "$MODE" == "link" ]]; then
    printf 'install.sh: --link cannot be used with --prebuilt: link mode would symlink the install into the bundle directory %s, which the caller deletes after the install\n' "$PREBUILT_DIR" >&2
    exit 2
  fi
  if [[ ! -d "$PREBUILT_DIR" ]]; then
    printf 'install.sh: --prebuilt %s: not a directory\n' "$PREBUILT_DIR" >&2
    exit 1
  fi
  SRC_ROOT="$(cd "$PREBUILT_DIR" && pwd -P)"
  _bundle_missing=()
  for _b in "${PLANAR_BINARIES[@]}"; do
    [[ -f "$SRC_ROOT/bin/$_b" && -x "$SRC_ROOT/bin/$_b" ]] || _bundle_missing+=("bin/$_b")
  done
  [[ -f "$SRC_ROOT/release.json" ]] || _bundle_missing+=("release.json")
  [[ -f "$SRC_ROOT/skills/planar/SKILL.md" ]] || _bundle_missing+=("skills/planar/SKILL.md")
  [[ -d "$SRC_ROOT/agents" ]] || _bundle_missing+=("agents/")
  _bundle_toml=0
  for _b in "$SRC_ROOT"/codex-agents/planar-*.toml; do [[ -f "$_b" ]] && _bundle_toml=1; done
  [[ "$_bundle_toml" -eq 1 ]] || _bundle_missing+=("codex-agents/planar-*.toml")
  # Every managed subtree is always shipped; an absent one is a broken bundle.
  for _b in workflows migrations scripts/install-lib; do
    [[ -d "$SRC_ROOT/$_b" ]] || _bundle_missing+=("$_b/")
  done
  if [[ ${#_bundle_missing[@]} -gt 0 ]]; then
    printf 'install.sh: the prebuilt directory %s is not a complete release bundle; missing:\n' "$SRC_ROOT" >&2
    for _b in "${_bundle_missing[@]}"; do printf '  %s\n' "$_b" >&2; done
    exit 1
  fi
else
  # A source install stages every managed subtree from the checkout; one whose
  # source directory is absent is an incomplete checkout, refused before any write.
  _src_missing=()
  for _b in skills/planar agents scripts/install-lib workflows migrations; do
    [[ -d "$SRC_ROOT/$_b" ]] || _src_missing+=("$_b/")
  done
  if [[ ${#_src_missing[@]} -gt 0 ]]; then
    printf 'install.sh: the checkout %s is incomplete; missing:\n' "$SRC_ROOT" >&2
    for _b in "${_src_missing[@]}"; do printf '  %s\n' "$_b" >&2; done
    exit 1
  fi
fi

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
# rm_managed <path...> — remove a directory tree the installer owns and re-stages,
# refusing when it is, lies inside, or holds a data path (a relocated data path
# can sit inside a managed tree). Data paths are never removed by an install.
rm_managed() {
  local p
  for p in "$@"; do
    if planar_removal_blocked "$ROOT_C" "$p"; then
      err "refusing to remove $p: it holds or is the data path '$PLANAR_DATA_PATH_HIT', which an install never removes. Move that data path out of the installer's trees."
    fi
    if [[ -L "$p" ]]; then rm -f "$p"; else rm -rf "$p"; fi
  done
}

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

# ---------- shared helpers: previous-projection sweep and OpenCode form ----------

SWEEP_REMOVED=0
SWEEP_LEFT=0

# sweep_remove <path> <what> — remove one proven entry and say so.
sweep_remove() {
  local path="$1" what="$2"
  if [[ -L "$path" ]]; then rm -f "$path"; else rm -rf "$path"; fi
  log "removed $what: $path"
  SWEEP_REMOVED=$((SWEEP_REMOVED + 1))
}

# sweep_link_into <link> <glob-prefix...> — true when <link> is a symlink (live
# or dangling: readlink, not -e) whose target matches one of the prefixes.
sweep_link_into() {
  local link="$1" target pat; shift
  [[ -L "$link" ]] || return 1
  target="$(readlink "$link" 2>/dev/null || true)"
  for pat in "$@"; do
    # shellcheck disable=SC2053  # the prefix is a glob on purpose
    [[ "$target" == $pat ]] && return 0
  done
  return 1
}

# sweep_commands <dir> — the retired ~/.claude/commands symlinks.
sweep_commands() {
  local dir="$1" link
  [[ -d "$dir" ]] || return 0
  while IFS= read -r -d '' link; do
    if sweep_link_into "$link" "$PLANAR_HOME/*" "$HOME/.planar/local/*"; then
      sweep_remove "$link" "command symlink"
    fi
  done < <(find "$dir" -maxdepth 1 -type l \( -name 'pl-*.md' -o -name 'local-*.md' \) -print0)
}

# sweep_retired_toplevel_agent <link> — true when <link> points at a direct
# child of $PLANAR_HOME/agents/ whose name is not planar-*: the layout an older
# release linked vendor agents to before roles were prefixed. The current
# layout links only to agents/planar-<role>.md, so it never matches. Live and
# dangling links both match (readlink, not -e).
sweep_retired_toplevel_agent() {
  local link="$1" target rest
  [[ -L "$link" ]] || return 1
  target="$(readlink "$link" 2>/dev/null || true)"
  [[ "$target" == "$PLANAR_HOME/agents/"* ]] || return 1
  rest="${target#"$PLANAR_HOME/agents/"}"
  [[ -n "$rest" && "$rest" != */* && "$rest" != planar-* ]]
}

# sweep_agents <dir> — the agent symlinks older installs placed: the unprefixed
# render output under $PLANAR_HOME/agents/<vendor>/, and the still older links
# to top-level $PLANAR_HOME/agents/<role>.md. A symlink to a top-level
# agents/planar-<role>.md (link mode, the current layout) is not matched.
sweep_agents() {
  local dir="$1" link v
  local -a pats=()
  [[ -d "$dir" ]] || return 0
  for v in claude codex copilot gemini antigravity opencode; do pats+=("$PLANAR_HOME/agents/$v/*"); done
  while IFS= read -r -d '' link; do
    if sweep_link_into "$link" "${pats[@]}" || sweep_retired_toplevel_agent "$link"; then
      sweep_remove "$link" "agent symlink"
    fi
  done < <(find "$dir" -maxdepth 1 -type l -print0)
}

# sweep_skills <dir> <staged-dir> — the pl-* skill directories an older install
# copied or linked into <dir>. Removed when a symlink into $PLANAR_HOME, or when
# the bytes equal <staged-dir>/<same name> (the old runtime marker file
# .planar-source is Planar's own and ignored in the comparison). A pl-* directory
# with neither proof is left and reported. Other entries are ignored.
sweep_skills() {
  local dir="$1" staged="$2" entry name
  [[ -d "$dir" ]] || return 0
  while IFS= read -r -d '' entry; do
    name="$(basename "$entry")"
    if [[ -L "$entry" ]]; then
      if sweep_link_into "$entry" "$PLANAR_HOME/*"; then sweep_remove "$entry" "skill symlink"; fi
    elif [[ -d "$entry" ]]; then
      if [[ -d "$staged/$name" ]] && diff -r -x .planar-source "$staged/$name/" "$entry/" >/dev/null 2>&1; then
        sweep_remove "$entry" "skill copy"
      else
        warn "left $entry: could not prove ownership (no byte-identical staged copy at $staged/$name)"
        SWEEP_LEFT=$((SWEEP_LEFT + 1))
      fi
    fi
  done < <(find "$dir" -maxdepth 1 -name 'pl-*' -print0)
}

# run_sweep — the sweep over every vendor directory that exists.
run_sweep() {
  local _d
  sweep_commands "$HOME/.claude/commands"
  for _d in "$HOME/.claude/agents" "$CODEX_HOME/agents" "$HOME/.copilot/agents" "$HOME/.gemini/agents" \
            "$HOME/.gemini/antigravity-cli/agents" "$HOME/.config/opencode/agents"; do
    sweep_agents "$_d"
  done
  sweep_skills "$CODEX_HOME/skills" "$PLANAR_HOME/codex-skills"
  sweep_skills "$HOME/.copilot/skills" "$PLANAR_HOME/copilot-skills"
  sweep_skills "$HOME/.gemini/antigravity-cli/skills" "$PLANAR_HOME/gemini-skills"
  sweep_skills "$HOME/.config/opencode/skills" "$PLANAR_HOME/opencode-skills"
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

# uninstall_owned <staged> <installed> <vendor> <kind> — <installed> is still
# what Planar placed: a symlink into $PLANAR_HOME, or content equal to <staged>
# (an OpenCode agent is compared with its derived form).
uninstall_owned() {
  local staged="$1" installed="$2" vendor="$3" kind="$4" tmp rc=0
  if [[ -L "$installed" ]]; then
    [[ "$(readlink "$installed" 2>/dev/null || true)" == "$PLANAR_HOME"/* ]]
    return
  fi
  if [[ "$kind" == skill ]]; then
    [[ -d "$installed" && -d "$staged" ]] && diff -r "$staged/" "$installed/" >/dev/null 2>&1
    return
  fi
  [[ -f "$installed" && -f "$staged" ]] || return 1
  if [[ "$vendor" == opencode ]]; then
    tmp="$(mktemp)"
    if opencode_derive "$staged" > "$tmp" 2>/dev/null; then cmp -s "$tmp" "$installed" || rc=1; else rc=1; fi
    rm -f "$tmp"
    return "$rc"
  fi
  cmp -s "$staged" "$installed"
}

# uninstall_recorded_targets — remove each projection the manifest records that
# is still Planar's, then report what is left under a planar name.
uninstall_recorded_targets() {
  local manifest="$PLANAR_HOME/install-manifest.json" staged installed vendor kind d e
  local removed=0 listing
  if [[ -f "$manifest" ]]; then
    if command -v python3 >/dev/null 2>&1; then
      listing="$(python3 - "$manifest" <<'PY' || true
import json, sys
try:
    m = json.load(open(sys.argv[1], encoding='utf-8'))
except Exception:
    sys.exit(0)
for r in m.get('projections', []):
    if all(isinstance(r.get(k), str) for k in ('staged_path', 'installed_path', 'vendor', 'kind')):
        sys.stdout.write('\t'.join((r['staged_path'], r['installed_path'], r['vendor'], r['kind'])) + '\n')
PY
)"
      while IFS=$'\t' read -r staged installed vendor kind; do
        [[ -n "$installed" ]] || continue
        [[ -e "$installed" || -L "$installed" ]] || continue
        if uninstall_owned "$staged" "$installed" "$vendor" "$kind"; then
          if [[ -L "$installed" ]]; then rm -f "$installed"; else rm -rf "$installed"; fi
          log "removed recorded target: $installed"
          removed=$((removed + 1))
        fi
      done <<< "$listing"
    else
      warn "python3 not found: cannot read $manifest, so the placed vendor targets are not removed"
    fi
  fi
  log "removed $removed recorded vendor target(s)"
  for d in "$HOME/.claude/skills" "$HOME/.agents/skills" "$HOME/.gemini/antigravity-cli/skills" \
           "$HOME/.claude/agents" "$CODEX_HOME/agents" "$HOME/.copilot/agents" "$HOME/.gemini/agents" \
           "$HOME/.gemini/antigravity-cli/agents" "$HOME/.config/opencode/agents"; do
    [[ -d "$d" ]] || continue
    while IFS= read -r -d '' e; do
      warn "left $e: Planar did not place it as recorded"
    done < <(find "$d" -maxdepth 1 \( -name 'planar' -o -name 'planar-*' \) -print0)
  done
}

# ---------- prefix guard ----------

# Runs before anything is removed, created or built, for install and
# --uninstall alike (scripts/install-lib/prefix-guard.sh). An install root that
# is the empty string, /, or $HOME exits 2 even with --force; a non-empty root
# with no Planar sign exits 1 unless --force adopts it.
_guard_op="install"
[[ "$UNINSTALL" -eq 1 ]] && _guard_op="uninstall"
_guard_rc=0
planar_prefix_guard "$PLANAR_HOME" "$FORCE" "$_guard_op" || _guard_rc=$?
[[ "$_guard_rc" -eq 0 ]] || exit "$_guard_rc"
# Every removal, the lock, the journal and the staging are keyed on the
# canonical root, whatever spelling of it the operator gave.
ROOT_C="$PLANAR_PREFIX_CANON"
case "$ROOT_C" in *$'\n'*|*$'\t'*) printf 'install.sh: the install root %s holds a newline or tab; refusing\n' "$ROOT_C" >&2; exit 2 ;; esac
HOME_PLANAR_C="$(trap - ERR; planar_canonical_path "$HOME/.planar" 2>/dev/null || printf '%s' "$HOME/.planar")"

# ---------- uninstall path ----------

if [[ "$UNINSTALL" -eq 1 ]]; then
  title "Uninstalling Planar"
  planar_data_paths_report "$PLANAR_HOME" | while IFS= read -r _l; do log "$_l"; done

  # Common mutation ownership (decision 1328), held until every removal has
  # finished. Every check that can still refuse runs under the lock BEFORE the
  # journal records `uninstalling`: that record ends any pending install for
  # good (no later install replays it), so it is written only when removals
  # begin. A refused uninstall leaves the journal as it found it.
  if [[ -d "$ROOT_C" ]]; then
    planar_lock_acquire "$ROOT_C" uninstall || { printf '\ninstall.sh: %s\n' "$PLANAR_LOCK_ERROR" >&2; exit 1; }
    trap 'planar_lock_release' EXIT
    if [[ -e "$ROOT_C/.planar-journal" || -L "$ROOT_C/.planar-journal" ]]; then
      planar_journal_load "$ROOT_C" \
        || err "$ROOT_C/.planar-journal is not a valid recovery journal; refusing to uninstall around it (it is kept)"
    else
      planar_journal_clear
    fi
  fi

  # The live-queue guard runs before anything is removed, --force or not:
  # uninstall removes agent.db (it is no longer preserved), so it must not
  # pull the store out from under a live old queue. This branch runs before
  # the dependency preflight, so the guard checks for python3 itself, and
  # only when agent.db exists. --ignore-live-queue is the only override.
  queue_live_guard uninstall || exit 1

  # Removals begin: record the cancellation. Nothing below refuses.
  if [[ -d "$ROOT_C" ]]; then
    J_phase=uninstalling
    J_operation=uninstall
    J_owner_pid="$$"
    J_owner_start="$(trap - ERR; planar_lock_start_token "$$")" || J_owner_start=unknown
    J_owner_lock="$PLANAR_LOCK_GEN"
    J_retry="cd $(printf '%q' "$REPO_ROOT") && ./install.sh --uninstall$([[ "$ROOT_C" == "$HOME_PLANAR_C" ]] || printf ' --prefix %q' "$ROOT_C")"
    planar_journal_write "$ROOT_C" || err "cannot record the uninstall in $ROOT_C/.planar-journal"
  fi

  # The targets this installer places (plan 1104, M2): every `installed_path`
  # the manifest's projection rows record, removed only while it is still what
  # Planar placed (a symlink into $PLANAR_HOME, or bytes equal to the staged
  # source). Only the `planar` entries go: no vendor directory and not
  # ~/.agents/skills is removed. Anything else named planar or planar-* in those
  # directories is left and reported. The previous skill and agent projections
  # an older install made are swept the same way as on install.
  uninstall_recorded_targets
  run_sweep
  log "retired $SWEEP_REMOVED previous projection(s); left $SWEEP_LEFT that could not be proven Planar's"

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
    log "removing everything under the install root except data paths: $PLANAR_HOME"
    log "(data paths are preserved, --force included: $(planar_data_path_names | tr '\n' ' ')the retired agent.db is removed)"
    # Remove each top-level entry unless it is a data path or holds one (a
    # relocated data path can sit inside a managed tree). The retired agent.db
    # and its sidecars are removed (plan 1089; the live-queue guard above
    # already ran). planar.db's SQLite sidecars hold committed data not yet
    # checkpointed into the main file, so they stay with it.
    while IFS= read -r -d '' _entry; do
      [[ "$_entry" == "$PLANAR_HOME/.planar-journal" ]] && continue
      if planar_removal_blocked "$PLANAR_HOME" "$_entry"; then
        log "kept $_entry (data path '$PLANAR_DATA_PATH_HIT')"
        continue
      fi
      rm -rf "$_entry"
    done < <(find "$PLANAR_HOME" -mindepth 1 -maxdepth 1 -print0)
    # The cancellation journal goes last, after every removal; an install
    # root left empty (no data path survived) goes too.
    rm -f "$PLANAR_HOME/.planar-journal"
    rmdir "$PLANAR_HOME" 2>/dev/null || true
  fi

  title "Uninstall complete."
  exit 0
fi

# ---------- preflight ----------

if [[ "$PREBUILT" -eq 1 ]]; then
  title "Planar — install the prebuilt bundle $SRC_ROOT"
else
  title "Planar — install from $REPO_ROOT"
fi
planar_data_paths_report "$PLANAR_HOME" | while IFS= read -r _l; do log "$_l"; done

# Live-queue preflight (plan 1089, tech spec 656 step 1). When the prefix
# still holds the retired agent.db, refuse before anything is built while its
# old queue has a live entry, or when the store cannot be read. It runs again
# immediately before agent.db is removed (step 4). --ignore-live-queue is the
# only override; --force is not one.
# The prebuilt path never reads agent.db (no python3 there): it moves the file
# aside, unread, once the queue store probe passes.
if [[ "$PREBUILT" -eq 0 ]]; then
  queue_live_guard preflight || exit 1
fi

# Dependency manifest — keep in sync with INSTALL.md § Prerequisites and the
# CLAUDE.md "external tool dependencies" rule. Format: "cmd|brewpkg|what for"
# (empty brewpkg = base system tool, no Homebrew hint).
#
# toolchain_deps: the source build needs these; a miss aborts a source install.
#             The prebuilt path (--prebuilt) never checks them.
# base_deps:  the installer itself shells these on every path; a miss aborts
#             the install. A prebuilt install checks only this tier.
# run_deps:   Planar (the binary + bundled agent skills) needs these at run
#             time; a miss only warns — the install still produces a binary.
TOOLCHAIN_DEPS=(
  "cmake|cmake|configures, builds, and installs the five Planar binaries"
  "ninja|ninja|C++26 module dependency scanning"
  "/opt/homebrew/opt/llvm/bin/clang|llvm|pinned LLVM C compiler required by CMakePresets.json"
  "/opt/homebrew/opt/llvm/bin/clang++|llvm|pinned LLVM C++ compiler required by CMakePresets.json"
  "python3|python|CMake configure (the Python test runners); install.sh's agent.db retirement reader (scripts/install-lib/queue_retire.py); the counter-reset log helper (scripts/queue-logs-after-reset.py); the Codex agent TOML renderer (scripts/render-codex-agents.py)"
)
BASE_DEPS=(
  "mktemp||capture the queue retirement probe's stderr (scripts/install-lib/queue-retire.sh)"
  "cp||copy install artifacts into place"
  "ln||symlink vendor surfaces"
  "mkdir||create the install tree"
  "rm||replace prior-install artifacts"
  "mv||atomically replace the install manifest"
  "cmp||skip a manifest or vendor target that is already identical"
  "sort||order the manifest's recorded paths so its bytes are stable"
  "diff||compare a copied vendor skill directory with the staged one"
  "find||walk vendor + template source trees"
  "head||take the first match in scripts/install-lib/install-manifest.sh and scripts/install-lib/queue-retire.sh"
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
  "date||name retired/<date>/ when an old agent.db is moved aside (scripts/install-lib/queue-retire.sh)"
  "uname||name the host os and arch in the release record the source install writes (release.json)"
  "realpath||resolve the install root and its data paths (scripts/install-lib/prefix-guard.sh, scripts/install-lib/data-paths.sh)"
  "od||read random bytes for the mutation-lock nonce and the staging name (scripts/install-lib/mutation-lock.sh)"
  "ps||read a lock owner's start time where there is no /proc, so a reused pid is not mistaken for a live owner (scripts/install-lib/mutation-lock.sh)"
  "sed||read release.json, the recovery records and the database probe's diagnostics"
  "wc||check the recovery journal's size (scripts/install-lib/journal.sh)"
  "cut||shorten the staging token (scripts/install-lib/install-state.sh)"
  "sleep||wait at a paused test fault point (scripts/install-lib/install-state.sh; test-only)"
  "sync||flush the recovery journal's mutating and complete records to disk (install.sh)"
)
BUILD_DEPS=("${TOOLCHAIN_DEPS[@]}" "${BASE_DEPS[@]}")
RUN_DEPS=(
  "cmp||compares staged and installed bytes in the installer (also a build dep above)"
  "git|git|repo discovery + 'planar import' (required at runtime)"
  "jq|jq|bundled agent skills parse 'planar … --json' output"
  "gh|gh|GitHub adapter auth + issue import (degrades gracefully)"
  "rg|ripgrep|agent-workflow code-search recipes (ripgrep)"
)

if [[ "$PREBUILT" -eq 1 ]]; then
  check_deps "base" 1 "${BASE_DEPS[@]}"
else
  check_deps "build" 1 "${BUILD_DEPS[@]}"

  # Check that we are in a CMake Planar source checkout.
  [[ -f "$REPO_ROOT/CMakeLists.txt" ]] || err "CMakeLists.txt not found in $REPO_ROOT (run install.sh from the Planar source repo)"
  [[ -f "$REPO_ROOT/CMakePresets.json" ]] || err "CMakePresets.json not found in $REPO_ROOT"
fi


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
if [[ "$PREBUILT" -eq 1 ]]; then
  log "bundle      = $SRC_ROOT (prebuilt: no build)"
else
  log "preset      = $BUILD_PRESET"
  log "build dir   = $BUILD_DIR"
fi
[[ "$DRY_RUN" -eq 1 ]] && log "dry-run     = yes (no changes will be made)"

# Writability — the install writes into $PLANAR_HOME (or creates it). Fail with
# a clear message now rather than a raw mkdir error mid-build.
if [[ -e "$PLANAR_HOME" ]]; then
  [[ -w "$PLANAR_HOME" ]] || err "cannot write to $PLANAR_HOME (permissions?)"
else
  _parent="$(dirname "$PLANAR_HOME")"
  [[ -d "$_parent" && -w "$_parent" ]] || err "cannot create $PLANAR_HOME — $_parent is not writable (permissions?)"
fi

PLANAR_STAMP="$PLANAR_HOME/.planar-install"

# Dry run — everything above is read-only preflight; stop here and print the
# plan rather than mutating anything.
if [[ "$DRY_RUN" -eq 1 ]]; then
  _cleanup_n=0
  [[ -f "$SRC_ROOT/install-cleanup.txt" ]] && \
    _cleanup_n="$(grep -cE '^[[:space:]]*[^#[:space:]]' "$SRC_ROOT/install-cleanup.txt" || true)"
  title "Dry run — planned actions"
  if [[ "$PREBUILT" -eq 1 ]]; then
    log "copy 5 Planar binaries from $SRC_ROOT/bin → $PLANAR_HOME/bin  [prebuilt: nothing is built]"
  else
    log "build 5 Planar binaries → $PLANAR_HOME/bin  [preset=$BUILD_PRESET]"
  fi
  log "take the mutation lock $(planar_lock_dir "$ROOT_C") and record a recovery journal in $ROOT_C/.planar-journal"
  if [[ -e "$ROOT_C/.planar-journal" ]]; then
    if planar_journal_load "$ROOT_C"; then
      log "recover first: a previous install left its journal in phase $J_phase"
    else
      log "stop: $ROOT_C/.planar-journal is not a valid recovery journal"
    fi
  fi
  log "stage every managed subtree under $ROOT_C/.staging-<token>/, then probe the database with the staged binaries (an ahead or unusable database stops the install before any change)"
  log "run cleanup manifest: $_cleanup_n path(s) checked for removal"
  log "swap bin/, skills/, agents/, codex-agents/, workflows/, scripts/ and migrations/ in through <name>.old"
  log "create a missing database, or migrate a behind one, with the installed planar init --skip-project --allow-no-repo"
  if [[ -e "$PLANAR_HOME/agent.db" && "$PREBUILT" -eq 1 ]]; then
    log "move $PLANAR_HOME/agent.db, its sidecars and the old numbered queue logs, unread, into $PLANAR_HOME/retired/<date>/"
  elif [[ -e "$PLANAR_HOME/agent.db" ]]; then
    log "re-check $PLANAR_HOME/agent.db for live queue entries, then retire it and its old numbered queue logs"
  fi
  if [[ "$PREBUILT" -eq 1 ]]; then
    log "stage skills/planar/ and agents/*.md; copy the bundle's codex-agents/ and release.json into $PLANAR_HOME"
  else
    log "stage skills/planar/ and agents/*.md; render the Codex agent TOML into $PLANAR_HOME/codex-agents; write $PLANAR_HOME/release.json"
  fi
  log "place templates/ (missing-only, --force included); write release.json, then the install stamp last"
  if [[ -n "$VENDORS" ]]; then
    log "place the planar skill and agents for each vendor found among: $VENDORS (presence markers decide; see the vendor table in install.sh)"
  else
    log "vendor surfaces: skipped (--no-vendor)"
  fi
  printf '\n'
  log "Re-run without --dry-run to apply."
  exit 0
fi

# ---------- install state helpers ----------

# The managed subtrees: each is staged complete, then swapped in by two renames
# (scripts/install-lib/install-state.sh). templates/ is a data path and is
# placed missing-only after the swap. Every subtree is always shipped (an absent
# one is refused above); commands/ and copilot/ are retired paths listed in
# install-cleanup.txt.
PLANAR_JOURNAL_SUBTREES="bin skills agents codex-agents workflows scripts migrations"
DEFAULT_RELEASE_BASE="https://github.com/rdrsss/planar/releases"

# release_field FILE KEY -- one value of a release.json written one key per line.
release_field() {
  sed -n "s/^[[:space:]]*\"$2\":[[:space:]]*\"\{0,1\}\([^\",]*\)\"\{0,1\},\{0,1\}[[:space:]]*\$/\1/p" "$1" 2>/dev/null | head -n 1
}

# release_base_valid URL -- the bootstrap's release-base grammar: https, a local
# file:// fixture, or http on the exact loopback hosts, with no userinfo, quote
# or blank.
release_base_valid() {
  [[ "$1" =~ ^(https://[A-Za-z0-9.-]+(:[0-9]+)?(/[^[:space:]\"\'@]*)?|file:///[^[:space:]\"\'@]*|http://(127\.0\.0\.1|localhost)(:[0-9]+)?(/[^[:space:]\"\'@]*)?)$ ]]
}

# quote_args ARG... -- the arguments as one shell-safe line.
quote_args() {
  local a out=""
  for a in "$@"; do out="$out${out:+ }$(printf '%q' "$a")"; done
  printf '%s' "$out"
}

# retry_command -- the durable command that completes this transaction. A
# release install names the version-pinned public bootstrap (never the updater's
# temporary directory, which cleanup removes); a source install names the
# checkout it ran from.
retry_command() {
  local envs="" args=() a skip=0
  for a in ${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}; do
    if [[ "$skip" -eq 1 ]]; then skip=0; continue; fi
    case "$a" in
      --cleanup|--prebuilt) skip=1; continue ;;
    esac
    args+=("$a")
  done
  if [[ "$PREBUILT" -eq 0 ]]; then
    printf 'cd %s && ./install.sh%s' "$(printf '%q' "$REPO_ROOT")" "${args[*]+ $(quote_args "${args[@]}")}"
    return 0
  fi
  if [[ "$J_target_version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    [[ "$J_release_base" == "$DEFAULT_RELEASE_BASE" ]] || envs="$envs PLANAR_RELEASE_URL=$(printf '%q' "$J_release_base")"
    [[ "$ROOT_C" == "$HOME_PLANAR_C" ]] || envs="$envs PLANAR_HOME=$(printf '%q' "$ROOT_C")"
    printf 'curl -fsSL %s/download/%s/get-planar.sh | PLANAR_VERSION=%s%s sh' "$J_release_base" "$J_target_version" "$J_target_version" "$envs"
    return 0
  fi
  case "$SRC_ROOT/" in
    "$ROOT_C/$PLANAR_LOCK_UPDATE_NS/"*)
      printf 're-run install.sh --prebuilt from an unpacked bundle of commit %s (this %s bundle is not published)' "$J_target_sha" "$J_target_version"
      ;;
    *)
      printf 'bash %s --prebuilt %s%s' "$(printf '%q' "$SRC_ROOT/install.sh")" "$(printf '%q' "$SRC_ROOT")" "${args[*]+ $(quote_args "${args[@]}")}"
      ;;
  esac
}

# journal_save -- write the journal or stop.
journal_save() {
  planar_journal_write "$ROOT_C" || err "cannot write the recovery journal $ROOT_C/.planar-journal"
}

# journal_save_durable -- write the journal, then flush it to disk with sync(1),
# so the record survives a power loss as well as a kill. Used for the two phase
# records the recovery depends on: mutating (before the first live change) and
# complete (before the backups are disposed of).
journal_save_durable() {
  journal_save
  sync || warn "sync failed; the recovery journal $ROOT_C/.planar-journal is written but may not be on disk yet"
}

# set_target -- the identity of the release this run installs, into J_target_*.
set_target() {
  local f n max=0
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  J_mode="$MODE"
  if [[ "$PREBUILT" -eq 1 ]]; then
    J_source=prebuilt
    J_target_version="$(release_field "$SRC_ROOT/release.json" version)"
    J_target_sha="$(release_field "$SRC_ROOT/release.json" sha)"
    J_target_schema="$(release_field "$SRC_ROOT/release.json" schema_version)"
    J_target_path="$SRC_ROOT"
    J_release_base="$DEFAULT_RELEASE_BASE"
    if [[ -n "${PLANAR_RELEASE_URL-}" ]]; then
      if release_base_valid "$PLANAR_RELEASE_URL"; then
        J_release_base="${PLANAR_RELEASE_URL%/}"
      else
        warn "PLANAR_RELEASE_URL is not a valid release base; the retry command names $DEFAULT_RELEASE_BASE"
      fi
    fi
    [[ "$J_target_version" =~ ^[A-Za-z0-9._+-]+$ ]] || err "the bundle's release.json has no readable version"
  else
    J_source=checkout
    J_target_version=source
    J_target_sha="$(trap - ERR; git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null)" || J_target_sha=unknown
    [[ -n "$J_target_sha" ]] || J_target_sha=unknown
    for f in "$SRC_ROOT"/migrations/*.up.sql; do
      [[ -f "$f" ]] || continue
      n="${f##*/}"; n="${n%%_*}"
      [[ "$n" =~ ^[0-9]+$ ]] || continue
      (( 10#$n > max )) && max=$((10#$n))
    done
    J_target_schema="$max"
    J_target_path="$REPO_ROOT"
    J_release_base=""
  fi
}

# same_target -- the loaded journal's transaction installs what this run would.
same_target() {
  local v="$J_target_version" s="$J_target_sha" c="$J_target_schema" src="$J_source" p="$J_target_path"
  [[ "$src" == "$1" && "$v" == "$2" && "$s" == "$3" && "$c" == "$4" ]] || return 1
  [[ "$src" == prebuilt || "$p" == "$5" ]]
}

# ---------- mutation ownership ----------

INSTALL_ATTEMPT=""      # fresh | recovery, once the journal is ours
RUN_MUTATED=0           # 1 from the first live change of this run
NEW_STAGING=""          # the staging directory this run created
CLEANUP_OK=""           # the validated --cleanup directory

# _install_on_exit -- the EXIT trap from the moment ownership is held. A failed
# attempt that never changed anything live is marked aborted-before-mutation,
# its own staging is removed and its journal last, leaving the previous install
# authoritative. A failure after the mutating record keeps the journal and the
# backups and names the durable retry command. Then the updater's temporary
# directory is removed (with --cleanup) and ownership is released.
_install_on_exit() {
  local rc=$? jf
  trap - ERR
  set +e
  jf="$ROOT_C/.planar-journal"
  if [[ -n "$INSTALL_ATTEMPT" && -f "$jf" && ! -L "$jf" ]]; then
    if [[ "$J_phase" == prepared ]]; then
      J_phase=aborted-before-mutation
      planar_journal_write "$ROOT_C"
      planar_state_dispose_staging "$ROOT_C"
      rm -f "$jf"
      printf '  this attempt stopped before changing the installation; its staging was removed and the previous install is unchanged\n' >&2
    elif [[ "$J_phase" == mutating ]]; then
      if [[ "$RUN_MUTATED" -eq 0 && -n "$NEW_STAGING" ]]; then
        planar_state_dispose_staging "$ROOT_C" "$NEW_STAGING"
        planar_journal_write "$ROOT_C"
      fi
      if [[ "$rc" -ne 0 ]]; then
        printf '\n%sThe installation at %s is incomplete.%s Data paths and the recovery evidence (%s, the *.old backups) are kept; the binaries may already have changed. Fix the cause above, then complete the install with:\n  %s\n' \
          "$C_RED$C_BOLD" "$ROOT_C" "$C_RESET" "$jf" "$J_retry" >&2
      fi
    fi
  fi
  if [[ -n "$CLEANUP_OK" ]] && planar_update_tmp_valid "$ROOT_C" "$CLEANUP_OK"; then
    rm -rf "$CLEANUP_OK"
    rmdir "$ROOT_C/$PLANAR_LOCK_UPDATE_NS" 2>/dev/null
  fi
  planar_lock_release
  exit "$rc"
}

title "Taking ownership of $ROOT_C"
if [[ -n "${PLANAR_MUTATION_HANDOFF-}" ]]; then
  planar_lock_adopt "$ROOT_C" "$PLANAR_MUTATION_HANDOFF" \
    || { printf '\ninstall.sh: refusing the update handoff: %s\n' "$PLANAR_LOCK_ERROR" >&2; exit 1; }
  log "adopted the updater's ownership (generation $PLANAR_LOCK_GEN of $PLANAR_LOCK_DIR)"
else
  if [[ -n "$CLEANUP_DIR" ]]; then
    printf '\ninstall.sh: --cleanup is accepted only from the updater, which hands its ownership over in PLANAR_MUTATION_HANDOFF; nothing was removed\n' >&2
    exit 1
  fi
  planar_lock_acquire "$ROOT_C" install || { printf '\ninstall.sh: %s\n' "$PLANAR_LOCK_ERROR" >&2; exit 1; }
  log "mutation lock: $PLANAR_LOCK_DIR (generation $PLANAR_LOCK_GEN)"
fi
trap _install_on_exit EXIT
if [[ -n "$CLEANUP_DIR" ]]; then
  if [[ "$CLEANUP_DIR" != "$PLANAR_LOCK_TMP" ]] || ! planar_update_tmp_valid "$ROOT_C" "$CLEANUP_DIR" \
     || planar_removal_blocked "$ROOT_C" "$CLEANUP_DIR"; then
    err "refusing --cleanup $CLEANUP_DIR: it must be exactly the temporary directory the updater recorded (${PLANAR_LOCK_TMP:-none}), a real directory you own under $ROOT_C/$PLANAR_LOCK_UPDATE_NS/; nothing was removed"
  fi
  CLEANUP_OK="$CLEANUP_DIR"
fi
if [[ -n "$PLANAR_LOCK_RECLAIMED" ]]; then
  log "reclaimed the mutation lock from an abandoned $PLANAR_LOCK_RECLAIMED"
  if [[ -n "$PLANAR_LOCK_RECLAIMED_TMP" ]]; then
    rm -rf "$PLANAR_LOCK_RECLAIMED_TMP"
    log "removed the abandoned updater's temporary directory $PLANAR_LOCK_RECLAIMED_TMP"
  fi
fi
planar_install_fault after-lock || err "test fault after taking ownership"

harden_planar_home

# ---------- recovery ----------

# Validate any recovery journal and finish or discard what it owns before
# anything new is staged (tech spec 677, steps 2 and 3).
title "Checking for an interrupted install"
JOURNAL_FILE="$ROOT_C/.planar-journal"
set_target
_want_source="$J_source"; _want_version="$J_target_version"; _want_sha="$J_target_sha"
_want_schema="$J_target_schema"; _want_path="$J_target_path"
if [[ -e "$JOURNAL_FILE" || -L "$JOURNAL_FILE" ]]; then
  planar_journal_load "$ROOT_C" \
    || err "$JOURNAL_FILE is not a valid recovery journal for $ROOT_C; it and every staging and backup entry are kept. Inspect it; remove it by hand only if no install was interrupted."
  case "$J_phase" in
    uninstalling)
      err "an uninstall of $ROOT_C was interrupted; an install never resumes a cancelled installation. Finish the uninstall first: ${J_retry:-$REPO_ROOT/install.sh --uninstall}"
      ;;
    complete)
      log "the previous install committed; removing its leftover backups and staging"
      planar_state_finish "$ROOT_C"
      planar_journal_clear
      ;;
    prepared|aborted-before-mutation)
      log "a previous attempt ($J_phase) stopped before changing the installation; removing its staging"
      planar_state_dispose_staging "$ROOT_C"
      rm -f "$JOURNAL_FILE"
      planar_journal_clear
      ;;
    mutating)
      if [[ "$J_owner_pid" != "$$" ]] && [[ "$(trap - ERR; planar_lock_start_token "$J_owner_pid" 2>/dev/null || true)" == "$J_owner_start" ]] && [[ -n "$J_owner_start" ]]; then
        err "the interrupted install's owner (pid $J_owner_pid) is still running although it no longer holds the mutation lock; nothing was changed"
      fi
      same_target "$_want_source" "$_want_version" "$_want_sha" "$_want_schema" "$_want_path" \
        || err "an install of $J_target_version ($J_target_sha) into $ROOT_C was interrupted after it changed the installation; finish that one first, then install another release: $J_retry"
      planar_state_reconcile "$ROOT_C" || err "the interrupted install cannot be resumed safely: $INSTALL_STATE_ERROR"
      log "resuming the interrupted install of $J_target_version"
      INSTALL_ATTEMPT=recovery
      ;;
  esac
fi
while IFS= read -r _l; do [[ -n "$_l" ]] && warn "$_l"; done < <(planar_state_report_unknown "$ROOT_C")

if [[ "$INSTALL_ATTEMPT" != recovery ]]; then
  planar_journal_clear
  J_phase=prepared
  J_operation=install
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  [[ -n "${PLANAR_MUTATION_HANDOFF-}" ]] && J_operation=update
  # shellcheck disable=SC2034  # J_* are written to the journal by name
  J_operation_id="$(trap - ERR; _pl_nonce)" || err "cannot read random bytes for the operation id"
  set_target
  J_retry="$(retry_command)"
  for _n in $PLANAR_JOURNAL_SUBTREES; do printf -v "J_$(planar_journal_sub_key "$_n")" '%s' pending; done
  INSTALL_ATTEMPT=fresh
fi
J_owner_pid="$$"
J_owner_start="$(trap - ERR; planar_lock_start_token "$$")" || J_owner_start=unknown
# shellcheck disable=SC2034  # J_* are written to the journal by name
J_owner_lock="$PLANAR_LOCK_GEN"
NEW_STAGING="$(trap - ERR; planar_state_pick_staging)" || err "cannot pick a staging name"
J_staging="${J_staging:+$J_staging }$NEW_STAGING"
journal_save
STAGE="$ROOT_C/$NEW_STAGING"
planar_install_fault after-prepared || err "test fault after recording the attempt"

# ---------- stage ----------

# stage_prebuilt -- copy every managed subtree of the bundle into $STAGE.
stage_prebuilt() {
  local b d
  mkdir -p "$STAGE/bin" "$STAGE/skills" "$STAGE/agents"
  for b in "${PLANAR_BINARIES[@]}"; do
    cp -f "$SRC_ROOT/bin/$b" "$STAGE/bin/$b"
    chmod 755 "$STAGE/bin/$b"
  done
  cp -R "$SRC_ROOT/skills/planar" "$STAGE/skills/planar"
  find "$SRC_ROOT/agents" -maxdepth 1 -type f -exec cp -f {} "$STAGE/agents/" \;
  cp -R "$SRC_ROOT/codex-agents" "$STAGE/codex-agents"
  for d in workflows scripts migrations; do
    cp -R "$SRC_ROOT/$d" "$STAGE/$d"
  done
  [[ -d "$STAGE/scripts" ]] && chmod +x "$STAGE/scripts/"* 2>/dev/null
  log "staged the bundle's bin/, skills/, agents/, codex-agents/, workflows/, scripts/ and migrations/ (prebuilt: nothing is built)"
}

# stage_source -- build the five binaries and stage every managed subtree from
# the checkout into $STAGE. In link mode scripts/, workflows/, migrations/ and
# skills/planar are symlinks into the checkout and agents/ holds links to the
# authored files (models.md is copied: the prefix owns its tier table).
stage_source() {
  local d f
  title "Building the Planar binaries"
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
  # 6537). The build installs into the staging directory; only its bin/ is
  # taken, and everything is swapped into place after the database probe.
  ( cd "$REPO_ROOT" \
    && cmake --preset "$BUILD_PRESET" -B "$BUILD_DIR" \
         -DPLANAR_VERSION_META=ON \
    && cmake --build "$BUILD_DIR" \
    && cmake --install "$BUILD_DIR" --prefix "$STAGE/.cmake-install" )
  [[ -d "$STAGE/.cmake-install/bin" ]] || err "the build installed no bin/ into $STAGE/.cmake-install"
  mv "$STAGE/.cmake-install/bin" "$STAGE/bin"
  rm -rf "$STAGE/.cmake-install"
  title "Staging the planar skill and agents"
  mkdir -p "$STAGE/skills" "$STAGE/agents"
  place "$SRC_ROOT/skills/planar" "$STAGE/skills/planar"
  while IFS= read -r -d '' f; do
    if [[ "$MODE" == "link" && "$(basename "$f")" != "models.md" ]]; then
      ln -s "$f" "$STAGE/agents/$(basename "$f")"
    else
      cp -f "$f" "$STAGE/agents/$(basename "$f")"
    fi
  done < <(find "$SRC_ROOT/agents" -maxdepth 1 -type f -print0)
  # The renderer exits 1 naming a malformed file and writes nothing.
  local out
  if ! out="$(python3 "$SRC_ROOT/scripts/render-codex-agents.py" "$SRC_ROOT/agents" "$STAGE/codex-agents" 2>&1)"; then
    err "rendering the Codex agents failed: $out"
  fi
  for d in workflows scripts migrations; do
    place "$SRC_ROOT/$d" "$STAGE/$d"
  done
  # Copied script tooling is executable. In link mode the path resolves into the
  # checkout, whose modes are left alone.
  [[ "$MODE" != "link" && -d "$STAGE/scripts" ]] && chmod +x "$STAGE/scripts/"* 2>/dev/null
  log "staged skills/planar/, agents/, codex-agents/, workflows/, scripts/ and migrations/ ($MODE)"
}

title "Staging into $STAGE"
mkdir "$STAGE" || err "cannot create the staging directory $STAGE"
if [[ "$PREBUILT" -eq 1 ]]; then
  stage_prebuilt
else
  stage_source
fi
planar_install_fault staging || err "test fault while staging"

# Smoke check — a build can succeed yet produce a binary that won't run. Confirm
# the staged binary executes (and capture the build id) before anything live
# changes.
PLANAR_VERSION_LINE="$("$STAGE/bin/planar" version 2>/dev/null || true)"
[[ -n "$PLANAR_VERSION_LINE" ]] || \
  err "$STAGE/bin/planar was staged but it failed to run ('planar version' produced no output)"
PLANAR_BUILD_ID="$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $2}')"
ok "5 Planar binaries staged  ${C_DIM}($PLANAR_VERSION_LINE)${C_RESET}"
planar_install_fault after-staging || err "test fault after staging"

# ---------- probe the database ----------

# The staged binaries classify the database before anything live changes
# (tech spec 677 step 5; decisions 1324-1326).
title "Probing the database with the staged binaries"
planar_db_resolve "$ROOT_C"
planar_db_probe "$STAGE/bin"
planar_install_fault probe || { INSTALL_DB_STATE=fault; INSTALL_DB_DETAIL="test fault injected into the probe"; }
log "database $PLANAR_INSTALL_DB: $INSTALL_DB_STATE ($INSTALL_DB_DETAIL)"
case "$INSTALL_DB_STATE" in
  missing)
    log "no database yet: the installed planar will create it after the swap (no project is registered)" ;;
  behind)
    log "the database is at schema $INSTALL_DB_VERSION, behind this release's $INSTALL_DB_TARGET: it is migrated after the swap" ;;
  current) ;;
  ahead)
    err "refusing to install: the database $PLANAR_INSTALL_DB is at schema $INSTALL_DB_VERSION, newer than this release's schema $INSTALL_DB_TARGET. An older release is never installed over a newer database; nothing was changed. Install a release at schema $INSTALL_DB_VERSION or later." ;;
  *)
    err "refusing to install: the database $PLANAR_INSTALL_DB cannot be used: $INSTALL_DB_DETAIL. Nothing was changed. Fix the database (or PLANAR_DB), then re-run." ;;
esac

# The still-installed read-only planar-watch names live queue entries before the
# swap (decision 1327). It never blocks.
planar_queue_warning "$ROOT_C/bin"

planar_state_unknown_backup "$ROOT_C" && err "$INSTALL_STATE_ERROR"
# A managed subtree is moved aside whole, so one that is, or holds, a data path
# (relocated into it) stops the install here, before anything changes.
for _n in $PLANAR_JOURNAL_SUBTREES; do
  if [[ -e "$ROOT_C/$_n" || -L "$ROOT_C/$_n" ]] && planar_removal_blocked "$ROOT_C" "$ROOT_C/$_n"; then
    err "refusing to replace $PLANAR_HOME/$_n: it holds or is the data path '$PLANAR_DATA_PATH_HIT', which an install never moves or removes. Nothing was changed. Move that data path out of the installer's trees."
  fi
done

# ---------- live changes begin ----------

# Durably record the mutating intent before the first live change (the vendor
# sweep is one). From here a failure keeps the journal and the backups.
J_db="$INSTALL_DB_STATE"
J_phase=mutating
journal_save_durable
planar_install_fault after-mutating || err "test fault after recording the mutating intent"
RUN_MUTATED=1

# ---------- retire the previous projections ----------

# Order matters (plan 1104, M2): this sweep runs BEFORE the $PLANAR_HOME cleanup
# below and before the subtrees are swapped. The staged <vendor>-skills/
# trees an older install left under $PLANAR_HOME are the only evidence that a
# pl-* directory in a vendor's skills directory is a copy Planar made, and the
# cleanup deletes those trees. Nothing is removed that cannot be proven
# Planar's: a symlink whose target is under $PLANAR_HOME (or the operator's
# ~/.planar/local/ for local-*), or a directory whose bytes equal the staged
# entry. A pl-* directory with no such proof is left and reported.
title "Retiring the previous skill and agent projections"
run_sweep
log "retired $SWEEP_REMOVED previous projection(s); left $SWEEP_LEFT that could not be proven Planar's"

# Iterate the cleanup manifest and delete any root-relative artifact current
# Planar no longer ships (see install-cleanup.txt). A data path is never
# removed, and neither is anything reached through a symlink: an entry whose
# path, or any directory on the way to it, is a symlink is removed only as the
# link itself when the entry names the link.
CLEANUP_LIST="$SRC_ROOT/install-cleanup.txt"
if [[ -f "$CLEANUP_LIST" ]]; then
  while IFS= read -r _raw; do
    _line="${_raw%%#*}"                  # strip an inline comment
    read -r _relpath _ <<< "$_line"      # trim whitespace; first token = path
    [[ -z "$_relpath" ]] && continue
    _rel="${_relpath%/}"
    case "/$_rel/" in */../*|*/./*|//) warn "skipped cleanup entry $_relpath: not a plain relative path"; continue ;; esac
    _target="$ROOT_C/$_rel"
    if planar_removal_blocked "$ROOT_C" "$_target"; then
      log "kept $PLANAR_HOME/$_relpath (data path '$PLANAR_DATA_PATH_HIT': a cleanup entry never removes a data path)"
      continue
    fi
    _p="$ROOT_C"; _via_link=0; _rest="$_rel"
    while [[ "$_rest" == */* ]]; do
      _p="$_p/${_rest%%/*}"; _rest="${_rest#*/}"
      [[ -L "$_p" ]] && { _via_link=1; break; }
    done
    if [[ "$_via_link" -eq 1 ]]; then
      vlog "kept $PLANAR_HOME/$_relpath: $_p is a symlink, which cleanup never follows"
      continue
    fi
    if [[ -L "$_target" ]]; then
      rm -f "$_target"; log "removed stale link $PLANAR_HOME/$_rel"
    elif [[ "$_relpath" == */ ]]; then
      [[ -d "$_target" ]] && { rm -rf "$_target"; log "removed stale dir  $PLANAR_HOME/$_relpath"; }
    else
      [[ -e "$_target" ]] && { rm -f "$_target"; log "removed stale file $PLANAR_HOME/$_relpath"; }
    fi
  done < "$CLEANUP_LIST"
fi

# ---------- swap the managed subtrees ----------

title "Swapping the managed subtrees into $ROOT_C"
for _n in $PLANAR_JOURNAL_SUBTREES; do
  _sk="$(planar_journal_sub_key "$_n")"
  _st="J_$_sk"
  if [[ "${!_st}" == swapped ]]; then
    vlog "$_n/ already swapped by the interrupted run"
    continue
  fi
  planar_state_swap "$ROOT_C" "$STAGE" "$_n" || err "could not swap $_n/: $INSTALL_STATE_ERROR"
  log "$_n/ → $ROOT_C/$_n ($MODE)"
done

# templates/ ships operator-editable defaults (workspace-capabilities.toml,
# doc prompts). It is a data path: each file is placed only when missing, so
# hand edits survive `install.sh` re-runs, --force included.
if [[ -d "$SRC_ROOT/templates" ]]; then
  mkdir -p "$PLANAR_HOME/templates"
  while IFS= read -r -d '' f; do
    rel="${f#"$SRC_ROOT"/templates/}"
    dst="$PLANAR_HOME/templates/$rel"
    mkdir -p "$(dirname "$dst")"
    if [[ -e "$dst" || -L "$dst" ]]; then
      vlog "templates/$rel: kept existing (templates are placed missing-only, even with --force)"
      continue
    fi
    if [[ "$MODE" == "link" ]]; then
      ln -sfn "$f" "$dst"
    else
      cp -f "$f" "$dst"
    fi
    vlog "templates/$rel → $dst ($MODE)"
  done < <(find "$SRC_ROOT/templates" -type f -print0)
fi

# ---------- initialize or migrate the database ----------

# A missing or behind database is brought current by the swapped-in installed
# planar (tech spec 677 step 8; decision 1325), then probed again. A failure
# keeps the journal and the backups: binaries are not rolled back across a
# schema change.
if [[ "$J_db" == missing || "$J_db" == behind ]]; then
  title "$([[ "$J_db" == missing ]] && echo Initializing || echo Migrating) the database"
  log "(cd / && PLANAR_DB=$(printf '%q' "$PLANAR_INSTALL_DB") PLANAR_CONFIG_PATH=$(printf '%q' "$PLANAR_INSTALL_CONFIG") $(printf '%q' "$ROOT_C/bin/planar") init --skip-project --allow-no-repo)"
  _db_ok=1
  planar_db_migrate "$ROOT_C/bin" || _db_ok=0
  planar_install_fault post-probe || { _db_ok=0; INSTALL_DB_DETAIL="test fault injected into the post-migration probe"; }
  if [[ "$_db_ok" -ne 1 ]]; then
    err "the database $PLANAR_INSTALL_DB could not be brought to this release's schema: $INSTALL_DB_DETAIL. Its rows are kept; the new binaries are already in place."
  fi
  ok "database $PLANAR_INSTALL_DB is current"
  J_db=current
  journal_save
fi
planar_install_fault after-migrate || err "test fault after the migration"

# ---------- queue store ----------

# The retired agent.db (plan 1089). The source path re-checks its old queue
# for live entries and retires it with its old numbered logs; the prebuilt path
# has no python3 and moves it aside unread. See scripts/install-lib/queue-retire.sh.
title "Retiring the old queue store"
if [[ "$PREBUILT" -eq 1 ]]; then
  queue_retire_prebuilt || exit 1
else
  queue_live_guard re-check || exit 1
  queue_retire_store || exit 1
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

# The previous install manifest, read once before any placement. The manifest
# on disk is rewritten after every placed target, so ownership is judged against
# this snapshot, never against a file the run itself has already changed.
PREV_MANIFEST_TEXT=""
load_prev_manifest() {
  PREV_MANIFEST_TEXT=""
  [[ -f "$PLANAR_HOME/install-manifest.json" ]] || return 0
  PREV_MANIFEST_TEXT="$(cat "$PLANAR_HOME/install-manifest.json" 2>/dev/null || true)"
}

# prev_manifest_records <path> — true when the previous install manifest records
# <path> as a whole JSON string.
prev_manifest_records() {
  [[ -n "$PREV_MANIFEST_TEXT" ]] || return 1
  [[ "$PREV_MANIFEST_TEXT" == *"$(install_manifest_json_quote "$1")"* ]]
}

# derive_to <staged agent .md> <out> — opencode_derive into a file.
derive_to() { opencode_derive "$1" > "$2"; }

# target_content_equal <src> <dst> <fmt> — the bytes at <dst> are what placing
# <src> would produce, whatever form <dst> has: a link is followed. A skill is a
# directory tree (same file set, same bytes); an OpenCode agent is compared with
# its derived form; everything else is a single file.
target_content_equal() {
  local src="$1" dst="$2" fmt="$3" tmp rc=0
  case "$fmt" in
    skill)
      [[ -d "$dst" ]] && diff -r "$src/" "$dst/" >/dev/null 2>&1
      ;;
    opencode)
      [[ -f "$dst" ]] || return 1
      tmp="$(mktemp)"
      if derive_to "$src" "$tmp" 2>/dev/null; then cmp -s "$tmp" "$dst" || rc=1; else rc=1; fi
      rm -f "$tmp"
      return "$rc"
      ;;
    *)
      [[ -f "$dst" ]] && cmp -s "$src" "$dst"
      ;;
  esac
}

# target_matches <src> <dst> <fmt> — <dst> already is exactly what this run
# would place under $MODE, so it is left alone. The form matters: in link mode a
# Markdown/Copilot agent or a skill is a symlink to <src>; in copy mode it is a
# regular file (a skill: a real directory holding no symlink) with the same
# bytes; an OpenCode agent is a derived regular file in both modes.
target_matches() {
  local src="$1" dst="$2" fmt="$3"
  if [[ "$fmt" != opencode && "$MODE" == link ]]; then
    [[ -L "$dst" && "$(readlink "$dst" 2>/dev/null || true)" == "$src" ]]
    return
  fi
  [[ ! -L "$dst" ]] || return 1
  target_content_equal "$src" "$dst" "$fmt" || return 1
  if [[ "$fmt" == skill ]]; then
    [[ -z "$(find "$dst" -type l 2>/dev/null | head -n 1)" ]]
  fi
}

# check_destination <path> [<src> <fmt>] — refuse a destination that cannot be
# proven Planar's. It is Planar's when it is absent, a symlink into $PLANAR_HOME,
# a path the previous manifest records, or (when <src> and <fmt> are given)
# already holds exactly what placing <src> would write, which covers a target
# placed by a run that stopped before it could record it. Anything else is
# refused. Never removes anything. The pre-check over every planned target and
# the per-target check inside the placement loop both call this one function.
check_destination() {
  local dst="$1" src="${2:-}" fmt="${3:-}" target
  if [[ -L "$dst" ]]; then
    target="$(readlink "$dst" 2>/dev/null || true)"
    [[ "$target" == "$PLANAR_HOME"/* ]] && return 0
  elif [[ ! -e "$dst" ]]; then
    return 0
  fi
  prev_manifest_records "$dst" && return 0
  [[ -n "$src" ]] && target_content_equal "$src" "$dst" "$fmt" && return 0
  err "$dst already exists and no Planar manifest records it, so Planar will not replace it (move or remove it, then re-run)"
}

# _try <command...> — run a command with the ERR trap suspended, keeping its
# output as PLACE_REASON and returning its status, so a placement failure
# reaches the caller as a reason instead of ending the install in on_err.
PLACE_REASON=""
_try() {
  local rc=0
  trap - ERR
  PLACE_REASON="$("$@" 2>&1)" || rc=$?
  trap 'on_err $? $LINENO' ERR
  return "$rc"
}

# place_target <src> <dst> <fmt> — put one target at <dst>, replacing whatever
# is there. A skill is a directory: a link to the staged directory, or a copy of
# it. A Markdown agent and a Copilot agent follow $MODE (link = a symlink to the
# staged file, which for Copilot carries the .agent.md name; copy = a copy). An
# OpenCode agent is a derived file: always a regular file. Returns non-zero with
# PLACE_REASON set when a step fails; it never calls err, so the caller names
# the target. Nothing is written through a link into $PLANAR_HOME.
place_target() {
  local src="$1" dst="$2" fmt="$3" tmp
  _try mkdir -p "$(dirname "$dst")" || return 1
  case "$fmt" in
    skill)
      _try rm -rf "$dst" || return 1
      if [[ "$MODE" == "link" ]]; then
        _try ln -s "$src" "$dst" || return 1
      else
        _try mkdir -p "$dst" || return 1
        _try cp -R "$src/." "$dst/" || return 1
      fi
      ;;
    opencode)
      tmp="$(mktemp)"
      if ! _try derive_to "$src" "$tmp"; then
        rm -f "$tmp"
        PLACE_REASON="could not derive the OpenCode agent from $src (frontmatter lacks a description?)${PLACE_REASON:+: $PLACE_REASON}"
        return 1
      fi
      _try rm -f "$dst" || { rm -f "$tmp"; return 1; }
      _try cp -f "$tmp" "$dst" || { rm -f "$tmp"; return 1; }
      rm -f "$tmp"
      ;;
    *)
      _try rm -f "$dst" || return 1
      if [[ "$MODE" == "link" ]]; then
        _try ln -s "$src" "$dst" || return 1
      else
        _try cp -f "$src" "$dst" || return 1
      fi
      ;;
  esac
}

# Vendors that are selected by --vendors, and of those the ones found.
VENDORS_FOUND=()
VENDORS_SKIPPED=()
# The placement plan, in placement order. PLAN_STATE[i] is "done" once target i
# is placed (or found already in place) by this run, "carried" while the
# previous manifest still owns it and this run has not reached it, else "".
PLAN_SRC=(); PLAN_DST=(); PLAN_FMT=(); PLAN_STATE=(); PLAN_OWNER=(); PLAN_KIND=(); PLAN_NAME=()
PLAN_PATHS=()            # PLAN_PATHS[i]: the newline-joined manifest paths of target i
STAGED_EXTRAS=()         # the staged-tree extras, read once (the staged tree does not change)
STAGED_EXTRAS_READ=0

# target_paths <i> — the manifest paths of planned target <i>: the target itself,
# and for a skill every file beneath it, sorted so the manifest bytes do not
# depend on directory order.
target_paths() {
  local src="${PLAN_SRC[$1]}" dst="${PLAN_DST[$1]}" f
  printf '%s\n' "$dst"
  [[ "${PLAN_FMT[$1]}" == skill ]] || return 0
  while IFS= read -r -d '' f; do
    printf '%s\n' "$dst/${f#"$src"/}"
  done < <(find -L "$src" -type f -print0 | sort -z)
}

# record_manifest — write the manifest as it stands now (version 2): the staged
# paths, then for every target that is done or still carried, in plan order, its
# paths in `extras` (absolute) and one `projections` row. `vendors` is the list
# of vendors this run found. A row's vendor is the target's only owner, or
# `shared` for the ~/.agents/skills root that four vendors read: it is placed
# once, so no one vendor owns it, and the reader accepts `shared` on a row
# without it being a selectable vendor. A row's install_kind is $MODE, except
# an OpenCode agent, which is a derived copy in both modes. It is called after
# each placed target, so a stop at any point leaves every target placed so far
# recorded; install_manifest_write renames a temp file into place and skips the
# rename when the bytes are unchanged. Files found only in vendor destinations
# (including personal local-* extensions) are deliberately excluded.
record_manifest() {
  local i p kind name install_kind
  install_manifest_begin "${PLANAR_BUILD_ID:-unknown}" "$MODE"
  for p in ${VENDORS_FOUND[@]+"${VENDORS_FOUND[@]}"}; do INSTALL_MANIFEST_VENDORS+=("$p"); done
  if [[ "$STAGED_EXTRAS_READ" -eq 0 ]]; then
    install_manifest_record_staged "$PLANAR_HOME" "$SRC_ROOT"
    STAGED_EXTRAS=("${INSTALL_MANIFEST_EXTRAS[@]}")
    STAGED_EXTRAS_READ=1
  else
    INSTALL_MANIFEST_EXTRAS=("${STAGED_EXTRAS[@]}")
  fi
  for ((i = 0; i < ${#PLAN_DST[@]}; i++)); do
    [[ -n "${PLAN_STATE[$i]:-}" ]] || continue
    while IFS= read -r p; do install_manifest_add_extra "$p"; done <<< "${PLAN_PATHS[$i]}"
    kind="${PLAN_KIND[$i]}"; name="${PLAN_NAME[$i]}"
    install_kind="$MODE"
    [[ "${PLAN_FMT[$i]}" == opencode ]] && install_kind=copy
    install_manifest_add "${PLAN_OWNER[$i]}" "$kind" "$name" "${PLAN_SRC[$i]}" "${PLAN_DST[$i]}" "$install_kind"
  done
  install_manifest_write "$PLANAR_HOME/install-manifest.json" \
    || err "could not write $PLANAR_HOME/install-manifest.json; the previous manifest is intact (placed targets are still on disk; re-run to finish)"
}

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
  # anything is written. PLACEMENT ORDER IS FIXED: the VENDOR_TARGETS rows top to
  # bottom; within a row the skill is one directory and the agent sources follow
  # the glob's sorted order (planar-coder first). The install-stage tests rely
  # on this order (a stop at the Nth target leaves exactly the first N-1 placed
  # and recorded).
  for _row in "${VENDOR_TARGETS[@]}"; do
    IFS='|' read -r _owners _fmt _dir <<< "$_row"
    _active=0
    IFS=',' read -r -a _owner_list <<< "$_owners"
    for _o in "${_owner_list[@]}"; do
      [[ " ${VENDORS_FOUND[*]:-} " == *" $_o "* ]] && _active=1
    done
    [[ "$_active" -eq 1 ]] || continue
    _owner="$_owners"
    [[ "${#_owner_list[@]}" -gt 1 ]] && _owner=shared
    case "$_fmt" in
      skill)
        PLAN_SRC+=("$PLANAR_HOME/skills/planar"); PLAN_DST+=("$_dir/planar"); PLAN_FMT+=(skill); PLAN_OWNER+=("$_owner")
        ;;
      md|copilot|opencode)
        for _f in "$PLANAR_HOME"/agents/planar-*.md; do
          [[ -f "$_f" ]] || continue
          _name="$(basename "$_f")"
          [[ "$_fmt" == copilot ]] && _name="${_name%.md}.agent.md"
          PLAN_SRC+=("$_f"); PLAN_DST+=("$_dir/$_name"); PLAN_FMT+=("$_fmt"); PLAN_OWNER+=("$_owner")
        done
        ;;
      toml)
        for _f in "$PLANAR_HOME"/codex-agents/planar-*.toml; do
          [[ -f "$_f" ]] || continue
          PLAN_SRC+=("$_f"); PLAN_DST+=("$_dir/$(basename "$_f")"); PLAN_FMT+=(toml); PLAN_OWNER+=("$_owner")
        done
        ;;
    esac
  done

  # The manifest row identity of each target, computed once: record_manifest
  # runs after every target and must not fork per row.
  for ((_i = 0; _i < ${#PLAN_DST[@]}; _i++)); do
    _base="${PLAN_DST[$_i]##*/}"
    if [[ "${PLAN_FMT[$_i]}" == skill ]]; then
      PLAN_KIND+=(skill); PLAN_NAME+=("$_base")
    else
      PLAN_KIND+=(agent); _base="${_base%.agent.md}"; _base="${_base%.toml}"; PLAN_NAME+=("${_base%.md}")
    fi
  done

  load_prev_manifest
  for ((_i = 0; _i < ${#PLAN_DST[@]}; _i++)); do
    check_destination "${PLAN_DST[$_i]}" "${PLAN_SRC[$_i]}" "${PLAN_FMT[$_i]}"
    PLAN_PATHS+=("$(target_paths "$_i")")
    if prev_manifest_records "${PLAN_DST[$_i]}"; then PLAN_STATE+=(carried); else PLAN_STATE+=(""); fi
  done

  # A manifest exists before the first target is placed, so a stop at the very
  # first target still leaves the staged paths (and what the previous install
  # owned) recorded.
  record_manifest

  _n_placed=0; _n_replaced=0; _n_unchanged=0
  for ((_i = 0; _i < ${#PLAN_DST[@]}; _i++)); do
    _src="${PLAN_SRC[$_i]}"; _dst="${PLAN_DST[$_i]}"; _fmt="${PLAN_FMT[$_i]}"
    check_destination "$_dst" "$_src" "$_fmt"
    if [[ ! -e "$_dst" && ! -L "$_dst" ]]; then
      _word=placed
    elif target_matches "$_src" "$_dst" "$_fmt"; then
      _word=unchanged
    else
      _word=replaced
    fi
    if [[ "$_word" != unchanged ]]; then
      place_target "$_src" "$_dst" "$_fmt" \
        || err "could not place $_dst: ${PLACE_REASON:-unknown failure}. Stopped at this target; the targets before it stay in place and recorded in the manifest. Fix the cause and re-run to finish."
    fi
    PLAN_STATE[_i]="done"
    record_manifest
    case "$_word" in
      placed)    _n_placed=$((_n_placed + 1)) ;;
      replaced)  _n_replaced=$((_n_replaced + 1)) ;;
      unchanged) _n_unchanged=$((_n_unchanged + 1)) ;;
    esac
    vlog "$_word: $_dst ($([[ "$_fmt" == opencode ]] && echo 'derived copy' || echo "$MODE"))"
  done
  # Skill dirs and agent files follow $MODE; OpenCode agents are derived regular files.
  if [[ ${#PLAN_DST[@]} -eq 0 ]]; then
    log "no vendor targets to place"
  elif [[ $((_n_placed + _n_replaced)) -eq 0 ]]; then
    log "no changes: all ${#PLAN_DST[@]} vendor target(s) already match ($MODE)"
  else
    log "placed $_n_placed, replaced $_n_replaced, unchanged $_n_unchanged of ${#PLAN_DST[@]} vendor target(s) ($MODE)"
  fi
fi

# ---------- install authority ----------

# The manifest was written after each placed target above; this last write covers
# an install with no vendor targets and is a no-op (same bytes, no rename) when
# nothing changed since the last target.
record_manifest
vlog "wrote $PLANAR_HOME/install-manifest.json (${#INSTALL_MANIFEST_EXTRAS[@]} recorded paths)"

# ---------- release record ----------

# write_release_json — $PLANAR_HOME/release.json, the single record of which
# release is installed (decision 1334). A prebuilt install copies the bundle's
# file. A source install writes one in the same shape scripts/dist.sh writes:
# `version` is the sixth token of `planar version` (the release tag, `dev`
# for a build without one), so `planar update --check` on a source install
# reports dev and always finds an update. The other fields are read from the
# build and the host: the full sha from `planar version --json`, the version
# line's date, the host os and arch, the os floor dist.sh records for that os,
# and the highest migration number the build embeds. The file is
# written beside its destination and renamed into place, and left alone when
# its bytes are unchanged.
release_json_safe() {
  # Print $1 when it is a plain token (no quote, backslash or space), else $2.
  if [[ "$1" =~ ^[A-Za-z0-9._+:-]+$ ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi
}
write_release_json() {
  local dest="$PLANAR_HOME/release.json" tmp="$PLANAR_HOME/release.json.new.$$"
  rm -f "$tmp"
  if [[ "$PREBUILT" -eq 1 ]]; then
    cp -f "$SRC_ROOT/release.json" "$tmp"
  else
    local version sha date os arch floor schema f n vjson
    version="$(release_json_safe "$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $6}')" dev)"
    # The full commit sha, as scripts/dist.sh records it, comes from
    # `planar version --json`; the version line carries a short form only.
    vjson="$(trap - ERR; "$PLANAR_RELEASE_BIN" version --json 2>/dev/null)" || vjson=""
    sha="$(printf '%s' "$vjson" | sed -n 's/.*"sha":"\([^"]*\)".*/\1/p' | head -n 1)"
    [[ -n "$sha" ]] || sha="$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $2}')"
    sha="$(release_json_safe "$sha" unknown)"
    date="$(release_json_safe "$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $3}')" unknown)"
    case "$(uname -s)" in
      Darwin) os=macos ;;
      Linux)  os=linux ;;
      *)      os="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
    esac
    arch="$(uname -m)"
    [[ "$arch" == aarch64 ]] && arch=arm64
    # The platform floor scripts/dist.sh records for a bundle of this OS: the
    # macOS deployment target, or the glibc floor of the Linux bundles. Keep
    # these two constants in step with dist.sh.
    case "$os" in
      macos) floor=26.0 ;;
      linux) floor=2.36 ;;
      *)     floor=unknown ;;
    esac
    schema=0
    for f in "$SRC_ROOT"/migrations/*.up.sql; do
      [[ -f "$f" ]] || continue
      n="${f##*/}"; n="${n%%_*}"
      [[ "$n" =~ ^[0-9]+$ ]] || continue
      (( 10#$n > schema )) && schema=$((10#$n))
    done
    printf '{\n  "version": "%s",\n  "sha": "%s",\n  "date": "%s",\n  "os": "%s",\n  "arch": "%s",\n  "os_floor": "%s",\n  "schema_version": %s\n}\n' \
      "$version" "$sha" "$date" "$os" "$arch" "$floor" "$schema" > "$tmp"
  fi
  if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
    rm -f "$tmp"
    vlog "release.json unchanged"
  else
    chmod 644 "$tmp"
    mv -f "$tmp" "$dest"
    log "wrote $dest"
  fi
}

title "Recording the release"
PLANAR_RELEASE_BIN="$ROOT_C/bin/planar"
write_release_json

# ---------- ownership stamp ----------

# Mark $PLANAR_HOME as a Planar-managed install. The prefix ownership guard
# reads this on re-install to distinguish "our tree" from a mis-typed --prefix.
# release.json, then this stamp, are the last writes of the install.
# A database the install itself created or replaced is private too.
harden_planar_home
printf 'planar-install %s\nbuild %s\n' "$INSTALLER_VERSION" "${PLANAR_BUILD_ID:-unknown}" > "$PLANAR_STAMP"

# ---------- commit ----------

# The installation is committed: record that first, then dispose of the
# backups and the staging, and remove the journal last (tech spec 677 step 9).
J_phase=complete
journal_save_durable
planar_install_fault after-complete || err "test fault after recording completion"
planar_state_finish "$ROOT_C"

# ---------- summary ----------

title "Install complete"

# The staged role set (planar-*.md), not the top-level agents/ dir — the latter
# mixes in shared docs (doctrine, methodology, models).
agents_n="$(count_glob "$PLANAR_HOME"/agents/planar-*.md)"

ok "Planar ${PLANAR_BUILD_ID:-installed} → $PLANAR_HOME  ${C_DIM}(${SECONDS}s, $MODE mode)${C_RESET}"
log "binaries:   planar, planar-agent, planar-watch, planar-execute, planar-ext"
log "surfaces:   planar skill · $agents_n agents · vendors found: ${VENDORS_FOUND[*]:-none}"
if [[ "$WARN_COUNT" -gt 0 ]]; then
  printf '  %s!%s %s warning(s) above — review before first run\n' "$C_YELLOW" "$C_RESET" "$WARN_COUNT"
fi
printf '\n'
log "database:   $PLANAR_INSTALL_DB (current)"
log "next:       planar init  (in a project checkout, to register it)"
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
