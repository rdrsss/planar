# shellcheck shell=bash
#
# ownership.sh -- the rules that prove a file under a vendor directory is
# Planar's (plan 1122, task rel-uninstall-script; tech spec 677, "Ownership
# evidence" and "The uninstaller"). Plan 1104's vendor sweep defined them in
# install.sh; install.sh and the uninstaller (scripts/uninstall.sh) both source
# this file, so the installer's sweep and the uninstaller's removals apply one
# check.
#
# Ownership evidence: a symlink whose target lies under $PLANAR_HOME (or, for a
# retired local-* command, under $HOME/.planar/local), a directory or file whose
# bytes equal the staged entry it was placed from (an OpenCode agent is compared
# with its derived form), or a path recorded in install-manifest.json (the
# callers read the manifest; this file judges each recorded path).
#
# Inputs from the caller's environment: PLANAR_HOME, HOME and CODEX_HOME. The
# sweep reports through the caller's `log` and `warn`, which the caller defines
# before it calls a sweep function. Sourced, never executed. Bash 3.2 and base utilities only;
# no python3.
#
#   sweep_* / run_sweep     the installer's sweep of the projections older
#                           installs placed, the planar-<role> agents among
#                           them (removes only proven entries).
#   opencode_derive FILE    print the OpenCode form of a staged agent.
#   uninstall_owned STAGED INSTALLED VENDOR KIND
#                           status 0 when INSTALLED is still what Planar placed
#                           from STAGED; never removes anything.

# The vendor name list is shared with install.sh and the uninstaller.
# shellcheck source=scripts/install-lib/managed-lists.sh
source "$(dirname "${BASH_SOURCE[0]}")/managed-lists.sh"

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
  for v in $PLANAR_VENDOR_NAMES; do pats+=("$PLANAR_HOME/agents/$v/*"); done
  while IFS= read -r -d '' link; do
    if sweep_link_into "$link" ${pats[@]+"${pats[@]}"} || sweep_retired_toplevel_agent "$link"; then
      sweep_remove "$link" "agent symlink"
    fi
  done < <(find "$dir" -maxdepth 1 -type l -print0)
}

# sweep_role_agents <dir> <fmt> — the planar-<role> agents an install placed in
# the vendor agent directory <dir> while Planar still placed role agents. <fmt>
# is the directory's placed form: md (Claude Code, Gemini CLI, Antigravity),
# copilot (planar-<role>.agent.md), opencode (the derived form) or toml (Codex).
# The previous install's staged trees $PLANAR_HOME/agents/ and
# $PLANAR_HOME/codex-agents/ are the evidence, so this runs before the cleanup
# list removes them. An entry is removed when it is a symlink (live or dangling)
# into one of those two trees, or a regular file whose bytes equal what placing
# its staged source wrote. A planar-local-* entry (`planar local link`) is never
# touched. A planar-<role> entry with no proof is left: it is reported when the
# previous install manifest records it (Planar placed it and it changed since),
# and otherwise it is not Planar's and is passed over silently.
sweep_role_agents() {
  local dir="$1" fmt="$2" entry name role src tmp proven
  [[ -d "$dir" ]] || return 0
  while IFS= read -r -d '' entry; do
    name="$(basename "$entry")"
    [[ "$name" == planar-local-* ]] && continue
    case "$fmt" in
      toml)    [[ "$name" == *.toml ]] || continue; role="${name%.toml}"; src="$PLANAR_HOME/codex-agents/$name" ;;
      copilot) [[ "$name" == *.agent.md ]] || continue; role="${name%.agent.md}"; src="$PLANAR_HOME/agents/$role.md" ;;
      *)       [[ "$name" == *.md ]] || continue; role="${name%.md}"; src="$PLANAR_HOME/agents/$role.md" ;;
    esac
    [[ -n "${role#planar-}" ]] || continue
    proven=0
    if [[ -L "$entry" ]]; then
      sweep_link_into "$entry" "$PLANAR_HOME/agents/planar-*" "$PLANAR_HOME/codex-agents/planar-*" && proven=1
    elif [[ -f "$entry" && -f "$src" ]]; then
      if [[ "$fmt" == opencode ]]; then
        tmp="$(mktemp)"
        if opencode_derive "$src" > "$tmp" 2>/dev/null && cmp -s "$tmp" "$entry"; then proven=1; fi
        rm -f "$tmp"
      elif cmp -s "$src" "$entry"; then
        proven=1
      fi
    fi
    if [[ "$proven" -eq 1 ]]; then
      sweep_remove "$entry" "role agent"
    elif sweep_manifest_records "$entry"; then
      warn "left $entry: Planar placed it, but it no longer matches the copy Planar placed (changed after the install?); Planar no longer installs role agents, so remove it yourself if you do not need it"
      SWEEP_LEFT=$((SWEEP_LEFT + 1))
    fi
  done < <(find "$dir" -maxdepth 1 \( -type f -o -type l \) -name 'planar-*' -print0)
}

# sweep_manifest_records <path> — true when $PLANAR_HOME/install-manifest.json
# holds <path> as a whole JSON string. The escaping mirrors
# install_manifest_json_quote in install-manifest.sh, which writes the
# manifest (the backslash, the quote, newline, carriage return and tab); it is
# repeated here because the uninstaller sources this file without that one.
# Keep the two in step.
sweep_manifest_records() {
  local manifest="$PLANAR_HOME/install-manifest.json" quoted="$1"
  [[ -f "$manifest" && ! -L "$manifest" ]] || return 1
  quoted="${quoted//\\/\\\\}"
  quoted="${quoted//\"/\\\"}"
  quoted="${quoted//$'\n'/\\n}"
  quoted="${quoted//$'\r'/\\r}"
  quoted="${quoted//$'\t'/\\t}"
  grep -qF -- "\"$quoted\"" "$manifest" 2>/dev/null
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
  sweep_role_agents "$HOME/.claude/agents" md
  sweep_role_agents "$CODEX_HOME/agents" toml
  sweep_role_agents "$HOME/.copilot/agents" copilot
  sweep_role_agents "$HOME/.gemini/agents" md
  sweep_role_agents "$HOME/.gemini/antigravity-cli/agents" md
  sweep_role_agents "$HOME/.config/opencode/agents" opencode
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
# what Planar placed: a symlink to <staged> or into $PLANAR_HOME, or content
# equal to <staged> (an OpenCode agent is compared with its derived form).
uninstall_owned() {
  local staged="$1" installed="$2" vendor="$3" kind="$4" tmp rc=0 target
  if [[ -L "$installed" ]]; then
    target="$(readlink "$installed" 2>/dev/null || true)"
    [[ -n "$target" && ( "$target" == "$staged" || "$target" == "$PLANAR_HOME"/* ) ]]
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
