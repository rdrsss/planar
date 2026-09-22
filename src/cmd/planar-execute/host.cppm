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

/// @brief Injected effects, so the decision sequence is testable without a daemon.
struct host_hooks {
  /// Whether a daemon is accepting on this socket right now.
  std::function<bool(const std::filesystem::path&)> probe_;
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
///  4. Remove a stale socket, spawn, and wait for the probe to succeed within
///     the budget. A socket file that no daemon accepts on is evidence of a
///     crashed owner, and it is removed only while holding the lock.
///
/// @param resolved The resolved profile.
/// @param daemon The installed `centuriond`.
/// @param hooks Injected probe, spawn, clock and sleep.
/// @param budget How long to wait for a spawned daemon to accept.
/// @return The serving endpoint, or the classified failure.
[[nodiscard]] auto ensure_host(const profile& resolved, const std::filesystem::path& daemon, const host_hooks& hooks,
                               std::chrono::milliseconds budget = std::chrono::seconds{30})
    -> std::expected<host_endpoint, host_error>;

/// @brief The production hooks: a real connect probe and a detached spawn.
/// @param probe Socket-liveness probe, supplied by the caller that links the client.
/// @return Hooks wired to the real clock, a real sleep and `posix_spawn`.
[[nodiscard]] auto real_hooks(std::function<bool(const std::filesystem::path&)> probe) -> host_hooks;

} // namespace planar::cmd::execute
