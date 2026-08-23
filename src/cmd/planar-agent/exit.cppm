/// @file exit.cppm
/// @brief `planar.cmd.planar_agent.exit` — the `planar-agent` binary's
/// failure envelope: a typed domain error a handler returns, its process
/// exit code under THIS binary's policy, and how it reaches stderr
/// (plan 996, task 6107).
///
/// ## Why this is a second module and not a parameter on the first
///
/// `planar.cmd.planar.exit` (task 6105) is byte-for-byte the same SHAPE:
/// same `domain_error`, same `from_body`/`from_rendered` split, same
/// `report`. The only thing that differs is the single line inside
/// `exit_code`, which passes `binary_kind::planar_agent` instead of
/// `binary_kind::planar`. Sharing the module and taking the binary as a
/// parameter was considered and rejected, and the reason is recorded in
/// that module's own header: the divergences are real and silent.
///
///   `parse_error`           exit 2 on `planar`, exit 1 here.
///   `schema_version_behind` exit 1 on `planar`, exit 7 here.
///
/// Both were verified against the reference binaries, not inferred — see
/// `planar.cli.exit`'s header for the captures, and this task re-confirmed
/// the first one directly: `planar-agent nosuchverb` exits 1 while `planar
/// nosuchverb` exits 2, from the same argv shape. A shared helper carrying
/// a `binary_kind` argument is one defaulted parameter or one copy-pasted
/// call site away from applying the wrong policy, with nothing but a code
/// review standing between the mistake and shipping it. Layer 1 already
/// owns the TABLE (`planar.cli.exit::exit_code_for`) so the numbers are not
/// duplicated; what is duplicated is a one-line binding of this binary to
/// its own row of it, in a file that is compiled into this binary alone and
/// cannot be reached from another.
///
/// That is also why `cmd_* -> cmd_*` being forbidden (D18) is not a problem
/// to work around here: the thing the four binaries appear to "share" is
/// exactly the thing that must not be shared.
module;

export module planar.cmd.planar_agent.exit;

import std;
import planar.cli;

namespace planar::cmd::agent {

/// @brief A handler failure: which exit-code bucket it falls in, plus the
/// stderr text.
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

/// @brief The process exit code for `err`, under the `planar-agent`
/// binary's policy.
///
/// Hard-wired to `binary_kind::planar_agent` for the reason this file's
/// header gives at length: the policy differs from `planar`'s on two kinds
/// and the difference is invisible at a call site.
/// @param err The handler failure.
/// @return The process exit code.
export auto exit_code(const domain_error& err) -> int {
  return cli::exit_code_for(err.kind, cli::binary_kind::planar_agent);
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

} // namespace planar::cmd::agent
