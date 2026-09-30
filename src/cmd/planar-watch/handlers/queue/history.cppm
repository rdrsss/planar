/// @file history.cppm
/// @brief `planar.cmd.planar_watch.handlers.queue.history` —
/// `planar-watch queue history` (plan 1080, task hq-watch-history; tech spec
/// 647 § CLI surface, § Finishing and history; product spec 646 § What the
/// operator sees).
///
/// Lists the entries that have ended, read-only, from the same agent store
/// `planar-watch queue` reads (`planar.cmd.planar_watch.agentstore`: read-only
/// handle, compatibility check, a missing store is an empty history). The main
/// database is never opened; `uses_main_database` exempts the whole `queue`
/// domain from the main database's path resolution.
///
/// ## Order
///
/// Oldest first: by `ended_at`, then sequence number, the order
/// `hostqueue::list_history` returns and the one a log is read in. Nothing is
/// re-sorted here.
///
/// ## `--since <duration>`
///
/// Only rows that ended within the last `<duration>`: `ended_at` at or after
/// `now - duration` (`since_cutoff_ms`). The value is an integer followed by
/// `ms`, `s`, `m` or `h`, parsed by the same `parse_duration_flag` as
/// `planar-agent queue run --timeout`, so it is greater than zero and at most
/// 24h. Anything else is refused at exit 2 (`invalid_input`) with a message
/// that names the value; nothing is printed on standard output.
///
/// ## Output
///
/// `--json` prints one array, `[]` when empty, of objects with exactly these
/// members in this order: `seq`, `outcome` (`exited`, `signaled`, `timeout`,
/// `cancelled`, `wait_timeout`, `not_started`, `abandoned`), `exit_code`,
/// `signal`, `cancelled_by` (an object with `vendor`, `role` and `pid`, when
/// cancelled), `superseded_by` (the successor's sequence number, when an
/// abandoned waiter re-enqueued; the same name `queue status --json` uses),
/// `nested`, `parent_seq`, `cwd`, `argv` (array), `label`, `vendor`, `role`,
/// `log_path`, `enqueued_at`, `started_at`, `ended_at` (wall-clock ms),
/// `waited_ms`, `ran_ms`, `run_limit_ms`, `wait_limit_ms`. A member that does
/// not apply is null; the two limits are null on a store still at agent schema
/// version 2.
///
/// The text form is a header and one line per row, columns padded to the
/// widest cell: `SEQ OUTCOME RESULT ENDED WAITED RAN NOTES VENDOR ROLE LABEL
/// DIRECTORY COMMAND`. RESULT is `code:<n>`, `signal:<n>` or `-`. ENDED is
/// UTC, `YYYY-MM-DDTHH:MM:SSZ`. NOTES is `-` or a comma-joined list of
/// `cancelled-by:<vendor>/<role>/<pid>`, `superseded-by:<seq>` and
/// `nested:<parent>`. An empty value is `-`. An empty history prints nothing.
/// Escaping is `queue_render`'s, shared with `planar-watch queue`.
///
/// ## Exit status
///
/// 0 on success (an empty or missing store included), 2 for an invalid
/// `--since`, 7 for a store from a newer release, 1 for a file that is not an
/// agent store or cannot be read.
module;

export module planar.cmd.planar_watch.handlers.queue.history;

import std;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief The `ended_at` cutoff a `--since` duration means.
/// @param now_ms The current wall-clock time, ms since the epoch.
/// @param since_ms The duration, ms.
/// @return `now_ms - since_ms`; rows that ended at or after it are listed.
export auto since_cutoff_ms(std::int64_t now_ms, std::int64_t since_ms) -> std::int64_t;

/// @brief Handle `planar-watch queue history`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto queue_history(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
