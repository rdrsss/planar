#!/usr/bin/env bash
set -euo pipefail

ROLE="${1:?controlled specialist role is required}"
CLAIM_TOKEN="${2:-eval-claim-not-supplied}"
# `CONTROL_REPO_ROOT` is always the MAIN fixture checkout -- the one
# `.eval/scenario` and `.eval/events.jsonl` live under -- resolved from
# this script's own cwd (`prepare_lifecycle_fixture` always invokes it
# with `cwd=context.repo`). `REPO_ROOT` is where a role actually WRITES
# and COMMITS: it defaults to the same path (every driver before task
# 6853), but the concurrent-coders driver overrides it with
# `EVAL_REPO_ROOT` to point at one of its linked `git worktree` lanes,
# whose own `git rev-parse --show-toplevel` would resolve to the LANE,
# not the main checkout -- exactly the value this script must not use
# for the scenario file / shared event log. `EVAL_EVENT_LOG` is kept as a
# distinct override (rather than always deriving from `REPO_ROOT`) so a
# lane's boundary events still land in the one `events.jsonl`
# `merge_lifecycle_events` reads, instead of a per-lane copy nothing
# merges back in.
CONTROL_REPO_ROOT="$(git rev-parse --show-toplevel)"
REPO_ROOT="${EVAL_REPO_ROOT:-$CONTROL_REPO_ROOT}"
SCENARIO="$(<"$CONTROL_REPO_ROOT/.eval/scenario")"
EVENT_LOG="${EVAL_EVENT_LOG:-$CONTROL_REPO_ROOT/.eval/events.jsonl}"

log_event() {
  local event="$1"
  local ts
  ts="$(python3 -c 'import time; print(f"{time.time():.6f}")')"
  printf '{"event":"%s","source":"controlled-%s","ts":%s}\n' \
    "$event" "$ROLE" "$ts" >> "$EVENT_LOG"
}

case "$ROLE" in
  coder)
    current="$(<"$REPO_ROOT/src/value.txt")"
    if [ "$SCENARIO" = "reviewer-bounce" ] && [ "$current" = "baseline" ]; then
      next_value="first-pass"
    else
      next_value="approved"
    fi
    printf '%s\n' "$next_value" > "$REPO_ROOT/src/value.txt"
    make -C "$REPO_ROOT" test >/dev/null
    # Falsifiability knob (task 6836): a case that declares
    # "task-completed-before-review" among its forbidden_events needs a real
    # emitter, or the assertion can never fail. Set only by a fixture-replay
    # self-test that wants to prove the grader rejects the violation; never
    # set during a real controlled run.
    if [ "${EVAL_SEED_VIOLATION:-}" = "task-completed-before-review" ]; then
      log_event "task-completed-before-review"
    fi
    log_event "coder-finished"
    cat <<EOF
Files changed
- src/value.txt: set the controlled lifecycle value to $next_value.

Validation run
- make test
- All 1 tests passed

Claim state
- claim token: $CLAIM_TOKEN
- controlled fixture stayed inside the leased task scope
- change remains as the classic-pwd working-tree boundary for blind review

Pre-flight checklist
- Scope, diff, tests, and commit boundary verified.

Residual risk
- None; this is the controlled Planar lifecycle fixture.

Reviewer focus
- Confirm src/value.txt contains the scenario-appropriate value.
EOF
    ;;
  reviewer)
    value="$(<"$REPO_ROOT/src/value.txt")"
    if [ "$SCENARIO" = "reviewer-always-bounce" ]; then
      # Task 6856: unconditional bounce, regardless of the coder's output.
      # Nothing in the fixture ever approves under this scenario -- the
      # iteration-cap replay driver's own cap is what has to end the loop,
      # not this script running out of scripted bounces.
      log_event "review-request-changes"
      cat <<EOF
decision: request-changes
finding: controlled iteration-cap fixture always requests changes.
remediation: none; this scenario exists to exercise the iteration cap.
claim: $CLAIM_TOKEN
EOF
    elif [ "$SCENARIO" = "reviewer-bounce" ] && [ "$value" = "first-pass" ]; then
      log_event "review-request-changes"
      cat <<EOF
decision: request-changes
finding: src/value.txt:1 must contain approved rather than first-pass.
remediation: change the controlled value to approved and rerun make test.
claim: $CLAIM_TOKEN
EOF
    else
      log_event "review-approved"
      cat <<EOF
decision: approve
finding: none
evidence: src/value.txt:1 contains approved and the coder reported All 1 tests passed.
claim: $CLAIM_TOKEN
EOF
    fi
    ;;
  test-coder)
    log_event "test-coder-no-expansion"
    cat <<EOF
decision: no-expansion-needed
reason: the controlled lifecycle fixture already has its single acceptance check.
claim: $CLAIM_TOKEN
EOF
    ;;
  coder-lane)
    # Task 6853: one concurrent-coders lane. Writes ITS OWN file
    # (src/lane-<n>.txt) so three lanes committing on three independent
    # `git worktree` branches never conflict on that path at fan-in-merge
    # time, AND sets the fixture's single shared acceptance file
    # (src/value.txt) to the same literal value every lane -- the
    # concurrent-coders case still grades `post_state.file_value` like
    # every other lifecycle case, and three lanes writing the identical
    # target content to value.txt merge cleanly (git's three-way merge
    # treats a matching final value as a no-op hunk, not a conflict) while
    # also letting every lane's own `make test` (which only checks
    # value.txt) pass independently, before any fan-in merge happens.
    LANE="${3:?lane number is required}"
    printf '%s\n' "approved" > "$REPO_ROOT/src/lane-$LANE.txt"
    printf '%s\n' "approved" > "$REPO_ROOT/src/value.txt"
    make -C "$REPO_ROOT" test >/dev/null
    git -C "$REPO_ROOT" add -A
    git -C "$REPO_ROOT" commit -q -m "eval: concurrent-coders lane $LANE controlled coder"
    log_event "coder-finished"
    cat <<EOF
Files changed
- src/lane-$LANE.txt: set the concurrent-coders lane $LANE value to approved.

Validation run
- make test
- All 1 tests passed

Claim state
- claim token: $CLAIM_TOKEN
- controlled fixture stayed inside the leased task scope
- change committed on its own worktree lane branch

Pre-flight checklist
- Scope, diff, tests, and commit boundary verified.

Residual risk
- None; this is the controlled Planar lifecycle fixture.

Reviewer focus
- Confirm src/lane-$LANE.txt contains approved.
EOF
    ;;
  session-death)
    # Task 6854: emulates a coder session that dies after doing its work
    # but before calling any planar-agent terminal verb. Writes the value
    # and logs the coder-finished boundary event exactly like the `coder`
    # branch, then exits WITHOUT reviewing or completing -- the harness's
    # session-death replay driver never calls this script's `reviewer`
    # branch and never invokes planar-agent complete/fail/release/block
    # for this claim token.
    printf '%s\n' "approved" > "$REPO_ROOT/src/value.txt"
    make -C "$REPO_ROOT" test >/dev/null
    log_event "coder-finished"
    exit 0
    ;;
  *)
    echo "unknown controlled specialist role: $ROLE" >&2
    exit 2
    ;;
esac
