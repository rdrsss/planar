#!/usr/bin/env bash
# Install-manifest support for install.sh. This file is sourced, not executed.

INSTALL_MANIFEST_VERSION=1
INSTALL_MANIFEST_BUILD_ID=""
INSTALL_MANIFEST_MODE=""
INSTALL_MANIFEST_VENDORS=()
INSTALL_MANIFEST_EXTRAS=()
INSTALL_MANIFEST_ROW_VENDOR=()
INSTALL_MANIFEST_ROW_KIND=()
INSTALL_MANIFEST_ROW_NAME=()
INSTALL_MANIFEST_ROW_STAGED=()
INSTALL_MANIFEST_ROW_INSTALLED=()
INSTALL_MANIFEST_ROW_INSTALL_KIND=()
INSTALL_MANIFEST_ROW_SOURCE_DIGEST=()
INSTALL_MANIFEST_ROW_PROJECTION_DIGEST=()

install_manifest_begin() {
  INSTALL_MANIFEST_BUILD_ID="$1"
  INSTALL_MANIFEST_MODE="$2"
  INSTALL_MANIFEST_VENDORS=()
  INSTALL_MANIFEST_EXTRAS=()
  INSTALL_MANIFEST_ROW_VENDOR=()
  INSTALL_MANIFEST_ROW_KIND=()
  INSTALL_MANIFEST_ROW_NAME=()
  INSTALL_MANIFEST_ROW_STAGED=()
  INSTALL_MANIFEST_ROW_INSTALLED=()
  INSTALL_MANIFEST_ROW_INSTALL_KIND=()
  INSTALL_MANIFEST_ROW_SOURCE_DIGEST=()
  INSTALL_MANIFEST_ROW_PROJECTION_DIGEST=()
}

install_manifest_add_extra() {
  INSTALL_MANIFEST_EXTRAS+=("$1")
}

install_manifest_digest() {
  local key="$1" path="$2" value
  value="$(sed -n "s/^[#[:space:]]*${key}:[[:space:]]*\([0-9a-f][0-9a-f]*\)[[:space:]]*$/\1/p" "$path" | head -n 1)"
  [[ ${#value} -eq 64 ]] || return 1
  printf '%s' "$value"
}

# install_manifest_digest_or_empty <key> <path> — like install_manifest_digest,
# but returns an empty string (never a non-zero exit) when the header is
# absent, rather than treating that as fatal. Scriptorium's rendered output
# does not embed the legacy x-planar-source-digest/x-planar-projection-digest
# headers (plan 918 tech-spec.md § Architecture "installedsurface.zig" — that
# in-band digest scheme has retired). The in-tree scriptorium checks staged
# rendering; this manifest tracks installed paths. Absence is expected now,
# not an install-time error.
#
# Brackets install.sh's ERR trap (`trap - ERR` / restore) rather than an
# `if`/`||` guard: install.sh runs under `set -eE`, and -E (errtrace)
# propagates the ERR trap into the command-substitution subshell below, where
# install_manifest_digest's expected non-zero return (header absent) fires
# the inherited trap immediately — independent of any if/|| wrapping in THIS
# shell (verified empirically by the install-manifest tests).
install_manifest_digest_or_empty() {
  local key="$1" path="$2" value rc=0
  trap - ERR
  value="$(install_manifest_digest "$key" "$path")" && rc=0 || rc=$?
  trap 'on_err $? $LINENO' ERR
  [[ $rc -eq 0 ]] && printf '%s' "$value"
  return 0
}

install_manifest_add() {
  local vendor="$1" kind="$2" name="$3" staged="$4" installed="$5" install_kind="$6"
  local source_digest projection_digest installed_source_digest installed_projection_digest
  source_digest="$(install_manifest_digest_or_empty x-planar-source-digest "$staged")"
  projection_digest="$(install_manifest_digest_or_empty x-planar-projection-digest "$staged")"
  [[ -f "$installed" ]] || {
    printf 'install.sh: managed projection was not installed at %s\n' "$installed" >&2
    return 1
  }
  installed_source_digest="$(install_manifest_digest_or_empty x-planar-source-digest "$installed")"
  installed_projection_digest="$(install_manifest_digest_or_empty x-planar-projection-digest "$installed")"
  # A digest is compared only when the STAGED side actually has one (old
  # skillrender-rendered content); scriptorium-rendered content has neither
  # side populated, so there is nothing to compare and no mismatch to report
  # — scriptorium's own manifest (check/status) owns that drift signal now.
  if [[ -n "$source_digest" && "$installed_source_digest" != "$source_digest" ]] || \
     [[ -n "$projection_digest" && "$installed_projection_digest" != "$projection_digest" ]]; then
    printf 'install.sh: installed projection digest mismatch at %s\n' "$installed" >&2
    return 1
  fi
  INSTALL_MANIFEST_ROW_VENDOR+=("$vendor")
  INSTALL_MANIFEST_ROW_KIND+=("$kind")
  INSTALL_MANIFEST_ROW_NAME+=("$name")
  INSTALL_MANIFEST_ROW_STAGED+=("$staged")
  INSTALL_MANIFEST_ROW_INSTALLED+=("$installed")
  INSTALL_MANIFEST_ROW_INSTALL_KIND+=("$install_kind")
  INSTALL_MANIFEST_ROW_SOURCE_DIGEST+=("$source_digest")
  INSTALL_MANIFEST_ROW_PROJECTION_DIGEST+=("$projection_digest")
}

install_manifest_agent_name() {
  local name
  name="$(basename "$1")"
  name="${name%.agent.md}"
  name="${name%.toml}"
  name="${name%.md}"
  printf '%s' "$name"
}

# Record only the canonical files wired by install.sh. Destination-only files
# (including local-* personal extensions) are intentionally never discovered.
install_manifest_record_vendor() {
  local vendor="$1" planar_home="$2" user_home="$3" codex_home="$4"
  local f name staged installed
  planar_home="$(cd "$planar_home" && pwd -P)"
  user_home="$(cd "$user_home" && pwd -P)"
  if [[ "$vendor" == "codex" ]]; then
    codex_home="$(cd "$codex_home" && pwd -P)"
  fi
  INSTALL_MANIFEST_VENDORS+=("$vendor")
  case "$vendor" in
    claude)
      while IFS= read -r -d '' f; do
        name="$(basename "$f" .md)"
        install_manifest_add claude skill "$name" "$f" "$user_home/.claude/commands/$name.md" link
      done < <(find "$planar_home/commands/claude" -maxdepth 1 -type f -name 'pl-*.md' -print0)
      while IFS= read -r -d '' f; do
        name="$(install_manifest_agent_name "$f")"
        install_manifest_add claude agent "$name" "$f" "$user_home/.claude/agents/$(basename "$f")" link
      done < <(find "$planar_home/agents/claude" -maxdepth 1 -type f -print0)
      ;;
    codex)
      # Scriptorium's built-in Codex profile stages skill render output as
      # one pl-<slug>/SKILL.md directory per skill (docs/format.md § 2.1's
      # `layout: dir`), not the flat pl-*.md files Claude/Copilot/Gemini get
      # — see install.sh's install_codex_vendor for the matching read side.
      while IFS= read -r -d '' f; do
        name="$(basename "$(dirname "$f")")"
        staged="$planar_home/codex-skills/$name/SKILL.md"
        installed="$codex_home/skills/$name/SKILL.md"
        install_manifest_add codex skill "$name" "$staged" "$installed" copy
      done < <(find "$planar_home/skills/codex" -mindepth 2 -maxdepth 2 -type f -name 'SKILL.md' -print0)
      while IFS= read -r -d '' f; do
        name="$(install_manifest_agent_name "$f")"
        install_manifest_add codex agent "$name" "$f" "$codex_home/agents/$(basename "$f")" link
      done < <(find "$planar_home/agents/codex" -maxdepth 1 -type f -print0)
      ;;
    copilot)
      while IFS= read -r -d '' f; do
        name="$(basename "$f" .md)"
        staged="$planar_home/copilot-skills/$name/SKILL.md"
        installed="$user_home/.copilot/skills/$name/SKILL.md"
        install_manifest_add copilot skill "$name" "$staged" "$installed" copy
      done < <(find "$planar_home/skills/copilot" -maxdepth 1 -type f -name 'pl-*.md' -print0)
      while IFS= read -r -d '' f; do
        name="$(install_manifest_agent_name "$f")"
        install_manifest_add copilot agent "$name" "$f" "$user_home/.copilot/agents/$(basename "$f")" link
      done < <(find "$planar_home/agents/copilot" -maxdepth 1 -type f -print0)
      ;;
    gemini)
      while IFS= read -r -d '' f; do
        name="$(basename "$f" .md)"
        staged="$planar_home/gemini-skills/$name/SKILL.md"
        installed="$user_home/.gemini/antigravity-cli/skills/$name/SKILL.md"
        install_manifest_add gemini skill "$name" "$staged" "$installed" copy
      done < <(find "$planar_home/skills/gemini" -maxdepth 1 -type f -name 'pl-*.md' -print0)
      while IFS= read -r -d '' f; do
        name="$(install_manifest_agent_name "$f")"
        install_manifest_add gemini agent "$name" "$f" "$user_home/.gemini/antigravity-cli/agents/$(basename "$f")" link
      done < <(find "$planar_home/agents/gemini" -maxdepth 1 -type f -print0)
      ;;
    *) return 1 ;;
  esac
}

install_manifest_json_quote() {
  local value="$1"
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  value=${value//$'\n'/\\n}
  value=${value//$'\r'/\\r}
  value=${value//$'\t'/\\t}
  printf '"%s"' "$value"
}

# The temp file is created beside the authority file so mv is a same-filesystem
# atomic rename. FD 3 is explicitly closed before replacement; a failed build
# removes the temp and leaves the previous manifest authoritative.
install_manifest_write() {
  local destination="$1" tmp="${1}.tmp.$$" i
  rm -f "$tmp"
  if ! {
    exec 3>"$tmp"
    printf '{\n  "version": %s,\n  "build_id": ' "$INSTALL_MANIFEST_VERSION" >&3
    install_manifest_json_quote "$INSTALL_MANIFEST_BUILD_ID" >&3
    printf ',\n  "install_mode": ' >&3
    install_manifest_json_quote "$INSTALL_MANIFEST_MODE" >&3
    printf ',\n  "vendors": [' >&3
    for ((i = 0; i < ${#INSTALL_MANIFEST_VENDORS[@]}; i++)); do
      [[ $i -gt 0 ]] && printf ', ' >&3
      install_manifest_json_quote "${INSTALL_MANIFEST_VENDORS[$i]}" >&3
    done
    printf '],\n  "extras": [' >&3
    for ((i = 0; i < ${#INSTALL_MANIFEST_EXTRAS[@]}; i++)); do
      [[ $i -gt 0 ]] && printf ', ' >&3
      install_manifest_json_quote "${INSTALL_MANIFEST_EXTRAS[$i]}" >&3
    done
    printf '],\n  "projections": [\n' >&3
    for ((i = 0; i < ${#INSTALL_MANIFEST_ROW_VENDOR[@]}; i++)); do
      [[ $i -gt 0 ]] && printf ',\n' >&3
      printf '    {"vendor": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_VENDOR[$i]}" >&3
      printf ', "kind": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_KIND[$i]}" >&3
      printf ', "name": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_NAME[$i]}" >&3
      printf ', "staged_path": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_STAGED[$i]}" >&3
      printf ', "installed_path": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_INSTALLED[$i]}" >&3
      printf ', "install_kind": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_INSTALL_KIND[$i]}" >&3
      printf ', "source_digest": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_SOURCE_DIGEST[$i]}" >&3
      printf ', "projection_digest": ' >&3; install_manifest_json_quote "${INSTALL_MANIFEST_ROW_PROJECTION_DIGEST[$i]}" >&3
      printf '}' >&3
    done
    printf '\n  ]\n}\n' >&3
    exec 3>&-
  }; then
    exec 3>&- 2>/dev/null || true
    rm -f "$tmp"
    return 1
  fi
  if ! mv -f "$tmp" "$destination"; then
    rm -f "$tmp"
    return 1
  fi
}
