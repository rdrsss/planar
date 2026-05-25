#!/usr/bin/env bash
# session-stats.sh — extract empirical metrics from a Claude Code session JSONL.
#
# Reads the per-session transcript at
#   ~/.claude/projects/<slug>/<session-id>.jsonl
# plus the per-subagent transcripts under
#   ~/.claude/projects/<slug>/<session-id>/subagents/agent-*.jsonl
# and produces a structured summary of dispatch shape, tool usage, model
# tier, and durations.
#
# Bootstrap prototype for the diagnosis/analysis tooling described in
# Planar artifact 182 (Agent collaboration retrospective log).
#
# USAGE
#   scripts/session-stats.sh                   # auto-detect most recent session for cwd
#   scripts/session-stats.sh <path-to-jsonl>   # explicit jsonl
#   scripts/session-stats.sh --slug <slug>     # explicit project slug
#   scripts/session-stats.sh --json            # raw JSON (default: markdown summary)
#   scripts/session-stats.sh --subagents       # include per-subagent breakdown
#
# REQUIRES: bash, jq.

set -euo pipefail

FORMAT="markdown"
SHOW_SUBAGENTS=0
JSONL=""
SLUG=""

usage() { sed -n '2,16p' "$0" | sed 's/^# \?//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        --json) FORMAT="json"; shift ;;
        --markdown) FORMAT="markdown"; shift ;;
        --subagents) SHOW_SUBAGENTS=1; shift ;;
        --slug) SLUG="${2:?--slug requires a value}"; shift 2 ;;
        -h|--help) usage ;;
        *) JSONL="$1"; shift ;;
    esac
done

if [ -z "$JSONL" ]; then
    if [ -z "$SLUG" ]; then
        SLUG="$(pwd | sed 's:/:-:g')"
    fi
    PROJ_DIR="$HOME/.claude/projects/$SLUG"
    [ -d "$PROJ_DIR" ] || { echo "no Claude project dir at $PROJ_DIR" >&2; exit 1; }
    # Pick most recent root-level *.jsonl (excludes subagents/*).
    JSONL="$(find "$PROJ_DIR" -maxdepth 1 -name '*.jsonl' -print0 \
        | xargs -0 ls -t 2>/dev/null | head -1)"
    [ -n "$JSONL" ] || { echo "no .jsonl files under $PROJ_DIR" >&2; exit 1; }
fi
[ -f "$JSONL" ] || { echo "jsonl not found: $JSONL" >&2; exit 1; }

SESSION_ID="$(basename "$JSONL" .jsonl)"
PROJ_DIR="$(dirname "$JSONL")"
SUB_DIR="$PROJ_DIR/$SESSION_ID/subagents"

# Top-level session stats. Note .message.content can be either string (user
# text) or array (tool_use blocks); guard with `if type=="array"`.
SESSION_JSON="$(jq -s '
    def blocks: .message.content // [];
    def array_blocks: blocks | if type=="array" then . else [] end;

    {
        session_id: $session_id,
        jsonl: $jsonl_path,
        line_count: length,
        first_ts: ([.[] | .timestamp // empty] | min),
        last_ts:  ([.[] | .timestamp // empty] | max),
        message_counts:
            ([.[] | .type] | group_by(.) | map({key: .[0], value: length}) | from_entries),
        per_model_usage:
            ([.[] | select(.type=="assistant") | {
                model: .message.model,
                in: (.message.usage.input_tokens // 0),
                out: (.message.usage.output_tokens // 0),
                cache_read: (.message.usage.cache_read_input_tokens // 0),
                cache_create: (.message.usage.cache_creation_input_tokens // 0),
            }]
            | group_by(.model)
            | map({
                model: .[0].model,
                turns: length,
                input: (map(.in) | add),
                output: (map(.out) | add),
                cache_read: (map(.cache_read) | add),
                cache_create: (map(.cache_create) | add),
            })),
        tool_calls_by_name:
            ([.[] | select(.type=="assistant") | array_blocks[] | select(.type=="tool_use") | .name]
             | group_by(.) | map({key: .[0], value: length}) | from_entries),
        agents_dispatched:
            [.[] | select(.type=="assistant") | array_blocks[]
                | select(.type=="tool_use" and .name=="Agent")
                | {
                    description: .input.description,
                    subagent_type: (.input.subagent_type // "general-purpose"),
                    model: (.input.model // "inherit"),
                    prompt_chars: (.input.prompt | length),
                }],
        agents_by_model:
            ([.[] | select(.type=="assistant") | array_blocks[]
                | select(.type=="tool_use" and .name=="Agent")
                | .input.model // "inherit"]
             | group_by(.) | map({key: .[0], value: length}) | from_entries),
        planar_verbs:
            ([.[] | select(.type=="assistant") | array_blocks[]
                | select(.type=="tool_use" and .name=="Bash")
                | (.input.command // "")
                | scan("planar [a-z][a-z_-]*(?:\\s+[a-z][a-z_-]*)?") // empty]
             | group_by(.) | map({key: .[0], value: length}) | from_entries),
        git_actions:
            ([.[] | select(.type=="assistant") | array_blocks[]
                | select(.type=="tool_use" and .name=="Bash")
                | (.input.command // "")
                | (if test("git commit") then "commit"
                   elif test("git push") then "push"
                   elif test("gh pr create") then "pr_create"
                   elif test("gh pr merge") then "pr_merge"
                   elif test("git worktree add") then "worktree_add"
                   else empty end)]
             | group_by(.) | map({key: .[0], value: length}) | from_entries),
        skills_invoked:
            ([.[] | select(.type=="assistant") | array_blocks[]
                | select(.type=="tool_use" and .name=="Skill")
                | .input.skill]
             | group_by(.) | map({key: .[0], value: length}) | from_entries),
    }
' --arg jsonl_path "$JSONL" --arg session_id "$SESSION_ID" "$JSONL")"

# Augment with subagent breakdown when requested or available.
if [ -d "$SUB_DIR" ]; then
    SUB_COUNT="$(find "$SUB_DIR" -name 'agent-*.jsonl' | wc -l | tr -d ' ')"
    SUB_JSON="["
    SEP=""
    while IFS= read -r sub_jsonl; do
        sub_id="$(basename "$sub_jsonl" .jsonl | sed 's/^agent-//')"
        # Per-subagent: first/last ts, tool count, total tokens by model
        item="$(jq -s --arg id "$sub_id" '
            def blocks: .message.content // [];
            def array_blocks: blocks | if type=="array" then . else [] end;
            {
                agent_id: $id,
                first_ts: ([.[] | .timestamp // empty] | min),
                last_ts:  ([.[] | .timestamp // empty] | max),
                turn_count: ([.[] | select(.type=="assistant")] | length),
                tool_count: ([.[] | select(.type=="assistant") | array_blocks[] | select(.type=="tool_use")] | length),
                input_tokens:  ([.[] | select(.type=="assistant") | .message.usage.input_tokens  // 0] | add),
                output_tokens: ([.[] | select(.type=="assistant") | .message.usage.output_tokens // 0] | add),
                model: ([.[] | select(.type=="assistant") | .message.model] | first),
            }
        ' "$sub_jsonl")"
        SUB_JSON="${SUB_JSON}${SEP}${item}"
        SEP=","
    done < <(find "$SUB_DIR" -name 'agent-*.jsonl' | sort)
    SUB_JSON="${SUB_JSON}]"

    SESSION_JSON="$(jq --argjson subs "$SUB_JSON" --argjson cnt "$SUB_COUNT" \
        '. + {subagent_count: $cnt, subagents: $subs}' <<<"$SESSION_JSON")"
fi

if [ "$FORMAT" = "json" ]; then
    echo "$SESSION_JSON" | jq .
    exit 0
fi

# Markdown summary.
echo "$SESSION_JSON" | jq -r --argjson show_subs "$SHOW_SUBAGENTS" '
    def iso_to_epoch: sub("\\.[0-9]+Z$"; "Z") | fromdateiso8601;
    def duration_h:
        if (.first_ts and .last_ts) then
            ((.last_ts | iso_to_epoch) - (.first_ts | iso_to_epoch)) / 3600
        else 0 end;

"**Session** `\(.session_id)`",
    "",
    "- Window: \(.first_ts) → \(.last_ts) (\(duration_h | tostring[0:5]) h)",
    "- Messages: \(.message_counts.user // 0) user / \(.message_counts.assistant // 0) assistant / \(.message_counts.system // 0) system",
    "- Tool calls: \(.tool_calls_by_name | to_entries | map("\(.key)=\(.value)") | join(", "))",
    "- Git: \(.git_actions | to_entries | map("\(.value) \(.key)") | join(", "))",
    "- Sub-agents dispatched: \(.agents_dispatched | length) total, by model: \(.agents_by_model | to_entries | map("\(.value) \(.key)") | join(", "))",
    if (.skills_invoked | length) > 0 then
        "- Skills invoked: \(.skills_invoked | to_entries | map("\(.value)× \(.key)") | join(", "))"
    else empty end,
    if (.planar_verbs | length) > 0 then
        "- Planar verbs (top 10): \(.planar_verbs | to_entries | sort_by(-.value) | .[0:10] | map("\(.value)× `\(.key)`") | join(", "))"
    else empty end,
    "",
    "**Token usage by model**",
    "",
    "| Model | Turns | Input | Output | Cache read | Cache create |",
    "|---|---:|---:|---:|---:|---:|",
    (.per_model_usage | sort_by(-.output)[] |
        "| `\(.model)` | \(.turns) | \(.input) | \(.output) | \(.cache_read) | \(.cache_create) |"),
    if (.subagent_count // 0) > 0 then
        "",
        "**Sub-agent runs**: \(.subagent_count) total"
    else empty end,
    if $show_subs == 1 and (.subagents // []) != [] then
        "",
        "| Agent ID | Model | Turns | Tools | Input | Output | Duration |",
        "|---|---|---:|---:|---:|---:|---:|",
        (.subagents | sort_by(.first_ts)[] |
            (if (.first_ts and .last_ts) then ((.last_ts | iso_to_epoch) - (.first_ts | iso_to_epoch)) else 0 end) as $dur_s |
            "| `\(.agent_id)` | \(.model // "?") | \(.turn_count) | \(.tool_count) | \(.input_tokens // 0) | \(.output_tokens // 0) | \(($dur_s/60)|floor)m\(($dur_s%60)|floor)s |")
    else empty end
'
