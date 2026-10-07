# shellcheck shell=bash
# shellcheck disable=SC2034  # INSTALL_STATE_* are read by the caller
#
# install-state.sh -- the install state machine's file operations (plan 1122,
# task rel-install-order; tech spec 677, "Order of an install" steps 2-4, 7 and
# 9, the journal phases after it, and "Mutation ownership and cleanup").
#
# Sourced by install.sh after journal.sh, data-paths.sh and mutation-lock.sh.
# Every function works on the canonical install root, reads and writes the
# journal through J_* (journal.sh), returns a status and never exits; a refusal
# leaves its reason in INSTALL_STATE_ERROR and removes nothing. Bash 3.2 and
# base utilities only.
#
# MANAGED SUBTREES are $PLANAR_JOURNAL_SUBTREES (install.sh sets the list). Each
# is replaced by two renames inside the root, recording progress first:
#
#   pending     --(journal: backing_up, live and staged inodes)-->
#   rename <root>/<n> to <root>/<n>.old      (skipped when there is no live <n>)
#               --(journal: backed_up)-->
#   rename <staging>/<n> to <root>/<n>
#               --(journal: swapped)-->
#
# A rename never targets an existing name, so live is never renamed into, nested
# under or over a backup. A symlinked subtree (link mode) is renamed, never
# followed. On a retry, planar_state_reconcile maps an interruption between a
# rename and its journal update back to a recorded state from the inodes it
# recorded, restores a journal-owned <n>.old whose live name is missing, and
# refuses (removing nothing) when the files match no recorded state.
#
# The backups have no PID in their names; only the journal associates them with
# the transaction. Unknown `.staging-*` and `*.old` entries are reported and
# kept.
#
# TEST-ONLY FAULT HOOK. planar_install_fault POINT does nothing unless BOTH
#   PLANAR_INSTALL_TEST_FAULT=<action>@<point>[,<action>@<point>...]
#   PLANAR_INSTALL_TEST_FAULT_ARMED=test-only
# are set. Actions: `kill` (SIGKILL the installer: no trap runs), `pause` (create
# $PLANAR_INSTALL_TEST_FAULT_DIR/paused.<point> and wait for resume.<point>) and
# `fail` (the function returns 1; the call site treats it as that step failing).
# The installer's points are named where it calls the hook. Nothing else reads
# these variables.

INSTALL_STATE_ERROR=""

planar_install_fault() {
  local point="$1" spec="${PLANAR_INSTALL_TEST_FAULT-}" item action at dir i=0
  [ -n "$spec" ] || return 0
  [ "${PLANAR_INSTALL_TEST_FAULT_ARMED-}" = "test-only" ] || return 0
  dir="${PLANAR_INSTALL_TEST_FAULT_DIR-}"
  while [ -n "$spec" ]; do
    item="${spec%%,*}"
    if [ "$item" = "$spec" ]; then spec=""; else spec="${spec#*,}"; fi
    action="${item%%@*}"; at="${item#*@}"
    [ "$at" = "$point" ] || continue
    case "$action" in
      kill)
        printf 'TEST FAULT: kill at %s\n' "$point" >&2
        kill -9 $$
        ;;
      pause)
        [ -n "$dir" ] || return 0
        : > "$dir/paused.$point"
        while [ ! -e "$dir/resume.$point" ]; do
          i=$((i + 1)); [ "$i" -lt 6000 ] || break
          sleep 0.1
        done
        ;;
      fail)
        printf 'TEST FAULT: fail at %s\n' "$point" >&2
        return 1
        ;;
    esac
  done
  return 0
}

# _ps_ino PATH -- the inode of PATH itself (a symlink is not followed), or
# `none` when nothing is there.
_ps_ino() {
  local out
  if [ ! -e "$1" ] && [ ! -L "$1" ]; then printf 'none\n'; return 0; fi
  out="$(trap - ERR; LC_ALL=C ls -di "$1" 2>/dev/null)" || out=""
  out="${out#"${out%%[! ]*}"}"
  out="${out%% *}"
  case "$out" in ""|*[!0-9]*) printf 'unknown\n' ;; *) printf '%s\n' "$out" ;; esac
}

# _ps_get KEY / _ps_set KEY VALUE -- J_* access by name.
_ps_get() { local v="J_$1"; printf '%s' "${!v-}"; }
_ps_set() { printf -v "J_$1" '%s' "$2"; }

# planar_state_remove_tree ROOT PATH -- remove PATH (a backup or staging entry
# directly under ROOT) without following a symlink, unless it is or holds a data
# path. Status 1 when refused.
planar_state_remove_tree() {
  local root="$1" p="$2"
  case "$p" in "$root"/*) ;; *) INSTALL_STATE_ERROR="refusing to remove $p: it is not under $root"; return 1 ;; esac
  if planar_removal_blocked "$root" "$p"; then
    INSTALL_STATE_ERROR="refusing to remove $p: it holds or is the data path '$PLANAR_DATA_PATH_HIT'"
    return 1
  fi
  if [ -L "$p" ]; then rm -f "$p"; else rm -rf "$p"; fi
}

# planar_state_pick_staging -- print a fresh staging name, `.staging-<16 hex>`.
# The caller records it in J_staging and writes the journal BEFORE it creates
# the directory (with mkdir, which fails when the name exists), so a kill
# between the two leaves the name owned, never an unowned directory.
planar_state_pick_staging() {
  local n
  n="$(trap - ERR; _pl_nonce)" || return 1
  printf '.staging-%s\n' "$(printf '%s' "$n" | cut -c1-16)"
}

# planar_state_report_unknown ROOT -- report `.staging-*` and `*.old` entries
# directly under ROOT that the journal does not own. Prints one line each.
planar_state_report_unknown() {
  local root="$1" e base owned n
  owned=" ${J_staging-} "
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    if [ "$(_ps_get "$(planar_journal_sub_key "$n")_live")" != "" ]; then owned="$owned$n.old "; fi
  done
  for e in "$root"/.staging-* "$root"/*.old; do
    [ -e "$e" ] || [ -L "$e" ] || continue
    base="${e##*/}"
    case "$owned" in *" $base "*) continue ;; esac
    printf 'kept %s: no recovery journal owns it\n' "$e"
  done
}

# planar_state_unknown_backup ROOT -- status 0, naming it in
# INSTALL_STATE_ERROR, when a <n>.old the journal does not own blocks a subtree
# that still has to be swapped.
planar_state_unknown_backup() {
  local root="$1" n sk st
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    st="$(_ps_get "$sk")"
    [ "$st" = swapped ] && continue
    if [ -e "$root/$n.old" ] || [ -L "$root/$n.old" ]; then
      INSTALL_STATE_ERROR="$root/$n.old exists and no recovery journal owns it, so $n cannot be backed up without overwriting it. Nothing was changed. Move $root/$n.old aside (it may hold a previous install), then re-run."
      return 0
    fi
  done
  return 1
}

# planar_state_swap ROOT STAGING NAME -- swap one managed subtree (see the
# header). Status 1 with INSTALL_STATE_ERROR when it cannot proceed safely, or
# when the journal cannot be written.
planar_state_swap() {
  local root="$1" stage="$2" n="$3" sk st l o s
  sk="$(planar_journal_sub_key "$n")"
  st="$(_ps_get "$sk")"
  l="$root/$n"; o="$root/$n.old"; s="$stage/$n"
  [ "$st" = swapped ] && return 0
  if [ "$st" != pending ] && [ -n "$st" ]; then
    INSTALL_STATE_ERROR="$n is in state $st, which only recovery may resolve"
    return 1
  fi
  if [ ! -e "$s" ] && [ ! -L "$s" ]; then
    INSTALL_STATE_ERROR="the staged $s is missing"
    return 1
  fi
  if [ -e "$o" ] || [ -L "$o" ]; then
    INSTALL_STATE_ERROR="$o already exists; refusing to back $l up over it"
    return 1
  fi
  _ps_set "${sk}_live" "$(_ps_ino "$l")"
  _ps_set "${sk}_staged" "$(_ps_ino "$s")"
  _ps_set "$sk" backing_up
  planar_journal_write "$root" || { INSTALL_STATE_ERROR="cannot write the recovery journal"; return 1; }
  if [ "$(_ps_get "${sk}_live")" != none ]; then
    mv "$l" "$o" || { INSTALL_STATE_ERROR="cannot rename $l to $o"; return 1; }
  fi
  planar_install_fault "backup:$n" || { INSTALL_STATE_ERROR="test fault after backing up $n"; return 1; }
  _ps_set "$sk" backed_up
  planar_journal_write "$root" || { INSTALL_STATE_ERROR="cannot write the recovery journal"; return 1; }
  planar_install_fault "backed-up:$n" || { INSTALL_STATE_ERROR="test fault after recording the backup of $n"; return 1; }
  if [ -e "$l" ] || [ -L "$l" ]; then
    INSTALL_STATE_ERROR="$l appeared while it was being replaced; refusing to nest the staged copy under it"
    return 1
  fi
  mv "$s" "$l" || { INSTALL_STATE_ERROR="cannot rename $s to $l"; return 1; }
  planar_install_fault "swap:$n" || { INSTALL_STATE_ERROR="test fault after swapping $n"; return 1; }
  _ps_set "$sk" swapped
  planar_journal_write "$root" || { INSTALL_STATE_ERROR="cannot write the recovery journal"; return 1; }
  return 0
}

# planar_state_reconcile ROOT -- bring every subtree of a mutating journal back
# to pending or swapped (tech spec 677 step 3). A subtree interrupted after its
# backup with no live name is restored from its journal-owned <n>.old. Writes
# the journal. Status 1, with nothing removed, when the files match no recorded
# state. Prints one line per restored subtree.
planar_state_reconcile() {
  local root="$1" n sk st rl rs li oi l o
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    st="$(_ps_get "$sk")"
    rl="$(_ps_get "${sk}_live")"; rs="$(_ps_get "${sk}_staged")"
    l="$root/$n"; o="$root/$n.old"
    li="$(_ps_ino "$l")"; oi="$(_ps_ino "$o")"
    case "$st" in
      ""|pending) continue ;;
      swapped)
        if [ "$li" != "$rs" ]; then
          INSTALL_STATE_ERROR="the journal records $n as swapped in, but $l is not the staged copy it recorded (inode $li, recorded $rs). Nothing was removed."
          return 1
        fi
        continue
        ;;
      backing_up)
        if [ "$li" = "$rl" ] && [ "$oi" = none ]; then
          _ps_set "$sk" pending; continue
        fi
        if [ "$rl" != none ] && [ "$li" = none ] && [ "$oi" = "$rl" ]; then
          st=backed_up
        else
          INSTALL_STATE_ERROR="cannot tell whether $l was backed up: live inode $li, backup inode $oi, recorded live $rl. Nothing was removed; inspect $l and $o."
          return 1
        fi
        ;;
    esac
    if [ "$st" = backed_up ]; then
      if [ "$li" = "$rs" ] && { [ "$rl" = none ] || [ "$oi" = "$rl" ]; }; then
        _ps_set "$sk" swapped; continue
      fi
      if [ "$li" = none ] && { { [ "$rl" = none ] && [ "$oi" = none ]; } || { [ "$rl" != none ] && [ "$oi" = "$rl" ]; }; }; then
        if [ "$rl" != none ]; then
          mv "$o" "$l" || { INSTALL_STATE_ERROR="cannot restore $o to $l"; return 1; }
          printf 'restored %s from %s\n' "$l" "$o"
        fi
        _ps_set "$sk" pending; _ps_set "${sk}_live" ""; _ps_set "${sk}_staged" ""
        continue
      fi
      INSTALL_STATE_ERROR="cannot reconcile $n: live inode $li, backup inode $oi, recorded live $rl and staged $rs. Nothing was removed; inspect $l and $o."
      return 1
    fi
    INSTALL_STATE_ERROR="the journal records an unknown state '$st' for $n. Nothing was removed."
    return 1
  done
  planar_journal_write "$root" || { INSTALL_STATE_ERROR="cannot write the recovery journal"; return 1; }
  return 0
}

# planar_state_dispose_staging ROOT [NAME...] -- remove the named journal-owned
# staging directories (all of J_staging when none is named) and drop them from
# J_staging. Does not write the journal.
planar_state_dispose_staging() {
  local root="$1" s keep="" want
  shift
  want=" $* "
  [ "$#" -gt 0 ] || want=" ${J_staging-} "
  for s in ${J_staging-}; do
    case "$want" in
      *" $s "*)
        case "$s" in
          .staging-*) ;;
          *) keep="$keep${keep:+ }$s"; continue ;;
        esac
        case "$s" in */*|*[!A-Za-z0-9._-]*) keep="$keep${keep:+ }$s"; continue ;; esac
        if [ -e "$root/$s" ] || [ -L "$root/$s" ]; then
          planar_state_remove_tree "$root" "$root/$s" || { keep="$keep${keep:+ }$s"; continue; }
        fi
        ;;
      *) keep="$keep${keep:+ }$s" ;;
    esac
  done
  J_staging="$keep"
  return 0
}

# planar_state_dispose_backups ROOT -- remove each journal-owned <n>.old whose
# inode is the one recorded as the live subtree before its swap. A backup that
# does not match is reported and kept.
planar_state_dispose_backups() {
  local root="$1" n sk rl o oi
  for n in ${PLANAR_JOURNAL_SUBTREES-}; do
    sk="$(planar_journal_sub_key "$n")"
    rl="$(_ps_get "${sk}_live")"
    [ -n "$rl" ] && [ "$rl" != none ] || continue
    o="$root/$n.old"
    oi="$(_ps_ino "$o")"
    [ "$oi" != none ] || continue
    if [ "$oi" = "$rl" ]; then
      planar_state_remove_tree "$root" "$o" || printf 'kept %s: %s\n' "$o" "$INSTALL_STATE_ERROR"
    else
      printf 'kept %s: it is not the backup the journal recorded\n' "$o"
    fi
  done
  return 0
}

# planar_state_finish ROOT -- the end of a committed transaction: dispose of the
# backups and the staging, then remove the journal last.
planar_state_finish() {
  local root="$1"
  planar_state_dispose_backups "$root"
  planar_state_dispose_staging "$root"
  rm -f "$(planar_journal_path "$root")"
}
