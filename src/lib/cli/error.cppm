/// @file error.cppm
/// @brief `planar.cli.error` — parse-error kinds, structured detail, the
/// human-readable formatter, and the exit-code mapping table.
///
/// Behavior-preserving port of zig/vendor/etcli/src/cli/error.zig (D2, D9).
/// `parse_error_kind` and `format_error`'s message wording/ordering are a
/// deliberate line-for-line match of etcli's `Parse` error set and
/// `format()` — these strings are captured, verbatim, from
/// `./zig/zig-out/bin/planar` invocations in `parser.t.cpp` (task brief:
/// derive expected values by running the reference binary, never
/// hand-assumed).
///
/// `exit_code_for_parse_error_planar_binary` reproduces the canonical
/// mapping documented in `zig/src/cmd/planar/exit.zig` (the `planar`
/// operator binary's table — the tech-spec's "exit-code mapping" reference
/// point). Note this table is a *policy* each binary's own `exit.zig`
/// applies to the `Parse` error it receives back from
/// `cli.dispatch`/`cli.parse` — etcli's parser itself never calls
/// `std.process.exit`. `zig/src/cmd/planar-agent/exit.zig` verifiably
/// applies a DIFFERENT policy (falls through to the generic-1 bucket for
/// every `Parse.*` kind except none — verified via `./zig/zig-out/bin/
/// planar-agent fail --reason x` → exit 1, not 2). This module exports the
/// full numeric convention (0/1/2/3/5/6/7/64) as named constants so a
/// caller can reproduce either binary's policy, or its own, without
/// hand-rolling magic numbers; `exit_code_for_parse_error_planar_binary`
/// itself encodes ONLY the `planar`-binary convention (every
/// `parse_error_kind` maps to `exit_user_input`) — see this function's own
/// doc comment for why it is not spelled `exit_code_for` and must not be.
module;

export module planar.cli.error;

import std;
import planar.cli.flag;

namespace planar::cli {

/// @brief Tagged parse-error kinds, mirroring etcli's `Parse` error set.
export enum class parse_error_kind : std::uint8_t {
  unknown_flag,
  missing_value,
  invalid_value,
  missing_required,
  missing_required_positional,
  too_many_positionals,
  unknown_subcommand,
  unexpected_argument,
  duplicate_flag,
  flag_group_violation,
};

/// @brief Structured error context returned alongside a parse failure.
/// Fields mirror etcli's `Detail` struct field-for-field.
export struct parse_error_detail {
  parse_error_kind               kind{};          ///< Which parse-error kind occurred.
  std::optional<std::string>     arg;             ///< The offending argv token, when applicable.
  std::optional<std::string>     flag_name;       ///< The flag's canonical long name, when applicable.
  std::optional<std::string>     positional_name; ///< The positional's name, when applicable.
  std::optional<std::string>     cmd_path;        ///< The matched-so-far command path, space-separated.
  std::optional<std::string>     suggestion;      ///< Nearest known flag/subcommand spelling, if any.
  std::optional<std::string>     message;         ///< Optional custom message from a value validator.
  std::optional<std::string>     group;           ///< The flag-group name, when `kind == flag_group_violation`.
  std::optional<flag_group_mode> group_mode;      ///< The violated flag-group's mode.
  std::vector<std::string>       group_flags;     ///< Canonical long names involved in the group violation.
};

/// @brief Stable snake_case name for a `parse_error_kind`, suitable for
/// JSON/structured-log output. Renaming one is a breaking change (matches
/// etcli's own `kindName` contract note).
/// @param k The error kind to name.
/// @return The kind's stable, machine-readable name.
export auto kind_name(parse_error_kind k) -> std::string_view {
  switch (k) {
  case parse_error_kind::unknown_flag:
    return "unknown_flag";
  case parse_error_kind::missing_value:
    return "missing_value";
  case parse_error_kind::invalid_value:
    return "invalid_value";
  case parse_error_kind::missing_required:
    return "missing_required";
  case parse_error_kind::missing_required_positional:
    return "missing_required_positional";
  case parse_error_kind::too_many_positionals:
    return "too_many_positionals";
  case parse_error_kind::unknown_subcommand:
    return "unknown_subcommand";
  case parse_error_kind::unexpected_argument:
    return "unexpected_argument";
  case parse_error_kind::duplicate_flag:
    return "duplicate_flag";
  case parse_error_kind::flag_group_violation:
    return "flag_group_violation";
  }
  return "unknown";
}

namespace detail {

/// @brief The human-readable lead-in clause for a `parse_error_kind`, used
/// by `format_error` (e.g. `parse_error_kind::unknown_flag` → `"unknown
/// flag"`). Distinct from `kind_name`'s stable snake_case identifier.
/// @param k The error kind to render.
/// @return The lead-in clause text.
inline auto lead_in(parse_error_kind k) -> std::string_view {
  switch (k) {
  case parse_error_kind::unknown_flag:
    return "unknown flag";
  case parse_error_kind::missing_value:
    return "flag missing value";
  case parse_error_kind::invalid_value:
    return "invalid value";
  case parse_error_kind::missing_required:
    return "required flag missing";
  case parse_error_kind::missing_required_positional:
    return "required positional missing";
  case parse_error_kind::too_many_positionals:
    return "too many positional arguments";
  case parse_error_kind::unknown_subcommand:
    return "unknown subcommand";
  case parse_error_kind::unexpected_argument:
    return "unexpected argument";
  case parse_error_kind::duplicate_flag:
    return "flag specified more than once";
  case parse_error_kind::flag_group_violation:
    return "flag group violation";
  }
  return "error";
}

/// @brief Stable snake_case name for a `flag_group_mode`, used by
/// `format_error` when rendering a `flag_group_violation`.
/// @param mode The flag-group mode to name.
/// @return The mode's stable, machine-readable name.
inline auto group_mode_name(flag_group_mode mode) -> std::string_view {
  switch (mode) {
  case flag_group_mode::mutually_exclusive:
    return "mutually_exclusive";
  case flag_group_mode::required_one:
    return "required_one";
  case flag_group_mode::required_exactly_one:
    return "required_exactly_one";
  }
  return "unknown";
}

} // namespace detail

/// @brief Render a parse error as `"error: <kind>: <context>...\n"`,
/// matching etcli's `format()` field order exactly: message, flag,
/// positional, group (+mode +flags), arg, cmd_path, suggestion.
/// @param d The structured error detail to render.
/// @return The rendered one-line (trailing-newline) message.
export auto format_error(parse_error_detail const& d) -> std::string {
  std::string out = "error: ";
  out += detail::lead_in(d.kind);
  if (d.message) {
    out += " (" + *d.message + ")";
  }
  if (d.flag_name) {
    out += ": " + *d.flag_name;
  }
  if (d.positional_name) {
    out += ": <" + *d.positional_name + ">";
  }
  if (d.group) {
    out += ": " + *d.group;
    if (d.group_mode) {
      out += " (";
      out += detail::group_mode_name(*d.group_mode);
      out += ")";
    }
    if (!d.group_flags.empty()) {
      out += " [";
      for (std::size_t i = 0; i < d.group_flags.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += d.group_flags[i];
      }
      out += "]";
    }
  }
  if (d.arg) {
    out += " (got " + *d.arg + ")";
  }
  if (d.cmd_path) {
    out += " [in: " + *d.cmd_path + "]";
  }
  if (d.suggestion) {
    out += "; did you mean " + *d.suggestion + "?";
  }
  out += "\n";
  return out;
}

// Exit-code convention (zig/src/cmd/planar/exit.zig, mirrored).
export inline constexpr int exit_success               = 0;  ///< Success.
export inline constexpr int exit_generic_failure       = 1;  ///< Unmapped/generic failure.
export inline constexpr int exit_user_input            = 2;  ///< Parse error / bad flag value / invalid entity ref.
export inline constexpr int exit_sync_conflict         = 3;  ///< Operational-plane sync conflict.
export inline constexpr int exit_scope_violation       = 5;  ///< Cross-scope write refused.
export inline constexpr int exit_precondition_conflict = 6;  ///< Slug conflict / already-exists precondition failure.
export inline constexpr int exit_schema_version_ahead  = 7;  ///< DB schema newer than this binary embeds.
export inline constexpr int exit_not_implemented       = 64; ///< Placeholder / not-yet-implemented handler.

/// @brief Map a `parse_error_kind` to the exit code the `planar` OPERATOR
/// binary's `exit.zig` maps every `cli.Parse.*` error to: every parse-error
/// kind is a user-input failure (`exit_user_input`, 2). See this file's
/// header comment for why this is one binary's policy, not an inherent
/// parser property.
///
/// M2 boundary review (plan 996 task 6066, pre-M3 trap): this function used
/// to be named `exit_code_for(parse_error_kind)`, sharing a name with
/// `planar.cli.exit`'s binary-AWARE `exit_code_for(domain_error_kind,
/// binary_kind)` (both re-exported together through `planar.cli` —
/// cli.cppm). Overload resolution picks the one-argument form purely by
/// argument count, so a call site written as `exit_code_for(err.kind)` from
/// a `planar-agent` code path would silently get THIS function's
/// planar-only policy (2) instead of the agent binary's actual policy (1,
/// task 6063) — a real, silent bug waiting for M3's dispatch wiring to
/// trip over it, not a hypothetical. Renamed so the name itself states its
/// scope and cannot be reached by accident: a `planar-agent` path MUST use
/// `exit_code_for(domain_error_kind::parse_error, binary_kind::
/// planar_agent)` from `planar.cli.exit` instead.
/// @param k The parse-error kind to map.
/// @return `exit_user_input` for every `parse_error_kind` (all ten kinds
/// map identically, matching `zig/src/cmd/planar/exit.zig`'s `codeFor`),
/// under the `planar` OPERATOR binary's policy only.
export auto exit_code_for_parse_error_planar_binary(parse_error_kind k) -> int {
  (void)k;
  return exit_user_input;
}

} // namespace planar::cli
