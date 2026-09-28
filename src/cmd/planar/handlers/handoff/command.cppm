/// @file src/cmd/planar/handlers/handoff/command.cppm
/// @brief `planar.cmd.planar.handlers.handoff` — the seven
/// `planar handoff *` leaves (plan 996, task 6040).
///
/// Port target: zig/src/cmd/planar/handlers/handoff/{cmd,create,validate,
/// consume,abandon,list,show}.zig.
///
/// ## `handoff` is a GROUP and a LEAF at once
///
/// The parent node carries six subcommands AND its own `run` with its own
/// `<task-id>` positional, and the oracle dispatches the parent when no
/// child is named:
///
///     $ planar handoff       -> exit 2, "no active session (run
///                               `planar capture session` first)"
///
/// `planar.cmd.planar.dispatch` grew a narrow rule for this in the same
/// task — a matched node with children renders help ONLY when the table
/// has no entry for it. See that module's header.
///
/// ## The composite is a four-step ritual, and the ORDER is observable
///
/// `planar handoff [<task-id>]` runs, in this order:
///
///   0. Require an ACTIVE session for the vendor tuple. It does NOT create
///      one. That refusal is the whole reason step 0 exists: an earlier
///      revision auto-started a session here for ergonomics, which masked
///      the missing-session error, and plan 314 task 2296 locked the
///      refusal back in. Reproducing the auto-start would silently undo a
///      deliberate fix.
///   1. Create a `context_snapshots` row.
///   2. Create a `pending` handoff on it, carrying worktree context copied
///      from the active claim.
///   3. Validate it (pending -> validated).
///   4. Append a `handoff captured: snapshot=N handoff=M` session note,
///      BEST-EFFORT — its failure is swallowed.
///
/// Then it runs the resumability check and reports it. The check is
/// advisory: a NON-resumable task still produces a validated handoff and
/// still exits 0. Only `resume validate` turns non-resumability into a
/// non-zero exit.
///
/// ## Two renderers for one dataset, and they genuinely differ
///
/// The composite's `--json` emits `"failures":[]` for a resumable task.
/// `resume validate --json` emits `"failures":null` for the same state.
/// Both are oracle-captured and both are deliberate (the `resume validate`
/// shape is Go-parity, and the Zig source says so at the call site). They
/// must not be merged into one renderer.
///
/// ## Exit codes, oracle-captured
///
///   no active session                  exit 2
///     `no active session (run `planar capture session` first)`
///   `<task-id>` not an integer         exit 2
///     `task id must be an integer, got 'x'`
///   task named but absent              exit 1  `task N not found`
///   `handoff show|validate|...` absent exit 1  `handoff N not found`
///   validate on a terminal handoff     exit 1
///     `handoff N cannot transition to validated`
///   consume on a terminal handoff      exit 1  `handoff N is terminal; cannot consume`
///   abandon on a terminal handoff      exit 1  `handoff N is terminal; cannot abandon`
///   `--status bogus`                   exit 2  `unknown handoff status 'bogus'`
///   `create <snapshot>` absent         exit 1  `snapshot N not found`
///
/// Every `<id>` positional is declared as a STRING and parsed in the
/// handler, for the reason `unlink` already documents: an int validator in
/// the tree moves the failure a layer earlier and changes both the message
/// and the exit code.
module;

export module planar.cmd.planar.handlers.handoff;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar handoff [<task-id>] [--vendor v] [--note n] [--json]` —
/// the composite snapshot-create-validate-note ritual.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff create <snapshot-id> [--vendor v] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_create(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff validate <handoff-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff consume <handoff-id> [--session N] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_consume(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff abandon <handoff-id> [--reason r] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_abandon(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff list [--status s[,s...]] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar handoff show <handoff-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto handoff_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `handoff` command tree on `root`.
///
/// The CLI declaration for every `handoff` node, colocated with the
/// handlers above (plan 1051, M11.3d — decision 1068). All seven were
/// SHADOWED before this fold — hand-declared in `tree.cpp` AND described
/// by a `node_spec` `apply_surface` skipped — and the two agreed on every
/// name, kind, default and description.
///
/// They did NOT agree on the order the three parent flags reach each
/// CHILD, and that difference is visible in `--help` even though the
/// `schema` catalog hides it (`render_flags` emits inherited flags first
/// regardless of declaration order). The hand declaration puts them first;
/// `apply_surface` appends ancestor flags AFTER the child's own. The hand
/// order is what ships, so it is what this declaration keeps.
///
/// A DUAL node: six subcommands AND its own `<task-id>` positional and
/// handler. `require_subcommand(0)` allows the bare form and
/// `planar.cmd.planar.dispatch` routes it to the parent's handler.
///
/// ## Inherited flags: redeclared on every child
///
/// `handoff` is the first group in this tree whose PARENT carries flags.
/// etcli inherited a parent's flags into every child both in the catalog
/// AND at parse time; CLI11 does neither. `fallthrough()` fixes parsing
/// but also exposes the parent's POSITIONAL on every child, and on
/// `resume validate` — whose own positional is also named `task-id` — it
/// threw `OptionAlreadyAdded` while the tree was still being BUILT,
/// aborting every invocation of the binary, `planar version` included. So
/// the flags are redeclared on each child instead. The duplicate that
/// creates in the catalog is deduped by `planar.cliapp.schema`.
/// @param root The root app to attach the `handoff` group to.
export auto declare_handoff(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
