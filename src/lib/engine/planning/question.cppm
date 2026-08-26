/// @file question.cppm
/// @brief `planar.engine.planning.question` — Question entity CRUD plus the
/// `answer` / `wontfix` terminal transitions (plan 996 roadmap M12 item 4,
/// task 6188).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/question.zig:
///   - `create_question`, `show_question`, `list_questions`,
///     `list_questions_touching` against the `questions` table.
///   - `answer_question` / `wontfix_question`, the two operator transitions.
///   - `render_text` / `render_json` / `render_list_text` /
///     `render_list_json`, per D-renderers-in-layer-2.
///
/// ## What this module does NOT carry, and why
///
/// - **The entity-create activity hook.** zig's `create` takes a
///   `session_id` and calls `agentactivity_store.recordEntityCreateAction`
///   after the INSERT commits. `engine.runtime.agentactivity` is LAYER 2,
///   same as this module, and `cmake/architecture.cmake` FATALs on a
///   same-layer edge (D15/D18). It is therefore composed at LAYER 3 by the
///   `question add` handler — the same D20 shape `annotate add` and
///   `unlink` already use for `has_active_claim_on_task`. The hook is
///   best-effort in the original (its failure is logged, never returned),
///   so moving it out of the engine changes no return value.
/// - **The cross-scope guard.** zig calls `policy.scope_guard.check(null,
///   null)` on every write path here, which is a no-op with two nulls. The
///   guard stays exactly as opt-in as `engine_identity` exposes it — task
///   6075 is an unresolved OPERATOR decision on whether it should apply to
///   every mutation verb, and wiring it in here would silently pick a side
///   (the identical carve-out `plan.cppm` documents).
///
/// Scope handling mirrors `plan.cppm`: a `scope` string is resolved through
/// layer-1 `planar.scope_ref` (D19), and "no scope at all" folds to
/// `global`.
module;

export module planar.engine.planning.question;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Which kind of scope a question belongs to. Mirrors zig's
/// question.zig `ScopeKind`.
export enum class question_scope_kind : std::uint8_t {
  repo,
  association,
  global,
};

/// @brief A question's lifecycle status. Mirrors zig's question.zig
/// `Status`. Both non-`open` members are TERMINAL — there is no
/// `question reopen` verb.
export enum class question_status : std::uint8_t {
  open,
  answered,
  wontfix,
};

/// @brief Parse a `status` column value / `--status` token.
/// @param s The raw text to parse.
/// @return The parsed status, or unset for an unrecognized string.
export auto question_status_from_text(std::string_view s) -> std::optional<question_status>;

/// @brief Render `s` as the wire/column text form.
/// @param s The status to render.
/// @return The wire/column text form.
export auto question_status_to_text(question_status s) -> std::string_view;

/// @brief One row from the `questions` table. Mirrors zig's question.zig
/// `Question`, INCLUDING field ORDER — `render_json` serializes in
/// declaration order because the oracle's JSON path is
/// `std.json.Stringify.value` over this struct's Zig twin.
export struct question {
  std::int64_t                id;          ///< The row's id.
  question_scope_kind         scope_kind;  ///< Which kind of scope this question belongs to.
  std::optional<std::int64_t> scope_id;    ///< The scope's row id, unset for global.
  std::string                 title;       ///< Display title.
  std::optional<std::string>  body;        ///< Optional free-text body.
  question_status             status;      ///< Current lifecycle status.
  std::optional<std::string>  answer_body; ///< The recorded answer; set iff `status == answered`.
  std::optional<std::string>  answered_at; ///< When the answer landed; set iff `status == answered`.
  std::string                 created_at;  ///< Row creation timestamp.
  std::string                 updated_at;  ///< Row last-update timestamp.
};

/// @brief Arguments to `create_question`. Mirrors zig's question.zig
/// `CreateArgs`, minus `session_id` (see this file's header).
export struct question_create_args {
  std::string                title; ///< The question's title (required).
  std::optional<std::string> body;  ///< Optional free-text body.
  std::optional<std::string> scope; ///< Scope slug accepted by `planar.scope_ref::resolve`.
  /// @brief Optional plan to link this question to through an
  /// `entity_links` (`question -> plan`, relationship `derives-from`) edge.
  /// A non-existent plan id is `question_error::not_found`, refused BEFORE
  /// the INSERT — the oracle checks it first and writes nothing.
  std::optional<std::int64_t> plan_id;
};

/// @brief Filter for `list_questions` / `list_questions_touching`. Mirrors
/// zig's question.zig `ListFilter`.
export struct question_list_filter {
  /// @brief Match any of these statuses. **EMPTY IS NOT "EVERY STATUS":**
  /// it means `open` alone. The oracle emits a literal
  /// `and status = 'open'` on the empty arm, so a caller that wants
  /// answered/wontfix rows must name them. Reading empty as "no predicate"
  /// is the same defect class that let `plan recompute-status --all`
  /// resurrect terminal plans.
  std::vector<question_status> statuses;
  /// @brief A scope slug, OR-ed with every member of `scopes`.
  std::optional<std::string> scope;
  /// @brief Additional scope slugs, OR-ed with `scope`. An empty
  /// combination applies NO scope predicate at all.
  std::vector<std::string> scopes;
  /// @brief Restrict to questions carrying a `derives-from` edge to this
  /// plan. Unset applies no plan predicate.
  std::optional<std::int64_t> plan_id;
};

/// @brief Error surface for every fallible operation in this module.
export enum class question_error : std::uint8_t {
  not_found,          ///< No such question (or, on create, no such `--plan`).
  unsupported_scope,  ///< The scope slug's grammar is not one this build resolves.
  slug_not_found,     ///< The scope slug's grammar parsed but named no row.
  query_failed,       ///< A prepare/bind/step failed, or a stored enum column is unparseable.
  answer_required,    ///< `answer_question` was given an empty answer.
  illegal_transition, ///< The status move is not in the legal edge set.
  audit_write_failed, ///< The `audit_log` row could not be written. Zig spelling: `WriteFailed`.
};

/// @brief Create a new question. Status is always `open` (the column's
/// default) — there is no create-time status argument.
///
/// Order of operations is the oracle's and is observable: `--plan` is
/// validated FIRST, so a dangling `--plan` leaves no `questions` row and no
/// `audit_log` row; then the INSERT; then the `create` audit row; then the
/// `derives-from` edge. The edge write does NOT get its own `link` audit
/// row (oracle-confirmed: `question add --plan 1` writes exactly one
/// `audit_log` row, verb `create`).
/// @param conn An open, migrated database connection.
/// @param args The question's title (required) plus optional body/scope/plan.
/// @return The created row, or `not_found` (dangling `--plan`),
/// `slug_not_found` / `unsupported_scope` (unresolvable `scope`),
/// `audit_write_failed`, or `query_failed`.
export auto create_question(db::connection& conn, const question_create_args& args) -> std::expected<question, question_error>;

/// @brief Look up a question by id.
/// @param conn An open, migrated database connection.
/// @param id The question's row id.
/// @return The row, or `question_error::not_found`, or `query_failed`.
export auto show_question(db::connection& conn, std::int64_t id) -> std::expected<question, question_error>;

/// @brief List questions matching `filter`, ordered by id.
/// @param conn An open, migrated database connection.
/// @param filter Status/scope/plan filters. See `question_list_filter` for
/// the empty-status default, which is `open` and NOT "everything".
/// @return The matching rows, or `slug_not_found` / `unsupported_scope`
/// when a scope member does not resolve, or `query_failed`.
export auto list_questions(db::connection& conn, const question_list_filter& filter)
    -> std::expected<std::vector<question>, question_error>;

/// @brief List questions that are EITHER scoped directly to `repo_id` OR
/// linked to it by `entity_links(relationship='touches')`, ordered by id.
///
/// Serves `question list --touches <repo-slug>`.
///
/// ## The two branches are filtered DIFFERENTLY, and that is the contract
///
/// The query is a UNION of a direct-scope branch and a touches-edge branch.
/// `filter.scope` / `filter.scopes` apply INSIDE the touches branch only.
/// They do not narrow the direct branch row-by-row; instead they switch that
/// whole branch ON or OFF, and it is on only when the scope set contains a
/// `repo` ref that is either unresolved-id or `repo_id` itself. A scope set
/// naming only `global`, for instance, suppresses every directly-repo-scoped
/// question while still admitting global questions reached through a touches
/// edge.
///
/// That asymmetry is the oracle's (`question.zig`'s `listTouching`, whose own
/// unit test "listTouching suppresses direct-repo branch when scope excludes
/// repo" pins it). Applying the scope predicate uniformly to both branches
/// would be tidier and would silently change which rows an operator sees —
/// `question.t.cpp`'s three-way discrimination case exists to make an
/// inert filter and a uniformly-applied one BOTH fail.
///
/// `filter.plan_id` is deliberately NOT applied here: the oracle's
/// `listTouching` ignores it entirely (only `list` carries the
/// `derives-from` subquery). Adding it would be an improvement, and D2 says
/// reproduce.
///
/// The empty-status default is `open`, emitted independently in EACH branch.
/// @param conn An open, migrated database connection.
/// @param repo_id The repo (`projects.id`) to filter on.
/// @param filter Status/scope filters, applied as described above.
/// @return The matching rows ordered by id, `slug_not_found` /
/// `unsupported_scope` when a scope member does not resolve, or
/// `query_failed`.
export auto list_questions_touching(db::connection& conn, std::int64_t repo_id, const question_list_filter& filter)
    -> std::expected<std::vector<question>, question_error>;

/// @brief Answer a question: `status='answered'`, `answer_body` and
/// `answered_at` written in the SAME statement.
///
/// The atomicity is a schema requirement, not a nicety: `questions` carries
/// a row-level CHECK that `status='answered'` implies both columns
/// non-null, so a split write is refused by SQLite. `answer` must be
/// non-empty (`answer_required`) — an empty answer would satisfy the CHECK
/// while recording nothing.
///
/// Re-answering an ALREADY-answered question SUCCEEDS and overwrites both
/// columns. That is not an oversight: `check_transition` short-circuits on
/// `from == to`, so `answered -> answered` never reaches the question
/// matrix. Oracle-confirmed by running `question answer` twice.
/// `wontfix -> answered` IS refused (`illegal_transition`).
/// @param conn An open, migrated database connection.
/// @param id The question's row id.
/// @param answer The answer text; must be non-empty.
/// @return The updated row, or `not_found`, `answer_required`,
/// `illegal_transition`, `audit_write_failed`, or `query_failed`.
export auto answer_question(db::connection& conn, std::int64_t id, std::string_view answer)
    -> std::expected<question, question_error>;

/// @brief Mark a question `wontfix`.
///
/// `reason` lands in the `audit_log` summary ONLY — `questions` has no
/// column for it, so the reason is unrecoverable from the row itself. The
/// summary is `wontfix: <reason>` when a reason is given and the bare word
/// `wontfix` when it is not; both were captured from the oracle.
///
/// As with `answer_question`, `wontfix -> wontfix` succeeds by the identity
/// short-circuit (and rewrites `updated_at`), while `answered -> wontfix` is
/// `illegal_transition`.
/// @param conn An open, migrated database connection.
/// @param id The question's row id.
/// @param reason Optional reason, recorded in the audit summary only.
/// @return The updated row, or `not_found`, `illegal_transition`,
/// `audit_write_failed`, or `query_failed`.
export auto wontfix_question(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<question, question_error>;

/// @brief Render one question as the operator-facing key/value block.
///
/// Ports zig's `renderText` byte for byte. Every value starts at column 12
/// — the label-with-colon occupies a left-justified field of ten followed
/// by one space, so `id:` is trailed by EIGHT spaces. `plan`'s block is one
/// column narrower (`id:` + seven), so a copy-paste from `plan.cpp` shifts
/// every line of this one. Three lines are CONDITIONAL and appear in
/// this order between `scope:` and `created:`: `body:`, `answer:`,
/// `answered:`, each only when its column is non-null. The `scope:` value
/// is the scope kind with `:<id>` appended when `scope_id` is set.
/// @param q The question to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const question& q) -> std::string;

/// @brief Render one question as the single-line JSON object.
///
/// Field order is the `question` struct's declaration order — see the
/// struct. `scope_id`, `body`, `answer_body` and `answered_at` render as
/// `null` when unset, which is how a caller distinguishes SQL NULL from the
/// empty string (`""`).
/// @param q The question to render.
/// @return The JSON object with NO trailing newline — a fragment the caller
/// terminates (the oracle's `output.emit` prints `"\n"` after stringifying).
export auto render_json(const question& q) -> std::string;

/// @brief Render a list of questions as the one-line-per-question table.
///
/// Ports zig's `renderListText` byte for byte, including the empty case: an
/// empty list renders the literal `"(no questions)\n"`, NOT an empty string
/// and NOT `(no plans)`. Columns are `{:>5}  {:<10}  {}` — id right-aligned
/// in five, then status LEFT-aligned and space-padded to ten, then the
/// title unpadded. There is no slug column (a question has no slug), so
/// this is a THREE-column table where `plan`'s is four.
/// @param items The questions to render, in the order given.
/// @return The complete block, INCLUDING the trailing newline on its last
/// line. The caller writes it verbatim and appends nothing.
export auto render_list_text(std::span<const question> items) -> std::string;

/// @brief Render a list of questions as the single-line JSON array.
///
/// Each element is exactly `render_json`'s object; the empty list renders
/// `[]`. Mirrors the oracle's `output.emitList`.
/// @param items The questions to render, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates, same contract as `render_json`.
export auto render_list_json(std::span<const question> items) -> std::string;

} // namespace planar::engine::planning
