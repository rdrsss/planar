/// @file checks_cli.cpp
/// @brief The CLI and failure-cluster family of the diagnose catalog (see diagnose.cppm).
///
/// Three checks (plan 1132, tasks 7379 and 7380; tech spec 689 § Check catalog):
///
///  - `apply-without-preview` (event, warning): a `spec ingest` invocation with the `--apply` flag
///    and no earlier `spec ingest` invocation without it inside the window. Any exit status counts on
///    both sides: a preview that exited non-zero still ran, and an apply that failed was still
///    attempted. One preview precedes every later apply.
///  - `cli-failure-cluster` (event, warning): at least three `cli_invocations` failures inside the
///    window with one `verb_path` and one `error_category`. Fingerprint `cli-failure-cluster|<verb_path>|
///    <error_category>|<scope>`; each invocation is a member, timed by its `recorded_at`. The scope is
///    always `global`: the capture writer stores no `scope_slug` (the column exists and is always null),
///    so no invocation has a scope to share. A `verb_path` enters a fingerprint, so a legacy row written
///    before write-time catalog checking cannot be allowed to carry prose into it. Rows are dropped
///    before counting unless the path has the catalog's shape (at most two tokens, each a lower-case
///    word with digits and hyphens, a digit string, `word:digits` or the writer's `<unknown>`
///    placeholder) and the caller's `verb_path_recognized` predicate, when it supplied one, accepts it.
///    `planar-watch` cannot import the `planar` CLI tree that the predicate reads, so it supplies none:
///    there a legacy row that is word-shaped (for example `acme`) is still counted. That is a known gap,
///    documented in docs/cli-reference.md.
///  - `claim-failure-cluster` (event, warning), over `agent_work_claims`: at least three claims that
///    ended inside the window (by `released_at`) with the same `failure_category` other than `unknown`.
///    Fingerprint `claim-failure-cluster|failure_category=<value>|<scope>`; each claim is a member, timed
///    by its `released_at`. The scope is the members' common entity scope (`repo:<slug>` or
///    `assoc:<slug>`; a plan or plan step contributes its plan's scope), and `global` when they differ or
///    any member has none. It needs no input.
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

/// The failures a cluster needs, per the tech spec: three.
constexpr std::size_t k_cluster_threshold = 3;

/// The shape of a catalog verb path token: a lower-case word of letters, digits and hyphens that starts
/// with a letter, a digit string, `word:digits`, or the writer's `<unknown>` placeholder.
auto catalog_token(std::string_view t) -> bool {
  if (t == "<unknown>") {
    return true;
  }
  auto lower  = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; };
  auto digits = [](std::string_view v) {
    return !v.empty() && std::ranges::all_of(v, [](unsigned char c) { return c >= '0' && c <= '9'; });
  };
  if (auto colon = t.find(':'); colon != std::string_view::npos) {
    auto kind = t.substr(0, colon);
    return !kind.empty() && std::ranges::all_of(kind, lower) && !(kind.front() >= '0' && kind.front() <= '9') &&
           digits(t.substr(colon + 1));
  }
  return digits(t) || (!t.empty() && t.front() >= 'a' && t.front() <= 'z' && std::ranges::all_of(t, lower));
}

/// True when a stored `verb_path` has the shape the capture writer produces: one or two catalog tokens.
auto catalog_shaped(std::string_view verb_path) -> bool {
  auto space = verb_path.find(' ');
  if (space == std::string_view::npos) {
    return catalog_token(verb_path);
  }
  return catalog_token(verb_path.substr(0, space)) && catalog_token(verb_path.substr(space + 1));
}

auto cli_failure_cluster(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // Every failure inside the window, filtered here by shape and by the caller's predicate before any
  // counting; the threshold is applied to what is left. Rows come in id order, so members do too.
  auto stmt = ctx.conn.prepare("select id, verb_path, error_category, recorded_at from cli_invocations"
                               " where exit_code != 0 and error_category is not null"
                               "   and recorded_at >= ?1 and recorded_at <= ?2"
                               " order by id");
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto ok = stmt->bind_text(1, ctx.window.from); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = stmt->bind_text(2, ctx.window.to); !ok) {
    return std::unexpected(ok.error());
  }
  std::map<std::pair<std::string, std::string>, std::vector<im::cluster_member>> groups;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (step.value() == db::step_result::done) {
      break;
    }
    auto verb_path = stmt->column_text(1);
    if (!catalog_shaped(verb_path) || (ctx.verb_path_recognized && !ctx.verb_path_recognized(verb_path))) {
      continue;
    }
    groups[{verb_path, stmt->column_text(2)}].push_back(im::cluster_member{
        .ref = im::entity_ref{.kind = "cli_invocation", .id = stmt->column_int64(0)}, .time = stmt->column_text(3)});
  }
  std::vector<im::finding> out;
  for (auto& [key, members] : groups) {
    if (members.size() < k_cluster_threshold) {
      continue;
    }
    im::finding f;
    f.severity = im::diagnostic_severity::warning;
    f.primary  = members.front().ref;
    f.group    = im::grouping{.key_parts = {key.first, key.second}, .scope = "global"};
    for (const auto& m : members) {
      f.evidence.push_back(m.ref);
      f.evidence_times.push_back(m.time);
    }
    f.members = std::move(members);
    out.push_back(std::move(f));
  }
  return out;
}

/// The plan a claim belongs to, over the joins of `claim_failure_cluster`: its task's plan, its plan,
/// or its step's plan.
constexpr std::string_view k_claim_plan = "case c.entity_kind when 'task' then t.plan_id when 'plan' then c.entity_id"
                                          " else s.plan_id end";

auto claim_failure_cluster(const check_context& ctx) -> std::expected<std::vector<im::finding>, db::db_error> {
  // A task claim takes its task's scope; a plan or plan-step claim takes its plan's. The scope text is
  // `repo:<slug>` or `assoc:<slug>`; an entity with none, or a scope row that no longer exists, is `global`.
  auto sql = std::format(
      "select c.id, c.failure_category, c.released_at,"
      "       coalesce(case case when c.entity_kind = 'task' then t.scope_kind else p.scope_kind end"
      "                  when 'repo' then 'repo:' || pr.slug when 'association' then 'assoc:' || a.slug end, 'global')"
      " from agent_work_claims c"
      " left join tasks t on c.entity_kind = 'task' and t.id = c.entity_id"
      " left join plan_steps s on c.entity_kind = 'plan_step' and s.id = c.entity_id"
      " left join plans p on p.id = ({0})"
      " left join projects pr on pr.id = (case when c.entity_kind = 'task' then t.scope_id else p.scope_id end)"
      " left join associations a on a.id = (case when c.entity_kind = 'task' then t.scope_id else p.scope_id end)"
      " where c.failure_category is not null and c.failure_category != 'unknown'"
      "   and c.released_at is not null and c.released_at >= ?1 and c.released_at <= ?2"
      "   and {1}"
      " order by c.id",
      k_claim_plan, plan_filter_sql(ctx.scope, k_claim_plan));
  auto stmt = ctx.conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  if (auto ok = stmt->bind_text(1, ctx.window.from); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = stmt->bind_text(2, ctx.window.to); !ok) {
    return std::unexpected(ok.error());
  }
  struct bucket {
    std::vector<im::cluster_member> members;
    std::set<std::string>           scopes;
  };
  std::map<std::string, bucket> groups;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (step.value() == db::step_result::done) {
      break;
    }
    auto& b = groups[stmt->column_text(1)];
    b.members.push_back(
        im::cluster_member{.ref = im::entity_ref{.kind = "claim", .id = stmt->column_int64(0)}, .time = stmt->column_text(2)});
    b.scopes.insert(stmt->column_text(3));
  }
  std::vector<im::finding> out;
  for (auto& [category, b] : groups) {
    if (b.members.size() < k_cluster_threshold) {
      continue;
    }
    im::finding f;
    f.severity = im::diagnostic_severity::warning;
    f.primary  = b.members.front().ref;
    f.group    = im::grouping{.key_parts = {std::format("failure_category={}", category)},
                              .scope     = b.scopes.size() == 1 ? *b.scopes.begin() : std::string{"global"}};
    for (const auto& m : b.members) {
      f.evidence.push_back(m.ref);
      f.evidence_times.push_back(m.time);
    }
    f.members = std::move(b.members);
    out.push_back(std::move(f));
  }
  return out;
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
  f.checks.push_back(check_def{.id       = "cli-failure-cluster",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "cli_failure_cluster",
                               .recovery = "planar report --days 7 lists the repeated failures; fix the cause before retrying",
                               .inputs   = {"cli_log"},
                               .built    = true,
                               .evaluate = cli_failure_cluster});
  f.checks.push_back(check_def{.id       = "claim-failure-cluster",
                               .kind     = im::check_kind::event,
                               .severity = im::diagnostic_severity::warning,
                               .category = "claim_failure_cluster",
                               .recovery = "planar-watch claims --status aborted lists the failed claims; fix the shared cause",
                               .inputs   = {},
                               .built    = true,
                               .evaluate = claim_failure_cluster});
  return f;
}

} // namespace planar::engine::diagnose::detail
