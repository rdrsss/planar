/// @file src/cmd/planar/handlers/scope/command.cppm
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
/// ## Every `--json` string is escaped through `planar.json_text` (task 6254)
///
/// It did not used to be. The oracle interpolated the working directory
/// with a raw `{s}` — `"cwd":"{s}"` — and `scope suggest` did the same to
/// `slug` and `reason`, so a path or an association slug containing a
/// double quote or a backslash emitted malformed JSON. This port
/// reproduced that byte-for-byte under D2, whose reason to exist was the
/// runtime differential lane against the oracle. Decision 1090 authorises
/// the fix and the pin rewrite for this row, applying the reasoning 1067
/// applied to its own nine: once the oracle was deleted D2's rule no longer
/// decides divergences with real consequences, and emitting unparseable output from the one flag whose entire contract
/// is "this parses" is one — on Windows, or any path carrying a backslash,
/// it was broken by default rather than as an edge case.
///
/// ## `scope suggest --json` is NDJSON, including when empty (task 6257)
///
/// Populated, it emits ONE JSON OBJECT PER LINE with no wrapper. Empty, it
/// emits NOTHING — zero bytes, exit 0. N lines for N results, and N may be
/// zero.
///
/// It used to emit a single `{"proposals":[]}` object when empty — a key
/// that appeared on NO other path, wrapping an array the populated form
/// never produced — so a consumer written against either shape broke on the
/// other, and the empty case is the one people write their parser against
/// first because it is the easy fixture. The rule chosen for the whole
/// shape-split family (6257 `scope suggest`, 6270 `links list`, 6326 `assoc
/// detect`, which disagreed with each other AND with themselves) is
/// NDJSON-with-zero-lines, for two reasons: it leaves the POPULATED bytes —
/// the shape field consumers actually read — untouched, and it is what
/// `links list --json` already did, so it is the majority behaviour rather
/// than a fourth invention. Pinned in `scope_leaves.t.cpp`.
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

/// @brief Declare the `scope` command tree on `root`.
///
/// The CLI declaration for every `scope` node, colocated with the
/// `scope_*` handlers above (plan 1051, M11.3e — decision 1068). Nothing
/// under `scope` was ever hand-declared in `tree.cpp`, so all six came
/// from `surface.cpp`'s generated table and no shadowed pair had to be
/// reconciled.
///
/// `use`, `pop` and `clear` are the tree's ONLY three `allow_extras`
/// leaves, which is why `planar.cmd.planar.declare` grew
/// `set_allow_extras` for this wave. See that function's header for what
/// the property buys: all three were removed in plan 153 M5 and answer
/// with a fixed refusal whatever a pre-M5 caller still passes them, so
/// CLI11 must not refuse the stale arguments at parse time first.
/// @param root The root app to attach the `scope` group to.
export auto declare_scope(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
