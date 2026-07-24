#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CASE="$ROOT/evals/pl-orchestrator/phase3-model-routing.json"
VENDOR=""
SURFACE="skill"
LIVE=0
KEEP=0

usage() {
  cat <<'EOF'
Usage: scripts/eval-pl-orchestrator.sh [--contract-only] [--live --vendor codex|claude --surface skill|agent] [--keep]

Without --live, validates the deterministic authored contract only. Live mode
creates an isolated Planar DB and repository, invokes the selected host, stops
at the Phase 3 preview gate, and grades transcript plus post-state.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --contract-only) LIVE=0; shift ;;
    --live) LIVE=1; shift ;;
    --vendor) VENDOR="$2"; shift 2 ;;
    --surface) SURFACE="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }
require() { command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"; }

require jq
jq -e '.expected.tier == "medium" and .expected.requires_explicit_gate == true' "$CASE" >/dev/null
rg -q 'every task defaults to `medium`' "$ROOT/skills/src/pl-orchestrator.md" || fail "medium-default contract missing"
# The "Active host vendor:" projection-note check that used to live here
# asserted on src/engine/skillrender.zig's special-cased pl-orchestrator
# injection. skillrender.zig was deleted at plan 918 (rendering moved to
# scriptorium); the note has no static-source equivalent left to assert on
# in this repo, so the check is retired rather than repointed.
if rg -q 'planar-execute run workflows/' "$ROOT/skills/src/pl-orchestrator.md" "$ROOT/agents/orchestrator.md"; then
  fail "checkout-relative workflow invocation remains"
fi
pass "authored orchestrator contract"

[ "$LIVE" -eq 1 ] || exit 0
case "$VENDOR" in codex|claude) ;; *) fail "--vendor must be codex or claude" ;; esac
case "$SURFACE" in skill|agent) ;; *) fail "--surface must be skill or agent" ;; esac
require planar
require planar-watch
require "$VENDOR"
require git

TMP="$(mktemp -d "${TMPDIR:-/tmp}/planar-orchestrator-eval.XXXXXX")"
cleanup() { [ "$KEEP" -eq 1 ] || rm -rf "$TMP"; }
trap cleanup EXIT
REPO="$TMP/repo"
mkdir -p "$REPO"
git -C "$REPO" init -q
export PLANAR_DB="$TMP/planar.db"
export PLANAR_CONFIG_PATH="$TMP/config.toml"
if [ "$VENDOR" = codex ]; then opposite=claude; else opposite=codex; fi
printf '[defaults]\nvendor = "%s"\n' "$opposite" > "$PLANAR_CONFIG_PATH"

(cd "$REPO" && planar init --name orchestrator-eval --json >/dev/null)
PLAN_JSON="$(cd "$REPO" && planar plan create "Orchestrator model-routing eval" --scope global --status active --json)"
PLAN_ID="$(printf '%s' "$PLAN_JSON" | jq -r '.id')"
while IFS=$'\t' read -r slug title; do
  (cd "$REPO" && planar task add "$title" --scope global --plan "$PLAN_ID" --slug "$slug" --editor=false --json >/dev/null)
done < <(jq -r '.tasks[] | [.slug, .title] | @tsv' "$CASE")

(cd "$REPO" && planar plan next "$PLAN_ID" --json) > "$TMP/before.json"
jq '{plan_id,available:[.available[]|{id,slug,status}],claimed,stale,blocked,summary}' "$TMP/before.json" > "$TMP/before.dispatch-state.json"
PROMPT="Use the installed pl-orchestrator workflow for active plan $PLAN_ID. Stop at its required Phase 3 strategy, isolation, tier, and routed-model preview gate and wait for operator confirmation. Do not claim or dispatch a task."
RAW="$TMP/transcript.jsonl"

set +e
if [ "$VENDOR" = codex ]; then
  if [ "$SURFACE" = agent ]; then
    (cd "$REPO" && codex exec --json --sandbox workspace-write "Do not read or invoke any skill. Immediately spawn the installed orchestrator subagent and delegate this entire task to it. Do not execute the orchestration workflow in the parent agent. $PROMPT") >"$RAW" 2>&1
  else
    (cd "$REPO" && codex exec --json --sandbox workspace-write "\$pl-orchestrator $PLAN_ID") >"$RAW" 2>&1
  fi
else
  if [ "$SURFACE" = agent ]; then
    (cd "$REPO" && claude --agent orchestrator -p --output-format stream-json --verbose --permission-mode dontAsk "$PROMPT") >"$RAW" 2>&1
  else
    (cd "$REPO" && claude -p --output-format stream-json --verbose --permission-mode dontAsk "/pl-orchestrator $PLAN_ID") >"$RAW" 2>&1
  fi
fi
RC=$?
set -e
if rg -q 'rate_limit|session limit|api_error_status.?[:=].?429' "$RAW"; then
  echo "BLOCKED: $VENDOR/$SURFACE host rate limit (artifact: $RAW)" >&2
  exit 75
fi
[ "$RC" -eq 0 ] || fail "$VENDOR/$SURFACE invocation exited $RC (artifact: $RAW)"

jq -r '.. | strings' "$RAW" 2>/dev/null > "$TMP/transcript.txt" || cp "$RAW" "$TMP/transcript.txt"
if [ "$VENDOR" = codex ]; then
  sed -n '/^{/p' "$RAW" | jq -s '[.[] | select(.type == "item.completed" and .item.type == "agent_message") | .item.text][-1] // ""' > "$TMP/final.txt"
else
  sed -n '/^{/p' "$RAW" | jq -s '[.[] | select(.type == "result") | .result][-1] // ""' > "$TMP/final.txt"
fi
(cd "$REPO" && planar plan next "$PLAN_ID" --json) > "$TMP/after.json"
jq '{plan_id,available:[.available[]|{id,slug,status}],claimed,stale,blocked,summary}' "$TMP/after.json" > "$TMP/after.dispatch-state.json"
cmp -s "$TMP/before.dispatch-state.json" "$TMP/after.dispatch-state.json" || fail "dispatch state changed before approval"
ACTIVE="$(cd "$REPO" && planar-watch ps --plan "$PLAN_ID" --json | jq '.active | length')"
[ "$ACTIVE" -eq 0 ] || fail "claim created before approval"

for slug in hoist-config-resolution per-module-config-types config-mapping-composition-root; do
  rg -q "$slug" "$TMP/final.txt" || fail "preview omitted $slug"
done
rg -qi 'medium' "$TMP/final.txt" || fail "preview did not propose medium tier"
rg -qi 'architectural' "$TMP/final.txt" && fail "preview inflated an already-decided task to architectural work type"
rg -qi 'confirm|approval|accept|wait' "$TMP/final.txt" || fail "preview did not stop at an explicit gate"
rg -q 'UnknownSubcommand|No such file or directory' "$TMP/transcript.txt" && fail "host trace contains invalid command or missing reference"
if [ "$VENDOR" = codex ] && [ "$SURFACE" = agent ] && rg -q 'pl-orchestrator/SKILL.md' "$RAW"; then
  fail "Codex direct-agent adapter leaked the skill into the parent context"
fi
if [ "$VENDOR" = codex ]; then
  rg -q 'gpt-5\.6-terra' "$TMP/final.txt" || fail "Codex preview did not bind the installed medium coder"
  rg -q 'claude-(haiku|sonnet|opus|fable)' "$TMP/final.txt" && fail "Codex preview proposed a Claude model"
else
  rg -q 'claude-sonnet-5' "$TMP/final.txt" || fail "Claude preview did not bind the medium coder"
  rg -q 'gpt-5\.' "$TMP/final.txt" && fail "Claude preview proposed a Codex model"
fi
pass "$VENDOR/$SURFACE gate-only behavioral eval"
echo "artifacts: $TMP"
