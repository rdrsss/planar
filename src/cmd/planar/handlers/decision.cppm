/// @file decision.cppm
/// @brief `planar.cmd.planar.handlers.decision` — the seven `planar
/// decision` CRUD/transition/link leaves (plan 996 roadmap M12 item 8,
/// task 6194).
///
/// Port target: zig/src/cmd/planar/handlers/decision/{add,show,list,accept,
/// superseded,withdraw,link}.zig. (The `supersede` VERB's source file is
/// spelled `superseded.zig` in the Zig tree; the verb is `supersede`.)
///
/// ## The family is ELEVEN leaves; seven land here, four are named refusals
///
/// `decision edit`, `decision view`, `decision diff` and `decision review`
/// are the workbench DRAFTING quartet every planning entity carries. They
/// round-trip the row through a file under `$PLANAR_WORKBENCH_ROOT`, diff
/// the file against the row, and gate the re-import — `engine_workbench`
/// plumbing plus an editflow layer, neither ported. They stay declared
/// exit-64 refusals that name themselves, and `dispatch.t.cpp` asserts each
/// one INDIVIDUALLY rather than trusting the count.
///
/// Running the oracle's four is worth recording, because two of them are
/// broken there: `decision edit 3` and `decision view 3` abort with an
/// unhandled `error.NotFound` and a Zig STACK TRACE on stderr (editflow's
/// `walkToAnchor` cannot resolve an anchor plan for an unlinked decision),
/// while `decision diff 3` and `decision review 3` report `no decision with
/// id 3` for a decision that DOES exist. So the quartet has no coherent
/// contract to port even if the plumbing existed.
///
/// ## `--status` and `--scope` on `decision list` are SINGLE-VALUED
///
/// `question list` and `plan list` both comma-split these two flags.
/// `decision list` does not: `--status proposed,accepted` is refused as
/// `unknown status 'proposed,accepted'` and `--scope global,repo:foo`
/// fails `SlugNotFound`. Both captured by running them. The flag specs
/// declare all three as plain strings and cannot express the difference,
/// so this is exactly the kind of thing a copy from `question.cpp` would
/// have got wrong silently.
///
/// `--status` also disagrees with `question list` on its EXIT CODE for an
/// unknown token: `decision list --status bogus` exits **2**, because zig's
/// list.zig raises `error.InvalidInput`, where question's engine-side
/// `error.InvalidStatus` falls to the generic 1 bucket. One flag name, two
/// verbs, two codes.
///
/// ## Where the scope comes from
///
/// `decision add` cwd-derives its write scope when `--scope` is absent and
/// — like `question add`, unlike `plan create` — does NOT refuse inside a
/// project with no association: the row lands `global`. Confirmed by
/// running it in a registered-but-unassociated project.
///
/// `decision list` cwd-derives a READ SET when `--scope` is absent and
/// refuses outright when the cwd is in no registered scope. An explicit
/// `--scope` REPLACES the set (it fills the engine filter's SINGULAR field
/// and leaves the vector empty — the mirror image of what `question
/// list`'s handler does with the same flag).
///
/// `decision accept`, `decision withdraw`, `decision supersede` and
/// `decision link` all DECLARE `--scope` and all IGNORE it. The oracle
/// accepts `--scope nosuchslug` on each and proceeds; that is deliberate
/// CLI parity in the original ("first-cut decisions are global, so the
/// engine ignores it") and is reproduced.
module;

export module planar.cmd.planar.handlers.decision;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar decision add <title> [--body --rationale --plan --scope --editor --json]`.
///
/// `--body` is REQUIRED by this verb even though the flag is declared
/// optional: without it the oracle exits 2 with `--body is required (or run
/// interactively to use the editor flow)`. `decisions.body` is `not null`
/// and the engine synthesizes no placeholder.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision show <decision-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision list [--scope --status --plan --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision accept <decision-id> [--scope --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_accept(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision withdraw <decision-id> [--scope --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_withdraw(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision supersede <decision-id> --by <new-id> [--scope --json]`.
///
/// The verb renders the OLD decision, not the new one, and its three
/// refusals each carry their own wording — see the implementation, where
/// every one is oracle-quoted.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_supersede(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision link <decision-id> <ref> --relationship <rel>
/// [--scope <s>] [--json]`.
///
/// One arm of the shared entity-link surface; the whole body lives in
/// `planar.cmd.planar.handlers.links::entity_link_verb`, which this
/// forwards to with this verb's subject kind, JSON key and arrow. This is
/// the ONLY place the decision family composes `engine_entitylink`, and it
/// is at layer 3 — `decision supersede`'s edge is written by the engine
/// itself, for the two reasons decision.cppm's header gives.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, an
/// absent `--relationship`, an unknown relationship or a malformed ref, or
/// `generic_failure` (exit 1) for a duplicate link or a missing endpoint.
export auto decision_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `decision` command tree on `root`.
///
/// The CLI declaration for every `decision` node, colocated with the
/// handlers above (plan 1051, M11.3c — decision 1068). All twelve came from
/// `surface.cpp`'s generated table; none was hand-declared in `tree.cpp`.
/// Note the order: `edit`, `view`, `diff` and `review` sit AFTER
/// `withdraw`, not next to `add`, because that is where the catalog puts
/// them — they are handled in `handlers/drafting.cpp` and declared here.
/// @param root The root app to attach the `decision` group to.
export auto declare_decision(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
