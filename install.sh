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
#     bin/planar-doc                  # the doc-state manifest tool
#                                     # (build/verify/diff/cover/nodoc/lint;
#                                     #  never opens SQLite)
#     bin/planar-execute              # deterministic spawn-free Lua workflow
#                                     # engine (run <wf.lua> --phase; shells
#                                     #  planar for state, holds no DB handle)
#     planar.db                       # created on first `planar init`
#     migrations/0001_foundation.up.sql  # canonical migration sources (also
#                                     # embedded into the binary at compile
#                                     # via build-time codegen)
#     agents/                         # vendor-neutral agent role specs
#     commands/claude/pl-*.md         # Claude slash-command sources
#     skills/codex/pl-*.md            # Codex skill sources
#     codex-skills/pl-*/SKILL.md      # Codex runtime skill directories
#     skills/copilot/pl-*.md          # Copilot skill sources
#     copilot/                        # Copilot instructions/prompts (if any)
#     scripts/validate-{barrel-modes,plan-status}-acceptance
#
# Then installs into the vendor harness dirs (only when --vendor flags select
# them; default is all three):
#
#   ~/.claude/commands/pl-*.md   ->   ~/.planar/commands/claude/pl-*.md
#   ~/.codex/skills/pl-*         # real Codex skill dirs copied from ~/.planar/codex-skills/pl-*
#   ~/.copilot/skills/pl-*       # real Copilot skill dirs copied from ~/.planar/copilot-skills/pl-*
#   ~/.claude/agents/<name>.md   ->   ~/.planar/agents/claude/<name>.md
#   ~/.codex/agents/<name>.toml  ->   ~/.planar/agents/codex/<name>.toml
#   ~/.copilot/agents/<name>.agent.md -> ~/.planar/agents/copilot/<name>.agent.md
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
#   ./install.sh                      # full install with all three vendors
#   ./install.sh --no-vendor          # install Planar core only; skip vendor symlinks
#   ./install.sh --vendors claude     # install + symlink Claude only
#   ./install.sh --vendors claude,codex
#   ./install.sh --link               # symlink from this repo instead of copying
#                                     #   (dev mode — edits to repo propagate)
#   ./install.sh --prefix /opt/planar # override ~/.planar
#   ./install.sh --force              # overwrite existing symlinks
#   ./install.sh --uninstall          # tear down everything install.sh created
#   ./install.sh --optimize Debug     # zig build optimize mode (default ReleaseSafe)
#   ./install.sh --dry-run            # preview planned actions without changing anything
#   ./install.sh --verbose            # per-file detail (default prints a summary)
#   ./install.sh --version            # print installer version and exit
#
# Run ./install.sh --help for the full option list.
# Output is colorized on a TTY; set NO_COLOR=1 (or pipe stdout) for plain text.

# -E (errtrace) propagates the ERR trap into subshells/functions so a failure
# inside the `( cd … && zig build … )` subshell is reported, not swallowed.
set -eEuo pipefail

# ---------- defaults ----------

PLANAR_HOME="${PLANAR_HOME:-$HOME/.planar}"
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
VENDORS="claude,codex,copilot"
MODE="copy"                   # copy | link
FORCE=0
UNINSTALL=0
NO_PRUNE=0                    # set with --no-prune to skip stale-vendor-file removal
OPTIMIZE="ReleaseSafe"        # zig optimize mode
VERBOSE=0                     # set with --verbose/-v for per-file detail
DRY_RUN=0                     # set with --dry-run/-n to preview without changes
INSTALLER_VERSION="1.0.0"     # install.sh's own version (see --version)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
  --vendors LIST     Comma-separated vendors to wire: claude,codex,copilot (default: all)
  --no-vendor        Install Planar core only; skip vendor surfaces
  --link             Symlink from this repo instead of copying (dev mode)
  --force            Overwrite existing symlinks / adopt a non-Planar prefix
  --no-prune         Skip removal of stale vendor files
  --optimize MODE    Zig optimize mode: Debug|ReleaseSafe|ReleaseFast|ReleaseSmall (default: ReleaseSafe)
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
    --optimize)   OPTIMIZE="$2"; shift 2 ;;
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

# set -e + this ERR trap turn a raw mid-script failure (a bad `zig build`, a
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

  for vendor_root in "$HOME/.claude/commands" "$HOME/.codex/skills" "$HOME/.copilot/skills"; do
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
  for agent_dir in "$HOME/.claude/agents" "$CODEX_HOME/agents" "$HOME/.copilot/agents"; do
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
    log "removing install root: $PLANAR_HOME"
    log "(planar.db is preserved if you have data — re-run with --force or rm manually)"
    if [[ "$FORCE" -eq 1 ]]; then
      rm -rf "$PLANAR_HOME"
    else
      # Preserve the database; remove everything else.
      find "$PLANAR_HOME" -mindepth 1 -maxdepth 1 ! -name 'planar.db' -exec rm -rf {} +
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
  "zig|zig|builds the five Planar binaries"
  "cp||copy install artifacts into place"
  "ln||symlink vendor surfaces"
  "mkdir||create the install tree"
  "rm||replace prior-install artifacts"
  "find||walk vendor + template source trees"
  "rmdir||remove emptied vendor skill directories"
  "sed||parse the pinned zig version from build.zig.zon"
  "awk||read the build id from 'planar version'"
  "grep||validate the Planar package manifest"
  "head||take the first match when parsing build.zig.zon"
  "cat||read help text + vendor ownership markers"
  "ls||detect a non-empty / foreign install prefix"
  "readlink||resolve existing vendor symlinks safely"
  "basename||derive install target names"
  "dirname||resolve parent directories"
  "tr||normalize version / PATH strings"
  "chmod||mark shipped scripts executable"
)
RUN_DEPS=(
  "git|git|repo discovery + 'planar import' (required at runtime)"
  "jq|jq|bundled agent skills parse 'planar … --json' output"
  "gh|gh|GitHub adapter auth + issue import (degrades gracefully)"
  "rg|ripgrep|agent-workflow code-search recipes (ripgrep)"
  "mtkahypar||optional: 'planar groups recommend --solver=mtkahypar' optimal arm; greedy runs without it (built from source, no brew)"
)

check_deps "build" 1 "${BUILD_DEPS[@]}"

# Zig version gate — build.zig.zon pins a minimum; an older toolchain otherwise
# fails deep in the build with a cryptic error. Surface it up front. Dev/build
# suffixes (0.16.0-dev.123+abc) are treated as their base release.
MIN_ZIG="$(sed -n 's/.*\.minimum_zig_version = "\([0-9.]*\)".*/\1/p' "$REPO_ROOT/build.zig.zon" | head -n1)"
HAVE_ZIG="$(zig version 2>/dev/null || true)"
HAVE_ZIG_CORE="${HAVE_ZIG%%-*}"; HAVE_ZIG_CORE="${HAVE_ZIG_CORE%%+*}"
if [[ -n "$MIN_ZIG" && -n "$HAVE_ZIG_CORE" ]] && ! version_ge "$HAVE_ZIG_CORE" "$MIN_ZIG"; then
  err "zig $MIN_ZIG or newer required, found $HAVE_ZIG ($(command -v zig))"
fi

# Check that we are in a Planar source checkout.
# Zig code lives under src/; build.zig + build.zig.zon sit at the repo root.
[[ -f "$REPO_ROOT/build.zig" ]] || err "build.zig not found in $REPO_ROOT (run install.sh from the Planar source repo)"
[[ -f "$REPO_ROOT/build.zig.zon" ]] || err "build.zig.zon not found in $REPO_ROOT"
grep -q '^[[:space:]]*\.name = \.planar' "$REPO_ROOT/build.zig.zon" || err "$REPO_ROOT does not look like the Planar Zig package (build.zig.zon name mismatch)"

# Runtime tools — non-fatal; the install still produces a working binary, but
# Planar's git-backed verbs and the bundled agent skills need these to work.
check_deps "Planar runtime" 0 "${RUN_DEPS[@]}"

log "PLANAR_HOME = $PLANAR_HOME"
log "mode        = $MODE"
log "vendors     = ${VENDORS:-(none)}"
log "optimize    = $OPTIMIZE"
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
  log "build 5 binaries (planar, planar-agent, planar-watch, planar-doc, planar-execute) → $PLANAR_HOME/bin  [optimize=$OPTIMIZE]"
  log "run cleanup manifest: $_cleanup_n path(s) checked for removal"
  log "wipe + re-place: agents/, scripts/, skills/, commands/, migrations/$([[ -d "$REPO_ROOT/copilot" ]] && echo ', copilot/')"
  log "render per-vendor skill + agent outputs into $PLANAR_HOME"
  log "place templates/ (missing-only; --force overwrites)"
  if [[ -n "$VENDORS" ]]; then
    log "wire vendor surfaces: $VENDORS → ~/.claude, ~/.codex, ~/.copilot"
  else
    log "vendor surfaces: skipped (--no-vendor)"
  fi
  printf '\n'
  log "Re-run without --dry-run to apply."
  exit 0
fi

# ---------- build the binary ----------

title "Building the Planar binaries"

mkdir -p "$PLANAR_HOME/bin"
# `zig build --prefix <root>` installs every `installArtifact` target into
# <root>/bin/. The build registers FIVE binaries:
#
#   planar         — operator surface
#   planar-agent   — agent-callable coordination (atomic claim ops,
#                    nested actions, ingest, operator recovery)
#   planar-watch   — human-facing read-only viewer (no write verbs,
#                    strict SQLITE_OPEN_READONLY handle)
#   planar-doc     — doc-state manifest tool (build/verify/diff/cover/
#                    nodoc/lint; never opens SQLite)
#   planar-execute — deterministic spawn-free Lua workflow engine
#                    (run <wf.lua> --phase; shells planar for state,
#                    holds no DB handle, no model-spawn host fn)
#
# All five land in $PLANAR_HOME/bin/ in one shot — no extra cp step needed.
# Migrations and templates/defaults are read from the repo root at
# codegen time (build.zig sits at the repo root).
( cd "$REPO_ROOT" && zig build -Doptimize="$OPTIMIZE" --prefix "$PLANAR_HOME" )
vlog "wrote $PLANAR_HOME/bin/planar"
vlog "wrote $PLANAR_HOME/bin/planar-agent"
vlog "wrote $PLANAR_HOME/bin/planar-watch"
vlog "wrote $PLANAR_HOME/bin/planar-doc"
vlog "wrote $PLANAR_HOME/bin/planar-execute"

# Smoke check — a build can succeed yet produce a binary that won't run. Confirm
# it executes now (and capture the build id) rather than discovering it broken
# at `planar init`. `planar version` prints `planar <sha> <ts> zig <ver>` on
# stdout and exits 0 even when no DB exists yet.
PLANAR_VERSION_LINE="$("$PLANAR_HOME/bin/planar" version 2>/dev/null || true)"
[[ -n "$PLANAR_VERSION_LINE" ]] || \
  err "built $PLANAR_HOME/bin/planar but it failed to run ('planar version' produced no output)"
PLANAR_BUILD_ID="$(printf '%s' "$PLANAR_VERSION_LINE" | awk '{print $2}')"
ok "built 5 binaries → $PLANAR_HOME/bin  ${C_DIM}($PLANAR_VERSION_LINE)${C_RESET}"

# `zig build --prefix` only writes the targets it builds — it never removes
# files a PRIOR install left behind. Iterate the cleanup manifest and delete
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

# ---------- place artifacts ----------

title "Placing source artifacts into $PLANAR_HOME"

# These directories are the canonical Planar artifacts that get installed.
# In copy mode we mirror them under $PLANAR_HOME; in link mode we symlink the
# whole tree so edits to the repo propagate.
#
# `skills/` is special-cased: only `skills/src/` is authored in the repo;
# the per-vendor outputs (`commands/`, `skills/codex/`, `skills/copilot/`)
# are rendered into $PLANAR_HOME below by `planar skills render`. In link
# mode we still symlink skills/src so source edits propagate; the
# rendered outputs are always real files (writing through a link-mode
# symlink would mutate the repo).
for d in agents scripts workflows; do
  if [[ -d "$REPO_ROOT/$d" ]]; then
    rm -rf "$PLANAR_HOME/$d"
    place "$REPO_ROOT/$d" "$PLANAR_HOME/$d"
    log "$d/ → $PLANAR_HOME/$d ($MODE)"
  fi
done

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
# skills/copilot, agents/models.md tier table) directly into
# $PLANAR_HOME. This replaces the previous workflow where the rendered
# outputs were committed to the repo and copied at install time.
title "Rendering per-vendor skill outputs"
# The renderer lists every file it writes on stdout; that per-file detail is
# verbose-only. Keep stderr (warnings/errors) so the ERR trap still fires.
if [[ "$VERBOSE" -eq 1 ]]; then
  ( cd "$PLANAR_HOME" && "$PLANAR_HOME/bin/planar" skills render --src "$PLANAR_HOME/skills/src" --out "$PLANAR_HOME" )
else
  ( cd "$PLANAR_HOME" && "$PLANAR_HOME/bin/planar" skills render --src "$PLANAR_HOME/skills/src" --out "$PLANAR_HOME" ) >/dev/null
fi
log "rendered: commands/claude, skills/codex, skills/copilot (+ agents/models.md tier table)"

# Migrations live at repo root in sqlx-cli format and are read by the Zig
# build via codegen. We also stage them under $PLANAR_HOME for ad-hoc
# tooling (e.g. operators running `sqlx migrate` against scratch DBs).
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

# Make sure shipped script tooling is executable.
if [[ -d "$PLANAR_HOME/scripts" ]]; then
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

  # Codex discovers skills as directories that contain SKILL.md. Keep Planar's
  # source-of-truth files flat, materialize Planar-owned runtime skill
  # directories, then install real Codex skill directories into CODEX_HOME so
  # discovery works even when the loader does not follow symlinked directories.
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
      skill_name="$(basename "$f" .md)"
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
      printf '%s\n' "$f" > "$marker"
      count=$((count + 1))
    done < <(find "$src_dir" -maxdepth 1 -name 'pl-*.md' -print0)
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

  IFS=',' read -r -a vendor_list <<< "$VENDORS"
  for v in "${vendor_list[@]}"; do
    case "$v" in
      claude)
        symlink_vendor "claude" "$PLANAR_HOME/commands/claude" "$HOME/.claude/commands"
        prune_stale_vendor "claude" "$PLANAR_HOME/commands/claude" "$HOME/.claude/commands"
        symlink_vendor_agents "claude" "$PLANAR_HOME/agents/claude" "$HOME/.claude/agents"
        prune_stale_vendor_agents "claude" "$PLANAR_HOME/agents/claude" "$HOME/.claude/agents"
        ;;
      codex)
        install_codex_vendor "$PLANAR_HOME/skills/codex" "$PLANAR_HOME/codex-skills" "$CODEX_HOME/skills"
        prune_stale_codex "$CODEX_HOME/skills"
        symlink_vendor_agents "codex" "$PLANAR_HOME/agents/codex" "$CODEX_HOME/agents"
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
      *) warn "unknown vendor: $v (skipping)" ;;
    esac
  done
fi

# ---------- ownership stamp ----------

# Mark $PLANAR_HOME as a Planar-managed install. The prefix ownership guard
# reads this on re-install to distinguish "our tree" from a mis-typed --prefix.
printf 'planar-install %s\nbuild %s\n' "$INSTALLER_VERSION" "${PLANAR_BUILD_ID:-unknown}" > "$PLANAR_STAMP"

# ---------- summary ----------

title "Install complete"

# Count the rendered role set (agents/claude/), not the top-level agents/ dir —
# the latter mixes in shared docs (doctrine, methodology, models). Both the
# rendered skills and agent roles are produced by `skills render`, so these
# reflect what actually got installed regardless of --vendors.
skills_n="$(count_glob "$PLANAR_HOME"/commands/claude/pl-*.md)"
agents_n="$(count_glob "$PLANAR_HOME"/agents/claude/*.md)"

ok "Planar ${PLANAR_BUILD_ID:-installed} → $PLANAR_HOME  ${C_DIM}(${SECONDS}s, $MODE mode)${C_RESET}"
log "binaries:   planar, planar-agent, planar-watch, planar-doc, planar-execute"
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
