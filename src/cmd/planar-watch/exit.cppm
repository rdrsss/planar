/// @file exit.cppm
/// @brief `planar.cmd.planar_watch.exit` — the `planar-watch` binary's
/// failure envelope (plan 996, task 6107).
///
/// Structurally identical to `planar.cmd.planar_agent.exit`, and mapped
/// through the same row of layer 1's table: `planar.cli.exit`'s
/// `binary_kind` deliberately has NO `planar_watch` enumerator, because
/// zig/src/cmd/planar-watch/exit.zig's policy is byte-for-byte
/// zig/src/cmd/planar-agent/exit.zig's — `Parse.*` falls through to 1, and
/// `SchemaVersionBehind`/`SchemaVersionAhead` both map to 7. That was
/// verified again in this task rather than assumed: `planar-watch
/// nosuchverb` exits 1, `planar nosuchverb` exits 2, `planar-watch
/// completion badshell` exits 2 (invalid input, not a parse error).
///
/// `binary_kind`'s own doc comment states the condition under which
/// `planar_watch` would earn an enumerator: "if `planar_watch` ever gains a
/// dispatch surface with its OWN divergent policy, it earns its own
/// enumerator then, not before." It has not, so this module passes
/// `planar_agent` and says why, rather than inventing an enumerator that
/// would duplicate a row.
///
/// A separate module from the agent's despite the identical body, for the
/// reason `planar.cmd.planar_agent.exit`'s header gives: D18 forbids a
/// `cmd_* -> cmd_*` edge, and what the four binaries appear to share here
/// is precisely the thing that must not be shared.
module;

export module planar.cmd.planar_watch.exit;

import std;
import planar.cli;

namespace planar::cmd::watch {

/// @brief A handler failure: which exit-code bucket it falls in, plus the
/// stderr text.
export struct domain_error {
  /// @brief The exit-code bucket, mapped through `planar.cli.exit`.
  cli::domain_error_kind kind = cli::domain_error_kind::generic_failure;
  /// @brief The stderr text. Interpreted per `rendered`.
  std::string text;
  /// @brief When true, `text` is a COMPLETE stderr payload written
  /// verbatim. When false, the reporting site composes
  /// `"error: " + text + "\n"` around it.
  bool rendered = false;
};

/// @brief Build a `domain_error` from a message BODY.
/// @param kind The exit-code bucket.
/// @param body The message body.
/// @return The constructed error.
export auto error_from_body(cli::domain_error_kind kind, std::string body) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(body), .rendered = false};
}

/// @brief Build a `domain_error` from a COMPLETE renderer payload.
/// @param kind The exit-code bucket.
/// @param payload The complete stderr payload, terminator included.
/// @return The constructed error.
export auto error_from_rendered(cli::domain_error_kind kind, std::string payload) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(payload), .rendered = true};
}

/// @brief The process exit code for `err`, under the `planar-watch`
/// binary's policy (identical to `planar-agent`'s — see this file's
/// header).
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

} // namespace planar::cmd::watch
