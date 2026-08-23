/// @file claims.cppm
/// @brief `planar.cmd.planar_agent.handlers.claims` — the acquisition half
/// of the claim ritual: `pull`, `peek`, `claim`, `heartbeat`, and
/// `claim-associate` (plan 996, task 6038).
///
/// Port target: zig/src/cmd/planar-agent/handlers/{pull,peek,claim,
/// heartbeat,claim_associate}.zig.
///
/// The five are grouped because they share their validation vocabulary —
/// the `--ttl` parse, the `--stage` requires `--run` rule, the
/// worktree-id-or-path split, and the `session::ensure_active` call that
/// mints the `sessions` row every claim's FK needs. Splitting them into
/// five files would put five copies of that vocabulary in five places, or
/// force a sixth file to hold it.
///
/// Three flag asymmetries between `pull` and `claim` are NOT accidents and
/// must not be tidied up — each changes a `--help` page that is
/// byte-comparable against the oracle:
///
///   `--model`, `--no-transition`, `--force`   claim only
///   `--base-ref`, `--metadata`, `--parent-action`  pull only
///   `--repo-root`, `--no-locality-probe`       both, but pull ALSO
///                                              consults the action
///                                              kind's `probe_default`
///                                              and `claim` does not
module;

export module planar.cmd.planar_agent.handlers.claims;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent pull <plan-id> [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto pull(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-agent peek <plan-id> [--json]`.
///
/// The read-only twin of `pull`: same selector, zero writes. A test that
/// asserts the row counts are unchanged after this verb is what makes
/// "read-only" a fact rather than a description.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto peek(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-agent claim --entity <ref> [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto claim(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-agent heartbeat --claim <token> [flags]`.
///
/// Owns its own transaction rather than delegating to
/// `agentatomic`, because the optional `--status` action row must land in
/// the SAME transaction as the lease refresh — a heartbeat that recorded
/// its status but failed to extend the lease would be worse than one that
/// did neither.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto heartbeat(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar-agent claim-associate --claim <token> --run <id>`.
///
/// Deliberately exits 0 with `updated:0` for an unknown or terminal claim
/// — the external harness that calls this is stamping metadata, not
/// asserting ownership, and a hard failure there would abort a dispatch
/// over a bookkeeping miss.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto claim_associate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
