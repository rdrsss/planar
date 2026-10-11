# shellcheck shell=bash
# Scenario selection and bounded parallel dispatch for scripts/install-*-test.sh
# (plan 1122 M6, task 7434; test spec 679, "one installer scenario runs on its own").
# Sourced by install-order-test.sh, install-stage-test.sh, install-uninstall-test.sh and
# install-bash32-test.sh; bash 3.2 safe (no wait -n, no associative arrays, no mapfile).
#
# Environment (documented in docs/testing.md, "Installer test speed"):
#   INSTALL_TEST_SCENARIO  one scenario name. Only that scenario runs and is reported, inside
#                          or outside its group (INSTALL_<TOOL>_GROUP is then not consulted).
#                          An unknown name is a usage error (exit 2), as an unknown group is.
#   INSTALL_TEST_JOBS      how many scenarios run at once when a run selects more than one
#                          (default 4; 1 runs them in this process, in file order).
#   INSTALL_TEST_SHARED    internal: set by the dispatching run for its children. The parent's
#                          scratch directory; its fixtures there are read-only to a child.
#
# The caller sets GROUP (its validated group filter, "all" for every group) and calls
# scen_init TOOL TABLE SERIAL: TOOL is the message prefix, TABLE the dispatch order as
# "name:group ...", SERIAL " name name " the scenarios that run alone after the parallel batch. Each scenario is a
# block wrapped in `if scen NAME; then ... fi` that touches only homes it makes under its own
# TMP, so a child process (a re-run of the same script with INSTALL_TEST_SCENARIO=NAME and its
# own mktemp scratch directory) is independent of every other one.

SCEN_FILTER=""
SCEN_JOBS=4
SCEN_RUNNING=""
SCEN_FAILED=""
SCEN_SKIPPED=""

# scen_group_of NAME -- the group a scenario belongs to; empty if NAME is not in the table.
scen_group_of() {
  local e
  for e in $SCEN_TABLE; do
    [[ "${e%%:*}" == "$1" ]] && { printf '%s' "${e#*:}"; return 0; }
  done
  return 0
}

# scen_init TOOL TABLE SERIAL -- register the calling script's scenarios, then read and validate
# INSTALL_TEST_SCENARIO and INSTALL_TEST_JOBS (usage errors exit 2).
scen_init() {
  SCEN_TOOL="$1"; SCEN_TABLE="$2"; SCEN_SERIAL="$3"; SCEN_SCRIPT="$0"
  SCEN_FILTER="${INSTALL_TEST_SCENARIO-}"
  if [[ -n "$SCEN_FILTER" && -z "$(scen_group_of "$SCEN_FILTER")" ]]; then
    local names="" e
    for e in $SCEN_TABLE; do names="$names ${e%%:*}"; done
    printf '%s: unknown INSTALL_TEST_SCENARIO %s (want one of:%s)\n' "$SCEN_TOOL" "$SCEN_FILTER" "$names" >&2
    exit 2
  fi
  SCEN_JOBS="${INSTALL_TEST_JOBS:-4}"
  case "$SCEN_JOBS" in
    ''|*[!0-9]*) printf '%s: INSTALL_TEST_JOBS must be a positive integer, got %s\n' "$SCEN_TOOL" "$SCEN_JOBS" >&2; exit 2 ;;
  esac
  # a leading zero is decimal here (08 is 8), never octal
  SCEN_JOBS="$((10#$SCEN_JOBS))"
  if [[ "$SCEN_JOBS" -lt 1 ]]; then
    printf '%s: INSTALL_TEST_JOBS must be a positive integer, got %s\n' "$SCEN_TOOL" "${INSTALL_TEST_JOBS:-4}" >&2; exit 2
  fi
}

# scen_skip REASON -- the running scenario cannot run on this host (a missing capability, tool or
# binary) and says so. Prints a `skip:` line and records the declaration. A run whose only outcome
# is such a declaration passes as skipped (see scen_none_ran); a scenario that runs nothing without
# declaring a skip still fails.
scen_skip() {
  SCEN_SKIPPED="${SCEN_SKIPPED:+$SCEN_SKIPPED; }$1"
  printf 'skip: %s\n' "$1"
}

# scen_none_ran -- call when the run made no check. A declared skip ends the run as passed with a
# `scenario-skipped:` line (the dispatcher reports it as skipped); otherwise returns, and the
# caller fails.
scen_none_ran() {
  [[ -z "$SCEN_SKIPPED" ]] || { printf 'scenario-skipped: %s\n' "$SCEN_SKIPPED"; exit 0; }
  return 0
}

# scen NAME -- is this scenario selected? A scenario filter wins over the group filter.
scen() {
  if [[ -n "$SCEN_FILTER" ]]; then
    [[ "$SCEN_FILTER" == "$1" ]]
  else
    [[ "$GROUP" == all || "$GROUP" == "$(scen_group_of "$1")" ]]
  fi
}

# scen_selected -- the selected scenario names in dispatch order, one per line.
scen_selected() {
  local e n
  for e in $SCEN_TABLE; do
    n="${e%%:*}"
    ! scen "$n" || printf '%s\n' "$n"
  done
}

# scen_fixture_sum DIR... -- a checksum listing of fixture trees, to prove a run left them as built.
scen_fixture_sum() {
  local d
  for d in "$@"; do
    ( cd "$d" && find . -print | LC_ALL=C sort && find . -type f -exec cksum {} + | LC_ALL=C sort )
  done
}

# _scen_start NAME LOGDIR ARGS... -- start one child run of the calling script in the background.
_scen_start() {
  local name="$1" logdir="$2"; shift 2
  local t0=$SECONDS pid
  rm -f "$logdir/$name.rc"
  # job control gives the child its own process group, so an abort can stop everything it started
  set -m
  (
    rc=0
    INSTALL_TEST_SCENARIO="$name" INSTALL_TEST_SHARED="$TMP" "$BASH" "$SCEN_SCRIPT" ${@+"$@"} </dev/null >"$logdir/$name.log" 2>&1 || rc=$?
    printf '%s\n' "$rc" > "$logdir/$name.rc.tmp" && mv "$logdir/$name.rc.tmp" "$logdir/$name.rc"
  ) &
  pid=$!
  set +m
  SCEN_RUNNING="$SCEN_RUNNING $pid:$name:$t0"
}

# _scen_alive PID -- is the child still running (a zombie that was not reaped is not)?
_scen_alive() {
  kill -0 "$1" 2>/dev/null || return 1
  [[ "$(ps -o stat= -p "$1" 2>/dev/null | cut -c1)" != Z ]]
}

# _scen_reap LOGDIR -- report every child that has finished; returns 0 when at least one did.
_scen_reap() {
  local logdir="$1" still="" e pid rest name t0 rc any=1
  for e in $SCEN_RUNNING; do
    pid="${e%%:*}"; rest="${e#*:}"; name="${rest%%:*}"; t0="${rest#*:}"
    if [[ ! -e "$logdir/$name.rc" ]]; then
      # the wrapper writes the status before it exits, so a dead wrapper with no status file was
      # killed; test liveness first and the file second, so a status written in between is seen
      if _scen_alive "$pid" || [[ -e "$logdir/$name.rc" ]]; then still="$still $e"; continue; fi
      printf '=== scenario %s: FAILED, exited without a status (%ss)\n' "$name" "$((SECONDS - t0))" >&2
      cat "$logdir/$name.log" >&2
      printf '%s: FAIL: scenario %s exited without a status\n' "$SCEN_TOOL" "$name" >&2
      SCEN_FAILED="$SCEN_FAILED $name"
      any=0
      continue
    fi
    wait "$pid" 2>/dev/null || true
    rc="$(cat "$logdir/$name.rc")"
    if [[ "$rc" == 0 ]] && grep -q '^scenario-skipped: ' "$logdir/$name.log"; then
      printf '=== scenario %s: skipped (%s) (%ss)\n' "$name" "$(sed -n 's/^scenario-skipped: //p' "$logdir/$name.log" | head -n 1)" "$((SECONDS - t0))"
      cat "$logdir/$name.log"
    elif [[ "$rc" == 0 ]]; then
      printf '=== scenario %s: ok (%ss)\n' "$name" "$((SECONDS - t0))"
      cat "$logdir/$name.log"
    else
      printf '=== scenario %s: FAILED, exit %s (%ss)\n' "$name" "$rc" "$((SECONDS - t0))" >&2
      cat "$logdir/$name.log" >&2
      printf '%s: FAIL: scenario %s exited %s\n' "$SCEN_TOOL" "$name" "$rc" >&2
      SCEN_FAILED="$SCEN_FAILED $name"
    fi
    any=0
  done
  SCEN_RUNNING="$still"
  return "$any"
}

# _scen_abort -- an interrupted dispatcher stops its children.
_scen_abort() {
  local e
  for e in $SCEN_RUNNING; do
    # the child's whole process group (see _scen_start), then the wrapper itself
    kill -s TERM -- "-${e%%:*}" 2>/dev/null || true
    kill -s TERM "${e%%:*}" 2>/dev/null || true
  done
  exit 143
}

# scen_dispatch FIXTURES ARGS... -- when this run selects more than one scenario and INSTALL_TEST_JOBS is
# above 1, run each selected scenario as a child of this script (ARGS are passed through), at
# most INSTALL_TEST_JOBS at a time, the SCEN_SERIAL ones afterwards one at a time, report every
# one under its own name, and exit; otherwise return so the caller runs the scenarios itself.
# Call it after the shared fixtures are built and before any scenario. FIXTURES is the directory
# holding them (empty for none): read-only to the children, so a change to it is a failure,
# whichever scenario made it.
scen_dispatch() {
  local fixtures="$1"; shift
  [[ -z "${INSTALL_TEST_SHARED-}" && -z "$SCEN_FILTER" && "$SCEN_JOBS" -gt 1 ]] || return 0
  local names n parallel="" serial="" count=0 logdir="$TMP/scen-logs" before="" t_start=$SECONDS
  names="$(scen_selected)"
  for n in $names; do
    count=$((count + 1))
    case "$SCEN_SERIAL" in *" $n "*) serial="$serial $n" ;; *) parallel="$parallel $n" ;; esac
  done
  [[ "$count" -gt 1 ]] || return 0
  mkdir -p "$logdir"
  [[ -z "$fixtures" ]] || before="$(scen_fixture_sum "$fixtures")"
  trap _scen_abort INT TERM HUP
  printf '%s: %s scenarios, %s at a time (%s serial: %s)\n' "$SCEN_TOOL" "$count" "$SCEN_JOBS" "$(printf '%s' "$serial" | wc -w | tr -d ' ')" "${serial:- none}"
  for n in $parallel; do
    while [[ "$(printf '%s' "$SCEN_RUNNING" | wc -w | tr -d ' ')" -ge "$SCEN_JOBS" ]]; do
      _scen_reap "$logdir" || sleep 0.3
    done
    _scen_start "$n" "$logdir" "$@"
  done
  while [[ -n "${SCEN_RUNNING# }" ]]; do _scen_reap "$logdir" || sleep 0.3; done
  for n in $serial; do
    _scen_start "$n" "$logdir" "$@"
    while [[ -n "${SCEN_RUNNING# }" ]]; do _scen_reap "$logdir" || sleep 0.3; done
  done
  if [[ -n "$fixtures" && "$(scen_fixture_sum "$fixtures")" != "$before" ]]; then
    printf '%s: FAIL: a scenario changed a shared fixture (%s); fixtures are read-only to scenarios\n' "$SCEN_TOOL" "$fixtures" >&2
    SCEN_FAILED="$SCEN_FAILED fixtures"
  fi
  if [[ -n "${SCEN_FAILED# }" ]]; then
    printf '%s: FAIL: %s of %s scenarios failed:%s\n' "$SCEN_TOOL" "$(printf '%s' "$SCEN_FAILED" | wc -w | tr -d ' ')" "$count" "$SCEN_FAILED" >&2
    exit 1
  fi
  printf '%s (group %s): %s scenarios passed, %s at a time, %ss\n' "$SCEN_TOOL" "$GROUP" "$count" "$SCEN_JOBS" "$((SECONDS - t_start))"
  exit 0
}
