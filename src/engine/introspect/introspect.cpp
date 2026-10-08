/// @file introspect.cpp
/// @brief Implementation of `planar.engine.introspect` (plan 996, tasks
/// 6121 and 6352). See introspect.cppm for scope and omissions.

module planar.engine.introspect;

import std;
import planar.db;
import planar.json_text;
import planar.introspection_preview;

namespace planar::engine::introspect {

namespace ip = planar::introspection_preview;

namespace {

using json_text::append_json_string;

/// @brief `@tagName` equivalents for the four preview enums, so
/// `render_text`/`render_json` can spell them the way the oracle's
/// `@tagName(...)` does — plain lowercase identifiers, no quoting logic
/// duplicated per call site.
auto tag_name(ip::vendor v) -> std::string_view {
  switch (v) {
  case ip::vendor::claude:
    return "claude";
  case ip::vendor::codex:
    return "codex";
  case ip::vendor::copilot:
    return "copilot";
  case ip::vendor::cli_log:
    return "cli_log";
  }
  return "unknown";
}

auto tag_name(ip::category c) -> std::string_view {
  switch (c) {
  case ip::category::failure:
    return "failure";
  case ip::category::retry:
    return "retry";
  case ip::category::abandonment:
    return "abandonment";
  case ip::category::gap:
    return "gap";
  }
  return "unknown";
}

auto tag_name(ip::coverage_state s) -> std::string_view {
  switch (s) {
  case ip::coverage_state::observed:
    return "observed";
  case ip::coverage_state::unavailable:
    return "unavailable";
  case ip::coverage_state::disabled:
    return "disabled";
  }
  return "unknown";
}

auto tag_name(ip::warning_kind k) -> std::string_view {
  switch (k) {
  case ip::warning_kind::unavailable:
    return "unavailable";
  case ip::warning_kind::disabled:
    return "disabled";
  case ip::warning_kind::malformed:
    return "malformed";
  case ip::warning_kind::file_cap:
    return "file_cap";
  case ip::warning_kind::byte_cap:
    return "byte_cap";
  case ip::warning_kind::record_cap:
    return "record_cap";
  case ip::warning_kind::evidence_cap:
    return "evidence_cap";
  case ip::warning_kind::cli_adapter_failed:
    return "cli_adapter_failed";
  }
  return "unknown";
}

/// @brief `-{window_days} days`, the SQLite `datetime()` modifier bound at
/// every windowed query below (a bind parameter, not string-formatted SQL —
/// C++-idiomatic replacement for the oracle's stack-buffered
/// `std::fmt.bufPrint`, same runtime behavior).
auto window_modifier(std::int64_t window_days) -> std::string {
  return std::format("-{} days", window_days);
}

/// @brief Run a single scalar `select count(*) ...`-shaped query with one
/// bound window modifier, returning `fallback` on any prepare/bind/step
/// failure — mirrors the oracle's `intQuery(...) catch <fallback>` calls.
auto count_query(db::connection& conn, std::string_view sql, std::int64_t window_days, std::int64_t fallback) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return fallback;
  }
  if (!stmt->bind_text(1, window_modifier(window_days))) {
    return fallback;
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return fallback;
  }
  return stmt->column_int64(0);
}

/// @brief Same as `count_query` but with no window bind at all (the
/// unwindowed `health` scalar queries).
auto count_query_unwindowed(db::connection& conn, std::string_view sql, std::int64_t fallback) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return fallback;
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return fallback;
  }
  return stmt->column_int64(0);
}

/// @brief Degraded if `not_resumable_tasks > 0` or `stale_handoffs > 0`.
/// Lightweight re-derivation rather than importing the full `health` module
/// (mirrors the oracle's own `healthSummary`, which does the same thing for
/// the same reason).
///
/// `inflight`'s failure path is FAIL-CLOSED, unlike every other fallback in
/// this file: the oracle's `healthSummary` (zig:334-336) reads
/// `intQuery(...) catch return "degraded"` for this one query specifically —
/// a query failure there reports degraded immediately, rather than falling
/// through to the `not_resumable`/`stale_handoffs` arithmetic on a
/// fabricated `0` (which would read as `not_resumable <= 0`, i.e. "healthy",
/// exactly backwards for a health signal). `resumable` and `stale_handoffs`
/// keep the oracle's `catch 0` (zig:338, zig:343/351) unchanged — their `0`
/// fallbacks are intentional, not a second instance of this bug: both only
/// ever subtract from or add to `inflight`'s already-verified-good count.
auto health_summary(db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select count(*) from tasks where status in ('doing','blocked')");
  if (!stmt) {
    return "degraded";
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return "degraded";
  }
  auto const inflight = stmt->column_int64(0);

  auto const resumable = count_query_unwindowed(conn,
                                                "select count(*) from tasks t"
                                                " where t.status in ('doing','blocked')"
                                                "   and coalesce(t.next_action,'') != ''"
                                                "   and exists (select 1 from context_snapshots cs where cs.task_id = t.id)",
                                                0);

  auto const not_resumable = inflight - resumable;

  auto const stale_handoffs = count_query_unwindowed(conn,
                                                     "select count(*) from handoffs"
                                                     " where status in ('pending','validated')"
                                                     "   and (julianday('now') - julianday(created_at)) * 24 > 24",
                                                     0);

  if (not_resumable > 0 || stale_handoffs > 0) {
    return "degraded";
  }
  return "ok";
}

auto query_invocations(db::connection& conn, std::int64_t window_days)
    -> std::expected<std::vector<verb_count>, introspect_error> {
  auto stmt = conn.prepare("select verb_path,"
                           " count(*) as total,"
                           " sum(case when exit_code = 0 then 1 else 0 end) as successes,"
                           " sum(case when exit_code != 0 then 1 else 0 end) as failures"
                           " from cli_invocations"
                           " where recorded_at >= datetime('now', ?)"
                           " group by verb_path"
                           " order by total desc");
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return std::unexpected(introspect_error::query_failed);
  }

  std::vector<verb_count> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(introspect_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(verb_count{
        .verb_path     = stmt->column_text(0),
        .count         = stmt->column_int64(1),
        .success_count = stmt->column_int64(2),
        .failure_count = stmt->column_int64(3),
    });
  }
}

auto query_failure_categories(db::connection& conn, std::int64_t window_days)
    -> std::expected<std::vector<failure_category>, introspect_error> {
  auto stmt = conn.prepare("select coalesce(error_category, 'unknown'), count(*)"
                           " from cli_invocations"
                           " where exit_code != 0"
                           "   and recorded_at >= datetime('now', ?)"
                           " group by error_category"
                           " order by count(*) desc");
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return std::unexpected(introspect_error::query_failed);
  }

  std::vector<failure_category> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(introspect_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(failure_category{.category = stmt->column_text(0), .count = stmt->column_int64(1)});
  }
}

auto query_failure_tail(db::connection& conn, std::int64_t window_days, std::int64_t tail_n)
    -> std::expected<std::vector<failure_tail_row>, introspect_error> {
  auto stmt = conn.prepare("select verb_path, coalesce(error_category,'unknown'), exit_code, recorded_at"
                           " from cli_invocations"
                           " where exit_code != 0"
                           "   and recorded_at >= datetime('now', ?)"
                           " order by recorded_at desc"
                           " limit ?");
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days)) || !stmt->bind_int64(2, tail_n)) {
    return std::unexpected(introspect_error::query_failed);
  }

  std::vector<failure_tail_row> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(introspect_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(failure_tail_row{
        .verb_path      = stmt->column_text(0),
        .error_category = stmt->column_text(1),
        .exit_code      = stmt->column_int64(2),
        .recorded_at    = stmt->column_text(3),
    });
  }
}

/// @brief `agent_actions` outcome aggregates. Fail-open to an empty vector
/// on prepare/step failure — mirrors the oracle's own "table may not exist
/// in older schemas" fallback (`catch return &.{}` / `catch break`).
auto query_action_outcomes(db::connection& conn, std::int64_t window_days) -> std::vector<action_outcome> {
  auto                        stmt = conn.prepare("select action_kind, coalesce(outcome,'unknown'), count(*)"
                                                  " from agent_actions"
                                                  " where started_at >= datetime('now', ?)"
                                                  " group by action_kind, outcome"
                                                  " order by count(*) desc");
  std::vector<action_outcome> rows;
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return rows;
  }
  while (true) {
    auto step = stmt->step();
    if (!step || *step != db::step_result::row) {
      return rows;
    }
    rows.push_back(action_outcome{
        .action_kind = stmt->column_text(0),
        .outcome     = stmt->column_text(1),
        .count       = stmt->column_int64(2),
    });
  }
}

/// @brief `sync_events` outcome aggregates. Same fail-open contract as
/// `query_action_outcomes`.
auto query_sync_outcomes(db::connection& conn, std::int64_t window_days) -> std::vector<sync_outcome> {
  auto                      stmt = conn.prepare("select coalesce(outcome,'unknown'), count(*)"
                                                " from sync_events"
                                                " where at >= datetime('now', ?)"
                                                " group by outcome"
                                                " order by count(*) desc");
  std::vector<sync_outcome> rows;
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return rows;
  }
  while (true) {
    auto step = stmt->step();
    if (!step || *step != db::step_result::row) {
      return rows;
    }
    rows.push_back(sync_outcome{.outcome = stmt->column_text(0), .count = stmt->column_int64(1)});
  }
}

auto query_claim_counts(db::connection& conn, std::int64_t window_days) -> claim_counts {
  auto const stale          = count_query(conn,
                                          "select count(*) from agent_work_claims"
                                          " where status = 'active'"
                                          "   and (julianday('now') - julianday(claimed_at)) * 24 > 24"
                                          "   and claimed_at >= datetime('now', ?)",
                                          window_days, 0);
  auto const never_consumed = count_query(conn,
                                          "select count(*) from agent_work_claims"
                                          " where status in ('expired','released')"
                                          "   and claimed_at >= datetime('now', ?)",
                                          window_days, 0);
  return claim_counts{.stale_claims = stale, .never_consumed = never_consumed};
}

/// @brief Aggregate failed/recovered claims by provider and the closed
/// terminal category. Null/uncategorized recovery is the bounded `unknown`
/// bucket. Completed/released claims are non-failure terminals and are
/// deliberately excluded.
auto query_claim_failure_categories(db::connection& conn, std::int64_t window_days)
    -> std::expected<std::vector<claim_failure_category_count>, introspect_error> {
  auto stmt = conn.prepare("select vendor, coalesce(failure_category, 'unknown') as category, count(*)"
                           " from agent_work_claims"
                           " where status in ('aborted','stale')"
                           "   and claimed_at >= datetime('now', ?)"
                           " group by vendor, coalesce(failure_category, 'unknown')"
                           " order by vendor asc,"
                           " case coalesce(failure_category, 'unknown')"
                           "   when 'usage_limit' then 1"
                           "   when 'context_limit' then 2"
                           "   when 'output_limit' then 3"
                           "   when 'tool_failure' then 4"
                           "   when 'validation' then 5"
                           "   when 'unknown' then 6"
                           "   else 7 end asc");
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return std::unexpected(introspect_error::query_failed);
  }

  std::vector<claim_failure_category_count> rows;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(introspect_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return rows;
    }
    rows.push_back(claim_failure_category_count{
        .provider = stmt->column_text(0),
        .category = stmt->column_text(1),
        .count    = stmt->column_int64(2),
    });
  }
}

auto query_handoff_counts(db::connection& conn, std::int64_t window_days) -> handoff_counts {
  auto const stale = count_query(conn,
                                 "select count(*) from handoffs"
                                 " where status in ('pending','validated')"
                                 "   and (julianday('now') - julianday(created_at)) * 24 > 24"
                                 "   and created_at >= datetime('now', ?)",
                                 window_days, 0);
  // "never consumed" = status is not 'consumed', regardless of staleness —
  // distinct from `stale` above, which requires age > 24h.
  auto const never_consumed = count_query(conn,
                                          "select count(*) from handoffs"
                                          " where status != 'consumed'"
                                          "   and created_at >= datetime('now', ?)",
                                          window_days, 0);
  return handoff_counts{.stale_handoffs = stale, .never_consumed = never_consumed};
}

auto query_reopen_count(db::connection& conn, std::int64_t window_days) -> std::int64_t {
  return count_query(conn, "select count(*) from task_reopens where created_at >= datetime('now', ?)", window_days, 0);
}

/// @brief The `render_text` preview block. Mirrors the oracle's
/// `renderText` preview section (zig:872-909) in full, now that `bundle`
/// carries a real `preview` field (decision 981, task 6352): `unset`
/// renders `" unavailable\n"`, present-but-empty-coverage renders
/// `" empty\n"`, and a populated preview renders one line per coverage
/// row, then one per signal, then one per warning — in that order, the
/// order `preview`'s three vectors are already sorted/collected in.
auto preview_text_block(const std::optional<ip::preview>& preview) -> std::string {
  std::string out = "[introspection preview]";
  if (!preview.has_value()) {
    out += " unavailable\n";
    return out;
  }
  if (preview->coverage.empty()) {
    out += " empty\n";
    return out;
  }
  out += "\n";
  for (auto const& coverage : preview->coverage) {
    out += std::format("  {}: state={} scanned={} normalized={} ignored={} malformed={} capped={}\n", tag_name(coverage.v),
                       tag_name(coverage.state), coverage.scanned, coverage.normalized, coverage.ignored, coverage.malformed,
                       coverage.capped);
  }
  for (auto const& signal : preview->signals) {
    out += std::format("  signal {}/{}/{}: count={} first={} last={}\n", tag_name(signal.v), signal.verb_path,
                       tag_name(signal.cat), signal.count, signal.first_seen, signal.last_seen);
  }
  for (auto const& warning : preview->warnings) {
    out += std::format("  warning {}/{}: count={}\n", tag_name(warning.v), tag_name(warning.kind), warning.count);
  }
  return out;
}

/// @brief The `render_json` `"introspection_preview"` block. Mirrors the
/// oracle's `renderJson` preview object shape (zig:1013-1040): `signals`,
/// `coverage`, `warnings`, each empty when `preview` is unset (the
/// oracle's `if (bundle.preview) |preview| { ... }` per-array guard, which
/// leaves every array `[]` rather than omitting the key).
auto preview_json_block(const std::optional<ip::preview>& preview) -> std::string {
  std::string out = R"(,"introspection_preview":{"signals":[)";
  if (preview.has_value()) {
    for (std::size_t i = 0; i < preview->signals.size(); ++i) {
      auto const& signal = preview->signals[i];
      if (i > 0) {
        out += ',';
      }
      out += std::format(R"({{"vendor":"{}","verb_path":)", tag_name(signal.v));
      append_json_string(out, signal.verb_path);
      out += std::format(R"(,"category":"{}","count":{},"first_seen":)", tag_name(signal.cat), signal.count);
      append_json_string(out, signal.first_seen);
      out += R"(,"last_seen":)";
      append_json_string(out, signal.last_seen);
      out += '}';
    }
  }
  out += R"(],"coverage":[)";
  if (preview.has_value()) {
    for (std::size_t i = 0; i < preview->coverage.size(); ++i) {
      auto const& coverage = preview->coverage[i];
      if (i > 0) {
        out += ',';
      }
      out += std::format(R"({{"vendor":"{}","state":"{}","scanned":{},"malformed":{},"normalized":{},"capped":{},"ignored":{}}})",
                         tag_name(coverage.v), tag_name(coverage.state), coverage.scanned, coverage.malformed,
                         coverage.normalized, coverage.capped, coverage.ignored);
    }
  }
  out += R"(],"warnings":[)";
  if (preview.has_value()) {
    for (std::size_t i = 0; i < preview->warnings.size(); ++i) {
      auto const& warning = preview->warnings[i];
      if (i > 0) {
        out += ',';
      }
      out +=
          std::format(R"({{"vendor":"{}","kind":"{}","count":{}}})", tag_name(warning.v), tag_name(warning.kind), warning.count);
    }
  }
  out += "]}";
  return out;
}

} // namespace

auto build(db::connection& conn, std::int64_t window_days, std::int64_t tail_n, bool logging_enabled, std::string_view version)
    -> std::expected<bundle, introspect_error> {
  bundle result;
  result.version         = std::string{version};
  result.schema_version  = count_query_unwindowed(conn, "select coalesce(max(version), 0) from schema_migrations", 0);
  result.health          = health_summary(conn);
  result.window_days     = window_days;
  result.logging_enabled = logging_enabled;

  if (logging_enabled) {
    auto invocations = query_invocations(conn, window_days);
    if (!invocations) {
      return std::unexpected(invocations.error());
    }
    auto failures = query_failure_categories(conn, window_days);
    if (!failures) {
      return std::unexpected(failures.error());
    }
    auto tail = query_failure_tail(conn, window_days, tail_n);
    if (!tail) {
      return std::unexpected(tail.error());
    }
    result.invocations  = std::move(*invocations);
    result.failures     = std::move(*failures);
    result.failure_tail = std::move(*tail);
  }

  result.actions = query_action_outcomes(conn, window_days);
  result.sync    = query_sync_outcomes(conn, window_days);
  result.claims  = query_claim_counts(conn, window_days);

  auto claim_failures = query_claim_failure_categories(conn, window_days);
  if (!claim_failures) {
    return std::unexpected(claim_failures.error());
  }
  result.claim_failure_categories = std::move(*claim_failures);

  result.handoffs = query_handoff_counts(conn, window_days);
  result.reopens  = query_reopen_count(conn, window_days);

  return result;
}

auto cli_preview_jsonl(db::connection& conn, std::int64_t window_days, std::size_t max_bytes)
    -> std::expected<cli_preview, introspect_error> {
  auto stmt = conn.prepare("select"
                           " case when verb_path like 'planar %' then verb_path else 'planar ' || verb_path end,"
                           " exit_code,"
                           " coalesce(error_category,''),"
                           " case when substr(recorded_at,-1)='Z' or substr(recorded_at,-6,1) in ('+','-')"
                           "      then replace(recorded_at,' ','T')"
                           "      else replace(recorded_at,' ','T') || 'Z' end"
                           " from cli_invocations"
                           " where recorded_at >= datetime('now', ?)"
                           " order by recorded_at desc, id desc");
  if (!stmt || !stmt->bind_text(1, window_modifier(window_days))) {
    return std::unexpected(introspect_error::query_failed);
  }

  // Newest first: rows are taken until the next one would push the text past
  // `max_bytes`; the rest of the window is only counted.
  std::vector<std::string> newest_first;
  std::size_t              used = 0;
  cli_preview              result;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(introspect_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    if (result.truncated) {
      ++result.omitted;
      continue;
    }
    std::string row = R"({"schema":1,"kind":"cli_invocation","verb_path":)";
    append_json_string(row, stmt->column_text(0));
    row += std::format(",\"exit_code\":{},\"error_category\":", stmt->column_int64(1));
    append_json_string(row, stmt->column_text(2));
    row += ",\"recorded_at\":";
    append_json_string(row, stmt->column_text(3));
    row += "}\n";
    if (row.size() > max_bytes - used) {
      result.truncated = true;
      result.omitted   = 1;
      continue;
    }
    used += row.size();
    newest_first.push_back(std::move(row));
  }

  result.rows = newest_first.size();
  result.jsonl.reserve(used);
  for (auto it = newest_first.rbegin(); it != newest_first.rend(); ++it) {
    result.jsonl += *it;
  }
  return result;
}

auto render_text(const bundle& b) -> std::string {
  std::string out = "=== planar diagnostic report ===\n";
  out += std::format("version:        {}\n", b.version);
  out += std::format("schema_version: {}\n", b.schema_version);
  out += std::format("health:         {}\n", b.health);
  out += std::format("window:         {} days\n\n", b.window_days);

  if (!b.logging_enabled) {
    out += "[invocations]   logging disabled\n";
  } else if (b.invocations.empty()) {
    out += "[invocations]   none in window\n";
  } else {
    out += "[invocations]\n";
    for (auto const& v : b.invocations) {
      out += std::format("  {}: total={} ok={} fail={}\n", v.verb_path, v.count, v.success_count, v.failure_count);
    }
  }
  out += "\n";

  if (!b.logging_enabled) {
    out += "[failures]      logging disabled\n";
  } else if (b.failures.empty()) {
    out += "[failures]      none in window\n";
  } else {
    out += "[failures]\n";
    for (auto const& f : b.failures) {
      out += std::format("  {}: {}\n", f.category, f.count);
    }
  }
  out += "\n";

  if (b.actions.empty()) {
    out += "[actions]       none in window\n";
  } else {
    out += "[actions]\n";
    for (auto const& a : b.actions) {
      out += std::format("  {}/{}: {}\n", a.action_kind, a.outcome, a.count);
    }
  }
  out += "\n";

  if (b.sync.empty()) {
    out += "[sync]          none in window\n";
  } else {
    out += "[sync]\n";
    for (auto const& s : b.sync) {
      out += std::format("  {}: {}\n", s.outcome, s.count);
    }
  }
  out += "\n";

  out += std::format("[claims]        stale={} never_consumed={}\n", b.claims.stale_claims, b.claims.never_consumed);

  if (b.claim_failure_categories.empty()) {
    out += "[claim failure categories] none in window\n";
  } else {
    out += "[claim failure categories]\n";
    for (auto const& row : b.claim_failure_categories) {
      out += std::format("  {}/{}: {}\n", row.provider, row.category, row.count);
    }
  }

  out += std::format("[handoffs]      stale={} never_consumed={}\n", b.handoffs.stale_handoffs, b.handoffs.never_consumed);
  out += std::format("[reopens]       {}\n\n", b.reopens);

  out += preview_text_block(b.preview);
  out += "\n";

  if (!b.logging_enabled) {
    out += "[failure tail]  logging disabled\n";
  } else if (b.failure_tail.empty()) {
    out += "[failure tail]  empty\n";
  } else {
    out += "[failure tail]\n";
    for (auto const& r : b.failure_tail) {
      out += std::format("  {}  cat={}  exit={}  at={}\n", r.verb_path, r.error_category, r.exit_code, r.recorded_at);
    }
  }
  return out;
}

auto render_json(const bundle& b) -> std::string {
  std::string out = "{";
  out += "\"version\":";
  append_json_string(out, b.version);
  out += std::format(",\"schema_version\":{}", b.schema_version);
  out += ",\"health\":";
  append_json_string(out, b.health);
  out += std::format(",\"window\":{}", b.window_days);
  out += std::format(",\"logging_enabled\":{}", b.logging_enabled ? "true" : "false");

  out += ",\"invocations\":[";
  if (b.logging_enabled) {
    for (std::size_t i = 0; i < b.invocations.size(); ++i) {
      auto const& v = b.invocations[i];
      if (i > 0) {
        out += ",";
      }
      out += "{\"verb_path\":";
      append_json_string(out, v.verb_path);
      out += std::format(",\"count\":{},\"success_count\":{},\"failure_count\":{}}}", v.count, v.success_count, v.failure_count);
    }
  }
  out += "]";

  out += ",\"failures\":[";
  if (b.logging_enabled) {
    for (std::size_t i = 0; i < b.failures.size(); ++i) {
      auto const& f = b.failures[i];
      if (i > 0) {
        out += ",";
      }
      out += "{\"category\":";
      append_json_string(out, f.category);
      out += std::format(",\"count\":{}}}", f.count);
    }
  }
  out += "]";

  out += ",\"failure_tail\":[";
  if (b.logging_enabled) {
    for (std::size_t i = 0; i < b.failure_tail.size(); ++i) {
      auto const& r = b.failure_tail[i];
      if (i > 0) {
        out += ",";
      }
      out += "{\"verb_path\":";
      append_json_string(out, r.verb_path);
      out += ",\"error_category\":";
      append_json_string(out, r.error_category);
      out += std::format(",\"exit_code\":{},\"recorded_at\":", r.exit_code);
      append_json_string(out, r.recorded_at);
      out += "}";
    }
  }
  out += "]";

  out += ",\"actions\":[";
  for (std::size_t i = 0; i < b.actions.size(); ++i) {
    auto const& a = b.actions[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"action_kind\":";
    append_json_string(out, a.action_kind);
    out += ",\"outcome\":";
    append_json_string(out, a.outcome);
    out += std::format(",\"count\":{}}}", a.count);
  }
  out += "]";

  out += ",\"sync\":[";
  for (std::size_t i = 0; i < b.sync.size(); ++i) {
    auto const& s = b.sync[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"outcome\":";
    append_json_string(out, s.outcome);
    out += std::format(",\"count\":{}}}", s.count);
  }
  out += "]";

  out += std::format(",\"claims\":{{\"stale_claims\":{},\"never_consumed\":{}}}", b.claims.stale_claims, b.claims.never_consumed);

  out += ",\"claim_failure_categories\":[";
  for (std::size_t i = 0; i < b.claim_failure_categories.size(); ++i) {
    auto const& row = b.claim_failure_categories[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"provider\":";
    append_json_string(out, row.provider);
    out += ",\"category\":";
    append_json_string(out, row.category);
    out += std::format(",\"count\":{}}}", row.count);
  }
  out += "]";

  out += std::format(",\"handoffs\":{{\"stale_handoffs\":{},\"never_consumed\":{}}}", b.handoffs.stale_handoffs,
                     b.handoffs.never_consumed);
  out += std::format(",\"reopens\":{}", b.reopens);

  out += preview_json_block(b.preview);

  out += "}\n";
  return out;
}

} // namespace planar::engine::introspect
