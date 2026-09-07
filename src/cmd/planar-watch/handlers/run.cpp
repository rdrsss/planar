/// @file run.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.run`.

module planar.cmd.planar_watch.handlers.run;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.runtime.contextrecords;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.format;

namespace planar::cmd::watch::handlers {

namespace cr = engine::runtime::contextrecords;

using json_text::append_json_string;

namespace {

/// @brief One run row, from either `workflow_runs` ("wf") or `runs` ("op").
///
/// Mirrors zig's `RunRow`: an op-source row maps `arm` -> `workflow_name`,
/// `run_uid` -> `run_identifier`, and sets `pid = 0` / `repo_root = ""`
/// (sentinels — neither column exists on `runs`).
struct run_row {
  std::int64_t               id      = 0;
  std::int64_t               plan_id = 0;
  std::string                workflow_name;
  std::string                run_identifier;
  std::int64_t               pid = 0;
  std::string                repo_root;
  std::string                started_at;
  std::optional<std::string> ended_at;
  std::string                status;
  std::string_view           source; ///< `"wf"` or `"op"`.
};

/// @brief Build the `where` clause `list_workflow_runs` / `list_op_runs`
/// share: zero, one, or both of `plan_id = ?` / `status = ?`, in that bind
/// order.
/// @param plan_filter Optional `--plan` value.
/// @param status_filter Optional `--status` value.
/// @return The clause, empty when neither filter is set.
auto where_clause(std::optional<std::int64_t> plan_filter, std::optional<std::string> status_filter) -> std::string {
  if (plan_filter.has_value() && status_filter.has_value()) {
    return " where plan_id = ? and status = ?";
  }
  if (plan_filter.has_value()) {
    return " where plan_id = ?";
  }
  if (status_filter.has_value()) {
    return " where status = ?";
  }
  return "";
}

/// @brief Bind whichever of `plan_filter` / `status_filter` are set, in the
/// order `where_clause` declared them.
/// @param stmt The prepared statement.
/// @param plan_filter Optional `--plan` value.
/// @param status_filter Optional `--status` value.
/// @return Success, or the first bind failure.
auto bind_filters(db::statement& stmt, std::optional<std::int64_t> plan_filter, std::optional<std::string> status_filter)
    -> std::expected<void, db::db_error> {
  int index = 1;
  if (plan_filter.has_value()) {
    if (auto r = stmt.bind_int64(index++, *plan_filter); !r) {
      return std::unexpected(r.error());
    }
  }
  if (status_filter.has_value()) {
    if (auto r = stmt.bind_text(index++, *status_filter); !r) {
      return std::unexpected(r.error());
    }
  }
  return {};
}

/// @brief Query `workflow_runs` (the "wf" source).
/// @param conn The connection.
/// @param plan_filter Optional `--plan` value.
/// @param status_filter Optional `--status` value.
/// @return Rows ordered `started_at desc, id desc`, or the query failure.
auto list_workflow_runs(db::connection& conn, std::optional<std::int64_t> plan_filter, std::optional<std::string> status_filter)
    -> std::expected<std::vector<run_row>, db::db_error> {
  auto const sql  = std::format("select id, plan_id, workflow_name, run_identifier, pid, repo_root, started_at, ended_at, status "
                                "from workflow_runs{} order by started_at desc, id desc",
                                where_clause(plan_filter, status_filter));
  auto       stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto r = bind_filters(*stmt, plan_filter, status_filter); !r) {
    return std::unexpected(r.error());
  }
  std::vector<run_row> out;
  while (true) {
    auto const step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(run_row{.id             = stmt->column_int64(0),
                          .plan_id        = stmt->column_int64(1),
                          .workflow_name  = stmt->column_text(2),
                          .run_identifier = stmt->column_text(3),
                          .pid            = stmt->column_int64(4),
                          .repo_root      = stmt->column_text(5),
                          .started_at     = stmt->column_text(6),
                          .ended_at       = stmt->is_null(7) ? std::nullopt : std::optional{stmt->column_text(7)},
                          .status         = stmt->column_text(8),
                          .source         = "wf"});
  }
  return out;
}

/// @brief Query the `runs` table (the "op" / bench-arm source).
///
/// Column mapping onto `run_row`: `arm` -> `workflow_name`, `run_uid` ->
/// `run_identifier`, `pid = 0`, `repo_root = ""` (sentinels; the `runs`
/// schema has neither column).
/// @param conn The connection.
/// @param plan_filter Optional `--plan` value.
/// @param status_filter Optional `--status` value.
/// @return Rows ordered `started_at desc, id desc`, or the query failure.
auto list_op_runs(db::connection& conn, std::optional<std::int64_t> plan_filter, std::optional<std::string> status_filter)
    -> std::expected<std::vector<run_row>, db::db_error> {
  auto const sql  = std::format("select id, plan_id, arm, run_uid, started_at, ended_at, status from runs{} "
                                "order by started_at desc, id desc",
                                where_clause(plan_filter, status_filter));
  auto       stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto r = bind_filters(*stmt, plan_filter, status_filter); !r) {
    return std::unexpected(r.error());
  }
  std::vector<run_row> out;
  while (true) {
    auto const step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(run_row{.id             = stmt->column_int64(0),
                          .plan_id        = stmt->column_int64(1),
                          .workflow_name  = stmt->column_text(2),
                          .run_identifier = stmt->column_text(3),
                          .pid            = 0,
                          .repo_root      = "",
                          .started_at     = stmt->column_text(4),
                          .ended_at       = stmt->is_null(5) ? std::nullopt : std::optional{stmt->column_text(5)},
                          .status         = stmt->column_text(6),
                          .source         = "op"});
  }
  return out;
}

/// @brief Read one `workflow_runs` row by id.
/// @param conn The connection.
/// @param run_id The row id.
/// @return The row, `std::nullopt` when absent, or the query failure.
auto fetch_workflow_run(db::connection& conn, std::int64_t run_id) -> std::expected<std::optional<run_row>, db::db_error> {
  auto stmt = conn.prepare(
      "select id, plan_id, workflow_name, run_identifier, pid, repo_root, started_at, ended_at, status from workflow_runs "
      "where id = ?");
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto r = stmt->bind_int64(1, run_id); !r) {
    return std::unexpected(r.error());
  }
  auto const step = stmt->step();
  if (!step) {
    return std::unexpected(step.error());
  }
  if (*step == db::step_result::done) {
    return std::optional<run_row>{};
  }
  return std::optional<run_row>{run_row{.id             = stmt->column_int64(0),
                                        .plan_id        = stmt->column_int64(1),
                                        .workflow_name  = stmt->column_text(2),
                                        .run_identifier = stmt->column_text(3),
                                        .pid            = stmt->column_int64(4),
                                        .repo_root      = stmt->column_text(5),
                                        .started_at     = stmt->column_text(6),
                                        .ended_at       = stmt->is_null(7) ? std::nullopt : std::optional{stmt->column_text(7)},
                                        .status         = stmt->column_text(8),
                                        .source         = "wf"}};
}

/// @brief Append one run as a JSON object, in the oracle's field order.
/// @param out The buffer.
/// @param row The run.
auto append_run(std::string& out, const run_row& row) -> void {
  out.append(std::format("{{\"id\":{}", row.id));
  out.append(std::format(",\"plan_id\":{}", row.plan_id));
  out.append(",\"workflow_name\":");
  append_json_string(out, row.workflow_name);
  out.append(",\"run_identifier\":");
  append_json_string(out, row.run_identifier);
  out.append(std::format(",\"pid\":{}", row.pid));
  out.append(",\"repo_root\":");
  append_json_string(out, row.repo_root);
  out.append(",\"started_at\":");
  append_json_string(out, row.started_at);
  out.append(",\"ended_at\":");
  if (row.ended_at.has_value()) {
    append_json_string(out, *row.ended_at);
  } else {
    out.append("null");
  }
  out.append(",\"status\":");
  append_json_string(out, row.status);
  out.append(",\"source\":");
  append_json_string(out, row.source);
  out.push_back('}');
}

/// @brief Append one context record as a JSON object, in the oracle's
/// field order.
/// @param out The buffer.
/// @param row The record.
auto append_context_record(std::string& out, const cr::record& row) -> void {
  out.append(std::format("{{\"id\":{}", row.id));
  out.append(std::format(",\"run_id\":{}", row.run_id));
  out.append(",\"stage\":");
  append_json_string(out, row.stage);
  out.append(std::format(",\"session_id\":{}", row.session_id));
  out.append(std::format(",\"claim_id\":{}", row.claim_id.value_or(0)));
  out.append(",\"kind\":");
  append_json_string(out, row.kind);
  out.append(",\"body\":");
  append_json_string(out, row.body);
  out.append(",\"status\":");
  append_json_string(out, row.status);
  out.append(",\"compiled_from\":");
  if (row.compiled_from.has_value()) {
    append_json_string(out, *row.compiled_from);
  } else {
    out.append("null");
  }
  out.append(",\"created_at\":");
  append_json_string(out, row.created_at);
  out.push_back('}');
}

} // namespace

auto run_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const arm = cliapp::flag_string(args, "--arm").value_or("all");
  if (arm != "wf" && arm != "op" && arm != "all") {
    // zig's `error.InvalidValue` is unmatched in `exit.zig`'s switch and
    // falls to the generic `else => 1` arm — NOT the 2 an `InvalidInput`
    // gets. Reproduced (D2): `generic_failure`, not `invalid_input`.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("run list: --arm must be wf, op, or all (got '{}')", arm)));
  }

  auto const plan_filter   = cliapp::flag_int(args, "--plan");
  auto const status_filter = cliapp::flag_string(args, "--status");

  std::vector<run_row> rows;
  if (arm == "wf" || arm == "all") {
    auto wf = list_workflow_runs(**conn, plan_filter, status_filter);
    if (!wf) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "run list: QueryFailed"));
    }
    rows.insert(rows.end(), std::make_move_iterator(wf->begin()), std::make_move_iterator(wf->end()));
  }
  if (arm == "op" || arm == "all") {
    auto op = list_op_runs(**conn, plan_filter, status_filter);
    if (!op) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "run list (op): QueryFailed"));
    }
    rows.insert(rows.end(), std::make_move_iterator(op->begin()), std::make_move_iterator(op->end()));
  }

  // Combined descending sort by started_at. `stable_sort` preserves the
  // wf-before-op insertion order on a tie, matching zig's insertion sort
  // (which is stable under its strict less-than comparison).
  std::stable_sort(rows.begin(), rows.end(), [](const run_row& a, const run_row& b) { return a.started_at > b.started_at; });

  bool const  json = cliapp::flag_bool(args, "--json");
  std::string out;
  if (json) {
    out.append("{\"generated_at\":");
    append_json_string(out, format::now_iso());
    out.append(",\"runs\":[");
    bool first = true;
    for (auto const& row : rows) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      append_run(out, row);
    }
    out.append("]}\n");
    ctx.out() << out;
    return {};
  }

  out.append(std::format("runs: {}\n", rows.size()));
  for (auto const& row : rows) {
    std::string_view const ended = row.ended_at.has_value() ? std::string_view{*row.ended_at} : std::string_view{"-"};
    out.append(std::format("  run:{}  plan:{}  status:{}  source:{}  started:{}  ended:{}  workflow:{}\n", row.id, row.plan_id,
                           row.status, row.source, row.started_at, ended, row.workflow_name));
  }
  ctx.out() << out;
  return {};
}

auto run_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const raw_id = cliapp::positional_string(args, "id");
  auto const run_id = raw_id.has_value() ? cliapp::parse_int64_zig(*raw_id) : std::nullopt;
  if (!run_id.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "run show: id must be an integer"));
  }

  auto found = fetch_workflow_run(**conn, *run_id);
  if (!found) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "run show: QueryFailed"));
  }
  if (!found->has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("run show: run {} not found", *run_id)));
  }
  auto const& run = **found;

  auto records = cr::list(**conn, *run_id, std::nullopt, std::nullopt, std::nullopt);
  if (!records) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "run show: context_records: QueryFailed"));
  }
  // The engine's own `list` orders by `id asc`; re-sort here by
  // `stage asc, created_at asc` (id asc as the tiebreak survives because
  // `stable_sort` preserves the input's relative order for equal keys).
  std::stable_sort(records->begin(), records->end(), [](const cr::record& a, const cr::record& b) {
    if (a.stage != b.stage) {
      return a.stage < b.stage;
    }
    return a.created_at < b.created_at;
  });

  bool const  json = cliapp::flag_bool(args, "--json");
  std::string out;
  if (json) {
    out.append("{\"run\":");
    append_run(out, run);
    out.append(",\"context_records\":[");
    bool first = true;
    for (auto const& record : *records) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      append_context_record(out, record);
    }
    out.append("]}\n");
    ctx.out() << out;
    return {};
  }

  std::string_view const ended = run.ended_at.has_value() ? std::string_view{*run.ended_at} : std::string_view{"-"};
  out.append(std::format("run:{}  plan:{}  status:{}  pid:{}  workflow:{}\n"
                         "  started:{}  ended:{}\n"
                         "  identifier:{}\n"
                         "  repo_root:{}\n",
                         run.id, run.plan_id, run.status, run.pid, run.workflow_name, run.started_at, ended, run.run_identifier,
                         run.repo_root));
  if (records->empty()) {
    out.append("  (no context records)\n");
    ctx.out() << out;
    return {};
  }
  out.append(std::format("context_records: {}\n", records->size()));
  std::optional<std::string> current_stage;
  for (auto const& record : *records) {
    if (!current_stage.has_value() || *current_stage != record.stage) {
      current_stage = record.stage;
      out.append(std::format("  [stage: {}]\n", record.stage));
    }
    constexpr std::size_t  preview_limit = 80;
    std::string_view const body_preview  = record.body.size() <= preview_limit
                                               ? std::string_view{record.body}
                                               : std::string_view{record.body}.substr(0, preview_limit);
    std::string_view const ellipsis      = record.body.size() > preview_limit ? "\xE2\x80\xA6" : "";
    out.append(std::format("    record:{}  kind:{}  status:{}  created:{}\n"
                           "      body: {}{}\n",
                           record.id, record.kind, record.status, record.created_at, body_preview, ellipsis));
    if (record.compiled_from.has_value()) {
      out.append(std::format("      compiled_from: {}\n", *record.compiled_from));
    }
  }
  ctx.out() << out;
  return {};
}

} // namespace planar::cmd::watch::handlers
