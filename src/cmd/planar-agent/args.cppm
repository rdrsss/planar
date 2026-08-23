/// @file args.cppm
/// @brief `planar.cmd.planar_agent.args` — typed accessors over a parsed
/// `cli::match_result`, plus the two value parsers every claim verb runs
/// on operator input (plan 996, task 6038).
///
/// ## Why this is a second copy of `planar.cmd.planar.args`
///
/// Because D18 forbids a `cmd_* -> cmd_*` edge and
/// `cmake/architecture.cmake` FATALs at configure time on one. The four
/// accessors below ARE byte-for-byte what the operator binary's `args`
/// module holds, and that duplication is the same trade this directory
/// already made for `context`, `exit`, `handler`, `dispatch` and `tree`:
/// ~60 lines of identical-looking code beats one shared module that has to
/// carry a `binary_kind` parameter nobody can see at a call site. See
/// `src/cmd/planar-agent/CMakeLists.txt`'s header for the full argument.
///
/// What is NOT a copy is everything below `parse_int64_zig` —
/// `parse_ttl_seconds` and `parse_entity_ref` exist only in this binary,
/// because only this binary has leases and entity refs.
module;

export module planar.cmd.planar_agent.args;

import std;
import planar.cli;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

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
///
/// Returns unset for an ABSENT flag. Note that an explicitly-empty
/// `--status ""` is present-and-empty, not absent, and the two behave
/// differently at the heartbeat call site — see that handler.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--claim"`.
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
/// @param name The canonical long name, e.g. `"--blocker"`.
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

/// @brief Read an integer positional.
/// @param args The parsed result.
/// @param name The positional's declared name, e.g. `"plan-id"`.
/// @return The value, or unset when absent or not an integer.
export auto positional_int(const cli::match_result& args, std::string_view name) -> std::optional<std::int64_t> {
  auto const it = args.positionals.find(std::string{name});
  if (it == args.positionals.end()) {
    return std::nullopt;
  }
  if (auto const* value = std::get_if<std::int64_t>(&it->second)) {
    return *value;
  }
  return std::nullopt;
}

/// @brief Parse a decimal integer exactly as Zig's
/// `std.fmt.parseInt(i64, s, 10)` does.
///
/// The same function `planar.cmd.planar.args` carries, and for the same
/// reason: Zig accepts a leading `+` and UNDERSCORE digit separators
/// (`1_0` is 10), both of which `std::from_chars` rejects and both of
/// which are operator-reachable. See that module's doc comment for the
/// full capture table.
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

/// @brief Parse a `--ttl` / `--stale-after` value into whole seconds.
///
/// Port of etcli-zig's `cli.duration.parseSeconds`. Three behaviours are
/// easy to miss and all three are operator-visible:
///
/// 1. **A BARE INTEGER IS SECONDS, not nanoseconds.** `600` means ten
///    minutes. This is the ergonomic default the flag's own help text
///    promises.
/// 2. **Sub-second inputs round DOWN, silently.** `--ttl 500ms` is a
///    LEGAL value that yields a ZERO-second lease — a claim that is
///    already expired the instant it is minted. The help text advertises
///    `500ms` as an example, so this is reachable by following the
///    documentation. Reproduced under D2, not corrected.
/// 3. **The unit is whitespace-trimmed.** Zig's `splitNumUnit` runs
///    `trim(text[i..], " \t")`, so `"600 "` and `"10 m"` both parse.
///
/// Overflowing the unsigned nanosecond range is malformed input, not a
/// value to clamp — it maps to the same "invalid" answer as garbage.
/// @param text The raw flag value.
/// @return The whole seconds, or unset when the value is malformed.
export auto parse_ttl_seconds(std::string_view text) -> std::optional<std::int64_t> {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  if (digits == 0) {
    return std::nullopt;
  }
  std::uint64_t num    = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + digits, num, 10);
  if (ec != std::errc{}) {
    return std::nullopt;
  }

  std::string_view unit = text.substr(digits);
  auto const       trim = [](std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    return value;
  };
  unit = trim(unit);

  constexpr std::uint64_t k_ns_per_us   = 1'000;
  constexpr std::uint64_t k_ns_per_ms   = 1'000'000;
  constexpr std::uint64_t k_ns_per_s    = 1'000'000'000;
  constexpr std::uint64_t k_ns_per_min  = 60ULL * k_ns_per_s;
  constexpr std::uint64_t k_ns_per_hour = 60ULL * k_ns_per_min;

  std::uint64_t scale = 0;
  if (unit.empty() || unit == "s") {
    scale = k_ns_per_s;
  } else if (unit == "ns") {
    scale = 1;
  } else if (unit == "us") {
    scale = k_ns_per_us;
  } else if (unit == "ms") {
    scale = k_ns_per_ms;
  } else if (unit == "m") {
    scale = k_ns_per_min;
  } else if (unit == "h") {
    scale = k_ns_per_hour;
  } else {
    return std::nullopt;
  }

  if (scale != 0 && num > std::numeric_limits<std::uint64_t>::max() / scale) {
    return std::nullopt;
  }
  std::uint64_t const nanos = num * scale;
  std::uint64_t const secs  = nanos / k_ns_per_s;
  if (secs > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(secs);
}

/// @brief A parsed `--entity` reference.
export struct entity_ref {
  engine::runtime::agentactivity::entity_kind kind{}; ///< The entity kind.
  std::int64_t                                id{};   ///< The entity id.
};

/// @brief Why an `--entity` value was refused.
///
/// TWO members and not one, because the two exit through DIFFERENT process
/// exit codes and the split is oracle-verified:
///
///     planar-agent claim --entity task:abc   -> InvalidEntityRef,      exit 2
///     planar-agent claim --entity bogus:1    -> UnsupportedEntityKind, exit 1
///
/// `planar-agent`'s `codeFor` maps `InvalidEntityRef` to 2 and has NO arm
/// for `UnsupportedEntityKind`, so the latter falls through to the generic
/// 1. Both captures were taken from the reference binary directly. The tag
/// itself is also operator-visible: the message interpolates it.
export enum class entity_ref_error : std::uint8_t {
  invalid_entity_ref,      ///< No colon, empty id, or a non-integer id.
  unsupported_entity_kind, ///< A well-formed ref naming a kind claims cannot hold.
};

/// @brief The Zig error tag for `err`, as it appears inside the operator-
/// visible message.
/// @param err The refusal.
/// @return The tag.
export auto entity_ref_error_name(entity_ref_error err) -> std::string_view {
  return err == entity_ref_error::invalid_entity_ref ? "InvalidEntityRef" : "UnsupportedEntityKind";
}

/// @brief Parse `task:<id>` / `plan:<id>` / `plan_step:<id>`.
///
/// Strict by design. Note the ORDER of the two failures: the id is parsed
/// BEFORE the kind is recognised, so `bogus:abc` reports
/// `invalid_entity_ref` (exit 2), not `unsupported_entity_kind` (exit 1) —
/// a detail that only matters because the two exit differently.
/// @param text The raw `--entity` value.
/// @return The parsed ref, or the refusal.
export auto parse_entity_ref(std::string_view text) -> std::expected<entity_ref, entity_ref_error> {
  auto const colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::unexpected(entity_ref_error::invalid_entity_ref);
  }
  auto const kind_text = text.substr(0, colon);
  auto const id_text   = text.substr(colon + 1);
  if (id_text.empty()) {
    return std::unexpected(entity_ref_error::invalid_entity_ref);
  }
  auto const id = parse_int64_zig(id_text);
  if (!id.has_value()) {
    return std::unexpected(entity_ref_error::invalid_entity_ref);
  }
  auto const kind = engine::runtime::agentactivity::entity_kind_from_text(kind_text);
  if (!kind.has_value()) {
    return std::unexpected(entity_ref_error::unsupported_entity_kind);
  }
  return entity_ref{.kind = *kind, .id = *id};
}

} // namespace planar::cmd::agent
