#!/usr/bin/env bash
#
# install.sh — install Planar from a source checkout.
#
# Layout produced:
#
#   ~/.planar/
#     bin/planar                      # the operator binary
#     bin/planar-agent                # the agent-callable coordination binary
#                                     # (plan 85 — three-binary architecture)
#     bin/planar-watch                # the human-facing read-only viewer
#                                     # (plan 85 M8 — opens DB read-only,
#                                     #  zero write verbs)
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

set -euo pipefail

# ---------- defaults ----------

PLANAR_HOME="${PLANAR_HOME:-$HOME/.planar}"
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
VENDORS="claude,codex,copilot"
MODE="copy"                   # copy | link
FORCE=0
UNINSTALL=0
NO_PRUNE=0                    # set with --no-prune to skip stale-vendor-file removal
OPTIMIZE="ReleaseSafe"        # zig optimize mode

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
    -h|--help)
      sed -n '1,/^set -euo/p' "$0" | sed -n '2,/^# Usage:/p; /^# Usage:/,/^set -euo/p' | sed 's/^# \?//;/^set -euo/d'
      exit 0
      ;;
    *) echo "install.sh: unknown flag: $1" >&2; exit 64 ;;
  esac
done

# ---------- helpers ----------

log()   { printf '  %s\n' "$*"; }
title() { printf '\n==> %s\n' "$*"; }
err()   { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || err "required command not found: $1"
}

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

require_cmd zig
require_cmd ln
require_cmd cp
require_cmd find

# Check that we are in a Planar source checkout.
# Zig code lives under src/; build.zig + build.zig.zon sit at the repo root.
[[ -f "$REPO_ROOT/build.zig" ]] || err "build.zig not found in $REPO_ROOT (run install.sh from the Planar source repo)"
[[ -f "$REPO_ROOT/build.zig.zon" ]] || err "build.zig.zon not found in $REPO_ROOT"
grep -q '^[[:space:]]*\.name = \.planar' "$REPO_ROOT/build.zig.zon" || err "$REPO_ROOT does not look like the Planar Zig package (build.zig.zon name mismatch)"

log "PLANAR_HOME = $PLANAR_HOME"
log "mode        = $MODE"
log "vendors     = ${VENDORS:-(none)}"
log "optimize    = $OPTIMIZE"

# ---------- build the binary ----------

title "Building the planar + planar-agent binaries"

mkdir -p "$PLANAR_HOME/bin"
# `zig build --prefix <root>` installs every `installArtifact` target into
# <root>/bin/. As of plan 85 M8 the build registers THREE binaries:
#
#   planar         — operator surface
#   planar-agent   — agent-callable coordination (atomic claim ops,
#                    nested actions, ingest, operator recovery)
#   planar-watch   — human-facing read-only viewer (no write verbs,
#                    strict SQLITE_OPEN_READONLY handle)
#
# All three land in $PLANAR_HOME/bin/ in one shot — no extra cp step needed.
# Migrations and templates/defaults are read from the repo root at
# codegen time (build.zig sits at the repo root).
( cd "$REPO_ROOT" && zig build -Doptimize="$OPTIMIZE" --prefix "$PLANAR_HOME" )
log "wrote $PLANAR_HOME/bin/planar"
log "wrote $PLANAR_HOME/bin/planar-agent"
log "wrote $PLANAR_HOME/bin/planar-watch"

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
for d in agents scripts; do
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
( cd "$PLANAR_HOME" && "$PLANAR_HOME/bin/planar" skills render --src "$PLANAR_HOME/skills/src" --out "$PLANAR_HOME" )
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
      log "templates/$rel: kept existing (use --force to overwrite)"
      continue
    fi
    if [[ "$MODE" == "link" ]]; then
      ln -sfn "$f" "$dst"
    else
      cp -f "$f" "$dst"
    fi
    log "templates/$rel → $dst ($MODE)"
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
      log "$name: source $src_dir not found, skipping"
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
      log "codex: source $src_dir not found, skipping"
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
      log "copilot: source $src_dir not found, skipping"
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
      log "$name agents: source $src_dir not found, skipping"
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
      log "$name agents: pruning stale symlink $(basename "$link") (source removed)"
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
      log "$name: pruning stale symlink $(basename "$link") (source removed)"
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
      log "codex: pruning stale skill dir $(basename "$skill_dir") (source removed)"
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
      log "copilot: pruning stale skill dir $(basename "$skill_dir") (source removed)"
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
      *) log "unknown vendor: $v (skipping)" ;;
    esac
  done
fi

# ---------- summary ----------

title "Install complete"
log "binary:        $PLANAR_HOME/bin/planar"
log "install root:  $PLANAR_HOME"
log "next:          planar init"
log "uninstall:     ./install.sh --uninstall"

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
