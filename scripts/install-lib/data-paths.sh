# shellcheck shell=bash
# shellcheck disable=SC2016,SC2034  # markdown backticks; PLANAR_DATA_PATH_HIT is read by callers
#
# data-paths.sh -- the one list of Planar data paths (plan 1122, task
# rel-data-paths; tech spec 677, "Data path" and the decision "Data paths are
# listed once and resolve from the home directory").
#
# A data path is a path the installer never removes and the uninstaller
# preserves without --purge. install.sh and the uninstaller source this file;
# INSTALL.md's "Preserved paths" section is generated from the same list
# (`bash data-paths.sh --markdown`) and a test compares the two.
#
# Sourced, never executed, except for the two read-only modes at the bottom.
# Requires prefix-guard.sh for planar_canonical_path (sourced below). bash 3.2
# safe; no python.
#
# Each path lives under the install root (default $HOME/.planar) unless one of
# five environment variables relocates it:
#   PLANAR_DB              planar.db and its -wal and -shm sidecars
#   PLANAR_CONFIG_PATH     config.toml
#   PLANAR_WORKBENCH_ROOT  workbench/
#   PLANAR_LOCAL_HOME      local/   (the runtime treats it as a stand-in for
#                          $HOME, so the location is $PLANAR_LOCAL_HOME/.planar/local)
#   PLANAR_TEMPLATES_DIR   templates/
# A variable that is unset or empty relocates nothing (the runtime ignores an
# empty PLANAR_CONFIG_PATH and PLANAR_TEMPLATES_DIR the same way). One that
# resolves to the default location, under the install root or under
# $HOME/.planar, relocates nothing either. A relative value is taken against the
# current directory. A leading `~/` is expanded against $HOME exactly where the
# runtime expands it, and nowhere else:
#   PLANAR_DB              literal (src/cmd/internal/environment.cpp,
#                          resolve_db_path)
#   PLANAR_CONFIG_PATH     `~` expanded (src/cmd/internal/config_path.cpp)
#   PLANAR_WORKBENCH_ROOT  `~` expanded (src/engine/workbench/root.cpp,
#                          resolve_root and expand_tilde)
#   PLANAR_LOCAL_HOME      literal (src/engine/local/manifest.cpp,
#                          resolve_home_and_root)
#   PLANAR_TEMPLATES_DIR   `~` expanded (src/cmd/planar/handlers/templates/
#                          command.cpp, resolve_templates_root)
#
# The default location is $HOME/.planar/NAME. The install root defaults to the
# same directory, but --prefix or PLANAR_HOME can move the root elsewhere, so
# the predicates protect both $ROOT/NAME and $HOME/.planar/NAME.
#
#   planar_data_path_names
#       Print the data path names, one per line, in canonical order.
#   planar_root_has_data_path ROOT
#       Status 0 when ROOT contains a data path, at its default location under
#       ROOT or at a relocated one inside ROOT. This is ownership evidence for
#       planar_prefix_guard: an uninstall leaves data behind and removes the
#       stamp and bin/. Unknown files alone are not evidence.
#   planar_data_paths_list ROOT
#       One line per data path, canonical order: NAME|KIND|LOCATION|RELOCATED_BY
#       KIND is `file` or `dir`; LOCATION is where the path actually lives;
#       RELOCATED_BY is the variable name, or empty when it lives under ROOT.
#   planar_data_paths_report ROOT
#       Print one line per relocated data path naming it, the variable, the
#       location, and that it is left where it is. Prints nothing when none is.
#   planar_is_data_path ROOT PATH
#       Status 0 when PATH is a data path or lies inside one, at its default
#       location under ROOT or at its relocated one. Sets PLANAR_DATA_PATH_HIT.
#   planar_covers_data_path ROOT PATH
#       Status 0 when PATH is a strict ancestor of a data path (removing it
#       recursively would remove one). Sets PLANAR_DATA_PATH_HIT.
#   planar_removal_blocked ROOT PATH
#       Status 0 when removing PATH is not allowed: either predicate above.
#       Sets PLANAR_DATA_PATH_HIT to the data path's name.
#   planar_data_paths_markdown
#       Print INSTALL.md's preserved-paths list, one bullet per path.
#
# None of these removes, creates or writes anything.

[ -z "${_PLANAR_DATA_PATHS_LOADED-}" ] || return 0
_PLANAR_DATA_PATHS_LOADED=1
_planar_data_paths_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# prefix-guard.sh sources this file back (to ask planar_root_has_data_path); the
# load flag above ends the cycle whichever file is sourced first.
if ! type planar_canonical_path >/dev/null 2>&1; then
  # shellcheck source=scripts/install-lib/prefix-guard.sh
  source "$_planar_data_paths_dir/prefix-guard.sh"
fi

# NAME|KIND|RELOCATION VARIABLE|WHAT IT HOLDS. The only copy of this list.
PLANAR_DATA_PATH_TABLE='planar.db|file|PLANAR_DB|the SQLite database
planar.db-wal|file|PLANAR_DB|its write-ahead log, which holds committed data not yet checkpointed
planar.db-shm|file|PLANAR_DB|its shared-memory index
queue-logs|dir||output of detached queue runs
retired|dir||databases and logs retired by an upgrade
workbench|dir|PLANAR_WORKBENCH_ROOT|the planning workbench
config.toml|file|PLANAR_CONFIG_PATH|the operator configuration
local|dir|PLANAR_LOCAL_HOME|operator-local skills and agents
workspaces|dir||workspace definitions
models|dir||model catalogs
execute|dir||execute profiles
templates|dir|PLANAR_TEMPLATES_DIR|operator-editable templates'

planar_data_path_names() {
  local name rest
  while IFS='|' read -r name rest; do
    [ -n "$name" ] && printf '%s\n' "$name"
  done <<EOF
$PLANAR_DATA_PATH_TABLE
EOF
  return 0
}

# _planar_dp_value VAR -- the variable's value as the runtime reads it: `~`
# expanded for the variables whose reader expands it (see the header), a relative
# value made absolute; empty when unset or empty.
_planar_dp_value() {
  local v="${!1-}"
  [ -n "$v" ] || return 0
  case "$1" in
    PLANAR_DB|PLANAR_LOCAL_HOME) ;;
    *)
      case "$v" in
        \~) v="${HOME-}" ;;
        \~/*) v="${HOME-}/${v#\~/}" ;;
      esac
      ;;
  esac
  case "$v" in
    /*) ;;
    *) v="$PWD/$v" ;;
  esac
  printf '%s' "$v"
}

# _planar_dp_location ROOT NAME VAR -- where the path actually lives.
_planar_dp_location() {
  local root="$1" name="$2" var="$3" val
  val="$(_planar_dp_value "$var")"
  if [ -z "$val" ]; then
    printf '%s/%s' "${root%/}" "$name"
    return 0
  fi
  case "$var" in
    PLANAR_DB) printf '%s%s' "$val" "${name#planar.db}" ;;
    PLANAR_LOCAL_HOME) printf '%s/.planar/local' "${val%/}" ;;
    *) printf '%s' "$val" ;;
  esac
}

# _planar_dp_canon PATH -- canonical form, or PATH itself when unresolvable.
_planar_dp_canon() {
  planar_canonical_path "$1" 2>/dev/null || printf '%s' "$1"
}

planar_data_paths_list() {
  local root="$1" name kind var _what loc loc_canon dflt by
  while IFS='|' read -r name kind var _what; do
    [ -n "$name" ] || continue
    loc="$(_planar_dp_location "$root" "$name" "$var")"
    by=""
    if [ -n "$var" ] && [ -n "$(_planar_dp_value "$var")" ]; then
      by="$var"
      loc_canon="$(_planar_dp_canon "$loc")"
      for dflt in "${root%/}/$name" "${HOME-}/.planar/$name"; do
        if [ "$loc_canon" = "$(_planar_dp_canon "$dflt")" ]; then by=""; fi
      done
    fi
    printf '%s|%s|%s|%s\n' "$name" "$kind" "$loc" "$by"
  done <<EOF
$PLANAR_DATA_PATH_TABLE
EOF
  return 0
}

planar_data_paths_report() {
  local name kind loc by
  while IFS='|' read -r name kind loc by; do
    [ -n "$by" ] || continue
    case "$name" in planar.db-*) continue ;; esac
    if [ "$name" = "planar.db" ]; then
      printf 'data path %s (and its -wal and -shm sidecars) is relocated by %s to %s; it is left where it is\n' "$name" "$by" "$loc"
    else
      printf 'data path %s is relocated by %s to %s; it is left where it is\n' "$name" "$by" "$loc"
    fi
  done <<EOF
$(planar_data_paths_list "$1")
EOF
  return 0
}

# _planar_dp_scan ROOT PATH MODE -- MODE `within`: PATH is a data path or inside
# one; MODE `covers`: PATH is a strict ancestor of one. Both the default
# location and the relocated one are checked.
_planar_dp_scan() {
  local root="$1" path="$2" mode="$3" p d name kind loc by
  PLANAR_DATA_PATH_HIT=""
  p="$(_planar_dp_canon "$path")"
  while IFS='|' read -r name kind loc by; do
    [ -n "$name" ] || continue
    for d in "$(_planar_dp_canon "${root%/}/$name")" "$(_planar_dp_canon "${HOME-}/.planar/$name")" "$(_planar_dp_canon "$loc")"; do
      if [ "$mode" = "within" ]; then
        if [ "$p" = "$d" ]; then PLANAR_DATA_PATH_HIT="$name"; return 0; fi
        case "$p" in "$d"/*) PLANAR_DATA_PATH_HIT="$name"; return 0 ;; esac
      else
        if [ "$p" = "/" ]; then PLANAR_DATA_PATH_HIT="$name"; return 0; fi
        case "$d" in "$p"/*) PLANAR_DATA_PATH_HIT="$name"; return 0 ;; esac
      fi
    done
  done <<EOF
$(planar_data_paths_list "$root")
EOF
  return 1
}

planar_root_has_data_path() {
  local root canon name kind loc by
  root="$(_planar_dp_canon "$1")"
  while IFS='|' read -r name kind loc by; do
    [ -n "$name" ] || continue
    if [ -e "$root/$name" ] || [ -L "$root/$name" ]; then return 0; fi
    canon="$(_planar_dp_canon "$loc")"
    case "$canon" in "$root"/*) if [ -e "$canon" ] || [ -L "$canon" ]; then return 0; fi ;; esac
  done <<EOF2
$(planar_data_paths_list "$root")
EOF2
  return 1
}

planar_is_data_path() { _planar_dp_scan "$1" "$2" within; }
planar_covers_data_path() { _planar_dp_scan "$1" "$2" covers; }
planar_removal_blocked() {
  planar_is_data_path "$1" "$2" || planar_covers_data_path "$1" "$2"
}

planar_data_paths_markdown() {
  local name kind var what shown
  while IFS='|' read -r name kind var what; do
    [ -n "$name" ] || continue
    shown="$name"
    [ "$kind" = "dir" ] && shown="$name/"
    if [ -n "$var" ]; then
      printf -- '- `%s`: %s; relocated by `%s`\n' "$shown" "$what" "$var"
    else
      printf -- '- `%s`: %s\n' "$shown" "$what"
    fi
  done <<EOF
$PLANAR_DATA_PATH_TABLE
EOF
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1-}" in
    --markdown) planar_data_paths_markdown ;;
    --names) planar_data_path_names ;;
    *) printf 'usage: data-paths.sh --markdown | --names\n' >&2; exit 2 ;;
  esac
fi
