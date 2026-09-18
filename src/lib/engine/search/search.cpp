/// @file search.cpp
/// @brief Implementation of `planar.engine.search` (plan 996, task 6090).
/// See search.cppm for scope and the reproduced-quirk notes.

module planar.engine.search;

import std;
import planar.db;
import planar.scope_ref;

namespace planar::engine::search {

namespace {

/// @brief The three table names one `UNION ALL` arm needs.
struct kind_config {
  std::string_view fts_table;        ///< The FTS5 virtual table.
  std::string_view base_table;       ///< The source table joined back on `rowid`.
  std::string_view entity_link_kind; ///< The `entity_links.from_kind` label for this kind.
};

/// @brief The six kinds, in the order the oracle emits arms.
///
/// `scenario` maps to `test_scenarios` / `test_scenario` — the CLI-facing
/// name, the table name and the `entity_links` label are three different
/// strings for that one kind, which is exactly why this table exists
/// instead of a name-mangling rule.
constexpr std::array<std::pair<std::string_view, kind_config>, 6> k_kinds{{
    {"plan", {"plans_fts", "plans", "plan"}},
    {"task", {"tasks_fts", "tasks", "task"}},
    {"question", {"questions_fts", "questions", "question"}},
    {"scenario", {"test_scenarios_fts", "test_scenarios", "test_scenario"}},
    {"decision", {"decisions_fts", "decisions", "decision"}},
    {"artifact", {"artifacts_fts", "artifacts", "artifact"}},
}};

constexpr std::array<std::string_view, 6> k_valid_kinds{"plan", "task", "question", "scenario", "decision", "artifact"};

auto config_for(std::string_view kind) -> std::optional<kind_config> {
  for (auto const& [name, cfg] : k_kinds) {
    if (name == kind) {
      return cfg;
    }
  }
  return std::nullopt;
}

/// @brief One value to bind, in the order the composed SQL expects it.
struct bind_value {
  bool         is_text = true;
  std::string  text;
  std::int64_t number = 0;
};

} // namespace

auto valid_kinds() -> std::span<const std::string_view> {
  return k_valid_kinds;
}

auto query(db::connection& conn, std::string_view query_str, const search_filter& filter)
    -> std::expected<std::vector<hit>, search_error> {
  // Resolve the kind list. EMPTY means all six; it does not mean none.
  std::vector<std::string_view> active_kinds;
  if (filter.kinds.empty()) {
    active_kinds.assign(k_valid_kinds.begin(), k_valid_kinds.end());
  } else {
    for (auto const& k : filter.kinds) {
      if (!config_for(k)) {
        return std::unexpected(search_error::unknown_kind);
      }
      active_kinds.push_back(k);
    }
  }

  // Resolve the scope ONCE; the same predicate goes into every arm.
  std::optional<scope_ref::slug_ref> resolved_scope;
  if (filter.scope.has_value()) {
    auto ref = scope_ref::resolve(conn, *filter.scope);
    if (!ref) {
      switch (ref.error()) {
      case scope_ref::error::slug_not_found:
        return std::unexpected(search_error::slug_not_found);
      case scope_ref::error::query_failed:
        return std::unexpected(search_error::query_failed);
      }
      return std::unexpected(search_error::query_failed);
    }
    resolved_scope = *ref;
  }

  std::int64_t const limit = filter.limit > 0 ? filter.limit : k_default_limit;

  std::string             sql;
  std::vector<bind_value> binds;

  for (std::size_t i = 0; i < active_kinds.size(); ++i) {
    auto const kind = active_kinds[i];
    auto const cfg  = *config_for(kind);

    if (i > 0) {
      sql += "\nUNION ALL\n";
    }

    // The literal kind label is INTERPOLATED, not bound: it is one of six
    // compile-time constants reached only through `config_for`, never
    // operator input, and a bound parameter cannot appear in the select
    // list position the oracle puts it in.
    // Only plans and tasks carry a slug column; migration 00037 dropped the
    // four that never held a value (task 6808). The other kinds always
    // rendered as `kind:id`, and still do, so the hit shape is unchanged.
    std::string_view const slug_expr = (kind == "plan" || kind == "task") ? "COALESCE(b.slug,'')" : "''";
    sql += std::format("SELECT '{}' AS kind, f.rowid AS id, {} AS slug, b.title AS title, "
                       "snippet({}, -1, '<mark>', '</mark>', '…', 32) AS snippet, -bm25({}) AS rank, "
                       "COALESCE(b.status,'') AS status, b.scope_kind AS scope_kind, b.scope_id AS scope_id "
                       "FROM {} AS f JOIN {} AS b ON b.id = f.rowid WHERE {} MATCH ?",
                       kind, slug_expr, cfg.fts_table, cfg.fts_table, cfg.fts_table, cfg.base_table, cfg.fts_table);
    binds.push_back(bind_value{.is_text = true, .text = std::string{query_str}});

    if (!filter.statuses.empty()) {
      sql += " AND b.status IN (";
      for (std::size_t j = 0; j < filter.statuses.size(); ++j) {
        if (j > 0) {
          sql += ", ";
        }
        sql += "?";
        binds.push_back(bind_value{.is_text = true, .text = filter.statuses[j]});
      }
      sql += ")";
    }

    if (resolved_scope.has_value()) {
      switch (resolved_scope->kind) {
      case scope_ref::scope_kind::global:
        sql += " AND b.scope_kind = 'global'";
        break;
      case scope_ref::scope_kind::association:
        sql += " AND b.scope_kind = 'association' AND b.scope_id = ?";
        binds.push_back(bind_value{.is_text = false, .number = resolved_scope->id.value_or(0)});
        break;
      case scope_ref::scope_kind::repo:
        sql += " AND b.scope_kind = 'repo' AND b.scope_id = ?";
        binds.push_back(bind_value{.is_text = false, .number = resolved_scope->id.value_or(0)});
        break;
      }
    }

    if (filter.plan_id.has_value()) {
      if (kind == "task") {
        sql += " AND b.plan_id = ?";
        binds.push_back(bind_value{.is_text = false, .number = *filter.plan_id});
      } else {
        sql += " AND EXISTS (SELECT 1 FROM entity_links AS el WHERE el.from_kind = ? AND el.from_id = b.id"
               " AND el.to_kind = 'plan' AND el.to_id = ? AND el.relationship = 'derives-from')";
        binds.push_back(bind_value{.is_text = true, .text = std::string{cfg.entity_link_kind}});
        binds.push_back(bind_value{.is_text = false, .number = *filter.plan_id});
      }
    }
  }

  // ORDER and LIMIT are appended ONCE, after the whole UNION — so the cap
  // applies to the merged result, not per arm.
  sql += "\nORDER BY rank DESC, kind ASC, id ASC\nLIMIT ?";
  binds.push_back(bind_value{.is_text = false, .number = limit});

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    // A prepare failure here is FTS5 rejecting the composed statement, and
    // the only operator-controlled part of it is `query_str`. The oracle
    // maps the same failure to `InvalidQuery` rather than `QueryFailed`.
    return std::unexpected(search_error::invalid_query);
  }
  for (std::size_t i = 0; i < binds.size(); ++i) {
    auto const  index = static_cast<int>(i) + 1;
    auto const& b     = binds[i];
    auto const  ok    = b.is_text ? stmt->bind_text(index, b.text) : stmt->bind_int64(index, b.number);
    if (!ok) {
      return std::unexpected(search_error::query_failed);
    }
  }

  std::vector<hit> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      // A step failure is also where a syntactically-valid-but-unmatchable
      // FTS5 expression surfaces; the oracle maps step failures here to
      // `InvalidQuery` too, not to `QueryFailed`.
      return std::unexpected(search_error::invalid_query);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(hit{
        .kind       = stmt->column_text(0),
        .id         = stmt->column_int64(1),
        .slug       = stmt->column_text(2),
        .title      = stmt->column_text(3),
        .snippet    = stmt->column_text(4),
        .rank       = stmt->column_double(5),
        .status     = stmt->column_text(6),
        .scope_kind = stmt->column_text(7),
        .scope_id   = stmt->is_null(8) ? std::nullopt : std::optional<std::int64_t>{stmt->column_int64(8)},
    });
  }
}

auto render_list_text(std::span<const hit> hits) -> std::string {
  if (hits.empty()) {
    return "(no results)\n";
  }
  std::string out;
  for (auto const& h : hits) {
    // The oracle formats this into a fixed [256]u8 and falls back to the
    // BARE KIND on overflow. See search.cppm; reproduced, not fixed.
    std::string ref = h.slug.empty() ? std::format("{}:{}", h.kind, h.id) : std::format("{}:{} ({})", h.kind, h.id, h.slug);
    if (ref.size() > 256) {
      ref = h.kind;
    }
    if (h.status.empty()) {
      out += std::format("{} — {}\n", ref, h.title);
    } else {
      out += std::format("{} [{}] — {}\n", ref, h.status, h.title);
    }
    if (!h.snippet.empty()) {
      out += std::format("  {}\n", h.snippet);
    }
  }
  return out;
}

} // namespace planar::engine::search
