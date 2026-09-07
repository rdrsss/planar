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
///
/// ## The attach-race fix (task 6450)
///
/// Watching the `-wal` file directly cannot observe a one-shot writer
/// (`planar-agent pull`, etc.): the file is created, written, and then
/// deleted again (SQLite's "delete `-wal`/`-shm` on last-connection-close"
/// behavior — confirmed empirically for this codebase's connection usage
/// pattern) entirely within that single short-lived process's lifetime.
/// Measured: the file exists for roughly the writer's own wall-clock
/// runtime (tens of ms), which is SHORTER than the `k_slice_ns` (100ms)
/// attach-retry sampling grid used by `interruptible_sleep`. Retrying
/// `try_attach()` only at 100ms boundaries therefore has a real (not
/// theoretical) chance of landing entirely outside the file's brief
/// existence window on every single sample, and once the writer exits
/// there is nothing left to attach to until the NEXT write.
///
/// The fix: watch the `-wal`'s PARENT DIRECTORY as well as the file
/// itself. The directory always exists (it is the DB's own directory),
/// so this watch attaches exactly once, at construction, with no race at
/// all. A directory-level filesystem event (`NOTE_WRITE` on the dir fd /
/// `IN_CREATE` on the dir path) fires the instant a sibling is created or
/// removed — including within the narrow window a one-shot writer's
/// `-wal` file lives — because kqueue/inotify queue the event as soon as
/// it happens, not just when we happen to sample. `wait_next` treats any
/// such directory event as `wal_changed` (the caller just re-queries;
/// redundant re-queries are harmless) and opportunistically retries
/// `try_attach()` immediately so future in-process commits by a
/// longer-lived writer are still tracked via the file's own watch.
class wake_source {
public:
  explicit wake_source(std::filesystem::path db_path)
      : wal_path_(db_path.string() + "-wal"),
        dir_path_(db_path.has_parent_path() ? db_path.parent_path() : std::filesystem::path(".")) {
#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
    kq_ = kqueue();
    try_attach_dir();
#elif defined(PLANAR_WATCH_FOLLOW_INOTIFY)
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    try_attach_dir();
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
  std::string           wal_path_;
  std::filesystem::path dir_path_;

#if defined(PLANAR_WATCH_FOLLOW_KQUEUE)
  int  kq_           = -1;
  int  wal_fd_       = -1;
  int  dir_fd_       = -1;
  bool fresh_attach_ = false;

  /// @brief Open the `-wal`'s parent directory (best-effort) and register
  /// an `EVFILT_VNODE` watch for `NOTE_WRITE` — fires the instant a
  /// sibling is created, removed, or renamed within it. This attach can
  /// never race the way the `-wal` file attach can: the directory always
  /// exists (it is the DB's own directory), so it succeeds exactly once,
  /// at construction. See the class doc "The attach-race fix" for why
  /// this is necessary at all.
  auto try_attach_dir() -> void {
    if (kq_ < 0 || dir_fd_ >= 0)
      return;
#if defined(O_EVTONLY)
    int const fd = ::open(dir_path_.c_str(), O_RDONLY | O_EVTONLY | O_CLOEXEC);
#else
    int const fd = ::open(dir_path_.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0)
      return;
    dir_fd_ = fd;
    struct kevent ev{};
    EV_SET(&ev, static_cast<std::uintptr_t>(dir_fd_), EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, nullptr);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
      ::close(dir_fd_);
      dir_fd_ = -1;
    }
  }

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
    struct kevent ev{};
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
    if (dir_fd_ < 0)
      try_attach_dir();
    if (fresh_attach_) {
      fresh_attach_ = false;
      return wake_event::wal_changed;
    }
    // Single wait covers whichever of {wal fd, dir fd} is currently
    // registered on kq_ — kqueue reports an event from ANY filter
    // attached to this kq, so there is no need to branch on wal_fd_'s
    // state here. This is what closes the attach-race: the directory
    // watch is already live (registered at construction), so a `-wal`
    // creation that happens entirely within a one-shot writer's brief
    // lifetime is queued the instant it happens, not just when we
    // happen to sample at a 100ms slice boundary.
    struct timespec ts{};
    ts.tv_sec  = static_cast<time_t>(timeout_ns / 1'000'000'000ULL);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000ULL);
    struct kevent out{};
    int const     n = kevent(kq_, nullptr, 0, &out, 1, &ts);
    if (n < 0)
      return errno == EINTR ? wake_event::interrupted : wake_event::heartbeat;
    if (n == 0)
      return wake_event::heartbeat;
    if (wal_fd_ >= 0 && out.ident == static_cast<std::uintptr_t>(wal_fd_)) {
      if ((out.fflags & (NOTE_DELETE | NOTE_RENAME)) != 0u) {
        // Rotation: the sibling was unlinked/renamed out from under us
        // (e.g. PRAGMA wal_checkpoint(TRUNCATE), or a one-shot writer's
        // own close-time cleanup). Drop the stale fd; the next
        // wait_next call re-attaches (and surfaces the synthetic
        // wal_changed for the attach-gap race above).
        ::close(wal_fd_);
        wal_fd_ = -1;
      }
      return wake_event::wal_changed;
    }
    // Directory-level event (a sibling — almost certainly the `-wal` —
    // was created, removed, or renamed). Retry the file attach right
    // now, while the odds of the file still existing are best; but
    // report wal_changed regardless of whether that attach succeeds —
    // the caller's re-query is the meaningful side effect either way,
    // and a spurious re-query on an unrelated directory write is
    // harmless.
    if (wal_fd_ < 0)
      try_attach();
    if (fresh_attach_)
      fresh_attach_ = false;
    return wake_event::wal_changed;
  }
#endif

#if defined(PLANAR_WATCH_FOLLOW_INOTIFY)
  int inotify_fd_ = -1;
  int watch_fd_   = -1;
  int dir_wd_     = -1;

  // NOTE: this backend is a build-verified, structurally-parallel port of
  // the kqueue fix above (task 6450) — it has NOT been runtime-verified
  // (this repo's oracle/dev host is macOS; there is no Linux CI leg
  // exercising `feed --follow` here). The failure this closes has the
  // same shape on inotify (`watch_fd_` only ever attaches to a `-wal`
  // that may not exist yet, and a one-shot writer can create+delete it
  // faster than the 100ms retry grid samples), and IN_CREATE-on-parent-
  // directory is the textbook inotify remedy for "watch for creation of
  // a file that doesn't exist yet" (see inotify(7)), but this specific
  // path is unexercised by this task's verification.

  /// @brief Register a persistent `IN_CREATE` watch on the `-wal`'s
  /// parent directory. Unlike the `-wal` file watch, this cannot race:
  /// the directory always exists, so this attaches exactly once, at
  /// construction. See the class doc "The attach-race fix".
  auto try_attach_dir() -> void {
    if (inotify_fd_ < 0 || dir_wd_ >= 0)
      return;
    int const wd = ::inotify_add_watch(inotify_fd_, dir_path_.c_str(), IN_CREATE);
    if (wd < 0)
      return;
    dir_wd_ = wd;
  }

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
    if (dir_wd_ < 0)
      try_attach_dir();
    if (fresh_attach_) {
      fresh_attach_ = false;
      return wake_event::wal_changed;
    }
    struct pollfd pfd{.fd = inotify_fd_, .events = POLLIN, .revents = 0};
    int const     timeout_ms = static_cast<int>(std::min<std::uint64_t>(timeout_ns / 1'000'000ULL, 1'000'000));
    int const     rc         = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0)
      return errno == EINTR ? wake_event::interrupted : wake_event::heartbeat;
    if (rc == 0)
      return wake_event::heartbeat;
    // Drain the event buffer. Any event (dir-level create, or the wal
    // file's own watch) is treated as wal_changed below — the caller's
    // re-query is the meaningful side effect either way, and a
    // spurious re-query is harmless. We still inspect the mask so a
    // rotation on the FILE watch clears the stale wd, and a directory
    // IN_CREATE retries the file attach immediately (best odds of
    // catching a one-shot writer's brief `-wal` window).
    alignas(struct inotify_event) char buf[4096];
    bool                               rotated      = false;
    bool                               dir_signaled = false;
    for (;;) {
      ssize_t const n = ::read(inotify_fd_, buf, sizeof(buf));
      if (n <= 0)
        break;
      ssize_t off = 0;
      while (off < n) {
        auto const* event = reinterpret_cast<struct inotify_event const*>(&buf[off]);
        if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0u)
          rotated = true;
        if (dir_wd_ >= 0 && event->wd == dir_wd_ && (event->mask & IN_CREATE) != 0u)
          dir_signaled = true;
        off += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
      }
    }
    if (rotated)
      watch_fd_ = -1; // re-attach on the next call (IN_IGNORED already removed it kernel-side)
    if (dir_signaled && watch_fd_ < 0)
      try_attach(); // best-effort; wal_changed below fires regardless
    return wake_event::wal_changed;
  }
#endif

  auto wait_next_degraded(std::uint64_t timeout_ns) -> wake_event {
    struct timespec ts{};
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
    if (dir_fd_ >= 0) {
      ::close(dir_fd_);
      dir_fd_ = -1;
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
    if (dir_wd_ >= 0 && inotify_fd_ >= 0) {
      ::inotify_rm_watch(inotify_fd_, dir_wd_);
      dir_wd_ = -1;
    }
    if (inotify_fd_ >= 0) {
      ::close(inotify_fd_);
      inotify_fd_ = -1;
    }
#endif
  }
};

std::atomic<bool>          g_interrupted{false};
std::optional<wake_source> g_wake;
bool                       g_wake_init_failed = false;

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
  std::uint64_t num    = 0;
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
  struct sigaction act{};
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
    auto const          ev         = g_wake->wait_next(this_slice);
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
