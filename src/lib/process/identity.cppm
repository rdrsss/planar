/// @file identity.cppm
/// @brief `planar.process.identity` — process and host identity primitives
/// for the host build and test queue (plan 1080, task 6998).
///
/// The queue engine (`src/engine/hostqueue/`) has to decide whether the
/// process that owns a queue entry is still alive, without a daemon and
/// possibly hours after that process was last seen. Everything it needs for
/// that judgement is here, and nothing else is:
///
///   * **Process start time** — an opaque value that changes when the
///     operating system reuses a process id, so a recorded pid plus its
///     recorded start time identifies one process incarnation. Absence of
///     the process is a value (`std::nullopt`), never an error.
///   * **Host identity** — a value that distinguishes one process-id
///     namespace from another, so a pid is only ever tested by a process
///     that can see it. When it cannot be read it is the literal `unknown`,
///     which compares unequal to every real identity and makes the engine
///     fall back to freshness alone. The source is injectable so a test can
///     supply an unreadable one.
///   * **Process and group existence** — `kill(pid, 0)` and `kill(-pgid,
///     0)`, with success and `EPERM` both meaning "exists". The errno to
///     verdict mapping is a pure function so it can be driven with every
///     errno even when the suite runs as root and `EPERM` cannot happen.
///   * **Signalling a process group** — `kill(-pgid, sig)` behind the
///     module's own error enum.
///   * **Clocks** — a monotonic millisecond clock that does not advance
///     while the host is asleep (decision 1195), and a wall-clock
///     millisecond helper for display values, behind an interface the
///     engine can be handed a fake of.
///
/// Every fallible operation returns `std::expected<T, error>`. No errno and
/// no exception crosses this boundary. The one deliberately errno-shaped
/// entry point is `exists_from_errno`, which exists precisely so the
/// mapping is testable; production callers reach it only through
/// `process_exists` and `group_has_members`.
///
/// Ids are taken as `std::int64_t`. An id that is not positive, or that a
/// platform `pid_t` cannot hold, names no process: the queries report
/// absence and `signal_group` reports `no_such_process`, with no system call
/// made. Group ids 0 and 1 are refused the same way, so no call here can
/// reach `kill(0, ...)` (the caller's own group) or `kill(-1, ...)` (every
/// process the user may signal).
///
/// The platform tables come from the tech spec (artifact 647, § Liveness,
/// precisely) and are restated on each declaration below.
module;

export module planar.process.identity;

import std;

namespace planar::process::identity {

/// @brief What an operation in this module can fail with.
///
/// Absence of a process is never an error: the query functions report it
/// as a value. These are the failures that remain once absence has been
/// taken out.
export enum class error : std::uint8_t {
  no_such_process, ///< A signal was sent to a group that has no member (`ESRCH`).
  not_permitted,   ///< A signal was refused by the kernel (`EPERM`).
  invalid_signal,  ///< The signal number is not valid on this host (`EINVAL`).
  query_failed,    ///< A start time or existence query failed for a reason other than absence.
  clock_failed,    ///< The monotonic clock could not be read.
};

/// @brief A process start time, as an opaque value.
///
/// The value is meaningful only for equality against another value read on
/// the same host for the same pid; nothing may interpret its units.
///
/// | Platform | Source | Value |
/// |---|---|---|
/// | macOS | `proc_pidinfo` with `PROC_PIDTBSDINFO` | `pbi_start_tvsec` and `pbi_start_tvusec`, as microseconds |
/// | Linux | `/proc/<pid>/stat`, field 22 | `starttime`, in clock ticks since boot |
export using start_time = std::uint64_t;

/// @brief Read the start time of the process with id `pid`.
///
/// Documented limit: on Linux with `/proc` mounted `hidepid=1` or
/// `hidepid=2`, another user's live process has no readable entry, so this
/// reports it absent while `process_exists` reports that it exists (`kill`
/// fails with `EPERM`). A rule of "exists and start time matches" reads such
/// a process as dead. Queue entries are normally the checking user's own,
/// where `hidepid` hides nothing.
/// @param pid The process id to look up.
/// @return The start time; `std::nullopt` when no process with that id
/// exists; an `error` only when the query itself failed.
export auto process_start_time(std::int64_t pid) -> std::expected<std::optional<start_time>, error>;

/// @brief The pure errno-to-verdict mapping behind `process_exists` and
/// `group_has_members`.
///
/// Given the `errno` a `kill(..., 0)` call left behind, or `0` when the call
/// succeeded, decides whether the target exists. Success and `EPERM` both
/// mean it exists (the kernel refuses to signal a process it can see);
/// `ESRCH` means it is absent. Any other value is a query failure. This is
/// exported so the mapping can be tested with every input regardless of
/// which of them the running host can produce.
/// @param err The errno value, or `0` for success.
/// @return `true` when the target exists, `false` when it is absent, or
/// `error::query_failed` for an errno the mapping does not know.
export auto exists_from_errno(int err) -> std::expected<bool, error>;

/// @brief Whether a process with id `pid` exists, by `kill(pid, 0)`.
/// @param pid The process id to test.
/// @return `true` when the process exists (including when it may not be
/// signalled), `false` when it is absent.
export auto process_exists(std::int64_t pid) -> std::expected<bool, error>;

/// @brief Whether the process group `pgid` has at least one member, by
/// `kill(-pgid, 0)`.
/// @param pgid The process group id to test.
/// @return `true` when any member exists, `false` when the group is empty.
export auto group_has_members(std::int64_t pgid) -> std::expected<bool, error>;

/// @brief Send `sig` to every member of the process group `pgid`, by
/// `kill(-pgid, sig)`.
/// @param pgid The process group id.
/// @param sig The signal number, as the platform defines it (`SIGTERM`,
/// `SIGKILL`, ...).
/// @return Success, or the module's error for the refusal.
export auto signal_group(std::int64_t pgid, int sig) -> std::expected<void, error>;

/// @brief The host identity used when the real one cannot be read.
export constexpr std::string_view k_unknown_host_identity = "unknown";

/// @brief A source of the raw host identity string.
///
/// Returns the identity, or `std::nullopt` when it cannot be read. An empty
/// string counts as unreadable.
export using identity_source = std::function<std::optional<std::string>()>;

/// @brief The platform's own identity source.
///
/// | Platform | Host identity |
/// |---|---|
/// | Linux | The boot id from `/proc/sys/kernel/random/boot_id`, joined by `:` with the target of `/proc/self/ns/pid` |
/// | macOS | `sysctl kern.bootsessionuuid` |
///
/// Both halves must be readable on Linux; when either is not, the source
/// reports unreadable.
/// @return A source reading the tables above.
export auto native_identity_source() -> identity_source;

/// @brief The host identity as the queue records it.
/// @param source Where to read it from; `native_identity_source()` in
/// production.
/// @return What `source` returned, or `k_unknown_host_identity` when it
/// could not be read.
export auto host_identity(const identity_source& source) -> std::string;

/// @brief The pair of clocks the queue engine compares and displays with.
///
/// `monotonic_ms` is the only clock the engine compares. `wall_ms` is for
/// values a person reads. The engine is handed a `clock&` so tests can
/// drive time by hand.
export class clock {
public:
  clock()                                = default;
  clock(const clock&)                    = default;
  clock(clock&&)                         = default;
  auto operator=(const clock&) -> clock& = default;
  auto operator=(clock&&) -> clock&      = default;
  virtual ~clock()                       = default;

  /// @brief Milliseconds from a clock that does not move when the wall
  /// clock is set and does not advance while the host is asleep.
  /// @return Milliseconds since an arbitrary fixed point, or `clock_failed`.
  [[nodiscard]] virtual auto monotonic_ms() -> std::expected<std::int64_t, error> = 0;

  /// @brief Milliseconds since the Unix epoch, for display only.
  /// @return Wall-clock milliseconds.
  [[nodiscard]] virtual auto wall_ms() -> std::int64_t = 0;
};

/// @brief The host's own clocks.
///
/// | Platform | Monotonic clock |
/// |---|---|
/// | Linux | `CLOCK_MONOTONIC` |
/// | macOS | `CLOCK_UPTIME_RAW` |
export class system_clock final : public clock {
public:
  /// @brief See `clock::monotonic_ms`.
  /// @return Milliseconds from the platform's sleep-excluding clock.
  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, error> override;

  /// @brief See `clock::wall_ms`.
  /// @return Milliseconds since the Unix epoch.
  [[nodiscard]] auto wall_ms() -> std::int64_t override;
};

} // namespace planar::process::identity
