/// @file src/cmd/planar/handlers/local/command.cppm
/// @brief `planar.cmd.planar.handlers.local` — all five `planar local`
/// leaves (plan 996, task 6189).
///
/// Port target: zig/src/cmd/planar/handlers/local/{list,link,unlink,import,
/// migrate,common}.zig.
///
/// `planar.engine.local` (task 6109) landed the whole bucket — the walk, the
/// linker, the importer, the migrator AND byte-exact renderers for every one
/// of the five leaves plus every refusal line. What was missing was a caller.
/// These handlers are that caller and nothing more: they resolve the sandbox
/// root from the context's environment, orchestrate the engine calls in the
/// oracle's order, and write the renderers' payloads verbatim.
///
/// ## The environment seam, and the variable that is NOT consulted
///
/// The sandbox root is `$PLANAR_LOCAL_HOME`, else `$HOME`, else a REFUSAL —
/// and deliberately NOT `$PLANAR_HOME`, which this surface has never read.
/// That asymmetry is the most dangerous fact in the bucket (an operator with
/// `PLANAR_HOME` set and `HOME` unset gets a refusal, not a redirect) and it
/// is enforced in `planar.engine.local.manifest.resolve_home_and_root`, which
/// takes the same `env_lookup` callable a `context` carries. Nothing here
/// calls `std::getenv`.
///
/// ## None of the five opens SQLite
///
/// The whole family is filesystem state: two JSON manifests under
/// `<home>/.planar/` and the vendor symlink trees beside them. `ctx.db().opened()`
/// is still false after any of them runs, and that is pinned.
///
/// ## Where the orchestration is load-bearing rather than mechanical
///
/// Four places, each of which a straight-line rewrite gets wrong:
///
/// 1. **`link`'s text-only diagnostics.** Walk errors and lint issues are
///    printed in TEXT MODE ONLY. Under `--json` a malformed sandbox source
///    is invisible to a scripted caller. That asymmetry is the oracle's and
///    is reproduced rather than corrected — see `render::walk_error_text`.
/// 2. **`link`'s named-source refusal.** `local link <name>` with no match is
///    exit 1 and names the root searched. `local link` with no sources at all
///    is exit 0 and a sentence. Two different situations, two different
///    outcomes; collapsing them would make an empty sandbox look like a
///    failure.
/// 3. **`unlink`'s kind fallback.** When the name resolves to neither a skill
///    nor an agent SOURCE, the oracle unlinks BOTH manifests rather than
///    refusing — an install whose source was already deleted is still
///    removable. The two passes are summed, and only a total of zero prints
///    the "already unlinked, or no such name" sentence.
/// 4. **`import`'s second phase.** Unless `--no-link`/`--dry-run`/nothing
///    imported, every imported file is RE-PARSED from its destination and
///    linked, with a `\nLinking imported files into vendor surfaces:\n`
///    banner in text mode and no banner at all under `--json`.
///
/// ## The clock
///
/// `link()` and `reconcile()` take the `linked_at` stamp as a parameter so
/// they stay deterministic; this layer is where `link::utc_now_stamp()` is
/// read. Exactly one read per invocation, shared by every source in the run,
/// so a multi-source `local link` cannot stamp two sources a second apart.
module;

export module planar.cmd.planar.handlers.local;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar local list [--vendor <v>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the sandbox root
/// cannot be resolved or a manifest cannot be read.
export auto local_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar local link [name] [--dry-run] [--vendor <v>]
/// [--reconcile] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for `--reconcile` with a
/// positional, or a `generic_failure` (exit 1) for an unresolvable root or a
/// named source that does not exist.
export auto local_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar local unlink <name> [--purge] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the sandbox root
/// cannot be resolved or a manifest cannot be read.
export auto local_unlink(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar local import <path> [--kind <k>] [--force]
/// [--dry-run] [--no-link] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for a bad `--kind` or an
/// unimportable source, or a `generic_failure` (exit 1) when the source
/// resolved to zero entries.
export auto local_import(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar local migrate [--dry-run] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the sandbox root
/// cannot be resolved.
export auto local_migrate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `local` command tree on `root`.
///
/// The CLI declaration for every `local` node, colocated with the
/// `local_*` handlers above (plan 1051, M11.3e — decision 1068). None was
/// hand-declared in `tree.cpp`.
///
/// `import`'s `--no-link` is a genuine flag name, not a negation:
/// `planar.cliapp.surface::add_bool_flag` synthesizes `--no-no-link` as
/// ITS negation, so the two never collide.
/// @param root The root app to attach the `local` group to.
export auto declare_local(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
