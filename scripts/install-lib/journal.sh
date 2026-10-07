# shellcheck shell=bash
#
# journal.sh -- the installer's recovery journal: file location, format and
# the validity predicate the prefix guard uses (plan 1122, task rel-prefix-guard;
# tech spec 677, "Order of an install" and "Mutation ownership and cleanup").
#
# The prefix guard uses `recovery_journal_valid`; the install state machine
# (install-state.sh, task rel-install-order) writes and reads the journal with
# `planar_journal_write` and `planar_journal_load`. The uninstaller writes the
# `uninstalling` phase through the same writer.
#
# LOCATION. `<canonical-root>/.planar-journal`, a regular file directly under
# the install root, never a symlink. It is replaced atomically: the writer
# creates `<root>/.planar-journal.tmp.<pid>` exclusively and renames it over
# the journal, so a reader (or a run after a KILL) sees the old or the new
# journal, never a mix.
#
# DURABILITY. planar_journal_write does not flush: a write survives the
# installer being killed, not a power loss. install.sh runs sync(1) after the
# two records recovery depends on, `mutating` (before the first live change)
# and `complete` (before the backups are disposed of). The per-subtree swap
# records between them are not flushed; after a power loss recovery maps the
# files back to a recorded state from the inodes, or refuses.
#
# FORMAT (version 1). A text file of at most 65536 bytes. Every byte must be
# tab, newline, a printable ASCII character (0x20-0x7e) or a byte of 0x80 or
# above; other C0 control bytes and DEL make it invalid. Bytes of 0x80 and above
# are accepted as they are, without checking that they form valid UTF-8, so a
# root under a non-ASCII home validates. Lines end in a newline; the last may
# omit it.
#
#   planar-journal 1            line 1, exactly: the format marker and version
#   root=<canonical root>       required, once: the canonical install root this
#                               journal belongs to; must equal the root being
#                               checked byte for byte
#   phase=<phase>               required, once: prepared | mutating | complete |
#                               aborted-before-mutation | uninstalling
#   <key>=<value>               zero or more further lines. key is
#                               [a-z][a-z0-9_]*, may not repeat, and may not be
#                               `root` or `phase`; a reader that does not know a
#                               key ignores it but still requires it well formed.
#
# The keys the installer writes (install-state.sh), in this order:
#   operation        install | update | uninstall
#   operation_id     32 hex digits, fresh per attempt
#   owner_pid, owner_start, owner_lock
#                    the owning process, its start token and its lock generation
#                    (mutation-lock.sh)
#   source           prebuilt | checkout
#   mode             copy | link
#   target_version, target_sha, target_schema
#                    the release being installed (release.json of the bundle, or
#                    the checkout's version line, HEAD and highest migration)
#   target_path      the bundle directory or the checkout
#   release_base     the validated release base URL (prebuilt), else empty
#   retry            the durable command that completes this transaction
#   staging          space-separated `.staging-<token>` names this transaction
#                    owns (directly under the root)
#   db               missing | behind | current: the pre-swap database probe
#   sub_<n>          per managed subtree (`-` in the name becomes `_`):
#                    pending | backing_up | backed_up | swapped
#   sub_<n>_live     inode of the live subtree before the swap, or `none`
#   sub_<n>_staged   inode of the staged subtree that replaces it
#
# A file that is missing, a symlink, not a regular file, not owned by the
# current user, oversized, holds a byte outside the set above, has any line that
# is not in the forms above, names a different root, a version other than 1 or
# an unknown phase is NOT valid. A bare `.staging-*` directory, a `.old` entry or
# any other marker is never evidence: only this file is. Validity is evidence
# that an installer started here, not proof of liveness; ownership and
# liveness belong to the mutation lock (mutation-lock.sh).
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
  # Reject C0 control characters (tab and newline allowed) and DEL; bytes
  # >= 0x80 are legal so a root under a non-ASCII home validates.
  nonprint="$(LC_ALL=C tr -d '\011\012\040-\176\200-\377' < "$jf" | wc -c | tr -d ' ')"
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

# The fixed keys, in the order the writer emits them after root and phase.
PLANAR_JOURNAL_KEYS="operation operation_id owner_pid owner_start owner_lock source mode target_version target_sha target_schema target_path release_base retry staging db"

# planar_journal_sub_key NAME -- the journal key stem of a managed subtree.
planar_journal_sub_key() {
  printf 'sub_%s\n' "$(printf '%s' "$1" | tr '-' '_')"
}

# planar_journal_clear -- unset every J_* journal variable for SUBTREES.
planar_journal_clear() {
  local k n sk
  # shellcheck disable=SC2034  # read by the caller
  J_root=""; J_phase=""
  for k in $PLANAR_JOURNAL_KEYS; do printf -v "J_$k" '%s' ""; done
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    printf -v "J_$sk" '%s' ""; printf -v "J_${sk}_live" '%s' ""; printf -v "J_${sk}_staged" '%s' ""
  done
  return 0
}

# planar_journal_write ROOT -- write J_phase and the J_* keys (and the subtree
# keys of $PLANAR_JOURNAL_SUBTREES) as ROOT's journal, atomically. Status 1
# when a value holds a newline or the file cannot be written.
planar_journal_write() {
  local root="${1%/}" jf tmp k v n sk body var
  jf="$(planar_journal_path "$root")"
  tmp="$jf.tmp.$$"
  planar_journal_phase_valid "${J_phase-}" || return 1
  body="planar-journal 1
root=$root
phase=$J_phase
"
  for k in $PLANAR_JOURNAL_KEYS; do
    var="J_$k"; v="${!var-}"
    case "$v" in *"
"*) return 1 ;; esac
    body="$body$k=$v
"
  done
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    for k in "$sk" "${sk}_live" "${sk}_staged"; do
      var="J_$k"; v="${!var-}"
      body="$body$k=$v
"
    done
  done
  rm -f "$tmp" 2>/dev/null || true
  (umask 077 && set -C && printf '%s' "$body" > "$tmp") 2>/dev/null || { rm -f "$tmp" 2>/dev/null; return 1; }
  mv -f "$tmp" "$jf" 2>/dev/null || { rm -f "$tmp" 2>/dev/null; return 1; }
  return 0
}

# planar_journal_load ROOT -- validate ROOT's journal and read it into J_*
# (J_phase, J_root, the fixed keys and the subtree keys of
# $PLANAR_JOURNAL_SUBTREES; unknown keys are ignored). Status 1 when it is not
# valid; the J_* variables are then cleared.
planar_journal_load() {
  local root="${1%/}" jf line key value n=0 known sk
  planar_journal_clear
  recovery_journal_valid "$root" || return 1
  jf="$(planar_journal_path "$root")"
  known=" root phase $PLANAR_JOURNAL_KEYS "
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    known="$known$sk ${sk}_live ${sk}_staged "
  done
  n=0
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    [ "$n" -gt 1 ] || continue
    key="${line%%=*}"; value="${line#*=}"
    case "$known" in *" $key "*) printf -v "J_$key" '%s' "$value" ;; esac
  done < "$jf"
  return 0
}
