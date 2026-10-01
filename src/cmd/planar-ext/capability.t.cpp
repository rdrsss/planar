// @file capability.t.cpp
// @brief `planar-ext`'s capability boundary (plan 996, task 6418; decision
// 995).
//
// Two independent things are pinned here, and neither subsumes the other
// — the same "two levels" shape the other three binaries' capability
// tests use (see e.g. `src/cmd/planar-watch/capability.t.cpp`'s header):
//
//   level 1  THE VERB SET. This task lands exactly `version` and `schema`
//            — no `ext`/`sync` verb exists here yet, and `tree.t.cpp`-
//            shaped coverage of that is folded into this file below
//            because there is, for now, only one tree-shape case worth
//            having: the declared surface is EXACTLY {version, schema}.
//
//   level 2  THE WRITE-CAPABILITY BOUNDARY (decision 995): read-write on
//            exactly `external_links`, `external_systems`, `sync_events`;
//            every other table read-only. THIS is the interesting half of
//            this file, and it is deliberately NOT a source-text check.
//
// ## Why level 2 asserts against PREPARED statements, not source text
//
// The task brief for this binary records a live measurement error: an
// earlier accounting of this exact write surface was WRONG because
// `src/engine/external/sync.cpp:352` composes its `UPDATE` with a
// table name interpolated at RUNTIME (`table_for` at that file's `:39`
// maps to `tasks`/`plans`/`questions`/`artifacts`) — a grep for
// `update plans` never finds it, because the literal text `update plans`
// never appears in the source.
//
// No verb has moved into this binary yet (this task's whole point), so
// there is nothing for a black-box, spawn-the-binary test to observe: a
// test that only checked "no verb currently writes a forbidden table"
// would pass identically whether the boundary is enforced or entirely
// absent — exactly the false-positive fixture shape this milestone keeps
// finding (six instances before this one). So this file does not wait for
// a verb to exist. It drives `planar.cmd.planar_ext.context::ensure_db`
// directly against a real, migrated, on-disk SQLite database, then
// attempts real writes through the SAME connection a future handler would
// get — including one composed by runtime string interpolation, exactly
// reproducing `sync.cpp`'s own shape — and asserts on whether SQLite's
// authorizer denies or permits each one at `prepare()`. That is real
// enforcement, not an inventory of what happens to be wired today, and it
// protects every verb tasks 6419-6421 move in afterward without those
// tasks having to remember to test the boundary themselves.
//
// Break-probes run against this file's write-boundary cases:
//
//   - `context.cpp`'s `restrict_writes_to` call REMOVED entirely ->
//     `ensure_db() enforces the decision-995 write allowlist` FAILS (the
//     forbidden INSERT now succeeds). Restored -> green.
//   - `write_allowlist()` changed to also include `"tasks"` -> the same
//     case FAILS (the interpolated `update tasks` now succeeds). Restored
//     -> green.
//   - `write_allowlist_authorizer` (src/lib/db/db.cpp) changed to `return
//     SQLITE_OK` unconditionally for INSERT/UPDATE/DELETE -> FAILS.
//     Restored -> green.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.db.migrate;
import planar.cliapp.walk;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.dispatch;
import planar.cmd.planar_ext.main;

namespace {

/// @brief A unique scratch database path under the system temp directory,
/// removed (best-effort, including SQLite's `-journal`/`-wal`/`-shm`
/// sidecars) when the guard goes out of scope. Mirrors db.t.cpp /
/// migrate.t.cpp's own helper — duplicated rather than shared because this
/// codebase has no header tree for first-party code (modules-only).
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ext_capability_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

/// @brief A migrated scratch database plus a `planar-ext` context already
/// pointed at it, ready for `ensure_db()`.
struct fixture {
  scratch_db_path           scratch;
  planar::cmd::ext::context ctx;

  fixture()
      : ctx({}, planar::cmd::ext::map_env({{"PLANAR_DB", scratch.path_.string()}}), std::filesystem::path{},
            std::make_shared<planar::cmd::ext::database>(scratch.path_, std::cerr), std::cout, std::cerr) {
    // Apply the full migration chain directly (not through `planar init`,
    // which does not exist in this test binary's link closure) so
    // `ensure_db()`'s schema-version check passes and every table decision
    // 995 references actually exists.
    auto conn = planar::db::connection::open(scratch.path_.string());
    REQUIRE(conn.has_value());
    REQUIRE(planar::db::apply_all(*conn));
    // Close this handle before ensure_db opens its own — SQLite tolerates
    // multiple connections to one file, but there is no reason to hold
    // two open.
  }
};

} // namespace

TEST_CASE("planar-ext's declared verb set is exactly {version, schema, ext..., sync...} after task 6421's ext propagate",
          "[cmd][ext][capability]") {
  auto const root = planar::cmd::ext::root_app();
  CHECK(root->get_name() == "planar-ext");

  std::set<std::string, std::less<>> names;
  for (auto const& node : planar::cliapp::all_nodes(*root)) {
    if (node.path.empty()) {
      continue;
    }
    names.insert(node.node->get_name());
  }
  // Every node NAME in the tree, group nodes and bare leaves alike — NOT
  // full paths, so "list"/"push"/"status"/"pull" here are `ext list` /
  // `sync push` / `sync status` / `sync pull`. `propagate` joined at task
  // 6421 (the github-parent-issue arm only — see `handlers/ext/propagate.cppm`).
  CHECK(names == std::set<std::string, std::less<>>{"version", "schema", "ext", "register", "jira", "github", "list", "test",
                                                    "create", "propagate-one", "propagate", "sync", "pull", "push", "status",
                                                    "resolve"});

  // The forbidden set: every write verb the OTHER agent-callable binary
  // carries, and every planning-entity verb the operator binary carries —
  // reproduced from `src/cmd/planar-watch/main.cppm::forbidden_verbs()`
  // rather than re-derived, so the two lists cannot drift apart silently.
  // `pull` is deliberately ABSENT from this list as of task 6419: the
  // claim-ritual `planar-agent pull` never landed here, but `sync pull`
  // legitimately did, and this check compares bare node NAMES — it cannot
  // tell the two apart, so keeping `pull` forbidden would make the exact-set
  // assertion above and this refusal check permanently disagree.
  static constexpr std::string_view k_forbidden[] = {
      "claim", "heartbeat", "complete", "fail", "release",   "block",    "action",   "ingest",   "reconcile",
      "abort", "peek",      "plan",     "task", "decision",  "question", "scenario", "artifact", "annotate",
      "init",  "workbench", "doc",      "spec", "templates", "promote",  "demote",   "capture",
  };
  for (auto const& forbidden : k_forbidden) {
    INFO("forbidden verb leaked into planar-ext's tree: " << forbidden);
    CHECK_FALSE(names.contains(forbidden));
  }
}

TEST_CASE("every planar-ext leaf is wired to a handler, and every handler is reachable", "[cmd][ext][dispatch]") {
  auto const root  = planar::cmd::ext::root_app();
  auto const table = planar::cmd::ext::handlers(*root);

  auto const missing = planar::cmd::ext::unregistered_leaves(*root, table);
  INFO("leaves with no handler: " << missing.size());
  CHECK(missing.empty());

  auto const dead = planar::cmd::ext::unreachable_handlers(*root, table);
  INFO("handlers no argv can reach: " << dead.size());
  CHECK(dead.empty());
}

TEST_CASE("ensure_db() enforces the decision-995 write allowlist: INSERT/UPDATE/DELETE against a "
          "planning table is DENIED at prepare() time, through the exact connection a handler gets",
          "[cmd][ext][capability][write-boundary]") {
  fixture fx;

  auto const opened = fx.ctx.db().ensure_db();
  REQUIRE(opened.has_value());
  auto& conn = **opened;

  // `plans`, `tasks`, `questions`, `artifacts` — the exact four tables
  // `sync.cpp`'s `table_for` maps a `sync.cpp:352`-shaped UPDATE onto, cited
  // verbatim in the task brief. Every one must be denied.
  for (auto const& table : {"plans", "tasks", "questions", "artifacts"}) {
    INFO("table: " << table);
    auto ins = conn.prepare(std::format("insert into {} default values;", table));
    CHECK_FALSE(ins.has_value());
    auto upd = conn.prepare(std::format("update {} set updated_at = updated_at;", table));
    CHECK_FALSE(upd.has_value());
    auto del = conn.prepare(std::format("delete from {} where 1 = 0;", table));
    CHECK_FALSE(del.has_value());
    // SELECT is untouched by the write allowlist — this binary reads
    // planning state, it just never writes it.
    auto sel = conn.prepare(std::format("select * from {};", table));
    CHECK(sel.has_value());
  }

  // Also deny the agent-plane tables `planar-agent` owns — planar-ext has
  // no more business writing those than it does the planning tables.
  for (auto const& table : {"agent_actions", "agent_work_claims"}) {
    INFO("table: " << table);
    auto upd = conn.prepare(std::format("update {} set id = id;", table));
    CHECK_FALSE(upd.has_value());
  }
}

TEST_CASE("ensure_db()'s write allowlist refuses every statement that writes a host-queue table, at prepare() time",
          "[cmd][ext][capability][write-boundary][qp-separability]") {
  // Plan 1089, task qp-separability (tech spec 656 § Separability): the host
  // queue lives in planar.db, and planar-ext's authorizer allowlist is what
  // keeps the operational-plane binary away from it. The allowlist names
  // exactly three tables and `queue_*` is not among them, so each write is
  // refused by the SQLite layer through the very connection a handler gets,
  // while reads stay open like every other table's.
  // A refusal counts only when SQLite says the authorizer refused it: a typo
  // in a column name also fails to prepare, and must not pass for a refusal.
  auto const refused_by_authorizer = [](planar::db::connection& c, std::string_view sql) {
    auto const prepared = c.prepare(sql);
    return !prepared.has_value() && prepared.error().message_.contains("not authorized");
  };

  fixture fx;

  auto const opened = fx.ctx.db().ensure_db();
  REQUIRE(opened.has_value());
  auto& conn = **opened;

  // Not vacuous: the tables exist (a statement against a missing table would
  // also fail to prepare, for the wrong reason) and a read prepares.
  for (auto const& table : {"queue_entries", "queue_history", "queue_schema"}) {
    INFO("table: " << table);
    CHECK(conn.prepare(std::format("select count(*) from {};", table)).has_value());
  }

  CHECK(refused_by_authorizer(conn, "insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, enqueued_at, "
                                    "refreshed_mono) values ('waiting', 'h', 1, 1, '/', '[]', 0, 0);"));
  CHECK(refused_by_authorizer(conn, "update queue_entries set state = 'running';"));
  CHECK(refused_by_authorizer(conn, "delete from queue_entries where seq = 1;"));
  CHECK(refused_by_authorizer(conn, "insert into queue_history (seq, outcome, cwd, argv, enqueued_at, ended_at, waited_ms) "
                                    "values (1, 'exited', '/', '[]', 0, 0, 0);"));
  CHECK(refused_by_authorizer(conn, "delete from queue_history;"));
  CHECK(refused_by_authorizer(conn, "update queue_schema set compat = 1;"));
  CHECK(refused_by_authorizer(conn, "insert into queue_schema (version, compat, description) values (9, 9, 'x');"));

  // A write through a runtime-composed table name is refused the same way.
  for (std::string const table : {std::string{"queue_entries"}, std::string{"queue_history"}, std::string{"queue_schema"}}) {
    INFO("interpolated table: " << table);
    CHECK(refused_by_authorizer(conn, std::format("delete from {};", table)));
  }

  // The tables are untouched.
  for (auto const& table : {"queue_entries", "queue_history"}) {
    auto count = conn.prepare(std::format("select count(*) from {};", table));
    REQUIRE(count.has_value());
    REQUIRE(count->step().value() == planar::db::step_result::row);
    CHECK(count->column_int64(0) == 0);
  }
}

TEST_CASE("ensure_db()'s write allowlist survives a table name composed by RUNTIME STRING "
          "INTERPOLATION, reproducing sync.cpp:352's own shape — not just a literal in the SQL text",
          "[cmd][ext][capability][write-boundary]") {
  fixture fx;

  auto const opened = fx.ctx.db().ensure_db();
  REQUIRE(opened.has_value());
  auto& conn = **opened;

  // Exactly `sync.cpp`'s composition shape: the table name is a runtime
  // std::string, spliced into the SQL text with std::format, so no
  // literal "update plans" (or "update tasks", ...) appears anywhere in
  // THIS translation unit for a source-text scan to find — and the
  // authorizer still denies it, because it inspects the PARSED statement.
  for (std::string const table :
       {std::string{"plans"}, std::string{"tasks"}, std::string{"questions"}, std::string{"artifacts"}}) {
    auto const sql = std::format("update {} set updated_at = updated_at;", table);
    auto       upd = conn.prepare(sql);
    INFO("interpolated table: " << table);
    CHECK_FALSE(upd.has_value());
  }
}

TEST_CASE("ensure_db()'s write allowlist permits INSERT/UPDATE/DELETE against exactly the three "
          "decision-995 tables",
          "[cmd][ext][capability][write-boundary]") {
  fixture fx;

  auto const opened = fx.ctx.db().ensure_db();
  REQUIRE(opened.has_value());
  auto& conn = **opened;

  REQUIRE(conn.execute("insert into external_systems (kind, slug, auth_method, auth_ref) "
                       "values ('github-issues', 'gh-test', 'token-env', 'GH_TOKEN');"));
  REQUIRE(conn.execute("update external_systems set base_url = 'https://example.invalid' where slug = 'gh-test';"));

  REQUIRE(conn.execute("insert into external_links (entity_kind, entity_id, system_id, external_id) "
                       "select 'plan', 1, id, '42' from external_systems where slug = 'gh-test';"));
  REQUIRE(conn.execute("update external_links set last_sync_status = 'ok' where external_id = '42';"));

  REQUIRE(conn.execute("insert into sync_events (direction, outcome) values ('push', 'ok');"));
  REQUIRE(conn.execute("update sync_events set outcome = 'noop' where direction = 'push';"));

  REQUIRE(conn.execute("delete from external_links where external_id = '42';"));
  REQUIRE(conn.execute("delete from external_systems where slug = 'gh-test';"));
  REQUIRE(conn.execute("delete from sync_events where direction = 'push';"));
}

TEST_CASE("planar-ext version opens no database — same invariant the other three binaries' spine leaf holds",
          "[cmd][ext][dispatch]") {
  auto const root  = planar::cmd::ext::root_app();
  auto const table = planar::cmd::ext::handlers(*root);

  fixture    fx;
  auto const outcome = table.at("version")(fx.ctx, {});
  CHECK(outcome.has_value());
  CHECK_FALSE(fx.ctx.db().opened());
}
