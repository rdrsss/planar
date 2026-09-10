/// @file exit.cppm
/// @brief `planar.cmd.planar_ext.exit` — the `planar-ext` binary's failure
/// envelope and exit-code table (plan 996, task 6418).
///
/// `planar-ext` has no Zig-tree oracle — it is a NEW binary this task
/// stands up, not a port — so this table is not oracle-captured the way
/// its three siblings' are. It is deliberately modeled on
/// `planar.cmd.planar_agent.exit`'s policy rather than invented from
/// scratch: `planar-ext` is, like `planar-agent`, a schema CONSUMER that
/// refuses on skew in BOTH directions (see `planar.cmd.planar_ext.context`
/// for why), so reusing the same bucket layout keeps one exit-code
/// vocabulary across the two consumer binaries instead of a second,
/// gratuitously different one.
///
/// Per task 6123's finding on the other three binaries: the table lives
/// HERE, complete and local, taking no binary parameter — a shared,
/// binary-parameterized helper is exactly the shape that let a call site
/// silently apply the wrong binary's policy by one defaulted argument.
///
/// ## Task 6419 widened the taxonomy to match the moved verbs
///
/// The skeleton (task 6418) declared only the five buckets `version`/
/// `schema` could ever raise. Task 6419 moved the `ext`/`sync` verb family
/// in, and their handlers raise `not_found`, `invalid_input`,
/// `scope_mismatch`, `slug_conflict` and `sync_conflict` too — the exact
/// five `planar-agent` already carries, mapped through the SAME buckets
/// (`exit_user_input`, `exit_scope_violation`, `exit_precondition_conflict`,
/// `exit_sync_conflict`), per this file's header note that `planar-ext`
/// models `planar-agent`'s policy rather than `planar`'s (the two disagree
/// on `parse_error` and `schema_version_behind`; this binary keeps
/// `planar-agent`'s choice on both, verified against that binary's own
/// `exit_code_for`).
module;

export module planar.cmd.planar_ext.exit;

import std;

namespace planar::cmd::ext {

/// @brief The domain-error taxonomy this binary's handlers raise.
export enum class domain_error_kind : std::uint8_t {
  generic_failure,
  not_found,
  invalid_input,
  scope_mismatch,
  parse_error,
  slug_conflict,
  sync_conflict,
  schema_version_ahead,
  schema_version_behind,
  not_implemented,
};

// The exit-code convention, as named constants rather than magic numbers.
export inline constexpr int exit_success               = 0;  ///< Success.
export inline constexpr int exit_generic_failure       = 1;  ///< Unmapped/generic failure, and parse errors.
export inline constexpr int exit_user_input            = 2;  ///< Bad flag value / invalid entity ref.
export inline constexpr int exit_sync_conflict         = 3;  ///< Operational-plane sync conflict.
export inline constexpr int exit_scope_violation       = 5;  ///< Cross-scope write refused.
export inline constexpr int exit_precondition_conflict = 6;  ///< Slug conflict / already-exists.
export inline constexpr int exit_schema_version        = 7;  ///< DB schema newer/older than this binary supports.
export inline constexpr int exit_not_implemented       = 64; ///< Placeholder / not-yet-implemented handler.

/// @brief A handler failure: which exit-code bucket it falls in, plus the
/// stderr text.
export struct domain_error {
  /// @brief The exit-code bucket.
  domain_error_kind kind = domain_error_kind::generic_failure;
  /// @brief The stderr text. Interpreted per `rendered`.
  std::string text;
  /// @brief When true, `text` is a COMPLETE stderr payload written
  /// verbatim. When false, the reporting site composes
  /// `"error: " + text + "\n"` around it.
  bool rendered = false;
};

/// @brief Build a `domain_error` from a message BODY — no `error: `
/// prefix, no trailing newline. The reporting site adds both.
/// @param kind The exit-code bucket.
/// @param body The message body.
/// @return The constructed error.
export auto error_from_body(domain_error_kind kind, std::string body) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(body), .rendered = false};
}

/// @brief Build a `domain_error` from a COMPLETE renderer payload —
/// written to stderr verbatim, with nothing appended.
/// @param kind The exit-code bucket.
/// @param payload The complete stderr payload, terminator included.
/// @return The constructed error.
export auto error_from_rendered(domain_error_kind kind, std::string payload) -> domain_error {
  return domain_error{.kind = kind, .text = std::move(payload), .rendered = true};
}

/// @brief The process exit code for `kind`, under the `planar-ext`
/// binary's policy.
/// @param kind The domain-error kind a handler raised.
/// @return The process exit code.
export auto exit_code_for(domain_error_kind kind) -> int {
  switch (kind) {
  case domain_error_kind::generic_failure:
  case domain_error_kind::not_found:
    return exit_generic_failure;
  case domain_error_kind::invalid_input:
    return exit_user_input;
  case domain_error_kind::parse_error:
    return exit_generic_failure;
  case domain_error_kind::sync_conflict:
    return exit_sync_conflict;
  case domain_error_kind::scope_mismatch:
    return exit_scope_violation;
  case domain_error_kind::slug_conflict:
    return exit_precondition_conflict;
  case domain_error_kind::schema_version_ahead:
  case domain_error_kind::schema_version_behind:
    return exit_schema_version;
  case domain_error_kind::not_implemented:
    return exit_not_implemented;
  }
  return exit_generic_failure;
}

/// @brief The process exit code for `err`.
/// @param err The handler failure.
/// @return The process exit code.
export auto exit_code(const domain_error& err) -> int {
  return exit_code_for(err.kind);
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

} // namespace planar::cmd::ext
