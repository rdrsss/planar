/// @file history.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.queue.history`.
/// See history.cppm for the contract.

module planar.cmd.planar_watch.handlers.queue.history;

import std;
import planar.cliapp.args;
import planar.cmd.planar_watch.agentstore;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.queue.render;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.process.identity;

namespace planar::cmd::watch::handlers {

namespace {

namespace hq    = engine::hostqueue;
namespace ident = process::identity;
namespace qcfg  = engine::config;

using namespace queue_render;

/// @brief A wall-clock reading as UTC `YYYY-MM-DDTHH:MM:SSZ`.
auto utc_text(std::int64_t ms) -> std::string {
  auto const seconds = std::chrono::sys_seconds{std::chrono::seconds{ms / 1000 - (ms % 1000 < 0 ? 1 : 0)}};
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds);
}

/// @brief `cancelled-by:<vendor>/<role>/<pid>`, each part escaped as a cell.
auto canceller_text(const hq::canceller& who) -> std::string {
  return std::format("cancelled-by:{}/{}/{}", field_cell(who.vendor.value_or("")), field_cell(who.role.value_or("")), who.pid);
}

auto result_text(const hq::history_row& r) -> std::string {
  if (r.exit_code) {
    return std::format("code:{}", *r.exit_code);
  }
  if (r.signal) {
    return std::format("signal:{}", *r.signal);
  }
  return "-";
}

auto notes_of(const hq::history_row& r) -> std::string {
  std::vector<std::string> notes;
  if (r.cancelled_by) {
    notes.push_back(canceller_text(*r.cancelled_by));
  }
  if (r.successor_seq) {
    notes.push_back(std::format("superseded-by:{}", *r.successor_seq));
  }
  if (r.parent_seq) {
    notes.push_back(std::format("nested:{}", *r.parent_seq));
  }
  if (notes.empty()) {
    return "-";
  }
  std::string joined;
  for (auto const& note : notes) {
    joined += joined.empty() ? "" : ",";
    joined += note;
  }
  return joined;
}

auto render_text(const std::vector<hq::history_row>& rows) -> std::string {
  if (rows.empty()) {
    return {};
  }
  std::vector<std::vector<std::string>> table;
  table.push_back(
      {"SEQ", "OUTCOME", "RESULT", "ENDED", "WAITED", "RAN", "NOTES", "VENDOR", "ROLE", "LABEL", "DIRECTORY", "COMMAND"});
  for (auto const& r : rows) {
    table.push_back({std::to_string(r.seq), std::string{hq::to_string(r.outcome)}, result_text(r), utc_text(r.ended_at),
                     duration_text(r.waited_ms), duration_text(r.ran_ms), notes_of(r), field_cell(r.vendor.value_or("")),
                     field_cell(r.role.value_or("")), field_cell(r.label.value_or("")), cell(r.cwd), shell_line(r.argv)});
  }
  return pad_table(table);
}

void put_canceller(std::string& out, const std::optional<hq::canceller>& who) {
  put_key(out, "cancelled_by");
  if (!who) {
    out += "null";
    return;
  }
  out += '{';
  put_text(out, "vendor", who->vendor);
  put_text(out, "role", who->role);
  put_int(out, "pid", who->pid);
  out += '}';
}

auto render_json(const std::vector<hq::history_row>& rows) -> std::string {
  std::string out = "[";
  for (auto const& r : rows) {
    out += out.back() == '[' ? "{" : ",{";
    put_int(out, "seq", r.seq);
    put_text(out, "outcome", std::string{hq::to_string(r.outcome)});
    put_int(out, "exit_code", r.exit_code);
    put_int(out, "signal", r.signal);
    put_canceller(out, r.cancelled_by);
    put_int(out, "superseded_by", r.successor_seq);
    put_key(out, "nested");
    out += r.nested ? "true" : "false";
    put_int(out, "parent_seq", r.parent_seq);
    put_key(out, "cwd");
    out += quote(r.cwd);
    put_argv(out, r.argv);
    put_text(out, "label", r.label);
    put_text(out, "vendor", r.vendor);
    put_text(out, "role", r.role);
    put_text(out, "log_path", r.log_path);
    put_int(out, "enqueued_at", r.enqueued_at);
    put_int(out, "started_at", r.started_at);
    put_int(out, "ended_at", r.ended_at);
    put_int(out, "waited_ms", r.waited_ms);
    put_int(out, "ran_ms", r.ran_ms);
    put_int(out, "run_limit_ms", r.run_limit_ms);
    put_int(out, "wait_limit_ms", r.wait_limit_ms);
    out += '}';
  }
  out += "]\n";
  return out;
}

} // namespace

auto since_cutoff_ms(std::int64_t now_ms, std::int64_t since_ms) -> std::int64_t {
  return now_ms - since_ms;
}

auto queue_history(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const as_json = cliapp::flag_bool(args, "--json");

  // Validate the value before opening anything: a bad --since is refused the
  // same way whether or not a store exists.
  std::optional<std::int64_t> since_ms;
  if (auto const text = cliapp::flag_string(args, "--since")) {
    auto const parsed = qcfg::parse_history_since(*text);
    if (!parsed) {
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, std::format("queue history: --since {}", parsed.error())));
    }
    since_ms = *parsed;
  }

  auto store = open_agent_store(ctx.env());
  if (!store) {
    return std::unexpected(std::move(store.error()));
  }
  std::optional<std::int64_t> cutoff;
  if (since_ms) {
    ident::system_clock clock;
    cutoff = since_cutoff_ms(clock.wall_ms(), *since_ms);
  }
  auto rows = store->history(cutoff);
  if (!rows) {
    return std::unexpected(std::move(rows.error()));
  }
  ctx.out() << (as_json ? render_json(*rows) : render_text(*rows));
  return {};
}

} // namespace planar::cmd::watch::handlers
