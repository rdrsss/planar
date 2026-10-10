/// @file closeout.cpp
/// @brief Implementation of `planar.engine.planning.closeout`. See the module
/// interface for the hard-gate rules, the three counting queries' disagreement
/// about `status`, and the single deliberate divergence (the advisory git
/// layer is ported WORKING over `planar.git`, because the oracle's own is
/// dead code under a Zig 0.16.0 stdlib bug).

module;

module planar.engine.planning.closeout;

import std;
import planar.db;
import planar.git;
import planar.json_text;
import planar.policy.audit;

namespace planar::engine::planning::closeout {

namespace {

using planar::json_text::append_json_string;

/// @brief The synthetic `repo_root` for the no-locality-data git-evidence
/// entry. Distinct from `k_unknown_repo`, which stands in for a NULL column
/// on a row that DOES exist.
constexpr std::string_view k_none_repo = "(none)";

/// @brief The stand-in for a NULL `repo_root` on a locality row that exists.
/// Unreachable through the queries below (both filter `repo_root is not
/// null`), reproduced because the oracle carries the same `orelse`.
constexpr std::string_view k_unknown_repo = "(unknown)";

/// @brief The `target_branch` placeholder in text output and in an
/// inconclusive epic roll-up.
constexpr std::string_view k_unknown_branch = "(unknown)";

/// @brief The oracle's fixed `refs/heads/{branch}` scratch buffer size.
///
/// @brief The recursive CTE naming the plan AND every descendant plan.
/// Seeded with the plan itself — used by the claim, finalization and
/// locality queries, all of which count the plan's own rows too.
constexpr std::string_view k_self_and_descendants = R"(with recursive desc_plans(id) as (
  select ? as id
  union all
  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
)
)";

/// @brief The recursive CTE naming only the plan's DESCENDANTS.
/// Seeded from the children — used by the two queries that must not count
/// the plan's own tasks/row twice.
constexpr std::string_view k_descendants_only = R"(with recursive desc_plans(id) as (
  select id from plans where parent_plan_id = ?
  union all
  select p.id from plans p join desc_plans d on p.parent_plan_id = d.id
)
)";

// =========================================================================
// Git subprocess probes (best-effort; every failure is "no answer")
// =========================================================================

/// @brief `git -C repo <args...>`, raw stdout on exit 0.
/// @param repo_root The repository to run in.
/// @param args The git subcommand and its arguments.
/// @return The raw stdout, or unset.
auto git_run(std::string_view repo_root, std::span<const std::string_view> args) -> std::optional<std::string> {
  return git::run(std::filesystem::path{std::string{repo_root}}, args);
}

/// @brief `git_run`, trimmed, with empty output reported as no answer.
/// @param repo_root The repository to run in.
/// @param args The git subcommand and its arguments.
/// @return The trimmed stdout, or unset.
auto git_run_trimmed(std::string_view repo_root, std::span<const std::string_view> args) -> std::optional<std::string> {
  return git::run_trimmed(std::filesystem::path{std::string{repo_root}}, args);
}

/// @brief True when `branch` resolves as a local head.
///
/// FIXED AT TASK 6321 (decision 1067's FIX set). This carried the oracle's
/// fixed-buffer ceiling: a `[128]u8` scratch buffer meant `refs/heads/` plus
/// anything over 117 bytes did not fit, and the function returned FALSE
/// rather than probing -- reporting a branch ABSENT when it exists. A
/// closeout that reads "the branch is gone" when it is not is the wrong
/// answer to the question the operator asked, and it is silent.
///
/// The ceiling was always artificial here: this port formats the ref into a
/// `std::string`, which has no such limit, so the guard existed ONLY to
/// reproduce the oracle. Decision 1067 ended that rule. Git's own limit on
/// ref names now applies and `rev-parse --verify` reports it.
/// @param repo_root The repository to probe.
/// @param branch The branch name.
/// @return Whether `refs/heads/<branch>` verifies.
auto branch_exists(std::string_view repo_root, std::string_view branch) -> bool {
  static constexpr std::string_view     k_prefix = "refs/heads/";
  std::string const                     ref      = std::string{k_prefix} + std::string{branch};
  std::array<std::string_view, 3> const args{"rev-parse", "--verify", ref};
  return git_run(repo_root, args).has_value();
}

/// @brief Detect the repository's default branch.
///
/// `symbolic-ref --short refs/remotes/origin/HEAD` first (stripping an
/// `origin/` prefix when present, returning the raw answer when not), then
/// `main`, then `master`.
/// @param repo_root The repository to probe.
/// @return The branch name, or unset when git cannot answer.
auto detect_target_branch(std::string_view repo_root) -> std::optional<std::string> {
  {
    std::array<std::string_view, 3> const args{"symbolic-ref", "--short", "refs/remotes/origin/HEAD"};
    if (auto const raw = git_run_trimmed(repo_root, args); raw.has_value()) {
      static constexpr std::string_view k_origin = "origin/";
      if (raw->starts_with(k_origin)) {
        return raw->substr(k_origin.size());
      }
      return *raw;
    }
  }
  for (auto const& candidate : {std::string_view{"main"}, std::string_view{"master"}}) {
    if (branch_exists(repo_root, candidate)) {
      return std::string{candidate};
    }
  }
  return std::nullopt;
}

/// @brief True when `git merge-base --is-ancestor sha target` exits 0.
///
/// That command answers with an EMPTY stdout and its exit status, so this
/// reads `git_run` (empty-is-an-answer) rather than `git_run_trimmed`
/// (empty-is-no-answer). Swapping them would report every ancestor as
/// not-an-ancestor.
/// @param repo_root The repository to probe.
/// @param sha The candidate ancestor.
/// @param target The branch to test against.
/// @return Whether `sha` is an ancestor of `target`.
auto check_is_ancestor(std::string_view repo_root, std::string_view sha, std::string_view target) -> bool {
  std::array<std::string_view, 4> const args{"merge-base", "--is-ancestor", sha, target};
  return git_run(repo_root, args).has_value();
}

/// @brief True when `branch` appears in `git branch --merged <target>`.
/// @param repo_root The repository to probe.
/// @param branch The branch to look for.
/// @param target The branch merged into.
/// @return Whether the branch is listed as merged.
auto check_branch_merged(std::string_view repo_root, std::string_view branch, std::string_view target) -> bool {
  std::array<std::string_view, 3> const args{"branch", "--merged", target};
  auto const                            raw = git_run(repo_root, args);
  if (!raw.has_value()) {
    return false;
  }
  // Each line is `  <branch>` or `* <branch>`. The oracle trims the same
  // four characters from BOTH ends, which is why `*` is in the set rather
  // than stripped as a leading marker.
  static constexpr std::string_view k_strip = " *\t\r";
  std::string_view                  rest{*raw};
  while (!rest.empty()) {
    auto const             newline = rest.find('\n');
    std::string_view const line    = rest.substr(0, newline);
    rest                           = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);

    auto const begin = line.find_first_not_of(k_strip);
    if (begin == std::string_view::npos) {
      continue;
    }
    auto const end = line.find_last_not_of(k_strip);
    if (line.substr(begin, end - begin + 1) == branch) {
      return true;
    }
  }
  return false;
}

/// @brief Run the advisory probes for one locality tuple.
/// @param repo_root_opt The claim's `repo_root`, or unset.
/// @param branch_opt The claim's `branch`, or unset.
/// @param sha_opt The claim's `head_sha_at_claim`, or unset.
/// @return The evidence entry; its `note` is always set.
auto probe_git_evidence(const std::optional<std::string>& repo_root_opt, const std::optional<std::string>& branch_opt,
                        const std::optional<std::string>& sha_opt) -> git_evidence {
  std::string const repo_root = repo_root_opt.value_or(std::string{k_unknown_repo});

  git_evidence out;
  out.repo_root = repo_root;
  out.branch    = branch_opt;

  auto const target = detect_target_branch(repo_root);
  if (!target.has_value()) {
    out.note = "git-evidence unavailable";
    return out;
  }
  out.target_branch = target;

  if (sha_opt.has_value()) {
    out.base_merged = check_is_ancestor(repo_root, *sha_opt, *target);
  }

  // A branch that is gone is the ordinary post-merge state, so its absence
  // is reported as inconclusive rather than as "not merged" — and that note
  // REPLACES the composed one below rather than joining it.
  std::optional<std::string> branch_note;
  if (branch_opt.has_value()) {
    if (branch_exists(repo_root, *branch_opt)) {
      out.branch_merged = check_branch_merged(repo_root, *branch_opt, *target);
    } else {
      branch_note = "branch absent — inconclusive";
    }
  }

  if (branch_note.has_value()) {
    out.note = *branch_note;
    return out;
  }
  if (!out.base_merged.has_value() && !out.branch_merged.has_value()) {
    out.note = "no sha to check — inconclusive";
    return out;
  }
  std::string_view const base_text = out.base_merged.has_value()
                                         ? (*out.base_merged ? "base-merged=true (weak signal)" : "base-merged=false")
                                         : "base-merged=unknown";
  std::string_view const branch_text =
      out.branch_merged.has_value() ? (*out.branch_merged ? "branch-merged=true" : "branch-merged=false") : "";
  out.note = branch_text.empty() ? std::string{base_text} : std::format("{}; {}", base_text, branch_text);
  return out;
}

// =========================================================================
// Hard-gate data collection
// =========================================================================

/// @brief Read one prepared, plan-bound aggregate row.
/// @param conn The connection.
/// @param sql The single-row aggregate query, taking one `?` plan id.
/// @param plan_id The plan id to bind.
/// @param sink Invoked with the row when one comes back.
/// @return Success, or `query_failed`.
auto for_one_row(db::connection& conn, std::string_view sql, std::int64_t plan_id,
                 const std::function<void(const db::statement&)>& sink) -> std::expected<void, closeout_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(closeout_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id)) {
    return std::unexpected(closeout_error::query_failed);
  }
  auto const stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(closeout_error::query_failed);
  }
  if (*stepped == db::step_result::row) {
    sink(*stmt);
  }
  return {};
}

/// @brief The plan's current `plans.status`.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The status text, or `not_found` / `query_failed`.
auto fetch_plan_status(db::connection& conn, std::int64_t plan_id) -> std::expected<std::string, closeout_error> {
  auto stmt = conn.prepare("select status from plans where id = ?");
  if (!stmt) {
    return std::unexpected(closeout_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id)) {
    return std::unexpected(closeout_error::query_failed);
  }
  auto const stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(closeout_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(closeout_error::not_found);
  }
  return stmt->column_text(0);
}

/// @brief Task counts, summed over the plan's own tasks AND its descendants'.
///
/// Two queries whose results ADD, exactly as the oracle does it: the first
/// is `plan_id = ?` and the second walks `desc_plans` seeded from the
/// CHILDREN, so no task is counted twice.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The counts, or `query_failed`.
auto collect_task_counts(db::connection& conn, std::int64_t plan_id) -> std::expected<task_counts, closeout_error> {
  task_counts counts;
  auto const  accumulate = [&counts](const db::statement& row) {
    counts.open += row.column_int64(0);
    counts.done += row.column_int64(1);
    counts.cancelled += row.column_int64(2);
  };
  {
    static constexpr std::string_view k_own = R"(select
  sum(case when status in ('todo','doing','blocked') then 1 else 0 end),
  sum(case when status = 'done' then 1 else 0 end),
  sum(case when status = 'cancelled' then 1 else 0 end)
from tasks where plan_id = ?)";
    if (auto const ok = for_one_row(conn, k_own, plan_id, accumulate); !ok) {
      return std::unexpected(ok.error());
    }
  }
  {
    auto const sql = std::format(R"({}select
  sum(case when t.status in ('todo','doing','blocked') then 1 else 0 end),
  sum(case when t.status = 'done' then 1 else 0 end),
  sum(case when t.status = 'cancelled' then 1 else 0 end)
from tasks t
where t.plan_id in (select id from desc_plans))",
                                 k_descendants_only);
    if (auto const ok = for_one_row(conn, sql, plan_id, accumulate); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return counts;
}

/// @brief Descendant-plan counts (recursive, excluding the plan itself).
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The counts, or `query_failed`.
auto collect_descendant_counts(db::connection& conn, std::int64_t plan_id) -> std::expected<descendant_counts, closeout_error> {
  descendant_counts counts;
  auto const        sql = std::format(R"({}select
  sum(case when p.status in ('draft','active','paused') then 1 else 0 end),
  sum(case when p.status in ('done','abandoned') then 1 else 0 end)
from plans p
where p.id in (select id from desc_plans))",
                                      k_descendants_only);
  auto const        ok  = for_one_row(conn, sql, plan_id, [&counts](const db::statement& row) {
    counts.open     = row.column_int64(0);
    counts.terminal = row.column_int64(1);
  });
  if (!ok) {
    return std::unexpected(ok.error());
  }
  return counts;
}

/// @brief Live vs stale ACTIVE claims across the plan and its descendants.
///
/// This is the ONLY one of the three claim-reading queries that filters
/// `c.status = 'active'`; the two locality queries below deliberately do
/// not. See the module header.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The counts, or `query_failed`.
auto collect_claim_counts(db::connection& conn, std::int64_t plan_id) -> std::expected<claim_counts, closeout_error> {
  claim_counts counts;
  auto const   sql = std::format(R"({}select
  sum(case when c.lease_expires_at > strftime('%Y-%m-%dT%H:%M:%fZ','now') then 1 else 0 end),
  sum(case when c.lease_expires_at <= strftime('%Y-%m-%dT%H:%M:%fZ','now') then 1 else 0 end)
from agent_work_claims c
join tasks t on t.id = c.entity_id
where c.entity_kind = 'task'
  and c.status = 'active'
  and t.plan_id in (select id from desc_plans))",
                                 k_self_and_descendants);
  auto const   ok  = for_one_row(conn, sql, plan_id, [&counts](const db::statement& row) {
    counts.live  = row.column_int64(0);
    counts.stale = row.column_int64(1);
  });
  if (!ok) {
    return std::unexpected(ok.error());
  }
  return counts;
}

/// @brief Count tasks whose slug carries a finalization prefix.
///
/// Advisory labelling only — the gate never reads it. See the module header.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The count, or `query_failed`.
auto collect_finalization_task_count(db::connection& conn, std::int64_t plan_id) -> std::expected<std::int64_t, closeout_error> {
  std::int64_t count = 0;
  auto const   sql   = std::format(R"({}select count(*)
from tasks t
where t.plan_id in (select id from desc_plans)
  and (
    t.slug like 'finalize-%'
    or t.slug like 'merge-%'
    or t.slug like 'reconcile-%'
  ))",
                                   k_self_and_descendants);
  auto const   ok    = for_one_row(conn, sql, plan_id, [&count](const db::statement& row) { count = row.column_int64(0); });
  if (!ok) {
    return std::unexpected(ok.error());
  }
  return count;
}

/// @brief One row of claim locality.
struct locality_row {
  std::optional<std::string> repo_root; ///< `agent_work_claims.repo_root`.
  std::optional<std::string> branch;    ///< `agent_work_claims.branch`.
  std::optional<std::string> head_sha;  ///< `agent_work_claims.head_sha_at_claim`.
};

/// @brief Read an optional text column.
/// @param row The stepped statement.
/// @param index The 0-indexed column.
/// @return The text, or unset when SQL NULL.
auto text_opt(const db::statement& row, int index) -> std::optional<std::string> {
  if (row.is_null(index)) {
    return std::nullopt;
  }
  return row.column_text(index);
}

/// @brief Run a locality query and collect its rows.
/// @param conn The connection.
/// @param sql The query; column 0 is `repo_root`, 1 is `branch`, and column
/// 2 is `head_sha_at_claim` when `with_sha` is set.
/// @param plan_id The plan to bind.
/// @param with_sha Whether the query selects a third column.
/// @return The rows, or `query_failed`.
auto collect_locality(db::connection& conn, std::string_view sql, std::int64_t plan_id, bool with_sha)
    -> std::expected<std::vector<locality_row>, closeout_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(closeout_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id)) {
    return std::unexpected(closeout_error::query_failed);
  }
  std::vector<locality_row> rows;
  while (true) {
    auto const stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(closeout_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    rows.push_back(locality_row{
        .repo_root = text_opt(*stmt, 0), .branch = text_opt(*stmt, 1), .head_sha = with_sha ? text_opt(*stmt, 2) : std::nullopt});
  }
  return rows;
}

/// @brief Collect the advisory per-tuple git evidence.
///
/// NOTE the absent `c.status` filter — a `released` or `expired` claim still
/// contributes evidence here while contributing nothing to
/// `collect_claim_counts`. That asymmetry is the oracle's.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return One entry per distinct locality tuple, or a single synthetic
/// `(none)` entry when there is no locality data at all.
auto collect_git_evidence(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::vector<git_evidence>, closeout_error> {
  auto const sql  = std::format(R"({}select distinct c.repo_root, c.branch, c.head_sha_at_claim
from agent_work_claims c
join tasks t on t.id = c.entity_id
where c.entity_kind = 'task'
  and t.plan_id in (select id from desc_plans)
  and c.repo_root is not null)",
                                k_self_and_descendants);
  auto const rows = collect_locality(conn, sql, plan_id, true);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  if (rows->empty()) {
    return std::vector<git_evidence>{
        git_evidence{.repo_root = std::string{k_none_repo},
                     .note      = "no commit attribution — inconclusive (hardens once session-commit capture is wired)"}};
  }

  std::vector<git_evidence> out;
  out.reserve(rows->size());
  for (auto const& row : *rows) {
    out.push_back(probe_git_evidence(row.repo_root, row.branch, row.head_sha));
  }
  return out;
}

/// @brief Collect the advisory epic-branch merge roll-up.
///
/// Distinct on `(repo_root, branch)` with BOTH non-null — a narrower set
/// than `collect_git_evidence`'s, which also keys on the sha and accepts a
/// null branch.
/// @param conn The connection.
/// @param plan_id The plan.
/// @return The roll-up, unset when there is no locality data (inconclusive,
/// not an error), or `query_failed`.
auto collect_epic_merge_rollup(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::optional<epic_merge_rollup>, closeout_error> {
  auto const sql  = std::format(R"({}select distinct c.repo_root, c.branch
from agent_work_claims c
join tasks t on t.id = c.entity_id
where c.entity_kind = 'task'
  and t.plan_id in (select id from desc_plans)
  and c.repo_root is not null
  and c.branch is not null)",
                                k_self_and_descendants);
  auto const rows = collect_locality(conn, sql, plan_id, false);
  if (!rows) {
    return std::unexpected(rows.error());
  }
  if (rows->empty()) {
    return std::optional<epic_merge_rollup>{};
  }

  auto const total = static_cast<std::int64_t>(rows->size());

  // The oracle detects the target branch from the FIRST row's repo only,
  // then applies it to every row — including rows from a different repo.
  // In practice one plan's claims share a root.
  std::string const first_repo = rows->front().repo_root.value_or(std::string{k_unknown_repo});
  auto const        target     = detect_target_branch(first_repo);
  if (!target.has_value()) {
    return std::optional<epic_merge_rollup>{
        epic_merge_rollup{.target_branch  = std::string{k_unknown_branch},
                          .total_branches = total,
                          .merged_count   = 0,
                          .note           = "git-evidence unavailable — epic-merge check inconclusive"}};
  }

  std::int64_t merged = 0;
  for (auto const& row : *rows) {
    if (!row.repo_root.has_value() || !row.branch.has_value()) {
      continue;
    }
    // An absent branch is the ordinary post-merge state: counted neither as
    // merged nor as unmerged.
    if (branch_exists(*row.repo_root, *row.branch) && check_branch_merged(*row.repo_root, *row.branch, *target)) {
      merged += 1;
    }
  }

  return std::optional<epic_merge_rollup>{
      epic_merge_rollup{.target_branch  = *target,
                        .total_branches = total,
                        .merged_count   = merged,
                        .note           = std::format("{} of {} contributing branch(es) merged to {} (advisory; absent branches "
                                                      "inconclusive)",
                                                      merged, total, *target)}};
}

/// @brief Write the status change and its audit row inside one transaction.
/// @param conn The connection.
/// @param plan_id The plan to close.
/// @param tasks The task counts, for the audit summary.
/// @param descendants The descendant counts, for the audit summary.
/// @return Success, `query_failed` for the UPDATE, or `write_failed` when
/// the audit INSERT fails (which rolls the UPDATE back).
auto apply_closeout(db::connection& conn, std::int64_t plan_id, const task_counts& tasks, const descendant_counts& descendants)
    -> std::expected<void, closeout_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(closeout_error::query_failed);
  }

  {
    auto stmt = conn.prepare("update plans set status = 'done', "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    if (!stmt) {
      return std::unexpected(closeout_error::query_failed);
    }
    if (!stmt->bind_int64(1, plan_id)) {
      return std::unexpected(closeout_error::query_failed);
    }
    auto const stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(closeout_error::query_failed);
    }
  }

  // Record WHICH path closed the plan and the evidence it passed on.
  // Without this, closeout and `plan update --status done` are
  // indistinguishable in `audit_log`.
  auto const summary  = std::format("closeout plan {}: → done; tasks done={} cancelled={}; descendants terminal={}", plan_id,
                                    tasks.done, tasks.cancelled, descendants.terminal);
  auto const recorded = policy::audit::record(conn, policy::audit::record_args{
                                                        .verb    = policy::audit::verb::status_change,
                                                        .entity  = {.kind = "plan", .id = plan_id},
                                                        .summary = summary,
                                                    });
  if (!recorded) {
    // `tx` rolls back on scope exit, so the status change never lands
    // without its audit row.
    return std::unexpected(closeout_error::write_failed);
  }

  if (!tx->commit()) {
    return std::unexpected(closeout_error::query_failed);
  }
  return {};
}

} // namespace

auto evaluate(db::connection& conn, std::int64_t plan_id, bool apply, bool check_merge)
    -> std::expected<closeout_result, closeout_error> {
  auto const status = fetch_plan_status(conn, plan_id);
  if (!status) {
    return std::unexpected(status.error());
  }

  // Task 6889: the hard-evidence counts are collected UNCONDITIONALLY, even
  // on the already-terminal path below — a plan already `done` still has
  // real task/descendant/claim rows, and `hard_evidence` reporting all
  // zeros next to `ready:true` misrepresents what closeout actually found
  // (task 6889 measured exactly this: 20 real `done` tasks read back as
  // `tasks: open 0 done 0 cancelled 0`). This is UNLIKE `git`/`epic_merge`
  // below, which the module header explicitly documents as empty/unset on
  // the already-terminal path — advisory locality data genuinely was never
  // collected there, and that omission IS deliberate.
  auto const tasks = collect_task_counts(conn, plan_id);
  if (!tasks) {
    return std::unexpected(tasks.error());
  }
  auto const descendants = collect_descendant_counts(conn, plan_id);
  if (!descendants) {
    return std::unexpected(descendants.error());
  }
  auto const claims = collect_claim_counts(conn, plan_id);
  if (!claims) {
    return std::unexpected(claims.error());
  }
  auto const finalization = collect_finalization_task_count(conn, plan_id);
  if (!finalization) {
    return std::unexpected(finalization.error());
  }

  // Already terminal: short-circuit BEFORE the gate rules and BEFORE any
  // git-evidence/epic-merge collection. Note the EMPTY `git` vector this
  // still produces — a live evaluation with no locality data emits a
  // one-entry synthetic instead. See the module header.
  if (*status == "done" || *status == "abandoned") {
    return closeout_result{
        .plan_id = plan_id,
        .ready   = true,
        .applied = false,
        .hard    = {.tasks = *tasks, .descendants = *descendants, .claims = *claims, .finalization_tasks = *finalization},
    };
  }

  // Reason order is rule order: tasks, then descendants, then claims.
  std::vector<std::string> blocked_by;
  if (tasks->open > 0) {
    blocked_by.push_back(std::format("{} open task(s) on plan (todo/doing/blocked)", tasks->open));
  }
  if (descendants->open > 0) {
    blocked_by.push_back(std::format("{} open descendant plan(s) (draft/active/paused)", descendants->open));
  }
  if (claims->live > 0) {
    blocked_by.push_back(std::format("{} live claim(s) still active on plan tasks", claims->live));
  }

  std::vector<std::string> warnings;
  if (claims->stale > 0) {
    warnings.push_back(std::format("{} stale/expired claim(s) on plan tasks — reconcilable, not blocking", claims->stale));
  }

  auto const evidence = collect_git_evidence(conn, plan_id);
  if (!evidence) {
    return std::unexpected(evidence.error());
  }

  std::optional<epic_merge_rollup> epic;
  if (check_merge) {
    auto const rollup = collect_epic_merge_rollup(conn, plan_id);
    if (!rollup) {
      return std::unexpected(rollup.error());
    }
    epic = *rollup;
  }

  bool const ready   = blocked_by.empty();
  bool       applied = false;
  if (apply && ready) {
    auto const written = apply_closeout(conn, plan_id, *tasks, *descendants);
    if (!written) {
      return std::unexpected(written.error());
    }
    applied = true;
  }

  return closeout_result{
      .plan_id    = plan_id,
      .ready      = ready,
      .applied    = applied,
      .hard       = {.tasks = *tasks, .descendants = *descendants, .claims = *claims, .finalization_tasks = *finalization},
      .blocked_by = std::move(blocked_by),
      .git        = *evidence,
      .epic_merge = epic,
      .warnings   = std::move(warnings),
  };
}

auto render_json(const closeout_result& result, std::string_view trailing_fields) -> std::string {
  auto const& tasks       = result.hard.tasks;
  auto const& descendants = result.hard.descendants;
  auto const& claims      = result.hard.claims;

  std::string out =
      std::format(R"({{"plan":{},"ready":{},"applied":{},"hard_evidence":{{)"
                  R"("tasks":{{"open":{},"done":{},"cancelled":{}}},)"
                  R"("descendants":{{"open":{},"terminal":{}}},)"
                  R"("claims":{{"live":{},"stale":{}}},)"
                  R"("finalization_tasks":{}}},"blocked_by":[)",
                  result.plan_id, result.ready, result.applied, tasks.open, tasks.done, tasks.cancelled, descendants.open,
                  descendants.terminal, claims.live, claims.stale, result.hard.finalization_tasks);

  bool first = true;
  for (auto const& reason : result.blocked_by) {
    if (!first) {
      out += ",";
    }
    first = false;
    append_json_string(out, reason);
  }

  out += R"(],"git_evidence":[)";
  first                      = true;
  auto const optional_string = [&out](const std::optional<std::string>& value) {
    if (value.has_value()) {
      append_json_string(out, *value);
    } else {
      out += "null";
    }
  };
  auto const optional_bool = [&out](std::optional<bool> value) {
    out += value.has_value() ? (*value ? "true" : "false") : "null";
  };
  for (auto const& entry : result.git) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += R"({"repo_root":)";
    append_json_string(out, entry.repo_root);
    out += R"(,"branch":)";
    optional_string(entry.branch);
    out += R"(,"target_branch":)";
    optional_string(entry.target_branch);
    out += R"(,"base_merged":)";
    optional_bool(entry.base_merged);
    out += R"(,"branch_merged":)";
    optional_bool(entry.branch_merged);
    out += R"(,"note":)";
    append_json_string(out, entry.note);
    out += "}";
  }

  out += R"(],"epic_merge":)";
  if (result.epic_merge.has_value()) {
    out += R"({"target_branch":)";
    append_json_string(out, result.epic_merge->target_branch);
    out += std::format(R"(,"total_branches":{},"merged_count":{},"note":)", result.epic_merge->total_branches,
                       result.epic_merge->merged_count);
    append_json_string(out, result.epic_merge->note);
    out += "}";
  } else {
    out += "null";
  }

  out += R"(,"warnings":[)";
  first = true;
  for (auto const& warning : result.warnings) {
    if (!first) {
      out += ",";
    }
    first = false;
    append_json_string(out, warning);
  }
  out += "]";
  out += trailing_fields;
  out += "}\n";
  return out;
}

auto render_text(const closeout_result& result, bool dry_run) -> std::string {
  std::string_view const mode_label = dry_run ? "[dry-run] " : "";

  std::string out;
  // TASK 6318. The middle arm used to read "already terminal — no change"
  // for ANY ready-but-not-applied evaluation, which meant every `--dry-run`
  // on an open, closeable plan. An operator running `--dry-run` to ask
  // "CAN this be closed?" was told it already IS closed -- the two states
  // differ in exactly the way the question is about.
  //
  // `dry_run` is the discriminator, and it is exact: in apply mode a ready
  // plan that was not already terminal sets `applied`, so reaching this arm
  // with `dry_run == false` does mean the plan was already terminal.
  if (result.applied) {
    out += std::format("{}plan {}: marked done\n", mode_label, result.plan_id);
  } else if (result.ready && dry_run) {
    out += std::format("{}plan {}: ready to close (no change made)\n", mode_label, result.plan_id);
  } else if (result.ready) {
    out += std::format("{}plan {}: ready (already terminal — no change)\n", mode_label, result.plan_id);
  } else {
    out += std::format("{}plan {}: NOT ready to close\n", mode_label, result.plan_id);
  }

  auto const& tasks       = result.hard.tasks;
  auto const& descendants = result.hard.descendants;
  auto const& claims      = result.hard.claims;

  out += "\nhard gate:\n";
  out += std::format("  tasks:       open={}  done={}  cancelled={}\n", tasks.open, tasks.done, tasks.cancelled);
  if (result.hard.finalization_tasks > 0) {
    out += std::format("    (finalization tasks: {} — merge/reconcile/finalize-prefixed)\n", result.hard.finalization_tasks);
  }
  out += std::format("  descendants: open={}  terminal={}\n", descendants.open, descendants.terminal);
  out += std::format("  claims:      live={}  stale={}\n", claims.live, claims.stale);

  if (!result.blocked_by.empty()) {
    out += "\nblocked by:\n";
    for (auto const& reason : result.blocked_by) {
      out += std::format("  - {}\n", reason);
    }
  }

  if (!result.warnings.empty()) {
    out += "\nwarnings:\n";
    for (auto const& warning : result.warnings) {
      out += std::format("  ! {}\n", warning);
    }
  }

  if (!result.git.empty()) {
    out += "\ngit evidence (advisory):\n";
    for (auto const& entry : result.git) {
      out += std::format("  repo: {}\n", entry.repo_root);
      out += std::format("    branch:  {}  target: {}\n", entry.branch.value_or(std::string{k_none_repo}),
                         entry.target_branch.value_or(std::string{k_unknown_branch}));
      out += std::format("    note:    {}\n", entry.note);
    }
  }

  if (result.epic_merge.has_value()) {
    out += "\nepic-branch merge check (advisory):\n";
    out += std::format("  target: {}\n", result.epic_merge->target_branch);
    out += std::format("  note:   {}\n", result.epic_merge->note);
  }

  return out;
}

} // namespace planar::engine::planning::closeout
