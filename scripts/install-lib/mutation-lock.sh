# shellcheck shell=bash
# shellcheck disable=SC2034  # PLANAR_LOCK_* are read by the sourcing script
#
# mutation-lock.sh -- the common mutation ownership protocol that serializes
# install, update (including its exec handoff into the installer) and
# uninstall (including --purge) of one Planar installation (plan 1122, task
# rel-install-order; tech spec 677, "Mutation ownership and cleanup";
# decision 1328).
#
# Sourced, never executed. Bash 3.2 (stock macOS /bin/bash) and Linux base
# utilities only: no python3, no flock(1). Functions return a status, set
# PLANAR_LOCK_* variables and never exit. Every command substitution that may
# fail clears the caller's ERR trap inside its own subshell (`trap - ERR`), so a
# caller running under `set -eE` and an ERR trap sees only the status.
#
# This header is the protocol's specification. The native implementation
# (`planar update`, plan 1122 M3) must follow it exactly so the two interoperate.
#
# THE COORDINATION DIRECTORY
#
#   L = "<canonical install root>.lock"     e.g. /Users/me/.planar.lock
#
#   A sibling of the install root, keyed by the root's canonical path
#   (prefix-guard.sh, planar_canonical_path): every spelling of the root reaches
#   the same L. It lives OUTSIDE the root, so removing or purging the
#   installation never removes it, and it is never removed by any Planar
#   program: it is created once (mkdir, mode 0700) and kept. A run refuses an L
#   that is a symlink, is not a directory, is not owned by the current user or
#   is writable by its group or by others (its `ls -ld` mode has `w` in the
#   group or other position). L's parent (the root's parent) must be writable
#   to create it; a run that cannot create it refuses and names the parent.
#   The mode check reads the permission bits only (`ls -ld`'s mode field here,
#   st_mode in the native port); an access control list is NOT read. On macOS
#   an ACL entry (`chmod +a`) that lets another user write L is therefore not
#   detected: keep ACLs off L. On Linux a POSIX ACL that grants write to a named
#   user or group shows in the group bits (the ACL mask) and is refused.
#   link(2) must work in L: when it fails for a reason other than an existing
#   record (no hard links on that filesystem, no permission), the run refuses
#   with that reason.
#
# RECORDS IN L (regular files, never symlinks)
#
#   owner.<G>     an ownership record of generation G (a positive decimal).
#   released.<G>  a hard link to owner.<G>: generation G was released.
#   handoff.<G>   a hard link to owner.<G>: the update handoff of G was consumed.
#   cand.<pid>.<nonce>
#                 a candidate record being prepared by process <pid>.
#
#   An ownership record is text, one `key=value` per line. This release writes
#   format 2:
#
#     planar-mutation-lock 2
#     gen=<G>
#     operation=install | update | uninstall
#     pid=<decimal pid of the owning process>
#     start=<start token of that process, see below>
#     host=<the host identity, see below>
#     node=<`uname -n` of the host; for diagnostics only, never compared>
#     nonce=<32 lowercase hex digits, fresh per acquisition>
#     root=<canonical install root>
#     tmp=<the updater's temporary directory, or empty>
#
#   `tmp` is set only by the updater: the exclusively created directory it
#   downloads into, <root>/.planar-update/<name> (see "Update temporaries").
#   Every key appears once; `host` and `node` are non-empty.
#
#   Format 1 (`planar-mutation-lock 1`) is what an older release wrote: the
#   same keys without `host`, its `node` being the host's identity. It is read,
#   never written (see "Older records"). A record in any other format, or with
#   a key its format does not have, is malformed.
#
# HOST IDENTITY (`host`; what makes a pid check meaningful)
#
#   The pid and start token can only be checked by a process that sees the same
#   processes: the same machine and, on Linux, the same pid namespace. Another
#   machine sharing the filesystem (a network home) must never judge a live
#   owner dead because its pid is absent locally. The identity is read afresh
#   at each use, from the first of:
#     machine-id:<id>        the first line of /etc/machine-id, when it is 32
#                            lowercase hex digits (Linux)
#     platform-uuid:<UUID>   the IOPlatformUUID that
#                            `LC_ALL=C /usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice`
#                            prints on the first line holding
#                            `"IOPlatformUUID" = "`, when it is 36 characters
#                            of [0-9A-F-] (macOS, the hardware UUID)
#     node:<name>            `uname -n`, when non-empty (a host offering
#                            neither: the older rule, see below)
#   then, when /proc/<pid>/ns/pid is a link reading `pid:[<digits>]` (Linux),
#   `/pidns:<digits>` is appended, so two containers that share a baked
#   machine-id, or a container and its host, are different hosts. A host that
#   yields none of the three writes `host=none`, which never proves locality,
#   not even on that host. Both ids survive a renamed host (scutil, DHCP) and
#   a reboot; the `node:` fallback does not survive a rename, which then
#   refuses (never reclaims). The shell and the native port derive the same
#   bytes; ioreg is run by its absolute path, so PATH cannot change it.
#
# OLDER RECORDS (written by an older release; never written by this one)
#
#   A format-1 record's `node` is `uname -n`: it is LOCAL only when it equals
#   this host's `uname -n` now. A renamed host cannot prove a format-1 record is
#   its own (the name it wrote may be another machine's), so that record is
#   AMBIGUOUS and refuses, naming both names; remove it by hand once no Planar
#   install, update or uninstall is running. Format-1 records can only exist on
#   hosts that ran a development build of the lock: no released Planar wrote
#   one.
#
# START TOKEN (PID reuse is detected by the start time, never by PID alone)
#
#   Linux: `proc:<boot_id>:<starttime>` -- /proc/sys/kernel/random/boot_id and
#          field 22 of /proc/<pid>/stat (clock ticks since boot; the field count
#          starts after the last `)` so a command name holding spaces or
#          parentheses cannot shift it).
#   Other: `psu:<TZ=UTC LC_ALL=C ps -o lstart= -p <pid>, runs of blanks squeezed
#          to one space>` (macOS: e.g. `psu:Tue Oct 6 19:57:57 2026`). `lstart`
#          is rendered in the caller's zone, so it is pinned to UTC: an owner
#          and a checker in different zones must read the same bytes for the
#          same live process.
#   Legacy: an older release wrote `ps:<lstart in the owner's local zone>`. Its
#          zone is unknown, so it cannot be compared; for a pid that still
#          exists it is AMBIGUOUS and refuses (never reclaimed as a reused pid).
#          A legacy record whose pid is gone is reclaimed like any dead owner.
#          The one exception is the update handoff, where the checker IS the
#          recorded process (see "Update handoff").
#   exec(2) keeps both the PID and the start time, which is what lets the
#   updater hand its ownership to the installer it execs.
#
# THE CURRENT OWNER is the record with the HIGHEST generation, Gmax. Records
# are never renamed and generations never go down, so Gmax only grows.
# Generation Gmax is FREE when released.<Gmax> exists, or when its owner is
# PROVEN dead:
#   - the record parses and is LOCAL: format 2 with `host` equal to this host's
#     identity (and not `none`), or format 1 with `node` equal to `uname -n`;
#   - `kill -0 <pid>` reports "No such process", or the process exists but its
#     start token differs from `start` (the PID was reused).
# A live owner (same PID and same start token) holds the lock. Anything else is
# AMBIGUOUS and refuses with a diagnostic, preserving every file: a malformed or
# foreign record, a record that is not local (another host's, one with no host
# identity, a format-1 record under another host name), a start token that
# cannot be read for an existing process, a legacy `ps:` start of an existing
# process, a symlinked or non-regular record. There is no timeout: a stuck live
# owner is never stolen from.
#
# ACQUIRE (operation OP)
#
#   1. Create L if missing; validate it.
#   2. Read Gmax (0 when there is no owner record). If generation Gmax is held
#      or ambiguous, refuse naming the holder's operation and PID.
#   3. Write the candidate record for G = Gmax + 1 to cand.<pid>.<nonce>, then
#      `ln cand owner.<G>`. link(2) fails when owner.<G> exists, so of any number
#      of processes racing for G exactly one creates it; the others start over
#      at step 2 (and then see a live owner). The shell knows link(2)'s EEXIST
#      only from ln's message under LC_ALL=C, which also carries the path: a
#      lost race is a message that ENDS in `: File exists` (or an owner.<G>
#      that now exists), so a root whose path contains `exists` is never
#      mistaken for one. Any other failure refuses with ln's message.
#   4. Re-read Gmax. If a generation above G exists, another process won a later
#      generation first (this one read a stale Gmax): remove owner.<G> (its own,
#      checked by nonce) and start over at step 2.
#   5. G is owned. Housekeeping: remove owner/released/handoff records of every
#      generation below G (an abandoned owner's `tmp` is read first, see below),
#      and candidate files of processes that no longer exist.
#   Two simultaneous reclaims of a dead owner both pass step 2 and race for the
#   same G in step 3, so at most one wins.
#
# RELEASE: `ln owner.<G> released.<G>`, only by the owner (nonce checked), and
#   only after every removal the operation makes has finished.
#
# UPDATE HANDOFF (no unlock gap)
#
#   The updater acquires with OP=update, records `tmp`, and execs the installer
#   with the environment variable
#     PLANAR_MUTATION_HANDOFF=<G>:<nonce>
#   and the argument vector `install.sh --prebuilt <dir> --cleanup <tmp>`. The
#   installer adopts generation G (planar_lock_adopt) only when owner.<G> is
#   Gmax, not released, its nonce and root match, its operation is `update`,
#   its PID is the installer's own PID with the same start token (exec kept
#   them), it names this host, and handoff.<G> does not exist; it then creates
#   handoff.<G> with link(2). Any other handoff -- forged, for another root,
#   from another process, or replayed (handoff.<G> exists) -- refuses and
#   changes nothing. The adopting installer owns G until it releases it after
#   its cleanup.
#
#   The record was written by the UPDATER, which is older than the installer it
#   downloaded, so it may be format 1 with a legacy `ps:` start. The installer
#   computes its OWN start token in the record's form (`ps:` is
#   `LC_ALL=C ps -o lstart=` in the inherited zone: exec kept the environment,
#   TZ included; any other form is planar_lock_start_token) and requires the
#   bytes to match, and the host to match in the record's format (`host` equal
#   to this host's identity, `none` included since the same process wrote it;
#   format 1: `node` equal to `uname -n`). The zone problem of a legacy start
#   does not arise here: the checker is the recorded process itself.
#
# UPDATE TEMPORARIES
#
#   The updater's temporary directory is <root>/.planar-update/<name>, <name>
#   matching [A-Za-z0-9][A-Za-z0-9._-]*, created exclusively (mkdir) and
#   recorded as `tmp` before anything is written into it. `--cleanup <dir>` is
#   honoured only when <dir> is byte-equal to the adopted record's `tmp` and
#   passes planar_update_tmp_valid. When an acquire reclaims a dead owner, the
#   new owner removes that owner's validated `tmp` (and nothing else matching a
#   name pattern): PLANAR_LOCK_RECLAIMED_TMP names it for the caller.
#
# Functions:
#   planar_lock_dir CANON                 print L for a canonical root
#   planar_lock_start_token PID           print the start token of PID
#   _pl_host_key                          print this host's identity
#   planar_lock_acquire CANON OP [TMP]    acquire; 0 owned, 1 refused
#   planar_lock_adopt CANON TOKEN         adopt an update handoff; 0 / 1
#   planar_lock_assert_owner              0 while this process still owns G
#   planar_lock_release                   release G (idempotent)
#   planar_update_tmp_valid CANON DIR     DIR is a valid update temporary
# On refusal PLANAR_LOCK_ERROR holds a one-line diagnostic. After success:
#   PLANAR_LOCK_DIR, PLANAR_LOCK_GEN, PLANAR_LOCK_NONCE, PLANAR_LOCK_OP,
#   PLANAR_LOCK_TMP (adopt: the record's tmp), PLANAR_LOCK_RECLAIMED_TMP
#   (acquire: an abandoned owner's validated tmp, else empty),
#   PLANAR_LOCK_RECLAIMED (the reclaimed owner's "<operation> pid <pid>", else
#   empty).

PLANAR_LOCK_DIR=""
PLANAR_LOCK_GEN=""
PLANAR_LOCK_NONCE=""
PLANAR_LOCK_OP=""
PLANAR_LOCK_TMP=""
PLANAR_LOCK_ROOT=""
PLANAR_LOCK_ERROR=""
PLANAR_LOCK_RECLAIMED=""
PLANAR_LOCK_RECLAIMED_TMP=""
PLANAR_LOCK_UPDATE_NS=".planar-update"
# Where the host identity is read from (see HOST IDENTITY). Fixed paths, never
# taken from the environment; a test that sources this file may point them at
# fixtures afterwards.
_PL_MACHINE_ID_FILE=/etc/machine-id
_PL_IOREG=/usr/sbin/ioreg

planar_lock_dir() {
  printf '%s.lock\n' "${1%/}"
}

# _pl_nonce -- 32 lowercase hex digits from /dev/urandom.
_pl_nonce() {
  local n
  n="$(trap - ERR; LC_ALL=C od -An -N16 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')" || n=""
  case "$n" in
    *[!0-9a-f]*|"") return 1 ;;
  esac
  [ "${#n}" -eq 32 ] || return 1
  printf '%s\n' "$n"
}

planar_lock_start_token() {
  local pid="$1" s rest boot="" tok
  case "$pid" in ""|*[!0-9]*) return 1 ;; esac
  if [ -r "/proc/$pid/stat" ]; then
    IFS= read -r s < "/proc/$pid/stat" 2>/dev/null || return 1
    rest="${s##*) }"
    [ "$rest" != "$s" ] || return 1
    if [ -r /proc/sys/kernel/random/boot_id ]; then
      IFS= read -r boot < /proc/sys/kernel/random/boot_id 2>/dev/null || boot=""
    fi
    set -f
    # shellcheck disable=SC2086  # word splitting of the stat fields is intended
    set -- $rest
    set +f
    [ "$#" -ge 20 ] || return 1
    shift 19
    case "$1" in ""|*[!0-9]*) return 1 ;; esac
    printf 'proc:%s:%s\n' "${boot:-none}" "$1"
    return 0
  fi
  s="$(trap - ERR; TZ=UTC LC_ALL=C ps -o lstart= -p "$pid" 2>/dev/null)" || return 1
  set -f
  # shellcheck disable=SC2086  # squeeze the blanks of ps's date
  set -- $s
  set +f
  [ "$#" -gt 0 ] || return 1
  tok="$*"
  printf 'psu:%s\n' "$tok"
}

# _pl_node -- this host's node name.
_pl_node() {
  local n
  n="$(trap - ERR; uname -n 2>/dev/null)" || n=""
  printf '%s\n' "${n:-unknown}"
}

# _pl_host_key -- this host's identity (see HOST IDENTITY): machine-id:<id>,
# platform-uuid:<UUID> or node:<name>, plus /pidns:<inode> on Linux; `none`
# when there is none of the three.
_pl_host_key() {
  local id="" line="" out="" ns="" name=""
  if [ -f "$_PL_MACHINE_ID_FILE" ] && [ -r "$_PL_MACHINE_ID_FILE" ]; then
    IFS= read -r line < "$_PL_MACHINE_ID_FILE" 2>/dev/null || true
    case "$line" in ""|*[!0-9a-f]*) line="" ;; esac
    if [ "${#line}" -eq 32 ]; then id="machine-id:$line"; fi
  fi
  if [ -z "$id" ] && [ -x "$_PL_IOREG" ]; then
    out="$(trap - ERR; LC_ALL=C "$_PL_IOREG" -rd1 -c IOPlatformExpertDevice 2>/dev/null)" || out=""
    while IFS= read -r line; do
      case "$line" in
        *'"IOPlatformUUID" = "'*)
          line="${line#*\"IOPlatformUUID\" = \"}"
          line="${line%%\"*}"
          case "$line" in ""|*[!0-9A-F-]*) line="" ;; esac
          if [ "${#line}" -eq 36 ]; then id="platform-uuid:$line"; fi
          break
          ;;
      esac
    done <<EOF
$out
EOF
  fi
  if [ -z "$id" ]; then
    name="$(trap - ERR; uname -n 2>/dev/null)" || name=""
    if [ -n "$name" ]; then id="node:$name"; fi
  fi
  if [ -z "$id" ]; then
    printf 'none\n'
    return 0
  fi
  if [ -L "/proc/$$/ns/pid" ]; then
    ns="$(trap - ERR; readlink "/proc/$$/ns/pid" 2>/dev/null)" || ns=""
    case "$ns" in
      "pid:["*"]") ns="${ns#"pid:["}"; ns="${ns%"]"}" ;;
      *) ns="" ;;
    esac
    case "$ns" in ""|*[!0-9]*) ns="" ;; esac
    if [ -n "$ns" ]; then id="$id/pidns:$ns"; fi
  fi
  printf '%s\n' "$id"
}

# _pl_record_local -- the parsed record (_PLR_*) was written on this host (see
# THE CURRENT OWNER). Status 1 with _PL_WHY set when it cannot be shown to be.
_pl_record_local() {
  local cur
  if [ "$_PLR_VER" = 1 ]; then
    cur="$(_pl_node)"
    [ "$_PLR_NODE" != "$cur" ] || return 0
    _PL_WHY="the owner ($_PLR_OP pid $_PLR_PID) is recorded on host $_PLR_NODE by an older Planar, which named hosts by name, and this host is named $cur: it may be another machine or this one before a rename, so its liveness cannot be checked"
    return 1
  fi
  if [ "$_PLR_HOST" = none ]; then
    _PL_WHY="the owner ($_PLR_OP pid $_PLR_PID) is recorded on host $_PLR_NODE with no host identity (no machine id and no host name), so it cannot be shown to be this host and its liveness cannot be checked"
    return 1
  fi
  cur="$(trap - ERR; _pl_host_key)"
  [ "$_PLR_HOST" != "$cur" ] || return 0
  _PL_WHY="the owner ($_PLR_OP pid $_PLR_PID) is recorded on host $_PLR_NODE (identity $_PLR_HOST), not this host (identity $cur), so its liveness cannot be checked"
  return 1
}

# _pl_own_start_like RECORDED -- this process's start token in RECORDED's form:
# a legacy `ps:` token (lstart in the inherited zone) for a `ps:` record, else
# planar_lock_start_token. Used only by the update handoff.
_pl_own_start_like() {
  local s
  case "$1" in
    ps:*)
      s="$(trap - ERR; LC_ALL=C ps -o lstart= -p "$$" 2>/dev/null)" || return 1
      set -f
      # shellcheck disable=SC2086  # squeeze the blanks of ps's date
      set -- $s
      set +f
      [ "$#" -gt 0 ] || return 1
      printf 'ps:%s\n' "$*"
      ;;
    *) planar_lock_start_token "$$" ;;
  esac
}

# _pl_validate_dir L -- L exists as a real directory owned by the caller and
# writable by no one else, as far as its mode bits say: an ACL is not read (see
# THE COORDINATION DIRECTORY).
_pl_validate_dir() {
  local l="$1" mode
  if [ -L "$l" ]; then
    PLANAR_LOCK_ERROR="the mutation lock directory $l is a symlink; refusing to use it (remove it only if no Planar install, update or uninstall can be running)"
    return 1
  fi
  if [ ! -d "$l" ]; then
    PLANAR_LOCK_ERROR="the mutation lock path $l exists and is not a directory; refusing to use it"
    return 1
  fi
  if [ ! -O "$l" ]; then
    PLANAR_LOCK_ERROR="the mutation lock directory $l is not owned by the current user; refusing to use it"
    return 1
  fi
  # Another user who can write L could forge or remove ownership records.
  mode="$(trap - ERR; LC_ALL=C ls -ld "$l" 2>/dev/null)" || mode=""
  mode="${mode%% *}"
  case "$mode" in
    ??????????*) ;;
    *) PLANAR_LOCK_ERROR="cannot read the permissions of the mutation lock directory $l; refusing to use it"; return 1 ;;
  esac
  case "$mode" in
    ?????w*|????????w*)
      PLANAR_LOCK_ERROR="the mutation lock directory $l is writable by its group or by others (mode $mode); refusing to use it. Make it private (chmod 700 $l) once no Planar install, update or uninstall is running."
      return 1
      ;;
  esac
  return 0
}

# _pl_ensure_dir L -- create L when missing (mode 0700), then validate it.
_pl_ensure_dir() {
  local l="$1"
  if [ ! -e "$l" ] && [ ! -L "$l" ]; then
    if ! (umask 077 && mkdir "$l") 2>/dev/null; then
      # Another process may have created it first; validation decides.
      if [ ! -e "$l" ] && [ ! -L "$l" ]; then
        PLANAR_LOCK_ERROR="cannot create the mutation lock directory $l (is $(dirname "$l") writable?)"
        return 1
      fi
    fi
  fi
  _pl_validate_dir "$l"
}

# _pl_max_gen L -- print the highest owner generation in L (0 when none).
_pl_max_gen() {
  local l="$1" f g max=0
  for f in "$l"/owner.*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    g="${f##*/owner.}"
    case "$g" in ""|*[!0-9]*) continue ;; esac
    [ "${#g}" -le 18 ] || continue
    if [ "$g" -gt "$max" ]; then max="$g"; fi
  done
  printf '%s\n' "$max"
}

# _pl_read_record FILE -- parse an ownership record into _PLR_* variables
# (_PLR_VER is the format, 1 or 2). Status 0 for a well-formed record.
_pl_read_record() {
  local f="$1" line key value n=0 seen=" "
  _PLR_VER=""; _PLR_GEN=""; _PLR_OP=""; _PLR_PID=""; _PLR_START=""; _PLR_HOST=""; _PLR_NODE=""
  _PLR_NONCE=""; _PLR_ROOT=""; _PLR_TMP=""
  [ ! -L "$f" ] && [ -f "$f" ] || return 1
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    [ "$n" -le 32 ] || return 1
    if [ "$n" -eq 1 ]; then
      case "$line" in
        "planar-mutation-lock 1") _PLR_VER=1 ;;
        "planar-mutation-lock 2") _PLR_VER=2 ;;
        *) return 1 ;;
      esac
      continue
    fi
    case "$line" in
      [a-z]*=*) ;;
      *) return 1 ;;
    esac
    key="${line%%=*}"; value="${line#*=}"
    case "$seen" in *" $key "*) return 1 ;; esac
    seen="$seen$key "
    case "$key" in
      gen) _PLR_GEN="$value" ;;
      operation) _PLR_OP="$value" ;;
      pid) _PLR_PID="$value" ;;
      start) _PLR_START="$value" ;;
      host) [ "$_PLR_VER" = 2 ] || return 1; _PLR_HOST="$value" ;;
      node) _PLR_NODE="$value" ;;
      nonce) _PLR_NONCE="$value" ;;
      root) _PLR_ROOT="$value" ;;
      tmp) _PLR_TMP="$value" ;;
      *) return 1 ;;
    esac
  done < "$f"
  case "$_PLR_GEN" in ""|*[!0-9]*) return 1 ;; esac
  case "$_PLR_PID" in ""|*[!0-9]*) return 1 ;; esac
  case "$_PLR_OP" in install|update|uninstall) ;; *) return 1 ;; esac
  case "$_PLR_NONCE" in ""|*[!0-9a-f]*) return 1 ;; esac
  [ "${#_PLR_NONCE}" -eq 32 ] || return 1
  [ -n "$_PLR_START" ] && [ -n "$_PLR_NODE" ] && [ -n "$_PLR_ROOT" ] || return 1
  if [ "$_PLR_VER" = 2 ]; then [ -n "$_PLR_HOST" ] || return 1; fi
  return 0
}

# _pl_owner_state L G -- classify generation G. Sets _PL_STATE to free,
# released, held, dead or ambiguous, and _PL_WHY to a diagnostic. Leaves the
# record's fields in _PLR_*.
_pl_owner_state() {
  local l="$1" g="$2" rec kout tok
  rec="$l/owner.$g"
  _PL_STATE="ambiguous"; _PL_WHY=""
  if [ "$g" = 0 ]; then _PL_STATE="free"; return 0; fi
  if [ ! -e "$rec" ] && [ ! -L "$rec" ]; then
    # Removed between the listing and the read: the caller starts over.
    _PL_STATE="vanished"; return 0
  fi
  if ! _pl_read_record "$rec"; then
    _PL_WHY="the ownership record $rec is malformed or not a regular file"
    return 0
  fi
  if [ "$_PLR_GEN" != "$g" ]; then
    _PL_WHY="the ownership record $rec names generation $_PLR_GEN"
    return 0
  fi
  if [ -e "$l/released.$g" ] || [ -L "$l/released.$g" ]; then
    if [ -f "$l/released.$g" ] && [ ! -L "$l/released.$g" ] && [ "$l/released.$g" -ef "$rec" ]; then
      _PL_STATE="released"; return 0
    fi
    _PL_WHY="the release marker $l/released.$g is not a link to its ownership record"
    return 0
  fi
  _pl_record_local || return 0
  kout="$(trap - ERR; LC_ALL=C kill -0 "$_PLR_PID" 2>&1)" && kout="exists" || true
  case "$kout" in
    exists|*[Pp]ermitted*) ;;
    *"No such process"*|*"no such process"*)
      _PL_STATE="dead"; _PL_WHY="$_PLR_OP pid $_PLR_PID is no longer running"; return 0 ;;
    *)
      _PL_WHY="cannot tell whether $_PLR_OP pid $_PLR_PID is running ($kout)"; return 0 ;;
  esac
  if ! tok="$(trap - ERR; planar_lock_start_token "$_PLR_PID")"; then
    _PL_WHY="$_PLR_OP pid $_PLR_PID exists but its start time cannot be read, so it cannot be told apart from a reused pid"
    return 0
  fi
  case "$_PLR_START" in
    ps:*)
      _PL_WHY="$_PLR_OP pid $_PLR_PID exists and its record holds a local-time start (written by an older Planar), which cannot be compared across time zones, so it cannot be told apart from a reused pid"
      return 0 ;;
  esac
  if [ "$tok" = "$_PLR_START" ]; then
    _PL_STATE="held"; _PL_WHY="$_PLR_OP pid $_PLR_PID"; return 0
  fi
  _PL_STATE="dead"; _PL_WHY="$_PLR_OP pid $_PLR_PID has exited and the pid was reused"
  return 0
}

# _pl_write_cand L G OP TMP -- write this process's candidate record for G.
# Sets _PL_CAND. Uses PLANAR_LOCK_NONCE.
_pl_write_cand() {
  local l="$1" g="$2" op="$3" tmp="$4" start host
  start="$(trap - ERR; planar_lock_start_token "$$")" || {
    PLANAR_LOCK_ERROR="cannot read this process's own start time (pid $$), so ownership could not be recorded"
    return 1
  }
  _PL_CAND="$l/cand.$$.$PLANAR_LOCK_NONCE"
  rm -f "$_PL_CAND" 2>/dev/null || true
  host="$(trap - ERR; _pl_host_key)"
  if ! (umask 077 && set -C && printf 'planar-mutation-lock 2\ngen=%s\noperation=%s\npid=%s\nstart=%s\nhost=%s\nnode=%s\nnonce=%s\nroot=%s\ntmp=%s\n' \
      "$g" "$op" "$$" "$start" "$host" "$(_pl_node)" "$PLANAR_LOCK_NONCE" "$PLANAR_LOCK_ROOT" "$tmp" > "$_PL_CAND") 2>/dev/null; then
    PLANAR_LOCK_ERROR="cannot write a candidate ownership record in $l"
    return 1
  fi
  return 0
}

# _pl_record_is_mine FILE -- FILE is a record carrying this acquisition's nonce
# and this process's pid.
_pl_record_is_mine() {
  _pl_read_record "$1" && [ "$_PLR_NONCE" = "$PLANAR_LOCK_NONCE" ] && [ "$_PLR_PID" = "$$" ]
}

# _pl_housekeep L G -- remove records of generations below G, reading an
# abandoned owner's tmp first, and candidates of exited processes.
_pl_housekeep() {
  local l="$1" g="$2" f k cpid rest kout
  for f in "$l"/owner.* "$l"/released.* "$l"/handoff.*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    k="${f##*.}"
    case "$k" in ""|*[!0-9]*) continue ;; esac
    [ "${#k}" -le 18 ] || continue
    [ "$k" -lt "$g" ] || continue
    rm -f "$f" 2>/dev/null || true
  done
  for f in "$l"/cand.*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    rest="${f##*/cand.}"; cpid="${rest%%.*}"
    case "$cpid" in ""|*[!0-9]*) continue ;; esac
    [ "$cpid" != "$$" ] || continue
    kout="$(trap - ERR; LC_ALL=C kill -0 "$cpid" 2>&1)" && continue
    case "$kout" in *"No such process"*|*"no such process"*) rm -f "$f" 2>/dev/null || true ;; esac
  done
  return 0
}

planar_lock_acquire() {
  local canon="$1" op="$2" tmp="${3:-}" l g new top tries=0 reclaimed="" reclaimed_tmp="" lerr
  if [ -n "$PLANAR_LOCK_GEN" ]; then
    PLANAR_LOCK_ERROR="this process already holds the mutation lock $PLANAR_LOCK_DIR (generation $PLANAR_LOCK_GEN)"
    return 1
  fi
  PLANAR_LOCK_ERROR=""; PLANAR_LOCK_RECLAIMED=""; PLANAR_LOCK_RECLAIMED_TMP=""
  PLANAR_LOCK_GEN=""; PLANAR_LOCK_TMP="$tmp"; PLANAR_LOCK_OP="$op"; PLANAR_LOCK_ROOT="$canon"
  case "$op" in install|update|uninstall) ;; *) PLANAR_LOCK_ERROR="unknown operation $op"; return 1 ;; esac
  l="$(planar_lock_dir "$canon")"
  PLANAR_LOCK_DIR="$l"
  _pl_ensure_dir "$l" || return 1
  PLANAR_LOCK_NONCE="$(trap - ERR; _pl_nonce)" || { PLANAR_LOCK_ERROR="cannot read random bytes for an ownership nonce (od and /dev/urandom are required)"; return 1; }
  while :; do
    tries=$((tries + 1))
    if [ "$tries" -gt 64 ]; then
      PLANAR_LOCK_ERROR="could not acquire the mutation lock $l: too much contention"
      rm -f "${_PL_CAND:-}" 2>/dev/null || true
      return 1
    fi
    g="$(_pl_max_gen "$l")"
    _pl_owner_state "$l" "$g"
    case "$_PL_STATE" in
      vanished) continue ;;
      held)
        PLANAR_LOCK_ERROR="another Planar $_PLR_OP (pid $_PLR_PID) is changing this installation; it holds the mutation lock $l (generation $g). Wait for it to finish, then re-run."
        rm -f "${_PL_CAND:-}" 2>/dev/null || true
        return 1
        ;;
      ambiguous)
        PLANAR_LOCK_ERROR="the mutation lock $l cannot be judged free: $_PL_WHY. Nothing was changed. Remove $l/owner.$g only after making sure no Planar install, update or uninstall is running."
        rm -f "${_PL_CAND:-}" 2>/dev/null || true
        return 1
        ;;
      dead)
        reclaimed="$_PLR_OP pid $_PLR_PID"
        reclaimed_tmp="$_PLR_TMP"
        ;;
      free|released)
        reclaimed=""; reclaimed_tmp=""
        ;;
    esac
    new=$((g + 1))
    _pl_write_cand "$l" "$new" "$op" "$tmp" || return 1
    if ! lerr="$(trap - ERR; LC_ALL=C ln "$_PL_CAND" "$l/owner.$new" 2>&1)"; then
      # Lost the race for G (link(2) gave EEXIST): start over. Any other
      # failure would repeat on every attempt, so name it. ln's message
      # carries the path, so only its END says EEXIST (see ACQUIRE step 3).
      case "$lerr" in *": File exists") continue ;; esac
      if [ -e "$l/owner.$new" ] || [ -L "$l/owner.$new" ]; then continue; fi
      PLANAR_LOCK_ERROR="cannot create the ownership record $l/owner.$new: ${lerr:-ln failed}. The mutation lock needs a directory you can write on a filesystem with hard links."
      rm -f "$_PL_CAND" 2>/dev/null || true
      return 1
    fi
    top="$(_pl_max_gen "$l")"
    if [ "$top" -gt "$new" ]; then
      if _pl_record_is_mine "$l/owner.$new"; then rm -f "$l/owner.$new" 2>/dev/null || true; fi
      continue
    fi
    rm -f "$_PL_CAND" 2>/dev/null || true
    PLANAR_LOCK_GEN="$new"
    PLANAR_LOCK_RECLAIMED="$reclaimed"
    if [ -n "$reclaimed_tmp" ] && planar_update_tmp_valid "$canon" "$reclaimed_tmp"; then
      PLANAR_LOCK_RECLAIMED_TMP="$reclaimed_tmp"
    fi
    _pl_housekeep "$l" "$new"
    return 0
  done
}

planar_lock_adopt() {
  local canon="$1" token="$2" l g nonce top own here there hrc=0
  if [ -n "$PLANAR_LOCK_GEN" ]; then
    PLANAR_LOCK_ERROR="this process already holds the mutation lock $PLANAR_LOCK_DIR (generation $PLANAR_LOCK_GEN); the update handoff was already used"
    return 1
  fi
  PLANAR_LOCK_ERROR=""; PLANAR_LOCK_RECLAIMED=""; PLANAR_LOCK_RECLAIMED_TMP=""
  PLANAR_LOCK_GEN=""; PLANAR_LOCK_ROOT="$canon"
  g="${token%%:*}"; nonce="${token#*:}"
  case "$g" in ""|*[!0-9]*) PLANAR_LOCK_ERROR="the update handoff '$token' is malformed (expected <generation>:<nonce>)"; return 1 ;; esac
  case "$nonce" in ""|*[!0-9a-f]*) PLANAR_LOCK_ERROR="the update handoff '$token' is malformed (expected <generation>:<nonce>)"; return 1 ;; esac
  l="$(planar_lock_dir "$canon")"
  PLANAR_LOCK_DIR="$l"
  if [ ! -e "$l" ] && [ ! -L "$l" ]; then
    PLANAR_LOCK_ERROR="the update handoff names generation $g, but there is no mutation lock $l for this installation"
    return 1
  fi
  _pl_validate_dir "$l" || return 1
  if ! _pl_read_record "$l/owner.$g"; then
    PLANAR_LOCK_ERROR="the update handoff names generation $g, which has no valid ownership record in $l"
    return 1
  fi
  top="$(_pl_max_gen "$l")"
  if [ "$top" != "$g" ]; then
    PLANAR_LOCK_ERROR="the update handoff names generation $g, but generation $top owns $l"
    return 1
  fi
  if [ -e "$l/released.$g" ] || [ -L "$l/released.$g" ]; then
    PLANAR_LOCK_ERROR="the update handoff names generation $g, which was already released"
    return 1
  fi
  # The updater may be older than this installer: compare in the record's own
  # format (see UPDATE HANDOFF).
  own="$(trap - ERR; _pl_own_start_like "$_PLR_START")" || own=""
  if [ "$_PLR_VER" = 1 ]; then here="$(_pl_node)"; there="$_PLR_NODE"; else here="$(trap - ERR; _pl_host_key)"; there="$_PLR_HOST"; fi
  if [ "$_PLR_NONCE" != "$nonce" ] || [ "$_PLR_ROOT" != "$canon" ] || [ "$_PLR_OP" != update ] \
    || [ "$_PLR_PID" != "$$" ] || [ -z "$own" ] || [ "$_PLR_START" != "$own" ] || [ "$there" != "$here" ]; then
    PLANAR_LOCK_ERROR="the update handoff does not match the owner of generation $g (it must come from the updater that holds the lock for $canon and exec'd this installer)"
    return 1
  fi
  ln "$l/owner.$g" "$l/handoff.$g" 2>/dev/null || hrc=1
  if [ "$hrc" -ne 0 ]; then
    PLANAR_LOCK_ERROR="the update handoff of generation $g was already used; a handoff is accepted once"
    return 1
  fi
  PLANAR_LOCK_GEN="$g"; PLANAR_LOCK_NONCE="$nonce"; PLANAR_LOCK_OP="update"; PLANAR_LOCK_TMP="$_PLR_TMP"
  return 0
}

planar_lock_assert_owner() {
  local l="$PLANAR_LOCK_DIR" g="$PLANAR_LOCK_GEN" top
  [ -n "$l" ] && [ -n "$g" ] || { PLANAR_LOCK_ERROR="this process holds no mutation lock"; return 1; }
  top="$(_pl_max_gen "$l")"
  if [ "$top" != "$g" ] || [ -e "$l/released.$g" ] || ! _pl_record_is_mine "$l/owner.$g"; then
    PLANAR_LOCK_ERROR="this process no longer owns the mutation lock $l (generation $g)"
    return 1
  fi
  return 0
}

planar_lock_release() {
  local l="$PLANAR_LOCK_DIR" g="$PLANAR_LOCK_GEN"
  [ -n "$l" ] && [ -n "$g" ] || return 0
  if _pl_record_is_mine "$l/owner.$g"; then
    if [ ! -e "$l/released.$g" ]; then
      ln "$l/owner.$g" "$l/released.$g" 2>/dev/null || true
    fi
  fi
  PLANAR_LOCK_GEN=""
  return 0
}

# planar_update_tmp_valid CANON DIR -- DIR is <CANON>/.planar-update/<name>,
# byte for byte, with <name> a plain name, and both it and the namespace
# directory are real directories (not symlinks) owned by the caller.
planar_update_tmp_valid() {
  local canon="${1%/}" dir="$2" ns name
  ns="$canon/$PLANAR_LOCK_UPDATE_NS"
  case "$dir" in
    "$ns"/*) name="${dir#"$ns"/}" ;;
    *) return 1 ;;
  esac
  case "$name" in
    ""|.|..|*/*|[!A-Za-z0-9]*|*[!A-Za-z0-9._-]*) return 1 ;;
  esac
  [ ! -L "$ns" ] && [ -d "$ns" ] && [ -O "$ns" ] || return 1
  [ ! -L "$dir" ] && [ -d "$dir" ] && [ -O "$dir" ] || return 1
  return 0
}
