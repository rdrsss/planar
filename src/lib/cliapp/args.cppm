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
  /// @brief Positional values keyed by declared name (`"plan-id"`). Holds
  /// only the LAST harvested value for a positional CLI11 collected more
  /// than one for — see `positional_lists` for the multi-value form.
  std::map<std::string, std::string, std::less<>> positionals;
  /// @brief Every value harvested for a positional, in argv order, keyed
  /// by declared name.
  ///
  /// The multi-value counterpart to `positionals` above, for a variadic
  /// "rest" positional — `capture commits`'s trailing SHA list is the
  /// only one this repo declares today (plan 996, task 6358), mirroring
  /// the oracle's etcli-zig `rest_field` mechanism. It is declared
  /// `->group("")`-hidden (see `tree.cpp`'s `add_capture`) precisely
  /// because `rest_field` is NOT a real positional and never appears in
  /// the oracle's own `schema` catalog — confirmed against a live oracle
  /// run, whose `capture commits` node reports `"positionals":[]`.
  std::map<std::string, std::vector<std::string>, std::less<>> positional_lists;
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

/// @brief Parse a floating-point value exactly as Zig's
/// `std.fmt.parseFloat(f64, s)` does.
///
/// Added for `models evals --quality-floor` (plan 996, task 6149), which is
/// declared `(string)` on the tree precisely so the leaf can apply Zig's
/// contract rather than CLI11's. The value is a RANKING GATE — it decides
/// which candidates are excluded before any ordering — so a parser that
/// merely "looks close enough" changes which candidate the verb
/// recommends, and does so at exit 0 with a plausible-looking payload.
///
/// Captured against the oracle rather than read off Zig's source:
///
///     0.5      -> 0.5      1e-1     -> 0.1      .5    -> 0.5
///     5.       -> 5        0_5.0    -> 5        +0.5  -> 0.5
///     -0.5     -> -0.5     0x1p-1   -> 0.5      0X1P-1-> 0.5
///     inf/INF/Infinity     -> +infinity        nan/NaN -> NaN
///     1,5      -> error    0.5abc   -> error    " 0.5" -> error
///     _5 / 5_ / 0.5_ / 0._5 / 1_e2  -> error
///
/// The underscore rule is the subtle one and is NOT "strip every
/// underscore": `0_5.0` parses while `0._5` and `1_e2` do not, because a
/// separator must sit BETWEEN two digits. Hex float literals are accepted
/// with a `p` exponent, which `std::from_chars`' general format does not
/// recognise — hence the explicit prefix split below.
/// @param raw The raw argument text.
/// @return The parsed value, or unset when Zig's parser would have raised.
export auto parse_float_zig(std::string_view raw) -> std::optional<double> {
  auto const is_dec = [](char c) { return c >= '0' && c <= '9'; };
  auto const is_hex = [&](char c) { return is_dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
  // Which digit class a separator must sit between depends on the LITERAL'S
  // BASE, and getting that wrong is not academic: with the hex class applied
  // to a decimal literal, `1_e2` passes (because `e` is a hex digit) and
  // parses as 100 where the oracle refuses at exit 2 — a gate value accepted
  // out of thin air. Detected by the differential harness.
  auto const unsigned_start = !raw.empty() && (raw.front() == '+' || raw.front() == '-') ? 1U : 0U;
  bool const hex_literal    = raw.size() > unsigned_start + 2 && raw[unsigned_start] == '0' &&
                              (raw[unsigned_start + 1] == 'x' || raw[unsigned_start + 1] == 'X');
  auto const is_digit_here  = [&](char c) { return hex_literal ? is_hex(c) : is_dec(c); };
  // Separators first, on the ORIGINAL text: each '_' needs a digit on both
  // sides. Checking after the strip would accept `0._5`.
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '_') {
      continue;
    }
    if (i == 0 || i + 1 == raw.size() || !is_digit_here(raw[i - 1]) || !is_digit_here(raw[i + 1])) {
      return std::nullopt;
    }
  }
  std::string body;
  body.reserve(raw.size());
  for (char const c : raw) {
    if (c != '_') {
      body.push_back(c);
    }
  }

  std::string_view rest{body};
  double           sign = 1.0;
  if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
    sign = rest.front() == '-' ? -1.0 : 1.0;
    rest.remove_prefix(1);
  }
  if (rest.empty()) {
    return std::nullopt;
  }

  auto const iequals = [](std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      auto const a = static_cast<char>(lhs[i] | (lhs[i] >= 'A' && lhs[i] <= 'Z' ? 0x20 : 0));
      if (a != rhs[i]) {
        return false;
      }
    }
    return true;
  };
  if (iequals(rest, "inf") || iequals(rest, "infinity")) {
    return sign * std::numeric_limits<double>::infinity();
  }
  if (iequals(rest, "nan")) {
    // Sign is deliberately not applied: `-nan` and `nan` are the same
    // value, and the renderer emits the literal `"nan"` either way.
    return std::numeric_limits<double>::quiet_NaN();
  }

  auto format = std::chars_format::general;
  if (rest.size() > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X')) {
    // `from_chars`' hex format wants the digits WITHOUT the `0x` prefix.
    rest.remove_prefix(2);
    format = std::chars_format::hex;
  } else if (!is_dec(rest.front()) && rest.front() != '.') {
    // Reject the spellings `from_chars`' general format would otherwise
    // accept on its own — a bare `inf`/`nan` reaching here means the text
    // had trailing bytes, which Zig refuses.
    return std::nullopt;
  }

  double value         = 0;
  auto const [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), value, format);
  if (ec != std::errc{} || ptr != rest.data() + rest.size()) {
    return std::nullopt;
  }
  return sign * value;
}

/// @brief Parse an UNSIGNED integer exactly as Zig's
/// `std.fmt.parseInt(u64, s, 10)` does.
///
/// `parse_int64_zig`'s rules plus a refusal of any negative value. Added
/// for `models evals --min-samples` (plan 996, task 6149), where the oracle
/// really does refuse `-1` — `error: invalid --min-samples '-1'` — while
/// accepting `+5`, `0`, and the separator form `1_0`. Reusing the signed
/// parser there would silently accept `-1` and then convert it to a huge
/// unsigned minimum, gating every candidate as `insufficient_data` at exit
/// 0.
/// @param raw The raw argument text.
/// @return The parsed value, or unset when Zig's parser would have raised.
export auto parse_uint64_zig(std::string_view raw) -> std::optional<std::uint64_t> {
  auto const signed_value = parse_int64_zig(raw);
  if (!signed_value || *signed_value < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*signed_value);
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
          out.positionals[name]      = opt->results().back();
          out.positional_lists[name] = opt->results();
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

/// @brief Read a boolean flag: absent is `fallback`, present is the VALUE
/// harvested for it.
///
/// ## Why this is not "present means true"
///
/// It was, until task 6138, and that answered the wrong question in two
/// operator-reachable ways — both of which only became reachable once
/// negations were declared:
///
///   * `--no-X` lands in `results()` as the string `"false"` (CLI11's
///     `Option::get_flag_value` resolves a `!`-prefixed name to its
///     declared flag value). Presence alone would read `--no-editor` as
///     "editor requested";
///   * a bool flag with a declared default — `planar task add --editor`
///     and `planar artifact add --editor` both default TRUE — is seeded by
///     `harvest` from `get_default_str()`, so its key is present even when
///     the flag never appeared on argv. Presence alone would then also
///     read an explicit `--no-editor` as true, because both cases look
///     identical from the key set.
///
/// The two share one fix: read the value, not the key. Only the exact
/// strings `"false"` and `"0"` are false — a plain `--X` stores `"true"`,
/// and an option CLI11 records with an empty result string is still a
/// PRESENT flag and stays true.
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--json"`.
/// @param fallback Returned when the flag is absent.
/// @return The flag's value.
export auto flag_bool(const parsed_args& args, std::string_view name, bool fallback = false) -> bool {
  auto const it = args.flags.find(name);
  if (it == args.flags.end() || it->second.empty()) {
    return fallback;
  }
  auto const& value = it->second.back();
  return !(value == "false" || value == "0");
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

/// @brief Read EVERY value supplied for a repeatable flag, in argv order.
///
/// The whole-vector accessor `parsed_args::flags`' own comment defers until
/// "a repeatable flag is actually declared". `bench start --task` is that
/// flag (plan 996, task 6149): the generated surface marks it
/// `.list = true`, `cliapp::surface`'s `declare_flag` turns that into
/// CLI11's unbounded `expected(1, -1)`, and the oracle really does accept
/// `--task 1 --task 2` and intersect the declared-touch snapshot with BOTH
/// ids. `flag_string` reads only the LAST element, so a `bench start`
/// written against it would silently snapshot one task where the operator
/// named three — a wrong-rows defect with identical stdout (the leaf prints
/// only the uid).
/// @param args The parsed result.
/// @param name The canonical long name, e.g. `"--task"`.
/// @return Every supplied value in argv order; empty when the flag is absent.
export auto flag_strings(const parsed_args& args, std::string_view name) -> std::vector<std::string> {
  auto const it = args.flags.find(name);
  if (it == args.flags.end()) {
    return {};
  }
  return it->second;
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

/// @brief Read every value harvested for a positional, in argv order.
///
/// The multi-value counterpart to `positional_string`, for a variadic
/// "rest" positional such as `capture commits`'s trailing SHA list (plan
/// 996, task 6358) — see `parsed_args::positional_lists`.
/// @param args The parsed result.
/// @param name The positional's declared name.
/// @return Every value, in argv order; empty when the positional is absent
/// or received no values.
export auto positional_strings(const parsed_args& args, std::string_view name) -> std::vector<std::string> {
  auto const it = args.positional_lists.find(name);
  if (it == args.positional_lists.end()) {
    return {};
  }
  return it->second;
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
