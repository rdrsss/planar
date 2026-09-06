/// @file follow.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.follow`.
module;

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define PLANAR_WATCH_FOLLOW_KQUEUE 1
#include <sys/event.h>
#include <sys/time.h>
#elif defined(__linux__)
#define PLANAR_WATCH_FOLLOW_INOTIFY 1
#include <poll.h>
#include <sys/inotify.h>
#endif

module planar.cmd.planar_watch.handlers.follow;

import std;
import planar.cmd.planar_watch.context;

namespace planar::cmd::watch::handlers {

namespace {

/// @brief What one `wait_next` call decided.
enum class wake_event : std::uint8_t {
  wal_changed, ///< A filesystem event landed on the `-wal` file; re-query.
  heartbeat,   ///< Timeout elapsed with no notification; re-query anyway.
  interrupted, ///< The wait was interrupted (EINTR); caller checks should_stop.
};

constexpr std::uint64_t k_slice_ns = 100'000'000ULL; // 100ms

/// @brief Cross-platform wake source watching a SQLite `-wal` sibling.
///
/// Single-threaded, one instance per follow loop. See this file's header
/// and `zig/src/engine/runtime/agentactivity/wake.zig` for the backend
/// rationale (kqueue on macOS/BSD, inotify on Linux, degraded elsewhere).
class wake_source {
public:
  explicit wake_source(std::filesystem::path db_path) : wal_path_(db_path.string() + "-wal") {
#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
    kq_ = kqueue();
#elif defined(PLANAR_WATCH_FOLLOW_INOTIFY)
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
  }

  wake_source(const wake_source&)            = delete;
  wake_source& operator=(const wake_source&) = delete;

  ~wake_source() {
    close_all();
  }

  auto wait_next(std::uint64_t timeout_ns) -> wake_event {
#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
    return wait_next_kqueue(timeout_ns);
#elif defined(PLANAR_WATCH_FOLLOW_INOTIFY)
    return wait_next_inotify(timeout_ns);
#else
    return wait_next_degraded(timeout_ns);
#endif
  }

private:
  std::string wal_path_;

#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
  int  kq_            = -1;
  int  wal_fd_        = -1;
  bool fresh_attach_  = false;

  /// @brief Open the `-wal` file (best-effort; missing is fine — SQLite
  /// only creates it on the first write) and register an `EVFILT_VNODE`
  /// watch for writes, extends, and rotation (delete/rename).
  auto try_attach() -> void {
    if (kq_ < 0 || wal_fd_ >= 0)
      return;
#if defined(O_EVTONLY)
    int const fd = ::open(wal_path_.c_str(), O_RDONLY | O_EVTONLY | O_CLOEXEC);
#else
    int const fd = ::open(wal_path_.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0)
      return;
    wal_fd_ = fd;
    struct kevent ev {};
    EV_SET(&ev, static_cast<std::uintptr_t>(wal_fd_), EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, nullptr);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
      ::close(wal_fd_);
      wal_fd_ = -1;
      return;
    }
    // Just transitioned from "not attached" to "attached": a write that
    // landed in the gap between the prior fd's teardown and this
    // registration is invisible to the edge-triggered watch. Surface a
    // synthetic wal_changed so the caller re-queries and catches it (the
    // "attach-gap race", plan 85 t#2623).
    fresh_attach_ = true;
  }

  auto wait_next_kqueue(std::uint64_t timeout_ns) -> wake_event {
    if (kq_ < 0)
      return wait_next_degraded(timeout_ns);
    if (wal_fd_ < 0)
      try_attach();
    if (fresh_attach_) {
      fresh_attach_ = false;
      return wake_event::wal_changed;
    }
    if (wal_fd_ < 0) {
      // Still no `-wal` (DB not yet written to) — fall through to a
      // plain timed wait so we still degrade to a heartbeat cadence.
      struct timespec ts {};
      ts.tv_sec  = static_cast<time_t>(timeout_ns / 1'000'000'000ULL);
      ts.tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000ULL);
      struct kevent out {};
      int const n = kevent(kq_, nullptr, 0, &out, 1, &ts);
      if (n < 0)
        return errno == EINTR ? wake_event::interrupted : wake_event::heartbeat;
      return wake_event::heartbeat;
    }
    struct timespec ts {};
    ts.tv_sec  = static_cast<time_t>(timeout_ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000ULL);
    struct kevent out {};
    int const n = kevent(kq_, nullptr, 0, &out, 1, &ts);
    if (n < 0)
      return errno == EINTR ? wake_event::interrupted : wake_event::heartbeat;
    if (n == 0)
      return wake_event::heartbeat;
    if ((out.fflags & (NOTE_DELETE | NOTE_RENAME)) != 0u) {
      // Rotation: the sibling was unlinked/renamed out from under us
      // (e.g. PRAGMA wal_checkpoint(TRUNCATE)). Drop the stale fd; the
      // next wait_next call re-attaches (and surfaces the synthetic
      // wal_changed for the attach-gap race above).
      ::close(wal_fd_);
      wal_fd_ = -1;
    }
    return wake_event::wal_changed;
  }
#endif

#if defined(PLANAR_WATCH_FOLLOW_INOTIFY)
  int inotify_fd_ = -1;
  int watch_fd_   = -1;

  auto try_attach() -> void {
    if (inotify_fd_ < 0 || watch_fd_ >= 0)
      return;
    int const wd = ::inotify_add_watch(inotify_fd_, wal_path_.c_str(), IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0)
      return;
    watch_fd_     = wd;
    fresh_attach_ = true;
  }

  bool fresh_attach_ = false;

  auto wait_next_inotify(std::uint64_t timeout_ns) -> wake_event {
    if (inotify_fd_ < 0)
      return wait_next_degraded(timeout_ns);
    if (watch_fd_ < 0)
      try_attach();
    if (fresh_attach_) {
      fresh_attach_ = false;
      return wake_event::wal_changed;
    }
    struct pollfd pfd {
      .fd = inotify_fd_, .events = POLLIN, .revents = 0
    };
    int const timeout_ms = static_cast<int>(std::min<std::uint64_t>(timeout_ns / 1'000'000ULL, 1'000'000));
    int const rc         = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0)
      return errno == EINTR ? wake_event::interrupted : wake_event::heartbeat;
    if (rc == 0)
      return wake_event::heartbeat;
    // Drain the event buffer; we don't care about individual event
    // contents, only that something fired.
    alignas(struct inotify_event) char buf[4096];
    bool                              rotated = false;
    for (;;) {
      ssize_t const n = ::read(inotify_fd_, buf, sizeof(buf));
      if (n <= 0)
        break;
      ssize_t off = 0;
      while (off < n) {
        auto const* event = reinterpret_cast<struct inotify_event const*>(&buf[off]);
        if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0u)
          rotated = true;
        off += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
      }
    }
    if (rotated)
      watch_fd_ = -1; // re-attach on the next call (IN_IGNORED already removed it kernel-side)
    return wake_event::wal_changed;
  }
#endif

  auto wait_next_degraded(std::uint64_t timeout_ns) -> wake_event {
    struct timespec ts {};
    ts.tv_sec  = static_cast<time_t>(timeout_ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000ULL);
    if (::nanosleep(&ts, nullptr) < 0 && errno == EINTR)
      return wake_event::interrupted;
    return wake_event::heartbeat;
  }

  auto close_all() -> void {
#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
    if (wal_fd_ >= 0) {
      ::close(wal_fd_);
      wal_fd_ = -1;
    }
    if (kq_ >= 0) {
      ::close(kq_);
      kq_ = -1;
    }
#endif
#if defined(PLANAR_WATCH_FOLLOW_INOTIFY)
    if (watch_fd_ >= 0 && inotify_fd_ >= 0) {
      ::inotify_rm_watch(inotify_fd_, watch_fd_);
      watch_fd_ = -1;
    }
    if (inotify_fd_ >= 0) {
      ::close(inotify_fd_);
      inotify_fd_ = -1;
    }
#endif
  }
};

std::atomic<bool>            g_interrupted{false};
std::optional<wake_source>   g_wake;
bool                         g_wake_init_failed = false;

extern "C" void sigint_handler(int) {
  g_interrupted.store(true, std::memory_order_release);
}

} // namespace

auto parse_duration_ns(std::string_view text) -> std::optional<std::uint64_t> {
  if (text.empty())
    return std::nullopt;
  std::size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    ++i;
  if (i == 0)
    return std::nullopt;
  std::uint64_t num = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + i, num);
  if (ec != std::errc{} || ptr != text.data() + i)
    return std::nullopt;
  auto unit = text.substr(i);
  // Trim ASCII space/tab, matching the oracle's std.mem.trim.
  while (!unit.empty() && (unit.front() == ' ' || unit.front() == '\t'))
    unit.remove_prefix(1);
  while (!unit.empty() && (unit.back() == ' ' || unit.back() == '\t'))
    unit.remove_suffix(1);

  auto const mul_checked = [](std::uint64_t a, std::uint64_t b) -> std::optional<std::uint64_t> {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
      return std::nullopt;
    return a * b;
  };

  constexpr std::uint64_t ns_per_us   = 1'000ULL;
  constexpr std::uint64_t ns_per_ms   = 1'000'000ULL;
  constexpr std::uint64_t ns_per_s    = 1'000'000'000ULL;
  constexpr std::uint64_t ns_per_min  = 60ULL * ns_per_s;
  constexpr std::uint64_t ns_per_hour = 60ULL * ns_per_min;

  if (unit.empty())
    return mul_checked(num, ns_per_s); // bare integer -> seconds
  if (unit == "ns")
    return num;
  if (unit == "us")
    return mul_checked(num, ns_per_us);
  if (unit == "ms")
    return mul_checked(num, ns_per_ms);
  if (unit == "s")
    return mul_checked(num, ns_per_s);
  if (unit == "m")
    return mul_checked(num, ns_per_min);
  if (unit == "h")
    return mul_checked(num, ns_per_hour);
  return std::nullopt;
}

auto interval_or_default(std::optional<std::string> const& text) -> std::uint64_t {
  if (!text.has_value())
    return k_default_interval_ns;
  return parse_duration_ns(*text).value_or(k_default_interval_ns);
}

auto install_sigint_handler() -> void {
  struct sigaction act {};
  act.sa_handler = sigint_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  sigaction(SIGINT, &act, nullptr);
}

auto should_stop() -> bool {
  return g_interrupted.load(std::memory_order_acquire);
}

auto reset_for_testing() -> void {
  g_interrupted.store(false, std::memory_order_release);
  g_wake.reset();
  g_wake_init_failed = false;
}

auto interruptible_sleep(context& ctx, std::uint64_t ns) -> void {
  if (!g_wake.has_value() && !g_wake_init_failed) {
    g_wake.emplace(ctx.db_path());
  }

  std::uint64_t remaining = ns;
  while (remaining > 0) {
    if (should_stop())
      return;
    std::uint64_t const this_slice = std::min(remaining, k_slice_ns);
    auto const           ev        = g_wake->wait_next(this_slice);
    switch (ev) {
    case wake_event::wal_changed:
      static_cast<void>(ctx.refresh_db());
      return;
    case wake_event::interrupted:
      return;
    case wake_event::heartbeat:
      break;
    }
    remaining -= this_slice;
  }
  // Heartbeat-exhausted path: refresh on the way out too, matching the
  // oracle (a WAL rotation can also surface on a pure heartbeat cadence
  // under high writer churn).
  static_cast<void>(ctx.refresh_db());
}

} // namespace planar::cmd::watch::handlers
