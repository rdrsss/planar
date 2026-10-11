/// @file model.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.agents.model`.
module planar.cmd.planar_watch.handlers.agents.model;
import std;
import planar.db;

namespace planar::cmd::watch::agents {

namespace {

/// Child plans deeper than this below an agent's root are not read.
constexpr int k_max_plan_depth = 4;

/// Parent links followed when naming a plan's ancestors.
constexpr int k_max_ancestors = 16;

using ms_time = std::chrono::sys_time<std::chrono::milliseconds>;

auto optional_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index))
    return std::nullopt;
  return stmt.column_text(index);
}

auto parse_int(std::string_view text, std::size_t pos, std::size_t len, int& out) -> bool {
  if (pos + len > text.size())
    return false;
  auto const* first = text.data() + pos;
  auto const  res   = std::from_chars(first, first + len, out);
  return res.ec == std::errc{} && res.ptr == first + len;
}

/// Parse `YYYY-MM-DDTHH:MM:SS[.mmm]Z`.
auto parse_timestamp(std::string_view text) -> std::optional<ms_time> {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  if (!parse_int(text, 0, 4, y) || !parse_int(text, 5, 2, mo) || !parse_int(text, 8, 2, d) || !parse_int(text, 11, 2, h) ||
      !parse_int(text, 14, 2, mi) || !parse_int(text, 17, 2, s))
    return std::nullopt;
  if (text.size() >= 23 && text[19] == '.' && !parse_int(text, 20, 3, ms))
    return std::nullopt;
  std::chrono::year_month_day const ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                        std::chrono::day{static_cast<unsigned>(d)}};
  if (!ymd.ok())
    return std::nullopt;
  return ms_time{std::chrono::sys_days{ymd}} + std::chrono::hours{h} + std::chrono::minutes{mi} + std::chrono::seconds{s} +
         std::chrono::milliseconds{ms};
}

auto format_timestamp(ms_time at) -> std::string {
  return std::format("{:%FT%T}Z", at);
}

auto load_tasks(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<task_node>, db::db_error> {
  auto stmt = conn.prepare("select id, title, status from tasks where plan_id = ?1 order by priority, id");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto b = stmt->bind_int64(1, plan_id); !b)
    return std::unexpected(b.error());
  std::vector<task_node> out;
  while (true) {
    auto step = stmt->step();
    if (!step)
      return std::unexpected(step.error());
    if (*step == db::step_result::done)
      break;
    out.push_back(
        task_node{.id = stmt->column_int64(0), .title = stmt->column_text(1), .status = stmt->column_text(2), .holders = {}});
  }
  return out;
}

auto load_plan(db::connection& conn, std::int64_t plan_id, int depth) -> std::expected<std::optional<plan_node>, db::db_error> {
  auto stmt = conn.prepare("select title, status from plans where id = ?1");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto b = stmt->bind_int64(1, plan_id); !b)
    return std::unexpected(b.error());
  auto step = stmt->step();
  if (!step)
    return std::unexpected(step.error());
  if (*step == db::step_result::done)
    return std::optional<plan_node>{};

  plan_node node{.id = plan_id, .title = stmt->column_text(0), .status = stmt->column_text(1), .tasks = {}, .milestones = {}};
  auto      tasks = load_tasks(conn, plan_id);
  if (!tasks)
    return std::unexpected(tasks.error());
  node.tasks = std::move(*tasks);
  if (depth >= k_max_plan_depth)
    return std::optional<plan_node>{std::move(node)};

  auto children = conn.prepare("select id from plans where parent_plan_id = ?1 order by id");
  if (!children)
    return std::unexpected(children.error());
  if (auto b = children->bind_int64(1, plan_id); !b)
    return std::unexpected(b.error());
  std::vector<std::int64_t> ids;
  while (true) {
    auto s = children->step();
    if (!s)
      return std::unexpected(s.error());
    if (*s == db::step_result::done)
      break;
    ids.push_back(children->column_int64(0));
  }
  for (auto const id : ids) {
    auto child = load_plan(conn, id, depth + 1);
    if (!child)
      return std::unexpected(child.error());
    if (child->has_value())
      node.milestones.push_back(std::move(**child));
  }
  return std::optional<plan_node>{std::move(node)};
}

/// Ancestors of `plan_id`, nearest first, as (id, title) pairs. Excludes `plan_id` itself.
auto load_ancestors(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::vector<std::pair<std::int64_t, std::string>>, db::db_error> {
  std::vector<std::pair<std::int64_t, std::string>> out;
  auto stmt = conn.prepare("select p.id, p.title from plans c join plans p on p.id = c.parent_plan_id where c.id = ?1");
  if (!stmt)
    return std::unexpected(stmt.error());
  std::int64_t current = plan_id;
  for (int hops = 0; hops < k_max_ancestors; ++hops) {
    if (auto r = stmt->reset(); !r)
      return std::unexpected(r.error());
    if (auto b = stmt->bind_int64(1, current); !b)
      return std::unexpected(b.error());
    auto step = stmt->step();
    if (!step)
      return std::unexpected(step.error());
    if (*step == db::step_result::done)
      break;
    current = stmt->column_int64(0);
    out.emplace_back(current, stmt->column_text(1));
  }
  return out;
}

auto titles_outermost_first(const std::vector<std::pair<std::int64_t, std::string>>& ancestors) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
    out.push_back(it->second);
  return out;
}

auto find_task(plan_node& plan, std::int64_t task_id) -> task_node* {
  for (auto& task : plan.tasks)
    if (task.id == task_id)
      return &task;
  for (auto& child : plan.milestones)
    if (auto* hit = find_task(child, task_id))
      return hit;
  return nullptr;
}

auto load_claims(db::connection& conn, const options& opts) -> std::expected<std::vector<claim_info>, db::db_error> {
  auto stmt =
      conn.prepare("select c.id, c.vendor, c.role, c.model, c.branch, c.entity_kind, c.entity_id, c.status, c.claimed_at, "
                   "c.last_heartbeat_at, c.lease_expires_at, c.released_at, c.release_reason, "
                   "(select a.summary from agent_actions a where a.claim_id = c.id and a.action_kind = 'heartbeat' "
                   "and a.summary is not null order by a.id desc limit 1) "
                   "from agent_work_claims c "
                   "where (c.status = 'active' and c.lease_expires_at >= ?1) "
                   "or (c.status <> 'active' and c.released_at >= ?1) "
                   "order by c.id");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto b = stmt->bind_text(1, opts.stopped_since); !b)
    return std::unexpected(b.error());
  std::vector<claim_info> out;
  while (true) {
    auto step = stmt->step();
    if (!step)
      return std::unexpected(step.error());
    if (*step == db::step_result::done)
      break;
    claim_info info{
        .id                = stmt->column_int64(0),
        .vendor            = stmt->column_text(1),
        .role              = optional_text(*stmt, 2),
        .model             = optional_text(*stmt, 3),
        .branch            = optional_text(*stmt, 4),
        .entity_kind       = stmt->column_text(5),
        .entity_id         = stmt->column_int64(6),
        .status            = stmt->column_text(7),
        .claimed_at        = stmt->column_text(8),
        .last_heartbeat_at = stmt->column_text(9),
        .lease_expires_at  = stmt->column_text(10),
        .released_at       = optional_text(*stmt, 11),
        .release_reason    = optional_text(*stmt, 12),
        .status_text       = optional_text(*stmt, 13),
    };
    info.state = classify(info, opts.now);
    out.push_back(std::move(info));
  }
  return out;
}

/// The plan a task belongs to, or unset for a task with no plan or no row.
auto task_plan(db::connection& conn, std::int64_t task_id) -> std::expected<std::optional<std::int64_t>, db::db_error> {
  auto stmt = conn.prepare("select plan_id from tasks where id = ?1");
  if (!stmt)
    return std::unexpected(stmt.error());
  if (auto b = stmt->bind_int64(1, task_id); !b)
    return std::unexpected(b.error());
  auto step = stmt->step();
  if (!step)
    return std::unexpected(step.error());
  if (*step == db::step_result::done || stmt->is_null(0))
    return std::optional<std::int64_t>{};
  return std::optional<std::int64_t>{stmt->column_int64(0)};
}

auto build_in_transaction(db::connection& conn, const options& opts) -> std::expected<snapshot, db::db_error> {
  snapshot snap{.generated_at = opts.now, .agents = {}, .claims = {}};
  auto     claims = load_claims(conn, opts);
  if (!claims)
    return std::unexpected(claims.error());

  // Plan claims first: each becomes a row, and the plans they cover are where task claims nest.
  std::map<std::int64_t, std::size_t> row_for_plan;
  for (auto const& claim : *claims) {
    snap.claims.emplace(claim.id, claim);
    if (claim.entity_kind == "task")
      continue;
    agent_node node{.claim_id = claim.id, .path = {}, .root = std::nullopt, .task_id = std::nullopt};
    if (claim.entity_kind == "plan") {
      auto plan = load_plan(conn, claim.entity_id, 0);
      if (!plan)
        return std::unexpected(plan.error());
      node.root      = std::move(*plan);
      auto ancestors = load_ancestors(conn, claim.entity_id);
      if (!ancestors)
        return std::unexpected(ancestors.error());
      node.path = titles_outermost_first(*ancestors);
      // A live orchestrator wins the plan over a stopped one.
      auto const existing = row_for_plan.find(claim.entity_id);
      if (existing == row_for_plan.end() ||
          (snap.claims.at(snap.agents[existing->second].claim_id).state == agent_state::stopped &&
           claim.state != agent_state::stopped))
        row_for_plan[claim.entity_id] = snap.agents.size();
    }
    snap.agents.push_back(std::move(node));
  }

  for (auto const& claim : *claims) {
    if (claim.entity_kind != "task")
      continue;
    auto plan_id = task_plan(conn, claim.entity_id);
    if (!plan_id)
      return std::unexpected(plan_id.error());
    std::vector<std::pair<std::int64_t, std::string>> ancestors;
    if (plan_id->has_value()) {
      auto loaded = load_ancestors(conn, **plan_id);
      if (!loaded)
        return std::unexpected(loaded.error());
      ancestors = std::move(*loaded);
    }

    // Nest under the nearest plan claim at or above the task's plan. A
    // completed or released claim there is not drawn at all: the task's own
    // status already says it finished.
    task_node* nest_target = nullptr;
    if (plan_id->has_value()) {
      std::vector<std::int64_t> chain{**plan_id};
      for (auto const& [id, title] : ancestors)
        chain.push_back(id);
      for (auto const id : chain) {
        auto const hit = row_for_plan.find(id);
        if (hit == row_for_plan.end())
          continue;
        auto& owner = snap.agents[hit->second];
        if (owner.root.has_value())
          nest_target = find_task(*owner.root, claim.entity_id);
        break;
      }
    }
    if (nest_target != nullptr) {
      if (claim.status == "active" || claim.status == "aborted" || claim.status == "stale")
        nest_target->holders.push_back(claim.id);
      continue;
    }

    agent_node node{
        .claim_id = claim.id, .path = titles_outermost_first(ancestors), .root = std::nullopt, .task_id = claim.entity_id};
    if (plan_id->has_value()) {
      auto plan = load_plan(conn, **plan_id, k_max_plan_depth);
      if (!plan)
        return std::unexpected(plan.error());
      node.root = std::move(*plan);
      if (node.root.has_value())
        if (auto* task = find_task(*node.root, claim.entity_id))
          task->holders.push_back(claim.id);
    }
    snap.agents.push_back(std::move(node));
  }

  std::ranges::stable_sort(snap.agents, [&](const agent_node& a, const agent_node& b) {
    auto const rank = [&](const agent_node& n) { return snap.claims.at(n.claim_id).state == agent_state::stopped ? 1 : 0; };
    return rank(a) < rank(b);
  });
  return snap;
}

auto state_rank(agent_state state) -> int {
  switch (state) {
  case agent_state::working:
    return 0;
  case agent_state::waiting:
    return 1;
  case agent_state::stopped:
    return 2;
  }
  return 2;
}

auto contains_holder(const plan_node& plan) -> bool {
  for (auto const& task : plan.tasks)
    if (!task.holders.empty())
      return true;
  for (auto const& child : plan.milestones)
    if (contains_holder(child))
      return true;
  return false;
}

auto count_tasks(const plan_node& plan, int& done, int& total) -> void {
  for (auto const& task : plan.tasks) {
    ++total;
    if (task.status == "done")
      ++done;
  }
  for (auto const& child : plan.milestones)
    count_tasks(child, done, total);
}

auto agent_label(const claim_info& claim) -> std::string {
  return std::format("{} · {}", claim.vendor, claim.role.value_or("agent"));
}

/// What a stopped claim's row says about why and when.
auto stopped_detail(const claim_info& claim, std::string_view now) -> std::string {
  if (claim.status == "active") {
    auto const age = short_age(claim.lease_expires_at, now);
    return age.empty() ? std::string{"lease expired"} : std::format("lease expired {} ago", age);
  }
  auto const age = claim.released_at.has_value() ? short_age(*claim.released_at, now) : std::string{};
  auto       out = age.empty() ? claim.status : std::format("{} {} ago", claim.status, age);
  if (claim.release_reason.has_value() && !claim.release_reason->empty())
    out += std::format(" · {}", *claim.release_reason);
  return out;
}

struct flattener {
  const snapshot&   snap;
  const view_state& view;
  std::vector<row>  rows;

  auto expanded(const std::string& key, bool by_default) const -> bool {
    return view.toggled.contains(key) ? !by_default : by_default;
  }

  auto add_task(const task_node& task, std::int64_t owner_claim, int depth) -> void {
    row                      r{.kind       = row_kind::task,
                               .key        = std::format("t:{}:{}", owner_claim, task.id),
                               .depth      = depth,
                               .text       = std::format("{} {}", task.id, task.title),
                               .detail     = {},
                               .dot        = std::nullopt,
                               .caret      = std::nullopt,
                               .status     = task.status,
                               .expandable = false,
                               .expanded   = false,
                               .dim        = task.status == "done" || task.status == "cancelled"};
    std::vector<std::string> others;
    for (auto const id : task.holders) {
      auto const& holder = snap.claims.at(id);
      if (!r.caret.has_value() || state_rank(holder.state) < state_rank(*r.caret))
        r.caret = holder.state;
      if (id == owner_claim)
        continue;
      auto label = agent_label(holder);
      if (holder.state == agent_state::stopped)
        label += std::format(" · {}", stopped_detail(holder, snap.generated_at));
      else if (holder.status_text.has_value())
        label += std::format(" · {}", *holder.status_text);
      others.push_back(std::move(label));
    }
    for (auto const& label : others)
      r.detail += r.detail.empty() ? label : std::format("; {}", label);
    rows.push_back(std::move(r));
  }

  auto add_plan(const plan_node& plan, std::int64_t owner_claim, int depth) -> void {
    int done = 0, total = 0;
    count_tasks(plan, done, total);
    auto const key  = std::format("p:{}:{}", owner_claim, plan.id);
    bool const open = expanded(key, contains_holder(plan));
    rows.push_back(row{.kind       = row_kind::plan,
                       .key        = key,
                       .depth      = depth,
                       .text       = plan.title,
                       .detail     = total > 0 ? std::format("{}/{} done", done, total) : std::string{},
                       .dot        = std::nullopt,
                       .caret      = std::nullopt,
                       .status     = plan.status,
                       .expandable = !plan.tasks.empty() || !plan.milestones.empty(),
                       .expanded   = open,
                       .dim        = plan.status == "done" || plan.status == "abandoned"});
    if (open)
      add_plan_children(plan, owner_claim, depth + 1);
  }

  auto add_plan_children(const plan_node& plan, std::int64_t owner_claim, int depth) -> void {
    for (auto const& task : plan.tasks)
      add_task(task, owner_claim, depth);
    for (auto const& child : plan.milestones)
      add_plan(child, owner_claim, depth);
  }

  auto add_agent(const agent_node& agent) -> void {
    auto const& claim = snap.claims.at(agent.claim_id);
    if (claim.state == agent_state::stopped && !view.show_stopped)
      return;

    std::string subject;
    if (!agent.path.empty())
      subject = agent.path.front();
    else if (agent.root.has_value())
      subject = agent.root->title;
    else
      subject = std::format("{} {}", claim.entity_kind, claim.entity_id);

    std::string detail;
    if (claim.state == agent_state::stopped)
      detail = stopped_detail(claim, snap.generated_at);
    else if (claim.status_text.has_value())
      detail = *claim.status_text;

    auto const key  = std::format("a:{}", claim.id);
    bool const has  = agent.root.has_value();
    bool const open = has && expanded(key, claim.state != agent_state::stopped);
    rows.push_back(row{.kind       = row_kind::agent,
                       .key        = key,
                       .depth      = 0,
                       .text       = std::format("{}  {}", agent_label(claim), subject),
                       .detail     = std::move(detail),
                       .dot        = claim.state,
                       .caret      = std::nullopt,
                       .status     = claim.status,
                       .expandable = has,
                       .expanded   = open,
                       .dim        = false});
    if (!open)
      return;
    // A plan with milestones lists its own tasks and milestones directly;
    // a leaf plan is shown as one node so its milestone name stays visible.
    if (agent.root->milestones.empty())
      add_plan(*agent.root, claim.id, 1);
    else
      add_plan_children(*agent.root, claim.id, 1);
  }
};

} // namespace

auto classify(const claim_info& claim, std::string_view now) -> agent_state {
  if (claim.status != "active" || claim.lease_expires_at < now)
    return agent_state::stopped;
  if (claim.status_text.has_value()) {
    std::string_view text = *claim.status_text;
    while (!text.empty() && text.front() == ' ')
      text.remove_prefix(1);
    if (text.starts_with("awaiting:"))
      return agent_state::waiting;
  }
  return agent_state::working;
}

auto build_snapshot(db::connection& conn, const options& opts) -> std::expected<snapshot, db::db_error> {
  // One short read transaction: every statement sees the same database
  // state, and nothing is held between refreshes. A failed BEGIN falls back
  // to per-statement reads rather than refusing service.
  bool const have_tx = conn.execute("BEGIN DEFERRED").has_value();
  auto       snap    = build_in_transaction(conn, opts);
  if (have_tx)
    static_cast<void>(conn.execute("COMMIT"));
  return snap;
}

auto now_timestamp() -> std::string {
  return format_timestamp(std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
}

auto timestamp_minus(std::string_view stamp, std::int64_t seconds) -> std::string {
  auto const at = parse_timestamp(stamp);
  if (!at.has_value())
    return std::string{stamp};
  return format_timestamp(*at - std::chrono::seconds{seconds});
}

auto short_age(std::string_view then, std::string_view now) -> std::string {
  auto const a = parse_timestamp(then);
  auto const b = parse_timestamp(now);
  if (!a.has_value() || !b.has_value())
    return {};
  auto const secs = std::chrono::duration_cast<std::chrono::seconds>(*b - *a).count();
  if (secs < 0)
    return {};
  if (secs < 60)
    return std::format("{}s", secs);
  if (secs < 3600)
    return std::format("{}m", secs / 60);
  if (secs < 86400)
    return std::format("{}h", secs / 3600);
  return std::format("{}d", secs / 86400);
}

auto flatten(const snapshot& snap, const view_state& view) -> std::vector<row> {
  flattener f{.snap = snap, .view = view, .rows = {}};
  for (auto const& agent : snap.agents)
    f.add_agent(agent);
  return std::move(f.rows);
}

} // namespace planar::cmd::watch::agents
