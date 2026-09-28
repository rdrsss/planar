/// @file schema.cppm
/// @brief `planar.cmd.planar_agent.handlers.schema` — the `planar-agent
/// schema` leaf (plan 996, task 6107).
///
/// Port target: zig/src/cmd/planar-agent/handlers/schema.zig.
///
/// Emits this binary's OWN command tree as a flat JSON catalog. In the
/// tree it is the leaf that makes the capability boundary machine-readable:
/// zig `tools/cli_usage_lint.zig` dumps each planning-state binary's
/// `schema` catalog and refuses any authored skill/agent/doc line that
/// references a flag the binary does not expose. A verb that is not in
/// this tree is not in the catalog, and therefore cannot be authored
/// against — which is the same boundary `tree.cppm` states in prose,
/// enforced by a different tool.
///
/// ## Terminator: this renderer returns a FRAGMENT
///
/// `planar.cliapp.schema::schema_json`'s `@return` says so in as many words —
/// "no trailing newline — callers matching `handlers/schema.zig`'s
/// `planar schema` verb append one at the write site, not here" — and the
/// Zig handler does exactly that (`writeAll(catalog)` then
/// `writeAll("\n")`). So this handler appends. That is the OPPOSITE of
/// what `planar workflow list --json` needs, where the renderer returns a
/// complete payload and appending would break the zero-bytes-on-empty
/// case. The rule is per-renderer, read off each `@return`; there is no
/// blanket policy (commit 5728133, task 6106).
///
/// The catalog reflects only the verbs THIS tree registers (two, against
/// the oracle's eighteen), so its bytes are not oracle-comparable while
/// the port is partial. Its SHAPE is, and is pinned.
module;

export module planar.cmd.planar_agent.handlers.schema;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent schema`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @param root The command tree to emit — passed in rather than rebuilt so
/// the catalog is provably the same tree dispatch just routed through.
/// @return Success; this leaf has no failure path.
export auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result;

} // namespace planar::cmd::agent::handlers
