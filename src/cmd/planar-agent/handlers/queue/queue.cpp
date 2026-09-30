/// @file queue.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue`. See
/// queue.cppm for the contract.

module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/// @brief How many times one submitter rejoins the queue after being reaped
/// while it waited (task hq-missing-entry). The spec has a reaped waiter
/// rejoin and names no limit; a submitter whose every new entry is reaped too
/// would otherwise queue for ever, so after this many rejoins it gives up
/// with 125. Reaping a waiter takes a stop longer than the staleness window,
/// so three is far more than a healthy host ever needs.
constexpr int k_max_rejoins = 3;

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

/// @brief The loader of the `[queue]` settings from the file the context's
/// environment names, exactly as `planar config` resolves it; when the path
/// cannot be resolved (no HOME and no PLANAR_CONFIG_PATH) the defaults apply.
/// @param ctx The invocation context, which must outlive the returned loader.
auto default_settings_loader(context& ctx) -> std::function<std::expected<qcfg::queue_settings, qcfg::queue_load_error>()> {
  return [&ctx]() -> std::expected<qcfg::queue_settings, qcfg::queue_load_error> {
    auto const path = internal::resolve_config_path(ctx.env());
    if (!path) {
      return qcfg::default_queue_settings();
    }
    return qcfg::load_queue_settings(*path);
  };
}

/// @brief One identity field of a submission: the flag when it is given and not
/// empty, else the environment variable when it is set and not empty, else
/// nothing (task hq-vendor-role). An empty value reads as absent, as
/// `$PLANAR_VENDOR` does everywhere else, so an entry never stores an empty
/// string. Planar does not guess a value.
/// @param args The parsed arguments.
/// @param flag The flag name, with its dashes.
/// @param env The context's environment lookup.
/// @param variable The environment variable that stands in for the flag.
/// @return The value to store, or `std::nullopt`.
template <class Env>
auto identity_field(const cliapp::parsed_args& args, std::string_view flag, const Env& env, std::string_view variable)
    -> std::optional<std::string> {
  if (auto given = cliapp::flag_string(args, flag); given && !given->empty()) {
    return given;
  }
  if (auto set = env(std::string{variable}); set && !set->empty()) {
    return set;
  }
  return std::nullopt;
}

/// @brief Writes queue notices to standard error when `--notices` was given
/// (tech spec 647 § CLI surface). Notices go to the error stream only, never
/// the output stream, and nothing is written without the flag. The
/// `warning: queue:` diagnostics are not notices: they report a degraded path
/// and are written with or without the flag.
class notices {
  std::ostream* _err;
  bool          _enabled;

public:
  /// @brief A notice writer.
  /// @param err The stream notices go to.
  /// @param enabled Whether `--notices` was given.
  notices(std::ostream& err, bool enabled) : _err(&err), _enabled(enabled) {
  }

  /// @brief Whether notices are on.
  [[nodiscard]] auto enabled() const -> bool {
    return _enabled;
  }

  /// @brief Writes `queue: entry <seq> <text>` and a newline, when enabled.
  /// @param seq The entry's sequence number.
  /// @param text What happened to it.
  void line(std::int64_t seq, const std::string& text) const {
    if (_enabled) {
      *_err << std::format("queue: entry {} {}\n", seq, text);
    }
  }
};

/// @brief The place of entry `seq` among the waiting entries, counting from
/// 1, or `std::nullopt` when it is not waiting or the store cannot say.
auto waiting_position(db::connection& conn, std::int64_t seq) -> std::optional<std::int64_t> {
  auto const all = hq::list(conn);
  if (!all) {
    return std::nullopt;
  }
  std::vector<std::int64_t> waiting;
  for (auto const& entry : *all) {
    if (entry.state == hq::entry_state::waiting) {
      waiting.push_back(entry.seq);
    }
  }
  std::ranges::sort(waiting);
  auto const found = std::ranges::find(waiting, seq);
  if (found == waiting.end()) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(found - waiting.begin()) + 1;
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

/// @brief The sequence number a slot marker names, when the text is one: a
/// decimal integer that fits the store's key and is positive, with nothing
/// before or after it. Anything else names no entry (tech spec 647 § The slot
/// marker is advisory).
auto parse_slot_marker(std::string_view text) -> std::optional<std::int64_t> {
  std::int64_t value  = 0;
  auto const*  first  = text.data();
  auto const*  last   = first + text.size();
  auto const   parsed = std::from_chars(first, last, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != last || value <= 0) {
    return std::nullopt;
  }
  return value;
}

/// @brief Whether a store failure is the store staying busy past the
/// connection's busy timeout: the plain `SQLITE_BUSY` in the low byte of the
/// extended code.
auto is_busy_failure(const hq::queue_error& error) -> bool {
  return error.kind == hq::queue_error_kind::query_failed &&
         db::is_busy(db::db_error{.code_ = error.sqlite_code, .message_ = {}});
}

/// @brief The words that follow `cannot start '<program>':` for `code`.
auto start_failure_text(int code) -> std::string_view {
  return code == 127 ? "no such program" : code == 126 ? "not executable" : "the command could not be started";
}

/// @brief What a detached child holds while it runs the ordinary submitter:
/// the write end of the ticket pipe, a capture of its standard error until the
/// log file exists, and the seams a test hooks (tech spec 647 § Submitting,
/// With `--detach`).
///
/// Until the ticket is reported, the child's standard error is a private
/// buffer, so every refusal the submitter writes (a configuration that cannot
/// be read, an unreachable store, a log file that cannot be created) is text
/// the invoked process can print; the invoked process is the only one with a
/// terminal to say it on. From the moment the log exists, standard error is
/// the log again.
///
/// Invariants: one link per detached child, used only by that child, which is
/// single-threaded; `write_fd` is -1 once the pipe has been closed.
struct detach_link {
  std::ostream*                         err      = nullptr; ///< The context's error stream, whose buffer is swapped.
  std::streambuf*                       original = nullptr; ///< The stream's own buffer, put back once the log exists.
  std::stringbuf                        capture;            ///< What was written to `err` before the log existed.
  int                                   write_fd = -1;      ///< The pipe to the invoked process.
  bool                                  reported = false;   ///< Whether the ticket or a failure has been sent.
  std::filesystem::path                 log_dir;            ///< `<agent-db-directory>/queue-logs`.
  std::function<void(std::string_view)> hook;               ///< The test seam; may be empty.

  /// @brief Points the error stream at the private buffer.
  void begin_capture(std::ostream& stream) {
    err      = &stream;
    original = stream.rdbuf(&capture);
  }

  /// @brief Puts the stream's own buffer back and moves what was captured to it.
  void end_capture() {
    if (err == nullptr || original == nullptr) {
      return;
    }
    auto const held = capture.str();
    err->rdbuf(original);
    original = nullptr;
    *err << std::unitbuf;
    *err << held;
    err->flush();
  }

  /// @brief Runs the test seam for `stage`, when there is one.
  void at(std::string_view stage) const {
    if (hook) {
      hook(stage);
    }
  }
};

/// @brief Writes all of `text` to `fd`. A closed reader is a failed write, not
/// a signal: SIGPIPE is ignored for the duration and put back.
auto write_all(int fd, std::string_view text) -> bool {
  struct sigaction ignore{};
  struct sigaction previous{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  ::sigaction(SIGPIPE, &ignore, &previous);
  bool ok = true;
  while (!text.empty()) {
    auto const n = ::write(fd, text.data(), text.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    text.remove_prefix(static_cast<std::size_t>(n));
  }
  ::sigaction(SIGPIPE, &previous, nullptr);
  return ok;
}

/// @brief The text of an `errno` value.
auto errno_text(int code) -> std::string {
  return std::error_code(code, std::generic_category()).message();
}

/// @brief Sends a failure to the invoked process, once, and closes the pipe.
/// What was captured from standard error is the message; a child that failed
/// without writing anything says only that no ticket was issued.
void report_failure(detach_link& link, std::string_view fallback) {
  if (link.reported || link.write_fd < 0) {
    return;
  }
  auto text = link.capture.str();
  if (text.empty()) {
    text = std::format("error: queue: no ticket was issued: {}\n", fallback);
  } else if (!text.ends_with('\n')) {
    text += '\n';
  }
  static_cast<void>(write_all(link.write_fd, "err\n" + text));
  ::close(link.write_fd);
  link.write_fd = -1;
  link.reported = true;
}

/// @brief Closes every descriptor a detached submitter inherited except
/// `keep`, so it holds nothing of its caller's: a reader waiting for the end
/// of a pipe the caller passed down would otherwise wait for the whole run.
void close_inherited_descriptors(int keep) {
  auto limit = ::sysconf(_SC_OPEN_MAX);
  if (limit < 0 || limit > 8192) {
    limit = 8192;
  }
  for (int fd = 3; fd < limit; ++fd) {
    if (fd != keep) {
      ::close(fd);
    }
  }
}

/// @brief The invoked process's half of a detached submission: reads the
/// pipe to its end and turns what came through into the ticket, the child's
/// failure, or a message that no ticket was issued (tech spec 647 §
/// Submitting, step 6). It never waits on the child itself: end of file on
/// the pipe means every holder of the write end is gone, which is the
/// child's death or its report.
auto await_ticket(context& ctx, int read_fd, ::pid_t child) -> handler_outcome {
  std::string text;
  char        buffer[512];
  while (true) {
    auto const n = ::read(read_fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    text.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(read_fd);

  if (text.starts_with("ok\n") && text.ends_with('\n')) {
    auto const body = std::string_view{text}.substr(3);
    auto const nl   = body.find('\n');
    if (nl != std::string_view::npos && nl + 1 < body.size()) {
      ctx.out() << body;
      ctx.out().flush();
      return exit_status{0};
    }
  }
  // The child is finished or finishing; collect it when it already is, so a
  // caller that stays alive (a test) does not keep a zombie. Bounded: the
  // child is never waited for.
  for (int tries = 0; tries < 20; ++tries) {
    int status = 0;
    if (::waitpid(child, &status, WNOHANG) != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (text.starts_with("err\n")) {
    auto message = text.substr(4);
    if (!message.ends_with('\n')) {
      message += '\n';
    }
    ctx.err() << message;
    return exit_status{exit_internal_error};
  }
  ctx.err() << "error: queue: no ticket was issued: the detached submitter ended before it reported\n";
  return exit_status{exit_internal_error};
}

/// @brief Steps 4 and 5 of a detached submission, in the child: creates
/// `queue-logs/<seq>.log`, records it on the entry, points standard output and
/// standard error at it, and writes the sequence number and the path to the
/// pipe. Any failure removes the entry (no history row: it never became a
/// run) and the log, and is returned as the message to refuse with.
auto publish_ticket(detach_link& link, db::connection& conn, std::int64_t seq) -> std::expected<void, std::string> {
  link.at("after_insert");
  auto const path = link.log_dir / std::format("{}.log", seq);
  auto const undo = [&](std::string why, bool remove_file) -> std::expected<void, std::string> {
    if (remove_file) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
    static_cast<void>(hq::discard_entry(conn, seq));
    return std::unexpected(std::move(why));
  };

  if (::mkdir(link.log_dir.c_str(), 0700) != 0 && errno != EEXIST) {
    return undo(std::format("cannot create the log directory {}: {}", link.log_dir.string(), errno_text(errno)), false);
  }
  auto const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    // O_EXCL: a log that already exists belongs to an earlier run (the store was
    // recreated and sequence numbers restarted), so it is neither appended to
    // nor removed here; `undo` is told it did not create the file.
    return undo(std::format("cannot create the log file {}: {}", path.string(), errno_text(errno)), false);
  }
  auto const recorded = hq::set_log_path(conn, seq, path.string());
  if (!recorded || !*recorded) {
    ::close(fd);
    return undo(recorded ? std::format("entry {} disappeared before its log was recorded", seq)
                         : std::format("cannot record the log file: {}", recorded.error().message),
                true);
  }
  if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) {
    auto const why = errno_text(errno);
    ::close(fd);
    return undo(std::format("cannot redirect output to the log file {}: {}", path.string(), why), true);
  }
  ::close(fd);
  link.end_capture();
  link.at("before_report");
  if (!write_all(link.write_fd, std::format("ok\n{}\n{}\n", seq, path.string()))) {
    return undo("the invoking process went away before it received the ticket; entry removed", true);
  }
  ::close(link.write_fd);
  link.write_fd = -1;
  link.reported = true;
  return {};
}

auto submit(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps, std::vector<std::string> argv,
            std::int64_t run_limit_ms, std::optional<std::int64_t> wait_limit_ms, detach_link* link) -> handler_outcome;

/// @brief The detached child: becomes the submitter (tech spec 647 §
/// Submitting, With `--detach`, steps 2 to 5). It never returns to its
/// caller's stack, which belongs to the invoked process's copy of `main`: it
/// leaves with `_exit`.
[[noreturn]] void run_detached_child(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps,
                                     std::vector<std::string> argv, std::int64_t run_limit_ms,
                                     std::optional<std::int64_t> wait_limit_ms, int read_fd, int write_fd) {
  ::close(read_fd);
  close_inherited_descriptors(write_fd);

  detach_link link;
  link.write_fd = write_fd;
  link.hook     = deps.detach_hook;
  int code      = exit_internal_error;
  try {
    // Step 2: a new session. The child of a fork is never a process-group
    // leader, so this cannot fail for that reason.
    if (::setsid() < 0) {
      link.begin_capture(ctx.err());
      ctx.err() << std::format("error: queue: cannot start a new session: {}\n", errno_text(errno));
    } else {
      // Standard input is nothing; output is the log once it exists.
      if (auto const null = ::open("/dev/null", O_RDWR); null >= 0) {
        ::dup2(null, STDIN_FILENO);
        if (null > STDERR_FILENO) {
          ::close(null);
        }
      }
      link.begin_capture(ctx.err());
      link.at("after_setsid");
      auto const dir = db::agent::resolve_agent_db_path(ctx.env());
      if (!dir) {
        ctx.err() << std::format("error: queue: {}\n", dir.error().message);
      } else {
        link.log_dir = dir->parent_path() / "queue-logs";
        auto outcome = submit(ctx, args, std::move(deps), std::move(argv), run_limit_ms, wait_limit_ms, &link);
        if (auto const* status = std::get_if<exit_status>(&outcome)) {
          code = status->code;
        }
      }
    }
  } catch (...) {
    // Fall through: nothing may unwind into the invoked process's frames.
  }
  report_failure(link, "the detached submitter failed before it reported");
  link.end_capture();
  ctx.err().flush();
  ::_exit(code);
}

/// @brief The invoked process's half of `--detach`: creates the pipe, forks
/// before any store handle or thread exists, and reads the ticket.
auto detach(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps, std::vector<std::string> argv,
            std::int64_t run_limit_ms, std::optional<std::int64_t> wait_limit_ms) -> handler_outcome {
  if (deps.detach_hook) {
    deps.detach_hook("before_fork");
  }
  int ends[2]{-1, -1};
  if (::pipe(ends) != 0) {
    return refuse(ctx, std::format("cannot detach: {}", errno_text(errno)));
  }
  for (auto const fd : ends) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  ctx.out().flush();
  ctx.err().flush();
  auto const child = ::fork();
  if (child < 0) {
    auto const why = errno_text(errno);
    ::close(ends[0]);
    ::close(ends[1]);
    return refuse(ctx, std::format("cannot detach: {}", why));
  }
  if (child == 0) {
    run_detached_child(ctx, args, std::move(deps), std::move(argv), run_limit_ms, wait_limit_ms, ends[0], ends[1]);
  }
  ::close(ends[1]);
  return await_ticket(ctx, ends[0], child);
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

  // With `--detach` the invoked process forks here, after every refusal it
  // can make on its own and before it opens anything (tech spec 647 §
  // Submitting).
  if (cliapp::flag_bool(args, "--detach")) {
    return detach(ctx, args, std::move(deps), argv, run_limit_ms, wait_limit_ms);
  }
  return submit(ctx, args, std::move(deps), argv, run_limit_ms, wait_limit_ms, nullptr);
}

namespace {

/// @brief Everything `queue run` does once the command has been checked: the
/// configuration, the store, the entry, the wait, the run and the end. The
/// foreground form calls it in the invoked process; the detached form calls it
/// in the detached child, with `link` set.
/// @param link The detached child's hold on the ticket pipe, or null in the
/// foreground.
auto submit(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps, std::vector<std::string> argv,
            std::int64_t run_limit_ms, std::optional<std::int64_t> wait_limit_ms, detach_link* link) -> handler_outcome {
  auto const vendor = identity_field(args, "--vendor", ctx.env(), "PLANAR_VENDOR");
  auto const role   = identity_field(args, "--role", ctx.env(), "PLANAR_ROLE");
  notices    notice{ctx.err(), cliapp::flag_bool(args, "--notices")};

  ident::system_clock system_clock;
  ident::clock&       clock     = deps.clock ? *deps.clock : static_cast<ident::clock&>(system_clock);
  auto const          probe     = deps.probe ? *deps.probe : hq::system_process_probe();
  auto const          signaller = deps.signaller ? deps.signaller : hq::system_group_signaller();
  auto const          rejoin_fn = deps.rejoin ? deps.rejoin : queue_run_deps::rejoiner{hq::rejoin};
  auto const          nested_fn = deps.enqueue_nested ? deps.enqueue_nested : queue_run_deps::nested_enqueuer{hq::enqueue_nested};

  // Configuration first: an unusable configuration must refuse before the
  // store is touched. The path is the context's environment's, exactly as
  // `planar config` resolves it; when it cannot be resolved (no HOME and no
  // PLANAR_CONFIG_PATH) the defaults apply.
  auto            load = deps.load_settings ? deps.load_settings : default_settings_loader(ctx);
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

  // The slot marker (tech spec 647 § Nested runs, § The slot marker is
  // advisory). A marker that names a live running entry makes this run a
  // nested entry, running at once outside the slot count; any other value,
  // or none, queues the command normally. The check and the insert are one
  // transaction inside the engine.
  //
  // A store that stays busy past its timeout says nothing about the marker,
  // and neither answer is safe to guess: queueing normally would put the
  // command behind the very entry that is waiting for it, and running it
  // unqueued would honour a marker nobody checked. So the insert is retried
  // at the poll interval, for as long as the staleness window a waiting
  // submitter is allowed (the same bound as a poll that cannot complete), and
  // then refused at 125 with the command not run (task 7052).
  auto const                       marker_text = ctx.env()("PLANAR_QUEUE_SLOT");
  auto const                       marker      = marker_text ? parse_slot_marker(*marker_text) : std::nullopt;
  std::optional<hq::nested_result> nested;
  if (marker) {
    std::optional<std::int64_t> busy_since;
    while (true) {
      if (auto const signals = relay->drain(); !signals.empty()) {
        ctx.err() << std::format("error: queue: interrupted by {} before the nested run was admitted; the command was not run\n",
                                 signal_name(signals.front()));
        return exit_status{exit_internal_error};
      }
      auto const at = clock.monotonic_ms();
      if (!at) {
        return refuse(ctx, "cannot read the monotonic clock");
      }
      auto inserted = nested_fn(
          conn, *marker,
          hq::enqueue_request{.host_id        = host,
                              .pid            = pid,
                              .pid_started    = static_cast<std::int64_t>(**started),
                              .cwd            = ctx.cwd().string(),
                              .argv           = argv,
                              .label          = cliapp::flag_string(args, "--label"),
                              .vendor         = vendor,
                              .role           = role,
                              .enqueued_at    = clock.wall_ms(),
                              .refreshed_mono = *at},
          hq::nested_limits{.stale_after_ms = settings.current().stale_after_ms, .run_limit_ms = run_limit_ms}, clock, probe);
      if (inserted) {
        if (inserted->status == hq::nested_status::inserted) {
          nested = *inserted;
        }
        break;
      }
      if (!is_busy_failure(inserted.error())) {
        return refuse(ctx, inserted.error().message);
      }
      if (!busy_since) {
        busy_since = *at;
      } else if (*at - *busy_since > settings.current().stale_after_ms) {
        return refuse(ctx, std::format("the store stayed busy for longer than the staleness window while starting a nested "
                                       "run under entry {}; the command was not run",
                                       *marker));
      }
      settings.reload(ctx.err());
      sleep(std::chrono::milliseconds{settings.current().poll_interval_ms});
    }
  }

  // The command guard and the 126/127 checks ran at the top of this function,
  // before the configuration and the store were touched.
  // The entry this submitter enqueues, and enqueues again when it rejoins the
  // queue: everything but the freshness baseline is the same. The wait limit
  // is one deadline for the whole wait, so a rejoined entry keeps the
  // original's.
  auto const make_request = [&](std::int64_t refreshed_mono) {
    return hq::enqueue_request{
        .host_id            = host,
        .pid                = pid,
        .pid_started        = static_cast<std::int64_t>(**started),
        .cwd                = ctx.cwd().string(),
        .argv               = argv,
        .label              = cliapp::flag_string(args, "--label"),
        .vendor             = vendor,
        .role               = role,
        .enqueued_at        = clock.wall_ms(),
        .refreshed_mono     = refreshed_mono,
        .wait_deadline_mono = wait_limit_ms ? std::optional<std::int64_t>{*now_mono + *wait_limit_ms} : std::nullopt,
        .wait_limit_ms      = wait_limit_ms,
    };
  };
  std::int64_t seq = 0;
  if (nested) {
    seq = nested->seq;
  } else {
    auto enqueued = hq::enqueue(conn, make_request(*now_mono), settings.current().history_days);
    if (!enqueued) {
      return refuse(ctx, enqueued.error().message);
    }
    seq = enqueued->seq;
    for (auto const& failure : enqueued->pruned.log_failures) {
      ctx.err() << std::format("warning: queue: cannot remove pruned log file {}: {}\n", failure.path, failure.message);
    }
  }

  // A detached child now has its sequence number: it creates the log, points
  // its standard streams at it and hands the ticket over (steps 4 and 5). A
  // failure takes the entry back out, without a history row.
  if (link != nullptr) {
    if (auto published = publish_ticket(*link, conn, seq); !published) {
      return refuse(ctx, published.error());
    }
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
  // Returns true when the entry was already gone: `already_gone` (the entry
  // was removed under us) writes nothing, because the process that removed it
  // wrote the row (tech spec 647 § Waiting and claiming a turn).
  auto const end = [&](hq::end_request request) -> bool {
    request.ended_at = clock.wall_ms();
    auto ended       = hq::end_entry(conn, seq, request);
    if (!ended) {
      ctx.err() << std::format("warning: queue: cannot record the end of entry {}: {}\n", seq, ended.error().message);
      return false;
    }
    return *ended == hq::end_result::already_gone;
  };

  // A refusal after the entry exists: the `error: queue:` line, then, with
  // `--notices`, the outcome as the last line (tech spec 647: the submitter's
  // last line names the sequence number and the outcome).
  auto const refuse_entry = [&](const std::string& body, const std::string& outcome) {
    auto refused = refuse(ctx, body);
    notice.line(seq, outcome);
    return refused;
  };

  // A signal that arrives before the command starts removes the entry and
  // runs nothing (tech spec 647 § Signals are forwarded and the entry is
  // always removed). The entry ends as `cancelled`, attributed to this
  // process, and the submitter exits 125.
  auto const interrupted = [&](int sig) {
    end(hq::end_request{.outcome      = hq::history_outcome::cancelled,
                        .cancelled_by = hq::canceller{.vendor = vendor, .role = role, .pid = pid}});
    ctx.err() << std::format("error: queue: entry {} was interrupted by {} before its turn; the command was not run\n", seq,
                             signal_name(sig));
    notice.line(seq, "cancelled before its turn");
    // The command never ran, so this is the queue's cancelled exit (decision
    // 1188), not 128 plus the signal, which is for a command a signal ended.
    return exit_status{exit_internal_error};
  };

  // The waiting submitter's own entry is gone (tech spec 647 § Waiting and
  // claiming a turn): it reads the history row for its sequence number.
  // `abandoned` (reaped while stopped) puts it back at the back of the queue
  // as a new entry recorded as its successor, in one transaction; every other
  // row, or none, means the entry ended by a path this submitter did not take,
  // and it exits 125 without running the command. A submitter rejoins at most
  // `k_max_rejoins` times.
  // @return The exit to make, or `std::nullopt` to go on waiting (it rejoined,
  // or the store was busy and the next poll asks again, within the staleness
  // window).
  int                         rejoins = 0;
  std::optional<std::int64_t> last_position;     // The position last told to `--notices`; reset when the entry is replaced.
  std::optional<std::int64_t> rejoin_busy_since; // Monotonic ms of the first busy failure of a streak.
  auto const                  on_missing_while_waiting = [&]() -> std::optional<handler_outcome> {
    auto const gone = [&](std::optional<hq::history_outcome> outcome) -> handler_outcome {
      if (!outcome) {
        return refuse_entry(
            std::format("entry {} is no longer in the queue and left no history row; the command was not run", seq),
            "ended without a history row");
      }
      if (*outcome == hq::history_outcome::cancelled) {
        return refuse_entry(std::format("entry {} was cancelled; the command was not run", seq), "cancelled");
      }
      if (*outcome == hq::history_outcome::abandoned) {
        return refuse_entry(
            std::format("entry {} was reaped after this submitter had already rejoined the queue {} times; giving "
                        "up, the command was not run",
                        seq, rejoins),
            "abandoned, rejoin limit reached");
      }
      return refuse_entry(
          std::format("entry {} ended as {} without this submitter; the command was not run", seq, hq::to_string(*outcome)),
          std::format("ended as {} without this submitter", hq::to_string(*outcome)));
    };
    auto const now = clock.monotonic_ms();
    if (!now) {
      return refuse_entry("cannot read the monotonic clock", "ended: cannot read the clock");
    }
    // A store failure here is the poll-cannot-complete case of tech spec 647 §
    // Waiting: busy is retried at the poll interval for up to the staleness
    // window, counted from the first failure and cleared by a success (the
    // successful poll that reported the missing entry says nothing about this
    // write); any other failure refuses at once. Nested insert, same rule.
    auto const store_failure = [&](const hq::queue_error& error) -> std::optional<handler_outcome> {
      if (!is_busy_failure(error)) {
        return refuse_entry(
            std::format("cannot rejoin the queue after entry {} went missing: {}; the command was not run", seq, error.message),
            "ended: cannot rejoin the queue");
      }
      if (!rejoin_busy_since) {
        rejoin_busy_since = *now;
      } else if (*now - *rejoin_busy_since > settings.current().stale_after_ms) {
        return refuse_entry(std::format("the store stayed busy for longer than the staleness window while entry {} was being put "
                                        "back in the queue; the command was not run",
                                        seq),
                            "ended: store busy while rejoining");
      }
      report.once(
          std::format("warning: queue: cannot rejoin the queue after entry {} went missing, will retry: {}", seq, error.message));
      return std::nullopt;
    };
    if (rejoins >= k_max_rejoins) {
      auto const row = hq::find_history(conn, seq);
      if (!row) {
        if (auto refused = store_failure(row.error())) {
          return refused;
        }
        return std::nullopt;
      }
      return gone(*row ? std::optional{(*row)->outcome} : std::nullopt);
    }
    auto const rejoined = rejoin_fn(conn, seq, make_request(*now));
    if (!rejoined) {
      if (auto refused = store_failure(rejoined.error())) {
        return refused;
      }
      return std::nullopt;
    }
    rejoin_busy_since.reset();
    if (rejoined->status != hq::rejoin_status::rejoined) {
      return gone(rejoined->outcome);
    }
    ++rejoins;
    report.once(std::format("warning: queue: entry {} was reaped while this submitter was not polling; rejoined the queue as "
                            "entry {}",
                            seq, rejoined->seq));
    seq      = rejoined->seq;
    poll.seq = seq;
    last_position.reset();
    return std::nullopt;
  };

  // --- Waiting for the turn ---------------------------------------------
  std::optional<std::int64_t> failing_since;
  std::optional<std::int64_t> own_deadline;
  if (nested) {
    // A nested entry is inserted running, with its deadline set: it has no
    // turn to wait for.
    own_deadline = nested->deadline_mono;
  }
  while (!nested) {
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
          return refuse_entry(std::format("entry {} could not be polled for longer than the staleness window; giving up", seq),
                              "abandoned, could not be polled");
        }
      }
    } else {
      failing_since.reset();
      if (polled->poll.entry_missing) {
        if (auto const verdict = on_missing_while_waiting()) {
          return *verdict;
        }
      } else if (polled->poll.running) {
        own_deadline = polled->poll.deadline_mono;
        break;
      } else if (notice.enabled()) {
        // Told once, and again whenever the place changes.
        if (auto const place = waiting_position(conn, seq); place && place != last_position) {
          notice.line(seq, std::format("waiting at position {}", *place));
          last_position = place;
        }
      }
    }
    // The wait limit. It is judged after the poll, so an entry whose turn has
    // come at its limit runs rather than being removed.
    if (wait_limit_ms && now && *now >= *now_mono + *wait_limit_ms) {
      end(hq::end_request{.outcome = hq::history_outcome::wait_timeout});
      auto const refused =
          refuse(ctx, std::format("entry {} waited longer than --wait-timeout and was removed; the command was not run", seq));
      notice.line(seq, "removed at its wait limit");
      return refused;
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
  auto started_child = runner::start(
      ctx.env(), argv,
      runner::start_options{.working_directory = ctx.cwd(), .env_name = "PLANAR_QUEUE_SLOT", .env_value = std::to_string(seq)});
  if (!started_child) {
    // The program vanished or lost its permission between the checks and the
    // turn, or could not be started at all.
    auto const code = start_exit_code(started_child.error());
    ctx.err() << std::format("error: queue: cannot start '{}': {}\n", argv.front(), start_failure_text(code));
    // `not_started` is the outcome of a 126 or 127 (tech spec 647); any other
    // start failure ends the entry `abandoned`, and the notice says the same.
    if (code == 127 || code == 126) {
      end(hq::end_request{.outcome = hq::history_outcome::not_started, .exit_code = code});
      notice.line(seq, "not started");
    } else {
      end(hq::end_request{.outcome = hq::history_outcome::abandoned});
      notice.line(seq, "abandoned, could not be started");
    }
    return exit_status{code};
  }
  auto const child = *started_child;
  notice.line(seq, "started");

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
  bool                        entry_gone     = false; // The entry was removed while its command runs.
  bool                        timed_out_here = false; // This submitter set the reason `timeout`.
  std::optional<std::int64_t> next_mark_attempt;      // Earliest monotonic ms to try marking the entry again.
  // A running submitter whose entry was removed (reaped, or ended by another
  // process) keeps supervising its command and exits with what it observed of
  // it; it never rejoins the queue and never runs the command a second time
  // (tech spec 647 § Waiting and claiming a turn). The entry's run limit and
  // its slot went with it. A removal by a stop (`timeout`, `cancelled`) is
  // already accounted for by the stop; any other says so, once.
  auto const entry_removed = [&] {
    entry_gone     = true;
    auto const row = hq::find_history(conn, seq);
    if (row && *row && ((*row)->outcome == hq::history_outcome::cancelled || (*row)->outcome == hq::history_outcome::timeout)) {
      return;
    }
    report.once(std::format("warning: queue: entry {} is no longer in the queue; its command keeps running to its end, and its "
                            "run limit is no longer enforced",
                            seq));
  };
  auto const enforce_run_limit = [&] {
    auto const now = clock.monotonic_ms();
    if (!now) {
      return;
    }
    if (!stopping && !entry_gone && *now >= *own_deadline && (!next_mark_attempt || *now >= *next_mark_attempt)) {
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
      if (begun->status == hq::begin_status::missing) {
        // The entry is gone, and an entry cannot come back under its number.
        // There is nothing to mark, so the run limit cannot be enforced
        // through the queue; see `entry_removed`.
        entry_removed();
      } else if (!stopping) {
        // The entry exists but is not running (not expected while this
        // submitter supervises its command); try again at the poll interval,
        // not at every tick, so a store that keeps saying so is not hit with a
        // write transaction every 20 ms.
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
      notice.line(seq, "abandoned, cannot observe the command's status");
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
      } else if (polled->poll.entry_missing && !entry_gone) {
        entry_removed();
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
    notice.line(seq, "stopped at its run limit");
    return exit_status{exit_run_limit};
  }
  if (reason == hq::stop_reason::cancelled) {
    notice.line(seq, "cancelled");
    return exit_status{exit_internal_error};
  }

  bool const removed_first = final_status.kind == runner::state::signalled
                                 ? end(hq::end_request{.outcome = hq::history_outcome::signaled, .signal = final_status.code})
                                 : end(hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = final_status.code});
  if (removed_first) {
    // Another process ended the entry after the stop reason was read above (a
    // cancel that found the command's group already empty, or a poll). Its
    // history row is the record (decision 1188: `queue status` is
    // authoritative), so report ITS outcome, as the path above does.
    if (auto const row = hq::find_history(conn, seq); row && *row) {
      if ((*row)->outcome == hq::history_outcome::cancelled) {
        notice.line(seq, "cancelled");
        return exit_status{exit_internal_error};
      }
      if ((*row)->outcome == hq::history_outcome::timeout) {
        notice.line(seq, "stopped at its run limit");
        return exit_status{exit_run_limit};
      }
      // Any other outcome (a reaped `abandoned`) keeps the documented notice
      // below: what this submitter observed of its command.
    }
  }
  if (final_status.kind == runner::state::signalled) {
    notice.line(seq, std::format("terminated by signal {}", final_status.code));
  } else {
    notice.line(seq, std::format("exited with code {}", final_status.code));
  }
  return exit_status{status_code(final_status)};
}

} // namespace

// ---------------------------------------------------------------------------
// queue cancel (task hq-queue-cancel)
// ---------------------------------------------------------------------------

namespace {

/// @brief The exit for an entry that has already ended, or never existed:
/// the history row decides which. A history row makes it 6 (the entry ended,
/// and the message names how); no row makes it 1 (no such entry, or one whose
/// history has been pruned).
auto ended_or_unknown(context& ctx, db::connection& conn, std::int64_t seq) -> handler_outcome {
  auto const row = hq::find_history(conn, seq);
  if (!row) {
    return refuse(ctx, std::format("cancel: cannot read the history of entry {}: {}", seq, row.error().message));
  }
  if (!row->has_value()) {
    ctx.err() << std::format("error: queue: cancel: no entry {} is in the queue or its history\n", seq);
    return exit_status{exit_generic_failure};
  }
  ctx.err() << std::format("error: queue: cancel: entry {} has already ended as {}\n", seq, hq::to_string((*row)->outcome));
  return exit_status{exit_precondition_conflict};
}

/// @brief What cancel reports once a stopped entry has left the queue: reads
/// the row that ended it. An entry that ended for another reason than the
/// cancellation (a run limit that was already under way) says so.
auto report_stopped(context& ctx, db::connection& conn, std::int64_t seq, bool killed, bool signalled) -> handler_outcome {
  auto const row = hq::find_history(conn, seq);
  if (!row || !row->has_value()) {
    ctx.err() << std::format("warning: queue: cancel: entry {} left the queue but its history row cannot be read\n", seq);
    return exit_status{exit_success};
  }
  if ((*row)->outcome == hq::history_outcome::cancelled) {
    if (!signalled) {
      // Nothing was sent: the command's group was already empty when cancel
      // looked, so it had exited on its own.
      ctx.out() << std::format("cancelled entry {}: its command had already exited, so no signal was sent\n", seq);
    } else {
      ctx.out() << std::format("cancelled entry {}: its command was stopped{}\n", seq,
                               killed ? " (SIGKILL after the grace period)" : "");
    }
    return exit_status{exit_success};
  }
  if ((*row)->outcome == hq::history_outcome::timeout) {
    ctx.out() << std::format("entry {} was already stopping and ended as {}\n", seq, hq::to_string((*row)->outcome));
    return exit_status{exit_success};
  }
  // The command's own submitter ended the entry with what it observed (its
  // command exited) before it read the marker: the cancellation did not take
  // effect, and the entry has ended.
  ctx.err() << std::format("error: queue: cancel: entry {} ended as {} before the cancellation took effect\n", seq,
                           hq::to_string((*row)->outcome));
  return exit_status{exit_precondition_conflict};
}

} // namespace

auto queue_cancel(context& ctx, const cliapp::parsed_args& args) -> handler_outcome {
  return queue_cancel_with(ctx, args, queue_run_deps{});
}

auto queue_cancel_with(context& ctx, const cliapp::parsed_args& args, queue_run_deps deps) -> handler_outcome {
  auto const numbers = cliapp::positional_strings(args, "seq");
  if (numbers.empty()) {
    ctx.err() << "error: queue: cancel: no entry number given\n";
    return exit_status{exit_generic_failure};
  }
  std::int64_t seq  = 0;
  auto const&  text = numbers.front();
  if (auto const parsed = std::from_chars(text.data(), text.data() + text.size(), seq);
      text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || seq <= 0) {
    ctx.err() << std::format("error: queue: cancel: '{}' is not an entry number (a positive integer)\n", text);
    return exit_status{exit_user_input};
  }

  auto const vendor = identity_field(args, "--vendor", ctx.env(), "PLANAR_VENDOR");
  auto const role   = identity_field(args, "--role", ctx.env(), "PLANAR_ROLE");

  ident::system_clock system_clock;
  ident::clock&       clock     = deps.clock ? *deps.clock : static_cast<ident::clock&>(system_clock);
  auto const          probe     = deps.probe ? *deps.probe : hq::system_process_probe();
  auto const          signaller = deps.signaller ? deps.signaller : hq::system_group_signaller();
  auto const          sleep =
      deps.sleep ? deps.sleep : std::function<void(std::chrono::milliseconds)>{[](auto d) { std::this_thread::sleep_for(d); }};

  settings_source settings{deps.load_settings ? deps.load_settings : default_settings_loader(ctx)};
  if (auto first = settings.initial(); !first) {
    return refuse(ctx, describe(first.error()));
  }
  auto opened = db::agent::open_agent_db(ctx.env());
  if (!opened) {
    return refuse(ctx, opened.error().message);
  }
  db::connection& conn = *opened;

  auto const          host = ident::host_identity(ident::native_identity_source());
  hq::canceller const who{.vendor = vendor, .role = role, .pid = static_cast<std::int64_t>(::getpid())};

  // A waiting entry has no command: remove it.
  auto removed = hq::cancel_waiting(conn, seq, who, clock.wall_ms());
  if (!removed) {
    return refuse(ctx, std::format("cancel: {}", removed.error().message));
  }
  if (removed->status == hq::cancel_waiting_status::removed) {
    ctx.out() << std::format("cancelled entry {}: removed before its turn\n", seq);
    return exit_status{exit_success};
  }
  if (removed->status == hq::cancel_waiting_status::missing) {
    return ended_or_unknown(ctx, conn, seq);
  }

  // A running entry: step one, the marker and SIGTERM after its commit.
  auto begun = hq::begin_terminate(
      conn, hq::begin_terminate_request{.seq = seq, .reason = hq::stop_reason::cancelled, .cancelled_by = who, .host_id = host},
      clock, probe, signaller);
  if (!begun) {
    return refuse(ctx, std::format("cancel: {}", begun.error().message));
  }
  switch (begun->status) {
  case hq::begin_status::missing:
    return ended_or_unknown(ctx, conn, seq);
  case hq::begin_status::not_running:
    return refuse(ctx, std::format("cancel: entry {} changed state while it was being cancelled; try again", seq));
  case hq::begin_status::marked:
  case hq::begin_status::already_terminating:
    break;
  }
  // An entry of another host identity cannot be judged or signalled from here
  // (its process ids mean nothing on this host), whether this call marked it
  // or it was already terminating: refuse at once instead of waiting for a
  // group that `advance_terminations` will never examine.
  if (begun->stored && !hq::same_host(*begun->stored, host)) {
    ctx.err() << std::format("error: queue: cancel: entry {} belongs to another host identity; it is {}, and a poll on "
                             "that host will stop it\n",
                             seq,
                             begun->status == hq::begin_status::marked ? "now marked cancelled" : "already marked terminating");
    return exit_status{exit_internal_error};
  }
  reporter report{ctx.err()};
  bool     term_pending = false;
  bool     signalled    = false; // Whether cancel itself delivered a signal to the command's group.
  if (begun->sigterm) {
    signalled = begun->sigterm->outcome == hq::signal_outcome::sent;
    report_signal_failure(*begun->sigterm, report);
    // The entry's submitter had not yet recorded the command's group: nothing
    // could be signalled, and nothing else will send this SIGTERM.
    term_pending = begun->sigterm->outcome == hq::signal_outcome::no_group;
  }

  // Step two, here and now: advance this entry until its group is empty and it
  // is gone. The overdue SIGKILL is sent by the advance once the entry has been
  // terminating for the grace period. Bounded, so a group that survives
  // SIGKILL cannot hold cancel for ever.
  auto const grace_ms = settings.current().grace_ms;
  auto const started  = clock.monotonic_ms();
  bool       killed   = false;
  if (!started) {
    return refuse(ctx, "cannot read the monotonic clock");
  }
  while (true) {
    auto advanced = hq::advance_terminations(conn, hq::advance_request{.host_id = host, .grace_ms = grace_ms, .seq = seq}, clock,
                                             probe, signaller);
    if (advanced) {
      surface_advance(*advanced, report);
      killed = killed || std::ranges::any_of(advanced->kills,
                                             [](const hq::signal_attempt& k) { return k.outcome == hq::signal_outcome::sent; });
      signalled = signalled || killed;
      if (!advanced->ended.empty()) {
        return report_stopped(ctx, conn, seq, killed, signalled);
      }
    } else {
      report.once(std::format("warning: queue: cannot advance the stop of entry {}: {}", seq, advanced.error().message));
    }
    auto const stored = hq::find(conn, seq);
    if (stored && !stored->has_value()) {
      // Ended by its own submitter, which read the marker.
      return report_stopped(ctx, conn, seq, killed, signalled);
    }
    if (stored && term_pending) {
      if (auto const attempt = hq::signal_child_group(**stored, hq::stop_signal::term, host, probe, signaller);
          attempt.outcome != hq::signal_outcome::no_group) {
        term_pending = false;
        signalled    = signalled || attempt.outcome == hq::signal_outcome::sent;
        report_signal_failure(attempt, report);
      }
    }
    auto const now = clock.monotonic_ms();
    if (now && *now - *started > grace_ms + k_drain_slack_ms) {
      if (term_pending) {
        ctx.err() << std::format("error: queue: cancel: entry {}'s command group was never recorded, so no signal was sent; the "
                                 "entry stays marked and the next poll will end it\n",
                                 seq);
      } else {
        ctx.err() << std::format("error: queue: cancel: entry {}'s process group still has members {}; the entry stays marked "
                                 "and the next poll will end it\n",
                                 seq, killed ? "after SIGKILL" : "and no SIGKILL could be sent");
      }
      return exit_status{exit_internal_error};
    }
    sleep(k_child_tick);
  }
}

} // namespace planar::cmd::agent::handlers
