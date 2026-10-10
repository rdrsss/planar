#!/usr/bin/env bash
# The common mutation ownership protocol (plan 1122, task rel-install-order;
# tech spec 677, "Mutation ownership and cleanup"; decision 1328), driven
# through scripts/install-lib/mutation-lock.sh with real processes:
#   - acquire, release and generation housekeeping; the lock directory sits
#     beside the root, so removing (purging) the root cannot split ownership;
#   - a live owner is refused by name and pid; a killed owner is reclaimed; a
#     reused pid (same pid, other start time) is reclaimed; a malformed record,
#     another host's record, a symlinked lock directory and an unreadable start
#     time are ambiguous and refused with every file preserved;
#   - many rounds of simultaneous reclaim attempts never yield two owners;
#   - the update handoff is accepted once, only in the process that holds the
#     update record (exec keeps the pid and start time), and refused when
#     forged, replayed or adopted from another process;
#   - update temporaries are recognised only under <root>/.planar-update/;
#   - a killed owner's record survives a change of the host name (records name
#     the host by its machine identity), while a record from another machine,
#     from a host with no identity at all, or a host-name-keyed record from an
#     older Planar naming another name is never reclaimed; a live owner is
#     recognised across the rename;
#   - an older updater's record (format 1, a local-time `ps:` or a `psu:` start)
#     is adopted by the installer it execs, and a mismatched one is still refused;
#   - a failing link(2) is named by its reason even when the install root's path
#     contains `exists` or `: File exists`, and such a root locks normally;
#   - (macOS) an ACL on the lock directory is not read: only the mode bits are.
# Runs under stock bash 3.2 and Linux bash with base utilities only: PATH holds
# no python3. Nothing outside a scratch directory is touched.
# shellcheck disable=SC2016,SC2012,SC2010,SC2015  # literal $ in generated scripts; ls for listings
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$ROOT/scripts/install-lib/mutation-lock.sh"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
PIDS=()
cleanup() {
  local p
  for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT
fail() { printf 'install-lock-test: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); }
BASH_BIN="${BASH_UNDER_TEST:-/bin/bash}"
export HOME="$TMP/home"
mkdir -p "$HOME"

# A PATH of base utilities only (no python3), as an operator host may have.
BASEBIN="$TMP/basebin"
mkdir -p "$BASEBIN"
for n in od tr uname ps ln rm rmdir mkdir dirname sleep cat ls cut grep sed mv chmod readlink; do
  f="$(command -v "$n" || true)"
  case "$f" in /*) ln -s "$f" "$BASEBIN/$n" ;; esac
done
export PATH="$BASEBIN"
[[ -z "$(command -v python3 || true)" ]] || fail "python3 is reachable on the test PATH"

# Test-only identity overrides, applied after sourcing the library (the
# library itself reads no environment for them): LOCK_TEST_MID names a
# machine-id file to read instead of /etc/machine-id, and LOCK_TEST_NO_IOREG=1
# hides the macOS platform UUID.
OVERRIDES='if [ -n "${LOCK_TEST_MID-}" ]; then _PL_MACHINE_ID_FILE="$LOCK_TEST_MID"; fi; if [ -n "${LOCK_TEST_NO_IOREG-}" ]; then _PL_IOREG=/nonexistent/ioreg; fi'

# lk SCRIPT... -- run a fresh bash that sources the library, then SCRIPT.
lk() { "$BASH_BIN" -c 'set -eEuo pipefail; trap '\''echo ERR-TRAP-FIRED >&2'\'' ERR; source "$1"; eval "$2"; shift 2; eval "$*"' lk "$LIB" "$OVERRIDES" "$@"; }

# hold ROOT OP READY [TMPDIR] -- background holder; sets HOLD_PID.
hold() {
  local root="$1" op="$2" ready="$3" t="${4:-}"
  "$BASH_BIN" -c 'source "$1"; eval "$6"; planar_lock_acquire "$2" "$3" "$5" || { echo "$PLANAR_LOCK_ERROR" >&2; exit 1; }; echo "$PLANAR_LOCK_GEN" > "$4"; exec sleep 300' \
    hold "$LIB" "$root" "$op" "$ready" "$t" "$OVERRIDES" &
  HOLD_PID=$!
  PIDS+=("$HOLD_PID")
  local i=0
  while [[ ! -s "$ready" ]]; do
    i=$((i + 1)); [[ "$i" -lt 300 ]] || fail "holder never acquired"
    sleep 0.05
  done
}

# --- acquire, release, housekeeping --------------------------------------------------------

R="$TMP/a/.planar"
mkdir -p "$TMP/a"
L="$R.lock"
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN dir=$PLANAR_LOCK_DIR" && planar_lock_assert_owner && planar_lock_release && echo released')"
[[ "$out" == *"gen=1 dir=$L"* && "$out" == *released* ]] || fail "first acquire/release: $out"
[[ -d "$L" && ! -L "$L" ]] || fail "the lock directory is not a directory beside the root"
[[ "$(ls -ld "$L" | cut -c1-10)" == drwx------ ]] || fail "the lock directory is not mode 0700: $(ls -ld "$L")"
[[ -f "$L/owner.1" && -f "$L/released.1" && "$L/owner.1" -ef "$L/released.1" ]] || fail "release did not link released.1 to owner.1"
[[ "$(sed -n 1p "$L/owner.1")" == "planar-mutation-lock 2" ]] && grep -Fxq 'operation=install' "$L/owner.1" \
  && grep -Eq '^start=(psu|proc):' "$L/owner.1" && grep -Eq '^nonce=[0-9a-f]{32}$' "$L/owner.1" \
  && grep -Eq '^host=.' "$L/owner.1" && grep -Fxq "node=$(uname -n)" "$L/owner.1" \
  || fail "the ownership record is not the documented format: $(cat "$L/owner.1")"
out="$(lk 'planar_lock_acquire "'"$R"'" uninstall && echo "gen=$PLANAR_LOCK_GEN"')"
[[ "$out" == "gen=2" ]] || fail "second acquire after a release: $out"
[[ ! -e "$L/owner.1" && ! -e "$L/released.1" ]] || fail "housekeeping left generation 1: $(ls "$L")"
[[ ! -e "$R" ]] || fail "acquiring the lock created the install root"
pass

# --- a live owner is refused, even after the root is purged -----------------------------------

R="$TMP/b/.planar"; mkdir -p "$R"; L="$R.lock"
hold "$R" install "$TMP/b.ready"
rc=0; out="$(lk 'planar_lock_acquire "'"$R"'" update || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"install (pid $HOLD_PID)"* ]] || fail "a live owner was not refused by pid ($rc): $out"
[[ "$out" != *ERR-TRAP-FIRED* ]] || fail "the refusal fired the caller's ERR trap"
rm -rf "$R"
rc=0; out="$(lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"pid $HOLD_PID"* ]] || fail "purging the root let a second owner in ($rc): $out"
pass

# --- a killed owner is reclaimed, and its update temporary is named ------------------------------

kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN reclaimed=$PLANAR_LOCK_RECLAIMED"')"
[[ "$out" == "gen=2 reclaimed=install pid $HOLD_PID" ]] || fail "a killed owner was not reclaimed: $out"
R="$TMP/c/.planar"; mkdir -p "$R/.planar-update/dl1"
hold "$R" update "$TMP/c.ready" "$R/.planar-update/dl1"
kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "tmp=$PLANAR_LOCK_RECLAIMED_TMP"')"
[[ "$out" == "tmp=$R/.planar-update/dl1" ]] || fail "the reclaimed updater temporary was not named: $out"
pass

# --- PID reuse and ambiguous records ---------------------------------------------------------------

# forge ROOT GEN PID START [NODE] -- write an ownership record by hand.
forge() {
  local l="$1.lock"
  mkdir -p "$l"; chmod 700 "$l"
  printf 'planar-mutation-lock 1\ngen=%s\noperation=install\npid=%s\nstart=%s\nnode=%s\nnonce=%s\nroot=%s\ntmp=\n' \
    "$2" "$3" "$4" "${5:-$(uname -n)}" 0123456789abcdef0123456789abcdef "$1" > "$l/owner.$2"
}
sleep 300 &
SLEEPER=$!; PIDS+=("$SLEEPER")
real_start="$(lk 'planar_lock_start_token '"$SLEEPER")"
[[ "$real_start" == psu:* || "$real_start" == proc:* ]] || fail "start token has no documented form: $real_start"
R="$TMP/d/.planar"; mkdir -p "$TMP/d"
forge "$R" 7 "$SLEEPER" "$real_start"
rc=0; out="$(lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"pid $SLEEPER"* ]] || fail "a live pid with its own start time was not refused: $out"
forge "$R" 7 "$SLEEPER" "psu:Thu Jan 1 00:00:00 1970"
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN reclaimed=$PLANAR_LOCK_RECLAIMED"')"
[[ "$out" == "gen=8 reclaimed=install pid $SLEEPER" ]] || fail "a reused pid (other start time) was not reclaimed: $out"

ambiguous() { # ambiguous NAME EXPECT-PHRASE -- the lock at $R must refuse and keep its files.
  local before after rc=0 out
  before="$(ls -la "$R.lock" 2>&1; cat "$R.lock"/owner.* 2>/dev/null || true)"
  out="$(lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
  after="$(ls -la "$R.lock" 2>&1; cat "$R.lock"/owner.* 2>/dev/null || true)"
  [[ "$rc" == 1 ]] || fail "$1: an ambiguous lock was taken: $out"
  [[ "$out" == *"$2"* ]] || fail "$1: the refusal does not say '$2': $out"
  [[ "$out" != *ERR-TRAP-FIRED* ]] || fail "$1: the refusal fired the caller's ERR trap: $out"
  [[ "$before" == "$after" ]] || fail "$1: the refusal changed the lock directory"
}
R="$TMP/e/.planar"; mkdir -p "$TMP/e"; forge "$R" 3 "$SLEEPER" "$real_start" other-host.example
ambiguous other-host "recorded on host other-host.example"
# A record from an older release holds a local-zone start (`ps:`): for a live pid it is
# ambiguous and refuses, whatever its text; for a dead pid it is reclaimed as usual.
R="$TMP/lg/.planar"; mkdir -p "$TMP/lg"; forge "$R" 5 "$SLEEPER" "ps:Thu Jan 1 00:00:00 1970"
ambiguous legacy-local-start "local-time start"
forge "$R" 5 "$SLEEPER" "ps:${real_start#psu:}"
ambiguous legacy-local-start-equal-text "local-time start"
R="$TMP/lgd/.planar"; mkdir -p "$TMP/lgd"; forge "$R" 5 99999999 "ps:Thu Jan 1 00:00:00 1970"
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN"')"
[[ "$out" == "gen=6" ]] || fail "a legacy record of a dead pid was not reclaimed: $out"
R="$TMP/f/.planar"; mkdir -p "$TMP/f/.planar.lock"; chmod 700 "$TMP/f/.planar.lock"; printf 'garbage\n' > "$TMP/f/.planar.lock/owner.4"
ambiguous malformed "malformed"
R="$TMP/g/.planar"; mkdir -p "$TMP/g/elsewhere"; ln -s "$TMP/g/elsewhere" "$TMP/g/.planar.lock"
ambiguous symlinked-dir "is a symlink"
if [[ ! -r /proc/$$/stat ]]; then
  # No /proc: the start time comes from ps. Without ps a live pid cannot be told
  # apart from a reused one, which refuses.
  R="$TMP/h/.planar"; mkdir -p "$TMP/h"; forge "$R" 2 "$SLEEPER" "psu:Thu Jan 1 00:00:00 1970"
  rm -f "$BASEBIN/ps"
  ambiguous no-ps "start time cannot be read"
  ln -s "$(PATH=/bin:/usr/bin command -v ps)" "$BASEBIN/ps"
fi
pass

# --- a live owner is recognised across time zones ----------------------------------------------------
# `ps -o lstart` renders in the caller's local zone; the start token must not,
# or a checker in another zone would call a live owner a reused pid and take the
# lock (test spec 679, "the lock recognises a live owner across time zones").
# Kiritimati is UTC+14 and Adak UTC-10 (-9 in summer): the same instant never
# prints the same lstart in both, whatever the date.

TZ_OWNER=Pacific/Kiritimati
TZ_CHECKER=America/Adak
R="$TMP/tz/.planar"; mkdir -p "$R"; L="$R.lock"
TZ="$TZ_OWNER" hold "$R" install "$TMP/tz.ready"
tz_owner_pid="$HOLD_PID"
rc=0; out="$(TZ="$TZ_CHECKER" lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"install (pid $tz_owner_pid)"* ]] \
  || fail "a live owner in $TZ_OWNER was not recognised by a checker in $TZ_CHECKER ($rc): $out"
rc=0; out="$(TZ="$TZ_CHECKER" lk 'planar_lock_acquire "'"$R"'" update || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"install (pid $tz_owner_pid)"* ]] \
  || fail "a second checker in $TZ_CHECKER reclaimed a live owner ($rc): $out"
[[ -f "$L/owner.1" && ! -e "$L/owner.2" && ! -e "$L/released.1" ]] || fail "a time-zone-skewed checker disturbed the lock: $(ls "$L")"
# The token itself is the same in every zone, and the zones really do differ in
# what ps prints (otherwise this test proves nothing on this platform).
t_owner="$(TZ="$TZ_OWNER" lk 'planar_lock_start_token '"$tz_owner_pid")"
t_check="$(TZ="$TZ_CHECKER" lk 'planar_lock_start_token '"$tz_owner_pid")"
[[ "$t_owner" == "$t_check" ]] || fail "the start token depends on the time zone: '$t_owner' vs '$t_check'"
if [[ ! -r /proc/$$/stat ]]; then
  [[ "$(TZ="$TZ_OWNER" LC_ALL=C ps -o lstart= -p "$tz_owner_pid")" != "$(TZ="$TZ_CHECKER" LC_ALL=C ps -o lstart= -p "$tz_owner_pid")" ]] \
    || fail "the chosen zones print the same lstart; the test cannot tell"
fi
kill -9 "$tz_owner_pid"; wait "$tz_owner_pid" 2>/dev/null || true
pass

# --- a killed owner's record survives a host rename ---------------------------------------------------
# Test spec 679, "Edge — a crashed owner's record survives a hostname change".
# A record names its host by the machine's identity, not by `uname -n`: a
# macOS host is renamed by scutil or DHCP between a crash and the next run, and
# the dead owner must still be recovered. A record from another machine sharing
# the filesystem (another identity, whatever its name) is never reclaimed.
# The identity here is a fixture machine-id file, so every host runs the case;
# the host's real identity is exercised further down where it has one.

REAL_UNAME="$(PATH=/bin:/usr/bin command -v uname)"
# mk_uname DIR NAME -- a uname whose -n prints NAME; anything else is the real one.
mk_uname() {
  mkdir -p "$1"
  cat > "$1/uname" <<EOU
#!/bin/sh
if [ "\$1" = -n ]; then printf '%s\n' '$2'; exit 0; fi
exec $REAL_UNAME "\$@"
EOU
  chmod 755 "$1/uname"
}
mk_uname "$TMP/name-a" host-a.example
mk_uname "$TMP/name-b" host-b.example
mk_uname "$TMP/name-none" ""
MID_A="$TMP/machine-id.a"; printf '0123456789abcdef0123456789abcdef\n' > "$MID_A"
MID_B="$TMP/machine-id.b"; printf 'fedcba9876543210fedcba9876543210\n' > "$MID_B"

# rename_case ROOT MID -- kill a holder running as host-a.example, then judge it
# as host-b.example on the same machine (identity MID; empty: the host's own).
rename_case() {
  local r="$1" mid="$2" holder out rc
  mkdir -p "$(dirname "$r")"
  PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID="$mid" hold "$r" install "$r.ready"
  holder="$HOLD_PID"
  grep -Fxq 'node=host-a.example' "$r.lock/owner.1" || fail "the holder did not run under the first host name: $(cat "$r.lock/owner.1")"
  kill -9 "$holder"; wait "$holder" 2>/dev/null || true
  rc=0; out="$(PATH="$TMP/name-b:$BASEBIN" LOCK_TEST_MID="$mid" lk 'planar_lock_acquire "'"$r"'" install && echo "gen=$PLANAR_LOCK_GEN reclaimed=$PLANAR_LOCK_RECLAIMED" || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
  [[ "$rc" == 0 && "$out" == "gen=2 reclaimed=install pid $holder" ]] \
    || fail "a killed owner was not recovered after a hostname change ($rc): $out"
  grep -Fxq 'node=host-b.example' "$r.lock/owner.2" || fail "the new owner did not run under the second host name"
  # A live owner is still recognised across the rename: never reclaimed.
  PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID="$mid" hold "$r.live" install "$r.live.ready"
  holder="$HOLD_PID"
  rc=0; out="$(PATH="$TMP/name-b:$BASEBIN" LOCK_TEST_MID="$mid" lk 'planar_lock_acquire "'"$r.live"'" update || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
  [[ "$rc" == 1 && "$out" == *"another Planar install (pid $holder)"* ]] \
    || fail "a live owner was not recognised after a hostname change ($rc): $out"
  kill -9 "$holder"; wait "$holder" 2>/dev/null || true
}
rename_case "$TMP/hn1/.planar" "$MID_A"

# Another machine (another identity) under the SAME host name: never reclaimed.
R="$TMP/hn2/.planar"; mkdir -p "$TMP/hn2"
PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID="$MID_A" hold "$R" install "$TMP/hn2.ready"
kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
rc=0; out="$(PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID="$MID_B" lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"recorded on host host-a.example"*"not this host"* ]] \
  || fail "a dead owner recorded on another machine with the same host name was reclaimed ($rc): $out"
[[ -f "$R.lock/owner.1" && ! -e "$R.lock/owner.2" ]] || fail "the refusal changed the lock directory: $(ls "$R.lock")"

# A host with no machine identity falls back to its host name, so it keeps the
# old rule: a rename refuses (never an unsafe reclaim), the same name reclaims.
R="$TMP/hn3/.planar"; mkdir -p "$TMP/hn3"
PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID=/nonexistent/machine-id LOCK_TEST_NO_IOREG=1 hold "$R" install "$TMP/hn3.ready"
fallback_pid="$HOLD_PID"
grep -Eq '^host=node:host-a\.example(/pidns:[0-9]+)?$' "$R.lock/owner.1" \
  || fail "a host without a machine identity did not fall back to its host name: $(cat "$R.lock/owner.1")"
kill -9 "$fallback_pid"; wait "$fallback_pid" 2>/dev/null || true
rc=0; out="$(PATH="$TMP/name-b:$BASEBIN" LOCK_TEST_MID=/nonexistent/machine-id LOCK_TEST_NO_IOREG=1 lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"recorded on host host-a.example"* ]] || fail "a host-name-keyed record was reclaimed under another name ($rc): $out"
out="$(PATH="$TMP/name-a:$BASEBIN" LOCK_TEST_MID=/nonexistent/machine-id LOCK_TEST_NO_IOREG=1 lk 'planar_lock_acquire "'"$R"'" install && echo "reclaimed=$PLANAR_LOCK_RECLAIMED"')"
[[ "$out" == "reclaimed=install pid $fallback_pid" ]] || fail "a host-name-keyed record was not reclaimed under its own name: $out"

# No identity and no host name: the record proves nothing, so even this host
# refuses to reclaim it.
R="$TMP/hn4/.planar"; mkdir -p "$TMP/hn4"
PATH="$TMP/name-none:$BASEBIN" LOCK_TEST_MID=/nonexistent/machine-id LOCK_TEST_NO_IOREG=1 hold "$R" install "$TMP/hn4.ready"
grep -Fxq 'host=none' "$R.lock/owner.1" || fail "a host with no identity and no name did not record host=none: $(cat "$R.lock/owner.1")"
kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
rc=0; out="$(PATH="$TMP/name-none:$BASEBIN" LOCK_TEST_MID=/nonexistent/machine-id LOCK_TEST_NO_IOREG=1 lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"no host identity"* ]] || fail "a record with no host identity was reclaimed ($rc): $out"

# A host-name-keyed record from an older Planar (format 1) is local only under
# the same name: renamed, even a dead pid is refused, naming the rename.
R="$TMP/hn5/.planar"; mkdir -p "$TMP/hn5"; forge "$R" 4 99999999 "psu:Thu Jan 1 00:00:00 1970" renamed-host.invalid
ambiguous legacy-renamed "recorded on host renamed-host.invalid"
R="$TMP/hn6/.planar"; mkdir -p "$TMP/hn6"; forge "$R" 4 99999999 "psu:Thu Jan 1 00:00:00 1970"
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN"')"
[[ "$out" == "gen=5" ]] || fail "a dead owner's format-1 record under this host's name was not reclaimed: $out"

# The host's own identity, where it has one (macOS: the platform UUID; Linux: a
# valid /etc/machine-id): the same rename, with no fixture.
real_key="$(lk '_pl_host_key')"
case "$real_key" in
  machine-id:*|platform-uuid:*)
    rename_case "$TMP/hn7/.planar" ""
    grep -Fxq "host=$real_key" "$TMP/hn7/.planar.lock/owner.2" || fail "the record does not carry the host's identity $real_key: $(cat "$TMP/hn7/.planar.lock/owner.2")"
    ;;
  *) printf 'install-lock-test: note: this host has no machine identity (%s); the rename case ran with a fixture identity only\n' "$real_key" ;;
esac
pass

# --- simultaneous reclaim attempts never make two owners ------------------------------------------

CONTENDER="$TMP/contender.sh"
cat > "$CONTENDER" <<'EOS'
source "$1"
root="$2"; crit="$3"; go="$4"; out="$5"
while [ ! -e "$go" ]; do :; done
if planar_lock_acquire "$root" install; then
  if mkdir "$crit" 2>/dev/null; then
    sleep 0.05
    rmdir "$crit"
    echo won >> "$out"
  else
    echo OVERLAP >> "$out"
  fi
  planar_lock_release
else
  echo refused >> "$out"
fi
EOS
R="$TMP/race/.planar"; mkdir -p "$TMP/race"
rounds=0; wins=0
while [[ "$rounds" -lt 25 ]]; do
  rounds=$((rounds + 1))
  hold "$R" install "$TMP/race.ready.$rounds"
  kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
  rm -f "$TMP/go"; : > "$TMP/race.out"
  cpids=()
  for _c in 1 2 3 4 5 6; do
    "$BASH_BIN" "$CONTENDER" "$LIB" "$R" "$TMP/crit" "$TMP/go" "$TMP/race.out" &
    cpids+=("$!")
  done
  : > "$TMP/go"
  for p in "${cpids[@]}"; do wait "$p" || true; done
  ! grep -q OVERLAP "$TMP/race.out" || fail "round $rounds: two processes owned the lock at once: $(cat "$TMP/race.out")"
  n="$(grep -c won "$TMP/race.out" || true)"
  [[ "$n" -ge 1 ]] || fail "round $rounds: no contender reclaimed the dead owner's lock: $(cat "$TMP/race.out")"
  wins=$((wins + n))
  [[ "$(ls "$R.lock" | grep -c '^owner\.' || true)" -le 1 ]] || fail "round $rounds: housekeeping left several generations: $(ls "$R.lock")"
done
pass

# --- the update handoff -------------------------------------------------------------------------------

R="$TMP/u/.planar"; mkdir -p "$R/.planar-update/t1"
ADOPT="$TMP/adopt.sh"
cat > "$ADOPT" <<'EOS'
source "$1"
if planar_lock_adopt "$2" "$PLANAR_MUTATION_HANDOFF"; then
  echo "adopted gen=$PLANAR_LOCK_GEN op=$PLANAR_LOCK_OP tmp=$PLANAR_LOCK_TMP"
  if [ "${3:-}" = twice ]; then
    if planar_lock_adopt "$2" "$PLANAR_MUTATION_HANDOFF"; then echo "adopted twice"; else echo "second: $PLANAR_LOCK_ERROR"; fi
  fi
  planar_lock_release
else
  echo "refused: $PLANAR_LOCK_ERROR"
fi
EOS
# updater ROOT OP [ADOPT-ARG] [TOKEN-OVERRIDE] -- acquire as OP, then exec the adopter.
updater() {
  "$BASH_BIN" -c 'source "$1"; planar_lock_acquire "$2" "$3" "$2/.planar-update/t1" || { echo "$PLANAR_LOCK_ERROR"; exit 1; }
    tok="${6:-$PLANAR_LOCK_GEN:$PLANAR_LOCK_NONCE}"; echo "$tok" > "$7"
    PLANAR_MUTATION_HANDOFF="$tok" exec "$4" "$5" "$1" "$2" "$8"' \
    up "$LIB" "$1" "$2" "$BASH_BIN" "$ADOPT" "${4:-}" "$TMP/token" "${3:-}"
}
out="$(updater "$R" update twice)"
[[ "$out" == *"adopted gen="*" op=update tmp=$R/.planar-update/t1"* ]] || fail "a valid handoff was not adopted: $out"
[[ "$out" == *"second: "*"already used"* ]] || fail "a replayed handoff in the same process was accepted: $out"
tok="$(cat "$TMP/token")"
g="${tok%%:*}"
[[ -f "$R.lock/handoff.$g" && -f "$R.lock/released.$g" ]] || fail "the handoff was not recorded and released: $(ls "$R.lock")"
out="$(PLANAR_MUTATION_HANDOFF="$tok" "$BASH_BIN" "$ADOPT" "$LIB" "$R")"
[[ "$out" == "refused: "* ]] || fail "a replayed handoff from another process was accepted: $out"
out="$(updater "$R" install)"
[[ "$out" == "refused: "*"does not match"* ]] || fail "a handoff of an install record was accepted: $out"
out="$(updater "$R" update "" "1:0123456789abcdef0123456789abcdef")"
[[ "$out" == "refused: "* ]] || fail "a forged handoff was accepted: $out"
hold "$R" update "$TMP/u.ready" "$R/.planar-update/t1"
g="$(cat "$TMP/u.ready")"; nonce="$(sed -n 's/^nonce=//p' "$R.lock/owner.$g")"
out="$(PLANAR_MUTATION_HANDOFF="$g:$nonce" "$BASH_BIN" "$ADOPT" "$LIB" "$R")"
[[ "$out" == "refused: "*"does not match"* ]] || fail "another process adopted a live updater's handoff: $out"
[[ ! -e "$R.lock/handoff.$g" ]] || fail "a refused handoff consumed the handoff marker"
kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
out="$(PLANAR_MUTATION_HANDOFF="garbage" "$BASH_BIN" "$ADOPT" "$LIB" "$R")"
[[ "$out" == "refused: "*"malformed"* ]] || fail "a malformed handoff was accepted: $out"
pass

# --- an older updater's handoff ---------------------------------------------------------------------
# `planar update` runs the downloaded (newer) installer, so the record it hands
# over was written by the OLDER updater: format 1 (`node=<uname -n>`, no host
# identity) and, before the UTC pin, a `ps:` start in the updater's local zone.
# exec keeps the pid, the start time and the environment (TZ included), so the
# installer recomputes its own start in the record's form and adopts it when it
# matches byte for byte; anything else still refuses.

OLD_UPDATER="$TMP/old-updater.sh"
cat > "$OLD_UPDATER" <<'EOU'
lib="$1"; root="$2"; form="$3"; adopt="$4"; bash_bin="$5"
source "$lib"
planar_lock_acquire "$root" update "$root/.planar-update/t1" || { echo "$PLANAR_LOCK_ERROR"; exit 1; }
case "$form" in
  ps-local) s="$(LC_ALL=C ps -o lstart= -p $$)"; set -f; set -- $s; set +f; start="ps:$*" ;;
  ps-wrong) start="ps:Thu Jan 1 00:00:00 1970" ;;
  current)  start="$(planar_lock_start_token $$)" ;;
esac
printf 'planar-mutation-lock 1\ngen=%s\noperation=update\npid=%s\nstart=%s\nnode=%s\nnonce=%s\nroot=%s\ntmp=%s\n' \
  "$PLANAR_LOCK_GEN" "$$" "$start" "$(uname -n)" "$PLANAR_LOCK_NONCE" "$root" "$root/.planar-update/t1" \
  > "$PLANAR_LOCK_DIR/owner.$PLANAR_LOCK_GEN"
PLANAR_MUTATION_HANDOFF="$PLANAR_LOCK_GEN:$PLANAR_LOCK_NONCE" exec "$bash_bin" "$adopt" "$lib" "$root"
EOU
R="$TMP/ou/.planar"; mkdir -p "$R/.planar-update/t1"
out="$(TZ="$TZ_OWNER" "$BASH_BIN" "$OLD_UPDATER" "$LIB" "$R" ps-local "$ADOPT" "$BASH_BIN")"
[[ "$out" == "adopted gen="*" op=update tmp=$R/.planar-update/t1" ]] \
  || fail "an older updater's handoff (format 1, local-time start) was not adopted: $out"
out="$(TZ="$TZ_OWNER" "$BASH_BIN" "$OLD_UPDATER" "$LIB" "$R" current "$ADOPT" "$BASH_BIN")"
[[ "$out" == "adopted gen="*" op=update"* ]] || fail "an older updater's handoff (format 1, current start) was not adopted: $out"
out="$(TZ="$TZ_OWNER" "$BASH_BIN" "$OLD_UPDATER" "$LIB" "$R" ps-wrong "$ADOPT" "$BASH_BIN")"
[[ "$out" == "refused: "*"does not match"* ]] || fail "an older-format handoff with another start time was adopted: $out"
[[ -z "$(ls "$R.lock" | grep '^handoff\.' || true)" ]] || fail "a refused older-format handoff consumed the handoff marker: $(ls "$R.lock")"
# The zone really matters for the local-time form here (otherwise the first case
# would prove nothing): the owner's zone prints another lstart than UTC.
if [[ ! -r /proc/$$/stat ]]; then
  [[ "$(TZ="$TZ_OWNER" LC_ALL=C ps -o lstart= -p $$)" != "$(TZ=UTC LC_ALL=C ps -o lstart= -p $$)" ]] \
    || fail "the owner's zone prints the same lstart as UTC; the older-updater case cannot tell"
fi
pass

# --- update temporaries ------------------------------------------------------------------------------

R="$TMP/v/.planar"; mkdir -p "$R/.planar-update/ok-1" "$R/workbench" "$TMP/v/elsewhere"
ln -s "$TMP/v/elsewhere" "$R/.planar-update/link"
valid() { "$BASH_BIN" -c 'source "$1"; planar_update_tmp_valid "$2" "$3"' v "$LIB" "$R" "$1"; }
valid "$R/.planar-update/ok-1" || fail "a plain update temporary was rejected"
for bad in "$R" "$R/workbench" "$R/.planar-update" "$R/.planar-update/link" "$R/.planar-update/../workbench" \
           "$R/.planar-update/missing" "$TMP/v/elsewhere" "$R/.planar-update/ok-1/" "$R//.planar-update/ok-1"; do
  if valid "$bad"; then fail "an invalid update temporary was accepted: $bad"; fi
done
mv "$R/.planar-update" "$TMP/v/ns"; ln -s "$TMP/v/ns" "$R/.planar-update"
if valid "$R/.planar-update/ok-1"; then fail "an update temporary under a symlinked namespace was accepted"; fi
pass

# --- a lock directory writable by its group or by others is refused ---------------------------

R="$TMP/m1/.planar"; mkdir -p "$R.lock"; chmod 770 "$R.lock"
ambiguous group-writable-dir "writable by its group or by others"
R="$TMP/m2/.planar"; mkdir -p "$R.lock"; chmod 703 "$R.lock"
ambiguous other-writable-dir "writable by its group or by others"
R="$TMP/m3/.planar"; mkdir -p "$R.lock"; chmod 755 "$R.lock"
out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN"')"
[[ "$out" == "gen=1" ]] || fail "a lock directory only readable by others was refused: $out"
# The handoff validates the directory the same way.
R="$TMP/m4/.planar"; mkdir -p "$R/.planar-update/t1"
hold "$R" update "$TMP/m4.ready" "$R/.planar-update/t1"
g="$(cat "$TMP/m4.ready")"; nonce="$(sed -n 's/^nonce=//p' "$R.lock/owner.$g")"
chmod 770 "$R.lock"
out="$(PLANAR_MUTATION_HANDOFF="$g:$nonce" "$BASH_BIN" "$ADOPT" "$LIB" "$R")"
[[ "$out" == "refused: "*"writable by its group or by others"* ]] || fail "a handoff through a group-writable lock directory was adopted: $out"
kill -9 "$HOLD_PID"; wait "$HOLD_PID" 2>/dev/null || true
pass

# --- a record that cannot be linked names the reason, not contention ---------------------------

STUBBIN="$TMP/stubbin"; mkdir -p "$STUBBIN"
printf '#!/bin/sh\necho "ln: $2: Operation not permitted" >&2\nexit 1\n' > "$STUBBIN/ln"
chmod 755 "$STUBBIN/ln"
R="$TMP/n/.planar"; mkdir -p "$TMP/n"
rc=0; out="$(PATH="$STUBBIN:$BASEBIN" lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
[[ "$rc" == 1 && "$out" == *"cannot create the ownership record $R.lock/owner.1"*"Operation not permitted"* ]] \
  || fail "a failing link(2) was not reported with its reason ($rc): $out"
[[ "$out" != *contention* && "$out" != *ERR-TRAP-FIRED* ]] || fail "a failing link(2) was reported as contention: $out"
[[ -z "$(ls "$R.lock")" ]] || fail "a failed acquire left records behind: $(ls "$R.lock")"
pass

# --- an install root whose path says `exists` ------------------------------------------------------------
# The lost-race test reads ln's message, which carries the path: only a message
# that ENDS in `: File exists` is a lost race. A root whose path contains
# `exists` (or even `: File exists`) acquires normally, and a link(2) failure
# under it is still named by its reason, never retried as contention.
for R in "$TMP/exists/.planar" "$TMP/x: File exists/y/.planar"; do
  mkdir -p "$(dirname "$R")"
  out="$(lk 'planar_lock_acquire "'"$R"'" install && g1=$PLANAR_LOCK_GEN && planar_lock_release && planar_lock_acquire "'"$R"'" uninstall && echo "gen=$g1,$PLANAR_LOCK_GEN"')"
  [[ "$out" == "gen=1,2" ]] || fail "an install root under '$R' did not lock normally: $out"
  R="$R-stub"; mkdir -p "$(dirname "$R")"
  rc=0; out="$(PATH="$STUBBIN:$BASEBIN" lk 'planar_lock_acquire "'"$R"'" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; }' 2>&1)" || rc=$?
  [[ "$rc" == 1 && "$out" == *"cannot create the ownership record $R.lock/owner.1"*"Operation not permitted"* ]] \
    || fail "a failing link(2) under a root whose path contains 'exists' was not named by its reason ($rc): $out"
  [[ "$out" != *contention* ]] || fail "a failing link(2) under '$R' was retried as contention: $out"
done
pass

# --- (macOS) the mode check reads the permission bits, not an ACL ------------------------------------------
# Documented limit (mutation-lock.sh, INSTALL.md): an ACL that lets another user
# write the lock directory is not detected on macOS; the mark that `ls -ld`
# prints after the mode (`+`, or `@`) does not disturb the check. (On Linux a POSIX ACL that
# grants write shows in the group bits, the ACL mask, and is refused.)
if [[ "$("$REAL_UNAME" -s)" == Darwin ]]; then
  R="$TMP/acl/.planar"; mkdir -p "$R.lock"; chmod 700 "$R.lock"
  if /bin/chmod +a "everyone allow add_file,delete_child" "$R.lock" 2>/dev/null; then
    # ls -le lists the entry; the mark after the mode is `+`, or `@` when the
    # directory also carries extended attributes.
    [[ "$(ls -led "$R.lock")" == *"allow add_file"* && "$(ls -ld "$R.lock" | cut -c1-10)" == drwx------ ]] \
      || fail "the ACL fixture did not take: $(ls -led "$R.lock")"
    out="$(lk 'planar_lock_acquire "'"$R"'" install && echo "gen=$PLANAR_LOCK_GEN"')"
    [[ "$out" == "gen=1" ]] || fail "a lock directory carrying an ACL was judged by more than its mode bits: $out"
    /bin/chmod -N "$R.lock"
  fi
fi
pass

printf 'install lock tests: %s passed (%s reclaim rounds, %s wins)\n' "$PASSED" "$rounds" "$wins"
