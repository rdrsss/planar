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
    if [ "$SCENARIO" = "reviewer-bounce" ] && [ "$value" = "first-pass" ]; then
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
  *)
    echo "unknown controlled specialist role: $ROLE" >&2
    exit 2
    ;;
esac
