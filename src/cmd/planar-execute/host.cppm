/// @file host.cppm
/// @brief Ensure a `centuriond` is serving one profile, and the configuration it is given
///        (plan 1033 M2, tasks 6502 and 6710; tech-spec D10/D13).
///
/// `planar-execute` is a CLIENT of the engine (decision 1007/1075): it never
/// links a Centurion execution component and never opens Centurion's database.
/// What it does own is the daemon's LIFECYCLE for a profile — writing the
/// configuration a stock `centuriond` reads, starting one when none is
/// serving, and waiting a bounded time for it to accept.
///
/// Everything here is pure except `ensure_host`, which takes its clock, its
/// spawn and its readiness probe as injected functions so the decision
/// sequence is testable without a daemon: `ensure_host` owns *when* to spawn
/// and *when to give up*, not how to talk gRPC.
///
/// Every fallible boundary returns `std::expected`; no exception crosses it.
export module planar.cmd.planar_execute.host;

import std;
import planar.cmd.planar_execute.profile;

export namespace planar::cmd::execute {

/// @brief Why a host could not be ensured.
enum class host_failure : std::uint8_t {
  state_dir, ///< The profile's state directory could not be created or written.
  lock,      ///< The exclusive startup lock could not be taken, and no host was serving.
  spawn,     ///< The daemon binary is missing or could not be started.
  occupied,  ///< A live process claims the socket but is not accepting on it.
  readiness, ///< A daemon was started but did not accept within the budget.
};

/// @brief One classified failure, with a diagnostic naming what to fix.
struct host_error {
  host_failure kind_{};  ///< Stable classification.
  std::string  message_; ///< Operator-facing reason.
};

/// @brief How this process reached a serving daemon.
enum class host_origin : std::uint8_t {
  joined,  ///< A daemon was already serving the profile's socket.
  spawned, ///< This call started one and waited for it to accept.
};

/// @brief A daemon serving one profile.
struct host_endpoint {
  std::string           target_;   ///< gRPC target, `unix:<socket>`.
  std::filesystem::path socket_;   ///< The socket the daemon accepts on.
  host_origin           origin_{}; ///< Whether this call started it.
};

/// @brief The filesystem layout one profile's daemon owns.
///
/// Centurion resolves its own `~/.centurion` layout from `$HOME`, so the
/// daemon is started with `HOME` set to the profile's state directory. That
/// is what keeps two profiles' daemons in separate configuration, credential
/// and socket trees without Centurion needing to know Planar exists.
struct host_layout {
  std::filesystem::path home_;      ///< `state_dir`; the daemon's `$HOME`.
  std::filesystem::path centurion_; ///< `<home>/.centurion`.
  std::filesystem::path config_;    ///< `<home>/.centurion/config`, the daemon's configuration.
  std::filesystem::path runtime_;   ///< `<home>/.centurion/runtime`.
  std::filesystem::path socket_;    ///< `<runtime>/centuriond-v1.sock`.
  std::filesystem::path database_;  ///< `<home>/centurion.db`.
  std::filesystem::path lock_;      ///< `<home>/host.lock`, the exclusive startup lock.
  std::filesystem::path log_;       ///< `<home>/centuriond.log`, the spawned daemon's output.
};

/// @brief Derive the layout for one profile. Pure.
/// @param resolved The resolved profile.
/// @return Its layout.
[[nodiscard]] auto layout_for(const profile& resolved) -> host_layout;

/// @brief The loopback TCP port this profile's daemon is given.
///
/// Planar wants only the owner-only Unix listener, but Centurion cannot start
/// one without also starting the loopback TCP adapter (a known limitation in
/// its `library-consumption.md`). Two profiles taking the default port would
/// therefore collide, and the second daemon would fail to start for a reason
/// having nothing to do with the work. The port is derived from the state
/// directory so it is stable per profile and distinct between profiles.
/// @param resolved The resolved profile.
/// @return A port in the 41000–41999 range.
[[nodiscard]] auto loopback_port(const profile& resolved) -> std::uint16_t;

/// @brief The configuration a stock `centuriond` is given for this profile.
///
/// Closed JSON: every key is one Centurion configuration key, and Planar
/// writes no key Centurion does not declare — an unknown key is a startup
/// refusal there, which is the behaviour we want rather than one to work
/// around. `bundles_dir` is what makes the profile's workflows startable at
/// all (Centurion ADR-0055).
/// @param resolved The resolved profile.
/// @return The configuration JSON, newline-terminated.
[[nodiscard]] auto daemon_config_json(const profile& resolved) -> std::string;

/// @brief Create the profile's state tree and write its daemon configuration.
/// @param resolved The resolved profile.
/// @return The layout that was written, or the failure.
[[nodiscard]] auto write_daemon_config(const profile& resolved) -> std::expected<host_layout, host_error>;

/// @brief The daemon's published ownership claim over one profile's socket.
///
/// Centurion writes this record (`<runtime>/host-<digest>.json`) when it
/// becomes ready, and it is the only evidence a client has about WHO owns a
/// socket that is not answering. Its identity fields are also what the
/// compatibility tuple compares (tech-spec D7).
struct endpoint_record {
  std::int64_t pid_{};            ///< The owning process.
  std::string  instance_id_;      ///< Per-instance identity.
  std::string  protocol_version_; ///< Wire contract the owner serves, e.g. `centurion.v1`.
  std::string  socket_target_;    ///< The target the owner published.
  std::string  start_token_;      ///< Process-start attestation; distinguishes a reused pid.
};

/// @brief Read the profile's published endpoint record, when one exists.
/// @param layout The profile's layout.
/// @return The record, or nullopt when absent or unreadable.
[[nodiscard]] auto read_endpoint_record(const host_layout& layout) -> std::optional<endpoint_record>;

/// @brief Injected effects, so the decision sequence is testable without a daemon.
struct host_hooks {
  /// Whether a daemon is accepting on this socket right now.
  std::function<bool(const std::filesystem::path&)> probe_;
  /// Whether a pid is still running; the ownership evidence for a silent socket.
  std::function<bool(std::int64_t)> alive_{};
  /// Ask a pid to terminate; the seam `stop_host` signals through.
  std::function<bool(std::int64_t)> terminate_{};
  /// Start the daemon detached; returns a diagnostic on failure.
  std::function<std::expected<void, std::string>(const host_layout&, const std::filesystem::path&)> spawn_;
  /// Wait between readiness polls.
  std::function<void(std::chrono::milliseconds)> sleep_{};
  /// Now, for the readiness deadline.
  std::function<std::chrono::steady_clock::time_point()> now_{};
};

/// @brief Ensure exactly one `centuriond` serves this profile, and return it.
///
/// The sequence, in this order for a reason:
///
///  1. Probe the socket. A daemon already serving is JOINED without taking the
///     lock. A fast path, not a guarantee: step 2's loser loop reaches the
///     same answer one probe later.
///  2. Take an OS-backed exclusive lock (`flock`) on the profile's lock file.
///     Losing it means another process is starting the same daemon, so this
///     one waits for readiness rather than starting a second.
///  3. Re-probe while holding the lock. The winner of a race between step 1
///     and step 2 has already started one.
///  4. Decide what a silent socket means, from the owner's published record.
///     A record whose pid is still alive is a daemon that owns the socket and
///     is not answering: that is REFUSED (`occupied`), because deleting a live
///     owner's socket would strand it. Only when no live process claims it is
///     the socket removed — under the lock — and a daemon spawned and waited
///     for within the budget.
///
/// @param resolved The resolved profile.
/// @param daemon The installed `centuriond`.
/// @param hooks Injected probe, spawn, clock and sleep.
/// @param budget How long to wait for a spawned daemon to accept.
/// @return The serving endpoint, or the classified failure.
[[nodiscard]] auto ensure_host(const profile& resolved, const std::filesystem::path& daemon, const host_hooks& hooks,
                               std::chrono::milliseconds budget = std::chrono::seconds{30})
    -> std::expected<host_endpoint, host_error>;

/// @brief Where a profile's drain marker lives.
/// @param layout The profile's layout.
/// @return The marker path.
[[nodiscard]] auto draining_marker(const host_layout& layout) -> std::filesystem::path;

/// @brief Whether this profile is draining and must not be submitted to.
/// @param layout The profile's layout.
/// @return True when the marker is present.
[[nodiscard]] auto is_draining(const host_layout& layout) -> bool;

/// @brief Start or end a drain.
///
/// Draining is a PLANAR-side admission gate, not a daemon state. Centurion
/// exposes no "stop admitting" operation, so what Planar can honestly stop is
/// its own submitting: work already running continues, and `host stop` is what
/// ends the daemon (under Centurion's own bounded, draining shutdown).
/// @param layout The profile's layout.
/// @param draining Whether the profile should refuse new submissions.
/// @return Nothing, or a diagnostic.
[[nodiscard]] auto set_draining(const host_layout& layout, bool draining) -> std::expected<void, std::string>;

/// @brief How a stop ended.
enum class stop_result : std::uint8_t {
  not_running, ///< No daemon was serving this profile; nothing to stop.
  stopped,     ///< The daemon was signalled and stopped within the budget.
  timed_out,   ///< The daemon was signalled and had not stopped when the budget expired.
};

/// @brief Ask the profile's daemon to stop, and wait a bounded time for it.
///
/// SIGTERM, because that is the signal `centuriond` bridges to its own
/// shutdown: admission closes, owned work drains, and the process ends under
/// Centurion's bounded-shutdown rules. Planar never kills it outright — a
/// forced stop would abandon exactly the work the drain exists to preserve.
/// @param layout The profile's layout.
/// @param hooks Injected liveness, probe, clock and sleep.
/// @param budget How long to wait for the daemon to go.
/// @return What happened, or the classified failure.
[[nodiscard]] auto stop_host(const host_layout& layout, const host_hooks& hooks,
                             std::chrono::milliseconds budget = std::chrono::seconds{30})
    -> std::expected<stop_result, host_error>;

/// @brief The production hooks: a real connect probe and a detached spawn.
/// @param probe Socket-liveness probe, supplied by the caller that links the client.
/// @return Hooks wired to the real clock, a real sleep and `posix_spawn`.
[[nodiscard]] auto real_hooks(std::function<bool(const std::filesystem::path&)> probe) -> host_hooks;

} // namespace planar::cmd::execute
