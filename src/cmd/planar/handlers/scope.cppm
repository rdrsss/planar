/// @file scope.cppm
/// @brief `planar.cmd.planar.handlers.scope` — the whole five-leaf `planar
/// scope` family: `show`, `suggest`, and the three plan-153-M5 removal
/// stubs `use` / `pop` / `clear` (plan 996, task 6214).
///
/// Port targets: zig/src/cmd/planar/handlers/scope/{show,suggest,removed}.zig.
///
/// ## The family is complete, and that is the point
///
/// Two of these leaves do real work and three are refusals. Porting only
/// the two would have left `scope use` answering exit 64 ("not implemented
/// in this build") where the oracle answers exit 2 with a paragraph
/// explaining that the scope STACK was removed and what to do instead.
/// Those are different answers to different questions, and the exit-64 one
/// is wrong in the direction that invites an operator to wait for a build
/// that will never implement it.
///
/// ## `scope show` renders from the READ SET, not from the write resolution
///
/// This is the shape most likely to be gotten wrong by reading the handler
/// top-down, because `scope show` computes BOTH and the write resolution is
/// mostly DEAD on the JSON path — the Zig original literally discards it
/// (`_ = res;`). The rule:
///
///   - Text form: when the read set is NON-EMPTY it wins outright and the
///     write resolution is never consulted. Only an EMPTY read set falls
///     through to the write resolution's `--scope` value, then its derived
///     scope, then its `reason` — a four-arm ladder whose lower three arms
///     are only reachable when `resolve_read_scope_set` came back empty.
///   - JSON form: the read set is the ONLY input. `source` is `"flag"`
///     whenever `--scope` was passed (even when the set is empty), `"cwd"`
///     when the set is non-empty, and `"none"` otherwise.
///
/// So `--scope <x>` on the JSON path reports `"source":"flag"` with a
/// populated `resolved_scopes`, while the text path prints the same set
/// under a DIFFERENT heading (`from --scope flag` vs `from cwd`). The
/// heading is the only place the text form records provenance.
///
/// ## `scope show --json` does NOT escape `cwd`, and this port preserves it
///
/// The oracle interpolates the working directory into the JSON payload with
/// a raw `{s}` — `"cwd":"{s}"` — so a path containing a double quote or a
/// backslash emits malformed JSON. This port reproduces that byte-for-byte
/// rather than fixing it, per D2: the fix is a behaviour change, it is
/// observable to any consumer that currently tolerates the raw form, and it
/// belongs to a task that can run the oracle and decide. Recorded as an
/// oracle defect in this cycle's report; the same raw interpolation applies
/// to the `slug` / `reason` fields of `scope suggest --json`.
///
/// ## `scope suggest --json` disagrees with itself on the empty case
///
/// Populated, it emits ONE JSON OBJECT PER LINE with no wrapper. Empty, it
/// emits a single `{"proposals":[]}` object — a key that appears on NO other
/// path, wrapping an array the populated form never produces. A consumer
/// written against either shape breaks on the other. Captured from the
/// oracle, reproduced verbatim, and pinned in `scope_leaves.t.cpp`.
///
/// ## `scope suggest` matches the project root EXACTLY
///
/// `engine::identity::suggest` queries `projects where root_path = ?`, so
/// running it from a SUBDIRECTORY of a registered project yields `no scope
/// suggestions for cwd` even though `scope show` from the same directory
/// resolves a scope by longest-prefix match. The two verbs genuinely
/// disagree about what "the cwd project" means. Verified against the oracle
/// from both a project root and a subdirectory of it.
module;

export module planar.cmd.planar.handlers.scope;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar scope show [--scope <slug>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, the failure otherwise.
export auto scope_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scope suggest [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, the failure otherwise.
export auto scope_suggest(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scope use [<slug>]` — refuses; the scope stack was
/// removed in plan 153 M5.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Always the refusal (exit 2).
export auto scope_use(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scope pop` — refuses; see `scope_use`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Always the refusal (exit 2).
export auto scope_pop(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scope clear` — refuses; see `scope_use`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Always the refusal (exit 2).
export auto scope_clear(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
