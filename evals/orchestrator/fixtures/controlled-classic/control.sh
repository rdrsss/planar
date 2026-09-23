#!/usr/bin/env bash
set -euo pipefail

ROLE="${1:?controlled specialist role is required}"
CLAIM_TOKEN="${2:-eval-claim-not-supplied}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
SCENARIO="$(<"$REPO_ROOT/.eval/scenario")"
EVENT_LOG="$REPO_ROOT/.eval/events.jsonl"

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
