#!/usr/bin/env bash
# The scenario filter and parallel dispatch of the installer tests (plan 1122 M6, task 7434;
# test spec 679, "one installer scenario runs on its own"). scripts/fixtures/scenario-runner.sh
# is driven here through a small stand-in script with five scenarios, and the four real
# installer test scripts are checked for their cheap usage paths, so nothing is built or
# installed and no real installer scenario runs except two that take a second.
#
#   - INSTALL_TEST_SCENARIO runs one scenario inside or outside its group; an unknown name, and
#     an INSTALL_TEST_JOBS that is not a positive integer, exit 2 in all four real scripts.
#   - A parallel run reports every scenario under its own name, gives each its own scratch
#     directory, never runs more than INSTALL_TEST_JOBS at once, and runs the serial scenario
#     alone.
#   - A failing scenario in a parallel run prints its own FAIL line naming it, the others still
#     run, and the run exits non-zero; so does a scenario that changes the shared fixture.
#
# Runs under /bin/bash (3.2 on macOS): the runner library uses no bash 4 feature.
# shellcheck disable=SC2016  # literal $ in the stand-in script
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() { printf 'install-scenarios-test: FAIL: %s\n' "$*" >&2; exit 1; }
PASSED=0
pass() { PASSED=$((PASSED + 1)); printf 'ok %s %s\n' "$PASSED" "$1"; }

# --- the stand-in script ------------------------------------------------------------------
# Five scenarios in three groups; e is serial and first in the table, so a run that did not hold it
# back would start it beside the others. a-d record how many scenarios are live while
# they run (MINI_LIVE is a directory of one file per live scenario).
cat > "$TMP/mini.sh" <<'MINI'
#!/usr/bin/env bash
set -euo pipefail
ROOT="$1"; shift
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
GROUP="${MINI_GROUP:-all}"
SCEN_TABLE="e:g3 a:g1 b:g1 c:g2 d:g2 ${MINI_EXTRA-}"
SCEN_SERIAL=" e "
source "$ROOT/scripts/fixtures/scenario-runner.sh"
scen_init mini "$SCEN_TABLE" "$SCEN_SERIAL"
if [[ -n "${INSTALL_TEST_SHARED-}" ]]; then FIX="$INSTALL_TEST_SHARED/fix"; else FIX="$TMP/fix"; mkdir -p "$FIX"; echo fixture > "$FIX/file"; fi
scen_dispatch "$TMP/fix" "$ROOT"
live() { # live NAME -- stay live for a second; print the live count at the start and at the end
  local n m; mkdir -p "$MINI_LIVE"
  : > "$MINI_LIVE/$1"; n="$(ls "$MINI_LIVE" | wc -l | tr -d ' ')"
  sleep 1; m="$(ls "$MINI_LIVE" | wc -l | tr -d ' ')"; rm -f "$MINI_LIVE/$1"
  printf 'ran %s live=%s end=%s scratch=%s\n' "$1" "$n" "$m" "$TMP"
  CHECKS=$((CHECKS + 1))
}
CHECKS=0
if scen a; then live a; fi
if scen b; then live b; [[ -z "${MINI_BREAK-}" ]] || { echo "b: an assertion failed" >&2; exit 1; }; fi
if scen c; then live c; fi
if scen d; then live d; [[ -z "${MINI_MUTATE-}" ]] || echo changed >> "$FIX/file"; fi
if scen e; then live e; fi
# (has N: only a scenario named in MINI_EXTRA exists; scen alone selects every name under group all)
has() { [[ " ${MINI_EXTRA-} " == *" $1:"* ]]; }
# f declares a skip and nothing else; g neither asserts nor declares; h's wrapper is killed
# from inside; s starts a long sleeper and records its pid (the abort test kills the run).
if has f && scen f; then scen_skip "no widget on this host"; fi
if has g && scen g; then :; fi
if has h && scen h; then kill -KILL "$PPID"; sleep 0.2; echo "h: ran on after its wrapper was killed"; CHECKS=$((CHECKS + 1)); fi
if has s && scen s; then sleep 300 & echo $! > "$MINI_LIVE.sleeper"; CHECKS=$((CHECKS + 1)); wait; fi
[[ "$CHECKS" -gt 0 ]] || { scen_none_ran; echo "mini: FAIL: group $GROUP ran no check" >&2; exit 1; }
MINI

# mini [ENV=V...] -- run the stand-in; RC, $TMP/out (stdout), $TMP/err (stderr).
mini() {
  RC=0
  env MINI_LIVE="$TMP/live" "$@" /bin/bash "$TMP/mini.sh" "$ROOT" >"$TMP/out" 2>"$TMP/err" || RC=$?
}
live_max() { sed -n 's/^ran [a-e] live=\([0-9]*\) .*/\1/p' "$TMP/out" | sort -n | tail -1; }

# --- one scenario on its own ----------------------------------------------------------------
mini INSTALL_TEST_JOBS=1
[[ "$RC" == 0 && "$(grep -c '^ran ' "$TMP/out")" == 5 ]] || fail "the serial run did not run all five scenarios ($RC): $(cat "$TMP/out" "$TMP/err")"
mini MINI_GROUP=g1 INSTALL_TEST_JOBS=1
[[ "$RC" == 0 && "$(grep '^ran ' "$TMP/out" | cut -d' ' -f2 | tr -d '\n')" == ab ]] || fail "a group filter did not select exactly a and b ($RC): $(cat "$TMP/out" "$TMP/err")"
mini INSTALL_TEST_SCENARIO=c MINI_GROUP=g1
[[ "$RC" == 0 && "$(grep '^ran ' "$TMP/out" | cut -d' ' -f2 | tr -d '\n')" == c ]] || fail "a scenario outside the group filter did not run alone ($RC): $(cat "$TMP/out" "$TMP/err")"
mini INSTALL_TEST_SCENARIO=a MINI_GROUP=g1
[[ "$RC" == 0 && "$(grep '^ran ' "$TMP/out" | cut -d' ' -f2 | tr -d '\n')" == a ]] || fail "a scenario inside the group filter did not run alone ($RC): $(cat "$TMP/out" "$TMP/err")"
mini INSTALL_TEST_SCENARIO=nope
[[ "$RC" == 2 ]] && grep -Fq 'unknown INSTALL_TEST_SCENARIO nope' "$TMP/err" || fail "an unknown scenario did not exit 2 naming it ($RC): $(cat "$TMP/err")"
for jobs in 0 many -1; do
  mini INSTALL_TEST_JOBS="$jobs"
  [[ "$RC" == 2 ]] || fail "INSTALL_TEST_JOBS=$jobs did not exit 2 ($RC): $(cat "$TMP/err")"
done
pass "a scenario filter runs one scenario inside or outside its group; unknown names and bad job counts exit 2"

# --- parallel dispatch ----------------------------------------------------------------------
mini INSTALL_TEST_JOBS=2
[[ "$RC" == 0 ]] || fail "the parallel run failed ($RC): $(cat "$TMP/out" "$TMP/err")"
for s in a b c d e; do grep -Fq "=== scenario $s: ok" "$TMP/out" || fail "scenario $s was not reported under its name: $(cat "$TMP/out")"; done
[[ "$(live_max)" == 2 ]] || fail "INSTALL_TEST_JOBS=2 did not run exactly two at a time (max live $(live_max)): $(cat "$TMP/out")"
[[ "$(sed -n 's/^ran e live=\([0-9]*\) end=\([0-9]*\) .*/\1/p' "$TMP/out")$(sed -n 's/^ran e live=[0-9]* end=\([0-9]*\) .*/\1/p' "$TMP/out")" == 11 ]] || fail "the serial scenario did not run alone: $(cat "$TMP/out")"
[[ "$(sed -n 's/^ran [a-e] live=[0-9]* end=[0-9]* scratch=//p' "$TMP/out" | sort -u | wc -l | tr -d ' ')" == 5 ]] || fail "the scenarios did not each get their own scratch directory: $(cat "$TMP/out")"
mini INSTALL_TEST_JOBS=4
[[ "$RC" == 0 && "$(live_max)" -gt 2 && "$(live_max)" -le 4 ]] || fail "INSTALL_TEST_JOBS=4 ran $(live_max) at a time ($RC)"
pass "a parallel run reports every scenario by name, bounds the job count, gives each its own scratch and runs the serial one alone"

# INSTALL_TEST_JOBS=08 is eight, not an invalid octal number; 00 is still not a positive count
mini INSTALL_TEST_JOBS=08
[[ "$RC" == 0 ]] && grep -Fq 'mini: 5 scenarios, 8 at a time' "$TMP/out" || fail "INSTALL_TEST_JOBS=08 was not read as 8 ($RC): $(cat "$TMP/out" "$TMP/err")"
mini INSTALL_TEST_JOBS=09
[[ "$RC" == 0 ]] && grep -Fq '9 at a time' "$TMP/out" || fail "INSTALL_TEST_JOBS=09 was not read as 9 ($RC): $(cat "$TMP/err")"
mini INSTALL_TEST_JOBS=00
[[ "$RC" == 2 ]] || fail "INSTALL_TEST_JOBS=00 did not exit 2 ($RC)"
pass "INSTALL_TEST_JOBS with a leading zero is decimal; zero in any spelling is refused"

# --- declared skips and silent scenarios -----------------------------------------------------
mini INSTALL_TEST_JOBS=4 MINI_EXTRA="f:g4"
[[ "$RC" == 0 ]] || fail "a run with a declared skip failed ($RC): $(cat "$TMP/out" "$TMP/err")"
grep -Fq '=== scenario f: skipped (no widget on this host)' "$TMP/out" || fail "the declared skip was not reported as skipped: $(cat "$TMP/out")"
! grep -Fq 'scenario f: ok' "$TMP/out" || fail "a skipped scenario was reported ok"
grep -Fq '=== scenario a: ok' "$TMP/out" || fail "the other scenarios did not run beside the skip"
mini INSTALL_TEST_SCENARIO=f MINI_EXTRA="f:g4"
[[ "$RC" == 0 ]] && grep -Fq 'scenario-skipped: no widget on this host' "$TMP/out" || fail "a lone skipped scenario did not pass as skipped ($RC): $(cat "$TMP/out" "$TMP/err")"
mini INSTALL_TEST_JOBS=4 MINI_EXTRA="g:g5"
[[ "$RC" == 1 ]] && grep -Fq 'mini: FAIL: scenario g exited 1' "$TMP/err" && grep -Fq 'ran no check' "$TMP/err" \
  || fail "a scenario that neither asserted nor declared a skip did not fail ($RC): $(cat "$TMP/err")"
mini INSTALL_TEST_SCENARIO=g MINI_EXTRA="g:g5"
[[ "$RC" == 1 ]] || fail "a lone silent scenario did not fail ($RC)"
pass "a declared skip passes and is reported as skipped; a scenario that asserts nothing and declares nothing fails"

# --- a child that dies without a status -----------------------------------------------------
# h kills its own wrapper, so no status file is written: the run must name it and end, not wait.
T0=$SECONDS
RC=0
perl -e 'alarm 90; exec @ARGV' env MINI_LIVE="$TMP/live" INSTALL_TEST_JOBS=4 MINI_EXTRA="h:g6" /bin/bash "$TMP/mini.sh" "$ROOT" >"$TMP/out" 2>"$TMP/err" || RC=$?
[[ "$RC" == 1 ]] || fail "a child killed without a status did not fail the run, or the run hung ($RC): $(cat "$TMP/err")"
grep -Fq 'mini: FAIL: scenario h exited without a status' "$TMP/err" || fail "the child killed without a status was not named: $(cat "$TMP/err")"
[[ $((SECONDS - T0)) -lt 30 ]] || fail "the run took $((SECONDS - T0))s to notice the dead child"
pass "a child that exits without a status fails by name within a bounded time"

# --- an interrupted dispatcher stops its children ---------------------------------------------
rm -f "$TMP/live.sleeper"
env MINI_LIVE="$TMP/live" INSTALL_TEST_JOBS=4 MINI_EXTRA="s:g7" /bin/bash "$TMP/mini.sh" "$ROOT" >"$TMP/out" 2>"$TMP/err" &
DISP=$!
for _ in $(seq 1 100); do [[ -s "$TMP/live.sleeper" ]] && break; sleep 0.1; done
[[ -s "$TMP/live.sleeper" ]] || { kill -KILL "$DISP" 2>/dev/null || true; fail "the long scenario never started"; }
SLEEPER="$(cat "$TMP/live.sleeper")"
kill -TERM "$DISP"
RC=0; wait "$DISP" || RC=$?
[[ "$RC" == 143 ]] || fail "an interrupted dispatcher exited $RC, not 143"
for _ in $(seq 1 50); do kill -0 "$SLEEPER" 2>/dev/null || break; sleep 0.1; done
! kill -0 "$SLEEPER" 2>/dev/null || { kill -KILL "$SLEEPER" 2>/dev/null || true; fail "an interrupted dispatcher left a scenario's process running"; }
pass "an interrupted dispatcher stops its children and what they started, without pkill"

# --- failure attribution ---------------------------------------------------------------------
mini INSTALL_TEST_JOBS=4 MINI_BREAK=1
[[ "$RC" == 1 ]] || fail "a failing scenario in a parallel run exited $RC, not 1"
grep -Fq 'mini: FAIL: scenario b exited 1' "$TMP/err" && grep -Fq 'b: an assertion failed' "$TMP/err" || fail "the failure is not attributed to scenario b: $(cat "$TMP/err")"
grep -Fq '=== scenario a: ok' "$TMP/out" && grep -Fq '=== scenario e: ok' "$TMP/out" || fail "the other scenarios did not run after the failure: $(cat "$TMP/out")"
! grep -Fq 'scenario a: FAILED' "$TMP/err" || fail "a passing scenario was reported as failed"
mini INSTALL_TEST_JOBS=4 MINI_MUTATE=1
[[ "$RC" == 1 ]] && grep -Fq 'changed a shared fixture' "$TMP/err" || fail "a scenario that changed the shared fixture did not fail the run ($RC): $(cat "$TMP/err")"
pass "a failing scenario is named and fails the run; a changed shared fixture fails it too"

# --- the real scripts: usage paths ---------------------------------------------------------
# The order script wants five executables; the usage errors come before any use of them.
FAKEBIN="$TMP/fakebin"; mkdir -p "$FAKEBIN"
for b in planar planar-agent planar-watch planar-execute planar-ext; do ln -s /usr/bin/true "$FAKEBIN/$b"; done
real() { # real SCRIPT [ENV=V...] -- run a real installer test script; RC, $TMP/out, $TMP/err
  local script="$1"; shift
  local args=()
  [[ "$script" != install-order-test.sh ]] || args=("$FAKEBIN/planar" "$FAKEBIN/planar-agent" "$FAKEBIN/planar-watch" "$FAKEBIN/planar-execute" "$FAKEBIN/planar-ext")
  RC=0
  env "$@" /bin/bash "$ROOT/scripts/$script" ${args[@]+"${args[@]}"} >"$TMP/out" 2>"$TMP/err" || RC=$?
}
for script in install-order-test.sh install-uninstall-test.sh install-stage-test.sh install-bash32-test.sh; do
  real "$script" INSTALL_TEST_SCENARIO=nope
  [[ "$RC" == 2 ]] && grep -Fq 'unknown INSTALL_TEST_SCENARIO nope' "$TMP/err" || fail "$script: an unknown scenario did not exit 2 naming it ($RC): $(cat "$TMP/err")"
  real "$script" INSTALL_TEST_JOBS=0
  [[ "$RC" == 2 ]] || fail "$script: INSTALL_TEST_JOBS=0 did not exit 2 ($RC): $(cat "$TMP/err")"
done
pass "all four installer test scripts exit 2 for an unknown scenario and for a bad job count"

# One real scenario of two scripts runs alone, inside and outside its group filter.
real install-bash32-test.sh INSTALL_TEST_SCENARIO=static INSTALL_BASH32_GROUP=static
[[ "$RC" == 0 ]] && grep -Fq '(static group)' "$TMP/out" || fail "bash32 static inside its group failed ($RC): $(cat "$TMP/out" "$TMP/err")"
real install-bash32-test.sh INSTALL_TEST_SCENARIO=static INSTALL_BASH32_GROUP=home
[[ "$RC" == 0 ]] && grep -Fq '(static group)' "$TMP/out" || fail "bash32 static outside its group failed ($RC): $(cat "$TMP/out" "$TMP/err")"
for g in staging modes; do
  real install-stage-test.sh INSTALL_TEST_SCENARIO=lists INSTALL_STAGE_GROUP="$g"
  [[ "$RC" == 0 && "$(grep -c '^stage: scenario ' "$TMP/out")" == 1 ]] && grep -Fq "(group $g, scenario lists): 1 scenarios run" "$TMP/out" \
    || fail "stage scenario lists with group $g did not run alone ($RC): $(cat "$TMP/out" "$TMP/err")"
done
pass "one scenario of the bash32 and stage scripts runs alone, inside and outside its group filter"

printf 'install-scenarios-test: %s checks passed\n' "$PASSED"
