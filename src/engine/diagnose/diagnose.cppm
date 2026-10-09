/// @file diagnose.cppm
/// @brief `planar.engine.diagnose` -- the read-only diagnosis framework behind
/// `planar-watch diagnose` and its later callers (plan 1132, tech spec 689).
///
/// Entry points: `run()` evaluates a `catalog` of checks over a plan scope and
/// window at an injected instant and returns a `diagnosis`; `render_text()` and
/// `render_json()` (schema `planar.diagnose/1`) print it; `builtin_catalog()`
/// is the shipped catalog. A check is a `check_def`: a stable id, kind,
/// severity, category, recovery hint, the inputs it needs, a `built` flag and a
/// function over a `check_context`. Adding a check is appending one
/// `check_def` (and any `input_def` it needs) to its family's factory, one
/// `checks_<family>.cpp` per family, which `builtin_catalog()` concatenates.
///
/// Reads only. Every read runs in one deferred read transaction on a
/// connection whose busy timeout is lowered to 250 ms for the run and restored
/// afterwards. Every query filters before any `LIMIT`; this module never calls
/// `planar-watch`'s `list_actions` or any other limit-before-filter helper (the
/// defect behind task 7333). The module imports only `db`, `incident_model`,
/// `json_text` and `core`: no other engine bucket and no `cmd` module.
///
/// Error-boundary contract. `run()` returns `std::expected<diagnosis,
/// run_error>`; the error is bad caller input only (an unknown check or plan, an
/// invalid instant or day count). A database that is busy past 250 ms, fails a
/// query or lacks the planning tables yields a `diagnosis` whose `result` is
/// `run_outcome::unavailable` with an `unavailable_reason`, not an error; what
/// exit status that earns is the caller's decision. An input that a
/// selected, built check needs and that reads `unavailable` or `disabled` makes
/// the result `partial`; an input that no selected, built check needs reads
/// `not_applicable` and never degrades it (decision 1346).
export module planar.engine.diagnose;

import std;
import planar.db;
import planar.incident_model;

namespace planar::engine::diagnose {

/// @brief The catalog version; a check's rule change bumps it. Check ids stay stable once shipped.
export inline constexpr int k_catalog_version = 1;

/// @brief The `schema` value of the JSON rendering.
export inline constexpr std::string_view k_json_schema = "planar.diagnose/1";

/// @brief The busy timeout, in milliseconds, held for the duration of a run.
export inline constexpr int k_busy_timeout_ms = 250;

/// @brief The window, in days, of a run with neither `--plan` nor `--days`.
export inline constexpr int k_default_days = 7;

/// @brief The overall result of one run.
export enum class run_outcome {
  ok,         ///< Every input a selected, built check needs was observed.
  partial,    ///< A needed input was unavailable or disabled; absence of findings is not "clean".
  unavailable ///< The run could not read the database; see `unavailable_reason`.
};

/// @brief Why a run ended `unavailable`.
export enum class unavailable_reason {
  busy,              ///< The database stayed locked past the 250 ms timeout.
  query_failed,      ///< Any other database failure.
  schema_unsupported ///< The database lacks the planning tables.
};

/// @brief Where a window's start came from.
export enum class window_source {
  plan_lifetime, ///< `--plan` without `--days`: the plan's `created_at`.
  days,          ///< `--days` was given.
  default_days   ///< Neither was given; `k_default_days`.
};

/// @brief Bad caller input, the only way `run()` fails.
export enum class run_error_code {
  invalid_instant, ///< `evaluated_at` is not an ISO-8601 UTC instant.
  invalid_days,    ///< `days` is below 1.
  unknown_check,   ///< A selected check id is not in the catalog.
  unknown_plan     ///< `plan_id` names no plan.
};

/// @brief A `run()` failure: a code and a message naming the offending value.
export struct run_error {
  run_error_code code = run_error_code::invalid_instant; ///< Which input was bad.
  std::string    message;                                ///< Human-readable, naming the value.
};

/// @brief The evaluation window, `from` through `to`, both ISO-8601 instants.
export struct time_window {
  std::string        from;                                 ///< Inclusive start.
  std::string        to;                                   ///< Inclusive end: the evaluation instant.
  window_source      source = window_source::default_days; ///< Where `from` came from.
  std::optional<int> days;                                 ///< The day count when `source` is `days` or `default_days`.
};

/// @brief The plan scope of a run.
export struct plan_scope {
  std::optional<std::int64_t> plan_id;  ///< The requested plan; empty when the run has no entity filter.
  std::vector<std::int64_t>   plan_ids; ///< That plan and every descendant plan, ascending; empty with no filter.
};

/// @brief The plan filter as a SQL predicate on `column`: `1 = 1` when the run has no plan filter,
/// otherwise `column in (<ids>)`. Task rows attached to the anchor directly are covered because the
/// anchor is in `plan_ids`. The ids are integers, so interpolation is safe.
/// @param scope The run's scope.
/// @param column A trusted SQL column expression, for example `t.plan_id`.
/// @return The predicate.
export auto plan_filter_sql(const plan_scope& scope, std::string_view column) -> std::string;

/// @brief What a check or an input probe sees. Every reference outlives the call.
export struct check_context {
  db::connection&    conn;         ///< The run's connection, inside the read transaction.
  const plan_scope&  scope;        ///< The plan scope.
  const time_window& window;       ///< The window; a check filters its evidence by it.
  std::string_view   evaluated_at; ///< The injected evaluation instant; never read from the clock.
};

/// @brief One input's observed state, as an input probe reports it.
export struct input_status {
  incident_model::coverage_state state = incident_model::coverage_state::observed; ///< How the input was read.
  std::string                    reason;                                           ///< A short code; empty when observed.
};

/// @brief An input probe: reads whether the input is usable, without reading the evidence itself.
export using input_probe = std::function<std::expected<input_status, db::db_error>(const check_context&)>;

/// @brief A named input a check can depend on, with its probe.
export struct input_def {
  std::string name;  ///< The coverage row's name.
  input_probe probe; ///< Called only when a selected, built check needs the input; may be empty while `built` is false.
  bool built = true; ///< `false` for an input whose reader is not implemented yet; it reads `not_applicable` (`check-not-built`)
                     ///< on every run.
};

/// @brief A check's evaluation: its findings, or the database failure that stopped it.
export using check_fn = std::function<std::expected<std::vector<incident_model::finding>, db::db_error>(const check_context&)>;

/// @brief One catalog check.
///
/// A finding's evidence times must be timestamps of database rows (a lease expiry, a gap's start
/// and end), never derived from the evaluation instant: the evidence digest depends on them, and a
/// time that moves with every run would make each run a new occurrence. The earliest evidence time
/// of a finding becomes its occurrence's `violation_at`. The engine adds a finding's `primary` to
/// its `evidence` when the check left it out, so the fingerprint always names the primary entity.
export struct check_def {
  std::string                         id;                                           ///< Stable kebab-case id; never reused.
  incident_model::check_kind          kind     = incident_model::check_kind::state; ///< `state` or `event`.
  incident_model::diagnostic_severity severity = incident_model::diagnostic_severity::warning; ///< Its highest severity.
  std::string                         category;                                                ///< The `incident_categories` id.
  std::string                         recovery;     ///< The recovery hint a finding defaults to; empty when none.
  std::vector<std::string>            inputs;       ///< Names of the `input_def`s it needs.
  bool                                built = true; ///< `false` for a catalogued check that is not implemented yet.
  check_fn                            evaluate;     ///< Required when `built`.
};

/// @brief A closed set of checks and the inputs they share.
export struct catalog {
  std::vector<input_def> inputs; ///< Every input any check names.
  std::vector<check_def> checks; ///< The checks, in catalog order.
};

/// @brief The shipped catalog: the concatenation of every family's checks and inputs. Built so
/// far: the claim-liveness checks (`claim-lease-lapsed`, `claim-process-died`,
/// `claim-superseded-active`, `task-doing-unclaimed`, `claim-closed-by-reconcile`,
/// `heartbeat-gap`) and `handoff-stale`. `queue-ended-unobserved` is declared as an unbuilt check;
/// the later check tasks fill in the remaining families.
/// @return The catalog.
export auto builtin_catalog() -> catalog;

/// @brief The checks and inputs one family contributes to `builtin_catalog()`.
export struct family {
  std::vector<input_def> inputs; ///< Inputs the family's checks need.
  std::vector<check_def> checks; ///< The family's checks.
};

namespace detail {

/// @brief Reads one finding from the current row of a statement.
using finding_reader = std::function<incident_model::finding(const db::statement&)>;

/// @brief Runs a read-only query and turns each row into a finding with `read`. The caller
/// applies its filters in `sql` itself; this helper adds no bound of its own.
/// @param ctx The check context; its connection runs the query.
/// @param sql The query. It may use the numbered text parameters `?1` and `?2`.
/// @param first Bound to `?1` when the query uses it; empty skips the bind.
/// @param second Bound to `?2` when the query uses it; empty skips the bind.
/// @param read Builds the finding from a row.
/// @return The findings in row order, or the database failure.
auto query_findings(const check_context& ctx, std::string_view sql, std::string_view first, std::string_view second,
                    const finding_reader& read) -> std::expected<std::vector<incident_model::finding>, db::db_error>;

/// @brief The claim-liveness family (lease, process death, supersession, unclaimed `doing`, reconcile closes, heartbeat gaps, unended actions).
/// @return The family.
auto claims_family() -> family;

/// @brief The dispatch family (no role action, unconfirmed, confirmed late).
/// @return The family.
auto dispatch_family() -> family;

/// @brief The CLI and failure-cluster family (`apply-without-preview`, the two cluster checks).
/// @return The family.
auto cli_family() -> family;

/// @brief The remaining state family (stale handoffs, unresolved sync conflicts).
/// @return The family.
auto records_family() -> family;

/// @brief The queue family (`queue-ended-unobserved`) and the run-scope input.
/// @return The family.
auto queue_family() -> family;

} // namespace detail

/// @brief What to evaluate.
export struct run_request {
  std::optional<std::int64_t> plan_id;      ///< `--plan`: the plan and its descendants.
  std::optional<int>          days;         ///< `--days`: overrides the plan-lifetime window; at least 1.
  std::vector<std::string>    checks;       ///< `--check`: selected ids; empty selects every check.
  std::string                 evaluated_at; ///< The injected instant, `YYYY-MM-DDTHH:MM:SS[.fff]Z`.
};

/// @brief What happened to one catalog check in a run.
export enum class check_state {
  ran,              ///< Evaluated.
  not_selected,     ///< Left out by `--check`.
  not_built,        ///< Catalogued but not implemented.
  input_unavailable ///< A needed input was not observed; no findings were produced.
};

/// @brief One catalog check's row in a diagnosis.
export struct check_summary {
  std::string                         id;                                                      ///< The check id.
  incident_model::check_kind          kind     = incident_model::check_kind::state;            ///< Its kind.
  incident_model::diagnostic_severity severity = incident_model::diagnostic_severity::warning; ///< Its highest severity.
  std::string                         category;                                                ///< Its incident category.
  check_state                         state         = check_state::ran;                        ///< What happened to it.
  std::size_t                         finding_count = 0;                                       ///< Findings it produced.
};

/// @brief The result of one run.
export struct diagnosis {
  int                                       catalog_version = k_catalog_version; ///< `k_catalog_version`.
  std::string                               evaluated_at; ///< The canonical evaluation instant, millisecond precision.
  plan_scope                                scope;        ///< The resolved plan scope.
  time_window                               window; ///< The resolved window; empty strings when `unavailable` before resolution.
  run_outcome                               result = run_outcome::ok; ///< The overall outcome.
  std::optional<unavailable_reason>         reason;                   ///< Set exactly when `result` is `unavailable`.
  std::vector<incident_model::coverage_row> coverage;                 ///< One row per catalog input.
  std::vector<check_summary>                checks;                   ///< One row per catalog check.
  std::vector<incident_model::finding>      findings; ///< In the fixed order (`incident_model::compare_findings`).
};

/// @brief Evaluates `cat` over the request's scope and window.
/// @param conn A connection to a Planar database. Its busy timeout is set to 250 ms for the call
/// and restored; no write is issued.
/// @param request What to evaluate; `evaluated_at` is injected, never read from the clock.
/// @param cat The catalog.
/// @return The diagnosis (including an `unavailable` one), or `run_error` for bad input.
export auto run(db::connection& conn, const run_request& request, const catalog& cat) -> std::expected<diagnosis, run_error>;

/// @brief `run()` over `builtin_catalog()`.
/// @param conn A connection to a Planar database.
/// @param request What to evaluate.
/// @return The diagnosis, or `run_error` for bad input.
export auto run(db::connection& conn, const run_request& request) -> std::expected<diagnosis, run_error>;

/// @brief The text of a window source: `plan-lifetime`, `days` or `default-days`.
/// @param s The source.
/// @return A static name.
export auto window_source_name(window_source s) noexcept -> std::string_view;

/// @brief The text of an outcome: `ok`, `partial` or `unavailable`.
/// @param o The outcome.
/// @return A static name.
export auto run_outcome_name(run_outcome o) noexcept -> std::string_view;

/// @brief The text of an unavailable reason: `busy`, `query-failed` or `schema-unsupported`.
/// @param r The reason.
/// @return A static name.
export auto unavailable_reason_name(unavailable_reason r) noexcept -> std::string_view;

/// @brief The text of a check state: `ran`, `not-selected`, `not-built` or `input-unavailable`.
/// @param s The state.
/// @return A static name.
export auto check_state_name(check_state s) noexcept -> std::string_view;

/// @brief Renders a diagnosis as text: a header (scope, window, outcome), one line per
/// `unavailable` or `disabled` input, then one line per finding,
/// `<severity> <check-id> <entity> -> <recovery>`. An `unavailable` run is one line,
/// `diagnose: unavailable (<reason>)`.
/// @param d The diagnosis.
/// @return The text, ending in a newline.
export auto render_text(const diagnosis& d) -> std::string;

/// @brief Renders a diagnosis as one `planar.diagnose/1` JSON object with `catalog_version`,
/// `evaluated_at`, `scope` (with `window.source`), `outcome`, `reason`, `coverage`, `checks`,
/// `findings` (each with `fingerprint` and `incident: null` until the ledger records it) and
/// `would_resolve`. Evidence carries entity refs and timestamps only.
/// @param d The diagnosis.
/// @return The JSON text without a trailing newline.
export auto render_json(const diagnosis& d) -> std::string;

} // namespace planar::engine::diagnose
