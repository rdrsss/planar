/// @file exit.cppm
/// @brief `planar.cmd.planar_agent.exit` — the `planar-agent` binary's
/// failure envelope AND its complete exit-code table (plan 996, tasks 6107
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
///     parse_error            exit 2 on `planar`, exit 1 HERE.
///     schema_version_behind  exit 1 on `planar`, exit 7 HERE.
///
/// `planar-agent nosuchverb` exits 1 while `planar nosuchverb` exits 2,
/// from the same argv shape; separately, `planar-agent claim --entity
/// task:abc` exits 2 (InvalidEntityRef) while `--entity bogus:1` exits 1
/// (UnsupportedEntityKind falls through to the generic bucket) — that pair
/// is what makes a collapsed mapping impossible to pass.
///
/// The `parse_error` row was the subject of task 6063: the Zig file's own
/// header USED to document `2 — user-input failure (cli.Parse.*)` while
/// its `codeFor` had no Parse arm and fell through to 1, so the port had a
/// documented intent and an actual behaviour to choose between. Resolved
/// in favour of the behaviour — 1, which is what this table already had —
/// and the Zig header was corrected rather than its code, because adding
/// the arm would change an observable contract every agent-facing caller
/// depends on. Nothing here changed; the disagreement it referenced is
/// simply gone.
/// `zig/src/cmd/planar-agent/exit.zig` folds `SchemaVersionBehind` into
/// the same 7 as `SchemaVersionAhead`, where `zig/src/cmd/planar/exit.zig`
/// has no `SchemaVersionBehind` arm at all and falls through to its
/// generic 1. Reproduced, not "fixed" (D2). `exit_codes.t.cpp` pins every
/// row.
///
/// ## Why a returned `std::expected`, not Zig's `die()`
///
/// `zig/src/cmd/planar-agent/exit.zig`'s `die(ctx, err, fmt, args)` is
/// `noreturn`. That shape cannot be unit-tested in-process — a Catch2 case
/// reaching it would kill the whole test binary — and it forces the
/// exit-code decision to hundreds of scattered call sites. Handlers here
/// return `std::expected<void, domain_error>` and exactly one place
/// (`dispatch::run`) turns that into bytes and a code.
module;

export module planar.cmd.planar_agent.exit;

import std;
import planar.json_text;

namespace planar::cmd::agent {

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

/// @brief The process exit code for `kind`, under the `planar-agent`
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
    // NOT the operator binary's 2. Oracle-captured on THIS binary.
    return exit_generic_failure;
  case domain_error_kind::sync_conflict:
    return exit_sync_conflict;
  case domain_error_kind::scope_mismatch:
    return exit_scope_violation;
  case domain_error_kind::slug_conflict:
  case domain_error_kind::already_exists:
    return exit_precondition_conflict;
  case domain_error_kind::schema_version_ahead:
  case domain_error_kind::schema_version_behind:
    // Both fold to 7 here, where `planar` has no `behind` arm at all.
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

/// @brief `kind`'s own enumerator spelling (task 6844), used as the JSON
/// error envelope's `tag` when `err.text` does not itself end in a bare
/// CamelCase tag (see `derive_tag`).
/// @param kind The domain-error kind.
/// @return The kind's snake_case name.
export auto kind_name(domain_error_kind kind) -> std::string_view {
  switch (kind) {
  case domain_error_kind::generic_failure:
    return "generic_failure";
  case domain_error_kind::not_found:
    return "not_found";
  case domain_error_kind::invalid_input:
    return "invalid_input";
  case domain_error_kind::invalid_entity_ref:
    return "invalid_entity_ref";
  case domain_error_kind::parse_error:
    return "parse_error";
  case domain_error_kind::sync_conflict:
    return "sync_conflict";
  case domain_error_kind::scope_mismatch:
    return "scope_mismatch";
  case domain_error_kind::slug_conflict:
    return "slug_conflict";
  case domain_error_kind::already_exists:
    return "already_exists";
  case domain_error_kind::schema_version_ahead:
    return "schema_version_ahead";
  case domain_error_kind::schema_version_behind:
    return "schema_version_behind";
  case domain_error_kind::not_implemented:
    return "not_implemented";
  }
  return "generic_failure";
}

namespace {

/// @brief True when every character of `s` is ASCII alphanumeric and the
/// first is a letter -- the shape of a bare Zig-style error tag
/// (`Busy`, `ClaimNotActive`, `QueryFailed`, `SchemaVersionBehind`).
auto looks_like_bare_tag(std::string_view s) -> bool {
  if (s.empty() || (std::isalpha(static_cast<unsigned char>(s.front())) == 0)) {
    return false;
  }
  return std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

/// @brief The JSON error envelope's `tag` for this binary (task 6844).
///
/// Almost every handler failure here already ends its (unrendered) text in
/// the exact CamelCase Zig-style tag the pinned `error: <verb>: <Tag>` line
/// prints (`handlers.support`'s `verb_error`/`session_error_message`, the
/// schema-handshake bodies in `context.cpp`, and more) -- reusing that tag
/// is strictly MORE useful to a caller than this binary's own
/// `domain_error_kind`, which collapses nearly every one of them into the
/// single `generic_failure` bucket (task 6843's whole point was giving
/// `Busy` its own distinct tag; an envelope that reported `generic_failure`
/// for it would erase that distinction again). A body that is not in that
/// shape (prose, or containing punctuation/spaces after the last `: `)
/// falls back to `kind_name(err.kind)`.
/// @param err The handler failure.
/// @return The envelope's `tag` value.
export auto derive_tag(const domain_error& err) -> std::string {
  if (!err.rendered) {
    auto const             sep = err.text.rfind(": ");
    std::string_view const candidate =
        sep == std::string::npos ? std::string_view{err.text} : std::string_view{err.text}.substr(sep + 2);
    if (looks_like_bare_tag(candidate)) {
      return std::string{candidate};
    }
  }
  return std::string{kind_name(err.kind)};
}

/// @brief Writes the additive `--json` error envelope (task 6844, decision
/// 1145, supersedes D5): one line, `{"error":{"verb":"<verb>","tag":"<tag>"}}`,
/// to `out_stream`. `tag` is `derive_tag(err)`.
///
/// This is ADDITIVE ONLY. Callers gate it on `--json` themselves and call
/// it alongside `report()`, never instead of it. The envelope is written
/// to STDOUT -- the same stream a successful handler's `--json` output
/// already uses -- so `report()`'s pinned `error: <verb>: <Tag>` stderr
/// text and this binary's exit codes stay byte-identical either way
/// (decision 1145: the byte-pinned stderr oracle stays intact; D5's
/// original stderr placement collided with those pins and was reversed).
/// @param verb The resolved verb path (e.g. `"claim"`, `"complete"`).
/// @param err The handler failure.
/// @param out_stream The stream to write to (stdout).
export auto report_json_envelope(std::string_view verb, const domain_error& err, std::ostream& out_stream) -> void {
  out_stream << R"({"error":{"verb":)" << json_text::json_string(verb) << R"(,"tag":)" << json_text::json_string(derive_tag(err))
             << "}}\n";
}

} // namespace planar::cmd::agent
