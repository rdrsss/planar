/// @file command.cppm
/// @brief `planar.cmd.planar.handlers.tree` — the `planar tree` leaf
/// (plan 996, task 6278).
///
/// Root assembly lives in `planar.cmd.planar.main`; this module owns the
/// `tree` verb alone.
module;

export module planar.cmd.planar.handlers.tree;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief Render the entity hierarchy for one or all scopes.
///
/// Exit contract, every arm CAPTURED from the oracle rather than inferred
/// from a sibling verb (empty flag values mean three different things
/// here — see `planar.engine.tree.walk`'s header):
///   - an empty tree is exit 0 with a label and a zeroed footer, NOT an error
///   - `--kind ''` and `--kind <bogus>` refuse at exit 2
///   - `--scope <unknown>` refuses at exit 1
///   - a cwd outside every registered scope refuses at exit 1
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, or the refusal.
export auto tree(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `tree` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_tree(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
