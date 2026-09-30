/// @file queue.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.queue`. See
/// queue.cppm for the contract.

module planar.cmd.planar_watch.handlers.queue;

import std;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
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

// ---- the rows ---------------------------------------------------------------

/// @brief One listed entry with everything derived for display.
struct row {
  hq::entry                   entry;
  std::optional<bool>         live;     ///< Empty when the probe failed.
  std::optional<std::int64_t> position; ///< Place among the waiting entries.
  std::optional<std::int64_t> waited_ms;
  std::optional<std::int64_t> ran_ms;
};

/// @brief The `[queue]` staleness window, degraded to the default (with one
/// warning) when the configuration cannot be used.
auto staleness_window(context& ctx) -> std::int64_t {
  auto const path     = internal::resolve_config_path(ctx.env());
  auto const settings = path ? qcfg::load_queue_settings(*path)
                             : std::expected<qcfg::queue_settings, qcfg::queue_load_error>{qcfg::default_queue_settings()};
  if (settings) {
    return settings->stale_after_ms;
  }
  std::string reason = settings.error().message;
  for (auto const& finding : settings.error().findings) {
    reason += std::format("; {}: {}", finding.key, finding.message);
  }
  ctx.err() << std::format("warning: queue: the [queue] configuration cannot be used ({}); liveness is judged against the "
                           "default staleness window\n",
                           reason);
  return qcfg::default_queue_settings().stale_after_ms;
}

/// @brief Entries in sequence order, running and waiting alike;
/// liveness judged, positions and durations derived. Reads only.
auto build_rows(context& ctx, std::vector<hq::entry> entries) -> std::expected<std::vector<row>, domain_error> {
  if (entries.empty()) {
    return std::vector<row>{};
  }
  std::ranges::sort(entries, [](const hq::entry& a, const hq::entry& b) { return a.seq < b.seq; });

  ident::system_clock clock;
  auto const          now_mono = clock.monotonic_ms();
  if (!now_mono) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "cannot read the monotonic clock"));
  }
  auto const now_wall = clock.wall_ms();
  auto const context  = hq::liveness_context{.host_id        = ident::host_identity(ident::native_identity_source()),
                                             .now_mono       = *now_mono,
                                             .stale_after_ms = staleness_window(ctx)};
  auto const probe    = hq::system_process_probe();

  std::vector<row> rows;
  rows.reserve(entries.size());
  std::int64_t place = 0;
  for (auto& e : entries) {
    row r;
    if (auto const verdict = hq::judge_liveness(e, context, probe)) {
      r.live = verdict->live;
    }
    if (e.state == hq::entry_state::waiting) {
      r.position  = ++place;
      r.waited_ms = span_ms(e.enqueued_at, now_wall);
    } else if (e.started_at) {
      r.waited_ms = span_ms(e.enqueued_at, *e.started_at);
      r.ran_ms    = span_ms(*e.started_at, now_wall);
    }
    r.entry = std::move(e);
    rows.push_back(std::move(r));
  }
  return rows;
}

// ---- JSON -------------------------------------------------------------------

auto render_json(const std::vector<row>& rows) -> std::string {
  std::string out = "[";
  for (auto const& r : rows) {
    auto const& e = r.entry;
    out += out.back() == '[' ? "{" : ",{";
    put_int(out, "seq", e.seq);
    put_key(out, "state");
    out += e.state == hq::entry_state::waiting ? "\"waiting\"" : "\"running\"";
    put_key(out, "live");
    out += r.live ? (*r.live ? "true" : "false") : "null";
    put_int(out, "position", r.position);
    put_text(out, "terminating", e.terminate_reason);
    put_key(out, "nested");
    out += e.parent_seq ? "true" : "false";
    put_int(out, "parent_seq", e.parent_seq);
    put_key(out, "cwd");
    out += quote(e.cwd);
    put_argv(out, e.argv);
    put_text(out, "label", e.label);
    put_text(out, "vendor", e.vendor);
    put_text(out, "role", e.role);
    put_text(out, "log_path", e.log_path);
    put_int(out, "enqueued_at", e.enqueued_at);
    put_int(out, "started_at", e.started_at);
    put_int(out, "waited_ms", r.waited_ms);
    put_int(out, "ran_ms", r.ran_ms);
    put_int(out, "run_limit_ms", e.run_limit_ms);
    put_int(out, "wait_limit_ms", e.wait_limit_ms);
    out += '}';
  }
  out += "]\n";
  return out;
}

// ---- text -------------------------------------------------------------------

auto notes_of(const row& r) -> std::string {
  std::vector<std::string> notes;
  if (r.live && !*r.live) {
    notes.emplace_back("NOT-LIVE");
  } else if (!r.live) {
    notes.emplace_back("LIVE-UNKNOWN");
  }
  if (r.entry.parent_seq) {
    notes.push_back(std::format("nested:{}", *r.entry.parent_seq));
  }
  if (r.entry.terminate_reason) {
    notes.push_back(std::format("stopping:{}", cell(*r.entry.terminate_reason)));
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

auto render_text(const std::vector<row>& rows) -> std::string {
  if (rows.empty()) {
    return {};
  }
  std::vector<std::vector<std::string>> table;
  table.push_back({"SEQ", "STATE", "POS", "NOTES", "WAITED", "RAN", "VENDOR", "ROLE", "LABEL", "DIRECTORY", "COMMAND"});
  for (auto const& r : rows) {
    auto const& e = r.entry;
    table.push_back({std::to_string(e.seq), e.state == hq::entry_state::waiting ? "waiting" : "running",
                     r.position ? std::to_string(*r.position) : "-", notes_of(r), duration_text(r.waited_ms),
                     duration_text(r.ran_ms), cell(e.vendor.value_or("")), cell(e.role.value_or("")), cell(e.label.value_or("")),
                     cell(e.cwd), shell_line(e.argv)});
  }

  return pad_table(table);
}

} // namespace

auto queue(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const as_json = cliapp::flag_bool(args, "--json");

  auto store = open_agent_store(ctx.env());
  if (!store) {
    return std::unexpected(std::move(store.error()));
  }
  auto entries = store->entries();
  if (!entries) {
    return std::unexpected(std::move(entries.error()));
  }
  auto rows = build_rows(ctx, std::move(*entries));
  if (!rows) {
    return std::unexpected(std::move(rows.error()));
  }
  ctx.out() << (as_json ? render_json(*rows) : render_text(*rows));
  return {};
}

} // namespace planar::cmd::watch::handlers
