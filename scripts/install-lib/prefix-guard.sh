# shellcheck shell=bash
# shellcheck disable=SC2016  # the refusal messages name $HOME literally
#
# prefix-guard.sh -- the install-root guard shared by install.sh and the
# uninstaller (plan 1122, task rel-prefix-guard; tech spec 677, "Prefix guard"
# and the decision "The install root is guarded, and older installs are
# adopted").
#
# Sourced, never executed. Requires journal.sh (sourced below).
#
#   planar_canonical_path PATH
#       Print PATH made absolute and with every symlink and `.`/`..` resolved.
#       Works for a path that does not exist yet (the missing tail is appended
#       lexically). Fails (status 1) on an empty path or a symlink loop.
#       Uses only `readlink` and builtins, so macOS bash 3.2 and Linux agree.
#
#   planar_prefix_guard ROOT FORCE OPERATION
#       ROOT       the install root as the operator gave it (may be empty)
#       FORCE      1 when --force was given, else 0
#       OPERATION  `install` or `uninstall`, used in messages
#       Status 0: ROOT may be used; PLANAR_PREFIX_CANON holds its canonical form.
#       Status 2: ROOT is the empty string, `/`, or resolves to $HOME or `/`
#                 (by canonical string, or, for an existing root, by device and
#                 inode, which catches case variants and firmlinks).
#                 Refused before anything is touched, and FORCE makes no
#                 difference.
#       Status 1: ROOT is an existing directory that is not empty and carries
#                 none of: the install stamp, an executable bin/planar,
#                 planar.db, a valid recovery journal. FORCE=1 adopts it.
#       A refusal prints one line to stderr naming the path and the rule. The
#       function never removes, creates or writes anything and never exits; the
#       caller turns the status into its exit code.

_planar_guard_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/install-lib/journal.sh
source "$_planar_guard_dir/journal.sh"

planar_canonical_path() {
  local input="$1" resolved="/" remaining comp rest target links=0
  [ -n "$input" ] || return 1
  case "$input" in
    /*) remaining="$input" ;;
    *) remaining="$PWD/$input" ;;
  esac
  while [ -n "$remaining" ]; do
    case "$remaining" in
      */*) comp="${remaining%%/*}"; rest="${remaining#*/}" ;;
      *) comp="$remaining"; rest="" ;;
    esac
    remaining="$rest"
    case "$comp" in
      ""|.) continue ;;
      ..)
        resolved="${resolved%/*}"
        [ -n "$resolved" ] || resolved="/"
        continue
        ;;
    esac
    if [ "$resolved" = "/" ]; then
      target="/$comp"
    else
      target="$resolved/$comp"
    fi
    if [ -L "$target" ]; then
      links=$((links + 1))
      [ "$links" -le 40 ] || return 1
      local link
      link="$(readlink "$target")" || return 1
      case "$link" in
        /*) resolved="/"; remaining="$link/$remaining" ;;
        *) remaining="$link/$remaining" ;;
      esac
      continue
    fi
    resolved="$target"
  done
  printf '%s\n' "$resolved"
}

planar_prefix_guard() {
  local root="$1" force="$2" op="$3" canon home_canon="" verb
  PLANAR_PREFIX_CANON=""
  if [ "$op" = "uninstall" ]; then verb="remove"; else verb="adopt"; fi

  if [ -z "$root" ]; then
    printf 'refusing to %s: the install root is the empty string. Rule: the install root must never be empty, /, or $HOME; --force does not override this. Pass a real --prefix.\n' "$op" >&2
    return 2
  fi
  if ! canon="$(planar_canonical_path "$root")"; then
    printf 'refusing to %s: cannot resolve the install root %s (symlink loop?). Rule: the install root must resolve to a real path that is never empty, /, or $HOME.\n' "$op" "$root" >&2
    return 2
  fi
  if [ -n "${HOME:-}" ]; then
    home_canon="$(planar_canonical_path "$HOME")" || home_canon=""
  fi
  if [ "$canon" = "/" ]; then
    printf 'refusing to %s: the install root %s resolves to / . Rule: the install root must never be /, $HOME or empty; --force does not override this.\n' "$op" "$root" >&2
    return 2
  fi
  if [ -n "$home_canon" ] && [ "$canon" = "$home_canon" ]; then
    printf 'refusing to %s: the install root %s resolves to $HOME (%s). Rule: the install root must never be $HOME, / or empty; --force does not override this. Pass the actual Planar install root.\n' "$op" "$root" "$home_canon" >&2
    return 2
  fi
  # The canonical string resolves symlinks only. A root that exists can still
  # alias $HOME or / under another spelling (a case variant on a
  # case-insensitive volume, a firmlink, a bind mount), so compare identity
  # (device and inode). A root that does not exist cannot be either of them.
  if [ -e "$canon" ]; then
    if [ "$canon" -ef / ]; then
      printf 'refusing to %s: the install root %s is the same directory as / . Rule: the install root must never be /, $HOME or empty; --force does not override this.\n' "$op" "$root" >&2
      return 2
    fi
    if [ -n "${HOME:-}" ] && [ "$canon" -ef "$HOME" ]; then
      printf 'refusing to %s: the install root %s is the same directory as $HOME (%s). Rule: the install root must never be $HOME, / or empty; --force does not override this. Pass the actual Planar install root.\n' "$op" "$root" "$HOME" >&2
      return 2
    fi
  fi
  # shellcheck disable=SC2034  # read by the caller after a successful check
  PLANAR_PREFIX_CANON="$canon"

  # A root that does not exist yet, or is empty, is a fresh install.
  if [ ! -e "$canon" ] && [ ! -L "$canon" ]; then
    return 0
  fi
  if [ ! -d "$canon" ]; then
    printf '%s exists and is not a directory; refusing to %s it. Pass a different --prefix.\n' "$canon" "$op" >&2
    return 1
  fi
  if [ -z "$(ls -A "$canon" 2>/dev/null)" ]; then
    return 0
  fi
  # Ownership signs: the install stamp, an executable bin/planar, planar.db (a
  # preserved database counts; the retired agent.db does not), or a validated
  # recovery journal.
  if [ -e "$canon/.planar-install" ] || [ -x "$canon/bin/planar" ] || [ -e "$canon/planar.db" ]; then
    return 0
  fi
  if recovery_journal_valid "$canon"; then
    return 0
  fi
  if [ "$force" = "1" ]; then
    return 0
  fi
  printf '%s does not look like a Planar install (no bin/planar, no planar.db, no .planar-install stamp, no valid recovery journal). Refusing to %s it. Pass the correct --prefix, or re-run with --force to %s it anyway.\n' "$canon" "$op" "$verb" >&2
  return 1
}
