/// @file diff.cppm
/// @brief `planar.engine.ingest.diff` — reconcile parsed spec data against
/// stored rows into a proposed change set.
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/diff.zig`. This
/// is step 2 of the three-phase ingest pipeline: parse markdown (see
/// `planar.engine.ingest.parse`) → COMPUTE a diff of add/update/remove
/// entries → apply it. The apply step is deliberately not part of this
/// module's C++ port; see this bucket's CMakeLists.txt for the D15/D18
/// layering reason.
///
/// The reconciliation key throughout is the entity's TITLE, compared
/// case-insensitively after trimming:
///
///   * roadmap H2 milestone → child plan;
///   * roadmap bullet       → task under that child plan;
///   * tech-spec `## Decisions` H3      → decision;
///   * tech-spec `## Open Questions` H3 → question, scoped to the anchor's scope;
///   * test-spec `## Scenarios` H3/H4   → `test_scenarios` row linked to the anchor.
///
/// Two preservation rules are load-bearing and easy to break silently:
///
///   * A task body is overwritten ONLY when it byte-matches a body Planar
///     itself generated (in either shipped shape). An operator who replaced
///     the generated acceptance criteria with real ones keeps them across
///     every re-ingest; overwriting unconditionally is what made enrichment
///     impossible, because the only way to refresh facts was to destroy the
///     content that made the task dispatchable.
///   * A task slug is back-filled only when the stored slug is NULL/empty.
///     A non-null slug is never silently overwritten.

module;

export module planar.engine.ingest.diff;

import std;
import planar.db;
import planar.engine.ingest.parse;

namespace planar::engine::ingest::diff {

/// @brief The kind of change proposed for one entity.
export enum class op : std::uint8_t {
  add,    ///< The entity does not exist yet.
  update, ///< The entity exists and its content differs.
  remove  ///< The entity exists but is no longer in the spec.
};

/// @brief Renders `value` as its lowercase wire name (`add`/`update`/`remove`).
/// @param value The op to name.
/// @return The stable wire name, used by both renderers.
export [[nodiscard]] auto to_string(op value) -> std::string_view;

/// @brief Proposed change for one task.
export struct task_entry {
  op                       op_ = op::add;     ///< Whether this task is added, updated, or removed.
  std::string              title_;            ///< Task title, from the roadmap bullet.
  std::string              body_;             ///< Empty on update means "leave the stored body alone".
  std::vector<std::string> touches_;          ///< Repos or paths from `[touches: …]`.
  std::vector<std::string> depends_;          ///< Slugs from the bullet's `[depends: …]`.
  std::string              slug_;             ///< From `[slug:]`; empty when absent.
  std::string              next_action_;      ///< Empty means "leave whatever is stored".
  std::int64_t             existing_id_ = 0;  ///< Stored task id; 0 for an add.
  std::string              child_plan_title_; ///< Parent milestone title (display only).
};

/// @brief Proposed change for one child plan (one roadmap milestone).
export struct plan_entry {
  op                      op_ = op::add;    ///< Whether this plan is added, updated, or removed.
  std::string             title_;           ///< Milestone heading text.
  std::int64_t            existing_id_ = 0; ///< Set for update/remove.
  std::vector<task_entry> tasks_;           ///< In source order.
};

/// @brief Proposed change for one decision.
export struct decision_entry {
  op           op_ = op::add;    ///< Whether this decision is added or updated.
  std::string  title_;           ///< Decision heading text.
  std::string  body_;            ///< Decision body text.
  std::int64_t existing_id_ = 0; ///< Stored decision id; 0 for an add.
};

/// @brief An `open` → `answered` transition proposed for a stored question.
///
/// Produced when the parsed question carries a `Resolution:` and the stored
/// row under the same title is still open.
export struct question_status_change {
  std::int64_t question_id_ = 0; ///< Stored question id to flip.
  std::string  question_title_;  ///< Stored question title, for display.
  std::string  old_status_;      ///< Status before the flip; always `open`.
  std::string  new_status_;      ///< Status after the flip; always `answered`.
  std::string  answer_;          ///< The `Resolution:` text to record as the answer.
};

/// @brief Proposed change for one test-spec scenario.
export struct scenario_entry {
  op                           op_ = op::add;    ///< Whether this scenario is added, updated, or removed.
  std::string                  title_;           ///< Scenario heading text.
  std::string                  body_;            ///< Reconstructed row body, field lines plus prose.
  std::string                  kind_;            ///< Free-form `**Kind:**` value.
  std::string                  acceptance_;      ///< One-line `**Acceptance:**` value.
  std::vector<parse::task_ref> verifies_;        ///< Parsed `**Verifies:**` refs.
  std::int64_t                 existing_id_ = 0; ///< Stored scenario id; 0 for an add.
};

/// @brief Parsed roadmap provenance for one task.
///
/// Emitted for EVERY roadmap bullet, including bullets whose task is unchanged
/// and therefore absent from the mutation diff — the citation must be staged
/// regardless, or an unchanged task loses its provenance on re-ingest.
export struct roadmap_citation {
  std::int64_t existing_task_id_ = 0; ///< 0 when the task is being created.
  std::string  child_plan_title_;     ///< Owning milestone title.
  std::string  task_title_;           ///< Task title, as the bullet renders it.
  std::string  source_locator_;       ///< `roadmap#milestone:N/item:M`, 1-based.
  std::string  source_text_;          ///< Canonical folded bullet.
};

/// @brief A proposed ADD task slug already held by a task outside this ingest.
///
/// `tasks.slug` carries a global partial-unique index (migration 00011
/// `ux_tasks_slug`), so applying this diff would fail with a slug conflict.
/// The preview surfaces the collision first, naming the holder, because the
/// obvious search for it does not find it: the index is status-agnostic, so a
/// CANCELLED or DONE task still holds its slug.
///
/// Child-plan slugs use per-parent uniqueness and are not globally unique, so
/// they are not checked here; decisions, questions and scenarios created by
/// ingest have null slugs.
export struct slug_collision {
  std::string  slug_;                 ///< The slug string that collides.
  std::int64_t existing_task_id_ = 0; ///< Id of the task already holding it.
  std::int64_t existing_plan_id_ = 0; ///< From the `tasks.plan_id` column.
};

/// @brief The full proposed change set for one ingestion pass.
export struct diff {
  std::int64_t anchor_plan_id_ = 0; ///< The root plan this diff is for.
  std::string  anchor_slug_;        ///< The anchor plan's slug.
  std::string  assoc_slug_;         ///< The anchor's association slug; empty when global.
  std::string  current_status_;     ///< The anchor's stored status at compute time.

  std::vector<plan_entry>             child_plans_;             ///< One per roadmap milestone, in source order.
  std::vector<decision_entry>         decisions_;               ///< Proposed decision adds and updates.
  std::vector<parse::question>        new_questions_;           ///< Questions with no stored counterpart.
  std::vector<question_status_change> updated_question_status_; ///< Proposed open -> answered flips.
  std::vector<task_entry>             orphan_tasks_;            ///< Stored tasks absent from the roadmap.
  std::vector<plan_entry>             orphan_plans_;            ///< Stored milestones absent from the roadmap.
  std::vector<scenario_entry>         scenarios_;               ///< Proposed scenario adds and updates.
  std::vector<roadmap_citation>       roadmap_citations_;       ///< Provenance for EVERY bullet, changed or not.
  std::vector<slug_collision>         slug_collisions_;         ///< ADD slugs already held elsewhere.

  /// @brief Count of proposed additions across plans, tasks, decisions, questions.
  /// @return The total.
  [[nodiscard]] auto total_additions() const -> std::size_t;
  /// @brief Count of proposed updates across tasks, decisions, question status flips.
  /// @return The total.
  [[nodiscard]] auto total_updates() const -> std::size_t;
  /// @brief Count of proposed removals (orphan tasks plus orphan plans).
  /// @return The total.
  [[nodiscard]] auto total_removals() const -> std::size_t;
  /// @brief Whether this diff proposes no change at all.
  /// @return `true` when additions, updates and removals are all zero.
  [[nodiscard]] auto is_empty() const -> bool;
};

/// @brief Failure surface for `compute`.
export enum class diff_error : std::uint8_t {
  not_found,   ///< The anchor plan does not exist, or is not a root plan.
  query_failed ///< A SQLite operation failed.
};

/// @brief Computes the proposed diff for `anchor_plan_id` against parsed spec data.
///
/// Reads the stored child plans, tasks, decisions, questions and scenarios
/// linked to the anchor and emits the additions, updates and removals an apply
/// step would commit. Writes nothing.
/// @param conn An open connection to a migrated database.
/// @param anchor_plan_id The root plan the spec documents belong to.
/// @param milestones Parsed roadmap milestones.
/// @param decisions Parsed tech-spec decisions.
/// @param questions Parsed tech-spec open questions.
/// @param scenarios Parsed test-spec scenarios.
/// @return The proposed change set, or the failure.
export [[nodiscard]] auto compute(db::connection& conn, std::int64_t anchor_plan_id, std::span<const parse::milestone> milestones,
                                  std::span<const parse::decision> decisions, std::span<const parse::question> questions,
                                  std::span<const parse::scenario> scenarios) -> std::expected<diff, diff_error>;

/// @brief Whether a task body warrants an auto-drafted test scenario.
///
/// The heuristic is two or more bullet lines, mirroring the Zig original.
/// @param body The task body.
/// @return `true` when the body holds at least two bullets.
export [[nodiscard]] auto is_non_trivial(std::string_view body) -> bool;

/// @brief The suffix older builds appended to every generated acceptance criterion.
///
/// Retained ONLY so bodies written by those builds are still recognised as
/// generated and can be upgraded. Without it, dropping the suffix would make
/// every pre-existing generated body differ from the current projection,
/// enrichment detection would classify it as operator-written, and the generic
/// text would be preserved permanently — the fix would look correct on new
/// tasks while freezing every old one.
export inline constexpr std::string_view legacy_acceptance_suffix = " is implemented and tested.";

/// @brief The next action older builds wrote for every generated task.
///
/// Routing's generic-next-action check rejects this phrase, so a task born
/// with it is unroutable. Recognised here only so a still-generated next
/// action can be replaced while a refined one is preserved.
export inline constexpr std::string_view legacy_next_action = "Implement per acceptance criteria.";

/// @brief Composes the generated task body from a roadmap work item.
///
/// The roadmap item title IS the acceptance criterion — it is authored
/// content, not a placeholder, so no suffix is appended to it.
/// @param item The roadmap work item.
/// @return The generated body.
export [[nodiscard]] auto build_task_body(const parse::work_item& item) -> std::string;

/// @brief Reproduces the body an older build would have generated for `item`.
///
/// Used only to answer "did a previous Planar write this, or did an operator?".
/// @param item The roadmap work item.
/// @return The legacy-shaped generated body.
export [[nodiscard]] auto build_legacy_task_body(const parse::work_item& item) -> std::string;

/// @brief Composes the generated starting next action for a work item.
///
/// Names the roadmap position the task derives from, which is specific and
/// true, rather than a platitude routing would reject. An operator refining it
/// is the expected path, not a correction.
/// @param item The roadmap work item.
/// @param milestone_index 1-based milestone position.
/// @param item_index 1-based item position within the milestone.
/// @return The generated next action.
export [[nodiscard]] auto build_task_next_action(const parse::work_item& item, std::size_t milestone_index,
                                                 std::size_t item_index) -> std::string;

/// @brief Whether `stored` is a body Planar generated rather than operator work.
///
/// Byte comparison against every shipped generated projection, so the answer
/// is exact. A "looks generated" heuristic would be the wrong tool: guessing
/// wrong discards real operator work on re-ingest.
/// @param stored The body currently in the database.
/// @param item The roadmap work item the task derives from.
/// @return `true` when `stored` matches a generated projection exactly.
export [[nodiscard]] auto is_generated_task_body(std::string_view stored, const parse::work_item& item) -> bool;

/// @brief Reconstructs a `test_scenarios` row body from a parsed scenario.
///
/// Preserves the `**Verifies:**` / `**Kind:**` / `**Acceptance:**` field lines
/// plus the prose, so the stored row stays human-readable and re-parses.
/// @param s The parsed scenario.
/// @return The composed body, trimmed.
export [[nodiscard]] auto build_scenario_body(const parse::scenario& s) -> std::string;

} // namespace planar::engine::ingest::diff
