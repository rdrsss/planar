/// @file parser.cpp
/// @brief Implementation of `planar.cli.parser::parse` — see parser.cppm's
/// file comment for the port-fidelity notes (two-pass shape preserved;
/// Zig's module-static backing buffers replaced by ordinary owned storage).
module;

module planar.cli.parser;

import std;
import planar.cli.flag;
import planar.cli.cmd;
import planar.cli.error;

namespace planar::cli {

namespace {

auto flag_long_matches(flag const& f, std::string_view tok) -> bool {
  if (f.long_name == tok) {
    return true;
  }
  return std::ranges::any_of(f.aliases, [&](std::string const& alias) { return alias == tok; });
}

auto flag_negation_matches(flag const& f, std::string_view tok) -> bool {
  auto matches_negation = [&](std::string_view long_form) {
    if (!long_form.starts_with("--")) {
      return false;
    }
    std::string negated = "--no-";
    negated += long_form.substr(2);
    return negated == tok;
  };
  if (matches_negation(f.long_name)) {
    return true;
  }
  return std::ranges::any_of(f.aliases, [&](std::string const& alias) { return matches_negation(alias); });
}

/// @brief Does `tok` name a non-bool flag visible in `scope` (root..the
/// currently-resolved ancestor chain)? Used during subcommand-path
/// resolution to know whether to swallow the following argv token as this
/// flag's value before it can be mistaken for a subcommand name.
auto flag_wants_value(std::span<const cmd* const> scope, std::string_view tok) -> bool {
  if (tok.size() >= 2 && tok[0] == '-' && tok[1] == '-') {
    for (auto const* node : scope) {
      for (auto const& f : node->flags) {
        if (flag_long_matches(f, tok)) {
          return f.value_kind != kind::boolean;
        }
      }
    }
    return false;
  }
  if (tok.size() == 2 && tok[0] == '-') {
    for (auto const* node : scope) {
      for (auto const& f : node->flags) {
        if (f.short_name && *f.short_name == tok[1]) {
          return f.value_kind != kind::boolean;
        }
      }
    }
  }
  return false;
}

auto looks_like_negative_number(std::string_view tok) -> bool {
  if (tok.size() < 2 || tok[0] != '-') {
    return false;
  }
  return std::ranges::all_of(tok.substr(1), [](char c) { return c >= '0' && c <= '9'; });
}

auto edit_distance_at_most(std::string_view a, std::string_view b, std::size_t max) -> std::optional<std::size_t> {
  if (a.size() > b.size() + max || b.size() > a.size() + max) {
    return std::nullopt;
  }
  std::vector<std::size_t> previous(b.size() + 1);
  std::vector<std::size_t> current(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) {
    previous[j] = j;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    current[0]          = i + 1;
    std::size_t row_min = current[0];
    for (std::size_t j = 0; j < b.size(); ++j) {
      std::size_t const cost         = (a[i] == b[j]) ? 0 : 1;
      std::size_t const deletion     = previous[j + 1] + 1;
      std::size_t const insertion    = current[j] + 1;
      std::size_t const substitution = previous[j] + cost;
      current[j + 1]                 = std::min({deletion, insertion, substitution});
      row_min                        = std::min(row_min, current[j + 1]);
    }
    if (row_min > max) {
      return std::nullopt;
    }
    previous = current;
  }
  if (previous[b.size()] <= max) {
    return previous[b.size()];
  }
  return std::nullopt;
}

auto best_candidate(std::string_view tok, std::vector<std::string_view> const& candidates) -> std::optional<std::string> {
  std::optional<std::string> best;
  std::size_t                best_score = std::numeric_limits<std::size_t>::max();
  for (auto candidate : candidates) {
    auto score = edit_distance_at_most(tok, candidate, 3);
    if (score && *score < best_score) {
      best_score = *score;
      best       = std::string(candidate);
    }
  }
  if (best_score <= 2) {
    return best;
  }
  return std::nullopt;
}

auto suggest_command(std::vector<cmd> const& cmds, std::string_view tok) -> std::optional<std::string> {
  std::vector<std::string_view> candidates;
  for (auto const& c : cmds) {
    candidates.push_back(c.name);
    for (auto const& alias : c.aliases) {
      candidates.push_back(alias);
    }
  }
  return best_candidate(tok, candidates);
}

auto flag_suggestion_token(std::string_view tok) -> std::string_view {
  auto pos = tok.find('=');
  if (pos != std::string_view::npos) {
    return tok.substr(0, pos);
  }
  return tok;
}

auto suggest_flag(std::vector<flag> const& flags, std::string_view tok) -> std::optional<std::string> {
  auto const                    name = flag_suggestion_token(tok);
  std::vector<std::string_view> candidates;
  std::vector<std::string>      short_forms; // owns storage for "-x" strings
  for (auto const& f : flags) {
    candidates.push_back(f.long_name);
    for (auto const& alias : f.aliases) {
      candidates.push_back(alias);
    }
    if (f.short_name) {
      short_forms.push_back(std::string("-") + *f.short_name);
    }
  }
  for (auto const& s : short_forms) {
    candidates.push_back(s);
  }
  return best_candidate(name, candidates);
}

auto nearest_choice(std::vector<std::string> const& choices, std::string_view raw) -> std::optional<std::string> {
  std::vector<std::string_view> candidates(choices.begin(), choices.end());
  return best_candidate(raw, candidates);
}

auto is_choice_member(std::vector<std::string> const& choices, std::string_view raw) -> bool {
  return std::ranges::any_of(choices, [&](std::string const& c) { return c == raw; });
}

auto parse_bool_value(std::string_view raw) -> std::optional<bool> {
  if (raw == "true") {
    return true;
  }
  if (raw == "false") {
    return false;
  }
  return std::nullopt;
}

/// @brief Minimal duration parser: bare integers are seconds; `<n><unit>`
/// accepts `ns`/`us`/`ms`/`s`/`m`/`h` (etcli-zig's `duration.zig` contract,
/// ported at reduced scope — no modeled verb in this task's subset uses a
/// `kind::duration` flag, so this exists only to keep the `kind` switch
/// exhaustive and round-trippable for a future schema catalog).
auto parse_duration_nanos(std::string_view text) -> std::optional<std::int64_t> {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
  }
  if (i == 0) {
    return std::nullopt;
  }
  std::int64_t num     = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + i, num);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  std::string_view       unit          = text.substr(i);
  constexpr std::int64_t k_ns_per_us   = 1'000;
  constexpr std::int64_t k_ns_per_ms   = 1'000'000;
  constexpr std::int64_t k_ns_per_s    = 1'000'000'000;
  constexpr std::int64_t k_ns_per_min  = 60LL * k_ns_per_s;
  constexpr std::int64_t k_ns_per_hour = 60LL * k_ns_per_min;
  if (unit.empty() || unit == "s") {
    return num * k_ns_per_s;
  }
  if (unit == "ns") {
    return num;
  }
  if (unit == "us") {
    return num * k_ns_per_us;
  }
  if (unit == "ms") {
    return num * k_ns_per_ms;
  }
  if (unit == "m") {
    return num * k_ns_per_min;
  }
  if (unit == "h") {
    return num * k_ns_per_hour;
  }
  return std::nullopt;
}

struct matched_flag {
  std::size_t                idx = 0;
  std::optional<std::string> inline_value;
  bool                       negated = false;
};

auto match_short_flag(std::vector<flag> const& flags, char short_char) -> std::optional<std::size_t> {
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (flags[i].short_name && *flags[i].short_name == short_char) {
      return i;
    }
  }
  return std::nullopt;
}

auto match_flag(std::vector<flag> const& flags, std::string_view tok) -> std::optional<matched_flag> {
  if (tok.size() >= 2 && tok[0] == '-' && tok[1] == '-') {
    for (std::size_t i = 0; i < flags.size(); ++i) {
      auto const& f = flags[i];
      if (flag_long_matches(f, tok)) {
        return matched_flag{.idx = i};
      }
      if (f.value_kind == kind::boolean && !f.count && flag_negation_matches(f, tok)) {
        return matched_flag{.idx = i, .negated = true};
      }
      auto try_inline = [&](std::string_view name) -> std::optional<std::string> {
        if (tok.starts_with(name) && tok.size() > name.size() && tok[name.size()] == '=') {
          return std::string(tok.substr(name.size() + 1));
        }
        return std::nullopt;
      };
      if (auto v = try_inline(f.long_name)) {
        return matched_flag{.idx = i, .inline_value = std::move(v)};
      }
      for (auto const& alias : f.aliases) {
        if (auto v = try_inline(alias)) {
          return matched_flag{.idx = i, .inline_value = std::move(v)};
        }
      }
    }
    return std::nullopt;
  }
  if (tok.size() == 2 && tok[0] == '-') {
    if (auto idx = match_short_flag(flags, tok[1])) {
      return matched_flag{.idx = *idx};
    }
  }
  return std::nullopt;
}

/// @brief Strip a numeric token's accept-set down to what
/// `std::from_chars` itself understands, matching zig's
/// `std.fmt.parseInt`/`std.fmt.parseFloat` accept-set (both are the
/// actual oracle behind `--plan`/`--priority`/etc, see
/// vendor/etcli-zig/src/cli/parser.zig:651,1039,1046,658,1046).
///
/// Both zig parsers accept a single leading '+' (which `from_chars`
/// rejects outright) and '_' digit separators (which `from_chars` never
/// understands). This layer normalizes those two zig-only extensions
/// away — dropping a leading '+' and stripping legal '_' separators —
/// so the cleaned token can be hard-parsed by `from_chars` unchanged.
/// Returns unset when the token uses '_' illegally (leading, trailing,
/// or — for floats — not directly between two digits), matching each
/// zig parser's own rejection rule for that case.
namespace numeric {

auto is_ascii_digit(char c) -> bool {
  return c >= '0' && c <= '9';
}

/// @brief Mirrors `parseIntWithSign`: reject only when the digit run
/// starts or ends with '_'; otherwise '_' (including consecutive runs)
/// is simply dropped.
auto normalize_int_token(std::string_view raw) -> std::optional<std::string> {
  if (raw.empty()) {
    return std::nullopt;
  }
  bool        negative = false;
  std::size_t start    = 0;
  if (raw.front() == '+' || raw.front() == '-') {
    negative = raw.front() == '-';
    start    = 1;
  }
  const auto digits = raw.substr(start);
  if (digits.empty() || digits.front() == '_' || digits.back() == '_') {
    return std::nullopt;
  }
  std::string cleaned;
  cleaned.reserve(digits.size() + 1);
  if (negative) {
    cleaned.push_back('-');
  }
  for (char c : digits) {
    if (c != '_') {
      cleaned.push_back(c);
    }
  }
  return cleaned;
}

/// @brief Mirrors zig's float number-scanner: every '_' must sit
/// directly between two ASCII digits (not before/after the sign, the
/// decimal point, an exponent marker, or the ends of the token; no two
/// consecutive '_'). Exponent forms and a single leading '+'/'-' are
/// otherwise left to `from_chars` to validate/parse.
auto normalize_float_token(std::string_view raw) -> std::optional<std::string> {
  if (raw.empty()) {
    return std::nullopt;
  }
  bool        negative = false;
  std::size_t start    = 0;
  if (raw.front() == '+' || raw.front() == '-') {
    negative = raw.front() == '-';
    start    = 1;
  }
  const auto body = raw.substr(start);
  if (body.empty()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '_') {
      continue;
    }
    const bool prev_digit = i > 0 && is_ascii_digit(body[i - 1]);
    const bool next_digit = i + 1 < body.size() && is_ascii_digit(body[i + 1]);
    if (!prev_digit || !next_digit) {
      return std::nullopt;
    }
  }
  std::string cleaned;
  cleaned.reserve(body.size() + 1);
  if (negative) {
    cleaned.push_back('-');
  }
  for (char c : body) {
    if (c != '_') {
      cleaned.push_back(c);
    }
  }
  return cleaned;
}

} // namespace numeric

/// @brief Coerce `raw` to `f.value_kind` and store it into `flags_out`
/// under `f.long_name`. List flags append; scalar flags overwrite (the
/// caller has already checked duplicate-ness for non-list flags).
auto coerce_and_store(flag const& f, std::string_view raw, std::unordered_map<std::string, value>& flags_out)
    -> std::expected<void, parse_error_kind> {
  if (f.value_kind == kind::choice && !is_choice_member(f.choices, raw)) {
    return std::unexpected(parse_error_kind::invalid_value);
  }
  value coerced;
  switch (f.value_kind) {
  case kind::boolean:
    return std::unexpected(parse_error_kind::invalid_value); // unreachable via caller's dispatch
  case kind::string:
  case kind::path:
  case kind::choice:
    coerced = std::string(raw);
    break;
  case kind::integer: {
    auto cleaned = numeric::normalize_int_token(raw);
    if (!cleaned) {
      return std::unexpected(parse_error_kind::invalid_value);
    }
    std::int64_t v       = 0;
    auto const [ptr, ec] = std::from_chars(cleaned->data(), cleaned->data() + cleaned->size(), v);
    if (ec != std::errc{} || ptr != cleaned->data() + cleaned->size()) {
      return std::unexpected(parse_error_kind::invalid_value);
    }
    coerced = v;
    break;
  }
  case kind::floating: {
    auto cleaned = numeric::normalize_float_token(raw);
    if (!cleaned) {
      return std::unexpected(parse_error_kind::invalid_value);
    }
    double v             = 0;
    auto const [ptr, ec] = std::from_chars(cleaned->data(), cleaned->data() + cleaned->size(), v);
    if (ec != std::errc{} || ptr != cleaned->data() + cleaned->size()) {
      return std::unexpected(parse_error_kind::invalid_value);
    }
    coerced = v;
    break;
  }
  case kind::duration: {
    auto ns = parse_duration_nanos(raw);
    if (!ns) {
      return std::unexpected(parse_error_kind::invalid_value);
    }
    coerced = *ns;
    break;
  }
  }
  if (f.list) {
    auto& slot = flags_out[f.long_name];
    if (!std::holds_alternative<std::vector<std::string>>(slot)) {
      slot = std::vector<std::string>{};
    }
    std::get<std::vector<std::string>>(slot).push_back(std::get<std::string>(coerced));
  } else {
    flags_out[f.long_name] = std::move(coerced);
  }
  return {};
}

auto flag_index_by_long(std::vector<flag> const& flags, std::string_view long_name) -> std::optional<std::size_t> {
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (flags[i].long_name == long_name) {
      return i;
    }
  }
  return std::nullopt;
}

auto join_path(std::span<std::string const> path, std::string_view root_name) -> std::string {
  std::string out(root_name);
  for (auto const& seg : path) {
    out += " ";
    out += seg;
  }
  return out;
}

struct leaf_parse_context {
  std::vector<flag> const&          all_flags;
  std::vector<positional> const&    positionals;
  std::vector<flag_group> const&    flag_groups;
  bool                              allow_unknown_flags;
  bool                              allow_extra_positionals;
  std::optional<std::string> const& rest_field;
  std::string                       cmd_path;
};

auto enforce_flag_groups(leaf_parse_context const& ctx, std::vector<bool> const& seen)
    -> std::expected<void, parse_error_detail> {
  for (auto const& group : ctx.flag_groups) {
    std::vector<std::string> selected;
    for (auto const& member : group.flags) {
      auto idx = flag_index_by_long(ctx.all_flags, member);
      if (!idx) {
        continue; // authoring bug (undeclared group member); not user-reachable in this tree.
      }
      if (seen[*idx]) {
        selected.push_back(ctx.all_flags[*idx].long_name);
      }
    }
    bool violation = false;
    switch (group.mode) {
    case flag_group_mode::mutually_exclusive:
      violation = selected.size() > 1;
      break;
    case flag_group_mode::required_one:
      violation = selected.empty();
      break;
    case flag_group_mode::required_exactly_one:
      violation = selected.size() != 1;
      break;
    }
    if (violation) {
      return std::unexpected(parse_error_detail{
          .kind        = parse_error_kind::flag_group_violation,
          .cmd_path    = ctx.cmd_path,
          .group       = group.name,
          .group_mode  = group.mode,
          .group_flags = selected.empty() ? group.flags : selected,
      });
    }
  }
  return {};
}

auto parse_leaf(leaf_parse_context const& ctx, std::span<std::string const> tail)
    -> std::expected<match_result, parse_error_detail> {
  match_result result;
  for (auto const& f : ctx.all_flags) {
    if (f.list) {
      result.flags[f.long_name] = std::vector<std::string>{};
    } else if (f.count) {
      result.flags[f.long_name] = std::int64_t{0};
    } else if (f.default_value) {
      result.flags[f.long_name] = *f.default_value;
    }
  }

  std::vector<bool>        seen(ctx.all_flags.size(), false);
  std::size_t              pos_filled = 0;
  std::vector<std::string> rest;
  bool                     seen_double_dash = false;

  for (std::size_t i = 0; i < tail.size(); ++i) {
    std::string_view tok = tail[i];
    if (tok.empty()) {
      continue;
    }
    if (!seen_double_dash && tok == "--") {
      seen_double_dash = true;
      continue;
    }
    bool const neg_num_positional = !seen_double_dash && looks_like_negative_number(tok) && pos_filled < ctx.positionals.size() &&
                                    ctx.positionals[pos_filled].value_kind == kind::integer;

    if (!seen_double_dash && tok.size() >= 2 && tok[0] == '-' && !neg_num_positional) {
      if (tok.size() > 2 && tok[1] != '-') {
        // Attempt short-flag expansion: attached-value (`-fvalue`) or a
        // bundle of bool shorts (`-vvv`).
        auto first_idx = match_short_flag(ctx.all_flags, tok[1]);
        bool handled   = false;
        if (first_idx) {
          flag const& first_flag = ctx.all_flags[*first_idx];
          if (first_flag.value_kind != kind::boolean) {
            std::string_view raw = tok.substr(2);
            if (!raw.empty()) {
              if (!first_flag.list && seen[*first_idx]) {
                return std::unexpected(
                    parse_error_detail{.kind = parse_error_kind::duplicate_flag, .flag_name = first_flag.long_name});
              }
              seen[*first_idx] = true;
              auto coerced     = coerce_and_store(first_flag, raw, result.flags);
              if (!coerced) {
                return std::unexpected(parse_error_detail{
                    .kind       = coerced.error(),
                    .arg        = std::string(raw),
                    .flag_name  = first_flag.long_name,
                    .suggestion = first_flag.value_kind == kind::choice ? nearest_choice(first_flag.choices, raw) : std::nullopt,
                });
              }
              handled = true;
            }
          } else {
            bool all_bool = true;
            for (std::size_t p = 1; p < tok.size(); ++p) {
              auto idx = match_short_flag(ctx.all_flags, tok[p]);
              if (!idx || ctx.all_flags[*idx].value_kind != kind::boolean) {
                all_bool = false;
                break;
              }
            }
            if (all_bool) {
              for (std::size_t p = 1; p < tok.size(); ++p) {
                auto        idx = match_short_flag(ctx.all_flags, tok[p]).value();
                flag const& f   = ctx.all_flags[idx];
                if (!f.count && seen[idx]) {
                  return std::unexpected(parse_error_detail{.kind = parse_error_kind::duplicate_flag, .flag_name = f.long_name});
                }
                seen[idx] = true;
                if (f.count) {
                  std::get<std::int64_t>(result.flags[f.long_name]) += 1;
                } else {
                  result.flags[f.long_name] = true;
                }
              }
              handled = true;
            }
          }
        }
        if (handled) {
          continue;
        }
      }

      auto matched = match_flag(ctx.all_flags, tok);
      if (matched) {
        std::size_t idx = matched->idx;
        flag const& f   = ctx.all_flags[idx];
        if (!f.list && !f.count && seen[idx]) {
          return std::unexpected(parse_error_detail{.kind = parse_error_kind::duplicate_flag, .flag_name = f.long_name});
        }
        seen[idx] = true;

        if (f.count) {
          std::get<std::int64_t>(result.flags[f.long_name]) += 1;
          continue;
        }

        if (f.value_kind == kind::boolean) {
          bool v = true;
          if (matched->negated) {
            v = false;
          } else if (matched->inline_value) {
            auto parsed = parse_bool_value(*matched->inline_value);
            if (!parsed) {
              return std::unexpected(parse_error_detail{
                  .kind = parse_error_kind::invalid_value, .arg = *matched->inline_value, .flag_name = f.long_name});
            }
            v = *parsed;
          }
          result.flags[f.long_name] = v;
          continue;
        }

        std::string raw;
        if (matched->inline_value) {
          raw = *matched->inline_value;
        } else {
          ++i;
          if (i >= tail.size()) {
            return std::unexpected(parse_error_detail{.kind = parse_error_kind::missing_value, .flag_name = f.long_name});
          }
          raw = tail[i];
        }
        auto coerced = coerce_and_store(f, raw, result.flags);
        if (!coerced) {
          return std::unexpected(parse_error_detail{
              .kind       = parse_error_kind::invalid_value,
              .arg        = raw,
              .flag_name  = f.long_name,
              .suggestion = f.value_kind == kind::choice ? nearest_choice(f.choices, raw) : std::nullopt,
          });
        }
      } else {
        if (ctx.allow_unknown_flags) {
          if (i + 1 < tail.size()) {
            std::string_view maybe_val = tail[i + 1];
            if (!(maybe_val.size() >= 1 && maybe_val[0] == '-')) {
              ++i;
            }
          }
          continue;
        }
        return std::unexpected(parse_error_detail{
            .kind       = parse_error_kind::unknown_flag,
            .arg        = std::string(tok),
            .suggestion = suggest_flag(ctx.all_flags, tok),
        });
      }
    } else {
      if (pos_filled >= ctx.positionals.size()) {
        if (ctx.allow_extra_positionals) {
          if (ctx.rest_field) {
            rest.emplace_back(tok);
          }
          continue;
        }
        return std::unexpected(parse_error_detail{.kind = parse_error_kind::too_many_positionals, .arg = std::string(tok)});
      }
      positional const& p = ctx.positionals[pos_filled];
      value             coerced;
      switch (p.value_kind) {
      case kind::boolean: {
        if (tok == "true") {
          coerced = true;
        } else if (tok == "false") {
          coerced = false;
        } else {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        break;
      }
      case kind::string:
      case kind::path:
        coerced = std::string(tok);
        break;
      case kind::integer: {
        auto cleaned = numeric::normalize_int_token(tok);
        if (!cleaned) {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        std::int64_t v       = 0;
        auto const [ptr, ec] = std::from_chars(cleaned->data(), cleaned->data() + cleaned->size(), v);
        if (ec != std::errc{} || ptr != cleaned->data() + cleaned->size()) {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        coerced = v;
        break;
      }
      case kind::floating: {
        auto cleaned = numeric::normalize_float_token(tok);
        if (!cleaned) {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        double v             = 0;
        auto const [ptr, ec] = std::from_chars(cleaned->data(), cleaned->data() + cleaned->size(), v);
        if (ec != std::errc{} || ptr != cleaned->data() + cleaned->size()) {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        coerced = v;
        break;
      }
      case kind::duration: {
        auto ns = parse_duration_nanos(tok);
        if (!ns) {
          return std::unexpected(
              parse_error_detail{.kind = parse_error_kind::invalid_value, .arg = std::string(tok), .positional_name = p.name});
        }
        coerced = *ns;
        break;
      }
      case kind::choice:
        // Positionals never carry kind::choice (matches etcli-zig's `unreachable`
        // guard); treated as a plain string here to keep the switch total
        // without a real code path exercising it.
        coerced = std::string(tok);
        break;
      }
      result.positionals[p.name] = std::move(coerced);
      ++pos_filled;
    }
  }

  for (std::size_t idx = 0; idx < ctx.all_flags.size(); ++idx) {
    if (ctx.all_flags[idx].required && !seen[idx]) {
      return std::unexpected(
          parse_error_detail{.kind = parse_error_kind::missing_required, .flag_name = ctx.all_flags[idx].long_name});
    }
  }

  if (auto groups_ok = enforce_flag_groups(ctx, seen); !groups_ok) {
    return std::unexpected(groups_ok.error());
  }

  for (std::size_t idx = 0; idx < ctx.positionals.size(); ++idx) {
    positional const& p = ctx.positionals[idx];
    if (idx >= pos_filled) {
      if (p.default_value) {
        result.positionals[p.name] = *p.default_value;
      } else if (p.required) {
        return std::unexpected(
            parse_error_detail{.kind = parse_error_kind::missing_required_positional, .positional_name = p.name});
      }
    }
  }

  if (ctx.rest_field) {
    result.rest = std::move(rest);
  }

  return result;
}

} // namespace

auto parse(cmd const& root, std::span<std::string const> argv) -> std::expected<parse_outcome, parse_error_detail> {
  if (argv.empty()) {
    return std::unexpected(parse_error_detail{.kind = parse_error_kind::unknown_subcommand, .cmd_path = root.name});
  }

  std::vector<std::string> path;
  std::vector<const cmd*>  ancestors{&root};
  std::vector<std::string> tail;

  const cmd* current     = &root;
  bool       passthrough = false;

  for (std::size_t i = 1; i < argv.size(); ++i) {
    std::string_view tok = argv[i];
    if (tok.empty()) {
      continue;
    }

    if (passthrough || tok[0] != '-' || tok == "-") {
      if (!passthrough) {
        const cmd* matched = nullptr;
        for (auto const& c : current->cmds) {
          if (command_matches(c, tok)) {
            matched = &c;
            break;
          }
        }
        if (matched != nullptr) {
          path.push_back(matched->name);
          ancestors.push_back(matched);
          current = matched;
          continue;
        }
      }
      tail.emplace_back(tok);
      continue;
    }

    if (tok == "--") {
      tail.emplace_back(tok);
      passthrough = true;
      continue;
    }

    tail.emplace_back(tok);
    if (flag_wants_value(ancestors, tok)) {
      ++i;
      if (i < argv.size()) {
        tail.emplace_back(argv[i]);
      }
    }
  }

  for (auto const& t : tail) {
    if (t == "--") {
      break;
    }
    if (t == "--help" || t == "-h") {
      return parse_outcome{.is_help = true, .help_path = path};
    }
  }

  auto const leaves = all_leaves(root);
  for (auto const& leaf : leaves) {
    if (leaf.path == path) {
      auto all_flags = collect_inherited_flags(root, path);
      all_flags.insert(all_flags.end(), leaf.node->flags.begin(), leaf.node->flags.end());
      leaf_parse_context ctx{
          .all_flags               = all_flags,
          .positionals             = leaf.node->positionals,
          .flag_groups             = leaf.node->flag_groups,
          .allow_unknown_flags     = leaf.node->allow_unknown_flags,
          .allow_extra_positionals = leaf.node->allow_extra_positionals || leaf.node->rest_field.has_value(),
          .rest_field              = leaf.node->rest_field,
          .cmd_path                = join_path(leaf.path, root.name),
      };
      auto matched = parse_leaf(ctx, tail);
      if (!matched) {
        return std::unexpected(matched.error());
      }
      matched->path = path;
      return parse_outcome{.is_help = false, .help_path = {}, .match = std::move(*matched)};
    }
  }

  const cmd* node = find_cmd(root, path);
  if (tail.empty() && node != nullptr && !node->cmds.empty()) {
    return parse_outcome{.is_help = true, .help_path = path};
  }

  std::optional<std::string> unknown = tail.empty() ? std::nullopt : std::optional<std::string>(tail.front());
  std::optional<std::string> suggestion;
  if (unknown && node != nullptr) {
    suggestion = suggest_command(node->cmds, *unknown);
  }
  return std::unexpected(parse_error_detail{
      .kind       = parse_error_kind::unknown_subcommand,
      .arg        = unknown,
      .cmd_path   = node != nullptr ? node->name : current->name,
      .suggestion = suggestion,
  });
}

} // namespace planar::cli
