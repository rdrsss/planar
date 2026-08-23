/// @file args.cppm
/// @brief `planar.cmd.planar.args` — typed accessors over a parsed
/// `cli::match_result` (plan 996, task 6105).
///
/// etcli generates a distinct comptime `ArgsType` struct per leaf, so a Zig
/// handler writes `args.json` and the compiler checks the field exists.
/// `planar.cli.flag`'s port deliberately does not reproduce that (see that
/// file's header: C++26 has no comptime struct reification, and the port
/// carries no handler-dispatch surface to serve), leaving `match_result`'s
/// `std::unordered_map<std::string, value>` as the parsed shape. These
/// accessors are the thin layer that makes reading it at a handler call
/// site as short as `args.json` was, without pretending to be typed.
///
/// Every accessor is total: a name that is absent, or present under a
/// different `value` alternative, yields the caller's fallback rather than
/// throwing or aborting. That is the right posture here because the
/// parser has ALREADY enforced required-ness, choice sets, and value kinds
/// against the leaf's declared specs before a handler ever runs — a
/// mismatch at this point would be a tree-authoring bug, and the fallback
/// keeps it from becoming a crash in an operator's shell.
module;

export module planar.cmd.planar.args;

import std;
import planar.cli;

namespace planar::cmd {

/// @brief Read a boolean flag.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--json"`.
/// @param fallback Returned when absent or not a boolean.
/// @return The flag's value.
export auto flag_bool(const cli::match_result& args, std::string_view name, bool fallback = false) -> bool {
  auto const it = args.flags.find(std::string{name});
  if (it == args.flags.end()) {
    return fallback;
  }
  if (auto const* value = std::get_if<bool>(&it->second)) {
    return *value;
  }
  return fallback;
}

/// @brief Read a string flag.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--scope"`.
/// @return The flag's value, or unset when absent or not a string.
export auto flag_string(const cli::match_result& args, std::string_view name) -> std::optional<std::string> {
  auto const it = args.flags.find(std::string{name});
  if (it == args.flags.end()) {
    return std::nullopt;
  }
  if (auto const* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return std::nullopt;
}

/// @brief Read an integer flag.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--line-start"`.
/// @return The flag's value, or unset when absent or not an integer.
export auto flag_int(const cli::match_result& args, std::string_view name) -> std::optional<std::int64_t> {
  auto const it = args.flags.find(std::string{name});
  if (it == args.flags.end()) {
    return std::nullopt;
  }
  if (auto const* value = std::get_if<std::int64_t>(&it->second)) {
    return *value;
  }
  return std::nullopt;
}

/// @brief Parse a decimal integer exactly as Zig's `std.fmt.parseInt(i64,
/// s, 10)` does — the function every handler that takes a numeric
/// POSITIONAL runs, because the tree declares those positionals as
/// `.kind = .string` and the handler converts.
///
/// Not `std::from_chars` alone, because Zig's parser accepts two things
/// `from_chars` rejects, and both are operator-reachable. Captured against
/// the oracle through `planar unlink`:
///
///     +12       -> 12    (a leading `+` sign is accepted)
///     007       -> 7     (leading zeros are fine — so is `from_chars`)
///     1_0       -> 10    (UNDERSCORE DIGIT SEPARATORS are accepted)
///     1__0      -> 10    (consecutive separators too)
///     _10       -> error (a separator may not lead)
///     10_       -> error (…nor trail)
///     +_1       -> error (…nor immediately follow the sign)
///     " 12"     -> error (no leading whitespace)
///     0x10      -> error (base 10 only, no prefix)
///     12abc     -> error (the WHOLE string must be consumed)
///     9223372036854775808 -> error (i64 overflow, exact at the boundary)
///
/// The underscore case is the one a reasonable port drops: `planar unlink
/// 1_0` really does address link 10 on the reference binary, and the error
/// message interpolates the PARSED value (`link 10 not found`), not the
/// raw argument, so a divergence here is visible in output.
///
/// Negative values parse (`unlink -- -5` reaches `link -5 not found`);
/// reaching them requires `--` because the flag parser claims a leading
/// `-` first.
/// @param raw The raw argument text.
/// @return The parsed value, or unset when Zig's parser would have raised.
export auto parse_int64_zig(std::string_view raw) -> std::optional<std::int64_t> {
  std::string_view body     = raw;
  bool             negative = false;
  if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
  if (body.empty() || !is_digit(body.front()) || !is_digit(body.back())) {
    return std::nullopt;
  }
  std::string digits;
  digits.reserve(body.size() + 1);
  if (negative) {
    digits.push_back('-');
  }
  for (char const c : body) {
    if (c == '_') {
      continue;
    }
    if (!is_digit(c)) {
      return std::nullopt;
    }
    digits.push_back(c);
  }
  std::int64_t value   = 0;
  auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 10);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

/// @brief Read a positional argument.
/// @param args The parsed result.
/// @param name The positional's declared name, e.g. `"name"`.
/// @return The positional's value, or unset when absent or not a string.
export auto positional_string(const cli::match_result& args, std::string_view name) -> std::optional<std::string> {
  auto const it = args.positionals.find(std::string{name});
  if (it == args.positionals.end()) {
    return std::nullopt;
  }
  if (auto const* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return std::nullopt;
}

} // namespace planar::cmd
