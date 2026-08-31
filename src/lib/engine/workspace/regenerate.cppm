/// @file regenerate.cppm
/// @brief `planar.engine.workspace.regenerate` — render workspace
/// `AGENTS.md` from the routing table and write the `.manifest-docs` merkle
/// beside it (plan 996, task 6364).
///
/// Behavior-preserving port (D2) of
/// zig/src/engine/workspace/regenerate.zig plus the render half of
/// zig/src/cmd/planar/handlers/workspace/regenerate.zig.
///
/// ## `load_layout`, not `ensure_layout` — and then a manual `mkdir`
///
/// The oracle calls `identity.workspace.loadLayout`, NOT `ensureLayout`, and
/// then explicitly creates `layout.dir` before writing:
///
/// ```zig
/// const layout = try identity.workspace.loadLayout(allocator, environ, org_id);
/// ...
/// try std.Io.Dir.cwd().createDirPath(io, layout.dir);
/// try writeFileAtomic(allocator, layout.agents_md, rendered);
/// ```
///
/// This port reproduces exactly that split rather than reaching for
/// `ensure_layout` (which does the same two things in one call, used by
/// `routing build`): `load_layout` first, then an explicit
/// `create_directories` before either file is written.
///
/// ## The routing table read reuses `routing::decode`
///
/// The oracle's own `routing.read()` is file-read-plus-parse fused into one
/// call; this tree's `routing::decode()` (already ported for `show`/`build`,
/// see routing.cppm) is the parse half applied to bytes this leaf reads
/// itself — the identical split `show`'s handler already uses. Reusing it
/// here means the JSON-failure taxonomy (`decode_error`, its four tags, the
/// two exit codes behind them) does not get a second, possibly-drifted
/// definition.
///
/// ## Failure surface (all oracle-derived)
///
///     table missing on disk       not_found       exit 1  "routing table not found; run `planar workspace routing build` first"
///     table not valid JSON        generic_failure exit 1  "regenerating AGENTS.md failed:
///     <SyntaxError|UnexpectedEndOfInput|DuplicateField>" table valid JSON, bad shape invalid_input   exit 2  "regenerating
///     AGENTS.md failed: InvalidInput" anything else (query, write) generic_failure exit 1  a specific message per failure point
///
/// The message TEMPLATE for the decode row differs from `show`'s
/// ("regenerating AGENTS.md failed: ..." vs "decoding routing table
/// failed: ..."), because `regenerate.zig`'s `else` catch arm interpolates
/// its OWN format string around the same `@errorName(e)` — see this leaf's
/// header for the source. Only `NotFound` gets its own dedicated message;
/// every other propagated error shares the generic template.
module;

export module planar.engine.workspace.regenerate;

import std;
import planar.db;
import planar.engine.workspace.identity;

namespace planar::engine::workspace::regenerate {

/// @brief What a successful regenerate reports.
export struct result {
  std::string  agents_path;       ///< `<state-dir>/AGENTS.md`.
  std::string  manifest_path;     ///< `<state-dir>/.manifest-docs`.
  std::int64_t project_count = 0; ///< `routing_table.projects.size()`.
  std::int64_t bytes_written = 0; ///< Bytes written to `agents_path`.
  std::string  manifest_root;     ///< The built manifest's `root` digest.
};

/// @brief Which oracle-observed failure bucket a regenerate hit. See this
/// file's header for the full table.
export enum class error_kind {
  not_found,       ///< The routing table does not exist on disk.
  invalid_input,   ///< The routing table parsed but is not a usable shape.
  generic_failure, ///< Everything else: org resolution, layout, decode syntax, query, or write failure.
};

/// @brief A regenerate failure: which bucket, and the exact body text
/// (without the `error: ` prefix or a trailing newline — the handler layer
/// adds both, same convention as `identity::resolve_error`'s siblings).
export struct failure {
  error_kind  kind;
  std::string message;
};

/// @brief Render `AGENTS.md` for `org_id`'s workspace and write it plus its
/// `.manifest-docs` merkle.
/// @param conn An open connection.
/// @param env Environment lookup, passed straight through to `identity`.
/// @param org_id The org association whose workspace to regenerate.
/// @return The written paths and counts, or why it failed.
export auto regenerate(db::connection& conn, const identity::env_lookup& env, std::int64_t org_id)
    -> std::expected<result, failure>;

} // namespace planar::engine::workspace::regenerate
