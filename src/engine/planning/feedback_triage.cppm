/// @file feedback_triage.cppm
/// @brief `planar.engine.planning.feedback_triage` — structured triage over
/// redacted feedback findings (plan 996, task 6303).
///
/// Ported from `zig/src/engine/planning/feedback_triage.zig` (302 lines).
/// The `feedback_triage` TABLE has been present since migration 00028; this
/// cycle ports the ENGINE that reads and writes it, unblocking the three
/// `feedback triage {list,show,set}` leaves that dispatch had carried as
/// `not_implemented` placeholders.
///
/// Three contracts here are load-bearing and were measured against
/// `zig/zig-out/bin/planar` in a pinned scratch arena rather than inferred
/// from the sibling planning modules:
///
///   1. A FINDING IS AN ENTITY REF, not an id. `task:12` / `question:3`.
///      `parse_finding_ref` is the only accepted spelling; a bare integer is
///      `invalid_input`, which is why the leaves declare the positional as a
///      string.
///   2. THE FEEDBACK PLAN IS IDENTIFIED BY SLUG, and the slug is the literal
///      `planar-feedback`. A finding whose plan is any other plan resolves to
///      `different_feedback_plan`, NOT to a successful triage row. This is
///      what makes a plain plan the wrong fixture for every interesting arm
///      of this family.
///   3. `duplicate` AND `duplicate_of` ARE MUTUALLY ENTAILING. Setting one
///      without the other is `invalid_input` in both directions, and the
///      duplicate target is walked to the end of its chain so a cycle is
///      rejected before the write rather than by the table's CHECK.
///
/// The table also carries a `before delete` trigger that resets dependents to
/// `untriaged`; nothing here deletes rows, so that trigger is out of scope for
/// this module but is why `duplicate_of_triage_id` is nullable.
module;

export module planar.engine.planning.feedback_triage;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Which entity table a feedback finding lives in.
export enum class feedback_finding_kind : std::uint8_t {
  task,     ///< Wire form `task`; the finding is a `tasks` row.
  question, ///< Wire form `question`; the finding is a `questions` row.
};

/// @brief Operator-assessed impact of a finding.
export enum class feedback_severity : std::uint8_t {
  info,
  low,
  medium,
  high,
  critical,
};

/// @brief What the operator decided to do with a finding.
///
/// Three enumerators are underscored here and HYPHENATED on the wire and in
/// the column: `needs_reproduction`, `retained_question`, `reported_external`.
export enum class feedback_disposition : std::uint8_t {
  untriaged,
  needs_reproduction, ///< Wire form `needs-reproduction`.
  accepted,
  retained_question, ///< Wire form `retained-question`.
  dismissed,
  reported_external, ///< Wire form `reported-external`.
  duplicate,
};

/// @brief Whether the finding was reproduced, and to what end.
///
/// `not_run` and `not_reproduced` are hyphenated on the wire.
export enum class feedback_reproduction : std::uint8_t {
  not_run,        ///< Wire form `not-run`.
  reproduced,     ///< Wire form `reproduced`.
  not_reproduced, ///< Wire form `not-reproduced`.
  inconclusive,   ///< Wire form `inconclusive`.
};

/// @brief Failure modes of this module.
///
/// Enumerators mirror the oracle's error set one-for-one so the handler's
/// `@errorName`-shaped diagnostics keep their spelling.
export enum class feedback_triage_error : std::uint8_t {
  not_found,               ///< Zig spelling: NotFound.
  invalid_input,           ///< Zig spelling: InvalidInput.
  missing_feedback_plan,   ///< Zig spelling: MissingFeedbackPlan.
  ambiguous_feedback_plan, ///< Zig spelling: AmbiguousFeedbackPlan.
  different_feedback_plan, ///< Zig spelling: DifferentFeedbackPlan.
  duplicate_cycle,         ///< Zig spelling: DuplicateCycle.
  query_failed,            ///< Zig spelling: QueryFailed.
};

/// @brief A reference to a feedback finding: a kind plus a row id.
export struct feedback_finding_ref {
  feedback_finding_kind kind; ///< Which entity table the finding lives in.
  std::int64_t          id;   ///< The finding's row id; always 1 or greater.
};

/// @brief One `feedback_triage` row, joined to its finding and duplicate target.
///
/// Field order is the JSON wire order — the oracle serializes this struct by
/// declaration order, so reordering these fields is a wire change.
export struct feedback_triage {
  std::int64_t                id;                  ///< The `feedback_triage` row id.
  std::string                 finding;             ///< Rendered ref, e.g. `task:12`.
  std::optional<std::int64_t> plan_id;             ///< The feedback plan, via the finding.
  feedback_severity           severity;            ///< Operator-assessed impact.
  feedback_disposition        disposition;         ///< What the operator decided.
  feedback_reproduction       reproduction_status; ///< Whether it was reproduced.
  std::optional<std::string>  duplicate_of;        ///< Rendered ref of the target, if any.
  std::optional<std::string>  evidence_summary;    ///< Redacted evidence, if recorded.
  std::string                 created_at;          ///< ISO-8601 creation timestamp.
  std::string                 updated_at;          ///< ISO-8601 last-write timestamp.
};

/// @brief Optional narrowing for `list_feedback_triage`.
export struct feedback_triage_list_filter {
  std::optional<std::int64_t>         plan_id;     ///< Narrow to one feedback plan.
  std::optional<feedback_severity>    severity;    ///< Narrow to one severity.
  std::optional<feedback_disposition> disposition; ///< Narrow to one disposition.
};

/// @brief The operator-confirmed fields `set_feedback_triage` writes.
export struct feedback_triage_set_args {
  feedback_severity                   severity;     ///< Operator-assessed impact.
  feedback_disposition                disposition;  ///< What the operator decided.
  feedback_reproduction               reproduction; ///< Whether it was reproduced.
  std::optional<feedback_finding_ref> duplicate_of; ///< Required iff `disposition` is `duplicate`.
  std::optional<std::string>          evidence;     ///< Stored as SQL NULL when unset.
};

/// @brief Parse a `task:<id>` / `question:<id>` finding reference.
///
/// Returns `invalid_input` for a missing colon, an unknown kind, a
/// non-numeric id, or an id below 1. Note this is `expected`, not `optional`:
/// the oracle raises the same error the rest of the module raises, and the
/// handler maps it to a dedicated message.
/// @param raw The operator-supplied token.
/// @return The parsed reference, or `invalid_input`.
export auto parse_finding_ref(std::string_view raw) -> std::expected<feedback_finding_ref, feedback_triage_error>;

/// @brief Render a finding reference back to its `kind:id` wire form.
/// @param ref The reference to render.
/// @return The `task:<id>` or `question:<id>` spelling.
export auto finding_ref_to_text(feedback_finding_ref ref) -> std::string;

/// @brief Parse a `--severity` token or a `severity` column value.
/// @param s The token to parse.
/// @return The severity, or `std::nullopt` when `s` names none.
export auto feedback_severity_from_text(std::string_view s) -> std::optional<feedback_severity>;
/// @brief Render `s` as the wire/column text form.
/// @param s The severity to render.
/// @return The wire spelling.
export auto feedback_severity_to_text(feedback_severity s) -> std::string_view;
/// @brief Parse a `--disposition` token or a `disposition` column value.
/// @param s The token to parse.
/// @return The disposition, or `std::nullopt` when `s` names none.
export auto feedback_disposition_from_text(std::string_view s) -> std::optional<feedback_disposition>;
/// @brief Render `d` as the wire/column text form.
/// @param d The disposition to render.
/// @return The wire spelling, hyphenated where the enumerator is underscored.
export auto feedback_disposition_to_text(feedback_disposition d) -> std::string_view;
/// @brief Parse a `--reproduction` token or a `reproduction_status` column value.
/// @param s The token to parse.
/// @return The reproduction status, or `std::nullopt` when `s` names none.
export auto feedback_reproduction_from_text(std::string_view s) -> std::optional<feedback_reproduction>;
/// @brief Render `r` as the wire/column text form.
/// @param r The reproduction status to render.
/// @return The wire spelling, hyphenated where the enumerator is underscored.
export auto feedback_reproduction_to_text(feedback_reproduction r) -> std::string_view;

/// @brief Resolve the scope the finding's own row is owned by.
///
/// Returns `std::nullopt` for a `global` finding — NOT an error. A repo-scoped
/// finding resolves to `repo:<project-slug>`; every other non-global kind
/// resolves to `assoc:<association-slug>`. The caller feeds this to the
/// cross-scope membership guard before a `set` writes.
/// @param conn Open database connection.
/// @param ref The finding to resolve.
/// @return The scope ref string, `std::nullopt` for global, or an error.
export auto entity_scope(db::connection& conn, feedback_finding_ref ref)
    -> std::expected<std::optional<std::string>, feedback_triage_error>;

/// @brief Read the triage row for one finding.
/// @param conn Open database connection.
/// @param ref The finding to read.
/// @return The row, or `not_found` when the finding has never been triaged.
export auto show_feedback_triage(db::connection& conn, feedback_finding_ref ref)
    -> std::expected<feedback_triage, feedback_triage_error>;

/// @brief List triage rows, narrowed by `filter`.
///
/// Ordered by feedback plan, then most-recently-updated first, then id. An
/// empty result is success, not `not_found`.
/// @param conn Open database connection.
/// @param filter Optional plan / severity / disposition narrowing.
/// @return The matching rows in oracle order, or an error.
export auto list_feedback_triage(db::connection& conn, const feedback_triage_list_filter& filter)
    -> std::expected<std::vector<feedback_triage>, feedback_triage_error>;

/// @brief Upsert the operator-confirmed triage for one finding.
///
/// Validates in the oracle's order — finding plan first, then the
/// duplicate/`duplicate_of` entailment, then the duplicate chain walk — so a
/// caller that supplies several bad inputs at once gets the same error the
/// oracle would report. Re-reads and returns the stored row.
/// @param conn Open database connection.
/// @param finding The finding being triaged.
/// @param args The operator-confirmed fields.
/// @return The stored row, or the first validation error.
export auto set_feedback_triage(db::connection& conn, feedback_finding_ref finding, const feedback_triage_set_args& args)
    -> std::expected<feedback_triage, feedback_triage_error>;

/// @brief Render one row as the operator-facing detail block.
/// @param row The row to render.
/// @return Complete payload, trailing newline INCLUDED.
export auto render_text(const feedback_triage& row) -> std::string;
/// @brief Render one row as a JSON object.
/// @param row The row to render.
/// @return A FRAGMENT with no trailing newline; the caller terminates it.
export auto render_json(const feedback_triage& row) -> std::string;
/// @brief Render rows as the operator-facing table.
///
/// The empty case is the literal `(no feedback triage)\n`, not zero bytes.
/// @param rows The rows to render.
/// @return Complete payload, trailing newline INCLUDED.
export auto render_list_text(std::span<const feedback_triage> rows) -> std::string;
/// @brief Render rows as a JSON array.
///
/// The empty case is `[]`, which the caller terminates to the oracle's `[]\n`.
/// @param rows The rows to render.
/// @return A FRAGMENT with no trailing newline; the caller terminates it.
export auto render_list_json(std::span<const feedback_triage> rows) -> std::string;

} // namespace planar::engine::planning
