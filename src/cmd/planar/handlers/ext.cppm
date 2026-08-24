/// @file ext.cppm
/// @brief `planar.cmd.planar.handlers.ext` — the three ported `planar ext`
/// leaves: `register jira`, `register github` and `list` (plan 996, task
/// 6041).
///
/// Port targets: zig/src/cmd/planar/handlers/ext/register/{jira,github}.zig
/// and ext/list.zig.
///
/// ## Why only three of six
///
/// `ext test` builds an adapter through the Zig `adapter_factory`, whose
/// job is auth RESOLUTION — reading `$<auth_ref>` for `token-env` systems
/// and shelling `gh auth token` for `gh-cli` ones — and whose five distinct
/// refusal messages are the observable surface. That factory is deferred as
/// a unit; it is not part of the adapter boundary this cycle landed.
/// `ext create` / `ext propagate-one` / `ext propagate` are deferred with
/// the create/propagate half of `engine_extsync` (see that bucket's
/// CMakeLists.txt). Neither deferral is speculative: each names the surface
/// it waits on.
///
/// ## The three renderers, and why they live HERE
///
/// `engine_external` has no render surface — the Zig originals emit these
/// bytes from the handler too — so the terminator belongs to this layer on
/// every path. All six payloads (three verbs x text/JSON) are captured
/// verbatim from the oracle and reproduced in `ext.t.cpp`'s header. The two
/// that are easy to get wrong:
///
///   - `ext list --json` is ONE JSON OBJECT PER LINE, not an array, and it
///     emits NOTHING AT ALL for an empty database (not `[]`, not a blank
///     line). The text form emits `no external systems registered\n`
///     instead, so the empty case is where the two shapes diverge most.
///   - `ext list --json` OMITS `base_url` and `default_project` entirely
///     when they are NULL rather than emitting `null`. Every currently
///     reachable registration sets both, so this only shows up on a row
///     written some other way — it is preserved because the Zig renderer
///     branches on the optional rather than serializing it.
///
/// ## A duplicate slug exits 6, not 1
///
/// `precondition_conflict`. Oracle-captured:
/// `error: external system 'gh-demo' already registered` on stderr with
/// exit 6. That is the same bucket `slug_conflict` maps to everywhere else
/// in this binary.
module;

export module planar.cmd.planar.handlers.ext;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar ext register jira <slug> --base-url <u> --project
/// <p> --auth-env <e> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `slug_conflict` (exit 6) when the slug is taken.
export auto ext_register_jira(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext register github <slug> --project <p>
/// [--auth-env <e>] [--json]`.
///
/// Omitting `--auth-env` is not an error — it selects `gh-cli` auth.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `slug_conflict` (exit 6) when the slug is taken.
export auto ext_register_github(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar ext list [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) on a query failure.
export auto ext_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
