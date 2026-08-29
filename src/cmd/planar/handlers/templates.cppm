/// @file templates.cppm
/// @brief `planar.cmd.planar.handlers.templates` — all six
/// `planar templates` leaves (plan 996, task 6190).
///
/// Port target:
/// `zig/src/cmd/planar/handlers/templates/{list,show,render,validate,init,path,common}.zig`.
///
/// ## This layer is where the two halves of the template plane meet
///
/// The plane is split across two LAYER-2 buckets and neither may import the
/// other (`cmake/architecture.cmake`'s D15 FATALs on an engine→engine
/// edge):
///
///   `planar.engine.config.templates`  RESOLUTION — the three-level
///                                     `<set>` → `default` → embedded
///                                     fallback chain, and the disk and
///                                     embedded enumerators.
///   `planar.engine.templates`         RENDERING — the JSON DOM, the
///                                     `{{...}}` substituter, the
///                                     validator, the context builder, the
///                                     disk extractor, and the six leaves'
///                                     byte-exact output.
///
/// Composing them is layer 3's job, which is this file. It is also the only
/// place that maps `config::template_entry` onto `templates::list_row` and
/// `config::template_file` onto `templates::embedded_file`.
///
/// ## Only ONE of the six opens SQLite
///
/// `templates render` calls `ctx.ensure_db()`; the other five never do, and
/// `ctx.db_opened()` is still false after each of them runs. That is not
/// incidental — `templates list` on a machine with no database must still
/// work, because listing what the binary ships cannot depend on the
/// operator having run `planar init`. Pinned.
///
/// ## The templates root, and the variable that is NOT consulted
///
/// The root is `templates.dir` from the resolved configuration —
/// `$PLANAR_TEMPLATES_DIR`, else `~/.planar/config.toml`'s `[templates]
/// dir`, else the embedded default `~/.planar/templates` — with a leading
/// `~` expanded against **`$HOME`**.
///
/// `$PLANAR_HOME` is NOT consulted, by this family or by the config layer
/// underneath it. Oracle-confirmed: under a pinned arena with `PLANAR_HOME`
/// and `HOME` pointing at different directories, `templates path` printed
/// the path under **`HOME`**. An implementation that reached for
/// `PLANAR_HOME` — the obvious guess, and what most of Planar does — would
/// send `templates init` to write ten files into the wrong tree.
///
/// ## Two leaves declare flags they do not use
///
/// `templates path` ignores `--system`, `--set` AND `--json`; `templates
/// init` ignores `--force`. Both reproduced from the oracle rather than
/// implemented. See `planar.engine.templates`'s CMakeLists for the full
/// list of reproduced oracle defects in this family, including the
/// 128-byte render failure.

module;

export module planar.cmd.planar.handlers.templates;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar templates list [--system <s>] [--set <s>] [--json]`.
///
/// An EMPTY `--system`/`--set` is no filter at all, not a match against the
/// empty string.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the templates root
/// cannot be resolved. Never opens the database.
export auto templates_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar templates show <set> <system> <kind> [--json]`.
///
/// Writes the template's RAW bytes — never a re-encoded round trip — so the
/// operator's own formatting survives. `--json` is accepted and changes
/// nothing, because the payload is already JSON.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `not_found` (exit 1) when no level of the
/// resolution chain matched. Never opens the database.
export auto templates_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar templates render <set> <system> <kind> --entity
/// <kind>:<id> [--json]`.
///
/// The ONLY leaf in this family that opens SQLite, and it only reads.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for a malformed or
/// unsupported `--entity`, a `not_found` (exit 1) for an absent template or
/// entity, or a `generic_failure` (exit 1) when the render itself fails.
export auto templates_render(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar templates validate <set> <system> <kind> [--json]`.
///
/// Issue detail goes to STDOUT and the count to STDERR, at exit 2 — see
/// this module's implementation. A clean template is exit 0.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, a `not_found` (exit 1) for an absent template, or an
/// `invalid_input` (exit 2) when the template has issues. Never opens the
/// database.
export auto templates_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar templates init [--force] [--json]`.
///
/// Idempotent, and `--force` is discarded — an existing file is never
/// overwritten under any flag combination.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the root cannot be
/// resolved or a file cannot be written. Never opens the database.
export auto templates_init(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar templates path`.
///
/// Prints the resolved root and nothing else, under every flag combination.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or a `generic_failure` (exit 1) when the root cannot be
/// resolved. Never opens the database.
export auto templates_path(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Resolve the operator's templates directory, for handlers OUTSIDE
/// this family that must render a template.
///
/// `$PLANAR_TEMPLATES_DIR` > `[templates] dir` in the config file > the
/// embedded default, then tilde-expanded.
///
/// Exported at task 6335 for `ext propagate-one`, which renders through the
/// same loader and MUST resolve the root identically — a second
/// implementation would let the two families disagree about which template
/// set wins, and nothing in the state differential compares resolution
/// order. It forwards exactly one environment variable into the config
/// layer rather than the whole process environment; see the definition on
/// why that hermeticity matters.
/// @param ctx The invocation context.
/// @return The resolved root, or the exit-1 refusal.
export auto templates_root_for(context& ctx) -> std::expected<std::string, domain_error>;

} // namespace planar::cmd::handlers
