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
#   - update temporaries are recognised only under <root>/.planar-update/.
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
for n in od tr uname ps ln rm rmdir mkdir dirname sleep cat ls cut grep sed mv chmod; do
  f="$(command -v "$n" || true)"
  case "$f" in /*) ln -s "$f" "$BASEBIN/$n" ;; esac
done
export PATH="$BASEBIN"
[[ -z "$(command -v python3 || true)" ]] || fail "python3 is reachable on the test PATH"

# lk SCRIPT... -- run a fresh bash that sources the library, then SCRIPT.
lk() { "$BASH_BIN" -c 'set -eEuo pipefail; trap '\''echo ERR-TRAP-FIRED >&2'\'' ERR; source "$1"; shift; eval "$*"' lk "$LIB" "$@"; }

# hold ROOT OP READY [TMPDIR] -- background holder; sets HOLD_PID.
hold() {
  local root="$1" op="$2" ready="$3" t="${4:-}"
  "$BASH_BIN" -c 'source "$1"; planar_lock_acquire "$2" "$3" "$5" || { echo "$PLANAR_LOCK_ERROR" >&2; exit 1; }; echo "$PLANAR_LOCK_GEN" > "$4"; exec sleep 300' \
    hold "$LIB" "$root" "$op" "$ready" "$t" &
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
grep -Fxq 'operation=install' "$L/owner.1" && grep -Eq '^start=(psu|proc):' "$L/owner.1" && grep -Eq '^nonce=[0-9a-f]{32}$' "$L/owner.1" \
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

printf 'install lock tests: %s passed (%s reclaim rounds, %s wins)\n' "$PASSED" "$rounds" "$wins"
