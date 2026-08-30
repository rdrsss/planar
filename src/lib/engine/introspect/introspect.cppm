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
/// ## What is NOT here, and a real architecture deviation from the oracle
///
/// The oracle's `Bundle.preview: ?adapters.Preview = null` embeds
/// `introspection_adapters.Preview` directly, and `build()` never
/// populates it — only the `report` handler does, by separately calling
/// the DISCOVERY half of `introspection_adapters.zig`
/// (`collectPreviewFromPaths` / `collectConfiguredPreview`), which is not
/// ported in this tree yet (see `planar.engine.introspection_adapters`'s
/// header — task 6121's tracking row).
///
/// THIS PORT CANNOT embed `planar.engine.introspection_adapters::preview`
/// the same way: `cmake/architecture.cmake`'s D15 forbids an
/// `engine_* -> engine_*` dependency edge (`engine_introspect ->
/// engine_introspection_adapters` FAILED configure with exactly that
/// diagnostic), unlike Zig, which has no such enforced layering. The
/// established fix for this shape elsewhere in the tree (see
/// `planar.scope_ref`, D19) is extracting the shared type to a NEW layer-1
/// module both engine buckets depend on — but `preview`/`vendor`/
/// `category`/etc. are already layer-2 in the reviewer-approved
/// `introspection_adapters.cppm` (task 6102), and that extraction is
/// out of scope for this row.
///
/// So `bundle` here carries NO `preview` field at all, and
/// `render_text`/`render_json` unconditionally emit the "unavailable" /
/// empty-arrays branch — which is honest: in the CURRENT dependency
/// graph nothing can populate a preview, so that branch is the only
/// reachable one. Whoever wires the `report` handler (layer 3, `cmd_planar`)
/// needs to either (a) extract `preview` + the four enums to a layer-1
/// module both `engine_introspect` and `engine_introspection_adapters`
/// depend on, then re-add the field here, or (b) keep `bundle` preview-free
/// and have the layer-3 handler splice a separately-rendered preview block
/// into the text/JSON output itself. This is a genuine finding worth its
/// own tracking row — not something to solve inline in this cycle.
module;

export module planar.engine.introspect;

import std;
import planar.db;

namespace planar::engine::introspect {

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
/// Member order is the `--json` wire-format key order the oracle emits:
/// version, schema_version, health, window, invocations, failures,
/// actions, sync, claims, claim_failure_categories, handoffs.
export struct bundle {
  std::string                               version;                  ///< Binary version string ("planar").
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
  // NOTE: no `preview` field. See this file's header — embedding
  // `introspection_adapters::preview` here would create a D15-forbidden
  // `engine_* -> engine_*` edge. `render_text`/`render_json` always emit
  // the "unavailable" / empty-arrays branch as a result.
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
/// @param db_path Borrowed path string, unused by this port (kept for
/// signature parity with the oracle, which also never reads it).
/// @return The bundle, or an `introspect_error`.
export auto build(db::connection& conn, std::int64_t window_days, std::int64_t tail_n, bool logging_enabled,
                  std::string_view db_path) -> std::expected<bundle, introspect_error>;

/// @brief Convert authoritative, structurally-redacted `cli_invocations`
/// rows into the adapter's private JSONL boundary. Only verb path, exit
/// code, error category, and timestamp are selected; argument shapes and
/// entity-bearing tables are never read.
/// @param conn An open, migrated database connection.
/// @param window_days How many days back to query.
/// @param max_bytes Abort with `query_failed` once the accumulated text
/// would exceed this many bytes (mirrors the oracle's `error.StreamTooLong`).
/// @return The JSONL text (always populated on success — see this file's
/// header), or an `introspect_error`.
export auto cli_preview_jsonl(db::connection& conn, std::int64_t window_days, std::size_t max_bytes)
    -> std::expected<std::string, introspect_error>;

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
