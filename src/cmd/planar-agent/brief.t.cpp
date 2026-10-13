// @file brief.t.cpp
// @brief Tests for `planar.cmd.planar_agent.brief` (plan 1142, task mo-brief-engine):
// the shell-quoting helper, the protocol templates, and the composition of the
// brief on a fixture database with a 20-task milestone, a live claim and a
// stale claim.
//
// What the cases pin: the shell round trip catches an unescaped embedded quote, the
// protocol cases catch a queue template without `use_queue`, and the read-only
// connection catches any write from `gather_brief`.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.json_dom;
import planar.engine.config;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.brief;

namespace {

namespace agent = planar::cmd::agent;
namespace aa    = planar::engine::runtime::agentactivity;
namespace cfg   = planar::engine::config;

struct scratch_db {
  std::filesystem::path path_;

  scratch_db()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_brief_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db(const scratch_db&)            = delete;
  scratch_db& operator=(const scratch_db&) = delete;

  ~scratch_db() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  [[nodiscard]] auto str() const -> std::string {
    return path_.string();
  }
};

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto done = conn.execute(sql);
  INFO(sql);
  REQUIRE(done.has_value());
}

auto scalar(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto parse_config(std::string_view text) -> cfg::toml_map {
  auto parsed = cfg::parse_toml(text);
  REQUIRE(parsed.has_value());
  return *parsed;
}

/// Run `/bin/sh -c 'printf %s <word-quoted>'` and return what the shell printed.
auto shell_echo(std::string_view quoted_word) -> std::string {
  auto const out_path = std::filesystem::temp_directory_path() /
                        std::format("planar_brief_sh_{}.out", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const script   = std::format("printf %s {}", quoted_word);
  auto const command  = std::format("/bin/sh -c {} > {}", agent::shell_quote(script), agent::shell_quote(out_path.string()));
  REQUIRE(std::system(command.c_str()) == 0);
  std::ifstream   in(out_path, std::ios::binary);
  std::string     got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  return got;
}

/// The fixture: anchor plan 1, milestone plan 2 (20 todo tasks 1..20, a live claim on doing task 21, a stale claim on
/// doing task 22, a done task 23), milestone plan 3 (one task 24).
///
/// Tasks 1..12 declare distinct touch paths and are rule-clean. 13 depends on 14 (rule 1). 15 has an open question (rule 5).
/// 16 has a proposed decision (rule 6). 17 and 18 declare the same path (rule 2). 19 touches a migration (rule 3). 20 touches a
/// singleton file (rule 4). Task 14, the blocker of 13, is itself eligible. Task 12 cites a superseded artifact, so only its
/// packet is unready (rule 7).
auto build_fixture(planar::db::connection& conn) -> void {
  REQUIRE(planar::db::apply_all(conn).has_value());
  exec(conn, "insert into projects (id, slug, name) values (1, 'repo-one', 'repo-one')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status) values (1, 'global', 'Anchor', 'p-anchor', 'active')");
  exec(conn, "insert into plans (id, scope_kind, title, slug, status, parent_plan_id) values "
             "(2, 'global', 'Milestone one', 'm1', 'active', 1), (3, 'global', 'Milestone two', 'm2', 'draft', 1)");
  for (int i = 1; i <= 20; ++i) {
    exec(conn, std::format("insert into tasks (id, scope_kind, plan_id, title, slug, status, priority) values "
                           "({0}, 'global', 2, 'Task {0}', 'task-{0}', 'todo', {1})",
                           i, 100 + i));
  }
  exec(conn,
       "insert into tasks (id, scope_kind, plan_id, title, slug, status, priority) values "
       "(21, 'global', 2, 'Held', 'held-live', 'doing', 200), (22, 'global', 2, 'Lapsed', 'held-stale', 'doing', 201), "
       "(23, 'global', 2, 'Finished', 'finished', 'done', 202), (24, 'global', 3, 'Next milestone', 'next-m2', 'todo', 300)");
  for (int i = 1; i <= 12; ++i) {
    exec(conn, std::format("insert into task_touch_paths (task_id, repo_id, path) values ({0}, 1, 'src/area{0}/file.cpp')", i));
  }
  exec(conn,
       "insert into task_touch_paths (task_id, repo_id, path) values "
       "(13, 1, 'src/area13/a.cpp'), (14, 1, 'src/area14/a.cpp'), (15, 1, 'src/area15/a.cpp'), (16, 1, 'src/area16/a.cpp'), "
       "(17, 1, 'src/shared/x.cpp'), (18, 1, 'src/shared/x.cpp'), (19, 1, 'migrations/00099_x.sql'), (20, 1, "
       "'docs/architecture.md')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 13, 'task', 14, "
             "'depends-on')");
  exec(conn, "insert into questions (scope_kind, title, status) values ('global', 'open q', 'open')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 15, 'question', last_insert_rowid(), 'addresses')");
  exec(conn, "insert into decisions (scope_kind, title, body, status) values ('global', 'proposed d', 'body', 'proposed')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 16, 'decision', last_insert_rowid(), 'addresses')");
  exec(conn, "insert into artifacts (scope_kind, kind, title, body, status) values ('global', 'tech_spec', 'old spec', 'b', "
             "'superseded')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('task', 12, 'artifact', last_insert_rowid(), 'cites')");

  auto const session = planar::engine::runtime::session::ensure_active(conn, "claude", std::nullopt);
  REQUIRE(session.has_value());
  auto const worktree = std::string{"/tmp/it's a worktree"};
  auto const live     = aa::acquire_claim(conn, aa::acquire_args{.session_id    = *session,
                                                                 .kind          = aa::entity_kind::task,
                                                                 .entity_id     = 21,
                                                                 .vendor        = "claude",
                                                                 .role          = "coder",
                                                                 .model         = "claude-opus",
                                                                 .worktree_path = worktree,
                                                                 .ttl_secs      = 7200});
  REQUIRE(live.has_value());
  auto const stale = aa::acquire_claim(conn, aa::acquire_args{.session_id = *session,
                                                              .kind       = aa::entity_kind::task,
                                                              .entity_id  = 22,
                                                              .vendor     = "claude",
                                                              .role       = "reviewer",
                                                              .ttl_secs   = -100});
  REQUIRE(stale.has_value());
}

auto row_totals(planar::db::connection& conn) -> std::vector<std::int64_t> {
  return {scalar(conn, "select count(*) from tasks"),         scalar(conn, "select count(*) from agent_work_claims"),
          scalar(conn, "select count(*) from agent_actions"), scalar(conn, "select count(*) from sessions"),
          scalar(conn, "select count(*) from plans"),         scalar(conn, "select count(*) from audit_log"),
          scalar(conn, "select count(*) from entity_links"),  scalar(conn, "select count(*) from queue_entries")};
}

} // namespace

TEST_CASE("shell_quote wraps a word in single quotes and writes an embedded quote as '\\''", "[cmd][agent][brief][shell_quote]") {
  CHECK(agent::shell_quote("abc") == "'abc'");
  CHECK(agent::shell_quote("") == "''");
  CHECK(agent::shell_quote("a b") == "'a b'");
  CHECK(agent::shell_quote("it's") == "'it'\\''s'");
  CHECK(agent::shell_quote("'") == "''\\'''");
  CHECK(agent::shell_quote("a\nb") == "'a\nb'");
  CHECK(agent::shell_quote("$(touch x)`id`") == "'$(touch x)`id`'");
}

TEST_CASE("shell_quote round-trips hostile words through /bin/sh", "[cmd][agent][brief][shell_quote]") {
  for (std::string_view word :
       {"plain", "", "has space", "it's", "''", "a'b'c", "$(echo pwned)", "`echo pwned`", "$HOME", "a;b&c|d", "tab\there",
        "line\nbreak", "back\\slash", "\"dq\"", "*?[x]", "~/path", "--flag=1", "-n"}) {
    INFO("word: " << word);
    CHECK(shell_echo(agent::shell_quote(word)) == word);
  }
}

TEST_CASE("format_duration prints the shortest exact unit", "[cmd][agent][brief]") {
  CHECK(agent::format_duration(3600) == "1h");
  CHECK(agent::format_duration(7200) == "2h");
  CHECK(agent::format_duration(90) == "90s");
  CHECK(agent::format_duration(120) == "2m");
  CHECK(agent::format_duration(0) == "0s");
  CHECK(agent::format_duration(-5) == "0s");
}

TEST_CASE("the protocol derives the heartbeat interval from the default lease and names the four terminal verbs",
          "[cmd][agent][brief][protocol]") {
  auto const protocol = agent::build_protocol(cfg::policy_values{}, aa::default_lease_secs);
  CHECK(protocol.lease_ttl_secs == 7200);
  CHECK(protocol.heartbeat_every_secs == 3600);
  std::map<std::string, std::string> by_name;
  for (auto const& t : protocol.templates) {
    by_name[t.name] = t.command;
  }
  CHECK(by_name.at("claim") ==
        "planar-agent claim --entity task:<id> --role coder --model '<model>' --ttl 7200 --worktree '<path>'");
  CHECK(by_name.at("heartbeat") == "planar-agent heartbeat --claim '<token>'");
  for (auto const* verb : {"complete", "fail", "release", "block"}) {
    CHECK(by_name.at(verb).starts_with(std::format("planar-agent {} --claim '<token>'", verb)));
  }
  CHECK(by_name.at("review") ==
        "planar-agent review --claim '<reviewed-token>' --verdict approve|request-changes --summary '<text>'");
  CHECK_FALSE(by_name.contains("gate"));
}

TEST_CASE("queue templates appear only under use_queue and quote each gate element", "[cmd][agent][brief][protocol]") {
  auto gate_of = [](const cfg::policy_values& values) -> std::optional<std::string> {
    for (auto const& t : agent::build_protocol(values, 7200).templates) {
      if (t.name == "gate") {
        return t.command;
      }
    }
    return std::nullopt;
  };
  cfg::policy_values values;
  values.task_gate = std::vector<std::string>{"make", "test"};
  CHECK_FALSE(gate_of(values).has_value()); // a gate with use_queue = false shows no queue template

  values.use_queue = true;
  CHECK(gate_of(values) == "planar-agent queue run --claim '<token>' -- 'make' 'test'");

  values.task_gate = std::vector<std::string>{"sh", "-c", "echo 'hi' $HOME"};
  CHECK(gate_of(values) == "planar-agent queue run --claim '<token>' -- 'sh' '-c' 'echo '\\''hi'\\'' $HOME'");

  values.task_gate.reset();
  CHECK(gate_of(values) == "planar-agent queue run --claim '<token>' -- <command>");
}

TEST_CASE("the brief of a 20-task milestone is complete, read-only and under 4 KB", "[cmd][agent][brief][compose]") {
  scratch_db scratch;
  {
    auto writer = planar::db::connection::open(scratch.str());
    REQUIRE(writer.has_value());
    build_fixture(*writer);
  }
  auto reader = planar::db::connection::open_read_only(scratch.str());
  REQUIRE(reader.has_value());
  auto const before = row_totals(*reader);

  auto const gathered = agent::gather_brief(*reader, 2, {});
  REQUIRE(gathered.has_value());
  auto const& brief = *gathered;
  auto const  text  = agent::render_brief_text(brief);
  INFO(text);

  // Layout facts.
  CHECK(brief.counts.todo == 20);
  CHECK(brief.counts.doing == 2);
  CHECK(brief.counts.done == 1);
  REQUIRE(brief.anchor.has_value());
  CHECK(brief.anchor->slug == "p-anchor");
  REQUIRE(brief.milestones.size() == 2);
  CHECK(brief.milestones[0].plan.slug == "m1");
  CHECK(brief.milestones[1].plan.slug == "m2");

  // Claims: one live, one stale, with role, model and worktree.
  REQUIRE(brief.claims.size() == 2);
  CHECK(brief.claims[0].state == "live");
  CHECK(brief.claims[0].entity == "task:21");
  CHECK(brief.claims[0].role == std::optional<std::string>{"coder"});
  CHECK(brief.claims[0].model == std::optional<std::string>{"claude-opus"});
  CHECK(brief.claims[0].worktree == std::optional<std::string>{"/tmp/it's a worktree"});
  CHECK(brief.claims[1].state == "stale");
  CHECK(brief.claims[1].entity == "task:22");

  // The wave is tasks 1..11 and 14 (13 waits on it; task 24 belongs to the other milestone); 12 is rule 7; 13..20 are serialized
  // with their rules.
  std::vector<std::int64_t> wave;
  for (auto const& t : brief.wave) {
    wave.push_back(t.id);
  }
  CHECK(wave == std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14});
  CHECK(brief.fan_out_available);
  std::map<std::int64_t, std::vector<int>> rules;
  for (auto const& t : brief.serialized) {
    for (auto const& why : t.why) {
      rules[t.id].push_back(why.rule);
    }
  }
  CHECK(rules.at(12) == std::vector<int>{7});
  CHECK(rules.at(13) == std::vector<int>{1});
  CHECK(rules.at(15) == std::vector<int>{5});
  CHECK(rules.at(16) == std::vector<int>{6});
  CHECK(rules.at(17) == std::vector<int>{2});
  CHECK(rules.at(18) == std::vector<int>{2});
  CHECK(rules.at(19) == std::vector<int>{3});
  CHECK(rules.at(20) == std::vector<int>{4});

  // Readiness: every unclaimed todo task, with reasons.
  std::map<std::int64_t, std::vector<std::string>> reasons;
  for (auto const& r : brief.readiness) {
    reasons[r.id] = r.reasons;
  }
  CHECK(reasons.size() == 20);
  CHECK(reasons.at(1).empty());
  CHECK(reasons.at(13) == std::vector<std::string>{"blocked_by_open_task"});
  CHECK(reasons.at(15) == std::vector<std::string>{"open_question_linked"});
  CHECK(reasons.at(16) == std::vector<std::string>{"proposed_decision_linked"});
  CHECK(reasons.at(12) == std::vector<std::string>{"unresolved_citation"});

  // Text content and size.
  CHECK(text.size() < 4096);
  CHECK(text.contains("brief v1 plan 2 m1 [active]"));
  CHECK(text.contains("anchor: 1 p-anchor"));
  CHECK(text.contains("live #"));
  CHECK(text.contains("stale #"));
  CHECK(text.contains("worktree='/tmp/it'\\''s a worktree'"));
  CHECK(text.contains("wave (12 parallel):"));
  CHECK(text.contains("serialized ("));
  CHECK(text.contains("13 task-13: rule 1: blocked_by not-done task 14"));
  CHECK(text.contains("12 task-12: packet not ready: unresolved_citation"));
  CHECK(text.contains("not ready:"));
  CHECK(text.contains("15 task-15: open_question_linked"));
  CHECK(text.contains("review_required=false [default]"));
  CHECK(text.contains(std::format("policy digest {}", brief.policy.digest)));
  CHECK(text.contains("heartbeat every 1h"));
  CHECK(text.contains("planar-agent heartbeat --claim '<token>'"));
  CHECK(text.contains("planar-agent review --claim '<reviewed-token>' --verdict approve|request-changes --summary '<text>'"));
  CHECK_FALSE(text.contains("queue run"));
  CHECK_FALSE(text.contains("confirm before"));

  // Read-only: the connection could not write, and nothing changed.
  CHECK(row_totals(*reader) == before);
}

TEST_CASE("the brief reports the policy source and shows the gate only under use_queue", "[cmd][agent][brief][policy]") {
  scratch_db scratch;
  auto       conn = planar::db::connection::open(scratch.str());
  REQUIRE(conn.has_value());
  build_fixture(*conn);
  exec(*conn, "insert into associations (id, slug, name, kind) values (1, 'acme', 'Acme', 'org')");
  exec(*conn, "update plans set scope_kind = 'association', scope_id = 1 where id in (1, 2, 3)");

  SECTION("an association table wins, a top-level key shows its table, and use_queue renders the gate") {
    auto const config   = parse_config(R"([policy]
task_gate = ["make", "test"]
use_queue = true
confirm_before = ["task done"]
[associations."acme".policy]
review_required = true
)");
    auto const gathered = agent::gather_brief(*conn, 2, config);
    REQUIRE(gathered.has_value());
    CHECK(gathered->policy.valid);
    CHECK(gathered->policy.association == std::optional<std::string>{"acme"});
    CHECK(gathered->policy.resolved.values.review_required);
    CHECK(gathered->confirm_before == std::vector<std::string>{"task done"});
    auto const text = agent::render_brief_text(*gathered);
    CHECK(text.contains("scope 'acme' (association_scope)"));
    CHECK(text.contains("review_required=true [associations.\"acme\".policy]"));
    CHECK(text.contains("use_queue=true [policy]"));
    CHECK(text.contains("planar-agent queue run --claim '<token>' -- 'make' 'test'"));
    CHECK(text.contains("confirm before: task done"));
    CHECK(text.size() < 4096);
  }
  SECTION("without use_queue no queue template renders even when a gate is set, and the policy is invalid") {
    auto const config   = parse_config(R"([policy]
task_gate = ["make", "test"]
)");
    auto const gathered = agent::gather_brief(*conn, 2, config);
    REQUIRE(gathered.has_value());
    CHECK_FALSE(gathered->policy.valid);
    REQUIRE(gathered->policy.findings.size() == 1);
    CHECK(gathered->policy.findings[0].key == "policy.task_gate");
    auto const text = agent::render_brief_text(*gathered);
    CHECK(text.contains("task_gate=[\"make\",\"test\"] [policy]"));
    CHECK_FALSE(text.contains("queue run"));
  }
  SECTION("an invalid config document still renders and says so") {
    auto const config   = parse_config("[policy]\nafter_review = \"merge\"\n");
    auto const gathered = agent::gather_brief(*conn, 2, config);
    REQUIRE(gathered.has_value());
    CHECK_FALSE(gathered->policy.valid);
    CHECK_FALSE(gathered->policy.findings.empty());
    CHECK(agent::render_brief_text(*gathered).contains("INVALID config"));
  }
  SECTION("a missing plan is plan_not_found") {
    auto const gathered = agent::gather_brief(*conn, 999, {});
    REQUIRE_FALSE(gathered.has_value());
    CHECK(gathered.error() == agent::brief_error::plan_not_found);
  }
}

TEST_CASE("the brief JSON is valid, versioned and has a stable key order", "[cmd][agent][brief][json]") {
  scratch_db scratch;
  auto       conn = planar::db::connection::open(scratch.str());
  REQUIRE(conn.has_value());
  build_fixture(*conn);

  auto const config   = parse_config(R"([policy]
use_queue = true
task_gate = ["sh", "-c", "echo 'x' \"y\""]
)");
  auto const gathered = agent::gather_brief(*conn, 2, config);
  REQUIRE(gathered.has_value());
  auto const json = agent::render_brief_json(*gathered);
  REQUIRE(json.ends_with("\n"));
  auto const parsed = planar::json_dom::parse_json(json);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->kind == planar::json_dom::json_kind::object);

  std::vector<std::string> keys;
  for (auto const& [name, value] : parsed->object) {
    keys.push_back(name);
  }
  CHECK(keys == std::vector<std::string>{"brief_version", "plan", "anchor", "counts", "milestones", "claims", "wave", "readiness",
                                         "policy", "confirm_before", "protocol"});
  CHECK(parsed->find("brief_version")->integer == 1);
  CHECK(parsed->find("claims")->array.size() == 2);
  CHECK(parsed->find("wave")->find("parallel")->array.size() == 12);
  CHECK(parsed->find("readiness")->array.size() == 20);
  auto const* policy = parsed->find("policy");
  REQUIRE(policy != nullptr);
  CHECK(policy->find("digest")->string == gathered->policy.digest);
  CHECK(policy->find("values")->find("use_queue")->boolean);
  CHECK(policy->find("sources")->find("use_queue")->find("kind")->string == "top_level");
  CHECK(parsed->find("confirm_before")->array.empty());
  bool saw_gate = false;
  for (auto const& t : parsed->find("protocol")->find("templates")->array) {
    if (t.find("name")->string == "gate") {
      saw_gate = true;
      CHECK(t.find("command")->string == "planar-agent queue run --claim '<token>' -- 'sh' '-c' 'echo '\\''x'\\'' \"y\"'");
    }
  }
  CHECK(saw_gate);

  // Rendering twice gives the same bytes.
  CHECK(agent::render_brief_json(*gathered) == json);
}
