/// @file schema.cppm
/// @brief `planar.cmd.planar_ext.handlers.schema` — the `planar-ext
/// schema` leaf (plan 996, task 6418).
///
/// NOT a nicety on this binary — decision 998 makes it a hard requirement.
/// `tools/cli_usage_lint` SKIPS command paths it cannot resolve rather
/// than flagging them, so a binary with no `schema` catalog turns the
/// authored-surface gate VACUOUS over its entire verb set. `planar-execute`
/// is exempt from that lint because it has no catalog and never will (see
/// its own CMakeLists.txt); `planar-ext` is explicitly NOT exempt, because
/// it carries operator-facing verbs (moving in over tasks 6419-6421) that
/// the lint must be able to police from the moment this skeleton lands.
///
/// FRAGMENT renderer, exactly like the other three binaries':
/// `planar.cliapp.schema::schema_json` returns no trailing newline, so
/// this handler appends one at the write site.
module;

export module planar.cmd.planar_ext.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

/// @brief Handle `planar-ext schema`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @param root The command tree to emit — passed in so the catalog is
/// provably the same tree dispatch just routed through.
/// @return Success; this leaf has no failure path.
export auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result;

} // namespace planar::cmd::ext::handlers
