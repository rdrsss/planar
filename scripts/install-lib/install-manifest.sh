#!/usr/bin/env bash
# Install-manifest support for install.sh. This file is sourced, not executed.
#
# Version 2 (plan 1104, M2). `vendors` lists the vendors the run found
# (claude codex copilot gemini antigravity opencode); `projections` holds one
# row per placed target (the skill directory, each agent file) with `vendor`
# one of those names, or `shared` for the ~/.agents/skills root that four
# vendors read; `extras` holds the staged paths and every placed file. A
# version 1 manifest (four retired vendors, or the interim extras-only layout)
# is read by `planar health` as `legacy` and rewritten by the next install.

INSTALL_MANIFEST_VERSION=2
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

# install_manifest_add <vendor> <kind> <name> <staged> <installed> <install_kind>
# — add one projection row. The digest fields stay empty: freshness is a byte
# comparison against the staged authority, made by `planar health`.
install_manifest_add() {
  local vendor="$1" kind="$2" name="$3" staged="$4" installed="$5" install_kind="$6"
  INSTALL_MANIFEST_ROW_VENDOR+=("$vendor")
  INSTALL_MANIFEST_ROW_KIND+=("$kind")
  INSTALL_MANIFEST_ROW_NAME+=("$name")
  INSTALL_MANIFEST_ROW_STAGED+=("$staged")
  INSTALL_MANIFEST_ROW_INSTALLED+=("$installed")
  INSTALL_MANIFEST_ROW_INSTALL_KIND+=("$install_kind")
  INSTALL_MANIFEST_ROW_SOURCE_DIGEST+=("")
  INSTALL_MANIFEST_ROW_PROJECTION_DIGEST+=("")
}

install_manifest_agent_name() {
  local name
  name="$(basename "$1")"
  name="${name%.agent.md}"
  name="${name%.toml}"
  name="${name%.md}"
  printf '%s' "$name"
}

# install_manifest_record_staged <planar_home> <repo_root> -- record the staged
# vendor-neutral trees as `extras`, one $PLANAR_HOME-relative path per file:
# skills/planar/**, the agents/*.md the checkout ships, and the rendered
# codex-agents/*.toml. They are not projections: no vendor owns them. `extras`
# is the schema's free-form string list. Call it after install_manifest_begin,
# which clears the list.
install_manifest_record_staged() {
  local planar_home="$1" repo_root="$2" f rel
  while IFS= read -r -d '' f; do
    rel="${f#"$planar_home"/}"
    install_manifest_add_extra "${rel//\/\//\/}"
  done < <(find "$planar_home/skills/planar/" -type f -print0 | sort -z)
  for f in "$repo_root"/agents/*.md; do
    [[ -f "$f" ]] && install_manifest_add_extra "agents/$(basename "$f")"
  done
  for f in "$planar_home"/codex-agents/*.toml; do
    [[ -f "$f" ]] && install_manifest_add_extra "codex-agents/$(basename "$f")"
  done
  return 0
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

# The single manifest writer, called after every placed target (install.sh) as
# well as at the ends. The temp file is created beside the authority file so mv
# is a same-filesystem atomic rename: a crash between the temp write and the
# rename leaves the previous complete manifest authoritative, never a truncated
# one. FD 3 is explicitly closed before replacement; a failed build removes the
# temp and leaves the previous manifest authoritative. When the rendered bytes
# equal the manifest already on disk the rename is skipped, so a re-run that
# changes nothing does not touch the file (its mtime stays).
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
  if [[ -f "$destination" ]] && cmp -s "$tmp" "$destination"; then
    rm -f "$tmp"
    return 0
  fi
  if ! mv -f "$tmp" "$destination"; then
    rm -f "$tmp"
    return 1
  fi
}
