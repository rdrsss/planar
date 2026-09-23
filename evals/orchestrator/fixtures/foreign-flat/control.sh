#!/usr/bin/env bash
set -euo pipefail

# The foreign-flat lifecycle fixture's controlled specialists (task 6858).
# Structurally identical in spirit to controlled-classic/control.sh (see
# its own comment for CONTROL_REPO_ROOT vs REPO_ROOT / EVAL_EVENT_LOG), but
# this fixture is deliberately NOT in Planar's own image: no Makefile, no
# src/ layout, a real Python unittest suite instead of a one-line grep
# check, and its own test invocation (./run-tests) and acceptance marker
# (STATUS.txt at the repo root). Only the "success" specialist_scenario is
# implemented -- this fixture exists to prove the classic replay driver
# generalizes across fixtures, not to re-exercise the reviewer-bounce /
# iteration-cap paths those already-covered scenarios grade against
# controlled-classic.

ROLE="${1:?controlled specialist role is required}"
CLAIM_TOKEN="${2:-eval-claim-not-supplied}"
CONTROL_REPO_ROOT="$(git rev-parse --show-toplevel)"
REPO_ROOT="${EVAL_REPO_ROOT:-$CONTROL_REPO_ROOT}"
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
    # Deterministic stand-in for a real coder fixing the seeded defect in
    # textkit.dedupe_words (see evals/fixtures/foreign-flat.src/repo/textkit.py):
    # the buggy version slices off the last kept word before joining.
    # Overwriting the whole file (rather than patching in place) keeps this
    # script simple and matches controlled-classic's own
    # `printf ... > file` coder pattern.
    cat > "$REPO_ROOT/textkit.py" <<'PYEOF'
"""textkit: a small, dependency-free text utility used by this fixture's
own test suite. Flat module layout (no src/), on purpose -- this repository
exists to exercise the Planar lifecycle evaluator against a fixture whose
conventions deliberately differ from Planar's own.
"""

from __future__ import annotations


def dedupe_words(text: str) -> str:
    """Collapse consecutive duplicate words (case-sensitive) to one.

    "a a b b b c" -> "a b c"
    """
    words = text.split()
    result: list[str] = []
    for index, word in enumerate(words):
        if index == 0 or words[index - 1] != word:
            result.append(word)
    return " ".join(result)
PYEOF
    printf 'approved\n' > "$REPO_ROOT/STATUS.txt"
    (cd "$REPO_ROOT" && ./run-tests) >/dev/null 2>&1
    log_event "coder-finished"
    cat <<REPORT
Files changed
- textkit.py: fixed dedupe_words() to stop dropping the final kept word.
- STATUS.txt: set to approved.

Validation run
- ./run-tests
- Ran 5 tests -- OK

Claim state
- claim token: $CLAIM_TOKEN
- controlled fixture stayed inside the leased task scope
- change remains as the classic-pwd working-tree boundary for blind review

Pre-flight checklist
- Scope, diff, tests, and commit boundary verified.

Residual risk
- None; this is the controlled Planar lifecycle fixture.

Reviewer focus
- Confirm STATUS.txt contains approved and ./run-tests exits 0.
REPORT
    ;;
  reviewer)
    value="$(<"$REPO_ROOT/STATUS.txt")"
    if [ "$value" = "approved" ]; then
      log_event "review-approved"
      cat <<REPORT
decision: approve
finding: none
evidence: STATUS.txt contains approved and the coder reported ./run-tests passing.
claim: $CLAIM_TOKEN
REPORT
    else
      log_event "review-request-changes"
      cat <<REPORT
decision: request-changes
finding: STATUS.txt must contain approved.
remediation: rerun the controlled coder and confirm ./run-tests passes.
claim: $CLAIM_TOKEN
REPORT
    fi
    ;;
  test-coder)
    log_event "test-coder-no-expansion"
    cat <<REPORT
decision: no-expansion-needed
reason: the foreign-flat fixture already has its own real unittest suite.
claim: $CLAIM_TOKEN
REPORT
    ;;
  *)
    echo "unknown controlled specialist role: $ROLE" >&2
    exit 2
    ;;
esac
