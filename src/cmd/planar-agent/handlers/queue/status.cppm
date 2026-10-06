/// @file status.cppm
/// @brief `planar.cmd.planar_agent.handlers.queue.status` — the `planar-agent
/// queue status <seq>` handler (plan 1080, task hq-queue-status; tech spec 647
/// § CLI surface).
///
/// `queue status <seq> [--json]` answers what became of a queue entry: from the
/// entry while it exists and from its history row afterwards, following a
/// successor (a reaped waiter that rejoined) to the entry that replaced it. It
/// is the authoritative record of how a command ended (decision 1188): a
/// command's exit code alone is ambiguous, `outcome` is not. The number it
/// takes is the first line of the ticket `queue run --detach` prints.
///
/// ## Read-only
///
/// The store is opened read-only at the SQLite layer
/// (`queue_store`'s `store_access::read_only`), so nothing here can write: no
/// reap, no refresh, no migration, and no store is created when none exists.
/// Liveness is judged by the same rules a poll applies and reported, never acted
/// on. The path is resolved here, not before dispatch (`uses_main_database`
/// exempts the whole `queue` domain), and the version handshake and the queue's
/// own compatibility check run first (tech spec 656 § Store and open path).
///
/// ## Output
///
/// `--json` prints one object with exactly the fields of the tech spec, in its
/// order, `null` where a field does not apply. Without it, the same answer as
/// `key: value` lines, one per field that applies, in the same order; the
/// nested `cancelled_by` reads `vendor=<v> role=<r> pid=<n>` and `argv` is a
/// compact JSON array. A text value that holds a control character, a Unicode
/// format character (bidirectional override, zero-width, line separator), a
/// backslash or a leading double quote is shown double-quoted and escaped, so
/// a quoted value is never mistaken for an unquoted one; a `label`, `vendor` or
/// `role` (and the canceller's) whose escaped form would pass 48 columns is cut
/// and ends with `…`. `--json` is never cut and carries the exact bytes.
///
/// ## Exit status
///
/// The handler returns an `exit_status`, so dispatch adds nothing of its own.
///
/// | Code | Meaning |
/// |---|---|
/// | 0 | The entry was found; the answer is on standard output |
/// | 1 | No entry and no history row has that sequence number (never issued, or its history was pruned), or no sequence number
/// was given (a parse failure) | | 2 | The argument is not a positive integer | | 125 | The queue failed: the store does not
/// exist or cannot be read, `planar.db` is behind this binary, its queue tables are
/// not usable by this binary, or an internal error |
///
/// The `tag` of a 125 is one of `store_unreachable`, `store_unreadable`, `internal`,
/// `schema_version_behind`, `queue_schema_incompatible` (ahead, queue check fails) and
/// `queue_schema_foreign` (equal version, queue check fails); none is the exit-7
/// `schema_version_ahead`.
///
/// An unusable `[queue]` configuration is not a failure (task
/// hq-status-degrade-config): the answer is printed and the exit is 0, with
/// `slots` and `grace_ms` null, liveness judged against the default staleness
/// window, and one `warning: queue status: <reason>` line on standard error.
/// The configuration is read only for an entry still in the queue, so an
/// ended entry's answer never warns.
///
/// A refusal writes `error: queue status: <message>` on standard error and,
/// under `--json`, one object on standard output:
/// `{"error":{"verb":"queue status","tag":"<tag>","message":"<message>"}}`
/// (plus `"seq"` for an unknown number).
module;

export module planar.cmd.planar_agent.handlers.queue.status;

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.process.identity;

namespace planar::cmd::agent::handlers {

/// @brief The seams `queue_status_with` reads instead of the process. Every
/// member left empty takes the production default.
export struct queue_status_deps {
  /// @brief The monotonic and wall clocks; the system clock when null.
  std::shared_ptr<process::identity::clock> clock;
  /// @brief The process queries liveness uses; `system_process_probe()` when empty.
  std::optional<engine::hostqueue::process_probe> probe;
  /// @brief Loads the `[queue]` settings; reads the configuration file the
  /// context's environment names when empty.
  std::function<std::expected<engine::config::queue_settings, engine::config::queue_load_error>()> load_settings;
};

/// @brief `planar-agent queue status <seq> [--json]` with the production defaults.
/// @param ctx The invocation context.
/// @param args The parsed arguments: the `seq` positional and `--json`.
/// @return The exit status described in this module's description.
export auto queue_status(context& ctx, const cliapp::parsed_args& args) -> handler_outcome;

/// @brief `queue_status` with its seams supplied.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param deps The clock, probe and settings loader.
/// @return The exit status described in this module's description.
export auto queue_status_with(context& ctx, const cliapp::parsed_args& args, queue_status_deps deps) -> handler_outcome;

/// @brief Render the existing queue status object as one JSON object with a newline.
/// @param status The typed status snapshot.
/// @return One escaped JSON object and a newline.
export auto queue_status_json(const engine::hostqueue::queue_status& status) -> std::string;

/// @brief Render the existing queue status as stable escaped text lines.
/// @param status The typed status snapshot.
/// @return Stable escaped status lines.
export auto queue_status_text(const engine::hostqueue::queue_status& status) -> std::string;

} // namespace planar::cmd::agent::handlers
