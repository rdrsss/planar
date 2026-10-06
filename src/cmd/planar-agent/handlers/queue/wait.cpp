/// @file wait.cpp
/// @brief Implementation of bounded read-only queue observation.
module;
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

module planar.cmd.planar_agent.handlers.queue.wait;
import std;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.queue_store;
import planar.cmd.planar_agent.handlers.queue.status;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.json_text;
import planar.process.identity;

namespace planar::cmd::agent::handlers {
namespace {
namespace hq    = engine::hostqueue;
namespace qcfg  = engine::config;
namespace ident = process::identity;

constexpr std::int64_t k_default_timeout_ms = 30 * 60 * 1000;
std::atomic<int>       g_wait_write_fd{-1};
std::atomic<unsigned>  g_wait_handler_active{0};
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);

void wait_signal_handler(int sig) {
  auto const saved = errno;
  g_wait_handler_active.fetch_add(1);
  auto const fd = g_wait_write_fd.load();
  if (fd >= 0) {
    unsigned char byte = static_cast<unsigned char>(sig);
    static_cast<void>(::write(fd, &byte, 1));
  }
  g_wait_handler_active.fetch_sub(1);
  errno = saved;
}

/// @brief Scoped observer-only signal notification; teardown blocks delivery
/// while restoring dispositions and closing the pipe.
class wait_signal_relay {
  int                                 _read  = -1;
  int                                 _write = -1;
  std::array<struct sigaction, 2>     _previous{};
  std::array<bool, 2>                 _installed{};
  int                                 _pending = 0;
  static constexpr std::array<int, 2> signals{SIGINT, SIGTERM};

  wait_signal_relay() = default;

public:
  wait_signal_relay(const wait_signal_relay&)                    = delete;
  auto operator=(const wait_signal_relay&) -> wait_signal_relay& = delete;
  ~wait_signal_relay() {
    sigset_t blocked{}, old{};
    sigemptyset(&blocked);
    for (auto sig : signals)
      sigaddset(&blocked, sig);
    auto const masked = ::sigprocmask(SIG_BLOCK, &blocked, &old) == 0;
    g_wait_write_fd.store(-1);
    for (std::size_t i = 0; i < signals.size(); ++i) {
      if (_installed[i])
        ::sigaction(signals[i], &_previous[i], nullptr);
    }
    while (g_wait_handler_active.load() != 0)
      std::this_thread::yield();
    if (_read >= 0)
      ::close(_read);
    if (_write >= 0)
      ::close(_write);
    if (masked)
      ::sigprocmask(SIG_SETMASK, &old, nullptr);
  }

  static auto open() -> std::unique_ptr<wait_signal_relay> {
    auto relay = std::unique_ptr<wait_signal_relay>(new wait_signal_relay());
    int  ends[2]{-1, -1};
    if (::pipe(ends) != 0)
      return nullptr;
    relay->_read  = ends[0];
    relay->_write = ends[1];
    for (auto fd : ends) {
      auto const flags = ::fcntl(fd, F_GETFL);
      if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return nullptr;
      }
    }
    sigset_t blocked{}, old{};
    sigemptyset(&blocked);
    for (auto sig : signals)
      sigaddset(&blocked, sig);
    if (::sigprocmask(SIG_BLOCK, &blocked, &old) != 0)
      return nullptr;
    g_wait_write_fd.store(relay->_write);
    bool ok = true;
    for (std::size_t i = 0; i < signals.size(); ++i) {
      struct sigaction current{};
      if (::sigaction(signals[i], nullptr, &current) != 0) {
        ok = false;
        break;
      }
      if (current.sa_handler == SIG_IGN)
        continue;
      struct sigaction ours{};
      ours.sa_handler = wait_signal_handler;
      ours.sa_flags   = 0;
      sigfillset(&ours.sa_mask);
      if (::sigaction(signals[i], &ours, &relay->_previous[i]) != 0) {
        ok = false;
        break;
      }
      relay->_installed[i] = true;
    }
    ::sigprocmask(SIG_SETMASK, &old, nullptr);
    if (!ok)
      return nullptr;
    return relay;
  }

  auto pending_signal() -> std::optional<int> {
    unsigned char buffer[64];
    while (true) {
      auto n = ::read(_read, buffer, sizeof buffer);
      if (n <= 0)
        break;
      if (_pending == 0)
        _pending = buffer[0];
    }
    if (_pending)
      return _pending;
    return std::nullopt;
  }

  void sleep(std::chrono::milliseconds span) {
    struct pollfd pfd{.fd = _read, .events = POLLIN, .revents = 0};
    static_cast<void>(::poll(&pfd, 1, static_cast<int>(span.count())));
  }
};

auto parse_seq(std::string_view text) -> std::optional<std::int64_t> {
  if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  std::int64_t value = 0;
  auto         done  = std::from_chars(text.data(), text.data() + text.size(), value);
  if (done.ec != std::errc{} || done.ptr != text.data() + text.size() || value <= 0)
    return std::nullopt;
  return value;
}

auto reason_name(hq::wait_reason reason) -> std::string_view {
  switch (reason) {
  case hq::wait_reason::completed:
    return "completed";
  case hq::wait_reason::timed_out:
    return "timed_out";
  case hq::wait_reason::interrupted:
    return "interrupted";
  case hq::wait_reason::stalled:
    return "stalled";
  case hq::wait_reason::history_unavailable:
    return "history_unavailable";
  case hq::wait_reason::error:
    return "error";
  case hq::wait_reason::pending:
    return "error";
  }
  return "error";
}

auto render_result(context& ctx, bool as_json, std::optional<std::int64_t> seq, std::optional<std::int64_t> timeout,
                   const hq::wait_result& result, std::optional<store_refusal> refusal = std::nullopt) -> handler_outcome {
  auto const                 reason   = reason_name(result.reason);
  auto const                 observed = result.snapshot ? result.snapshot->observed_seq : std::nullopt;
  auto const                 status   = result.snapshot ? result.snapshot->status : std::nullopt;
  auto const                 code     = result.result_exit_code.value_or(125);
  std::optional<std::string> tag      = refusal ? std::optional{refusal->tag} : result.tag;
  std::optional<std::string> message  = refusal ? std::optional{refusal->message} : std::nullopt;
  if (!message && result.error)
    message = result.error->message;
  if (!message && tag && reason != "completed") {
    if (reason == "history_unavailable")
      message = std::format("queue history unavailable ({})", *tag);
    else if (reason == "stalled")
      message = "the active entry was confirmed dead; job outcome is unknown; re-observation is safe";
    else
      message = std::format("queue observation failed ({})", *tag);
  }
  if (reason == "error" || reason == "history_unavailable" || reason == "stalled") {
    ctx.err() << std::format("error: queue wait: {}\n", message.value_or("observation failed"));
  }
  if (as_json) {
    auto        number = [](std::optional<std::int64_t> n) { return n ? std::to_string(*n) : std::string{"null"}; };
    std::string out = std::format("{{\"seq\":{},\"observed_seq\":{},\"wait_reason\":{},\"elapsed_ms\":{},\"timeout_ms\":{},"
                                  "\"result_exit_code\":{},\"status\":",
                                  number(seq), number(observed), json_text::json_string(reason),
                                  seq ? number(result.elapsed_ms) : "null", number(timeout), seq ? std::to_string(code) : "null");
    if (status) {
      auto status_json = queue_status_json(*status);
      status_json.pop_back();
      out += status_json;
    } else
      out += "null";
    out += ",\"error\":";
    if (tag)
      out += std::format("{{\"verb\":\"queue wait\",\"tag\":{},\"message\":{}}}", json_text::json_string(*tag),
                         json_text::json_string(message.value_or("")));
    else
      out += "null";
    ctx.out() << out << "}\n";
  } else {
    if (seq)
      ctx.out() << "seq: " << *seq << '\n';
    if (observed)
      ctx.out() << "observed_seq: " << *observed << '\n';
    ctx.out() << "wait_reason: " << reason << '\n';
    if (seq)
      ctx.out() << "elapsed_ms: " << result.elapsed_ms << '\n';
    if (timeout)
      ctx.out() << "timeout_ms: " << *timeout << '\n';
    ctx.out() << "result_exit_code: " << code << '\n';
    if (status)
      ctx.out() << queue_status_text(*status);
    if (tag)
      ctx.out() << "error_tag: " << *tag << '\n';
  }
  return exit_status{code};
}

auto monotonic(ident::clock& clock) -> std::optional<std::int64_t> {
  auto now = clock.monotonic_ms();
  return now ? std::optional{*now} : std::nullopt;
}

} // namespace

auto queue_wait(context& ctx, const cliapp::parsed_args& args) -> handler_outcome {
  bool const                  as_json     = cliapp::flag_bool(args, "--json");
  auto const                  raw_seq     = cliapp::positional_string(args, "seq");
  auto const                  seq         = raw_seq ? parse_seq(*raw_seq) : std::nullopt;
  auto const                  raw_timeout = cliapp::flag_string(args, "--timeout");
  std::optional<std::int64_t> timeout     = k_default_timeout_ms;
  if (raw_timeout) {
    auto parsed = qcfg::parse_duration_flag(*raw_timeout);
    if (!parsed)
      timeout.reset();
    else
      timeout = *parsed;
  }
  if (!seq || !timeout) {
    hq::wait_result invalid{.reason = hq::wait_reason::error, .result_exit_code = 2, .tag = "invalid_input"};
    return render_result(ctx, as_json, seq, timeout, invalid,
                         store_refusal{.tag     = "invalid_input",
                                       .message = !seq ? "expected a positive ASCII sequence number within int64"
                                                       : "expected a positive duration with ms, s, m or h, at most 24h"});
  }
  ident::system_clock clock;
  auto                started = monotonic(clock);
  if (!started) {
    hq::wait_result failed{.reason = hq::wait_reason::error, .result_exit_code = 125, .tag = "clock_unavailable"};
    return render_result(ctx, as_json, seq, timeout, failed);
  }
  auto deadline = hq::checked_wait_deadline(*timeout, *started);
  if (!deadline) {
    hq::wait_result failed{.reason = hq::wait_reason::error, .result_exit_code = 125, .tag = std::string{deadline.error()}};
    return render_result(ctx, as_json, seq, timeout, failed);
  }
  auto relay = wait_signal_relay::open();
  if (!relay) {
    hq::wait_result failed{.reason = hq::wait_reason::error, .result_exit_code = 125, .tag = "signal_setup_failed"};
    return render_result(ctx, as_json, seq, timeout, failed);
  }
  auto budget = [&]() -> std::expected<int, store_refusal> {
    if (auto sig = relay->pending_signal())
      return std::unexpected(store_refusal{"interrupted", std::format("observation interrupted by signal {}", *sig)});
    auto now = monotonic(clock);
    if (!now)
      return std::unexpected(store_refusal{"clock_unavailable", "cannot read the monotonic clock"});
    if (*now < *started)
      return std::unexpected(store_refusal{"clock_backwards", "monotonic clock moved backwards"});
    if (*now >= *deadline)
      return std::unexpected(store_refusal{"timed_out", "observation deadline expired"});
    return static_cast<int>(std::min<std::int64_t>(*deadline - *now, std::numeric_limits<int>::max()));
  };
  // `opened` is declared after `budget` and `relay`: its SQLite busy callback
  // is destroyed before either capture can be released.
  auto opened = open_queue_store_for_wait(ctx.env(), budget);
  if (!opened) {
    hq::wait_result failed{.reason = hq::wait_reason::error, .result_exit_code = 125, .tag = opened.error().tag};
    if (opened.error().tag == "timed_out") {
      failed.reason           = hq::wait_reason::timed_out;
      failed.result_exit_code = 124;
    }
    if (opened.error().tag == "interrupted") {
      failed.reason           = hq::wait_reason::interrupted;
      failed.result_exit_code = 128 + relay->pending_signal().value_or(SIGINT);
    }
    if (auto now = monotonic(clock); now && *now >= *started)
      failed.elapsed_ms = *now - *started;
    return render_result(ctx, as_json, seq, timeout, failed, opened.error());
  }
  bool             warned = false;
  auto const       probe  = hq::system_process_probe();
  auto const       host   = ident::host_identity(ident::native_identity_source());
  hq::wait_runtime runtime{
      .now_ms             = [&]() { return monotonic(clock); },
      .interrupted_signal = [&]() { return relay->pending_signal(); },
      .read               = [&](const std::function<std::expected<int, hq::status_error>()>& check)
          -> std::expected<hq::wait_status_lookup, hq::status_error> {
        auto now = monotonic(clock);
        if (!now)
          return std::unexpected(hq::status_error{.message = "cannot read the monotonic clock"});
        hq::status_request request{
            .host_id  = host,
            .now_mono = *now,
            .now_wall = clock.wall_ms(),
            .settings = [&]() -> std::expected<hq::status_settings, std::string> {
              auto path     = internal::resolve_config_path(ctx.env());
              auto settings = path ? qcfg::load_queue_settings(*path)
                                   : std::expected<qcfg::queue_settings, qcfg::queue_load_error>{qcfg::default_queue_settings()};
              if (!settings) {
                if (!warned) {
                  ctx.err() << "warning: queue wait: the [queue] configuration cannot be used; slots and grace_ms are unknown "
                               "and liveness uses the default staleness window\n";
                  warned = true;
                }
                return hq::status_settings{.slots          = std::nullopt,
                                           .stale_after_ms = qcfg::default_queue_settings().stale_after_ms,
                                           .grace_ms       = std::nullopt};
              }
              return hq::status_settings{
                  .slots = settings->slots, .stale_after_ms = settings->stale_after_ms, .grace_ms = settings->grace_ms};
            },
            .probe = probe};
        return hq::query_wait_status(opened->conn, *seq, request, [&]() -> std::expected<void, hq::status_error> {
          auto allowance = check();
          if (!allowance)
            return std::unexpected(allowance.error());
          return {};
        });
      },
      .sleep = [&](std::chrono::milliseconds span) { relay->sleep(span); },
  };
  auto result = hq::observe_wait(*timeout, runtime, *started);
  return render_result(ctx, as_json, seq, timeout, result);
}
} // namespace planar::cmd::agent::handlers
