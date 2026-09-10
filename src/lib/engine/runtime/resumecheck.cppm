/// @file resumecheck.cppm
/// @brief `planar.engine.runtime.resumecheck` — the resumability check
/// behind `planar resume validate` and `audit handoff-readiness`
/// (plan 996, task 6040).
///
/// Port target: `validate`, `ValidationFailure`, `ValidationResult` and
/// their two renderers from zig/src/engine/runtime/resume.zig.
///
/// ## Named `resumecheck`, not `resume`
///
/// `resume` is not a C++ keyword, but it IS a reserved-ish identifier in
/// the neighbourhood of coroutine vocabulary and — more concretely — the
/// Zig original already has to spell its own module `@"resume"` at every
/// call site because `resume` IS a keyword there. Naming the C++ module
/// for what it CONTAINS avoids inheriting a quoting wart for no gain.
///
/// ## The 8-section PACKET (task 6455)
///
/// zig/src/engine/runtime/resume.zig is 994 lines, of which `validate` is
/// ~75; the rest is `buildPacket`. Task 6452's blocker inventory (repeated
/// in `src/lib/engine/runtime/CMakeLists.txt` and in this module's own
/// prior revision) named FOUR layer-2 surfaces this bucket supposedly
/// lacked. Re-verified at task 6455 by RUNNING the oracle and reading the
/// actual module interfaces rather than trusting the inherited note: TWO
/// of the four had already landed as unrelated work progressed —
/// `planar.engine.external.link::links_for_entity` (task 6106) and all
/// three of `planar.engine.planning.{decision,question,artifact}` exist.
/// A THIRD was never actually missing: `planar.engine.planning.plan_step`
/// already carries `list_steps(conn, plan_id)`, ordered by ordinal — the
/// note's claim that "`engine.planning.plan` has no step read path" was
/// true of `plan.cppm` specifically but overlooked its sibling module.
/// Only session's per-task readers were genuinely absent, and task 6455
/// added them to `session.cppm`: `recent_entries_for_task` and
/// `recent_sessions_for_task`.
///
/// None of that changes the LAYER shape, though: `plan_step`,
/// `external.link`, and `engine.planning.{decision,question,artifact}`
/// are layer-2 buckets sibling to this one, and `cmake/architecture.cmake`
/// FATALs on any `engine_* -> engine_*` edge (D15/D18) — the same
/// constraint `agentatomic.cppm` and `handoff.cppm` already document at
/// length. Rather than inject four more callables (the injection pattern
/// those two modules use for policy hooks), `build_packet` below reproduces
/// the oracle's OWN raw SQL for plan position, operational-plane links,
/// decisions, questions, and artifacts directly — exactly the shape
/// `resume.zig`'s `buildPlanPosition` / `buildOperationalPlane` /
/// `buildDecisions` / `buildQuestions` / `buildArtifacts` already use
/// (none of them call into `plan_step.zig` or `link.zig` either; they
/// read the tables directly). Only `session`, `snapshot`, `agentactivity`
/// and `handoff` are consulted through real calls, because all four live
/// in THIS bucket already (same-library imports, not a cross-bucket edge).
///
/// `decisions_for_task` in `planar.engine.planning.decision` is NOT reused
/// here even though the name suggests it should be: its query (LEFT JOIN
/// sessions, OR scope_kind = 'global') is `decision.zig`'s unrelated
/// `forTask` reader, not `resume.zig`'s `buildDecisions` (INNER JOIN,
/// task-bound sessions only, no global fallback) — see decision.cppm's
/// own header. Reusing it here would silently change resume's decisions
/// section to include every global decision in the database.
///
/// ## The two rules, DERIVED BY RUNNING THE ORACLE
///
/// Not read off the validator — probed against deliberately broken states
/// in a pinned scratch arena, which is how the three negative results
/// below were found. A packet that validates when it should not is worse
/// than one that refuses, because the caller acts on it.
///
///   1. `next_action` must be non-empty.
///   2. At least one `context_snapshots` row must exist FOR THAT TASK.
///
/// And, equally load-bearing, what is NOT a rule:
///
///   * Task STATUS is not checked. A `done` task with a next_action and a
///     snapshot reports `resumable:true`. Probed directly: task 2 was
///     driven `todo -> doing -> done` and kept validating.
///   * The `next_action` check is a BYTE-LENGTH check, not a
///     whitespace-aware one. A next_action of `" "` PASSES — probed with a
///     task created `--next-action " "`, which produced only the snapshot
///     failure. The Zig source is `coalesce(next_action,'')` plus
///     `.len == 0`, and the port reproduces exactly that. Trimming here
///     would make a task the oracle calls resumable non-resumable.
///   * A SESSION-level snapshot does not count. `capture snapshot` with no
///     `--task` writes a row with `task_id` NULL; task 3 still reported
///     `no context snapshot found` afterwards. The lookup is
///     `where task_id = ?`, so NULL matches nothing.
///   * A snapshot's BODY may be empty. `capture snapshot --task 1` with no
///     `--note` satisfied the check.
///
/// Failures accumulate in a fixed order — `next_action` then `snapshot` —
/// because the JSON array is ordered and a caller may read `failures[0]`.
module;

export module planar.engine.runtime.resumecheck;

import std;
import planar.db;

namespace planar::engine::runtime::resumecheck {

/// @brief One failed resumability check. Mirrors zig's
/// `resume.ValidationFailure`.
export struct validation_failure {
  std::string check;       ///< Stable machine key: `"next_action"` or `"snapshot"`.
  std::string message;     ///< Human-readable statement of what is missing.
  std::string remediation; ///< The exact command that fixes it.
};

/// @brief The outcome of a resumability check. Mirrors zig's
/// `resume.ValidationResult`.
export struct validation_result {
  std::int64_t                    task_id{};         ///< The task checked.
  bool                            resumable = false; ///< True when `failures` is empty.
  std::vector<validation_failure> failures;          ///< Every failed check, in fixed order.
};

/// @brief Error surface for this module.
export enum class resume_error : std::uint8_t {
  not_found,    ///< No task with that id.
  query_failed, ///< An underlying SQL statement failed.
};

/// @brief Check whether `task_id` can be resumed.
/// @param conn An open, migrated database connection.
/// @param task_id The task to check.
/// @return The result, or `resume_error::not_found` when no such task.
export auto validate(db::connection& conn, std::int64_t task_id) -> std::expected<validation_result, resume_error>;

/// @brief Render `planar resume validate --json`.
///
/// Oracle-captured, and the empty case is the part that matters: a
/// resumable task emits `"failures":null`, NOT `"failures":[]`. The
/// `handoff` composite emits `[]` for the same data, so the two renderers
/// genuinely differ and must not be merged.
///
///   {"task_id":2,"resumable":true,"failures":null}
///   {"task_id":1,"resumable":false,"failures":[{"check":"next_action",...}]}
///
/// @param result The checked result.
/// @return The complete stdout payload: the JSON object WITH its trailing
/// newline. The caller writes it verbatim and appends nothing.
export auto render_validate_json(const validation_result& result) -> std::string;

/// @brief Render `planar resume validate`'s text form. Oracle-captured:
///
///   OK task:2 is resume-ready
///   FAIL task:1 is not resumable:
///     - next_action is null → run: planar task update 1 --next-action "<text>"
///
/// The arrow is U+2192, and the two-space indent and `→ run: ` separator
/// are exact.
/// @param result The checked result.
/// @return The complete stdout payload, WITH its trailing newline.
export auto render_validate_text(const validation_result& result) -> std::string;

// =========================================================================
// The 8-section resume packet (task 6455)
// =========================================================================

/// @brief Section 1. Mirrors zig's `resume.Identity`.
export struct identity {
  std::int64_t                task_id{};  ///< The resolved task.
  std::optional<std::int64_t> plan_id;    ///< The task's parent plan, when bound.
  std::string                 title;      ///< Task title.
  std::string                 status;     ///< Task status text.
  std::string                 scope_kind; ///< Task scope kind (`repo`/`association`/`global`).
  std::optional<std::int64_t> scope_id;   ///< Task scope id, unset for `global`.
};

/// @brief Section 2. Mirrors zig's `resume.State`.
export struct state {
  std::string status;                ///< Task status text (duplicated from `identity` in the oracle).
  std::string next_action;           ///< The task's stored `next_action`.
  std::string last_action_at   = ""; ///< Timestamp of the most recent context, snapshot-preferred.
  std::string last_action_body = ""; ///< Body of the most recent context, snapshot-preferred.
};

/// @brief One `plan_steps` row folded into a plan-position bucket. Mirrors
/// zig's `resume.PlanStep`.
export struct plan_step_summary {
  std::int64_t ordinal{}; ///< Position within the plan.
  std::string  body;      ///< The step text.
  std::string  status;    ///< The step's stored status text.
};

/// @brief Section 3. Mirrors zig's `resume.PlanPosition`. `done` and
/// `skipped` steps fold into `completed`; `in-progress` folds into
/// `current`; everything else (`pending`) folds into `remaining` —
/// matching `resume.zig`'s `buildPlanPosition` fold exactly.
export struct plan_position {
  std::optional<std::int64_t>    plan_id;         ///< The task's parent plan, when bound.
  std::string                    plan_title = ""; ///< The parent plan's title.
  std::vector<plan_step_summary> completed;       ///< `done` + `skipped` steps.
  std::vector<plan_step_summary> current;         ///< `in-progress` steps.
  std::vector<plan_step_summary> remaining;       ///< Every other (`pending`) step.
};

/// @brief One recent `session_entries` row. Mirrors zig's
/// `resume.RecentEntry`.
export struct recent_entry {
  std::int64_t session_id{}; ///< The owning session.
  std::string  prefix;       ///< Entry prefix (`action`, `note`, ...).
  std::string  body;         ///< Entry text.
  std::string  created_at;   ///< Entry timestamp.
};

/// @brief One `decisions` row summary. Mirrors zig's
/// `resume.DecisionSummary`.
export struct decision_summary {
  std::int64_t id{};   ///< The decision's row id.
  std::string  title;  ///< Decision title.
  std::string  status; ///< Decision status text.
};

/// @brief One `questions` row summary. Mirrors zig's
/// `resume.QuestionSummary`.
export struct question_summary {
  std::int64_t id{};             ///< The question's row id.
  std::string  title;            ///< Question title.
  std::string  status;           ///< Question status text.
  std::string  answer_body = ""; ///< The answer body, when answered.
};

/// @brief One artifact linked to the task. Mirrors zig's
/// `resume.ArtifactLink`.
export struct artifact_link {
  std::int64_t artifact_id{}; ///< The artifact's row id.
  std::string  title;         ///< Artifact title.
  std::string  kind;          ///< Artifact kind text.
  std::string  relationship;  ///< The `entity_links.relationship` value.
};

/// @brief One `external_links` row surfaced on the operational plane.
/// Mirrors zig's `resume.ExternalLinkState`. `remote_status` /
/// `remote_assignee` are always empty — decision 996 retired the
/// auto-applied remote mirror this port would otherwise have carried;
/// see `planar-ext`'s doctrine in the top-level agent guide.
export struct external_link_state {
  std::int64_t link_id{};             ///< The link's row id.
  std::string  external_id;           ///< The external ticket id.
  std::string  external_url    = "";  ///< The ticket URL, when known.
  std::string  remote_status   = "";  ///< Always empty (see above).
  std::string  remote_assignee = "";  ///< Always empty (see above).
  std::string  last_synced_at  = "";  ///< Last sync timestamp, when ever synced.
  std::string  sync_status;           ///< `last_sync_status` text.
  bool         conflict      = false; ///< True when `sync_status == "conflict"`.
  std::string  refresh_error = "";    ///< Always empty in this port.
};

/// @brief Section 4. Mirrors zig's `resume.OperationalPlane`.
export struct operational_plane {
  std::vector<external_link_state> links;             ///< Every `external_links` row on this task.
  std::string                      refresh_note = ""; ///< Always empty in this port.
};

/// @brief Section 8's session half. Mirrors zig's `resume.Audit`.
export struct audit_footer {
  std::int64_t session_id{}; ///< The most recent session bound to the task.
  std::string  vendor;       ///< That session's vendor.
  std::string  started_at;   ///< That session's start timestamp.
};

/// @brief Section 8's active-claim half. Mirrors zig's
/// `resume.ActiveClaim`.
export struct active_claim_state {
  std::int64_t claim_id{};         ///< The claim's row id.
  std::string  claim_token;        ///< The claim token.
  std::string  vendor;             ///< Claiming vendor.
  std::string  worktree_path = ""; ///< Worktree path, when the claim carries one.
  std::string  repo_root     = ""; ///< Repo root, when the claim carries one.
  std::string  branch        = ""; ///< Branch, when the claim carries one.
};

/// @brief Section 8's cold-start fallback. Mirrors zig's
/// `resume.HandoffWorktree`.
export struct handoff_worktree_state {
  std::int64_t handoff_id{};       ///< The handoff's row id.
  std::string  worktree_path = ""; ///< Worktree path, when the handoff carries one.
  std::string  repo_root     = ""; ///< Repo root, when the handoff carries one.
  std::string  branch        = ""; ///< Branch, when the handoff carries one.
};

/// @brief The complete 8-section resume packet. Mirrors zig's
/// `resume.Packet` field-for-field, including field ORDER — the JSON
/// renderer emits them in this order to match `std.json.Stringify`'s
/// declaration-order emission.
export struct packet {
  identity                              ident;           ///< Section 1.
  state                                 st;              ///< Section 2.
  plan_position                         plan;            ///< Section 3.
  operational_plane                     op_plane;        ///< Section 4.
  std::vector<recent_entry>             recent_activity; ///< Section 5.
  std::vector<decision_summary>         decisions;       ///< Section 6a.
  std::vector<question_summary>         questions;       ///< Section 6b.
  std::vector<artifact_link>            artifacts;       ///< Section 7.
  std::optional<audit_footer>           audit;           ///< Section 8, session half.
  std::optional<active_claim_state>     active_claim;    ///< Section 8, active-claim half.
  std::optional<handoff_worktree_state> from_handoff;    ///< Section 8, cold-start fallback.
};

/// @brief Assemble the full resume packet for `task_id`. Mirrors zig's
/// `resume.buildPacket`.
/// @param conn An open, migrated database connection.
/// @param task_id The task to build the packet for.
/// @return The packet, or `resume_error::not_found` when no such task.
export auto build_packet(db::connection& conn, std::int64_t task_id) -> std::expected<packet, resume_error>;

/// @brief Render `planar resume [<task-id>] --json`. Oracle-captured shape
/// (field names/nesting; see this module's header for how the values were
/// re-derived at task 6455).
/// @param p The assembled packet.
/// @return The complete stdout payload: the JSON object WITH its trailing
/// newline.
export auto render_packet_json(const packet& p) -> std::string;

/// @brief Render `planar resume [<task-id>]`'s text form: eight `## N.
/// <Title>` sections under a `=== Resume Packet: task N ===` banner.
/// Oracle-captured byte-for-byte from `handlers/resume/cmd.zig`'s
/// `renderText`.
/// @param p The assembled packet.
/// @return The complete stdout payload, WITH its trailing newline.
export auto render_packet_text(const packet& p) -> std::string;

} // namespace planar::engine::runtime::resumecheck
