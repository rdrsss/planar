/// @file queue.cppm
/// @brief `planar.cmd.planar_watch.handlers.queue` — `planar-watch queue`
/// (plan 1080, task hq-watch-queue; tech spec 647 § CLI surface).
///
/// Lists every running and waiting entry of the host queue, across every
/// project, read-only. The store comes from `planar.cmd.planar_watch.agentstore`
/// (read-only handle, compatibility check, a missing store is an empty queue);
/// the main database is never opened, and `uses_main_database` exempts this
/// domain from the main database's path resolution in `main.cpp`.
///
/// ## Order and position
///
/// Running entries first, then waiting entries, each group in sequence order.
/// `position` is the place among the WAITING entries from 1 (a running entry
/// holds a slot and is not in the waiting order) and is null for a running
/// entry.
///
/// ## Liveness, judged and left alone
///
/// Each entry is judged by `hostqueue::judge_liveness`, the rule a poll
/// applies, against the staleness window in the `[queue]` configuration. The
/// judgement is only reported: nothing is reaped, refreshed, marked or
/// removed, and every statement the store runs is a read. When the
/// configuration cannot be used the default window is applied and one
/// `warning: queue: ...` line goes to standard error, the same degradation
/// `planar-agent queue status` makes. A probe that fails leaves `live` null
/// (text `LIVE-UNKNOWN`) rather than guessing.
///
/// ## Output
///
/// `--json` prints one array, `[]` when empty, of objects with exactly these
/// members in this order: `seq`, `state` (`waiting` | `running`), `live`,
/// `position`, `terminating` (the reason while the entry is being stopped),
/// `nested`, `parent_seq`, `cwd`, `argv` (array), `label`, `vendor`, `role`,
/// `log_path`, `enqueued_at`, `started_at` (wall-clock ms), `waited_ms`,
/// `ran_ms`, `run_limit_ms`, `wait_limit_ms`; a member that does not apply
/// is null.
///
/// The text form is a header and one line per entry, columns padded to the
/// widest cell: `SEQ STATE POS NOTES WAITED RAN VENDOR ROLE LABEL DIRECTORY
/// COMMAND`. NOTES is `-` or a comma-joined list of `NOT-LIVE`,
/// `LIVE-UNKNOWN`, `nested:<parent>` and `stopping:<reason>`. An empty value
/// is `-`. The command is the argument vector, each word shell-quoted. An
/// empty queue prints nothing.
///
/// ## Escaping
///
/// A submitted value (argv word, label, vendor, role, directory) may hold any
/// bytes. In text a value containing a control character, DEL, a C1 control,
/// a space or a double quote is written as a double-quoted string with
/// `\n`, `\u00xx` and similar escapes, and a shell-quoted argv word with a
/// control character likewise, so no value can start a line of its own or
/// move the cursor. In JSON the same escapes keep DEL and C1 controls from
/// reaching a terminal raw. Bidirectional-text and other Unicode format
/// characters are not escaped here (task 7082).
///
/// ## Exit status
///
/// 0 on success (an empty queue included), 7 for a store from a newer
/// release (both versions named), 1 for a file that is not an agent store or
/// cannot be read.
module;

export module planar.cmd.planar_watch.handlers.queue;

import std;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch queue`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto queue(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
