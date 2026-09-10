/// @file decision.cppm
/// @brief `planar.engine.planning.decision` — Decision entity CRUD plus the
/// `accept` / `withdraw` / `supersede` lifecycle transitions (plan 996
/// roadmap M12 item 8, task 6194).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/decision.zig:
///   - `create_decision`, `show_decision`, `list_decisions` against the
///     `decisions` table.
///   - `accept_decision` / `withdraw_decision` / `supersede_decision`, the
///     three operator transitions.
///   - `decisions_for_task`, a reader with no CLI leaf (see below).
///   - `render_text` / `render_json` / `render_list_text` /
///     `render_list_json`, per D-renderers-in-layer-2.
///
/// ## What this module does NOT carry, and why
///
/// - **The entity-create activity hook.** zig's `create` takes a
///   `session_id` and calls `agentactivity_store.recordEntityCreateAction`
///   after the INSERT commits. `engine.runtime.agentactivity` is LAYER 2,
///   same as this module, and `cmake/architecture.cmake` FATALs on a
///   same-layer edge (D15/D18). It is composed at LAYER 3 by the `decision
///   add` handler, the same shape `question add` uses. Note that
///   `decisions` ALSO has a `session_id` COLUMN, which this module does
///   write — the column and the hook are different things and only the hook
///   moves out.
/// - **The cross-scope guard.** zig calls `policy.scope_guard.check(null,
///   null)` on every write path here, which is a no-op with two nulls. Same
///   carve-out `plan.cppm` and `question.cppm` document (task 6075 is an
///   unresolved OPERATOR decision).
///
/// ## `supersede` writes `entity_links` DIRECTLY, and must
///
/// Task 6194's brief said to compose the newly-landed
/// `planar.engine.entitylink::add` here rather than writing the edge by
/// hand. That is not possible AND would not be correct, for two
/// INDEPENDENT reasons — either one alone settles it:
///
///  1. `engine_entitylink` is LAYER 2, exactly like this module.
///     `cmake/architecture.cmake` FATALs at configure time on an
///     `engine_* -> engine_*` edge, so the composition cannot even be
///     declared. Composing it at layer 3 instead is not available either:
///     the status UPDATE and the edge INSERT are ONE transaction whose
///     rollback is observable (see `supersede_decision`), and splitting
///     them across the engine/handler boundary would lose that.
///  2. It would change observable rows. `entitylink::add` writes a
///     `link|entity_link|<id>` `audit_log` row and performs an endpoint
///     existence check; the oracle's `supersede` does NEITHER. Captured:
///     after `decision supersede 1 --by 3` the `audit_log` holds exactly
///     one new row, `status_change|decision|1|supersede: …`, and no `link`
///     row at all. `decision link` on the same pair DOES write one. The two
///     paths genuinely differ and D2 says reproduce.
///
/// The `decision_link` LEAF does compose `engine_entitylink`, at layer 3,
/// through the shared `entity_link_verb` — that is where the composition
/// belongs and where it already lives.
///
/// ## `decisions_for_task` traverses `sessions`, NOT `entity_links`
///
/// The same brief described `for-task` as "the traversal that reads those
/// links". It does not: zig's `forTask` LEFT JOINs `sessions` on
/// `decisions.session_id` and matches `sessions.task_id`, OR-ed with
/// `scope_kind = 'global'`. No `entity_links` row is consulted on any path.
/// It also has NO CLI leaf and NO caller anywhere in the Zig tree (verified
/// by grep over `zig/src`) — `decision` declares eleven leaves and
/// `for-task` is not among them. It is ported anyway, because it is part of
/// the module under port and its absence would be a silent gap, but callers
/// should know it is currently reachable only from tests.
///
/// Scope handling mirrors `plan.cppm` / `question.cppm`: a `scope` string is
/// resolved through layer-1 `planar.scope_ref` (D19), and "no scope at all"
/// folds to `global`.
module;

export module planar.engine.planning.decision;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Which kind of scope a decision belongs to. Mirrors zig's
/// decision.zig `ScopeKind`.
export enum class decision_scope_kind : std::uint8_t {
  repo,
  association,
  global,
};

/// @brief A decision's lifecycle status. Mirrors zig's decision.zig
/// `Status`. `superseded` and `withdrawn` are TERMINAL.
export enum class decision_status : std::uint8_t {
  proposed,
  accepted,
  superseded,
  withdrawn,
};

/// @brief Parse a `status` column value / `--status` token.
/// @param s The raw text to parse.
/// @return The parsed status, or unset for an unrecognized string.
export auto decision_status_from_text(std::string_view s) -> std::optional<decision_status>;

/// @brief Render `s` as the wire/column text form.
/// @param s The status to render.
/// @return The wire/column text form.
export auto decision_status_to_text(decision_status s) -> std::string_view;

/// @brief Whether `s` is one of the two terminal statuses (`superseded`,
/// `withdrawn`). Mirrors zig's `Status.isTerminal`, which is what decides
/// between the `terminal_status` and `invalid_status` error spellings.
/// @param s The status to classify.
/// @return True for `superseded` and `withdrawn`.
export auto decision_status_is_terminal(decision_status s) -> bool;

/// @brief One row from the `decisions` table. Mirrors zig's decision.zig
/// `Decision`, INCLUDING field ORDER — `render_json` serializes in
/// declaration order because the oracle's JSON path is
/// `std.json.Stringify.value` over this struct's Zig twin.
///
/// `body` is a plain `std::string`, not an optional, because
/// `decisions.body` is `not null`. The empty string is a legal value and
/// the engine never synthesizes a placeholder — `render_json` emits `""`
/// for it, never `null`. `rationale`, `decided_at` and `session_id` ARE
/// genuinely nullable and stay optional so SQL NULL keeps rendering as
/// `null`.
export struct decision {
  std::int64_t                id;         ///< The row's id.
  decision_scope_kind         scope_kind; ///< Which kind of scope this decision belongs to.
  std::optional<std::int64_t> scope_id;   ///< The scope's row id, unset for global.
  std::string                 title;      ///< Display title.
  std::string                 body;       ///< The decision text. NOT NULL; may be empty.
  std::optional<std::string>  rationale;  ///< Optional free-text rationale.
  decision_status             status;     ///< Current lifecycle status.
  /// @brief When the decision was accepted. Written by `accept` ALONE —
  /// `withdraw` and `supersede` leave it exactly as they found it, so an
  /// accepted-then-withdrawn decision keeps its stamp.
  std::optional<std::string>  decided_at;
  std::optional<std::int64_t> session_id; ///< The session that recorded it, when there was one.
  std::string                 created_at; ///< Row creation timestamp.
  std::string                 updated_at; ///< Row last-update timestamp.
};

/// @brief Arguments to `create_decision`. Mirrors zig's decision.zig
/// `CreateArgs`.
export struct decision_create_args {
  std::string title; ///< The decision's title (required).
  /// @brief The decision text. Required by the COLUMN (`not null`), and the
  /// engine does not synthesize a placeholder; the empty string is accepted
  /// here and the `--body is required` refusal is the HANDLER's.
  std::string                body;
  std::optional<std::string> rationale; ///< Optional free-text rationale.
  /// @brief The session to stamp on the row's `session_id` column. Distinct
  /// from the layer-3 activity hook, which takes the same value.
  std::optional<std::int64_t> session_id;
  /// @brief Optional plan to link this decision to through an
  /// `entity_links` (`decision -> plan`, relationship `derives-from`) edge.
  ///
  /// The plan is CHECKED for existence before anything is written, so a
  /// nonexistent id yields `decision_error::not_found` and writes no
  /// `decisions` row, no `audit_log` row and no edge (task 6197). The check
  /// is in code because `entity_links` carries no FK on its polymorphic
  /// column pair — nothing in the schema would refuse the edge. Until 6197
  /// this verb accepted a dangling id at exit 0 where `question add --plan`
  /// refused; all four `--plan`-carrying create verbs now agree. The edge
  /// gets no `link` audit row, matching `question`.
  std::optional<std::int64_t> plan_id;
  std::optional<std::string>  scope; ///< Scope slug accepted by `planar.scope_ref::resolve`.
};

/// @brief Filter for `list_decisions`. Mirrors zig's decision.zig
/// `ListFilter`.
export struct decision_list_filter {
  /// @brief Match this ONE status. **UNSET IS NOT "EVERY STATUS":** it
  /// means `proposed` and `accepted` — the open lifecycle states. The
  /// oracle emits a literal `and status in ('proposed','accepted')` on the
  /// unset arm, so there is NO filter value that lists every decision;
  /// terminal rows are reachable only by naming `superseded` or
  /// `withdrawn` explicitly. Reading unset as "no predicate" is the same
  /// defect class that let `plan recompute-status --all` resurrect
  /// terminal plans.
  ///
  /// It is a single `std::optional`, not a vector: `decision list --status`
  /// takes ONE token and is NOT comma-split. `--status proposed,accepted`
  /// is refused as `unknown status 'proposed,accepted'` (oracle-captured),
  /// where the same flag on `question list` and `plan list` IS a CSV. The
  /// three verbs genuinely disagree.
  std::optional<decision_status> status;
  /// @brief A scope slug, OR-ed with every member of `scopes`. Also NOT
  /// comma-split: `--scope global,repo:foo` is one slug and fails
  /// `slug_not_found` (oracle-captured).
  std::optional<std::string> scope;
  /// @brief Additional scope slugs, OR-ed with `scope`. An empty
  /// combination applies NO scope predicate at all.
  std::vector<std::string> scopes;
  /// @brief Restrict to decisions carrying a `derives-from` edge to this
  /// plan. Unset applies no plan predicate.
  std::optional<std::int64_t> plan_id;
};

/// @brief Error surface for every fallible operation in this module.
export enum class decision_error : std::uint8_t {
  not_found,         ///< No such decision.
  unsupported_scope, ///< The scope slug's grammar is not one this build resolves.
  slug_not_found,    ///< The scope slug's grammar parsed but named no row.
  /// The transition's SOURCE is terminal (`superseded` / `withdrawn`).
  /// Zig spelling: `TerminalStatus`.
  terminal_status,
  /// The transition is refused for a NON-terminal source, or the stored
  /// status is unrecognized. Zig spelling: `InvalidStatus`.
  ///
  /// @warning Unreachable through today's CLI, and that is a property of
  /// the matrix rather than of the code: every move out of `proposed` is
  /// legal, both moves out of `accepted` are legal, and an identity move
  /// short-circuits before the matrix is consulted. The `unknown_status`
  /// path cannot fire either, because `decisions.status` carries a CHECK
  /// constraint and `read_row` rejects an unparseable value as
  /// `query_failed` before any transition is validated. It exists so the
  /// mapping is total, not because a caller can provoke it.
  invalid_status,
  query_failed,       ///< A prepare/bind/step failed, or a stored enum column is unparseable.
  link_exists,        ///< `supersede` found the `supersedes` edge already recorded.
  audit_write_failed, ///< The `audit_log` row could not be written. Zig spelling: `WriteFailed`.
};

/// @brief Create a new decision. Status is always `proposed` (written
/// literally in the INSERT) — there is no create-time status argument.
///
/// Order of operations is the oracle's and is observable: resolve the scope
/// (an unresolvable slug refuses BEFORE any write), INSERT, `create` audit
/// row, then the optional `--plan` edge. The edge write does NOT get its
/// own `link` audit row — `decision add --plan` writes exactly one
/// `audit_log` row and its verb is `create`, with summary `create decision
/// '<title>'`.
/// @param conn An open, migrated database connection.
/// @param args The decision's title and body (required) plus optional
/// rationale/session/plan/scope.
/// @return The created row, or `not_found` (the `--plan` id names no
/// `plans` row — see `decision_create_args::plan_id`), `slug_not_found` /
/// `unsupported_scope` (unresolvable `scope`), `audit_write_failed`, or
/// `query_failed`.
export auto create_decision(db::connection& conn, const decision_create_args& args) -> std::expected<decision, decision_error>;

/// @brief Look up a decision by id.
/// @param conn An open, migrated database connection.
/// @param id The decision's row id.
/// @return The row, or `decision_error::not_found`, or `query_failed`.
export auto show_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error>;

/// @brief List decisions matching `filter`, ordered by id.
/// @param conn An open, migrated database connection.
/// @param filter Status/scope/plan filters. See `decision_list_filter` for
/// the unset-status default, which is `{proposed, accepted}` and NOT
/// "everything".
/// @return The matching rows, or `slug_not_found` / `unsupported_scope`
/// when a scope member does not resolve, or `query_failed`.
export auto list_decisions(db::connection& conn, const decision_list_filter& filter)
    -> std::expected<std::vector<decision>, decision_error>;

/// @brief Accept a decision: `status='accepted'`, and `decided_at` stamped
/// in the SAME statement.
///
/// Re-accepting an ALREADY-accepted decision SUCCEEDS, RE-STAMPS
/// `decided_at`, and writes a second `status_change` audit row — the
/// identity short-circuit in `check_transition` means `accepted ->
/// accepted` never reaches the decision matrix. Oracle-confirmed by running
/// `decision accept` twice. A terminal source is refused as
/// `terminal_status`.
/// @param conn An open, migrated database connection.
/// @param id The decision's row id.
/// @return The updated row, or `not_found`, `terminal_status`,
/// `audit_write_failed`, or `query_failed`.
export auto accept_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error>;

/// @brief Withdraw a decision: `status='withdrawn'`.
///
/// Does NOT touch `decided_at` — an accepted-then-withdrawn decision keeps
/// the stamp it earned, which is visible as a `decided:` line on a
/// `withdrawn` row (oracle-captured). `withdrawn -> withdrawn` succeeds by
/// the same identity short-circuit `accept` has, bumping `updated_at` and
/// writing another audit row even though the status is terminal;
/// `accepted -> withdrawn` is legal, and `superseded -> withdrawn` is
/// `terminal_status`.
/// @param conn An open, migrated database connection.
/// @param id The decision's row id.
/// @return The updated row, or `not_found`, `terminal_status`,
/// `audit_write_failed`, or `query_failed`.
export auto withdraw_decision(db::connection& conn, std::int64_t id) -> std::expected<decision, decision_error>;

/// @brief Supersede `old_id` with `new_id`: flip the old row to
/// `superseded` AND record a `decision:<new> supersedes decision:<old>`
/// `entity_links` edge, atomically.
///
/// `new_id` is verified to EXIST before any write, and its own status is
/// never consulted or changed — superseding by a withdrawn decision is
/// permitted.
///
/// ## `terminal_status` here fires ONLY for a WITHDRAWN old decision
///
/// An already-SUPERSEDED decision can be superseded AGAIN, by a different
/// decision, and accumulates one `supersedes` edge per superseding
/// decision. The target status is `superseded`, so a `superseded` source is
/// an IDENTITY move and short-circuits before the matrix is consulted;
/// `withdrawn -> superseded` is a real edge the matrix refuses. Reading
/// "terminal source is refused" off the matrix alone predicts the wrong
/// answer for half the cases, which is exactly what a first draft of
/// decision.t.cpp did — oracle-confirmed by running `decision supersede 3
/// --by 2` followed by `decision supersede 3 --by 4`, both exit 0.
///
/// The two writes share one transaction and the rollback is OBSERVABLE:
/// when the edge already exists the UNIQUE constraint fires, the status
/// UPDATE is rolled back, and the old decision is left at its ORIGINAL
/// status with its ORIGINAL `updated_at` and no audit row. Captured by
/// creating the edge through `decision link` first and then calling
/// `decision supersede` — the row stayed `proposed`. A port that let the
/// UPDATE stand would leave a decision marked superseded by nothing.
///
/// See this file's header for why the edge is written directly rather than
/// through `planar.engine.entitylink`.
/// @param conn An open, migrated database connection.
/// @param old_id The decision being superseded.
/// @param new_id The decision superseding it.
/// @return The updated OLD row, or `not_found` (either id),
/// `terminal_status`, `link_exists`, `audit_write_failed`, or
/// `query_failed`.
export auto supersede_decision(db::connection& conn, std::int64_t old_id, std::int64_t new_id)
    -> std::expected<decision, decision_error>;

/// @brief Decisions recorded by a session attached to `task_id`, UNION-ed
/// with every `global`-scoped decision, ordered by id.
///
/// The `global` arm is unconditional: a global decision is returned for
/// EVERY task id, including one that does not exist. That is the oracle's
/// `where s.task_id = ? or d.scope_kind = 'global'` and it is the reason
/// this reader cannot be used as a membership test.
///
/// Has no CLI leaf and no caller in the Zig tree — see this file's header.
/// @param conn An open, migrated database connection.
/// @param task_id The task whose sessions' decisions to collect.
/// @return The matching rows ordered by id, or `query_failed`.
export auto decisions_for_task(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<decision>, decision_error>;

/// @brief Render one decision as the operator-facing key/value block.
///
/// Ports zig's `renderText` byte for byte. Every value starts at column 13
/// — the label-with-colon is padded to a width of TWELVE. That is one wider
/// than `question`'s block and two wider than `plan`'s, so a copy-paste
/// from either shifts every line of this one. Four lines are CONDITIONAL
/// and appear in this order between `body:` and `created:`: `rationale:`,
/// `decided:`, `session:`, each only when its column is non-null. Note that
/// `body:` is UNCONDITIONAL — the column is `not null`, so an empty body
/// renders as a `body:` label with nothing after it.
/// @param d The decision to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const decision& d) -> std::string;

/// @brief Render one decision as the single-line JSON object.
///
/// Field order is the `decision` struct's declaration order — see the
/// struct. `scope_id`, `rationale`, `decided_at` and `session_id` render as
/// `null` when unset. `body` NEVER renders as `null` (the column is `not
/// null`); an absent body is `""`.
/// @param d The decision to render.
/// @return The JSON object with NO trailing newline — a fragment the caller
/// terminates (the oracle's `output.emit` prints `"\n"` after stringifying).
export auto render_json(const decision& d) -> std::string;

/// @brief Render a list of decisions as the one-line-per-decision table.
///
/// Ports zig's `renderListText` byte for byte, including the empty case:
/// an empty list renders the literal `"no decisions\n"` — with NO
/// parentheses, where `question`'s is `"(no questions)\n"` and `plan`'s is
/// `"(no plans)\n"`. Two sibling families, three spellings; this one was
/// captured, not assumed. Columns are `{:>5}  {:<10}  {}` — id
/// right-aligned in five, then status LEFT-aligned and space-padded to ten,
/// then the title unpadded.
/// @param items The decisions to render, in the order given.
/// @return The complete block, INCLUDING the trailing newline on its last
/// line. The caller writes it verbatim and appends nothing.
export auto render_list_text(std::span<const decision> items) -> std::string;

/// @brief Render a list of decisions as the single-line JSON array.
///
/// Each element is exactly `render_json`'s object; the empty list renders
/// `[]`. Mirrors the oracle's `output.emitList`.
/// @param items The decisions to render, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates, same contract as `render_json`.
export auto render_list_json(std::span<const decision> items) -> std::string;

} // namespace planar::engine::planning
