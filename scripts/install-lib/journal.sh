# shellcheck shell=bash
#
# journal.sh -- the installer's recovery journal: file location, format and
# the validity predicate the prefix guard uses (plan 1122, task rel-prefix-guard;
# tech spec 677, "Order of an install" and "Mutation ownership and cleanup").
#
# SCOPE. This file defines only the narrow seam the prefix guard needs: where
# the journal lives, what a well-formed one looks like, and
# `recovery_journal_valid`. The install state machine (writing the journal,
# phase transitions, recovery, owner identity, retention and removal) is task
# rel-install-order and extends this file; it must keep every rule below.
#
# LOCATION. `<canonical-root>/.planar-journal`, a regular file directly under
# the install root, written by the installer with an atomic rename. It sits
# beside the install stamp (`.planar-install`) and is never a symlink.
#
# FORMAT (version 1). A UTF-8/ASCII text file of at most 65536 bytes made of
# printable characters only. Lines end in a newline; the last may omit it.
#
#   planar-journal 1            line 1, exactly: the format marker and version
#   root=<canonical root>       required, once: the canonical install root this
#                               journal belongs to; must equal the root being
#                               checked byte for byte
#   phase=<phase>               required, once: prepared | mutating | complete |
#                               aborted-before-mutation | uninstalling
#   <key>=<value>               zero or more further lines. key is
#                               [a-z][a-z0-9_]*, may not repeat, and may not be
#                               `root` or `phase`. Reserved for later tasks
#                               (operation, owner, target release, per-subtree
#                               progress); a reader that does not know a key
#                               ignores it but still requires it well formed.
#
# A file that is missing, a symlink, not a regular file, not owned by the
# current user, oversized, holds a non-printable byte, has any line that is not
# in the forms above, names a different root, a version other than 1 or an
# unknown phase is NOT valid. A bare `.staging-*` directory, a `.old` entry or
# any other marker is never evidence: only this file is. Validity is evidence
# that an installer started here, not proof of liveness; ownership and
# liveness belong to the mutation lock.
#
# Bash 3.2 and Linux base utilities only; no python3. Functions return a status
# and never exit.

PLANAR_JOURNAL_NAME=".planar-journal"
PLANAR_JOURNAL_MAX_BYTES=65536

# planar_journal_path ROOT -- print the journal path for a canonical root.
planar_journal_path() {
  printf '%s/%s\n' "${1%/}" "$PLANAR_JOURNAL_NAME"
}

# planar_journal_phase_valid PHASE -- succeed for a known journal phase.
planar_journal_phase_valid() {
  case "$1" in
    prepared|mutating|complete|aborted-before-mutation|uninstalling) return 0 ;;
    *) return 1 ;;
  esac
}

# recovery_journal_valid CANONICAL_ROOT -- succeed when CANONICAL_ROOT holds a
# well-formed version-1 journal that belongs to it. Sets PLANAR_JOURNAL_PHASE to
# the journal's phase on success.
recovery_journal_valid() {
  local root="$1" jf line key value n=0 seen_root=0 seen_phase=0 seen_keys=" "
  PLANAR_JOURNAL_PHASE=""
  [ -n "$root" ] || return 1
  jf="$(planar_journal_path "$root")"
  [ ! -L "$jf" ] || return 1
  [ -f "$jf" ] || return 1
  [ -O "$jf" ] || return 1
  local size nonprint
  size="$(wc -c < "$jf" | tr -d ' ')"
  [ -n "$size" ] && [ "$size" -le "$PLANAR_JOURNAL_MAX_BYTES" ] || return 1
  nonprint="$(LC_ALL=C tr -d '[:print:]\n' < "$jf" | wc -c | tr -d ' ')"
  [ "$nonprint" = "0" ] || return 1
  local phase=""
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    if [ "$n" -eq 1 ]; then
      [ "$line" = "planar-journal 1" ] || return 1
      continue
    fi
    case "$line" in
      [a-z]*=*) ;;
      *) return 1 ;;
    esac
    key="${line%%=*}"
    value="${line#*=}"
    case "$key" in
      *[!a-z0-9_]*) return 1 ;;
    esac
    case "$seen_keys" in
      *" $key "*) return 1 ;;
    esac
    seen_keys="$seen_keys$key "
    case "$key" in
      root)
        seen_root=1
        [ "$value" = "$root" ] || return 1
        ;;
      phase)
        seen_phase=1
        planar_journal_phase_valid "$value" || return 1
        phase="$value"
        ;;
    esac
  done < "$jf"
  [ "$n" -ge 1 ] && [ "$seen_root" -eq 1 ] && [ "$seen_phase" -eq 1 ] || return 1
  # shellcheck disable=SC2034  # read by the caller after a successful check
  PLANAR_JOURNAL_PHASE="$phase"
  return 0
}
