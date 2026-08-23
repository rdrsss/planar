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
