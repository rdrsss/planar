/// @file scenario.cppm
/// @brief `planar.engine.planning.scenario` — test-scenario entity CRUD plus
/// the `ready` / `verify` / `retire` lifecycle transitions (plan 996 roadmap
/// M12 item 9, task 6195).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/scenario.zig:
///   - `create_scenario`, `show_scenario`, `list_scenarios` and
///     `list_scenarios_touching` against the `test_scenarios` table.
///   - `verify_scenario` / `ready_scenario` / `retire_scenario`.
///   - `render_text` / `render_json` / `render_list_text` /
///     `render_list_json`, per D-renderers-in-layer-2.
///
/// ## `verify` does NOT reach `test_spec_status`, and cannot
///
/// Task 6195's brief said "`scenario`'s distinguishing arm is `verify`,
/// which reaches `test_spec_status`", and asked which layer the call sits
/// at. VERIFIED FALSE before a line was written, by reading the caller and
/// then grepping the whole Zig tree: zig's `handlers/scenario/verify.zig`
/// calls `engine.planning.scenario.verify` and NOTHING else, and
/// `engine.planning.test_spec_status` has exactly three callers in the
/// tree — `handlers/test_spec/status.zig`, `engine/routing/packet.zig` and
/// `cmd/planar-execute/state.zig` (which shells the CLI). None of them is
/// on any `scenario` path.
///
/// The dependency runs the OTHER WAY and is data-level, not call-level:
/// `test_spec_status::compute` READS `test_scenarios` rows and the
/// `test_scenario -> task` `verifies` edges this module's rows participate
/// in. So porting `scenario` adds no edge in either direction and needs no
/// composition at any layer. Had the brief been right it would still have
/// been an intra-target call — `test_spec_status` lives in this same
/// `engine_planning` module — but it is not a call at all.
///
/// ## What this module does NOT carry, and why
///
/// - **The entity-create activity hook and the session stamp.** Unlike
///   `question add` and `decision add`, zig's `handlers/scenario/add.zig`
///   starts NO session and calls no
///   `agentactivity_store.recordEntityCreateAction`. `test_scenarios` has
///   no `session_id` column either. So there is nothing to compose at
///   layer 3 here — `scenario add` writes zero `sessions` rows, which is
///   asserted rather than assumed.
/// - **The cross-scope guard.** zig calls `policy.scope_guard.check(null,
///   null)` on every write path, a no-op with two nulls. Same carve-out
///   `plan.cppm` / `question.cppm` / `decision.cppm` document (task 6075 is
///   an unresolved OPERATOR decision).
///
/// ## The `--related` endpoint IS checked; the `--plan` endpoint is NOT
///
/// The two link-ish arguments on `create_scenario` behave OPPOSITELY, and
/// neither behaviour is this module's code:
///
///  - `related_artifact_id` is a real COLUMN carrying
///    `references artifacts(id)`, so a nonexistent artifact fails the
///    INSERT itself. Oracle-captured: `scenario add x --related 77` exits 1
///    with `scenario add: QueryFailed` and writes NO row — not even a
///    partial one, since the failure is the insert.
///  - `plan_id` becomes an `entity_links` row, and `entity_links` carries
///    no FK on its polymorphic column pair, so the schema refuses nothing.
///    The existence check is therefore done IN CODE, before the insert:
///    `scenario add x --plan 4242` exits 1 with `scenario add: NotFound`
///    and writes no row and no edge (task 6197). Until 6197 it exited 0 and
///    left a dangling edge — so this one verb answered its two reference
///    flags differently, for a structural reason rather than a policy one.
///
/// The edge's `from_kind` MUST be `test_scenario` (not `scenario`): the
/// oracle's editflow anchor resolver queries that spelling, and it is what
/// makes `scenario view` / `diff` / `review` resolvable in the oracle.
///
/// Scope handling mirrors `question.cppm`: a `scope` string is resolved
/// through layer-1 `planar.scope_ref` (D19), and "no scope at all" folds to
/// `global`.
module;

export module planar.engine.planning.scenario;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Which kind of scope a scenario belongs to. Mirrors zig's
/// scenario.zig `ScopeKind`.
export enum class scenario_scope_kind : std::uint8_t {
  repo,
  association,
  global,
};

/// @brief A scenario's lifecycle status. Mirrors zig's scenario.zig
/// `Status`. `retired` is the sole TERMINAL state.
export enum class scenario_status : std::uint8_t {
  draft,
  ready,
  verified,
  failing,
  retired,
};

/// @brief The result of one recorded test run. Mirrors zig's scenario.zig
/// `Outcome`. Stored in `test_scenarios.last_outcome`, which is nullable
/// and CHECK-constrained to exactly these four.
export enum class scenario_outcome : std::uint8_t {
  pass,
  fail,
  error_case, ///< The `error` outcome. Named `error_case` because `error`
              ///< is awkward as an enumerator; the wire form is `error`.
  skipped,
};

/// @brief Parse a `status` column value / `--status` token.
/// @param s The raw text to parse.
/// @return The parsed status, or unset for an unrecognized string.
export auto scenario_status_from_text(std::string_view s) -> std::optional<scenario_status>;

/// @brief Render `s` as the wire/column text form.
/// @param s The status to render.
/// @return The wire/column text form.
export auto scenario_status_to_text(scenario_status s) -> std::string_view;

/// @brief Parse a `last_outcome` column value / `--outcome` token.
/// @param s The raw text to parse.
/// @return The parsed outcome, or unset for an unrecognized string.
export auto scenario_outcome_from_text(std::string_view s) -> std::optional<scenario_outcome>;

/// @brief Render `o` as the wire/column text form. `error_case` renders as
/// the bare word `error`.
/// @param o The outcome to render.
/// @return The wire/column text form.
export auto scenario_outcome_to_text(scenario_outcome o) -> std::string_view;

/// @brief One row from the `test_scenarios` table. Mirrors zig's
/// scenario.zig `Scenario`, INCLUDING field ORDER — `render_json`
/// serializes in declaration order because the oracle's JSON path is
/// `std.json.Stringify.value` over this struct's Zig twin.
///
/// `body`, `last_run_at` and `last_outcome` are genuinely nullable columns
/// and stay optional so SQL NULL keeps rendering as `null`; `--body ""`
/// stores `''` and renders `""`, which is a DIFFERENT observable value.
///
/// The table also carries a `slug` column (added by a later migration) that
/// the oracle's projection does not select and no scenario verb reads or
/// writes. It is left out here for the same reason.
export struct scenario {
  std::int64_t                id;         ///< The row's id.
  scenario_scope_kind         scope_kind; ///< Which kind of scope this scenario belongs to.
  std::optional<std::int64_t> scope_id;   ///< The scope's row id, unset for global.
  std::string                 title;      ///< Display title.
  std::optional<std::string>  body;       ///< The scenario text, when there is one.
  scenario_status             status;     ///< Current lifecycle status.
  /// @brief The artifact this scenario verifies, when one was named. A real
  /// FK — see this file's header.
  std::optional<std::int64_t> related_artifact_id;
  /// @brief When the most recent run was recorded. Updated by EVERY
  /// `verify`, including a non-passing one that leaves `status` alone.
  std::optional<std::string> last_run_at;
  /// @brief The most recent run's result. Tracked INDEPENDENTLY of
  /// `status`: a `retired` scenario can carry `last_outcome = 'skipped'`,
  /// and `retire` never touches this column.
  std::optional<scenario_outcome> last_outcome;
  std::string                     created_at; ///< Row creation timestamp.
  std::string                     updated_at; ///< Row last-update timestamp.
};

/// @brief Arguments to `create_scenario`. Mirrors zig's scenario.zig
/// `CreateArgs`.
export struct scenario_create_args {
  std::string title; ///< The scenario's title (required).
  /// @brief The scenario text. Genuinely optional, unlike `decision`'s
  /// body: `test_scenarios.body` is nullable and `scenario add` has no
  /// `--body is required` refusal. Unset binds SQL NULL; `""` binds `''`.
  std::optional<std::string> body;
  /// @brief The artifact this scenario verifies. Checked by the COLUMN's
  /// foreign key, so a nonexistent id refuses the whole create — see this
  /// file's header.
  std::optional<std::int64_t> related_artifact_id;
  /// @brief Optional plan to link this scenario to through an
  /// `entity_links` (`test_scenario -> plan`, relationship `derives-from`)
  /// edge.
  ///
  /// CHECKED for existence before anything is written: a nonexistent id
  /// yields `scenario_error::not_found` and leaves no row, no audit row and
  /// no edge (task 6197). See this file's header for why the check is in
  /// code here and in the schema for `--related`.
  std::optional<std::int64_t> plan_id;
  std::optional<std::string>  scope; ///< Scope slug accepted by `planar.scope_ref::resolve`.
};

/// @brief Filter for `list_scenarios` / `list_scenarios_touching`. Mirrors
/// zig's scenario.zig `ListFilter`.
export struct scenario_list_filter {
  /// @brief Match any of these statuses. **EMPTY MEANS EVERY STATUS**, not
  /// a default subset: the oracle emits NO `status` predicate at all on the
  /// empty arm, so a bare `scenario list` returns `retired` rows alongside
  /// `draft` ones (captured). That is the OPPOSITE of `decision`, where
  /// unset means `{proposed, accepted}` and terminal rows are unreachable
  /// without naming them. Three sibling families, and `plan`'s
  /// empty-statuses arm meant "open" — reading this one off either of the
  /// others gives the wrong answer.
  ///
  /// It is a vector because `scenario list --status` IS comma-split
  /// (`--status draft,verified` returns both), matching `question list` and
  /// `plan list` and NOT `decision list`, whose identical-looking flag
  /// takes one token.
  std::vector<scenario_status> statuses;
  /// @brief Restrict to scenarios whose `related_artifact_id` is this.
  /// Unset applies no artifact predicate.
  std::optional<std::int64_t> related_artifact_id;
  /// @brief A scope slug, OR-ed with every member of `scopes`. Unused by
  /// the CLI — `scenario list`'s handler fills `scopes` on BOTH of its
  /// branches — but part of the ported filter, and OR-ed first when set.
  std::optional<std::string> scope;
  /// @brief Additional scope slugs, OR-ed with `scope`. An empty
  /// combination applies NO scope predicate at all.
  std::vector<std::string> scopes;
};

/// @brief Error surface for every fallible operation in this module.
export enum class scenario_error : std::uint8_t {
  not_found,         ///< No such scenario.
  unsupported_scope, ///< The scope slug's grammar is not one this build resolves.
  slug_not_found,    ///< The scope slug's grammar parsed but named no row.
  /// The transition is not in the matrix. Zig spelling: `IllegalTransition`
  /// — propagated out of `policy.status` UNFOLDED, unlike `decision`, which
  /// rewrites it into two of its own spellings. The operator sees the bare
  /// tag: `error: scenario verify: IllegalTransition`.
  illegal_transition,
  /// The stored status is not one the matrix recognizes. Zig spelling:
  /// `UnknownStatus`. Unreachable through the CLI — `test_scenarios.status`
  /// carries a CHECK constraint and `read_row` refuses an unparseable value
  /// as `query_failed` first — but the mapping is total.
  unknown_status,
  query_failed,       ///< A prepare/bind/step failed, or a stored enum column is unparseable.
  audit_write_failed, ///< The `audit_log` row could not be written. Zig spelling: `WriteFailed`.
};

/// @brief Create a new scenario. Status is always `draft` (the column's
/// default) — there is no create-time status argument.
///
/// Order of operations is observable: resolve the scope (an unresolvable
/// slug refuses BEFORE any write), check `--plan` exists (task 6197 — also
/// before any write), INSERT, `create` audit row, then the optional
/// `--plan` edge. The edge write does NOT get its
/// own `link` audit row — `scenario add --plan` writes exactly one
/// `audit_log` row and its verb is `create`, with summary `create scenario
/// '<title>'`.
/// @param conn An open, migrated database connection.
/// @param args The scenario's title (required) plus optional
/// body/related/plan/scope.
/// @return The created row, or `not_found` (the `--plan` id names no
/// `plans` row — checked in code, see this file's header),
/// `slug_not_found` / `unsupported_scope` (unresolvable `scope`),
/// `audit_write_failed`, or `query_failed` (which is also what a
/// nonexistent `--related` artifact produces, via the column's foreign
/// key).
export auto create_scenario(db::connection& conn, const scenario_create_args& args) -> std::expected<scenario, scenario_error>;

/// @brief Look up a scenario by id.
/// @param conn An open, migrated database connection.
/// @param id The scenario's row id.
/// @return The row, or `scenario_error::not_found`, or `query_failed`.
export auto show_scenario(db::connection& conn, std::int64_t id) -> std::expected<scenario, scenario_error>;

/// @brief List scenarios matching `filter`, ordered by id.
/// @param conn An open, migrated database connection.
/// @param filter Status/artifact/scope filters. See `scenario_list_filter`
/// for the empty-statuses default, which is EVERY status.
/// @return The matching rows, or `slug_not_found` / `unsupported_scope`
/// when a scope member does not resolve, or `query_failed`.
export auto list_scenarios(db::connection& conn, const scenario_list_filter& filter)
    -> std::expected<std::vector<scenario>, scenario_error>;

/// @brief List scenarios that either are scoped to `repo_id` directly OR
/// carry a `test_scenario -> repo` `touches` edge to it — the UNION behind
/// `scenario list --touches <repo-slug>`.
///
/// The two arms treat the scope predicate DIFFERENTLY, which is why this
/// cannot be a post-filter over `list_scenarios`:
///
///  - The DIRECT arm is all-or-nothing against the scope set. With no scope
///    set it stays on; with one, it survives only if some member is a
///    `repo` ref naming `repo_id`. Otherwise the arm is emitted as `1 = 0`
///    rather than dropped, keeping the UNION's column lists identical.
///  - The TOUCHES arm always runs, and the scope predicate is applied to it
///    normally.
///
/// So the same `--touches` slug returns DISJOINT sets under two different
/// scopes, which is the sharpest available proof that the scope filter
/// excludes. Oracle-captured against one fixture: `--touches oracle` from
/// inside the repo returns only the repo-scoped row, and `--touches oracle
/// --scope global` returns only the globally-scoped row that touches it.
/// Neither is a subset of the other and neither is a count.
/// @param conn An open, migrated database connection.
/// @param repo_id The `projects.id` to match, already resolved from a slug
/// by the caller.
/// @param filter The same filters `list_scenarios` takes.
/// @return The matching rows ordered by id, or the first failure.
export auto list_scenarios_touching(db::connection& conn, std::int64_t repo_id, const scenario_list_filter& filter)
    -> std::expected<std::vector<scenario>, scenario_error>;

/// @brief Record a test run, and on a PASS flip the scenario to `verified`.
///
/// `last_outcome` and `last_run_at` are written on EVERY path, including a
/// non-passing run — the run log is independent of the status. A `fail`,
/// `error` or `skipped` run leaves `status` exactly as it found it, so a
/// `draft` scenario that fails stays `draft` and a `retired` one that is
/// skipped stays `retired` (both captured).
///
/// ## The auto-transition, and the audit row that proves it happened
///
/// On a PASS from `draft`, this walks `draft -> ready` and then
/// `ready -> verified`, checking EACH hop against the shared matrix — the
/// matrix is honored step by step, not bypassed, and `draft -> verified` is
/// deliberately absent from it. The intermediate hop writes its OWN
/// `status_change` row, summary `ready: auto-transition via verify`, so a
/// draft-to-verified call leaves TWO audit rows where a ready-to-verified
/// call leaves one. That row is the only observable difference between the
/// two paths and is asserted directly.
///
/// ## A PASS is refused when the source cannot reach `verified`
///
/// `retired -> verified` is not an edge, so `scenario verify <retired>`
/// with the default `pass` outcome refuses as `illegal_transition` and
/// writes NOTHING — no status change, no run stamp, no audit row, and
/// `updated_at` unmoved. The same scenario accepts `--outcome skipped`,
/// because that path never consults the matrix. Both captured.
///
/// Re-verifying an ALREADY-verified scenario succeeds and re-stamps: the
/// identity short-circuit in `check_transition` means `verified ->
/// verified` never reaches the matrix.
/// @param conn An open, migrated database connection.
/// @param id The scenario's row id.
/// @param outcome The run's result.
/// @param summary Optional free text folded into the audit summary as
/// `verify(<outcome>): <summary>`; absent gives the bare `verify:
/// <outcome>`.
/// @return The updated row, or `not_found`, `illegal_transition`,
/// `unknown_status`, `audit_write_failed`, or `query_failed`.
export auto verify_scenario(db::connection& conn, std::int64_t id, scenario_outcome outcome,
                            std::optional<std::string_view> summary) -> std::expected<scenario, scenario_error>;

/// @brief Advance a scenario to `ready`.
///
/// Only `draft -> ready` is legal, so every other non-identity source
/// refuses as `illegal_transition`. Has NO CLI leaf — the scenario family
/// declares ten and `ready` is not among them; the transition is reachable
/// from the operator only as `verify`'s internal first hop. Ported anyway,
/// because it is part of the module under port and its absence would be a
/// silent gap.
/// @param conn An open, migrated database connection.
/// @param id The scenario's row id.
/// @param reason Optional free text folded into the audit summary as
/// `ready: <reason>`; absent gives the bare `ready`.
/// @return The updated row, or `not_found`, `illegal_transition`,
/// `unknown_status`, `audit_write_failed`, or `query_failed`.
export auto ready_scenario(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<scenario, scenario_error>;

/// @brief Retire a scenario: `status='retired'`.
///
/// Legal from EVERY other status, and from `retired` itself by the identity
/// short-circuit — re-retiring succeeds, bumps `updated_at` and writes a
/// second audit row. `last_outcome` and `last_run_at` are left exactly as
/// found: retirement says nothing about the last run, and a retired
/// scenario keeps whatever result it earned (captured).
/// @param conn An open, migrated database connection.
/// @param id The scenario's row id.
/// @param reason Optional free text folded into the audit summary as
/// `retire: <reason>`; absent gives the bare `retire`.
/// @return The updated row, or `not_found`, `illegal_transition`,
/// `unknown_status`, `audit_write_failed`, or `query_failed`.
export auto retire_scenario(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<scenario, scenario_error>;

/// @brief Render one scenario as the operator-facing key/value block.
///
/// Ports zig's `renderText` byte for byte. Every value starts at column 13
/// — the label-with-colon is padded to a width of TWELVE, matching
/// `decision`'s block but not `question`'s (11) or `plan`'s (10). FOUR
/// lines are CONDITIONAL and appear in this order between `scope:` and
/// `created:`: `artifact:`, `outcome:`, `last run:`, `body:` — each only
/// when its column is non-null. Note `body:` comes LAST of the four, AFTER
/// the run columns, where `decision`'s unconditional `body:` comes first.
/// The `last run:` label is the only two-word one in the family.
/// @param s The scenario to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const scenario& s) -> std::string;

/// @brief Render one scenario as the single-line JSON object.
///
/// Field order is the `scenario` struct's declaration order — see the
/// struct. `scope_id`, `body`, `related_artifact_id`, `last_run_at` and
/// `last_outcome` render as `null` when unset, and `""` is a distinct value
/// for `body`.
/// @param s The scenario to render.
/// @return The JSON object with NO trailing newline — a fragment the caller
/// terminates (the oracle's `output.emit` prints `"\n"` after stringifying).
export auto render_json(const scenario& s) -> std::string;

/// @brief Render a list of scenarios as the one-line-per-scenario table.
///
/// Ports zig's `renderListText` byte for byte, including the empty case:
/// an empty list renders the literal `"(no scenarios)\n"` — WITH
/// parentheses, matching `question`'s `(no questions)` and NOT
/// `decision`'s bare `no decisions`. Captured, not inherited.
///
/// Columns are `{:>5}  {:<10}  {:<8}  {}` — id right-aligned in five, then
/// status LEFT-aligned in ten, then the OUTCOME left-aligned in eight, then
/// the title unpadded, with two spaces between each pair. A null
/// `last_outcome` renders as a single `-`, still padded to eight; this is
/// the family's only three-column-plus-title list, so a copy from
/// `decision`'s `{:>5}  {:<10}  {}` silently drops the outcome.
/// @param items The scenarios to render, in the order given.
/// @return The complete block, INCLUDING the trailing newline on its last
/// line. The caller writes it verbatim and appends nothing.
export auto render_list_text(std::span<const scenario> items) -> std::string;

/// @brief Render a list of scenarios as the single-line JSON array.
///
/// Each element is exactly `render_json`'s object; the empty list renders
/// `[]`. Mirrors the oracle's `output.emitList`.
/// @param items The scenarios to render, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates, same contract as `render_json`.
export auto render_list_json(std::span<const scenario> items) -> std::string;

} // namespace planar::engine::planning
