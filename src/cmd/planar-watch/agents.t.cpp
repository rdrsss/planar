// agents.t.cpp
// Tests for the interactive agent view's snapshot model and its launch gate.
//
// The model is driven against a real migrated database seeded with raw SQL,
// so every timestamp is fixed and classification is checked against a known
// `now`. The screen itself is not drawn here; `flatten` produces everything it
// shows.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handlers.agents.model;
import planar.cmd.planar_watch.handlers.agents.tui;

namespace {

namespace ag = planar::cmd::watch::agents;

constexpr std::string_view k_now = "2026-10-09T12:00:00.000Z";

struct scratch {
  std::filesystem::path root;
  std::filesystem::path db_path;
};

auto make_scratch(std::string_view tag) -> scratch {
  auto const root =
      std::filesystem::temp_directory_path() /
      std::format("planar_cmd_watch_agents_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(root);
  return scratch{.root = root, .db_path = root / "planar.db"};
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// One anchor plan (1) with two milestones (2: M1, 3: M2), tasks 10-12 in
/// M1 and 20-21 in M2, and one session.
auto seed_plans(const scratch& s) -> planar::db::connection {
  auto conn = planar::db::connection::open(s.db_path.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  exec(*conn, "insert into sessions (id, vendor) values (1, 'claude')");
  exec(*conn,
       "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'Host queue', 'host-queue', 'active')");
  exec(*conn, "insert into plans (id, scope_kind, title, slug, status, parent_plan_id) "
              "values (2, 'global', 'M1 — Queue engine', 'm1', 'active', 1)");
  exec(*conn, "insert into plans (id, scope_kind, title, slug, status, parent_plan_id) "
              "values (3, 'global', 'M2 — Queue CLI', 'm2', 'active', 1)");
  exec(*conn, "insert into tasks (id, scope_kind, plan_id, title, status, priority) values "
              "(10, 'global', 2, 'Tables', 'done', 100), "
              "(11, 'global', 2, 'Enqueue verb', 'doing', 101), "
              "(12, 'global', 2, 'History view', 'todo', 102), "
              "(20, 'global', 3, 'Wait verb', 'todo', 100), "
              "(21, 'global', 3, 'Cancel verb', 'todo', 101)");
  return std::move(*conn);
}

struct claim_seed {
  std::int64_t               id;
  std::string_view           role;
  std::string_view           kind;
  std::int64_t               entity;
  std::string_view           status   = "active";
  std::string_view           expires  = "2026-10-09T12:10:00.000Z";
  std::optional<std::string> released = std::nullopt;
};

auto add_claim(planar::db::connection& conn, const claim_seed& c) -> void {
  exec(conn, std::format("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, status, vendor, "
                         "role, claimed_at, last_heartbeat_at, lease_expires_at, released_at) values ({}, 'tok{}', 1, '{}', {}, "
                         "'{}', 'claude', '{}', '2026-10-09T11:00:00.000Z', '2026-10-09T11:55:00.000Z', '{}', {})",
                         c.id, c.id, c.kind, c.entity, c.status, c.role, c.expires,
                         c.released.has_value() ? std::format("'{}'", *c.released) : std::string{"null"}));
}

auto add_heartbeat(planar::db::connection& conn, std::int64_t claim_id, std::string_view summary) -> void {
  exec(conn, std::format("insert into agent_actions (session_id, claim_id, action_kind, vendor, summary) "
                         "values (1, {}, 'heartbeat', 'claude', '{}')",
                         claim_id, summary));
}

auto snapshot_of(const scratch& s) -> ag::snapshot {
  auto conn = planar::db::connection::open_read_only(s.db_path.string());
  REQUIRE(conn.has_value());
  auto snap = ag::build_snapshot(*conn, ag::options{.now = std::string{k_now}, .stopped_since = "2026-10-09T11:00:00.000Z"});
  REQUIRE(snap.has_value());
  return std::move(*snap);
}

auto find_row(const std::vector<ag::row>& rows, std::string_view key) -> const ag::row* {
  for (auto const& r : rows)
    if (r.key == key)
      return &r;
  return nullptr;
}

auto claim_with(std::string_view status, std::string_view expires, std::optional<std::string> text) -> ag::claim_info {
  ag::claim_info c;
  c.status           = std::string{status};
  c.lease_expires_at = std::string{expires};
  c.status_text      = std::move(text);
  return c;
}

} // namespace

TEST_CASE("agents: classify maps claim state to a light", "[cmd][watch][agents]") {
  CHECK(ag::classify(claim_with("active", "2026-10-09T12:10:00.000Z", std::nullopt), k_now) == ag::agent_state::working);
  CHECK(ag::classify(claim_with("active", "2026-10-09T12:10:00.000Z", "reading brief"), k_now) == ag::agent_state::working);
  CHECK(ag::classify(claim_with("active", "2026-10-09T12:10:00.000Z", "awaiting:coder"), k_now) == ag::agent_state::waiting);
  CHECK(ag::classify(claim_with("active", "2026-10-09T12:10:00.000Z", "  awaiting: review"), k_now) == ag::agent_state::waiting);
  // An expired lease is stopped even with an awaiting status.
  CHECK(ag::classify(claim_with("active", "2026-10-09T11:59:59.999Z", "awaiting:coder"), k_now) == ag::agent_state::stopped);
  CHECK(ag::classify(claim_with("completed", "2026-10-09T12:10:00.000Z", std::nullopt), k_now) == ag::agent_state::stopped);
  CHECK(ag::classify(claim_with("aborted", "2026-10-09T12:10:00.000Z", std::nullopt), k_now) == ag::agent_state::stopped);
}

TEST_CASE("agents: a coder's task claim nests under the orchestrator holding its anchor plan", "[cmd][watch][agents]") {
  auto const s    = make_scratch("nest");
  auto       conn = seed_plans(s);
  add_claim(conn, {.id = 1, .role = "orchestrator", .kind = "plan", .entity = 1});
  add_claim(conn, {.id = 2, .role = "coder", .kind = "task", .entity = 11});
  add_heartbeat(conn, 1, "dispatching coder: task 11");
  add_heartbeat(conn, 1, "awaiting:coder task 11");
  add_heartbeat(conn, 2, "reading brief");

  auto const snap = snapshot_of(s);
  REQUIRE(snap.agents.size() == 1);
  CHECK(snap.agents[0].claim_id == 1);
  CHECK(snap.claims.at(1).state == ag::agent_state::waiting); // latest heartbeat wins
  CHECK(snap.claims.at(2).state == ag::agent_state::working);

  auto const  rows  = ag::flatten(snap, {});
  auto const* agent = find_row(rows, "a:1");
  REQUIRE(agent != nullptr);
  CHECK(agent->dot == ag::agent_state::waiting);
  CHECK(agent->expanded);
  CHECK(agent->text.contains("orchestrator"));
  CHECK(agent->text.contains("Host queue"));
  CHECK(agent->detail == "awaiting:coder task 11");

  // M1 holds the claimed task, so it opens by default; M2 does not.
  auto const* m1 = find_row(rows, "p:1:2");
  auto const* m2 = find_row(rows, "p:1:3");
  REQUIRE(m1 != nullptr);
  REQUIRE(m2 != nullptr);
  CHECK(m1->expanded);
  CHECK(m1->detail == "1/3 done");
  CHECK_FALSE(m2->expanded);
  CHECK(find_row(rows, "t:1:20") == nullptr);

  auto const* held = find_row(rows, "t:1:11");
  REQUIRE(held != nullptr);
  CHECK(held->caret == ag::agent_state::working);
  CHECK(held->detail == "claude · coder · reading brief");
  auto const* free = find_row(rows, "t:1:12");
  REQUIRE(free != nullptr);
  CHECK_FALSE(free->caret.has_value());
  CHECK(find_row(rows, "t:1:10")->dim);
}

TEST_CASE("agents: a task claim with no orchestrator above it gets its own row", "[cmd][watch][agents]") {
  auto const s    = make_scratch("standalone");
  auto       conn = seed_plans(s);
  add_claim(conn, {.id = 5, .role = "coder", .kind = "task", .entity = 20});

  auto const snap = snapshot_of(s);
  REQUIRE(snap.agents.size() == 1);
  CHECK(snap.agents[0].task_id == 20);
  CHECK(snap.agents[0].path == std::vector<std::string>{"Host queue"});

  auto const rows = ag::flatten(snap, {});
  REQUIRE(rows.size() == 4); // agent, M2, two tasks
  CHECK(rows[0].key == "a:5");
  CHECK(rows[0].text.contains("Host queue"));
  CHECK(rows[1].key == "p:5:3");
  CHECK(rows[1].text == "M2 — Queue CLI");
  CHECK(rows[2].key == "t:5:20");
  CHECK(rows[2].caret == ag::agent_state::working);
  CHECK(rows[2].detail.empty()); // its own claim is named on the agent row
  CHECK_FALSE(rows[3].caret.has_value());
}

TEST_CASE("agents: stopped claims sort last, age out, and can be hidden", "[cmd][watch][agents]") {
  auto const s    = make_scratch("stopped");
  auto       conn = seed_plans(s);
  add_claim(
      conn,
      {.id = 1, .role = "reviewer", .kind = "task", .entity = 21, .status = "completed", .released = "2026-10-09T11:46:00.000Z"});
  add_claim(conn, {.id = 2, .role = "coder", .kind = "task", .entity = 20});
  // Released before the window: not listed at all.
  add_claim(
      conn,
      {.id = 3, .role = "coder", .kind = "task", .entity = 12, .status = "completed", .released = "2026-10-09T09:00:00.000Z"});
  // Active row whose lease expired inside the window: stopped.
  add_claim(conn, {.id = 4, .role = "coder", .kind = "task", .entity = 10, .expires = "2026-10-09T11:30:00.000Z"});

  auto const snap = snapshot_of(s);
  REQUIRE(snap.agents.size() == 3);
  CHECK(snap.agents[0].claim_id == 2);
  CHECK_FALSE(snap.claims.contains(3));

  auto const  rows = ag::flatten(snap, {});
  auto const* done = find_row(rows, "a:1");
  REQUIRE(done != nullptr);
  CHECK(done->dot == ag::agent_state::stopped);
  CHECK_FALSE(done->expanded);
  CHECK(done->detail == "completed 14m ago");
  CHECK(find_row(rows, "a:4")->detail == "lease expired 30m ago");

  ag::view_state hidden;
  hidden.show_stopped = false;
  auto const live     = ag::flatten(snap, hidden);
  CHECK(find_row(live, "a:1") == nullptr);
  CHECK(find_row(live, "a:4") == nullptr);
  CHECK(find_row(live, "a:2") != nullptr);
}

TEST_CASE("agents: a finished coder claim is not marked under its orchestrator", "[cmd][watch][agents]") {
  auto const s    = make_scratch("finished");
  auto       conn = seed_plans(s);
  add_claim(conn, {.id = 1, .role = "orchestrator", .kind = "plan", .entity = 1});
  add_claim(
      conn,
      {.id = 2, .role = "coder", .kind = "task", .entity = 10, .status = "completed", .released = "2026-10-09T11:50:00.000Z"});
  add_claim(
      conn,
      {.id = 3, .role = "coder", .kind = "task", .entity = 11, .status = "aborted", .released = "2026-10-09T11:50:00.000Z"});

  auto const snap = snapshot_of(s);
  REQUIRE(snap.agents.size() == 1); // both coders nest or drop; neither gets a row
  auto const rows = ag::flatten(snap, {});
  CHECK_FALSE(find_row(rows, "t:1:10")->caret.has_value());
  auto const* aborted = find_row(rows, "t:1:11");
  REQUIRE(aborted != nullptr);
  CHECK(aborted->caret == ag::agent_state::stopped);
  CHECK(aborted->detail.starts_with("claude · coder · aborted"));
}

TEST_CASE("agents: a toggled key flips its row from the default", "[cmd][watch][agents]") {
  auto const s    = make_scratch("toggle");
  auto       conn = seed_plans(s);
  add_claim(conn, {.id = 1, .role = "orchestrator", .kind = "plan", .entity = 1});
  add_claim(conn, {.id = 2, .role = "coder", .kind = "task", .entity = 11});
  auto const snap = snapshot_of(s);

  ag::view_state view;
  view.toggled.insert("p:1:3"); // open M2
  view.toggled.insert("p:1:2"); // close M1
  auto const rows = ag::flatten(snap, view);
  CHECK(find_row(rows, "p:1:3")->expanded);
  CHECK(find_row(rows, "t:1:20") != nullptr);
  CHECK_FALSE(find_row(rows, "p:1:2")->expanded);
  CHECK(find_row(rows, "t:1:11") == nullptr);

  view.toggled = {"a:1"}; // close the agent
  CHECK(ag::flatten(snap, view).size() == 1);
}

TEST_CASE("agents: an orchestrator on a milestone shows that milestone as one node", "[cmd][watch][agents]") {
  auto const s    = make_scratch("milestone");
  auto       conn = seed_plans(s);
  add_claim(conn, {.id = 1, .role = "orchestrator", .kind = "plan", .entity = 2});
  add_claim(conn, {.id = 2, .role = "coder", .kind = "task", .entity = 12});

  auto const snap = snapshot_of(s);
  REQUIRE(snap.agents.size() == 1);
  auto const rows = ag::flatten(snap, {});
  REQUIRE(rows.size() == 5); // agent, M1, three tasks
  CHECK(rows[0].text.contains("Host queue"));
  CHECK(rows[1].key == "p:1:2");
  CHECK(rows[4].key == "t:1:12");
  CHECK(rows[4].caret == ag::agent_state::working);
}

TEST_CASE("agents: timestamps and ages", "[cmd][watch][agents]") {
  CHECK(ag::timestamp_minus("2026-10-09T12:00:00.000Z", 3600) == "2026-10-09T11:00:00.000Z");
  CHECK(ag::timestamp_minus("2026-10-01T00:00:30.250Z", 60) == "2026-09-30T23:59:30.250Z");
  CHECK(ag::timestamp_minus("garbage", 60) == "garbage");
  CHECK(ag::short_age("2026-10-09T11:59:15.000Z", k_now) == "45s");
  CHECK(ag::short_age("2026-10-09T09:00:00.000Z", k_now) == "3h");
  CHECK(ag::short_age("2026-10-07T12:00:00.000Z", k_now) == "2d");
  CHECK(ag::short_age("2026-10-09T13:00:00.000Z", k_now).empty());
  CHECK(ag::now_timestamp().size() == 24);
}

TEST_CASE("agents: only a bare invocation on a capable terminal opens the view", "[cmd][watch][agents]") {
  auto const env  = planar::cmd::watch::map_env({{"TERM", "xterm-256color"}});
  auto const bare = std::vector<std::string>{"planar-watch"};
  CHECK(ag::wants_interactive(bare, true, true, env));
  CHECK_FALSE(ag::wants_interactive(std::vector<std::string>{"planar-watch", "--json"}, true, true, env));
  CHECK_FALSE(ag::wants_interactive(std::vector<std::string>{"planar-watch", "feed"}, true, true, env));
  CHECK_FALSE(ag::wants_interactive(bare, false, true, env));
  CHECK_FALSE(ag::wants_interactive(bare, true, false, env));
  CHECK_FALSE(ag::wants_interactive(bare, true, true, planar::cmd::watch::map_env({{"TERM", "dumb"}})));
  CHECK(ag::wants_interactive(bare, true, true, planar::cmd::watch::map_env({})));
  // A string stream is never the terminal, whatever the test runner's stdout is.
  std::ostringstream out;
  CHECK_FALSE(ag::stdout_is_tty(out));
}
