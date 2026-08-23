/// @file schema.cppm
/// @brief `planar.cmd.planar_watch.handlers.schema` — the `planar-watch
/// schema` leaf (plan 996, task 6107).
///
/// Port target: zig/src/cmd/planar-watch/handlers/schema.zig.
///
/// FRAGMENT renderer: `planar.cli.schema::schema_json` documents itself as
/// returning no trailing newline and the Zig handler appends one at the
/// write site (`writeAll(catalog)` then `writeAll("\n")`), so this handler
/// appends. Read each renderer's own `@return`; the opposite case is live
/// in this same tree's sibling binary (`planar workflow list --json`,
/// whose renderer returns a complete payload and must not be appended to).
module;

export module planar.cmd.planar_watch.handlers.schema;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch schema`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @param root The command tree to emit — passed in so the catalog is
/// provably the same tree dispatch just routed through.
/// @return Success; this leaf has no failure path.
export auto schema(context& ctx, const cli::match_result& args, const cli::cmd& root) -> handler_result;

} // namespace planar::cmd::watch::handlers
