/// @file skills.cppm
/// @brief `planar.cmd.planar.handlers.skills` — the `planar skills` leaf
/// (plan 996, task 6106).
///
/// Port target: zig/src/cmd/planar/handlers/skills/cmd.zig, which is
/// twelve lines and declares no `.run` at all.
///
/// ## A verb that does nothing, and why it still needs a handler
///
/// Plan 918 M5 retired `skills render` / `status` / `repair` — scriptorium
/// is the sole renderer and owns install-drift detection through its own
/// manifest. The node stays registered as a placeholder so `planar skills`
/// reports "no subcommands" rather than an unknown-verb error.
///
/// On the Zig side that falls out for free: a `Cmd` with no children and
/// no `.run` renders its own help page. In this tree it does not, and the
/// difference is how a leaf is defined here — `planar.cliapp.args::
/// leaf_keys` classifies a `CLI::App` with no subcommands as a LEAF, so
/// `skills` is matched, routed, and would hit
/// `dispatch::run`'s unregistered-leaf arm: `error: not implemented yet`,
/// exit 64. Against the oracle's exit 0 and a help page, that is a real
/// divergence, and `unregistered_leaves` would have failed the build
/// first. So the placeholder needs a handler that renders the placeholder.
///
/// ## It rebuilds the tree rather than widening the handler signature
///
/// `CLI::App::help()` is a member of the NODE, and `handler_fn` carries
/// only `(context&, const cliapp::parsed_args&)` — no tree. Two ways to
/// close that: thread the root into every handler, or let this one handler
/// call `root_app()`. Threading it would change the signature of every verb
/// in the binary to serve a single retired placeholder whose whole output
/// is a static page; the cost of rebuilding the tree is one vector of
/// nodes on a path that then does nothing else. If a second leaf ever
/// needs the root, revisit the signature then — with two call sites to
/// justify it.
module;

export module planar.cmd.planar.handlers.skills;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar skills` — render the retired verb's own help
/// page, exit 0.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @return Success; this leaf has no failure path and never opens the
/// database.
export auto skills(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `skills` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_skills(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
