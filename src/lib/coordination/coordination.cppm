/// @file coordination.cppm
/// @brief `planar.coordination` — the shared predicates behind a task's
/// coordination readiness (plan 1142, task 7477).
///
/// A task can be held up by three things that are not a claim: a dependency
/// that is not finished, a question that is still open, and a decision that is
/// still proposed. The task packet reports each as a readiness reason and
/// `recommend-strategy` reports the last two as rules 5 and 6. Both ask this
/// module, so a link is never open to one reader and resolved to the other.
///
/// Every function is a read-only SELECT over `entity_links` and the linked
/// entity's table. Entry points return the ids of the blocking entities in
/// ascending order, so a caller that only needs a yes or no tests for empty
/// and a caller that reports the entities has them. Failure is
/// `error::query_failed`; no other error exists at this boundary.
module;

export module planar.coordination;

import std;
import planar.db;

namespace planar::coordination {

/// @brief Why a predicate could not be evaluated.
export enum class error : std::uint8_t {
  query_failed, ///< SQL failure.
};

/// @brief The `questions.status` that holds a task back.
export inline constexpr std::string_view open_question_status = "open";

/// @brief The `decisions.status` that holds a task back.
export inline constexpr std::string_view proposed_decision_status = "proposed";

/// @brief The questions linked to a task, in either direction, that are
/// still open.
///
/// Any relationship counts: a task that `addresses` a question and a question
/// that was linked to the task both hold it back.
/// @param conn An open database connection.
/// @param task_id The task.
/// @return The open question ids ascending, or the failure.
export [[nodiscard]] auto open_questions_linked(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<std::int64_t>, error>;

/// @brief The decisions linked to a task, in either direction, that are
/// still proposed.
/// @param conn An open database connection.
/// @param task_id The task.
/// @return The proposed decision ids ascending, or the failure.
export [[nodiscard]] auto proposed_decisions_linked(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<std::int64_t>, error>;

/// @brief The tasks this task directly depends on that are not finished.
///
/// A dependency is a `depends-on` link from the task to another task, and it
/// is unfinished while that task's status is neither `done` nor `cancelled`.
/// The inverse `blocks` relationship named in older notes no longer exists:
/// migration 00033 renamed it `depends-on` and the table's CHECK constraint
/// admits no other spelling, so one direction is the whole predicate. A
/// self-link never counts.
/// @param conn An open database connection.
/// @param task_id The task.
/// @return The unfinished dependency ids ascending, or the failure.
export [[nodiscard]] auto unfinished_dependencies(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<std::int64_t>, error>;

} // namespace planar::coordination
