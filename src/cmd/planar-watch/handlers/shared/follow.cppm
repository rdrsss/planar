/// @file follow.cppm
/// @brief `planar.cmd.planar_watch.handlers.follow` — the Tier-2 wake loop
/// shared across `planar-watch` verbs that support `--follow` (plan 1006,
/// task 6449).
///
/// Port target: `zig/src/cmd/planar-watch/handlers/follow.zig` and its
/// wake source at `zig/src/engine/runtime/agentactivity/wake.zig`.
///
/// ## Public contract vs. transport
///
/// Per the tech spec's "Live tail / follow implementation" section, the
/// PUBLIC CONTRACT is the JSON event shape, the `--follow` semantics
/// (initial snapshot, then incremental events ordered by occurrence time),
/// and the SIGINT exit-0 behavior. The TRANSPORT — how the loop learns a
/// write landed — is internal: Tier 1 is a fixed sleep-and-poll; Tier 2
/// wakes on a filesystem event against the SQLite `-wal` sibling (kqueue on
/// macOS/BSD, inotify on Linux) and degrades to Tier 1 elsewhere.
/// `--interval` becomes a HEARTBEAT under Tier 2 — the maximum delay before
/// the loop re-queries even without a kernel notification, covering a
/// coalesced or missed wake event.
///
/// ## SIGINT
///
/// `install_sigint_handler` installs a POSIX handler that flips a global
/// atomic; `should_stop` reads it. The poll loop checks between chunked
/// waits (`slice_ns`, 100ms) so SIGINT latency stays bounded regardless of
/// the configured `--interval`. Exiting because the operator typed Ctrl-C
/// is success, not failure — the caller returns normally and the binary
/// exits 0.
///
/// ## WAL-rotation snapshot staleness
///
/// A long-lived strict-read-only SQLite connection can keep a stale
/// snapshot across another process's `PRAGMA wal_checkpoint(TRUNCATE)` —
/// the read-only handle cannot write its read-mark back into the shared
/// SHM segment, so it never re-syncs to the truncated WAL header and
/// already-committed rows stay invisible. `interruptible_sleep` closes and
/// reopens the read-only handle (`context::refresh_db`) before returning,
/// on every path (wake, heartbeat, and interrupted), so the caller's next
/// watermark query always runs against a fresh snapshot. Reproduced from
/// `zig/…/follow.zig`'s `interruptibleSleep` — see plan 85 t#2623.
module;

export module planar.cmd.planar_watch.handlers.follow;

import std;
import planar.cmd.planar_watch.context;

namespace planar::cmd::watch::handlers {

/// @brief Default poll/heartbeat interval: 1 second.
export inline constexpr std::uint64_t k_default_interval_ns = 1'000'000'000ULL;

/// @brief Parse a duration spec into nanoseconds.
///
/// Grammar (matching `zig/vendor/etcli-zig/src/cli/duration.zig`'s
/// `parseNanos`, the oracle's own flag-duration parser):
///   - A bare non-negative integer with no suffix is interpreted as
///     SECONDS (human ergonomics): "1" -> 1s, "600" -> 600s.
///   - `<uint><unit>` where unit is one of `ns`, `us`, `ms`, `s`, `m`, `h`.
///   - Anything else (empty, no digits, unknown unit, trailing garbage,
///     negative, or an overflow of u64 nanoseconds) is malformed.
/// @param text The flag value.
/// @return The nanosecond value, or `std::nullopt` on malformed input.
export auto parse_duration_ns(std::string_view text) -> std::optional<std::uint64_t>;

/// @brief Parse `text` (a `--interval` flag value) into nanoseconds,
/// falling back to `k_default_interval_ns` on missing or malformed input.
///
/// Matches the oracle's `ps.parseIntervalOrDefault`: a malformed
/// `--interval` is silently ignored, not a CLI error — reproduced
/// verbatim under D2 rather than "fixed" into a validation failure.
/// @param text The flag value, or `std::nullopt` if the flag was absent.
/// @return The resolved interval in nanoseconds.
export auto interval_or_default(std::optional<std::string> const& text) -> std::uint64_t;

/// @brief Install a SIGINT handler that flips the module-global
/// "should stop" flag. POSIX-only; best-effort (a failure to install is
/// not fatal — the operator can still `kill` the process).
export auto install_sigint_handler() -> void;

/// @brief True once SIGINT has been received and the follow loop should
/// exit cleanly.
/// @return Whether the loop should stop.
export auto should_stop() -> bool;

/// @brief Test-only: clear the "should stop" flag and drop the lazily
/// created wake source so successive test cases don't observe each
/// other's signal / fd state.
export auto reset_for_testing() -> void;

/// @brief Wait up to `ns` nanoseconds for a `-wal` change, returning early
/// if `should_stop()` flips. Refreshes `ctx`'s read-only DB handle before
/// returning on every path so the caller's next query sees a fresh
/// snapshot (see this file's header, "WAL-rotation snapshot staleness").
/// @param ctx The invocation context; also the source of the DB path the
/// wake source watches.
/// @param ns The heartbeat/timeout budget, in nanoseconds.
export auto interruptible_sleep(context& ctx, std::uint64_t ns) -> void;

} // namespace planar::cmd::watch::handlers
