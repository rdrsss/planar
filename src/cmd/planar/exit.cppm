/// @file exit.cppm
/// @brief `planar.cmd.planar.exit` — the `planar` OPERATOR binary's
/// failure envelope AND its complete exit-code table (plan 996, tasks 6105
/// and 6123).
///
/// ## Task 6123 moved the TABLE in here, on purpose
///
/// This module used to hold only the `domain_error` value and delegate the
/// numbers to layer 1's `planar.cli.exit::exit_code_for(kind,
/// binary_kind)`. That shared, binary-PARAMETERIZED helper is exactly the
/// shape task 6066 spent a rename fighting: overload resolution picked
/// between a binary-aware and a binary-blind `exit_code_for` purely by
/// argument count, so a call site written one token short silently applied
/// the `planar` operator binary's policy from an agent/watch code path.
/// The task 6123 brief is explicit — "do not reintroduce one" — so the
/// switch below is complete, local, and takes no binary parameter. There
/// is no way to reach it from another binary and no argument that could
/// select the wrong row.
///
/// ## This binary's policy, and where it differs
///
/// Verified against the reference binaries rather than inferred:
///
///     parse_error            exit 2 HERE, exit 1 on planar-agent/-watch.
///     schema_version_behind  exit 1 HERE, exit 7 on planar-agent/-watch.
///
/// `planar nosuchverb` exits 2 while `planar-agent nosuchverb` exits 1,
/// from the same argv shape. `zig/src/cmd/planar/exit.zig`'s `codeFor` has
/// NO `SchemaVersionBehind` arm at all — only `SchemaVersionAhead => 7` —
/// so it falls through to the generic `else => 1` bucket, where BOTH
/// `zig/src/cmd/planar-agent/exit.zig` and `zig/src/cmd/planar-watch/
/// exit.zig` fold the two together at 7. Reproduced, not "fixed" (D2).
/// `exit_codes.t.cpp` pins every row.
///
/// ## Rendered payloads vs message bodies
///
/// The M4 boundary review settled (commit 5728133) that an engine renderer
/// returning a COMPLETE payload returns the oracle's exact bytes INCLUDING
/// the terminator, while a renderer returning a FRAGMENT the caller
/// composes carries none. Both shapes exist for stderr text, and both are
/// live in this binary's verb subset:
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
/// build it with `error_from_body` (this module composes `error: <body>\n`)
/// or with `error_from_rendered` (written verbatim, nothing appended).
///
/// ## Why a returned `std::expected`, not Zig's `die()`
///
/// `zig/src/cmd/planar/exit.zig`'s `die(ctx, err, fmt, args)` is
/// `noreturn`. That shape cannot be unit-tested in-process — a Catch2 case
/// reaching it would kill the whole test binary — and it forces the
/// exit-code decision to hundreds of scattered call sites. Handlers here
/// return `std::expected<void, domain_error>` and exactly one place
/// (`dispatch::run`) turns that into bytes and a code.
module;

export module planar.cmd.planar.exit;

import std;

namespace planar::cmd {

/// @brief The domain-error taxonomy this binary's handlers raise.
export enum class domain_error_kind : std::uint8_t {
  generic_failure,
  not_found,
  invalid_input,
  invalid_entity_ref,
  parse_error,
  sync_conflict,
  scope_mismatch,
  slug_conflict,
  already_exists,
  schema_version_ahead,
  schema_version_behind,
  not_implemented,
};

// The exit-code convention, as named constants rather than magic numbers.
export inline constexpr int exit_success               = 0;  ///< Success.
export inline constexpr int exit_generic_failure       = 1;  ///< Unmapped/generic failure.
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
  /// @brief An EXACT process exit code, overriding `kind`'s bucket.
  ///
  /// Unset for every ordinary failure, which is the whole point: the bucket
  /// table above is this binary's policy and nothing should route around
  /// it. It is set by exactly one caller — `workflow run`, which execs
  /// `planar-execute` and must propagate that child's status *exactly*
  /// (`zig/src/cmd/planar/handlers/workflow/run.zig` ends in
  /// `std.process.exit(code)`). A workflow's `flow.fail` can end in a code
  /// no `domain_error_kind` names, and mapping it into the nearest bucket
  /// would silently rewrite a caller-visible status.
  std::optional<int> passthrough_code;
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

/// @brief The process exit code for `kind`, under the `planar` OPERATOR
/// binary's policy. Complete and local — see this file's header for why it
/// takes no binary parameter.
/// @param kind The domain-error kind a handler raised.
/// @return The process exit code.
export auto exit_code_for(domain_error_kind kind) -> int {
  switch (kind) {
  case domain_error_kind::generic_failure:
  case domain_error_kind::not_found:
    return exit_generic_failure;
  case domain_error_kind::invalid_input:
  case domain_error_kind::invalid_entity_ref:
    return exit_user_input;
  case domain_error_kind::parse_error:
    // The OPERATOR binary maps every parse failure to 2. The other two
    // binaries map it to 1 — see this file's header.
    return exit_user_input;
  case domain_error_kind::sync_conflict:
    return exit_sync_conflict;
  case domain_error_kind::scope_mismatch:
    return exit_scope_violation;
  case domain_error_kind::slug_conflict:
  case domain_error_kind::already_exists:
    return exit_precondition_conflict;
  case domain_error_kind::schema_version_ahead:
    return exit_schema_version;
  case domain_error_kind::schema_version_behind:
    // NO arm on the Zig side: falls through to the generic 1 bucket, where
    // planar-agent/-watch both fold it into the same 7 as `ahead`.
    return exit_generic_failure;
  case domain_error_kind::not_implemented:
    return exit_not_implemented;
  }
  return exit_generic_failure;
}

/// @brief The process exit code for `err`.
/// @param err The handler failure.
/// @return The process exit code.
export auto exit_code(const domain_error& err) -> int {
  // The override is consulted BEFORE the bucket table, and only ever set by
  // a handler that is propagating another process's status verbatim. See
  // `domain_error::passthrough_code`.
  if (err.passthrough_code.has_value()) {
    return *err.passthrough_code;
  }
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

} // namespace planar::cmd
