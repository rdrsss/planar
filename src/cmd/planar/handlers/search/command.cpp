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
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace se = engine::search;

namespace {

// FLOAT FORMATTING (task 6261). `rank` is emitted through
// `planar.json_text`'s `append_json_double`. This file used to carry its
// own transcription of the shortest-round-trip-in-fixed-notation algorithm
// -- `format_zig_float` -- under a standing note that `planar.json_dom`'s
// `format_double` "has the `to_chars`-default behaviour and therefore the
// same divergence ... It is NOT reused here, and it is not fixed here
// either. Flagged rather than silently worked around."
//
// Fixing json_dom was task 6261's job and it is done, which would have left
// this the fourth of four transcriptions of one algorithm -- the exact D19
// shape `json_text` exists to end. See json_text.cppm for the inventory.
//
// THE DELETED COPY WAS ITSELF BROKEN, and this is not a cleanup that
// preserved behaviour. `format_zig_float` asked `to_chars` for
// `chars_format::scientific`, which spells its exponent `e+NN` for a
// non-negative exponent, and then handed that exponent text to
// `std::from_chars` -- which REJECTS a leading `+` rather than skipping
// it. The parse failed and the function fell through its own
// `return std::string{sci}` guard, emitting the RAW SCIENTIFIC form.
// Measured on the pre-consolidation installed binary:
//
//     "rank":1.4048523469614144e+01     "rank":6.588114215686955e+00
//
// `rank` is `-bm25()` and is routinely >= 1, so the broken path was the
// COMMON case; only ranks below 1 (a term appearing in nearly every row,
// where IDF collapses) took the negative exponent that `from_chars`
// accepts and came out correctly fixed. `manifest.cpp` was the only
// correct transcription of the four. So this consolidation FIXES A LIVE
// DEFECT in `search --json` -- it does not merely deduplicate -- and the
// pin in search_health_audit_leaves.t.cpp now asserts the absence of `e+`
// against a rank above 1, which is what would have caught it.
//
// `append_json_double` rather than `format_double_fixed`, because this IS a
// JSON emitter: a non-finite rank is `null`, not a bare `inf`. It stays
// unreachable for bm25, which is what the deleted copy argued; the emitter
// is now total either way rather than by argument.
//
// A NOTE ON THE BREAK-PROBE, carried forward from task 6261's body: the
// manual exponent shift survived one mutation as an EQUIVALENT MUTANT,
// because switching `chars_format` changed nothing while the shift handled
// both forms. The probe that killed it returned the SCIENTIFIC form
// verbatim. Whoever re-probes the shared definition should keep that second
// shape; the first proves nothing.

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
    json_text::append_json_double(out, h.rank);
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

/// @brief Declare the `search` leaf.
auto declare_search(CLI::App& root) -> void {
  CLI::App* search = root.add_subcommand(
      "search", "Run a full-text search across every searchable entity kind.\n\n  Queries are passed to SQLite's FTS5 MATCH "
                "operator directly.\n  Multi-word queries are AND'd unless the operator is given\n  explicitly (OR, NOT, NEAR, "
                "\"phrase\"). Tokens are unicode61-folded\n  (case-insensitive, diacritic-stripped).");
  add_string(*search, "--kind", "Restrict to a single kind");
  add_string(*search, "--status", "Restrict to a single status");
  add_string(*search, "--scope", "Restrict to a scope slug");
  add_int(*search, "--plan", "Restrict to a plan id");
  add_int_default(*search, "--limit", "50", "Max results");
  add_json(*search);
  add_positional_described(*search, "query", "FTS5 query string");
}

} // namespace planar::cmd::handlers
