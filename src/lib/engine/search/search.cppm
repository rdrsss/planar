/// @file search.cppm
/// @brief `planar.engine.search` — the FTS5 query engine behind
/// `planar search` (plan 996, task 6090).
///
/// Behavior-preserving port (D2) of zig/src/engine/search.zig: `query` and
/// `renderListText`, plus the `Hit` / `Filter` vocabulary they exchange.
///
/// ## Why a bucket of its own
///
/// The Zig original is a TOP-LEVEL `engine/search.zig`, not a member of any
/// bucket, and it needs exactly two things: `db`, and scope-slug
/// resolution. The second used to mean `engine/identity/scope.zig`, which
/// as a C++ dependency would be an `engine_* -> engine_*` edge — forbidden
/// by D15/D18 and FATAL at configure time in `cmake/architecture.cmake`.
/// It does not mean that any more: D19 moved the resolution to layer-1
/// `planar.scope_ref`, and this module depends on THAT. So `engine_search`
/// is a leaf bucket with two downward edges and no sideways ones. Nothing
/// needed extracting for this port; the extraction that made it possible
/// already happened at task 6089.
///
/// ## The six kinds are a CLOSED set, and that is the contract
///
/// Migration 00011 installs exactly six FTS5 virtual tables. `query`
/// composes one `UNION ALL` arm per requested kind against them, so an
/// unrecognised `--kind` is `search_error::unknown_kind` rather than an arm
/// that matches nothing. That refusal is load-bearing: a silently-empty
/// result set from a typo'd kind is indistinguishable from a genuine
/// no-match, which is the class of defect this milestone keeps closing.
module;

export module planar.engine.search;

import std;
import planar.db;

namespace planar::engine::search {

/// @brief One FTS5 match.
///
/// Field ORDER mirrors the oracle's `Hit` and the `select` list behind it.
/// Note that `scope_kind` / `scope_id` are read but are NOT part of the
/// `--json` wire format the handler emits — the oracle's `HitJSON` carries
/// seven of these nine fields. They are returned anyway because the
/// handler's cross-scope merge needs nothing from them today but the text
/// renderer's sibling verbs do, and dropping a column the query already
/// selects would be a silent narrowing of the engine's surface.
export struct hit {
  std::string                 kind;       ///< One of the six entity kinds.
  std::int64_t                id = 0;     ///< The source row's id.
  std::string                 slug;       ///< The source row's slug, or empty when it has none.
  std::string                 title;      ///< The source row's title.
  std::string                 snippet;    ///< The FTS5 `snippet()` extract, `<mark>`-delimited.
  double                      rank = 0.0; ///< Negated bm25; HIGHER is more relevant.
  std::string                 status;     ///< The source row's status, or empty.
  std::string                 scope_kind; ///< The source row's `scope_kind` column.
  std::optional<std::int64_t> scope_id;   ///< The source row's `scope_id` column.
};

/// @brief Narrows a search and caps its result count.
export struct search_filter {
  /// @brief Restrict to these kinds. EMPTY means all six — it does not
  /// mean "none". Every element must be one of `valid_kinds`.
  std::vector<std::string> kinds;
  /// @brief Restrict to these `status` values. Empty means any status.
  /// Values are NOT validated against any per-kind status vocabulary:
  /// a status that no kind uses simply matches nothing, which is the
  /// oracle's behaviour (the predicate is a plain `status IN (...)`).
  std::vector<std::string> statuses;
  /// @brief Restrict to one scope slug. Unset means cross-scope.
  std::optional<std::string> scope;
  /// @brief Restrict to entities tied to this plan id.
  std::optional<std::int64_t> plan_id;
  /// @brief Maximum rows. Zero or negative means `k_default_limit`.
  std::int64_t limit = 0;
};

/// @brief The cap applied when `search_filter::limit` is zero or negative.
export constexpr std::int64_t k_default_limit = 50;

/// @brief The six searchable entity kinds, in the order `query` emits
/// `UNION ALL` arms when no kind filter is given.
///
/// Exposed so a caller can validate `--kind` and name the offending value
/// in its own message rather than re-listing the six here.
/// @return A view over static storage; it outlives every caller.
export auto valid_kinds() -> std::span<const std::string_view>;

/// @brief Error surface for this module.
export enum class search_error : std::uint8_t {
  query_failed,      ///< An underlying SQL statement failed.
  invalid_query,     ///< FTS5 rejected the query string (e.g. an unclosed quote).
  unsupported_scope, ///< The scope slug named a form this module cannot filter on.
  slug_not_found,    ///< The scope slug resolved to no row.
  unknown_kind,      ///< A requested kind is not one of `valid_kinds`.
};

/// @brief Run a full-text search and return ranked, snippet-decorated hits.
///
/// Ordering is `rank DESC, kind ASC, id ASC`. `rank` is the NEGATED bm25
/// score, so descending really is most-relevant-first; the tie-breakers
/// exist because bm25 ties are common across kinds and an unordered tie
/// would make output non-deterministic between runs.
///
/// `query_str` is handed to FTS5's `MATCH` verbatim — the full operator
/// grammar (`OR`, `NOT`, `NEAR`, `"phrase"`, prefix `*`) is available, and
/// bare multi-word input is implicitly AND-ed. Syntax FTS5 rejects comes
/// back as `search_error::invalid_query`, not as an empty result.
///
/// The `--plan` predicate is NOT uniform across kinds, and the asymmetry is
/// the schema's: `task` rows carry a `plan_id` COLUMN and are filtered
/// directly on it, while the other five reach their plan through an
/// `entity_links` row with `relationship = 'derives-from'` and are filtered
/// with an `EXISTS` subquery. A task linked to a plan only by an
/// `entity_links` edge therefore does NOT match `plan_id`, and a plan-less
/// task with a `derives-from` edge does not either. Reproduced, not
/// harmonised.
///
/// An empty result is success, not an error.
/// @param conn An open, migrated database connection.
/// @param query_str The FTS5 query.
/// @param filter Kind/status/scope/plan narrowing plus the result cap.
/// @return The hits in rank order (possibly empty), or a `search_error`.
export auto query(db::connection& conn, std::string_view query_str, const search_filter& filter)
    -> std::expected<std::vector<hit>, search_error>;

/// @brief Render a hit list as `planar search` prints it without `--json`.
///
/// Per hit: a reference line, then the snippet indented two spaces when the
/// snippet is non-empty. The reference is `<kind>:<id> (<slug>)` when the
/// row has a slug and `<kind>:<id>` when it does not, followed by
/// ` [<status>]` when the row has a status, then an em-dash and the title.
///
/// An EMPTY list renders the literal `(no results)` line rather than zero
/// bytes. That is the oracle's, and it is why this returns a complete
/// payload rather than a fragment.
///
/// ## The 256-byte reference truncation is REPRODUCED, not fixed
///
/// The oracle formats the reference into a fixed `[256]u8` and, on
/// overflow, falls back to printing the BARE KIND with no id and no slug
/// (`bufPrint(...) catch h.kind`). Nothing in the schema bounds a slug's
/// length, so this arm is reachable: a row whose
/// `<kind>:<id> (<slug>)` rendering exceeds 256 bytes prints as just
/// `plan [active] — <title>`, losing the id. Ported verbatim because a
/// differential run would flag any divergence, however sensible; the
/// widening belongs in a task against the oracle, not in this port.
/// @param hits The hits to render.
/// @return The complete stdout payload INCLUDING its trailing newline (see
/// `src/lib/engine/runtime/CMakeLists.txt`'s stdout-terminator contract).
export auto render_list_text(std::span<const hit> hits) -> std::string;

} // namespace planar::engine::search
