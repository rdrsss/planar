/// @file queue.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue`. See
/// queue.cppm for the contract.

module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

/// @brief How long past the grace period a submitter keeps advancing a stop
/// whose command group still has members, before it leaves the entry for the
/// next poll (of any process) to finish. Bounds the drain loop; SIGKILL has
/// been sent by then, so only an unkillable member outlasts it.
constexpr std::int64_t k_drain_slack_ms = 15'000;

/// @brief How often a running submitter looks at its child. The queue is
/// polled at the configured interval; the child is checked more often so
/// the submitter exits promptly when the command does.
constexpr std::chrono::milliseconds k_child_tick{20};

/// @brief The write end of the signal relay's pipe, or -1 when no relay is
/// installed. A signal handler cannot be given state, so this is the one piece
/// of process-wide state the handler reads; it is set and cleared only by
/// `signal_relay`, and a lock-free atomic is safe to read in a handler.
std::atomic<int> g_relay_fd{-1};

/// @brief The handler for the forwarded signals. Async-signal-safe: it does
/// nothing but write the signal number, as one byte, to the relay's pipe. A
/// full pipe drops the byte, which loses nothing: the wait loop forwards each
/// signal it reads, and a repeat of a signal already pending carries no
/// information.
void relay_handler(int sig) {
  auto const saved = errno;
  auto const fd    = g_relay_fd.load();
  if (fd >= 0) {
    auto const byte = static_cast<unsigned char>(sig);
    static_cast<void>(::write(fd, &byte, 1));
  }
  errno = saved;
}

/// @brief The signals a submitter forwards to its command (tech spec 647 §
/// Signals are forwarded and the entry is always removed).
constexpr std::array<int, 3> k_forwarded_signals{SIGINT, SIGTERM, SIGHUP};

/// @brief The name of a forwarded signal.
auto signal_name(int sig) -> std::string {
  switch (sig) {
  case SIGINT:
    return "SIGINT";
  case SIGTERM:
    return "SIGTERM";
  case SIGHUP:
    return "SIGHUP";
  default:
    return std::format("signal {}", sig);
  }
}

/// @brief Catches SIGINT, SIGTERM and SIGHUP for the lifetime of one
/// submission and hands them to the wait loops through a self-pipe.
///
/// The handler only writes the signal number to the pipe; every decision is
/// made in the loops, which call `drain` and `wait`. A signal that was ignored
/// when the process started (a `nohup`ed or backgrounded submitter) stays
/// ignored: the relay does not install a handler for it. The previous
/// dispositions are restored when the relay is destroyed, so a caller that
/// runs the handler in-process gets its own handlers back.
///
/// Invariants: at most one relay exists at a time (`g_relay_fd`); not
/// thread-safe, like the submitter.
class signal_relay {
  int                             _read  = -1;
  int                             _write = -1;
  std::array<struct sigaction, 3> _previous{};
  std::array<bool, 3>             _installed{};
  std::vector<int>                _pending; ///< Signals read off the pipe and not yet drained.

  signal_relay() = default;

public:
  signal_relay(const signal_relay&)            = delete;
  signal_relay& operator=(const signal_relay&) = delete;

  ~signal_relay() {
    for (std::size_t i = 0; i < k_forwarded_signals.size(); ++i) {
      if (_installed[i]) {
        ::sigaction(k_forwarded_signals[i], &_previous[i], nullptr);
      }
    }
    g_relay_fd.store(-1);
    if (_read >= 0) {
      ::close(_read);
    }
    if (_write >= 0) {
      ::close(_write);
    }
  }

  /// @brief Creates the pipe and installs the handlers.
  /// @return The relay, or `std::nullopt` when the pipe cannot be made.
  static auto open() -> std::unique_ptr<signal_relay> {
    auto relay = std::unique_ptr<signal_relay>(new signal_relay());
    int  ends[2]{-1, -1};
    if (::pipe(ends) != 0) {
      return nullptr;
    }
    relay->_read  = ends[0];
    relay->_write = ends[1];
    for (auto const fd : ends) {
      auto const flags = ::fcntl(fd, F_GETFL);
      if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return nullptr;
      }
    }
    g_relay_fd.store(relay->_write);
    for (std::size_t i = 0; i < k_forwarded_signals.size(); ++i) {
      struct sigaction current{};
      if (::sigaction(k_forwarded_signals[i], nullptr, &current) == 0 && current.sa_handler == SIG_IGN) {
        continue;
      }
      struct sigaction ours{};
      ours.sa_handler = relay_handler;
      ours.sa_flags   = SA_RESTART;
      sigfillset(&ours.sa_mask);
      if (::sigaction(k_forwarded_signals[i], &ours, &relay->_previous[i]) == 0) {
        relay->_installed[i] = true;
      }
    }
    return relay;
  }

  /// @brief Reads every signal received since the last call, including any
  /// that `wait` already took off the pipe.
  /// @return The signal numbers, in arrival order; empty when none.
  auto drain() -> std::vector<int> {
    pull();
    return std::exchange(_pending, {});
  }

  /// @brief Sleeps up to `span`, returning early when a signal arrives. The
  /// signal is moved off the pipe into the relay's own pending list, where
  /// `drain` finds it, so a loop that waits here and never drains cannot spin
  /// on a byte that stays readable.
  /// @param span The longest time to wait.
  void wait(std::chrono::milliseconds span) {
    struct pollfd watched{.fd = _read, .events = POLLIN, .revents = 0};
    static_cast<void>(::poll(&watched, 1, static_cast<int>(std::max<std::int64_t>(span.count(), 0))));
    pull();
  }

private:
  /// @brief Moves every byte now in the pipe to the pending list.
  void pull() {
    unsigned char buffer[64];
    while (true) {
      auto const n = ::read(_read, buffer, sizeof buffer);
      if (n <= 0) {
        break;
      }
      for (ssize_t i = 0; i < n; ++i) {
        _pending.push_back(buffer[i]);
      }
    }
  }
};

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

/// @brief The exit code a failed resolution or start maps to: 127 for a program
/// that cannot be found, 126 for one that cannot be executed, and the queue's
/// own 125 for anything else.
auto start_exit_code(runner::error error) -> int {
  switch (error) {
  case runner::error::not_found:
  case runner::error::empty_command:
    return 127;
  case runner::error::not_executable:
    return 126;
  default:
    return exit_internal_error;
  }
}

/// @brief The words that follow `cannot start '<program>':` for `code`.
auto start_failure_text(int code) -> std::string_view {
  return code == 127 ? "no such program" : code == 126 ? "not executable" : "the command could not be started";
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

  // The command is checked before anything else is touched, so a refusal
  // creates no entry, no ticket and no history row, and does not even open the
  // store (tech spec 647 § Submitting). The guard comes first: a launcher that
  // is also missing is refused as a launcher.
  if (auto allowed = hq::check_command(argv); !allowed) {
    ctx.err() << std::format("error: queue: refusing to queue '{}': it is a model launcher\n", allowed.error().program);
    return exit_status{exit_user_input};
  }
  if (auto resolved = runner::resolve(ctx.env(), argv.front()); !resolved) {
    auto const code = start_exit_code(resolved.error());
    ctx.err() << std::format("error: queue: cannot start '{}': {}\n", argv.front(), start_failure_text(code));
    return exit_status{code};
  }

  ident::system_clock system_clock;
  ident::clock&       clock     = deps.clock ? *deps.clock : static_cast<ident::clock&>(system_clock);
  auto const          probe     = deps.probe ? *deps.probe : hq::system_process_probe();
  auto const          signaller = deps.signaller ? deps.signaller : hq::system_group_signaller();

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

  // From here on SIGINT, SIGTERM and SIGHUP are caught: a waiting submitter
  // removes its entry, a running one forwards the signal to its command. The
  // handlers are put back when this function returns, on every path.
  auto const relay = signal_relay::open();
  if (!relay) {
    return refuse(ctx, "cannot set up signal handling");
  }
  auto const sleep =
      deps.sleep ? deps.sleep
                 : std::function<void(std::chrono::milliseconds)>{[&relay](std::chrono::milliseconds d) { relay->wait(d); }};

  // The command guard and the 126/127 checks ran at the top of this function,
  // before the configuration and the store were touched.
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

  // A signal that arrives before the command starts removes the entry and
  // runs nothing (tech spec 647 § Signals are forwarded and the entry is
  // always removed). The entry ends as `cancelled`, attributed to this
  // process, and the submitter exits 125.
  auto const interrupted = [&](int sig) {
    end(hq::end_request{.outcome      = hq::history_outcome::cancelled,
                        .cancelled_by = hq::canceller{.vendor = std::nullopt, .role = std::nullopt, .pid = pid}});
    ctx.err() << std::format("error: queue: entry {} was interrupted by {} before its turn; the command was not run\n", seq,
                             signal_name(sig));
    // The command never ran, so this is the queue's cancelled exit (decision
    // 1188), not 128 plus the signal, which is for a command a signal ended.
    return exit_status{exit_internal_error};
  };

  // --- Waiting for the turn ---------------------------------------------
  std::optional<std::int64_t> failing_since;
  std::optional<std::int64_t> own_deadline;
  while (true) {
    if (auto const signals = relay->drain(); !signals.empty()) {
      return interrupted(signals.front());
    }
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

  // The turn came, and a signal arrived while it was being taken: the command
  // has not started, so nothing is left to forward it to.
  if (auto const signals = relay->drain(); !signals.empty()) {
    return interrupted(signals.front());
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
    auto const code = start_exit_code(started_child.error());
    ctx.err() << std::format("error: queue: cannot start '{}': {}\n", argv.front(), start_failure_text(code));
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

  // SIGINT, SIGTERM and SIGHUP received from here on are forwarded to the
  // child group (tech spec 647 § Signals are forwarded and the entry is always
  // removed). Forwarding is all the submitter does: it does not mark the entry
  // terminating and does not escalate, so a command that carries on keeps its
  // slot until it ends, or until its run limit, and the submitter then exits
  // with what the command did (128 plus N when the forwarded signal killed it).
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
  bool                        stopping       = false; // The entry carries a stop reason this submitter is advancing.
  bool                        timed_out_here = false; // This submitter set the reason `timeout`.
  std::optional<std::int64_t> next_mark_attempt;      // Earliest monotonic ms to try marking the entry again.
  auto const                  enforce_run_limit = [&] {
    auto const now = clock.monotonic_ms();
    if (!now) {
      return;
    }
    if (!stopping && *now >= *own_deadline && (!next_mark_attempt || *now >= *next_mark_attempt)) {
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
      if (!stopping) {
        // SEAM (task hq-missing-entry): the entry is missing or not running
        // although this submitter is supervising its command; what to do
        // about that belongs to that task. Until then the mark is retried at
        // the poll interval, not at every tick, so a store that keeps saying
        // so is not hit with a write transaction every 20 ms.
        next_mark_attempt = *now + settings.current().poll_interval_ms;
      }
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
    // The child was just observed running, so it is not yet reaped and its
    // group id cannot have been reused.
    for (auto const sig : relay->drain()) {
      if (auto sent = runner::signal(child, sig); !sent && sent.error() != runner::error::no_such_process) {
        report.once(std::format("warning: queue: {} to the command's process group failed", signal_name(sig)));
      }
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
  if (timed_out_here) {
    reason = hq::stop_reason::timeout;
  } else if (auto stored = hq::find(conn, seq); stored && *stored) {
    if ((*stored)->terminate_reason == hq::to_string(hq::stop_reason::timeout)) {
      reason = hq::stop_reason::timeout;
    } else if ((*stored)->terminate_reason == hq::to_string(hq::stop_reason::cancelled)) {
      reason = hq::stop_reason::cancelled;
    }
  } else if (stored) {
    if (auto row = hq::find_history(conn, seq); row && *row) {
      if ((*row)->outcome == hq::history_outcome::timeout) {
        reason = hq::stop_reason::timeout;
      } else if ((*row)->outcome == hq::history_outcome::cancelled) {
        reason = hq::stop_reason::cancelled;
      }
    }
  }

  if (reason) {
    // A stopped entry keeps its slot until its child group is empty (tech spec
    // 647 § Stopping a command). The leader is reaped, but members it left
    // behind may still run, and one that ignores SIGTERM needs the SIGKILL
    // that only `advance_terminations` sends once the grace period has passed.
    // So the submitter keeps advancing its own entry each tick until the
    // engine has ended it, with the outcome its reason names, and only then
    // maps the reason to an exit code. The loop is bounded: a member that
    // survives SIGKILL past the grace period plus a slack leaves the entry in
    // place, live because its group has members, for the next poll of any
    // process to finish.
    auto const drain_started = clock.monotonic_ms().value_or(0);
    auto const gone          = [&] {
      auto const stored = hq::find(conn, seq);
      return stored && !stored->has_value();
    };
    while (!gone()) {
      auto advanced = hq::advance_terminations(
          conn, hq::advance_request{.host_id = host, .grace_ms = settings.current().grace_ms, .seq = seq}, clock, probe,
          signaller);
      if (advanced) {
        surface_advance(*advanced, report);
      } else {
        report.once(std::format("warning: queue: cannot advance the stop of entry {}: {}", seq, advanced.error().message));
      }
      if (gone()) {
        break;
      }
      auto const now = clock.monotonic_ms();
      if (now && *now - drain_started > settings.current().grace_ms + k_drain_slack_ms) {
        report.once(std::format("warning: queue: entry {}'s process group still has members after SIGKILL; leaving the entry "
                                "for the next poll to end",
                                seq));
        break;
      }
      // A signal that arrives while the group drains has nothing to add: the
      // stop is already under way and the leader is reaped. Take it off so it
      // is not left readable.
      static_cast<void>(relay->drain());
      sleep(k_child_tick);
    }
  }
  if (reason == hq::stop_reason::timeout) {
    return exit_status{exit_run_limit};
  }
  if (reason == hq::stop_reason::cancelled) {
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
