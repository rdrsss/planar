/// @file flag.cppm
/// @brief `planar.cli.flag` — value kinds, and the `flag`/`positional`/
/// `flag_group` spec types the operator declares when building a `cmd`
/// tree (see `planar.cli.cmd`).
///
/// Behavior-preserving port of zig/vendor/etcli-zig/src/cli/flag.zig (D2, D9),
/// deliberately NOT a transliteration: the Zig source generates a distinct
/// comptime `ArgsType` struct per leaf command so each flag gets its own
/// statically-typed field. C++26 has no equivalent comptime struct
/// reification (no `@Struct`/`@Union` reflection-driven type synthesis), and
/// this port has no `cli.handler`/`cli.castArgs` dispatch surface to serve
/// in the first place — the tree/parser/help/completion contract this task
/// covers never hands a typed args struct to a verb handler (that dispatch
/// glue is engine/cmd scope, explicitly out of bounds here). A single
/// runtime `value` variant keyed by canonical long flag name (or positional
/// name) in a `std::unordered_map` is therefore the idiomatic, "buys
/// nothing to make constexpr" choice (tech-spec instruction: "constexpr/
/// consteval where it buys real compile-time structure, plain runtime data
/// where it does not"). `cmd` trees are still ordinary `constinit`-friendly
/// aggregates — see `planar.cli.cmd` — so a later schema-catalog pass
/// (task 6029) can walk them without any runtime construction cost, but the
/// *parsed result* is plain runtime data.
module;

export module planar.cli.flag;

import std;

namespace planar::cli {

/// @brief Value kind for a flag or positional. Mirrors etcli-zig's `Kind` enum
/// exactly (bool/string/int/float/duration/path/choice) so a later schema
/// catalog pass can round-trip kind names without a remapping table.
/// `choice` is a string constrained at parse time to a declared `choices`
/// set.
export enum class kind : std::uint8_t {
  boolean,
  string,
  integer,
  floating,
  duration,
  path,
  choice,
};

/// @brief Runtime value for one flag or positional. `std::monostate` marks
/// "not present" — callers `contains()`-check `match_result::flags` /
/// `::positionals` rather than relying on a null variant alternative, since
/// SQLite-style optionality (absent vs explicit) is a map-membership
/// question, not a variant-alternative question. `duration` values are
/// nanoseconds stored in the `std::int64_t` alternative (etcli-zig's `u64` is
/// widened to fit `int64_t` uniformly with `integer`, avoiding a second
/// integral alternative). `list`-flag accumulation stores repeated string
/// values as `std::vector<std::string>`.
export using value = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>>;

/// @brief Mode for a command-level flag group (etcli-zig's `FlagGroupMode`).
export enum class flag_group_mode : std::uint8_t {
  mutually_exclusive,
  required_one,
  required_exactly_one,
};

/// @brief A relationship between multiple visible flags at one command path.
/// `flags` holds canonical long names (e.g. `"--json"`); short forms and
/// aliases resolve to those names during enforcement.
export struct flag_group {
  std::string              name;                                       ///< The group's identifier, shown in help/errors.
  flag_group_mode          mode = flag_group_mode::mutually_exclusive; ///< The relationship enforced across `flags`.
  std::vector<std::string> flags;                                      ///< Canonical long names of the member flags.
  std::string              desc; ///< One-line summary shown in the FLAG GROUPS help section.
};

/// @brief A flag spec. `long_name` is the canonical long form (e.g.
/// `"--verbose"`); `short_name` is an optional single-char alias (`'v'` for
/// `-v`). `value_kind` drives parse-time coercion; `default_value` supplies
/// a fallback when the flag is absent. `required = true` without a default
/// is a parse-time error when the flag never appears.
export struct flag {
  std::string              long_name;                 ///< Canonical long form, e.g. `"--verbose"`.
  std::vector<std::string> aliases;                   ///< Additional long forms that also resolve to this flag.
  std::optional<char>      short_name;                ///< Optional single-char alias, e.g. `'v'` for `-v`.
  bool                     hidden = false;            ///< Excluded from help/completion listings when true.
  std::string              desc;                      ///< One-line summary shown in the FLAGS help section.
  kind                     value_kind = kind::string; ///< Drives parse-time coercion and the generated help kind label.
  /// When true the flag may repeat (`--tag a --tag b`); accumulated values
  /// land in `match_result::flags[long_name]` as the `vector<string>`
  /// alternative. A list flag takes no `default_value`.
  bool list = false;
  /// When true the flag takes no value and counts occurrences (`-vvv` or
  /// repeated `--verbose`); the accumulated count lands in
  /// `match_result::flags[long_name]` as the `int64_t` alternative.
  /// Requires `value_kind == kind::boolean`; mutually exclusive with
  /// `list`, `default_value`, and `required`.
  bool count = false;
  /// Manual/help placeholder for non-bool flag values (e.g. `PATH`).
  /// Parsing is driven only by `value_kind`; this never affects coercion.
  std::optional<std::string> value_name;
  /// Allowed values for a `kind::choice` flag. Required (non-empty) when
  /// `value_kind == kind::choice`; must be empty otherwise.
  std::vector<std::string> choices;          ///< The declared value set for a `kind::choice` flag.
  std::optional<value>     default_value;    ///< Fallback value applied when the flag is absent.
  bool                     required = false; ///< Parse-time error if absent and no `default_value`.
  /// Documented in help output as an env-var fallback name. Parsing itself
  /// is env-unaware (matches etcli-zig: `cli.run` reads the environment, `parse`
  /// does not) — no engine `run` surface exists in this port to own that
  /// concern.
  std::optional<std::string> env;
};

/// @brief A positional argument spec. Positionals are consumed, in
/// declaration order, from the tokens left over after subcommand and flag
/// resolution. Anything after a bare `--` token is passthrough and always
/// treated as positional.
export struct positional {
  std::string name;                      ///< The positional's identifier, e.g. shown as `<name>` in help.
  std::string desc;                      ///< One-line summary shown in the POSITIONAL ARGUMENTS help section.
  kind        value_kind = kind::string; ///< Drives parse-time coercion.
  bool        required   = true;         ///< Parse-time error if omitted and no `default_value`.
  /// Applied when the positional is omitted; makes an otherwise-`required`
  /// slot effectively optional. Contradictory with `required = true`
  /// (validated by a later authoring-lint pass, not enforced here).
  std::optional<value> default_value;
};

/// @brief Derive the schema/help identifier for a flag: strip the leading
/// `-`/`--` and leave hyphens as-is (etcli-zig strips to a Zig field name;
/// this port has no generated struct field to serve, so the identifier is
/// only ever used for map lookups / catalog emission — hyphens stay
/// intact there). Exposed for the future schema-catalog pass (task 6029).
/// @param long_name A canonical long flag name, e.g. `"--no-auto-promote"`.
/// @return The name with leading dashes stripped.
export auto flag_identifier(std::string_view long_name) -> std::string_view {
  if (long_name.starts_with("--")) {
    return long_name.substr(2);
  }
  if (long_name.starts_with("-")) {
    return long_name.substr(1);
  }
  return long_name;
}

} // namespace planar::cli
