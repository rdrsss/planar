/// @file store.cppm
/// @brief `planar.engine.closure.store` — the read side of a task's derived
/// symbol-level closure, plus the two `closure show` renderers (plan 996,
/// task 6095).
///
/// Behavior-preserving port (D2) of the READ path of
/// zig/src/engine/closure/store.zig, plus the rendering half of
/// zig/src/cmd/planar/handlers/closure/show.zig. One of the two `closure`
/// schema leaves — `closure show`.
///
/// ## What `closure compute` is, and why it is NOT here
///
/// `closure compute` is deferred WITH its dependencies, named explicitly
/// rather than silently dropped. It is not a SQL verb: it walks the repo
/// root's filesystem collecting a `.zig` corpus, parses every file through
/// **tree-sitter** (`zig/src/engine/closure/{symbols,walk,weight}.zig`, 1637
/// lines of AST work against `@import("treesitter")`), and only then writes
/// rows. Three separate blockers, any one of which is disqualifying for this
/// cycle:
///
///   1. tree-sitter is not vendored in this tree, and its Zig-grammar
///      traversal semantics are what the extracted symbol names and token
///      weights actually mean. A hand-rolled substitute would produce
///      different symbols and different weights — a silent parity break that
///      no test in this file would catch.
///   2. There is no filesystem-corpus-walk seam here (`collectCorpusRel`
///      recursively iterates the project root).
///   3. Its HANDLER additionally reaches into `engine.planning.task` for the
///      task lookup AND `engine.identity.scope` for the `--scope` write
///      guard. Both are layer-2 peers of this bucket, so composing them is a
///      D15/D18 same-layer violation with no legal home until a `cmd_*`
///      layer exists (decision 947) — the same wall cycle 4 hit with
///      `unlink` and correctly filed rather than forced.
///
/// Its oracle behavior IS captured for whoever picks it up — see
/// store.t.cpp's header for the full transcript, including the empty-seed
/// refusal, the extractor version constant, and the delete-then-insert
/// replacement semantics.
///
/// ## Ordering — HAZARD 4, established by constructing an actual tie
///
/// `show`'s ordering is `role, path, symbol`, where `role` sorts through an
/// explicit CASE (`modify` 0, `reference` 1, everything else 2) rather than
/// alphabetically. The two happen to agree for the schema's three legal
/// values, so a fixture cannot tell them apart — but path-before-symbol CAN
/// be discriminated, and was: rows were seeded so path order and symbol
/// order disagree (`a/first.zig::z.zzz` vs `z/last.zig::a.aaa`) and the
/// oracle sorted by PATH first. Insertion id is not consulted at all; the
/// captured output runs ids 2, 6, 3, 1, 4, 5.
///
/// ## Cut list
///
/// - `closure compute` and the whole extractor (see above).
/// - `policy.audit` rows — no such module in the C++ tree (the same omission
///   engine/planning, engine/promotion, engine/runtime and engine/runs all
///   document). This leaf is read-only.

module;

export module planar.engine.closure.store;

import std;
import planar.db;

namespace planar::engine::closure::store {

/// @brief One persisted closure row.
///
/// `role` is kept as plain text rather than an enum: the schema constrains it
/// with a CHECK, but the ORDER BY's CASE has an explicit `else` arm, so the
/// read path is written to survive a value outside the known three rather
/// than to refuse it.
export struct row {
  std::int64_t id      = 0;       ///< The `closures.id` key.
  std::int64_t task_id = 0;       ///< The owning task.
  std::int64_t repo_id = 0;       ///< The project the path belongs to.
  std::string  path;              ///< Repo-relative source path.
  std::string  symbol;            ///< Qualified symbol name, e.g. `a.alpha`.
  std::string  role;              ///< `modify`, `reference`, or `transitive`.
  std::int64_t token_weight = 0;  ///< Extractor-assigned residency cost.
  std::string  extractor_version; ///< e.g. `m2-closure-0.1`.
  std::string  created_at;        ///< Schema-stamped ISO-8601.
};

/// @brief Failure surface for this module.
export enum class closure_error {
  query_failed ///< Any SQLite failure. There is no `not_found`: an unknown
               ///< task id is an EMPTY result, not an error (oracle-confirmed
               ///< — `closure show 999` exits 0 with `{"task_id":999,"rows":[]}`).
};

/// @brief Read back a task's persisted closure rows.
///
/// Ordered `role` (modify, reference, then everything else), then `path`,
/// then `symbol`. See this module's header for the tie-break derivation.
/// Rows from DIFFERENT extractor versions are returned together, undistinguished
/// — `show` does not filter by version even though the UNIQUE key includes it.
/// @param conn An open, migrated database connection.
/// @param task_id The task whose closure to read.
/// @return The ordered rows, empty when the task has none or does not exist.
export auto show(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<row>, closure_error>;

/// @brief Render `closure show --json`.
///
/// The envelope carries the REQUESTED task_id, echoed from the argument
/// rather than read back from a row — which is why `closure show 999` on an
/// empty database still reports `{"task_id":999,"rows":[]}`.
/// @param task_id The requested task id.
/// @param rows The ordered rows.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_show_json(std::int64_t task_id, std::span<const row> rows) -> std::string;

/// @brief Render `closure show`'s text form.
///
/// The empty case is NOT an empty list: it prints a parenthesised hint naming
/// the exact command to run, with an em dash (U+2014), and returns.
/// @param task_id The requested task id.
/// @param rows The ordered rows.
/// @return The text block, WITH a trailing newline.
export auto render_show_text(std::int64_t task_id, std::span<const row> rows) -> std::string;

/// @brief The refusal `closure show` emits for a non-integer positional.
///
/// Note it is NOT prefixed with the leaf name, unlike almost every other
/// diagnostic in this port — oracle-captured as bare
/// `error: task id must be an integer, got 'notanint'` at exit 2.
/// @param argument The rejected positional, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_task_id(std::string_view argument) -> std::string;

} // namespace planar::engine::closure::store
