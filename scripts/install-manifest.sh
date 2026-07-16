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

install_manifest_add() {
  local vendor="$1" kind="$2" name="$3" staged="$4" installed="$5" install_kind="$6"
  local source_digest projection_digest installed_source_digest installed_projection_digest
  source_digest="$(install_manifest_digest x-planar-source-digest "$staged")" || {
    printf 'install.sh: missing or invalid source digest in %s\n' "$staged" >&2
    return 1
  }
  projection_digest="$(install_manifest_digest x-planar-projection-digest "$staged")" || {
    printf 'install.sh: missing or invalid projection digest in %s\n' "$staged" >&2
    return 1
  }
  [[ -f "$installed" ]] || {
    printf 'install.sh: managed projection was not installed at %s\n' "$installed" >&2
    return 1
  }
  installed_source_digest="$(install_manifest_digest x-planar-source-digest "$installed")" || return 1
  installed_projection_digest="$(install_manifest_digest x-planar-projection-digest "$installed")" || return 1
  if [[ "$installed_source_digest" != "$source_digest" || "$installed_projection_digest" != "$projection_digest" ]]; then
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
      while IFS= read -r -d '' f; do
        name="$(basename "$f" .md)"
        staged="$planar_home/codex-skills/$name/SKILL.md"
        installed="$codex_home/skills/$name/SKILL.md"
        install_manifest_add codex skill "$name" "$staged" "$installed" copy
      done < <(find "$planar_home/skills/codex" -maxdepth 1 -type f -name 'pl-*.md' -print0)
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
