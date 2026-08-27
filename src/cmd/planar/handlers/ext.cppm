/// @file ext.cppm
/// @brief `planar.cmd.planar.handlers.ext` — the four ported `planar ext`
/// leaves: `register jira`, `register github`, `list` (plan 996, task 6041)
/// and `test` (task 6258).
///
/// Port targets: zig/src/cmd/planar/handlers/ext/register/{jira,github}.zig,
/// ext/list.zig and ext/test.zig.
///
/// ## Why four of six
///
/// `ext test` was deferred through task 6041 on the adapter FACTORY — the
/// auth-resolution unit that turns an `external_systems` row into an adapter
/// instance. Task 6258 ported it as
/// `planar.cmd.planar.handlers.ext_adapter_factory`, and `ext test` is the
/// leaf that makes it observable: SIX refusal messages (the header of that
/// module says why it is six and not the five this file previously claimed)
/// and one success line are its entire surface.
///
/// `ext create` / `ext propagate-one` / `ext propagate` remain deferred, and
/// the factory did NOT unblock them — they wait on the create/propagate half
/// of `engine_extsync` (see that bucket's CMakeLists.txt), which is a
/// separate absence. Neither deferral is speculative: each names the surface
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

/// @brief Handle `planar ext test <slug> [--json]`.
///
/// Builds an adapter and reports whether it was wired. It does NOT contact
/// the remote — the oracle's own comment says so and its `probeOk` only
/// checks that construction produced an adapter — so this leaf reaches the
/// network on no path and is testable with no fixture server. The whole
/// observable surface is the six refusals in
/// `planar.cmd.planar.handlers.ext_adapter_factory`, plus one success line.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, `not_found` (exit 1) for an unknown slug, or
/// `invalid_input` (exit 2) for any credential or kind refusal.
export auto ext_test(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
