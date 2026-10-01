/// @file queue.cppm
/// @brief `planar.cmd.planar_watch.handlers.queue` — `planar-watch queue`
/// (plan 1080, task hq-watch-queue; tech spec 647 § CLI surface).
///
/// Lists every running and waiting entry of the host queue, across every
/// project, read-only, from the `queue_entries` table of `planar.db` (plan
/// 1089, task qp-watch-queue; tech spec 656 § planar-watch). It reads through
/// the same main read-only handle every other viewer verb uses and under its
/// rules: a missing file is the usual open error, and a schema version behind
/// or ahead of this binary is exit 7. The queue has no path or open rule of
/// its own.
///
/// ## Order and position
///
/// Every entry in sequence order, running and waiting alike (test spec 649: rows "in
/// sequence order").
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
/// Submitted values are escaped by `planar.cmd.planar_watch.handlers.queue.render`,
/// shared with `planar-watch queue history`: a value with a control character,
/// DEL, a C1 control, a space or a double quote is written as a double-quoted
/// string with `\n` / `\u00xx` escapes in text, and JSON escapes the same bytes,
/// so no value can start a line of its own or move the cursor. Bidirectional-
/// text and other Unicode format characters are not escaped (task 7082).
///
/// ## Both a verb and a group
///
/// `queue` has a child, `queue history`, and is still a verb of its own:
/// `planar-watch queue` lists, `planar-watch queue history` lists what ended.
/// `planar.cmd.planar_watch.dispatch` runs this handler for the bare group
/// instead of printing its help page.
///
/// ## Exit status
///
/// 0 on success (an empty queue included), 7 for a `planar.db` behind or ahead
/// of this binary (both versions named), 1 for a missing or unreadable
/// database.
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
