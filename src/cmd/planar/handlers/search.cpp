/// @file search.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.search`.
/// See search.cppm for the flag-convention captures.

module planar.cmd.planar.handlers.search;

import std;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.search;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;

namespace planar::cmd::handlers {

namespace se = engine::search;

namespace {

/// @brief Format a double the way the oracle's `{}` on an `f64` does:
/// SHORTEST ROUND-TRIP DIGITS IN PLAIN DECIMAL, never scientific.
///
/// This exists because `std::to_chars`'s default is NOT the same function.
/// `to_chars` picks whichever of fixed and scientific is shorter, so
/// bm25's typical magnitudes come out as `1.375e-06` where the oracle
/// prints `0.000001375`. Verified against the oracle across the range —
/// `1e-20` prints as twenty zeros and a one, `1e20` as a one and twenty
/// zeros, `3.0` as `3` with no fractional part.
///
/// Implementation: ask `to_chars` for SCIENTIFIC explicitly, which gives
/// the shortest round-tripping digit string plus a base-10 exponent, then
/// move the decimal point by hand. Going through the shortest-scientific
/// form rather than `std::format("{:f}", …)` is what preserves the
/// "shortest that round-trips" property; a fixed-notation formatter has to
/// be told a precision and any choice is wrong for some input.
///
/// NOTE: `planar.json_dom`'s own internal `format_double` has the
/// `to_chars`-default behaviour and therefore the same divergence from the
/// oracle. It is NOT reused here, and it is not fixed here either — it has
/// its own callers and its own differential coverage. Flagged rather than
/// silently worked around.
/// @param value The value to render.
/// @return The decimal text, with no exponent and no trailing `.0`.
auto format_zig_float(double value) -> std::string {
  if (std::isnan(value) || std::isinf(value)) {
    // Unreachable for bm25, and JSON has no spelling for either; the
    // oracle would print `nan`/`inf` unquoted. Kept explicit so the
    // shifting below never sees input it cannot parse.
    return std::format("{}", value);
  }

  std::array<char, 64> buf{};
  auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::scientific);
  if (ec != std::errc{}) {
    return "0";
  }
  std::string_view sci{buf.data(), static_cast<std::size_t>(ptr - buf.data())};

  bool negative = false;
  if (sci.starts_with('-')) {
    negative = true;
    sci.remove_prefix(1);
  }

  auto const e_at = sci.find('e');
  if (e_at == std::string_view::npos) {
    return std::string{sci};
  }
  std::string_view mantissa = sci.substr(0, e_at);
  int              exponent = 0;
  auto const       exp_text = sci.substr(e_at + 1);
  if (std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), exponent).ec != std::errc{}) {
    return std::string{sci};
  }

  // Collapse the mantissa to its bare digits. The decimal point's position
  // is recoverable from the exponent alone, so nothing about where it SAT
  // needs remembering — `to_chars`'s scientific form always puts exactly one
  // digit before it.
  std::string digits;
  for (char const c : mantissa) {
    if (c != '.') {
      digits += c;
    }
  }
  // `point` is the number of digits that belong to the left of the decimal
  // point once the exponent is applied.
  int const point = exponent + 1;

  std::string out;
  if (point <= 0) {
    out = "0.";
    out.append(static_cast<std::size_t>(-point), '0');
    out += digits;
  } else if (static_cast<std::size_t>(point) >= digits.size()) {
    out = digits;
    out.append(static_cast<std::size_t>(point) - digits.size(), '0');
  } else {
    out = digits.substr(0, static_cast<std::size_t>(point));
    out += '.';
    out += digits.substr(static_cast<std::size_t>(point));
  }

  // Trim a trailing point left by a mantissa like `3.` (cannot happen with
  // to_chars, kept defensive) and restore the sign.
  if (out.ends_with('.')) {
    out.pop_back();
  }
  return negative ? "-" + out : out;
}

/// @brief Render the hit list as the oracle's `--json` payload.
///
/// Key order is `kind, id, slug, title, status, snippet, rank` — the
/// oracle's `HitJSON` order, which is NOT the order of the engine's `hit`
/// struct (that one has `snippet` before `rank` but `status` after both).
/// Captured from a live run rather than read off either struct.
///
/// An empty list renders `[]`, not zero bytes and not `null`.
/// @param hits The hits.
/// @return The complete stdout payload including its trailing newline.
auto render_json(std::span<const se::hit> hits) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < hits.size(); ++i) {
    auto const& h = hits[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"kind\":";
    out += json_text::json_string(h.kind);
    out += std::format(",\"id\":{},\"slug\":", h.id);
    out += json_text::json_string(h.slug);
    out += ",\"title\":";
    out += json_text::json_string(h.title);
    out += ",\"status\":";
    out += json_text::json_string(h.status);
    out += ",\"snippet\":";
    out += json_text::json_string(h.snippet);
    out += ",\"rank\":";
    out += format_zig_float(h.rank);
    out += "}";
  }
  out += "]\n";
  return out;
}

/// @brief The merge order the engine applies inside one query, reapplied
/// across the per-scope result sets.
auto hit_less(const se::hit& a, const se::hit& b) -> bool {
  if (a.rank != b.rank) {
    return a.rank > b.rank;
  }
  if (a.kind != b.kind) {
    return a.kind < b.kind;
  }
  return a.id < b.id;
}

auto map_search_error(se::search_error err) -> domain_error {
  switch (err) {
  case se::search_error::unsupported_scope:
    return error_from_body(domain_error_kind::invalid_input, "unsupported scope form");
  case se::search_error::slug_not_found:
    // exit 1, NOT 2 — the oracle maps this through `error.NotFound`.
    return error_from_body(domain_error_kind::not_found, "scope slug not found");
  case se::search_error::unknown_kind:
    // Unreachable from this handler: `--kind` is validated before the
    // engine sees it, and the message there NAMES the offending value.
    // Kept because the engine can still return it.
    return error_from_body(domain_error_kind::invalid_input, "unknown kind");
  case se::search_error::invalid_query:
    return error_from_body(domain_error_kind::invalid_input, "invalid FTS5 query syntax");
  case se::search_error::query_failed:
    return error_from_body(domain_error_kind::generic_failure, "search failed: QueryFailed");
  }
  return error_from_body(domain_error_kind::generic_failure, "search failed: QueryFailed");
}

} // namespace

auto search(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const query_str = cliapp::positional_string(args, "query");
  if (!query_str) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "query is required"));
  }

  se::search_filter filter;

  // `--kind` is the ONE validated flag on this verb. An unrecognised value
  // refuses and NAMES itself; see search.cppm on why the sibling flags do
  // not do the same.
  if (auto const kind = cliapp::flag_string(args, "--kind"); kind.has_value()) {
    bool known = false;
    for (auto const valid : se::valid_kinds()) {
      if (valid == *kind) {
        known = true;
        break;
      }
    }
    if (!known) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown kind '{}'", *kind)));
    }
    filter.kinds.push_back(*kind);
  }

  // `--status ""` becomes a one-element list holding the empty string, and
  // therefore matches NOTHING. Deliberate; see search.cppm.
  if (auto const status = cliapp::flag_string(args, "--status"); status.has_value()) {
    filter.statuses.push_back(*status);
  }

  if (auto const plan = cliapp::flag_int(args, "--plan"); plan.has_value() && *plan != 0) {
    // Zero means "not provided" — the flag has no default in the oracle
    // and its `args.plan` arrives as 0 when absent. So `--plan 0` is
    // indistinguishable from no `--plan` at all, which is the oracle's
    // behaviour and not a bug this port introduces.
    filter.plan_id = *plan;
  }

  auto const limit = cliapp::flag_int(args, "--limit");
  filter.limit     = limit.value_or(se::k_default_limit);

  std::vector<se::hit> hits;
  auto const           scope_flag = cliapp::flag_string(args, "--scope");
  if (scope_flag.has_value()) {
    // An EMPTY `--scope` means cross-scope, not "the empty-named scope".
    if (!scope_flag->empty()) {
      filter.scope = *scope_flag;
    }
    auto rows = se::query(**conn, *query_str, filter);
    if (!rows) {
      return std::unexpected(map_search_error(rows.error()));
    }
    hits = std::move(*rows);
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    for (auto const& slug : *slugs) {
      filter.scope = slug;
      auto rows    = se::query(**conn, *query_str, filter);
      if (!rows) {
        return std::unexpected(map_search_error(rows.error()));
      }
      hits.insert(hits.end(), std::make_move_iterator(rows->begin()), std::make_move_iterator(rows->end()));
    }
    std::ranges::stable_sort(hits, hit_less);
    // The cap applies to the MERGED list, not per scope.
    auto const cap = static_cast<std::size_t>(filter.limit > 0 ? filter.limit : se::k_default_limit);
    if (hits.size() > cap) {
      hits.resize(cap);
    }
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render_json(hits) : se::render_list_text(hits));
  return {};
}

} // namespace planar::cmd::handlers
