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
/// for what it CONTAINS avoids inheriting a quoting wart for no gain. The
/// packet builder, when it lands, joins this module rather than displacing
/// it.
///
/// ## Only `validate` is here — the 8-section PACKET is deferred
///
/// zig/src/engine/runtime/resume.zig is 994 lines, of which `validate` is
/// ~75. The rest is `buildPacket`, and it is blocked at LAYER 2 on four
/// separate surfaces this tree does not have, not on this bucket:
///
///   section 3, plan position   needs the plan_step completed/current/
///                              remaining fold. `planar.engine.planning.plan`
///                              carries no step read path.
///   section 4, operational     needs external_links listed BY ENTITY plus
///                              their remote-status/conflict state.
///                              `planar.engine.external.link` is
///                              create/show/remove only.
///   section 5, recent activity needs `session::recent_entries_for_task`,
///                              which is not ported (the session bucket has
///                              `list_entries_for_session` only).
///   sections 6+7, decisions,   need engine.planning.{decision,question,
///   questions, artifacts       artifact}. NONE of those modules exists in
///                              `src/lib/engine/planning/` at all.
///
/// Deferred WITH its dependencies, the way `capture commits`, `report` and
/// `dashboard` already are. The oracle behavior it must reproduce is
/// captured in `src/cmd/planar/handlers/resume.cppm`.
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

} // namespace planar::engine::runtime::resumecheck
