/// @file args.cppm
/// @brief `planar.cmd.planar_agent.args` — the two value parsers that
/// exist ONLY in this binary (plan 996, tasks 6038 and 6123).
///
/// ## Task 6123 deleted the copies; this is what was actually
/// binary-specific
///
/// This module used to open with four accessors (`flag_bool`,
/// `flag_string`, `flag_int`, `positional_int`) and `parse_int64_zig` that
/// were byte-for-byte the operator binary's, duplicated because D18
/// forbids a `cmd_* -> cmd_*` edge. All five now live once, at layer 1, in
/// `planar.cliapp.args` — reachable DOWNWARD by every binary, which D18
/// has always permitted. That is the "consolidate on the survivor"
/// instruction from the task brief, applied to its root cause rather than
/// to one of its two copies: the tree carried THREE implementations of
/// Zig's `parseInt` semantics (this one, `cmd/planar/args.cppm`'s, and
/// `cli/parser.cpp`'s `numeric::normalize_int_token`) and now carries one.
///
/// Handler call sites now spell them
/// `cliapp::flag_string(args, "--claim")`, qualified rather than
/// re-exported through a using-declaration: the qualification says where
/// the one definition lives, and a re-export would put an undocumented
/// alias in this module's Doxygen surface for no reading benefit.
///
/// What was NEVER a copy is everything below — `parse_ttl_seconds` and
/// `parse_entity_ref` exist only in this binary, because only this binary
/// has leases and entity refs.
module;

export module planar.cmd.planar_agent.args;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

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
  auto const id = cliapp::parse_int64_zig(id_text);
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
