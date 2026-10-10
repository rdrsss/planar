/// @file diagnose.cpp
/// @brief Implementation of `planar.engine.diagnose` (see diagnose.cppm).

module;

module planar.engine.diagnose;

import std;
import planar.core.check;
import planar.db;
import planar.incident_model;
import planar.json_text;

namespace planar::engine::diagnose {

namespace {

namespace im = planar::incident_model;

/// A database failure, a bad-input error or a structural reason that ends a run early.
using stop = std::variant<run_error, db::db_error, unavailable_reason>;

template <typename T> using result = std::expected<T, stop>;

auto from_db(const db::db_error& err) -> stop {
  return stop{err};
}

auto reason_of(const db::db_error& err) -> unavailable_reason {
  return db::is_busy(err) ? unavailable_reason::busy : unavailable_reason::query_failed;
}

/// Restores the connection's busy timeout when the run ends, however it ends.
class busy_timeout_guard {
private:
  db::connection& _conn;
  int             _previous;
  std::int64_t    _previous_query_only;

public:
  busy_timeout_guard(db::connection& conn, int previous, std::int64_t previous_query_only)
      : _conn(conn), _previous(previous), _previous_query_only(previous_query_only) {
    _conn.set_busy_timeout(k_busy_timeout_ms);
    // The run issues only reads; let SQLite refuse anything else on a read-write connection.
    (void)_conn.execute("pragma query_only = 1");
  }
  busy_timeout_guard(const busy_timeout_guard&)            = delete;
  busy_timeout_guard& operator=(const busy_timeout_guard&) = delete;
  ~busy_timeout_guard() {
    _conn.set_busy_timeout(_previous);
    (void)_conn.execute(_previous_query_only != 0 ? "pragma query_only = 1" : "pragma query_only = 0");
  }
};

auto read_pragma(db::connection& conn, std::string_view sql) -> std::expected<std::int64_t, db::db_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(step.error());
  }
  return step.value() == db::step_result::row ? stmt->column_int64(0) : 0;
}

auto is_digits(std::string_view text, std::size_t from, std::size_t count) -> bool {
  if (from + count > text.size()) {
    return false;
  }
  return std::ranges::all_of(text.substr(from, count), [](char c) { return c >= '0' && c <= '9'; });
}

/// `YYYY-MM-DDTHH:MM:SSZ` or `YYYY-MM-DDTHH:MM:SS.fffZ`.
auto valid_instant_shape(std::string_view t) -> bool {
  if (t.size() != 20 && t.size() != 24) {
    return false;
  }
  if (!is_digits(t, 0, 4) || t[4] != '-' || !is_digits(t, 5, 2) || t[7] != '-' || !is_digits(t, 8, 2) || t[10] != 'T' ||
      !is_digits(t, 11, 2) || t[13] != ':' || !is_digits(t, 14, 2) || t[16] != ':' || !is_digits(t, 17, 2) || t.back() != 'Z') {
    return false;
  }
  return t.size() == 20 || (t[19] == '.' && is_digits(t, 20, 3));
}

auto leap_year(int y) -> bool {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/// The instant with millisecond precision, or nullopt when its shape or calendar value is invalid.
auto canonical_instant(std::string_view t) -> std::optional<std::string> {
  if (!valid_instant_shape(t)) {
    return std::nullopt;
  }
  auto num = [&](std::size_t at, std::size_t n) {
    int v = 0;
    std::from_chars(t.data() + at, t.data() + at + n, v);
    return v;
  };
  int  year = num(0, 4), month = num(5, 2), day = num(8, 2), hour = num(11, 2), minute = num(14, 2), second = num(17, 2);
  auto days = std::to_array<int>({31, leap_year(year) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31});
  if (month < 1 || month > 12 || day < 1 || day > days[static_cast<std::size_t>(month - 1)] || hour > 23 || minute > 59 ||
      second > 59) {
    return std::nullopt;
  }
  return t.size() == 20 ? std::string{t.substr(0, 19)} + ".000Z" : std::string{t};
}

auto unknown_plan_error(std::int64_t id) -> run_error {
  return run_error{.code = run_error_code::unknown_plan, .message = std::format("no plan with id {}", id)};
}

/// One text cell, or nullopt for SQL NULL, from a one-row single-column query with bound text.
auto scalar_text(db::connection& conn, std::string_view sql, std::span<const std::string_view> binds)
    -> result<std::optional<std::string>> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(from_db(stmt.error()));
  }
  int index = 1;
  for (auto bind : binds) {
    if (auto ok = stmt->bind_text(index++, bind); !ok) {
      return std::unexpected(from_db(ok.error()));
    }
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(from_db(step.error()));
  }
  if (step.value() == db::step_result::done || stmt->is_null(0)) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{stmt->column_text(0)};
}

auto resolve_plan_scope(db::connection& conn, std::int64_t plan_id) -> result<plan_scope> {
  auto stmt = conn.prepare("with recursive d(id) as (select id from plans where id = ?1 "
                           "union select p.id from plans p join d on p.parent_plan_id = d.id) "
                           "select id from d order by id");
  if (!stmt) {
    return std::unexpected(from_db(stmt.error()));
  }
  if (auto ok = stmt->bind_int64(1, plan_id); !ok) {
    return std::unexpected(from_db(ok.error()));
  }
  plan_scope scope{.plan_id = plan_id, .plan_ids = {}};
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(from_db(step.error()));
    }
    if (step.value() == db::step_result::done) {
      break;
    }
    scope.plan_ids.push_back(stmt->column_int64(0));
  }
  if (scope.plan_ids.empty()) {
    return std::unexpected(stop{unknown_plan_error(plan_id)});
  }
  return scope;
}

auto find_check(const catalog& cat, std::string_view id) -> const check_def* {
  auto it = std::ranges::find(cat.checks, id, &check_def::id);
  return it == cat.checks.end() ? nullptr : &*it;
}

auto find_input(const catalog& cat, std::string_view name) -> const input_def* {
  auto it = std::ranges::find(cat.inputs, name, &input_def::name);
  return it == cat.inputs.end() ? nullptr : &*it;
}

/// True when the input could not be read and the outcome must not read clean. An input the operator turned off
/// (`disabled`) is a choice, not a failure, and does not degrade the outcome.
auto degrades_outcome(const im::coverage_row& row) -> bool {
  return row.state == im::coverage_state::unavailable;
}

/// True when the text rendering names the input: one that was not read, whether or not it degrades the outcome.
auto is_unread(const im::coverage_row& row) -> bool {
  return row.state == im::coverage_state::unavailable || row.state == im::coverage_state::disabled;
}

/// Everything after the transaction opens. Fills `d` as it goes.
auto evaluate(db::connection& conn, const run_request& request, const catalog& cat, const std::vector<bool>& selected,
              diagnosis& d) -> result<void> {
  auto tx = conn.begin_transaction(db::lock_mode::deferred);
  if (!tx) {
    return std::unexpected(from_db(tx.error()));
  }

  std::array<std::string_view, 0> no_binds{};
  auto                            tables =
      scalar_text(conn, "select count(*) from sqlite_master where type = 'table' and name in ('plans', 'tasks')", no_binds);
  if (!tables) {
    return std::unexpected(tables.error());
  }
  if (tables->value_or("0") != "2") {
    return std::unexpected(stop{unavailable_reason::schema_unsupported});
  }

  // The canonical instant, and the window start for a day-count window. SQLite validates the date.
  std::string offset    = std::format("-{} days", request.days.value_or(k_default_days));
  auto        binds     = std::to_array<std::string_view>({request.evaluated_at, offset});
  auto        from_days = scalar_text(conn, "select strftime('%Y-%m-%dT%H:%M:%fZ', ?1, ?2)", binds);
  if (!from_days) {
    return std::unexpected(from_days.error());
  }
  if (!from_days->has_value()) {
    return std::unexpected(stop{run_error{.code    = run_error_code::invalid_instant,
                                          .message = std::format("invalid evaluation instant '{}'", request.evaluated_at)}});
  }
  d.evaluated_at = request.evaluated_at;

  if (request.plan_id) {
    auto scope = resolve_plan_scope(conn, *request.plan_id);
    if (!scope) {
      return std::unexpected(scope.error());
    }
    d.scope = std::move(*scope);
  }

  d.window.to = d.evaluated_at;
  if (request.plan_id && !request.days) {
    auto id_text = std::to_string(*request.plan_id);
    auto id_bind = std::to_array<std::string_view>({id_text});
    auto created = scalar_text(conn, "select created_at from plans where id = ?1", id_bind);
    if (!created) {
      return std::unexpected(created.error());
    }
    check(created->has_value(), "the resolved plan has a created_at");
    d.window.from   = **created;
    d.window.source = window_source::plan_lifetime;
  } else {
    d.window.from   = **from_days;
    d.window.source = request.days ? window_source::days : window_source::default_days;
    d.window.days   = request.days.value_or(k_default_days);
  }

  check_context ctx{.conn                 = conn,
                    .scope                = d.scope,
                    .window               = d.window,
                    .evaluated_at         = d.evaluated_at,
                    .cli_log_enabled      = request.cli_log_enabled,
                    .verb_path_recognized = request.verb_path_recognized};

  // Which inputs does a selected, built check need?
  for (const auto& input : cat.inputs) {
    bool needed_by_runnable = false;
    bool needed_by_unbuilt  = false;
    for (std::size_t i = 0; i < cat.checks.size(); ++i) {
      const auto& def = cat.checks[i];
      if (!std::ranges::contains(def.inputs, input.name)) {
        continue;
      }
      if (!def.built) {
        needed_by_unbuilt = true;
      } else if (selected[i]) {
        needed_by_runnable = true;
      }
    }
    im::coverage_row row{.input  = input.name,
                         .state  = im::coverage_state::not_applicable,
                         .reason = (needed_by_unbuilt || !input.built) ? "check-not-built" : "not-selected"};
    if (needed_by_runnable) {
      check(static_cast<bool>(input.probe), "an input a runnable check needs has a probe");
      auto probed = input.probe(ctx);
      if (!probed) {
        return std::unexpected(from_db(probed.error()));
      }
      row = im::coverage_row{.input = input.name, .state = probed->state, .reason = probed->reason};
    }
    d.coverage.push_back(std::move(row));
  }

  auto row_of = [&](std::string_view name) -> const im::coverage_row& {
    auto it = std::ranges::find(d.coverage, name, &im::coverage_row::input);
    check(it != d.coverage.end(), "every input a check names is in the catalog");
    return *it;
  };

  d.result = run_outcome::ok;
  for (std::size_t i = 0; i < cat.checks.size(); ++i) {
    const auto&   def = cat.checks[i];
    check_summary summary{.id            = def.id,
                          .kind          = def.kind,
                          .severity      = def.severity,
                          .category      = def.category,
                          .state         = check_state::ran,
                          .finding_count = 0};
    if (!def.built) {
      summary.state = check_state::not_built;
    } else if (!selected[i]) {
      summary.state = check_state::not_selected;
    } else {
      for (const auto& name : def.inputs) {
        const auto& row = row_of(name);
        if (degrades_outcome(row)) {
          d.result = run_outcome::partial;
        }
        if (row.state != im::coverage_state::observed) {
          summary.state = check_state::input_unavailable;
        }
      }
      if (summary.state == check_state::ran) {
        check(static_cast<bool>(def.evaluate), "a built check has an evaluate function");
        auto found = def.evaluate(ctx);
        if (!found) {
          return std::unexpected(from_db(found.error()));
        }
        for (auto& f : *found) {
          f.check_id = def.id;
          if (!std::ranges::contains(f.evidence, f.primary)) {
            f.evidence.push_back(f.primary);
          }
          if (f.recovery.empty()) {
            f.recovery = def.recovery;
          }
          d.findings.push_back(std::move(f));
          ++summary.finding_count;
        }
      }
    }
    d.checks.push_back(std::move(summary));
  }
  im::sort_findings(d.findings);
  // A read-only transaction: a failed commit loses nothing, and the destructor rolls back.
  (void)tx->commit();
  return {};
}

auto unavailable_diagnosis(unavailable_reason reason, std::string_view evaluated_at) -> diagnosis {
  diagnosis d;
  d.evaluated_at = std::string{evaluated_at};
  d.result       = run_outcome::unavailable;
  d.reason       = reason;
  return d;
}

void append_string(std::string& out, std::string_view text) {
  json_text::append_json_string(out, text);
}

/// One finding as a text line: `<severity> <check-id> <entity> -> <recovery>`, newline included.
auto finding_line(const im::finding& f) -> std::string {
  // A cluster names its grouping key and size: its primary entity alone says neither.
  auto cluster =
      !f.members.empty() ? std::format(" ({}, {} members)", im::finding_fingerprint(f), f.members.size()) : std::string{};
  return std::format("{} {} {}{} -> {}\n", im::diagnostic_severity_name(f.severity), f.check_id, im::entity_ref_text(f.primary),
                     cluster, f.recovery.empty() ? std::string_view{"none"} : std::string_view{f.recovery});
}

/// One finding as its JSON object, appended to `out`.
void append_finding_json(std::string& out, const im::finding& f) {
  out += "{\"check\":";
  append_string(out, f.check_id);
  out += ",\"severity\":";
  append_string(out, im::diagnostic_severity_name(f.severity));
  out += ",\"entity\":";
  append_string(out, im::entity_ref_text(f.primary));
  out += ",\"evidence\":[";
  for (std::size_t j = 0; j < f.evidence.size(); ++j) {
    out += j == 0 ? "" : ",";
    append_string(out, im::entity_ref_text(f.evidence[j]));
  }
  out += "],\"evidence_times\":[";
  for (std::size_t j = 0; j < f.evidence_times.size(); ++j) {
    out += j == 0 ? "" : ",";
    append_string(out, f.evidence_times[j]);
  }
  out += "],\"fingerprint\":";
  append_string(out, im::finding_fingerprint(f));
  out += ",\"recovery\":";
  if (f.recovery.empty()) {
    out += "null";
  } else {
    append_string(out, f.recovery);
  }
  out += ",\"incident\":null}";
}

} // namespace

namespace detail {

auto query_findings(const check_context& ctx, std::string_view sql, std::string_view first, std::string_view second,
                    const finding_reader& read) -> std::expected<std::vector<incident_model::finding>, db::db_error> {
  auto stmt = ctx.conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (!first.empty()) {
    if (auto ok = stmt->bind_text(1, first); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (!second.empty()) {
    if (auto ok = stmt->bind_text(2, second); !ok) {
      return std::unexpected(ok.error());
    }
  }
  std::vector<im::finding> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (step.value() == db::step_result::done) {
      return out;
    }
    out.push_back(read(*stmt));
  }
}

} // namespace detail

auto plan_filter_sql(const plan_scope& scope, std::string_view column) -> std::string {
  if (!scope.plan_id) {
    return "1 = 1";
  }
  std::string out{column};
  out += " in (";
  bool first = true;
  for (auto id : scope.plan_ids) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += std::to_string(id);
  }
  out += ")";
  return out;
}

auto builtin_catalog() -> catalog {
  catalog cat;
  for (auto make :
       {detail::claims_family, detail::dispatch_family, detail::cli_family, detail::records_family, detail::queue_family}) {
    auto fam = make();
    std::ranges::move(fam.inputs, std::back_inserter(cat.inputs));
    std::ranges::move(fam.checks, std::back_inserter(cat.checks));
  }
  return cat;
}

auto window_source_name(window_source s) noexcept -> std::string_view {
  switch (s) {
  case window_source::plan_lifetime:
    return "plan-lifetime";
  case window_source::days:
    return "days";
  case window_source::default_days:
    return "default-days";
  }
  return "default-days";
}

auto run_outcome_name(run_outcome o) noexcept -> std::string_view {
  switch (o) {
  case run_outcome::ok:
    return "ok";
  case run_outcome::partial:
    return "partial";
  case run_outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

auto unavailable_reason_name(unavailable_reason r) noexcept -> std::string_view {
  switch (r) {
  case unavailable_reason::busy:
    return "busy";
  case unavailable_reason::query_failed:
    return "query-failed";
  case unavailable_reason::schema_unsupported:
    return "schema-unsupported";
  }
  return "query-failed";
}

auto check_state_name(check_state s) noexcept -> std::string_view {
  switch (s) {
  case check_state::ran:
    return "ran";
  case check_state::not_selected:
    return "not-selected";
  case check_state::not_built:
    return "not-built";
  case check_state::input_unavailable:
    return "input-unavailable";
  }
  return "ran";
}

auto run(db::connection& conn, const run_request& request) -> std::expected<diagnosis, run_error> {
  return run(conn, request, builtin_catalog());
}

auto run(db::connection& conn, const run_request& request, const catalog& cat) -> std::expected<diagnosis, run_error> {
  auto instant = canonical_instant(request.evaluated_at);
  if (!instant) {
    return std::unexpected(run_error{.code    = run_error_code::invalid_instant,
                                     .message = std::format("invalid evaluation instant '{}'", request.evaluated_at)});
  }
  run_request normalised  = request;
  normalised.evaluated_at = *instant;
  if (request.days && *request.days < 1) {
    return std::unexpected(run_error{.code    = run_error_code::invalid_days,
                                     .message = std::format("--days must be at least 1, got {}", *request.days)});
  }
  std::vector<bool> selected(cat.checks.size(), request.checks.empty());
  for (const auto& id : request.checks) {
    auto it = std::ranges::find(cat.checks, id, &check_def::id);
    if (it == cat.checks.end()) {
      return std::unexpected(run_error{.code = run_error_code::unknown_check, .message = std::format("unknown check '{}'", id)});
    }
    selected[static_cast<std::size_t>(it - cat.checks.begin())] = true;
  }
  for (const auto& def : cat.checks) {
    for (const auto& name : def.inputs) {
      check(find_input(cat, name) != nullptr, "every input a check names is declared in the catalog");
    }
  }

  auto previous = read_pragma(conn, "pragma busy_timeout");
  if (!previous) {
    return unavailable_diagnosis(reason_of(previous.error()), *instant);
  }
  auto previous_query_only = read_pragma(conn, "pragma query_only");
  if (!previous_query_only) {
    return unavailable_diagnosis(reason_of(previous_query_only.error()), *instant);
  }
  diagnosis d;
  {
    busy_timeout_guard guard{conn, static_cast<int>(*previous), *previous_query_only};
    auto               done = evaluate(conn, normalised, cat, selected, d);
    if (!done) {
      const auto& why = done.error();
      if (const auto* bad = std::get_if<run_error>(&why)) {
        return std::unexpected(*bad);
      }
      auto reason = std::holds_alternative<unavailable_reason>(why) ? std::get<unavailable_reason>(why)
                                                                    : reason_of(std::get<db::db_error>(why));
      return unavailable_diagnosis(reason, *instant);
    }
  }
  return d;
}

auto render_text(const diagnosis& d) -> std::string {
  std::string out;
  if (d.result == run_outcome::unavailable) {
    out += std::format("diagnose: unavailable ({})\n", d.reason ? unavailable_reason_name(*d.reason) : "query-failed");
    return out;
  }
  std::string scope =
      d.scope.plan_id ? std::format("plan {} ({} plan(s))", *d.scope.plan_id, d.scope.plan_ids.size()) : std::string{"all plans"};
  out += std::format("diagnose: {}, window {} to {} ({}), outcome {}\n", scope, d.window.from, d.window.to,
                     window_source_name(d.window.source), run_outcome_name(d.result));
  for (const auto& row : d.coverage) {
    if (is_unread(row)) {
      out += std::format("input {}: {}{}\n", row.input, im::coverage_state_name(row.state),
                         row.reason.empty() ? std::string{} : std::format(" ({})", row.reason));
    }
  }
  for (const auto& f : d.findings) {
    out += finding_line(f);
  }
  return out;
}

auto render_json(const diagnosis& d) -> std::string {
  std::string out = "{\"schema\":";
  append_string(out, k_json_schema);
  out += std::format(",\"catalog_version\":{},\"evaluated_at\":", d.catalog_version);
  append_string(out, d.evaluated_at);
  out += ",\"scope\":{\"plan_id\":";
  out += d.scope.plan_id ? std::to_string(*d.scope.plan_id) : std::string{"null"};
  out += ",\"plan_ids\":[";
  for (std::size_t i = 0; i < d.scope.plan_ids.size(); ++i) {
    out += i == 0 ? "" : ",";
    out += std::to_string(d.scope.plan_ids[i]);
  }
  out += "],\"window\":{\"from\":";
  append_string(out, d.window.from);
  out += ",\"to\":";
  append_string(out, d.window.to);
  out += ",\"source\":";
  append_string(out, window_source_name(d.window.source));
  out += ",\"days\":";
  out += d.window.days ? std::to_string(*d.window.days) : std::string{"null"};
  out += "}},\"outcome\":";
  append_string(out, run_outcome_name(d.result));
  out += ",\"reason\":";
  if (d.reason) {
    append_string(out, unavailable_reason_name(*d.reason));
  } else {
    out += "null";
  }
  out += ",\"coverage\":[";
  for (std::size_t i = 0; i < d.coverage.size(); ++i) {
    const auto& row = d.coverage[i];
    out += i == 0 ? "{\"input\":" : ",{\"input\":";
    append_string(out, row.input);
    out += ",\"state\":";
    append_string(out, im::coverage_state_name(row.state));
    out += ",\"reason\":";
    if (row.reason.empty()) {
      out += "null";
    } else {
      append_string(out, row.reason);
    }
    out += "}";
  }
  out += "],\"checks\":[";
  for (std::size_t i = 0; i < d.checks.size(); ++i) {
    const auto& c = d.checks[i];
    out += i == 0 ? "{\"id\":" : ",{\"id\":";
    append_string(out, c.id);
    out += ",\"kind\":";
    append_string(out, im::check_kind_name(c.kind));
    out += ",\"severity\":";
    append_string(out, im::diagnostic_severity_name(c.severity));
    out += ",\"category\":";
    append_string(out, c.category);
    out += ",\"state\":";
    append_string(out, check_state_name(c.state));
    out += std::format(",\"findings\":{}}}", c.finding_count);
  }
  out += "],\"findings\":[";
  for (std::size_t i = 0; i < d.findings.size(); ++i) {
    out += i == 0 ? "" : ",";
    append_finding_json(out, d.findings[i]);
  }
  out += "],\"would_resolve\":[]}";
  return out;
}

auto render_section_text(const diagnosis& d) -> std::string {
  if (d.result == run_outcome::unavailable) {
    return std::format("diagnose: unavailable ({})\n", d.reason ? unavailable_reason_name(*d.reason) : "query-failed");
  }
  std::string out =
      d.findings.empty() ? std::string{"diagnose: clean"} : std::format("diagnose: {} finding(s)", d.findings.size());
  if (d.result == run_outcome::partial) {
    std::string sources;
    for (const auto& row : d.coverage) {
      if (row.state == im::coverage_state::unavailable) {
        sources += sources.empty() ? "" : ", ";
        sources += row.input;
      }
    }
    out += std::format(" (partial: {} unavailable)", sources);
  }
  out += '\n';
  for (const auto& f : d.findings) {
    out += finding_line(f);
  }
  return out;
}

auto render_section_json(const diagnosis& d, std::int64_t plan_id) -> std::string {
  const char* state = d.result == run_outcome::unavailable ? "unavailable" : d.findings.empty() ? "clean" : "findings";
  std::string out   = std::format("{{\"plan_id\":{},\"state\":\"{}\",\"outcome\":", plan_id, state);
  append_string(out, run_outcome_name(d.result));
  out += ",\"reason\":";
  if (d.reason) {
    append_string(out, unavailable_reason_name(*d.reason));
  } else {
    out += "null";
  }
  out += ",\"evaluated_at\":";
  append_string(out, d.evaluated_at);
  out += ",\"findings\":[";
  for (std::size_t i = 0; i < d.findings.size(); ++i) {
    out += i == 0 ? "" : ",";
    append_finding_json(out, d.findings[i]);
  }
  out += "],\"incidents\":{\"state\":\"not_applicable\",\"reason\":null}}";
  return out;
}

auto run_section(db::connection& conn, std::int64_t plan_id, std::string_view evaluated_at, std::optional<bool> cli_log_enabled)
    -> section {
  return run_section(conn, plan_id, evaluated_at, cli_log_enabled, builtin_catalog());
}

auto run_section(db::connection& conn, std::int64_t plan_id, std::string_view evaluated_at, std::optional<bool> cli_log_enabled,
                 const catalog& cat) -> section {
  run_request request{.plan_id = plan_id, .evaluated_at = std::string{evaluated_at}, .cli_log_enabled = cli_log_enabled};
  auto        ran = run(conn, request, cat);
  diagnosis   d;
  if (ran) {
    d = std::move(*ran);
  } else {
    // Bad input cannot be produced by the callers (the plan exists and the instant is theirs),
    // so a rejected run reads as a failed one rather than as the caller's mistake.
    d = unavailable_diagnosis(unavailable_reason::query_failed, evaluated_at);
  }
  return section{.text = render_section_text(d), .json = render_section_json(d, plan_id)};
}

} // namespace planar::engine::diagnose
