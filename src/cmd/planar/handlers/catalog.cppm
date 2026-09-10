/// @file catalog.cppm
/// @brief `planar.cmd.planar.handlers.catalog` — the two `planar` leaves
/// that describe the command TREE rather than the database: `schema` and
/// `completion` (plan 996, task 6065).
///
/// ## Why they land in a task about DECLARING a surface
///
/// `schema` is not optional here. This task's whole point is that
/// `zig/tools/cli_usage_lint` must be able to read this binary's catalog —
/// and until now `planar schema` did not exist at all, so
/// `src/cmd/catalog_parity.hpp` could compare `planar-agent` and
/// `planar-watch` against the oracle but had nothing at all to compare for
/// the binary with 223 of the 260 leaves. `planar schema` was the blind
/// spot in the parity harness, not merely a missing verb.
///
/// `completion` comes with it because it is the same shape — a pure
/// function of a built `CLI::App`, already implemented at layer 1
/// (`planar.cliapp.completion`), with the identical handler its
/// `planar-watch` twin already has. Declaring it and leaving it at exit 64
/// would be registering a stub for code that is sitting right there.
///
/// ## Oracle capture (scratch arena, `PLANAR_DB` / `PLANAR_CONFIG_PATH`
/// pinned)
///
///     $ planar completion badshell
///     stderr: "error: unsupported shell 'badshell'; supported: bash, zsh, fish\n"
///     exit:   2                      (invalid_input)
///
///     $ planar completion
///     stdout: "error: required positional missing: <shell>\n"
///     stderr: "error: MissingRequiredPositional\n"
///     exit:   2                      (parse error — this binary's policy)
///
/// Unlike `planar-watch`, where those two paths carry DIFFERENT codes (2
/// and 1), the operator binary answers 2 for both. That is why the
/// `planar-watch` twin's header calls its pair "the discrimination that
/// matters" and this one cannot: here the exit code does not separate
/// them, only the stream split does.
///
/// The generated script is NOT byte-comparable to the oracle's —
/// `planar.cliapp.completion`'s header records why (flag-VALUE completion
/// was already deferred by the emitter this replaced). No test here claims
/// otherwise.
module;

export module planar.cmd.planar.handlers.catalog;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar schema`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @param root The command tree to describe — passed in rather than
/// rebuilt, so the catalog always describes the tree dispatch actually
/// routes through.
/// @return Success; this leaf has no failure path.
export auto schema(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result;

/// @brief Handle `planar completion <shell>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments; `shell` is the required positional.
/// @param root The command tree to generate completions for.
/// @return Success, or an `invalid_input` (exit 2) carrying the oracle's
/// `unsupported shell '<x>'; supported: bash, zsh, fish` body.
export auto completion(context& ctx, const cliapp::parsed_args& args, const CLI::App& root) -> handler_result;

/// @brief Declare the `completion` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_completion(CLI::App& root) -> void;

/// @brief Declare the `schema` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_schema(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
