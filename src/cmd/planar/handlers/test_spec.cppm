/// @file test_spec.cppm
/// @brief `planar.cmd.planar.handlers.test_spec` — the `planar test-spec
/// status` leaf (plan 996, task 6299).
///
/// Port target: zig/src/cmd/planar/handlers/test_spec/status.zig (172
/// lines), of which only the first twenty are wiring — the anchor lookup,
/// the walk and BOTH renderers already live in
/// `planar.engine.planning.test_spec_status` (ported at an earlier cycle
/// with its own oracle captures), so this handler is a lookup, a compute and
/// a stream write.
///
/// ## `fetch_anchor` requires an ANCHOR, and that is the surprising half
///
/// Both the numeric and the slug lookup add `parent_plan_id is null`, so a
/// perfectly real MILESTONE plan reports `plan '2' not found` rather than
/// "not an anchor". That is the oracle's behaviour, captured in the engine
/// module rather than assumed here, and it is reproduced without
/// improvement.
///
/// ## Exit codes, oracle-captured
///
///   unresolvable / non-anchor plan   exit 1  `plan '<arg>' not found`
///   an anchor with no milestones     exit 0  one milestone row (itself)
module;

export module planar.cmd.planar.handlers.test_spec;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar test-spec status <plan> [--json]`.
///
/// `--json` is NDJSON — one object per milestone, then the summary object on
/// its own line — not a single document. See the engine module's
/// `render_json`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, or the refusal.
export auto test_spec_status(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
