/// @file exit.cppm
/// @brief `planar.cmd.planar.exit` — the layer-3 failure envelope: a typed
/// domain error a handler returns, its process exit code, and how it
/// reaches stderr (plan 996, task 6105).
///
/// This is the `cmd/` half of the split `planar.cli.exit` already set up.
/// That layer-1 module owns the TABLE (`domain_error_kind` ->
/// `exit_code_for(kind, binary_kind)`, including the two per-binary
/// divergences task 6063 and task 6066 pinned). This module owns the
/// VALUE a handler actually returns and the bytes the operator sees.
///
/// ## Why a returned `std::expected`, not Zig's `die()`
///
/// zig/src/cmd/planar/exit.zig's `die(ctx, err, fmt, args)` is `noreturn`:
/// it prints, tears the runtime down, and calls `std.process.exit`. That
/// shape cannot be unit-tested in-process — a Catch2 case that reached it
/// would kill the whole test binary — and it forces the exit-code decision
/// to be made at hundreds of scattered call sites. Handlers here instead
/// return `std::expected<void, domain_error>`, and exactly one place
/// (`planar.cmd.planar.dispatch`'s `run`) turns that into bytes and a
/// code. The observable contract is identical; the difference is that
/// every failure path is reachable from a test.
///
/// ## Rendered payloads vs message bodies
///
/// The M4 boundary review settled (commit 5728133) that an engine renderer
/// returning a COMPLETE payload returns the oracle's exact bytes INCLUDING
/// the terminator, while a renderer returning a FRAGMENT the caller
/// composes carries none. Both shapes exist for stderr text, and both are
/// live in this task's own verb subset:
///
///   - `planar.engine.workflows.render::not_found_error("nope")` returns
///     the WHOLE line, `"error: workflow 'nope' not found\n"` — prefix and
///     terminator included.
///   - `planar.engine.runs.render::render_run_not_found(...)` documents
///     itself as "the message body (no `error: ` prefix, no trailing
///     newline)".
///
/// A single `std::string message` field cannot express both without the
/// reporting site guessing, and a guess here silently doubles or drops the
/// `error: ` prefix. So `domain_error` carries the distinction explicitly:
/// build it with `from_body` (this module composes `error: <body>\n`) or
/// with `from_rendered` (this module writes the string verbatim and
/// appends nothing, exactly as a stdout renderer payload is written).
module;

export module planar.cmd.planar.exit;

import std;
import planar.cli;

namespace planar::cmd {

/// @brief A handler failure: which exit-code bucket it falls in, plus the
/// stderr text (see this file's header for the two text shapes).
export struct domain_error {
  /// @brief The exit-code bucket, mapped through `planar.cli.exit`.
  cli::domain_error_kind kind = cli::domain_error_kind::generic_failure;
  /// @brief The stderr text. Interpreted per `rendered`.
  std::string text;
  /// @brief When true, `text` is a COMPLETE stderr payload written
  /// verbatim (prefix and terminator already present). When false, `text`
  /// is a message body and the reporting site composes
  /// `"error: " + text + "\n"` around it.
  bool rendered = false;
};

/// @brief Build a `domain_error` from a message BODY — no `error: `
/// prefix, no trailing newline. The reporting site adds both.
/// @param kind The exit-code bucket.
/// @param body The message body.
/// @return The constructed error.
export auto error_from_body(cli::domain_error_kind kind, std::string body) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(body), .rendered = false};
}

/// @brief Build a `domain_error` from a COMPLETE renderer payload —
/// written to stderr verbatim, with nothing appended.
/// @param kind The exit-code bucket.
/// @param payload The complete stderr payload, terminator included.
/// @return The constructed error.
export auto error_from_rendered(cli::domain_error_kind kind, std::string payload) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(payload), .rendered = true};
}

/// @brief The process exit code for `err`, under the `planar` OPERATOR
/// binary's policy.
///
/// Deliberately hard-wired to `binary_kind::planar` rather than taking the
/// binary as a parameter: this module lives in `src/cmd/planar/` and is
/// compiled into that binary alone. `planar-agent` and `planar-watch`
/// (task 6107) get their own `exit` module, and their policies genuinely
/// differ — `parse_error` is exit 1 there, not 2, and
/// `schema_version_behind` is 7 there, not 1 (both verified, see
/// `planar.cli.exit`'s header). A shared, binary-parameterized helper here
/// would be one call site away from silently applying the wrong policy,
/// which is exactly the bug task 6066 renamed
/// `exit_code_for_parse_error_planar_binary` to prevent.
/// @param err The handler failure.
/// @return The process exit code.
export auto exit_code(const domain_error& err) -> int {
  return cli::exit_code_for(err.kind, cli::binary_kind::planar);
}

/// @brief Write `err`'s stderr text to `err_stream`, composing the
/// `error: ` prefix and terminator only when `err.rendered` is false.
/// @param err The handler failure.
/// @param err_stream The stream to write to.
export auto report(const domain_error& err, std::ostream& err_stream) -> void {
  if (err.rendered) {
    err_stream << err.text;
    return;
  }
  err_stream << "error: " << err.text << '\n';
}

} // namespace planar::cmd
