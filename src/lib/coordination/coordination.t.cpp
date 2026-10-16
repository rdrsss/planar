// @file coordination.t.cpp
// @brief Unit tests for `planar.coordination`: the predicates the task packet
// and `recommend-strategy` share. Each predicate is exercised on a real,
// migrated SQLite database, with the link in both directions and the status
// that clears it.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.coordination;

namespace {

namespace co = planar::coordination;

/// @brief A scratch database path, removed with its sidecars on destruction.
struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_coordination_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
    }
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  INFO("sql: " << sql);
  REQUIRE(conn.execute(sql).has_value());
}

/// @brief Two tasks (1 and 2) on one plan.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug, status) "
             "values (1, 'global', null, 'Plan', 'plan', 'active')");
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) values "
             "(1, 'global', null, 1, 'One', 'todo', 100, 'one'),"
             "(2, 'global', null, 1, 'Two', 'todo', 100, 'two')");
}

using ids = std::vector<std::int64_t>;

} // namespace

TEST_CASE("open_questions_linked sees an open question linked in either direction", "[coordination]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status) values "
             "(10,'global',null,'A','a','open'),(11,'global',null,'B','b','open')");
  exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status, answer_body, answered_at) "
             "values (12,'global',null,'C','c','answered','done','2020-01-01T00:00:00.000Z')");
  CHECK(co::open_questions_linked(conn, 1).value().empty());

  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',1,'question',11,'addresses'),('question',10,'task',1,'addresses'),('task',1,'question',12,'addresses')");
  // Both directions count, the answered question does not, and the ids come back ascending.
  CHECK(co::open_questions_linked(conn, 1).value() == ids{10, 11});
  // A different task is not affected.
  CHECK(co::open_questions_linked(conn, 2).value().empty());

  exec(conn, "update questions set status='answered', answer_body='done', answered_at='2020-01-01T00:00:00.000Z' "
             "where id in (10, 11)");
  CHECK(co::open_questions_linked(conn, 1).value().empty());
}

TEST_CASE("proposed_decisions_linked sees a proposed decision until it is accepted", "[coordination]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into decisions (id, scope_kind, scope_id, title, body, status) values "
             "(20,'global',null,'P','p','proposed'),(21,'global',null,'Q','q','accepted')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',1,'decision',20,'cites'),('decision',21,'task',1,'cites')");
  CHECK(co::proposed_decisions_linked(conn, 1).value() == ids{20});
  CHECK(co::proposed_decisions_linked(conn, 2).value().empty());

  exec(conn, "update decisions set status='accepted' where id=20");
  CHECK(co::proposed_decisions_linked(conn, 1).value().empty());
}

TEST_CASE("unfinished_dependencies lists direct depends-on targets that are not done or cancelled", "[coordination]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) values "
             "(3, 'global', null, 1, 'Three', 'doing', 100, 'three'),"
             "(4, 'global', null, 1, 'Four', 'done', 100, 'four'),"
             "(5, 'global', null, 1, 'Five', 'cancelled', 100, 'five'),"
             "(6, 'global', null, 1, 'Six', 'blocked', 100, 'six')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',1,'task',2,'depends-on'),('task',1,'task',3,'depends-on'),('task',1,'task',4,'depends-on'),"
             "('task',1,'task',5,'depends-on'),('task',1,'task',6,'depends-on'),"
             // Not a dependency of task 1: the reverse edge, another relationship, and a self-link.
             "('task',3,'task',1,'depends-on'),('task',1,'task',2,'derives-from'),('task',1,'task',1,'depends-on')");
  CHECK(co::unfinished_dependencies(conn, 1).value() == ids{2, 3, 6});
  // The reverse edge makes task 3 depend on task 1, which is unfinished.
  CHECK(co::unfinished_dependencies(conn, 3).value() == ids{1});

  exec(conn, "update tasks set status='done' where id in (2, 3, 6)");
  CHECK(co::unfinished_dependencies(conn, 1).value().empty());
}
