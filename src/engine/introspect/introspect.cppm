/// @file introspect.cppm
/// @brief `planar.engine.introspect` — diagnostic bundle aggregation behind
/// `planar report` (plan 996, task 6121).
///
/// Behavior-preserving port (D2) of zig/src/engine/introspect.zig's
/// `Bundle`, `build`, `cliPreviewJsonl`, `renderText`, and `renderJson`.
///
/// All queries in this module are structurally redacted BY CONSTRUCTION:
/// no query reads an entity-table text column (`tasks.title`,
/// `plans.title`, `questions.title`, and so on — the full denylist is
/// below). That IS the boundary this module actually holds, and it is
/// real: nothing here can turn an entity's title/body/summary into report
/// output.
///
/// It is NOT a guarantee that nothing rendered here is operator-authored
/// free text. Two columns this module DOES select are themselves
/// operator-influenced at the CAPTURE layer, outside this module's control:
/// `cli_invocations.verb_path` and `agent_work_claims.vendor`. In
/// particular, `verb_path` is bounded to the first `max_verb_depth = 2`
/// non-flag tokens (`cli_log.zig:128`), which is enough to hide a
/// SUBCOMMAND's own free-text argument (`task add "<title>"`'s token 2 is
/// the literal `add`, not the title) — but `search` is a TOP-LEVEL verb
/// with a REQUIRED free-text positional (`handlers/search.zig:38`), so
/// `planar search <query>` records `verb_path = "search <query>"` and that
/// text is selected verbatim here (`query_invocations`, `query_failure_tail`,
/// `cli_preview_jsonl`) and rendered into `[invocations]`, `[failure tail]`,
/// and the JSONL boundary. This IS the oracle's own behavior — `cli_log.zig`
/// is the writer and out of scope for this module — not a defect introduced
/// by this port; it is called out here because the paragraph above no
/// longer claims otherwise. `introspect.t.cpp` pins the leaking case
/// directly rather than leaving it to be discovered by a future reader.
///
/// Tables read: `cli_invocations`, `agent_actions`, `sync_events`,
/// `task_reopens`, `agent_work_claims`, `handoffs`, `schema_migrations`,
/// `tasks` (status/next_action only), `context_snapshots` (existence only).
/// Tables never read: `questions`, `scenarios`, `decisions`, `artifacts`,
/// `plans`, `projects`, `project_associations` — any column carrying entity
/// text.
///
/// ## `preview`, and how it got here (decision 981, task 6352)
///
/// The oracle's `Bundle.preview: ?adapters.Preview = null` embeds
/// `introspection_adapters.Preview` directly, and `build()` never
/// populates it — only the `report` handler does, by separately calling
/// the DISCOVERY half of `introspection_adapters.zig`
/// (`collectPreviewFromPaths` / `collectConfiguredPreview`).
///
/// THIS PORT CANNOT embed `planar.engine.introspection_adapters::preview`
/// the same way: `cmake/architecture.cmake`'s D15 forbids an
/// `engine_* -> engine_*` dependency edge (`engine_introspect ->
/// engine_introspection_adapters` FAILED configure with exactly that
/// diagnostic, verified experimentally by a reviewer on task 6121), unlike
/// Zig, which has no such enforced layering. Decision 981 settled the fix:
/// extract `preview` and its four enums to the NEW layer-1
/// `planar.introspection_preview` module (task 6352) both
/// `engine_introspect` and `engine_introspection_adapters` depend on
/// downward — the same shape `planar.scope_ref` (D19) already uses. The
/// rejected alternative — a layer-3 handler splicing a separately-rendered
/// preview block into `render_json`'s output via string surgery — would
/// have split ONE WIRE FORMAT ACROSS TWO LAYERS; see decision 981's body.
///
/// So `bundle` carries `preview` as `std::optional<preview_type>`. `build`
/// (this module, DB-only) still never populates it — that stays the
/// `report` handler's job, calling
/// `introspection_adapters::collect_preview_from_paths` after `build`
/// returns, exactly mirroring the oracle's own two-step assembly.
/// `render_text`/`render_json` branch on whether it is set: unset renders
/// the "unavailable" / empty-arrays shape (still the ONLY reachable
/// output of a bare `build()` call, e.g. every test in this file that
/// never sets `.preview` by hand), set renders the populated rows.
module;

export module planar.engine.introspect;

import std;
import planar.db;
import planar.introspection_preview;

namespace planar::engine::introspect {

/// @brief Alias for the shared layer-1 preview type, so this module's
/// public surface can say `preview` without a fully-qualified name at
/// every use site. NOT re-exported under this bucket's own vocabulary the
/// way `introspection_adapters` re-exports the enums (task 6102's
/// callers): this bucket has no pre-existing callers to keep compiling.
using preview_type = planar::introspection_preview::preview;

/// @brief The placeholder rendered for a stored `verb_path` the catalog
/// predicate rejects.
export inline constexpr std::string_view unrecognized_verb_path = "<unrecognized>";

/// @brief Decides whether a stored `verb_path` is one the live CLI catalog
/// could have produced.
///
/// Injected by the caller so this engine never imports a `cmd` module. The
/// argument is the stored value as written (never prefixed). A rejected
/// value is rendered as `unrecognized_verb_path`; the stored row is never
/// changed or purged (decision 1045). Must be cheap and side-effect free.
export using verb_path_predicate = std::function<bool(std::string_view)>;

/// @brief One verb-path invocation aggregate row.
export struct verb_count {
  std::string  verb_path;         ///< The recorded verb path.
  std::int64_t count         = 0; ///< Total invocations in the window.
  std::int64_t success_count = 0; ///< Of those, `exit_code == 0`.
  std::int64_t failure_count = 0; ///< Of those, `exit_code != 0`.
};

/// @brief One error-category failure aggregate row.
export struct failure_category {
  std::string  category;  ///< The `error_category`, or `"unknown"`.
  std::int64_t count = 0; ///< Failure count in the window.
};

/// @brief One agent-action outcome aggregate row.
export struct action_outcome {
  std::string  action_kind; ///< The `agent_actions.action_kind`.
  std::string  outcome;     ///< The `agent_actions.outcome`, or `"unknown"`.
  std::int64_t count = 0;   ///< Count in the window.
};

/// @brief One sync-event outcome aggregate row.
export struct sync_outcome {
  std::string  outcome;   ///< The `sync_events.outcome`, or `"unknown"`.
  std::int64_t count = 0; ///< Count in the window.
};

/// @brief One failure-tail row (most-recent failed invocations, newest
/// first). Structurally redacted — no entity text, no scope slug.
export struct failure_tail_row {
  std::string  verb_path;      ///< The recorded verb path.
  std::string  error_category; ///< The `error_category`, or `"unknown"`.
  std::int64_t exit_code = 0;  ///< The recorded exit code.
  std::string  recorded_at;    ///< The recorded timestamp.
};

/// @brief Claim aggregate counts.
export struct claim_counts {
  std::int64_t stale_claims   = 0; ///< Active claims older than 24h.
  std::int64_t never_consumed = 0; ///< Claims that closed expired/released.
};

/// @brief One privacy-safe terminal claim failure aggregate.
export struct claim_failure_category_count {
  std::string  provider;  ///< The claim's `vendor`.
  std::string  category;  ///< The closed `failure_category`, or `"unknown"`.
  std::int64_t count = 0; ///< Count in the window.
};

/// @brief Handoff aggregate counts.
export struct handoff_counts {
  std::int64_t stale_handoffs = 0; ///< Pending/validated handoffs older than 24h.
  std::int64_t never_consumed = 0; ///< Handoffs never reaching `consumed`.
};

/// @brief The complete diagnostic bundle returned by `build`.
///
/// Member order is NOT the `--json` key order; `render_json` fixes that:
/// version, schema_version, health, window, logging_enabled, invocations,
/// failures, failure_tail, actions, sync, claims, claim_failure_categories,
/// handoffs, reopens, introspection_preview.
export struct bundle {
  std::string                               version;                  ///< Build version token (see `build`).
  std::int64_t                              schema_version = 0;       ///< Max applied `schema_migrations` version.
  std::string                               health;                   ///< `"ok"` or `"degraded"`.
  std::int64_t                              window_days     = 0;      ///< Window in days that was queried.
  bool                                      logging_enabled = false;  ///< Whether `[introspection].cli_log` is on.
  std::vector<verb_count>                   invocations;              ///< Empty when logging disabled.
  std::vector<failure_category>             failures;                 ///< Empty when logging disabled.
  std::vector<action_outcome>               actions;                  ///< Always-on.
  std::vector<sync_outcome>                 sync;                     ///< Always-on.
  claim_counts                              claims;                   ///< Always-on.
  std::vector<claim_failure_category_count> claim_failure_categories; ///< Always-on.
  handoff_counts                            handoffs;                 ///< Always-on.
  std::int64_t                              reopens = 0;              ///< Task reopen count in the window.
  std::vector<failure_tail_row>             failure_tail;             ///< Empty when logging disabled.
  /// The introspection-adapters preview, or unset. `build` (this module)
  /// never sets it; the `report` handler populates it after `build`
  /// returns by calling `introspection_adapters::collect_preview_from_paths`
  /// — see this file's header, "`preview`, and how it got here".
  std::optional<preview_type> preview;
};

/// @brief Error surface for `build` / `cli_preview_jsonl`.
export enum class introspect_error : std::uint8_t {
  query_failed, ///< An underlying SQL statement failed.
};

/// @brief Build the diagnostic bundle for the given window and tail size.
/// @param conn An open, migrated database connection.
/// @param window_days How many days back to query. Caller validates `> 0`.
/// @param tail_n How many failure-tail rows to return. Caller validates `> 0`.
/// @param logging_enabled Whether `[introspection].cli_log` is on. When
/// false the invocation/failure/failure-tail sections are left empty so the
/// caller can render "logging disabled" instead of zeros.
/// @param version The build's version token (the sha token of the
/// `planar version` line), reported verbatim as `bundle::version`.
/// @param recognized Catalog predicate over stored `verb_path` values. A
/// rejected path is rendered as `unrecognized_verb_path`, and rejected paths
/// aggregate into one `[invocations]` row. Required, not defaulted: an
/// omitted predicate must be a compile error rather than a silent no-mask.
/// @return The bundle, or an `introspect_error`.
export auto build(db::connection& conn, std::int64_t window_days, std::int64_t tail_n, bool logging_enabled,
                  std::string_view version, const verb_path_predicate& recognized) -> std::expected<bundle, introspect_error>;

/// @brief The bounded CLI-invocation JSONL `cli_preview_jsonl` produced.
export struct cli_preview {
  std::string jsonl;             ///< Rows read, oldest first; empty when none fit.
  bool        truncated = false; ///< True when the byte budget left rows unread.
  std::size_t rows      = 0;     ///< Number of rows in `jsonl`.
  std::size_t omitted   = 0;     ///< Window rows not in `jsonl` (the oldest ones).
};

/// @brief Convert authoritative, structurally-redacted `cli_invocations`
/// rows into the adapter's private JSONL boundary. Only verb path, exit
/// code, error category, and timestamp are selected; argument shapes and
/// entity-bearing tables are never read.
///
/// Rows are selected newest first and taken while the accumulated text stays
/// within `max_bytes` (a row that would exceed it is not taken, so a text of
/// exactly `max_bytes` is not truncated). The rows taken are returned
/// oldest first. Rows left over are counted in `omitted`; the call does not
/// fail because the window is large.
/// @param conn An open, migrated database connection.
/// @param window_days How many days back to query.
/// @param max_bytes Upper bound on `jsonl.size()`.
/// @param recognized Catalog predicate over stored `verb_path` values; a
/// rejected path is emitted as `planar <unrecognized>`.
/// @return The preview (always populated on success), or `query_failed`
/// when a statement fails.
export auto cli_preview_jsonl(db::connection& conn, std::int64_t window_days, std::size_t max_bytes,
                              const verb_path_predicate& recognized) -> std::expected<cli_preview, introspect_error>;

/// @brief Render the diagnostic bundle as human-readable text.
/// @param b The bundle to render.
/// @return The complete stdout payload INCLUDING its trailing newline.
export auto render_text(const bundle& b) -> std::string;

/// @brief Emit the bundle as the stable `--json` wire format. Field names
/// ARE the contract consumed by downstream agents and skills. Empty
/// windows emit empty arrays, never nulls or missing fields.
/// @param b The bundle to render.
/// @return The JSON text INCLUDING its trailing newline.
export auto render_json(const bundle& b) -> std::string;

} // namespace planar::engine::introspect
