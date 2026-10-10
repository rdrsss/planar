/// @file checks_cli.cpp
/// @brief The CLI and failure-cluster family of the diagnose catalog (see diagnose.cppm).
///
/// One check so far, over `cli_invocations` (plan 1132, task 7379; tech spec 689 § Check catalog):
///
///  - `apply-without-preview` (event, warning): a `spec ingest` invocation with the `--apply` flag
///    and no earlier `spec ingest` invocation without it inside the window. Any exit status counts on
///    both sides: a preview that exited non-zero still ran, and an apply that failed was still
///    attempted. One preview precedes every later apply.
///
/// The capture log is the check's input (`cli_log`). The caller reads `[introspection].cli_log` and
/// passes it in the run request, because this module reads no configuration: off reads `disabled`,
/// an unknown setting `unavailable`. Only `unavailable` makes the outcome `partial`; a log the operator
/// turned off does not. When the log is off the check is not run, so rows an earlier logging period left are never read as
/// evidence.
///
/// Known limit: `args_shape` keeps flag names and only the arity of a positional (`<pos:1>`), so a
/// preview of any plan satisfies an apply of any other. The log carries no plan either, so a plan
/// scope changes the window and nothing else. The check reads `args_shape` only to test for the
/// `--apply` flag; a finding's evidence is the invocation's row reference and its `recorded_at`.

module;

module planar.engine.diagnose;

import std;
import planar.db;
import planar.incident_model;

namespace planar::engine::diagnose::detail {

namespace {

namespace im = planar::incident_model;

/// The capture-log input: what the caller said about `[introspection].cli_log`.
auto cli_log_status(const check_context& ctx) -> std::expected<input_status, db::db_error> {
  if (!ctx.cli_log_enabled) {
    return input_status{.state = im::coverage_state::unavailable, .reason = "cli_log-config-unknown"};
  }
  if (!*ctx.cli_log_enabled) {
    return input_status{.state = im::coverage_state::disabled, .reason = "cli_log-off"};
  }
  return input_status{.state = im::coverage_state::observed, .reason = {}};
}

auto apply_without_preview(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // `?1` is the window start and `?2` its end. `--apply` is a whole flag name: padding with spaces keeps
  // `--apply-removals` from matching. A preview is an earlier `spec ingest` row without the flag that is
  // itself inside the window; ties on `recorded_at` fall back to the row id.
  auto sql = std::string{"select a.id, a.recorded_at from cli_invocations a"
                         " where a.verb_path = 'spec ingest' and instr(' ' || a.args_shape || ' ', ' --apply ') > 0"
                         "   and a.recorded_at >= ?1 and a.recorded_at <= ?2"
                         "   and not exists (select 1 from cli_invocations p"
                         "                   where p.verb_path = 'spec ingest'"
                         "                     and instr(' ' || p.args_shape || ' ', ' --apply ') = 0"
                         "                     and p.recorded_at >= ?1"
                         "                     and (p.recorded_at < a.recorded_at"
                         "                          or (p.recorded_at = a.recorded_at and p.id < a.id)))"
                         " order by a.id"};
  return query_findings(ctx, sql, ctx.window.from, ctx.window.to, [](const db::statement& row) {
    im::finding f;
    f.severity       = im::diagnostic_severity::warning;
    f.primary        = im::entity_ref{.kind = "cli_invocation", .id = row.column_int64(0)};
    f.evidence       = {f.primary};
    f.evidence_times = {row.column_text(1)};
    return f;
  });
}

} // namespace

auto cli_family() -> family {
  family f;
  f.inputs.push_back(input_def{.name = "cli_log", .probe = cli_log_status, .built = true});
  f.checks.push_back(check_def{.id       = "apply-without-preview",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "cli_apply_without_preview",
                               .recovery = "run planar spec ingest <plan> without --apply and read the preview first",
                               .inputs   = {"cli_log"},
                               .built    = true,
                               .evaluate = apply_without_preview});
  return f;
}

} // namespace planar::engine::diagnose::detail
