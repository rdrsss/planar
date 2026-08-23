/// @file args.cppm
/// @brief `planar.cliapp.args` — the parsed-argv value shape every binary's
/// handlers read, harvested out of a `CLI::App` after CLI11 has parsed
/// (plan 996, task 6123).
///
/// ## Why the handlers do not read `CLI::Option*` directly
///
/// CLI11's idiomatic shape binds each option to a variable at tree-build
/// time. That does not fit this tree's dispatch: a handler is looked up in
/// a path-keyed table AFTER the parse and is handed the result, so the
/// values have to be reachable from the parse outcome rather than from
/// variables the tree builder happens to still own. `harvest` walks the
/// matched command chain and collects every option's raw results into
/// `parsed_args`, keyed by canonical long name (`"--json"`) or positional
/// name (`"plan-id"`) — the same keys the deleted `cli::match_result`
/// used, so no handler call site had to change meaning.
///
/// ## THE ZIG parseInt CONTRACT LIVES HERE, ONCE
///
/// Before this task the tree carried THREE implementations of Zig's
/// `std.fmt.parseInt(i64, s, 10)` semantics: `cli/parser.cpp`'s
/// `numeric::normalize_int_token` (layer 1, died with `src/lib/cli`), and
/// a verbatim copy of `parse_int64_zig` in each of
/// `cmd/planar/args.cppm` and `cmd/planar-agent/args.cppm`. The task
/// brief called for consolidating on the survivor. It is below, once, and
/// it is layer 1 for the same reason `json_text` is: it is a shared
/// PARSING PRIMITIVE, not a command tree, and D18's `cmd_* -> cmd_*`
/// prohibition is about trees.
///
/// This matters beyond tidiness. `zig_int_validator` below is the
/// CLI11 validator every integer-valued flag in every binary attaches, so
/// the underscore-separator contract is enforced at PARSE time (`--plan
/// 1_0` is plan 10; `--plan 10_` is refused with a validation error)
/// rather than degrading into a silent `std::nullopt` at the handler. A
/// second copy of the parser would mean a second, drifting answer to
/// "which integers does this CLI accept".
module;

export module planar.cliapp.args;

import std;
import cli11;
import planar.cliapp.walk;

namespace planar::cliapp {

/// @brief The parsed outcome handed to a verb handler: the resolved
/// subcommand path, every flag's raw string results keyed by canonical
/// long name, and every positional's raw string value keyed by its
/// declared name.
export struct parsed_args {
  /// @brief The resolved subcommand path, e.g. `{"workflow","list"}`.
  std::vector<std::string> path;
  /// @brief Flag values keyed by canonical long name (`"--json"`).
  /// A key is present exactly when the flag was supplied on argv OR the
  /// tree declared a default for it (see `harvest`).
  ///
  /// A `vector` rather than a single string because CLI11's
  /// `Option::results()` is one, and flattening at harvest time would
  /// discard information no accessor could recover. No tree in this repo
  /// declares a repeatable flag today — and CLI11 REFUSES a repeated
  /// single-valued option at parse time (`--purpose a --purpose b` is a
  /// ValidationError, not last-wins), so in practice these vectors hold at
  /// most one element. `flag_string` reads the last for that reason rather
  /// than as a policy choice. Add a whole-vector accessor when a repeatable
  /// flag is actually declared, not before.
  std::map<std::string, std::vector<std::string>, std::less<>> flags;
  /// @brief Positional values keyed by declared name (`"plan-id"`).
  std::map<std::string, std::string, std::less<>> positionals;
};

/// @brief Parse a decimal integer exactly as Zig's `std.fmt.parseInt(i64,
/// s, 10)` does.
///
/// Not `std::from_chars` alone, because Zig's parser accepts two things
/// `from_chars` rejects and both are operator-reachable. Captured against
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

/// @brief The CLI11 validator every integer-valued flag and positional
/// attaches, so the Zig `parseInt` contract above is enforced at PARSE
/// time rather than silently degrading to "absent" at the handler.
/// @return The validator; its type name renders as `INT` in help.
export auto zig_int_validator() -> CLI::Validator {
  return CLI::Validator(
      [](std::string& value) -> std::string {
        if (parse_int64_zig(value).has_value()) {
          return {};
        }
        return "value is not a base-10 integer: " + value;
      },
      "INT", "zig-int");
}

/// @brief Collect the values CLI11 parsed into `parsed_args`.
///
/// Walks from `root` down the chain of subcommands CLI11 actually matched
/// (`CLI::App::get_subcommands()` returns the PARSED children at each
/// level), so a flag declared on an ancestor is harvested alongside the
/// leaf's own — the inherited-flag behaviour the deleted parser had.
///
/// A flag that did not appear on argv but whose tree declaration carries a
/// default string is inserted WITH that default. CLI11's `default_str` is
/// help-only metadata — it never lands in `Option::results()` for an
/// option with no bound variable — so without this step every handler
/// reading `--ttl` or `--category` or `--vendor` would see "absent" where
/// the tree promised a value, and the defaults would be silently inert.
/// @param root The parsed root app.
/// @return The harvested values.
export auto harvest(const CLI::App& root) -> parsed_args {
  parsed_args     out;
  const CLI::App* node = &root;
  while (true) {
    const CLI::Option* help = node->get_help_ptr();
    for (const CLI::Option* opt : node->get_options()) {
      if (opt == help) {
        continue;
      }
      auto const name = canonical_name(*opt);
      if (opt->get_positional()) {
        if (opt->count() > 0) {
          out.positionals[name] = opt->results().back();
        }
        continue;
      }
      if (opt->count() > 0) {
        out.flags[name] = opt->results();
      } else if (!opt->get_default_str().empty()) {
        out.flags[name] = std::vector<std::string>{opt->get_default_str()};
      }
    }
    auto const matched = node->get_subcommands();
    if (matched.empty()) {
      break;
    }
    node = matched.front();
    out.path.push_back(node->get_name());
  }
  return out;
}

/// @brief Read a boolean flag: present on argv is true, absent is
/// `fallback`.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--json"`.
/// @param fallback Returned when the flag is absent.
/// @return The flag's value.
export auto flag_bool(const parsed_args& args, std::string_view name, bool fallback = false) -> bool {
  auto const it = args.flags.find(name);
  if (it == args.flags.end()) {
    return fallback;
  }
  return true;
}

/// @brief Read a string flag.
///
/// Returns unset for an ABSENT flag. An explicitly-empty `--status ""` is
/// present-and-empty, not absent, and the two behave differently at some
/// call sites (see `planar-agent`'s heartbeat handler).
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--scope"`.
/// @return The flag's value, or unset when absent.
export auto flag_string(const parsed_args& args, std::string_view name) -> std::optional<std::string> {
  auto const it = args.flags.find(name);
  if (it == args.flags.end() || it->second.empty()) {
    return std::nullopt;
  }
  return it->second.back();
}

/// @brief Read an integer flag, through `parse_int64_zig`.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--line-start"`.
/// @return The flag's value, or unset when absent or unparseable.
export auto flag_int(const parsed_args& args, std::string_view name) -> std::optional<std::int64_t> {
  auto const raw = flag_string(args, name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return parse_int64_zig(*raw);
}

/// @brief Read a positional argument.
/// @param args The parsed result.
/// @param name The positional's declared name, e.g. `"name"`.
/// @return The positional's value, or unset when absent.
export auto positional_string(const parsed_args& args, std::string_view name) -> std::optional<std::string> {
  auto const it = args.positionals.find(name);
  if (it == args.positionals.end()) {
    return std::nullopt;
  }
  return it->second;
}

/// @brief Read an integer positional, through `parse_int64_zig`.
/// @param args The parsed result.
/// @param name The positional's declared name, e.g. `"plan-id"`.
/// @return The value, or unset when absent or unparseable.
export auto positional_int(const parsed_args& args, std::string_view name) -> std::optional<std::int64_t> {
  auto const raw = positional_string(args, name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  return parse_int64_zig(*raw);
}

/// @brief The key a resolved command path maps to: its segments joined
/// with single spaces, e.g. `{"workflow","list"}` -> `"workflow list"`.
/// @param path The resolved command path.
/// @return The table key.
export auto path_key(std::span<const std::string> path) -> std::string {
  std::string key;
  for (auto const& segment : path) {
    if (!key.empty()) {
      key += ' ';
    }
    key += segment;
  }
  return key;
}

/// @brief Every leaf (childless node) in `root`, as `path_key` strings.
///
/// The registration gate's input: a binary's dispatch table is required to
/// have an entry for each of these, and to have no entry that is not one
/// of these. See each binary's `dispatch` module.
/// @param root The command tree.
/// @return The leaf keys, in tree-walk order.
export auto leaf_keys(const CLI::App& root) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const& node : all_nodes(root)) {
    if (children(*node.node).empty()) {
      out.push_back(path_key(node.path));
    }
  }
  return out;
}

} // namespace planar::cliapp
