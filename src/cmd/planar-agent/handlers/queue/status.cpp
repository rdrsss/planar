/// @file status.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue.status`.
/// See status.cppm for the contract.

module planar.cmd.planar_agent.handlers.queue.status;

import std;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.db;
import planar.db.agentdb;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.json_text;
import planar.process.identity;

namespace planar::cmd::agent::handlers {

namespace {

namespace hq    = engine::hostqueue;
namespace ident = process::identity;
namespace qcfg  = engine::config;

constexpr int k_exit_no_such_entry = 1;
constexpr int k_exit_bad_argument  = 2;
constexpr int k_exit_queue_failed  = 125;

/// @brief Writes a refusal: the one line on standard error and, under
/// `--json`, the error object on standard output.
auto refuse(context& ctx, bool as_json, int code, std::string_view tag, const std::string& message,
            std::optional<std::int64_t> seq = std::nullopt) -> handler_outcome {
  ctx.err() << std::format("error: queue status: {}\n", message);
  if (as_json) {
    std::string out = std::format(R"({{"error":{{"verb":"queue status","tag":{},"message":{})", json_text::json_string(tag),
                                  json_text::json_string(message));
    if (seq) {
      out += std::format(R"(,"seq":{})", *seq);
    }
    out += "}}\n";
    ctx.out() << out;
  }
  return exit_status{code};
}

/// @brief A sequence number: ASCII digits only, at least 1, within `int64`.
auto parse_seq(std::string_view text) -> std::optional<std::int64_t> {
  if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  auto const   done  = std::from_chars(text.data(), text.data() + text.size(), value);
  if (done.ec != std::errc{} || done.ptr != text.data() + text.size() || value < 1) {
    return std::nullopt;
  }
  return value;
}

/// @brief A configuration failure as one line.
auto describe(const qcfg::queue_load_error& err) -> std::string {
  std::string text = err.message;
  for (auto const& finding : err.findings) {
    text += std::format("; {}: {}", finding.key, finding.message);
  }
  return text;
}

// ---- JSON -----------------------------------------------------------------

void put_key(std::string& out, std::string_view key) {
  out += out.back() == '{' ? "" : ",";
  json_text::append_json_string(out, key);
  out += ':';
}

void put_null(std::string& out, std::string_view key) {
  put_key(out, key);
  out += "null";
}

void put_int(std::string& out, std::string_view key, const std::optional<std::int64_t>& value) {
  put_key(out, key);
  out += value ? std::to_string(*value) : "null";
}

void put_text(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  put_key(out, key);
  if (value) {
    json_text::append_json_string(out, *value);
  } else {
    out += "null";
  }
}

auto argv_json(const std::vector<std::string>& argv) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    json_text::append_json_string(out, argv[i]);
  }
  out += ']';
  return out;
}

auto render_json(const hq::queue_status& s) -> std::string {
  std::string out = "{";
  put_key(out, "seq");
  out += std::to_string(s.seq);
  put_key(out, "state");
  json_text::append_json_string(out, hq::to_string(s.state));
  put_key(out, "live");
  out += s.live ? (*s.live ? "true" : "false") : "null";
  put_int(out, "position", s.position);
  put_key(out, "outcome");
  if (s.outcome) {
    json_text::append_json_string(out, hq::to_string(*s.outcome));
  } else {
    out += "null";
  }
  put_int(out, "exit_code", s.exit_code);
  put_int(out, "signal", s.signal);
  put_text(out, "terminating", s.terminating);
  put_key(out, "cancelled_by");
  if (s.cancelled_by) {
    out += '{';
    put_text(out, "vendor", s.cancelled_by->vendor);
    put_text(out, "role", s.cancelled_by->role);
    put_int(out, "pid", s.cancelled_by->pid);
    out += '}';
  } else {
    out += "null";
  }
  put_int(out, "superseded_by", s.superseded_by);
  put_key(out, "nested");
  out += s.nested ? "true" : "false";
  put_int(out, "parent_seq", s.parent_seq);
  put_key(out, "cwd");
  json_text::append_json_string(out, s.cwd);
  put_key(out, "argv");
  out += argv_json(s.argv);
  put_text(out, "label", s.label);
  put_text(out, "vendor", s.vendor);
  put_text(out, "role", s.role);
  put_text(out, "log_path", s.log_path);
  put_int(out, "enqueued_at", s.enqueued_at);
  put_int(out, "started_at", s.started_at);
  put_int(out, "ended_at", s.ended_at);
  put_int(out, "waited_ms", s.waited_ms);
  put_int(out, "ran_ms", s.ran_ms);
  put_int(out, "run_limit_ms", s.run_limit_ms);
  put_int(out, "wait_limit_ms", s.wait_limit_ms);
  put_int(out, "slots", s.slots);
  put_int(out, "grace_ms", s.grace_ms);
  out += "}\n";
  return out;
}

// ---- text -----------------------------------------------------------------

/// @brief `value` as one line's text: verbatim, or quoted as a JSON string when
/// a control character (or, with `spaces`, a space or quote) would break the
/// line-oriented form.
auto line_text(std::string_view value, bool spaces = false) -> std::string {
  auto const risky = std::ranges::any_of(value, [&](char c) {
    auto const u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f || (spaces && (c == ' ' || c == '"'));
  });
  return risky || (spaces && value.empty()) ? json_text::json_string(value) : std::string{value};
}

void line(std::string& out, std::string_view key, std::string_view value) {
  out += std::format("{}: {}\n", key, value);
}

void line_int(std::string& out, std::string_view key, const std::optional<std::int64_t>& value) {
  if (value) {
    line(out, key, std::to_string(*value));
  }
}

void line_text(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  if (value) {
    line(out, key, line_text(*value));
  }
}

auto render_text(const hq::queue_status& s) -> std::string {
  std::string out;
  line(out, "seq", std::to_string(s.seq));
  line(out, "state", hq::to_string(s.state));
  if (s.live) {
    line(out, "live", *s.live ? "true" : "false");
  }
  line_int(out, "position", s.position);
  if (s.outcome) {
    line(out, "outcome", hq::to_string(*s.outcome));
  }
  line_int(out, "exit_code", s.exit_code);
  line_int(out, "signal", s.signal);
  line_text(out, "terminating", s.terminating);
  if (s.cancelled_by) {
    line(out, "cancelled_by",
         std::format("vendor={} role={} pid={}", line_text(s.cancelled_by->vendor.value_or(""), true),
                     line_text(s.cancelled_by->role.value_or(""), true), s.cancelled_by->pid));
  }
  line_int(out, "superseded_by", s.superseded_by);
  line(out, "nested", s.nested ? "true" : "false");
  line_int(out, "parent_seq", s.parent_seq);
  line(out, "cwd", line_text(s.cwd));
  line(out, "argv", argv_json(s.argv));
  line_text(out, "label", s.label);
  line_text(out, "vendor", s.vendor);
  line_text(out, "role", s.role);
  line_text(out, "log_path", s.log_path);
  line_int(out, "enqueued_at", s.enqueued_at);
  line_int(out, "started_at", s.started_at);
  line_int(out, "ended_at", s.ended_at);
  line_int(out, "waited_ms", s.waited_ms);
  line_int(out, "ran_ms", s.ran_ms);
  line_int(out, "run_limit_ms", s.run_limit_ms);
  line_int(out, "wait_limit_ms", s.wait_limit_ms);
  line_int(out, "slots", s.slots);
  line_int(out, "grace_ms", s.grace_ms);
  return out;
}

} // namespace

auto queue_status_with(context& ctx, const cliapp::parsed_args& args, queue_status_deps deps) -> handler_outcome {
  auto const as_json = cliapp::flag_bool(args, "--json");

  auto const raw = cliapp::positional_string(args, "seq");
  if (!raw) {
    // The tree declares the positional required, so this is unreachable.
    return refuse(ctx, as_json, k_exit_bad_argument, "invalid_input", "a sequence number is required");
  }
  auto const seq = parse_seq(*raw);
  if (!seq) {
    return refuse(ctx, as_json, k_exit_bad_argument, "invalid_input",
                  std::format("'{}' is not a sequence number: expected a positive integer", *raw));
  }

  ident::system_clock system_clock;
  ident::clock&       clock = deps.clock ? *deps.clock : static_cast<ident::clock&>(system_clock);
  auto const          probe = deps.probe ? *deps.probe : hq::system_process_probe();

  // Read-only: no store is created, none is migrated, nothing is written.
  auto opened = db::agent::open_agent_db_read_only(ctx.env());
  if (!opened) {
    return refuse(ctx, as_json, k_exit_queue_failed, "store_unreachable", opened.error().message);
  }
  db::connection& conn = *opened;

  auto const now_mono = clock.monotonic_ms();
  if (!now_mono) {
    return refuse(ctx, as_json, k_exit_queue_failed, "internal", "cannot read the monotonic clock");
  }

  // The configuration is read only when the entry is still in the queue: an
  // ended entry's answer does not depend on it.
  auto load = deps.load_settings;
  if (!load) {
    load = [&ctx]() -> std::expected<qcfg::queue_settings, qcfg::queue_load_error> {
      auto const path = internal::resolve_config_path(ctx.env());
      if (!path) {
        return qcfg::default_queue_settings();
      }
      return qcfg::load_queue_settings(*path);
    };
  }

  hq::status_request request{
      .host_id  = ident::host_identity(ident::native_identity_source()),
      .now_mono = *now_mono,
      .now_wall = clock.wall_ms(),
      // An unusable `[queue]` table does not stop the answer (task
      // hq-status-degrade-config): what the store says is still true, only
      // the configuration-derived fields are unknown. `slots` and `grace_ms`
      // are reported null, liveness is judged against the default staleness
      // window (none of the broken table's values is trusted, including a
      // `stale_after` that parsed), and the reason goes to standard error.
      .settings = [&load, &ctx]() -> std::expected<hq::status_settings, std::string> {
        auto settings = load();
        if (!settings) {
          ctx.err() << std::format(
              "warning: queue status: the [queue] configuration cannot be used ({}); slots and grace_ms are unknown and "
              "liveness is judged against the default staleness window\n",
              describe(settings.error()));
          return hq::status_settings{
              .slots = std::nullopt, .stale_after_ms = qcfg::default_queue_settings().stale_after_ms, .grace_ms = std::nullopt};
        }
        return hq::status_settings{
            .slots = settings->slots, .stale_after_ms = settings->stale_after_ms, .grace_ms = settings->grace_ms};
      },
      .probe = probe,
  };

  auto answered = hq::query_status(conn, *seq, request);
  if (!answered) {
    return refuse(ctx, as_json, k_exit_queue_failed,
                  // The supplier above never fails (an unusable configuration
                  // degrades the answer), so a settings failure is internal.
                  answered.error().kind == hq::status_error_kind::settings ? "internal" : "store_unreadable",
                  answered.error().message);
  }
  if (!answered->has_value()) {
    return refuse(ctx, as_json, k_exit_no_such_entry, "not_found",
                  std::format("no queue entry or history row has sequence number {}", *seq), *seq);
  }
  ctx.out() << (as_json ? render_json(**answered) : render_text(**answered));
  return exit_status{0};
}

auto queue_status(context& ctx, const cliapp::parsed_args& args) -> handler_outcome {
  return queue_status_with(ctx, args, queue_status_deps{});
}

} // namespace planar::cmd::agent::handlers
