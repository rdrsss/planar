/// @file queue.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue`. See
/// queue.cppm for the contract.

module;

#include <unistd.h>

module planar.cmd.planar_agent.handlers.queue;

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
import planar.process.identity;
import planar.process.runner;

namespace planar::cmd::agent::handlers {

namespace {

namespace hq     = engine::hostqueue;
namespace ident  = process::identity;
namespace runner = process::runner;
namespace qcfg   = engine::config;

/// @brief The run limit an entry is given when nothing else names one
/// (tech spec 647 § Time limits are built in: thirty minutes). `--timeout`
/// replaces it.
constexpr std::int64_t k_default_run_limit_ms = 30LL * 60 * 1000;

/// @brief The exit status of a command stopped at its run limit (decision
/// 1188).
constexpr int exit_run_limit = 124;

/// @brief How often a running submitter looks at its child. The queue is
/// polled at the configured interval; the child is checked more often so
/// the submitter exits promptly when the command does.
constexpr std::chrono::milliseconds k_child_tick{20};

/// @brief Writes one diagnostic line at most once per distinct text, so a
/// condition that persists across polls is not repeated every interval.
class reporter {
  std::ostream*         _err;
  std::set<std::string> _seen;

public:
  /// @brief A reporter writing to `err`.
  /// @param err The stream diagnostics go to.
  explicit reporter(std::ostream& err) : _err(&err) {
  }

  /// @brief Writes `line` and a newline unless this text was written before.
  /// @param line The diagnostic, without a newline.
  void once(const std::string& line) {
    if (_seen.insert(line).second) {
      *_err << line << '\n';
    }
  }
};

/// @brief The words a failed process query or signal is reported with.
auto describe(ident::error err) -> std::string_view {
  switch (err) {
  case ident::error::no_such_process:
    return "no such process";
  case ident::error::not_permitted:
    return "not permitted";
  case ident::error::invalid_signal:
    return "invalid signal";
  case ident::error::query_failed:
    return "process query failed";
  case ident::error::clock_failed:
    return "clock failed";
  }
  return "unknown failure";
}

/// @brief A configuration failure as one line: the message and every refused
/// key.
auto describe(const qcfg::queue_load_error& err) -> std::string {
  std::string text = err.message;
  for (auto const& finding : err.findings) {
    text += std::format("; {}: {}", finding.key, finding.message);
  }
  return text;
}

/// @brief The `[queue]` settings in force, reloaded at every poll.
class settings_source {
  std::function<std::expected<qcfg::queue_settings, qcfg::queue_load_error>()> _load;
  qcfg::queue_settings                                                         _current;
  std::optional<std::string>                                                   _failure;

public:
  /// @brief A source that calls `load`.
  /// @param load The loader.
  explicit settings_source(std::function<std::expected<qcfg::queue_settings, qcfg::queue_load_error>()> load)
      : _load(std::move(load)) {
  }

  /// @brief Loads the settings for the first time.
  /// @return The failure, when the settings cannot be used.
  auto initial() -> std::expected<void, qcfg::queue_load_error> {
    auto loaded = _load();
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    _current = *loaded;
    return {};
  }

  /// @brief Reloads the settings. On failure the previous settings stay in
  /// force and the failure is written to `err` once per streak.
  /// @param err The stream the notice goes to.
  void reload(std::ostream& err) {
    auto loaded = _load();
    if (loaded) {
      _current = *loaded;
      _failure.reset();
      return;
    }
    auto text = describe(loaded.error());
    if (_failure != text) {
      err << "warning: queue: cannot reload the [queue] configuration, keeping the previous settings: " << text << '\n';
      _failure = std::move(text);
    }
  }

  /// @brief The settings in force.
  /// @return The last settings that loaded.
  [[nodiscard]] auto current() const -> const qcfg::queue_settings& {
    return _current;
  }
};

/// @brief The queue's failure exit (decision 1188): one line, exit 125.
auto refuse(context& ctx, const std::string& body) -> handler_outcome {
  ctx.err() << "error: queue: " << body << '\n';
  return exit_status{exit_internal_error};
}

/// @brief Reports a stopping signal that could not be sent, once.
void report_signal_failure(const hq::signal_attempt& attempt, reporter& report) {
  if (attempt.outcome != hq::signal_outcome::failed) {
    return;
  }
  auto const name   = attempt.signal == hq::stop_signal::term ? "SIGTERM" : "SIGKILL";
  auto const reason = attempt.error ? describe(*attempt.error) : std::string_view{"unknown failure"};
  report.once(std::format("warning: queue: {} to entry {}'s process group failed: {}", name, attempt.seq, reason));
}

/// @brief Writes what one `advance_terminations` could not do, each distinct
/// line once.
void surface_advance(const hq::advance_result& advanced, reporter& report) {
  for (auto const& attempt : advanced.kills) {
    report_signal_failure(attempt, report);
  }
  for (auto const& attempt : advanced.failures) {
    report.once(std::format("warning: queue: cannot judge the process group of terminating entry {}: {}", attempt.seq,
                            attempt.error ? describe(*attempt.error) : std::string_view{"unknown failure"}));
  }
  for (auto const& failure : advanced.end_failures) {
    report.once(std::format("warning: queue: cannot end terminating entry {}: {}", failure.seq, failure.error.message));
  }
}

/// @brief Writes what one `poll_and_stop` could not do, each distinct line
/// once: failed SIGTERMs and SIGKILLs, a failed advance, terminating entries
/// that could not be ended, and entries whose liveness could not be judged.
void surface(const hq::poll_stop_result& result, reporter& report) {
  for (auto const& attempt : result.sigterms) {
    report_signal_failure(attempt, report);
  }
  surface_advance(result.advanced, report);
  if (result.advance_error) {
    report.once(std::format("warning: queue: cannot advance terminating entries: {}", result.advance_error->message));
  }
  for (auto const& failure : result.poll.liveness_errors) {
    report.once(std::format("warning: queue: cannot judge whether entry {} is live: {}", failure.seq, describe(failure.error)));
  }
}

/// @brief What a submitter needs to poll: the store, the identity, the
/// clocks and the settings, gathered so the waiting and running loops share
/// one poll.
struct poller {
  db::connection&            conn;
  std::int64_t               seq;
  std::string                host_id;
  ident::clock&              clock;
  const hq::process_probe&   probe;
  const hq::group_signaller& signaller;
  settings_source&           settings;
  std::ostream&              err;
  reporter&                  report;
  std::int64_t               run_limit_ms = k_default_run_limit_ms; ///< The entry's run limit, from `--timeout`.

  /// @brief Reloads the settings and runs one poll and its stopping steps.
  /// @return The result, or the failure that kept the poll from running.
  auto poll_once() -> std::expected<hq::poll_stop_result, hq::queue_error> {
    settings.reload(err);
    auto const& now    = settings.current();
    auto        result = hq::poll_and_stop(conn,
                                           hq::poll_stop_request{.poll = hq::poll_request{.seq            = seq,
                                                                                          .host_id        = host_id,
                                                                                          .slots          = now.slots,
                                                                                          .stale_after_ms = now.stale_after_ms,
                                                                                          .run_limit_ms   = run_limit_ms},
                                                                 .grace_ms = now.grace_ms},
                                           clock, probe, signaller);
    if (result) {
      surface(*result, report);
    }
    return result;
  }
};

/// @brief The exit status a finished command maps to: its own code, or 128
/// plus the signal that ended it.
auto status_code(const runner::status& status) -> int {
  if (status.kind == runner::state::signalled) {
    return std::min(exit_status_max, 128 + status.code);
  }
  return status.code;
}

} // namespace

auto queue_run(context& ctx, const cliapp::parsed_args& args) -> handler_outcome {
  return queue_run_with(ctx, args, queue_run_deps{});
}

auto queue_run_with(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps) -> handler_outcome {
  auto const argv = cliapp::positional_strings(args, "command");
  if (argv.empty()) {
    // The tree declares the positional required, so this is unreachable
    // through argv; a caller that built `args` by hand gets the parse
    // failure's code rather than an enqueue of nothing.
    ctx.err() << "error: queue: run: no command given\n";
    return exit_status{exit_generic_failure};
  }

  // The time limits. A refused value is refused before the configuration or
  // the store is touched, and nothing is enqueued.
  std::int64_t                run_limit_ms = k_default_run_limit_ms;
  std::optional<std::int64_t> wait_limit_ms;
  for (auto const* name : {"--timeout", "--wait-timeout"}) {
    auto const text = cliapp::flag_string(args, name);
    if (!text) {
      continue;
    }
    auto const parsed = qcfg::parse_duration_flag(*text);
    if (!parsed) {
      ctx.err() << std::format("error: queue: run: {}: {}\n", name, parsed.error());
      return exit_status{exit_user_input};
    }
    if (std::string_view{name} == "--timeout") {
      run_limit_ms = *parsed;
    } else {
      wait_limit_ms = *parsed;
    }
  }

  ident::system_clock system_clock;
  ident::clock&       clock     = deps.clock ? *deps.clock : static_cast<ident::clock&>(system_clock);
  auto const          probe     = deps.probe ? *deps.probe : hq::system_process_probe();
  auto const          signaller = deps.signaller ? deps.signaller : hq::system_group_signaller();
  auto const sleep = deps.sleep ? deps.sleep : std::function<void(std::chrono::milliseconds)>{[](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  }};

  // Configuration first: an unusable configuration must refuse before the
  // store is touched. The path is the context's environment's, exactly as
  // `planar config` resolves it; when it cannot be resolved (no HOME and no
  // PLANAR_CONFIG_PATH) the defaults apply.
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
  settings_source settings{std::move(load)};
  if (auto first = settings.initial(); !first) {
    return refuse(ctx, describe(first.error()));
  }

  // The store. An unreachable store refuses the command (decision 1185).
  auto opened = db::agent::open_agent_db(ctx.env());
  if (!opened) {
    return refuse(ctx, opened.error().message);
  }
  db::connection& conn = *opened;

  // The submitter's identity, read once at enqueue.
  auto const host    = ident::host_identity(ident::native_identity_source());
  auto const pid     = static_cast<std::int64_t>(::getpid());
  auto const started = ident::process_start_time(pid);
  if (!started || !started->has_value()) {
    return refuse(ctx, "cannot read this process's start time");
  }
  auto const now_mono = clock.monotonic_ms();
  if (!now_mono) {
    return refuse(ctx, "cannot read the monotonic clock");
  }

  // SEAM (task hq-command-guard, hq-not-started): the guard and the 126/127
  // checks run here, before anything is enqueued.
  // SEAM (task hq-vendor-role): `--vendor` and `--role` fill `vendor` and
  // `role`.
  auto enqueued = hq::enqueue(
      conn,
      hq::enqueue_request{
          .host_id            = host,
          .pid                = pid,
          .pid_started        = static_cast<std::int64_t>(**started),
          .cwd                = ctx.cwd().string(),
          .argv               = argv,
          .label              = cliapp::flag_string(args, "--label"),
          .enqueued_at        = clock.wall_ms(),
          .refreshed_mono     = *now_mono,
          .wait_deadline_mono = wait_limit_ms ? std::optional<std::int64_t>{*now_mono + *wait_limit_ms} : std::nullopt,
      },
      settings.current().history_days);
  if (!enqueued) {
    return refuse(ctx, enqueued.error().message);
  }
  auto const seq = enqueued->seq;
  for (auto const& failure : enqueued->pruned.log_failures) {
    ctx.err() << std::format("warning: queue: cannot remove pruned log file {}: {}\n", failure.path, failure.message);
  }

  reporter report{ctx.err()};
  poller   poll{.conn         = conn,
                .seq          = seq,
                .host_id      = host,
                .clock        = clock,
                .probe        = probe,
                .signaller    = signaller,
                .settings     = settings,
                .err          = ctx.err(),
                .report       = report,
                .run_limit_ms = run_limit_ms};

  // Ends the entry with `request`, reporting a store failure. The command's
  // status is the caller's to return either way.
  auto const end = [&](hq::end_request request) {
    request.ended_at = clock.wall_ms();
    auto ended       = hq::end_entry(conn, seq, request);
    if (!ended) {
      ctx.err() << std::format("warning: queue: cannot record the end of entry {}: {}\n", seq, ended.error().message);
    }
    // `already_gone` (the entry was reaped under us) writes nothing; the
    // missing-entry rule (task hq-missing-entry) refines it.
  };

  // --- Waiting for the turn ---------------------------------------------
  std::optional<std::int64_t> failing_since;
  std::optional<std::int64_t> own_deadline;
  while (true) {
    auto       polled = poll.poll_once();
    auto const now    = clock.monotonic_ms();
    if (!polled || polled->poll.status == hq::poll_status::skipped) {
      if (!polled) {
        report.once(std::format("warning: queue: cannot poll the queue: {}", polled.error().message));
      }
      if (now) {
        if (!failing_since) {
          failing_since = *now;
        } else if (*now - *failing_since > settings.current().stale_after_ms) {
          // By now other submitters may have reaped this entry. Best effort:
          // end it ourselves as abandoned (the entry's fate when its submitter
          // stops being live) so it is not left for a reaper, then give up.
          end(hq::end_request{.outcome = hq::history_outcome::abandoned});
          return refuse(ctx, std::format("entry {} could not be polled for longer than the staleness window; giving up", seq));
        }
      }
    } else {
      failing_since.reset();
      if (polled->poll.entry_missing) {
        // SEAM (task hq-missing-entry): read the history row and either
        // re-enqueue (`abandoned`) or exit 125 (`cancelled`).
        return refuse(ctx, std::format("entry {} is no longer in the queue", seq));
      }
      if (polled->poll.running) {
        own_deadline = polled->poll.deadline_mono;
        break;
      }
    }
    // The wait limit. It is judged after the poll, so an entry whose turn has
    // come at its limit runs rather than being removed.
    if (wait_limit_ms && now && *now >= *now_mono + *wait_limit_ms) {
      end(hq::end_request{.outcome = hq::history_outcome::wait_timeout});
      return refuse(ctx, std::format("entry {} waited longer than --wait-timeout and was removed; the command was not run", seq));
    }
    // Sleep a poll interval, or less when the wait limit falls inside it, so the
    // limit is honoured to the millisecond rather than to the next poll.
    auto nap = settings.current().poll_interval_ms;
    if (wait_limit_ms && now) {
      nap = std::clamp(*now_mono + *wait_limit_ms - *now, std::int64_t{1}, nap);
    }
    sleep(std::chrono::milliseconds{nap});
  }

  // --- Running -----------------------------------------------------------
  // SEAM (task hq-nested-run): a run that finds PLANAR_QUEUE_SLOT naming a
  // live running entry runs as a nested entry instead of waiting above.
  auto started_child = runner::start(
      ctx.env(), argv,
      runner::start_options{.working_directory = ctx.cwd(), .env_name = "PLANAR_QUEUE_SLOT", .env_value = std::to_string(seq)});
  if (!started_child) {
    // The program vanished or lost its permission between the checks and the
    // turn, or could not be started at all.
    auto const error = started_child.error();
    int        code  = exit_internal_error;
    if (error == runner::error::not_found) {
      code = 127;
    } else if (error == runner::error::not_executable) {
      code = 126;
    }
    ctx.err() << std::format("error: queue: cannot start '{}': {}\n", argv.front(),
                             code == 127   ? "no such program"
                             : code == 126 ? "not executable"
                                           : "the command could not be started");
    if (code == 127 || code == 126) {
      end(hq::end_request{.outcome = hq::history_outcome::not_started, .exit_code = code});
    } else {
      end(hq::end_request{.outcome = hq::history_outcome::abandoned});
    }
    return exit_status{code};
  }
  auto const child = *started_child;

  // Record the child group on the entry, so that the entry stays live while
  // the group has members even if this process is killed. A store failure is
  // reported and supervision goes on.
  if (auto recorded = hq::record_child(conn, seq, child.pgid, static_cast<std::int64_t>(child.started)); !recorded) {
    report.once(std::format("warning: queue: cannot record the command's process group: {}", recorded.error().message));
  }

  // SEAM (task hq-signal-forwarding): SIGINT, SIGTERM and SIGHUP received
  // here are forwarded to the child group.
  //
  // The submitter enforces its own run limit (tech spec 647 § Running, step
  // 3): at the entry's recorded deadline it marks the entry terminating with
  // reason `timeout`, which sends SIGTERM, and from then on advances the
  // stopping steps for its own entry each tick, which sends SIGKILL once the
  // grace period has passed. Both go through the engine's terminate module.
  if (!own_deadline) {
    // The poll that started the entry always reports its deadline; if it did
    // not, derive it from the limit rather than run without one.
    own_deadline = clock.monotonic_ms().value_or(0) + run_limit_ms;
  }
  bool       stopping          = false; // The entry carries a stop reason this submitter is advancing.
  bool       timed_out_here    = false; // This submitter set the reason `timeout`.
  auto const enforce_run_limit = [&] {
    auto const now = clock.monotonic_ms();
    if (!now) {
      return;
    }
    if (!stopping && *now >= *own_deadline) {
      auto begun =
          hq::begin_terminate(conn, hq::begin_terminate_request{.seq = seq, .reason = hq::stop_reason::timeout, .host_id = host},
                              clock, probe, signaller);
      if (!begun) {
        // Retried at the next tick.
        report.once(std::format("warning: queue: cannot stop entry {} at its run limit: {}", seq, begun.error().message));
        return;
      }
      if (begun->sigterm) {
        report_signal_failure(*begun->sigterm, report);
      }
      stopping       = begun->status == hq::begin_status::marked || begun->status == hq::begin_status::already_terminating;
      timed_out_here = begun->status == hq::begin_status::marked;
    }
    if (stopping) {
      auto advanced = hq::advance_terminations(
          conn, hq::advance_request{.host_id = host, .grace_ms = settings.current().grace_ms, .seq = seq}, clock, probe,
          signaller);
      if (advanced) {
        surface_advance(*advanced, report);
      } else {
        report.once(std::format("warning: queue: cannot advance the stop of entry {}: {}", seq, advanced.error().message));
      }
    }
  };

  runner::status final_status;
  auto           next_poll = clock.monotonic_ms().value_or(0) + settings.current().poll_interval_ms;
  while (true) {
    auto observed = runner::poll(child);
    if (!observed) {
      ctx.err() << "error: queue: cannot observe the command's status\n";
      end(hq::end_request{.outcome = hq::history_outcome::abandoned});
      return exit_status{exit_internal_error};
    }
    if (observed->kind != runner::state::running) {
      final_status = *observed;
      break;
    }
    enforce_run_limit();
    sleep(k_child_tick);
    auto const now = clock.monotonic_ms();
    if (now && *now >= next_poll) {
      // A refresh that fails keeps supervising: a running submitter that
      // cannot reach the store never stops its command.
      if (auto polled = poll.poll_once(); !polled) {
        report.once(std::format("warning: queue: cannot poll the queue: {}", polled.error().message));
      }
      next_poll = *now + settings.current().poll_interval_ms;
    }
  }

  // How the entry ends. A stop reason on the submitter's own entry decides it,
  // whoever set it (this submitter at its run limit, or another process: a
  // cancellation, or an orphan-deadline poll), and a command that died of the
  // SIGTERM or SIGKILL sent for it is never recorded as `signaled`. Another
  // process may already have ended the entry once its group was empty; then
  // the history row it wrote names the reason.
  std::optional<hq::stop_reason> reason;
  std::optional<hq::canceller>   who;
  if (timed_out_here) {
    reason = hq::stop_reason::timeout;
  } else if (auto stored = hq::find(conn, seq); stored && *stored) {
    if ((*stored)->terminate_reason == hq::to_string(hq::stop_reason::timeout)) {
      reason = hq::stop_reason::timeout;
    } else if ((*stored)->terminate_reason == hq::to_string(hq::stop_reason::cancelled)) {
      reason = hq::stop_reason::cancelled;
      if ((*stored)->cancelled_by) {
        if (auto decoded = hq::decode_canceller(*(*stored)->cancelled_by); decoded) {
          who = std::move(*decoded);
        }
      }
    }
  } else if (stored) {
    if (auto row = hq::find_history(conn, seq); row && *row) {
      if ((*row)->outcome == hq::history_outcome::timeout) {
        reason = hq::stop_reason::timeout;
      } else if ((*row)->outcome == hq::history_outcome::cancelled) {
        reason = hq::stop_reason::cancelled;
        who    = (*row)->cancelled_by;
      }
    }
  }

  if (reason == hq::stop_reason::timeout) {
    end(hq::end_request{.outcome = hq::history_outcome::timeout});
    return exit_status{exit_run_limit};
  }
  if (reason == hq::stop_reason::cancelled) {
    // A cancellation without a readable canceller ends as `abandoned`, as the
    // terminate module ends it, because a `cancelled` row needs one.
    end(who ? hq::end_request{.outcome = hq::history_outcome::cancelled, .cancelled_by = *who}
            : hq::end_request{.outcome = hq::history_outcome::abandoned});
    return exit_status{exit_internal_error};
  }

  if (final_status.kind == runner::state::signalled) {
    end(hq::end_request{.outcome = hq::history_outcome::signaled, .signal = final_status.code});
  } else {
    end(hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = final_status.code});
  }
  return exit_status{status_code(final_status)};
}

} // namespace planar::cmd::agent::handlers
