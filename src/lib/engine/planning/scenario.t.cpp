// @file scenario.t.cpp
// @brief Unit tests for `planar.engine.planning.scenario` (plan 996
// roadmap M12 item 9, task 6195).
//
// ## What these tests assert, and why they are shaped this way
//
// Thirteen wiring cycles into this milestone, every one has surfaced a
// latent engine bug that was unit-tested and green until a caller existed:
// a `list` with no status predicate that resurrected terminal plans, a
// renderer that emitted invalid JSON, an `add`/`remove` pair that wrote no
// audit rows for four cycles because the READER was cut too, a `--plan`
// that wrote a dangling edge. The common shape is a test that asserts a
// COUNT, or asserts against the engine's own read path, and therefore
// cannot see the engine and the test agreeing on the same wrong thing.
//
// So, four rules here, inherited from decision.t.cpp:
//
//   1. **Row assertions go through raw SQL**, not through `show_scenario`.
//      A test that reads back what it wrote with the same decoder cannot
//      catch a decoder that collapses SQL NULL onto the empty string.
//      `sql_text_or_null` renders NULL as the sentinel `<NULL>`.
//   2. **Every filter is proven to EXCLUDE**, by asserting the
//      out-of-scope rows SURVIVE in the table afterwards and by asserting
//      the returned ID SET rather than its size.
//   3. **The status, artifact and scope filters get MULTI-WAY
//      discriminations.** The status fixture holds one row in each of the
//      five statuses and is queried seven ways; an inert filter returns the
//      SAME set every time and a filter applied where it should not be
//      collapses to empty. Only real branch behaviour produces seven
//      different answers. `--touches` gets the sharpest one available: the
//      SAME repo, queried under two different scopes, returns two DISJOINT
//      sets, neither a subset of the other.
//   4. **Every REFUSAL is followed by an after-state snapshot.** A refusal
//      that half-wrote is the worse bug and only the after-state
//      distinguishes it. `verify`'s illegal-transition refusal is the
//      sharpest case here: it must leave `updated_at` UNMOVED, which a
//      status-only assertion would not catch.
//
// One class of defect a raw-SQL row snapshot still cannot see: an engine
// READ that collapses NULL onto a default before the value ever reaches
// SQL. For that class the assertion is on the RENDERED form, where `null`
// and `""` are different bytes — see the render cases at the bottom.
//
// ## The empty status filter, tested BOTH directions
//
// `scenario_list_filter::statuses` empty means EVERY status, `retired`
// included. That is this family's own answer and it agrees with NEITHER
// sibling: `question`'s empty arm means `open`, `decision`'s means
// `{proposed, accepted}`, and `plan`'s meaning "everything" was the bug
// that flipped done plans back to active. It is asserted here by naming
// the full five-id set, not by a count, so an accidental default predicate
// fails loudly.
//
// ## Two arguments that behave OPPOSITELY on one verb
//
// `related_artifact_id` is a real column FK and a nonexistent artifact
// REFUSES the create; `plan_id` is an `entity_links` row and a nonexistent
// plan DANGLES at success. Both are the oracle's, both are reproduced, and
// both are asserted with an after-state — the refusal proves no partial row
// landed, the dangle proves the edge is really there.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.scenario;

namespace {

using planar::engine::planning::create_scenario;
using planar::engine::planning::list_scenarios;
using planar::engine::planning::list_scenarios_touching;
using planar::engine::planning::ready_scenario;
using planar::engine::planning::render_json;
using planar::engine::planning::render_list_json;
using planar::engine::planning::render_list_text;
using planar::engine::planning::render_text;
using planar::engine::planning::retire_scenario;
using planar::engine::planning::scenario;
using planar::engine::planning::scenario_create_args;
using planar::engine::planning::scenario_error;
using planar::engine::planning::scenario_list_filter;
using planar::engine::planning::scenario_outcome;
using planar::engine::planning::scenario_outcome_from_text;
using planar::engine::planning::scenario_outcome_to_text;
using planar::engine::planning::scenario_scope_kind;
using planar::engine::planning::scenario_status;
using planar::engine::planning::scenario_status_from_text;
using planar::engine::planning::scenario_status_to_text;
using planar::engine::planning::show_scenario;
using planar::engine::planning::verify_scenario;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_scenario_test_{}_{}.db",
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

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

/// @brief Run a single-column, single-row query and return the value with
/// SQL NULL rendered as the literal `<NULL>`.
///
/// The sentinel is the whole point: `''` and NULL are different rows and a
/// test that cannot tell them apart cannot catch a bind that turned an
/// absent optional into an empty string.
auto sql_text_or_null(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  if (*step != planar::db::step_result::row) {
    return "<NO ROW>";
  }
  if (stmt->is_null(0)) {
    return "<NULL>";
  }
  return stmt->column_text(0);
}

auto sql_int(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

void exec(planar::db::connection& conn, std::string_view sql) {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Render a multi-row, multi-column projection as `a|b|c` lines
/// joined by `;`, SQL NULL as `<NULL>`.
auto sql_rows(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  for (;;) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += ';';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `test_scenarios` row as
/// `id|scope_kind|scope_id|title|body|status|related_artifact_id|last_run_at|last_outcome`.
///
/// `last_run_at` is a wall-clock stamp, so it is projected as the two-state
/// `<NULL>` / `SET`; whether the column is null is the contract (every
/// `verify` writes it, `retire` and `ready` must not), its value is not.
auto scenario_rows(planar::db::connection& conn) -> std::string {
  return sql_rows(conn,
                  "select id, scope_kind, scope_id, title, body, status, related_artifact_id, "
                  "case when last_run_at is null then null else 'SET' end, last_outcome "
                  "from test_scenarios order by id",
                  9);
}

/// @brief Every `audit_log` row as `verb|entity_kind|entity_id|summary|actor|scope`.
///
/// `actor` and `scope` are included precisely because they are ALWAYS NULL
/// on this path: a port that helpfully filled them in would be writing rows
/// the oracle does not, invisibly to every other assertion.
auto audit_rows(planar::db::connection& conn) -> std::string {
  return sql_rows(conn, "select verb, entity_kind, entity_id, summary, actor, scope from audit_log order by id", 6);
}

/// @brief Every `entity_links` row as
/// `from_kind|from_id|to_kind|to_id|relationship`.
auto edge_rows(planar::db::connection& conn) -> std::string {
  return sql_rows(conn, "select from_kind, from_id, to_kind, to_id, relationship from entity_links order by id", 5);
}

/// @brief The ids of `rows`, comma-joined — the shape every filter case
/// asserts against, so a wrong SET fails rather than a wrong COUNT.
auto ids_of(std::span<const scenario> rows) -> std::string {
  std::string out;
  for (const auto& s : rows) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::format("{}", s.id);
  }
  return out;
}

} // namespace

// ===========================================================================
// enum round-trips
// ===========================================================================

TEST_CASE("scenario status text round-trips and rejects unknowns") {
  for (auto const s : {scenario_status::draft, scenario_status::ready, scenario_status::verified, scenario_status::failing,
                       scenario_status::retired}) {
    INFO(scenario_status_to_text(s));
    CHECK(scenario_status_from_text(scenario_status_to_text(s)) == s);
  }
  CHECK_FALSE(scenario_status_from_text("bogus").has_value());
  CHECK_FALSE(scenario_status_from_text("").has_value());
  // NOT comma-split at this layer — the handler splits. A CSV reaching here
  // is one unknown token.
  CHECK_FALSE(scenario_status_from_text("draft,verified").has_value());
}

TEST_CASE("scenario outcome text round-trips; error_case spells itself error") {
  for (auto const o : {scenario_outcome::pass, scenario_outcome::fail, scenario_outcome::error_case, scenario_outcome::skipped}) {
    INFO(scenario_outcome_to_text(o));
    CHECK(scenario_outcome_from_text(scenario_outcome_to_text(o)) == o);
  }
  // The enumerator is `error_case` but the WIRE form is the bare `error` —
  // the column's CHECK constraint accepts only the latter.
  CHECK(scenario_outcome_to_text(scenario_outcome::error_case) == "error");
  CHECK(scenario_outcome_from_text("error") == scenario_outcome::error_case);
  CHECK_FALSE(scenario_outcome_from_text("error_case").has_value());
  CHECK_FALSE(scenario_outcome_from_text("passed").has_value());
}

// ===========================================================================
// create
// ===========================================================================

TEST_CASE("create writes a draft row with NULL body, and one create audit row") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = create_scenario(conn, scenario_create_args{.title = "Happy path: first"});
  REQUIRE(created.has_value());
  CHECK(created->id == 1);
  CHECK(created->status == scenario_status::draft);
  CHECK(created->scope_kind == scenario_scope_kind::global);

  // The row, through RAW SQL. `body` absent must be SQL NULL, never `''`.
  CHECK(scenario_rows(conn) == "1|global|<NULL>|Happy path: first|<NULL>|draft|<NULL>|<NULL>|<NULL>");
  // ORACLE: the entity kind is `scenario`, and the title is single-quoted.
  CHECK(audit_rows(conn) == "create|scenario|1|create scenario 'Happy path: first'|<NULL>|<NULL>");
  // No `--plan`, so no edge. And no session: this verb starts none.
  CHECK(edge_rows(conn).empty());
  CHECK(sql_int(conn, "select count(*) from sessions") == 0);
}

TEST_CASE("create with an EMPTY body writes '' and not NULL") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // The inverse of the case above, and the reason `body` is an optional
  // rather than a plain string: `--body ""` is a legal, DISTINCT value.
  auto created = create_scenario(conn, scenario_create_args{.title = "T1", .body = std::string{""}});
  REQUIRE(created.has_value());
  CHECK(sql_text_or_null(conn, "select body from test_scenarios where id = 1") == "");
  CHECK(sql_text_or_null(conn, "select body from test_scenarios where id = 1") != "<NULL>");
  // And the engine's own read agrees it is present-but-empty, not absent.
  REQUIRE(created->body.has_value());
  CHECK(created->body->empty());
}

TEST_CASE("create resolves each scope grammar onto the right (kind, id) pair") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  exec(conn, "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')");
  auto const assoc_id = sql_int(conn, "select id from associations where slug = 'acme'");
  auto const repo_id  = sql_int(conn, "select id from projects where slug = 'foo'");

  REQUIRE(create_scenario(conn, scenario_create_args{.title = "no scope"}).has_value());
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "literal global", .scope = "global"}).has_value());
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "assoc", .scope = "acme"}).has_value());
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "repo", .scope = "repo:foo"}).has_value());

  CHECK(sql_rows(conn, "select id, scope_kind, scope_id from test_scenarios order by id", 3) ==
        std::format("1|global|<NULL>;2|global|<NULL>;3|association|{};4|repo|{}", assoc_id, repo_id));
}

TEST_CASE("create with an unresolvable scope refuses BEFORE any write") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = create_scenario(conn, scenario_create_args{.title = "x", .scope = "no-such-slug"});
  REQUIRE_FALSE(created.has_value());
  CHECK(created.error() == scenario_error::slug_not_found);

  // The after-state: a refusal that half-wrote is the worse bug.
  CHECK(scenario_rows(conn).empty());
  CHECK(audit_rows(conn).empty());
}

TEST_CASE("create --related a NONEXISTENT artifact refuses and writes NOTHING") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // `related_artifact_id references artifacts(id)` is a real FK, so the
  // INSERT itself fails. Not a code check — the schema's. Asserted so a
  // future port that drops the FK or catches the failure differently is
  // caught.
  auto created = create_scenario(conn, scenario_create_args{.title = "dangling artifact", .related_artifact_id = 77});
  REQUIRE_FALSE(created.has_value());
  CHECK(created.error() == scenario_error::query_failed);

  CHECK(scenario_rows(conn).empty());
  // Not even the audit row: the failure is the create, and the audit write
  // is downstream of it.
  CHECK(audit_rows(conn).empty());
}

TEST_CASE("create --related an EXISTING artifact stores the id") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values ('global', null, 'test_spec', 'spec A')");

  auto created = create_scenario(conn, scenario_create_args{.title = "linked", .related_artifact_id = 1});
  REQUIRE(created.has_value());
  CHECK(created->related_artifact_id == 1);
  CHECK(sql_text_or_null(conn, "select related_artifact_id from test_scenarios where id = 1") == "1");
}

TEST_CASE("create --plan a NONEXISTENT plan refuses and writes NOTHING") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Task 6197, checked at the ENDPOINT rather than inherited from a
  // sibling. This verb used to exit 0 here and leave an `entity_links` edge
  // to a plan that never existed, where `question add --plan` refused; the
  // old pin said "if a future migration adds an FK here, this case fails
  // and someone decides deliberately". The decision went the other way: no
  // migration, an explicit engine-side existence check, matching
  // `create_question` and `create_artifact`.
  //
  // Compare `--related` two cases up, which refuses for a STRUCTURAL
  // reason: `related_artifact_id references artifacts(id)` is a real FK and
  // SQLite does the refusing. `entity_links` has no FK to its target table,
  // so this endpoint had to be checked in code or not at all — which is why
  // one verb contradicted itself on its two reference flags.
  auto created = create_scenario(conn, scenario_create_args{.title = "P-only", .plan_id = 4242});
  REQUIRE_FALSE(created.has_value());
  CHECK(created.error() == scenario_error::not_found);

  // The after-state carries the contract: the original defect exited 0, so
  // an error-only assertion could not have seen it.
  CHECK(edge_rows(conn).empty());
  CHECK(scenario_rows(conn).empty());
  CHECK(audit_rows(conn).empty());
}

TEST_CASE("create --plan an EXISTING plan still writes the test_scenario edge") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn,
       "insert into plans (scope_kind, scope_id, title, slug, status) values ('global', null, 'anchor', 'anchor', 'draft')");

  // The positive half: the refusal above must not have been bought by
  // breaking the working path. The edge's `from_kind` is `test_scenario` —
  // the spelling editflow's anchor resolver queries; `scenario` would not
  // resolve and would break `scenario view`.
  auto created = create_scenario(conn, scenario_create_args{.title = "P-only", .plan_id = 1});
  REQUIRE(created.has_value());
  CHECK(edge_rows(conn) == "test_scenario|1|plan|1|derives-from");
  // Exactly ONE audit row, and its verb is `create` — the edge gets no
  // `link` row of its own.
  CHECK(audit_rows(conn) == "create|scenario|1|create scenario 'P-only'|<NULL>|<NULL>");
}

// ===========================================================================
// show
// ===========================================================================

TEST_CASE("show returns not_found for an absent id") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto            found = show_scenario(conn, 9999);
  REQUIRE_FALSE(found.has_value());
  CHECK(found.error() == scenario_error::not_found);
}

// ===========================================================================
// verify
// ===========================================================================

TEST_CASE("verify PASS from draft auto-transitions through ready, leaving TWO audit rows") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "auto"}).has_value());

  auto verified = verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt);
  REQUIRE(verified.has_value());
  CHECK(verified->status == scenario_status::verified);
  CHECK(verified->last_outcome == scenario_outcome::pass);
  REQUIRE(verified->last_run_at.has_value());

  CHECK(scenario_rows(conn) == "1|global|<NULL>|auto|<NULL>|verified|<NULL>|SET|pass");
  // The intermediate hop's OWN row is the only observable proof the walk
  // happened rather than a direct draft->verified UPDATE. Asserting only
  // the final status would pass either way.
  CHECK(audit_rows(conn) == "create|scenario|1|create scenario 'auto'|<NULL>|<NULL>;"
                            "status_change|scenario|1|ready: auto-transition via verify|<NULL>|<NULL>;"
                            "status_change|scenario|1|verify: pass|<NULL>|<NULL>");
}

TEST_CASE("verify PASS from ready is a SINGLE hop with no auto-transition row") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "via ready"}).has_value());
  REQUIRE(ready_scenario(conn, 1, std::nullopt).has_value());

  REQUIRE(verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt).has_value());

  // The `ready` row here says `ready`, NOT `ready: auto-transition via
  // verify` — the two paths are distinguishable in the log.
  CHECK(audit_rows(conn) == "create|scenario|1|create scenario 'via ready'|<NULL>|<NULL>;"
                            "status_change|scenario|1|ready|<NULL>|<NULL>;"
                            "status_change|scenario|1|verify: pass|<NULL>|<NULL>");
}

TEST_CASE("verify with a NON-passing outcome records the run and leaves status ALONE") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "failing run"}).has_value());

  auto ran = verify_scenario(conn, 1, scenario_outcome::fail, std::string_view{"broke"});
  REQUIRE(ran.has_value());
  CHECK(ran->status == scenario_status::draft);
  CHECK(ran->last_outcome == scenario_outcome::fail);

  CHECK(scenario_rows(conn) == "1|global|<NULL>|failing run|<NULL>|draft|<NULL>|SET|fail");
  // ORACLE: the outcome moves INSIDE the parentheses when a summary is
  // given. `verify: fail: broke` would be the natural guess and is wrong.
  CHECK(sql_text_or_null(conn, "select summary from audit_log order by id desc limit 1") == "verify(fail): broke");
}

TEST_CASE("verify PASS on a RETIRED scenario refuses and leaves updated_at UNMOVED") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "retired"}).has_value());
  REQUIRE(retire_scenario(conn, 1, std::nullopt).has_value());
  auto const before      = sql_text_or_null(conn, "select updated_at from test_scenarios where id = 1");
  auto const audit_count = sql_int(conn, "select count(*) from audit_log");

  auto refused = verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt);
  REQUIRE_FALSE(refused.has_value());
  // Propagated UNFOLDED — the operator sees `IllegalTransition`, not a
  // humanised "is terminal" the way `decision` produces.
  CHECK(refused.error() == scenario_error::illegal_transition);

  // The whole after-state. `updated_at` is the assertion a status-only
  // check would miss: a port that ran the UPDATE and then refused would
  // still report `retired` here.
  CHECK(scenario_rows(conn) == "1|global|<NULL>|retired|<NULL>|retired|<NULL>|<NULL>|<NULL>");
  CHECK(sql_text_or_null(conn, "select updated_at from test_scenarios where id = 1") == before);
  CHECK(sql_int(conn, "select count(*) from audit_log") == audit_count);
}

TEST_CASE("verify SKIPPED on a RETIRED scenario SUCCEEDS — the matrix is never consulted") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "retired"}).has_value());
  REQUIRE(retire_scenario(conn, 1, std::nullopt).has_value());

  // The complement of the case above, on the SAME row: whether a `verify`
  // is refused depends entirely on the OUTCOME, not on the status. A port
  // that gated the whole verb on the matrix passes the refusal case and
  // fails this one.
  auto ran = verify_scenario(conn, 1, scenario_outcome::skipped, std::nullopt);
  REQUIRE(ran.has_value());
  CHECK(ran->status == scenario_status::retired);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|retired|<NULL>|retired|<NULL>|SET|skipped");
}

TEST_CASE("re-verifying an ALREADY-verified scenario succeeds by the identity short-circuit") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "twice"}).has_value());
  REQUIRE(verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt).has_value());

  // `verified -> verified` never reaches the matrix (which has no such
  // edge), so this is a success and writes a SECOND `verify: pass` row.
  REQUIRE(verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt).has_value());
  CHECK(sql_int(conn, "select count(*) from audit_log where summary = 'verify: pass'") == 2);
  // ...and only ONE auto-transition row, from the first call.
  CHECK(sql_int(conn, "select count(*) from audit_log where summary like 'ready:%'") == 1);
}

TEST_CASE("verify from `failing` reaches verified — the lateral edge exists both ways") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  // `failing` is unreachable through the scenario VERBS (none targets it),
  // so it is seeded directly. It is still a legal SOURCE and a row put
  // there by another writer must move correctly — the matrix carries
  // `failing -> verified` AND `verified -> failing`, unlike the annotation
  // arm where outcome states never move laterally.
  exec(conn,
       "insert into test_scenarios (scope_kind, scope_id, title, status) values ('global', null, 'was failing', 'failing')");

  auto verified = verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt);
  REQUIRE(verified.has_value());
  CHECK(verified->status == scenario_status::verified);
  // No auto-transition row: the source was not `draft`.
  CHECK(sql_int(conn, "select count(*) from audit_log where summary like 'ready:%'") == 0);
}

TEST_CASE("verify on an absent id returns not_found and writes nothing") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto            refused = verify_scenario(conn, 999, scenario_outcome::pass, std::nullopt);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == scenario_error::not_found);
  CHECK(audit_rows(conn).empty());
}

// ===========================================================================
// ready / retire
// ===========================================================================

TEST_CASE("ready advances draft and refuses every other source") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "advance"}).has_value());

  auto advanced = ready_scenario(conn, 1, std::string_view{"spec locked"});
  REQUIRE(advanced.has_value());
  CHECK(advanced->status == scenario_status::ready);
  CHECK(sql_text_or_null(conn, "select summary from audit_log order by id desc limit 1") == "ready: spec locked");

  // `ready -> ready` is an identity move and SUCCEEDS; `verified -> ready`
  // is a real edge the matrix refuses. Both on the same row, so the
  // difference is the matrix and not the fixture.
  REQUIRE(ready_scenario(conn, 1, std::nullopt).has_value());
  REQUIRE(verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt).has_value());
  auto refused = ready_scenario(conn, 1, std::nullopt);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == scenario_error::illegal_transition);
  CHECK(sql_text_or_null(conn, "select status from test_scenarios where id = 1") == "verified");
}

TEST_CASE("retire is legal from everywhere, leaves the run columns alone, and repeats") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create_scenario(conn, scenario_create_args{.title = "old test"}).has_value());
  REQUIRE(verify_scenario(conn, 1, scenario_outcome::pass, std::nullopt).has_value());

  auto retired = retire_scenario(conn, 1, std::string_view{"feature removed"});
  REQUIRE(retired.has_value());
  CHECK(retired->status == scenario_status::retired);
  // `last_outcome` SURVIVES retirement — retiring says nothing about the
  // last run. A port that cleared it would still report `retired` here.
  CHECK(retired->last_outcome == scenario_outcome::pass);
  CHECK(scenario_rows(conn) == "1|global|<NULL>|old test|<NULL>|retired|<NULL>|SET|pass");
  CHECK(sql_text_or_null(conn, "select summary from audit_log order by id desc limit 1") == "retire: feature removed");

  // `retired -> retired` short-circuits, so a second retire SUCCEEDS and
  // writes a second row — this one with the BARE summary.
  REQUIRE(retire_scenario(conn, 1, std::nullopt).has_value());
  CHECK(sql_text_or_null(conn, "select summary from audit_log order by id desc limit 1") == "retire");
}

// ===========================================================================
// list — the multi-way discriminations
// ===========================================================================

namespace {

/// @brief Five scenarios, one in each status, all `global`.
///
/// Seeded by SQL rather than by walking the transitions, so the fixture
/// says what it means and `failing` (unreachable through the verbs) is
/// available too.
void seed_one_per_status(planar::db::connection& conn) {
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title, status) values "
             "('global', null, 'd', 'draft'), "
             "('global', null, 'r', 'ready'), "
             "('global', null, 'v', 'verified'), "
             "('global', null, 'f', 'failing'), "
             "('global', null, 'x', 'retired')");
}

} // namespace

TEST_CASE("list --status: seven queries, seven different answers, and no row is destroyed") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_one_per_status(conn);

  // 1. EMPTY means EVERY status, `retired` included. This is the assertion
  //    that would have caught the `list_plans` defect: an inert filter and
  //    a correct one BOTH return everything here, so the discriminating
  //    cases are the six below, not this one.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{})) == "1,2,3,4,5");
  // 2.-6. One status each. Every answer is a different single id, so an
  //    inert filter (which would return "1,2,3,4,5" five times) fails five
  //    times over.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::draft}})) == "1");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::ready}})) == "2");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::verified}})) == "3");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::failing}})) == "4");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::retired}})) == "5");
  // 7. A multi-status set is a real IN, not a first-wins.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::draft, scenario_status::verified}})) ==
        "1,3");

  // The EXCLUDED rows SURVIVE. A filter that deleted its non-matches would
  // pass every assertion above.
  CHECK(sql_int(conn, "select count(*) from test_scenarios") == 5);
  CHECK(sql_rows(conn, "select id, status from test_scenarios order by id", 2) ==
        "1|draft;2|ready;3|verified;4|failing;5|retired");
}

TEST_CASE("list --scope: five queries over three scope kinds, and the excluded rows survive") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  exec(conn, "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')");
  auto const assoc_id = sql_int(conn, "select id from associations where slug = 'acme'");
  auto const repo_id  = sql_int(conn, "select id from projects where slug = 'foo'");
  exec(conn, std::format("insert into test_scenarios (scope_kind, scope_id, title) values "
                         "('global', null, 'g'), ('association', {}, 'a'), ('repo', {}, 'r')",
                         assoc_id, repo_id));

  // No scope predicate at all.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{})) == "1,2,3");
  // One kind each — three DIFFERENT single-id answers.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.scopes = {"global"}})) == "1");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.scopes = {"acme"}})) == "2");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.scopes = {"repo:foo"}})) == "3");
  // A two-member set ORs, and the omitted kind stays omitted.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.scopes = {"global", "repo:foo"}})) == "1,3");
  // The singular `scope` field ORs with the vector rather than replacing
  // it — the CLI never sets both, but the engine's contract does.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.scope = "acme", .scopes = {"global"}})) == "1,2");

  CHECK(sql_rows(conn, "select id, scope_kind from test_scenarios order by id", 2) == "1|global;2|association;3|repo");
}

TEST_CASE("list with an unresolvable scope member fails the WHOLE call, never a short list") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title) values ('global', null, 'g')");

  // Dropping the bad member and returning the good one's rows would look
  // like success and be a silently short list.
  auto refused = list_scenarios(conn, scenario_list_filter{.scopes = {"global", "no-such-slug"}});
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == scenario_error::slug_not_found);
}

TEST_CASE("list --related: three queries, three different answers, non-matches survive") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values "
             "('global', null, 'test_spec', 'A'), ('global', null, 'tech_spec', 'B')");
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title, related_artifact_id) values "
             "('global', null, 'for A', 1), ('global', null, 'for B', 2), ('global', null, 'for none', null)");

  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{})) == "1,2,3");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.related_artifact_id = 1})) == "1");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.related_artifact_id = 2})) == "2");
  // A NULL `related_artifact_id` is matched by NO id — `= ?` is never true
  // against NULL, so scenario 3 is unreachable through this filter.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.related_artifact_id = 99})).empty());

  CHECK(sql_int(conn, "select count(*) from test_scenarios") == 3);
}

TEST_CASE("list --status and --related COMPOSE rather than one winning") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into artifacts (scope_kind, scope_id, kind, title) values ('global', null, 'test_spec', 'A')");
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title, status, related_artifact_id) values "
             "('global', null, 'draft+A', 'draft', 1), "
             "('global', null, 'retired+A', 'retired', 1), "
             "('global', null, 'draft+none', 'draft', null)");

  // Each single filter returns a two-row set; the conjunction returns the
  // ONE row in their intersection. A last-clause-wins bug returns one of
  // the two-row sets and is caught.
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::draft}})) == "1,3");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.related_artifact_id = 1})) == "1,2");
  CHECK(ids_of(*list_scenarios(conn, scenario_list_filter{.statuses = {scenario_status::draft}, .related_artifact_id = 1})) ==
        "1");
}

// ===========================================================================
// list_scenarios_touching — the disjoint-sets discrimination
// ===========================================================================

TEST_CASE("touching: the SAME repo under two scopes returns two DISJOINT sets") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1'), ('r2', 'R2', '/r2')");
  auto const r1 = sql_int(conn, "select id from projects where slug = 'r1'");
  auto const r2 = sql_int(conn, "select id from projects where slug = 'r2'");
  exec(conn, std::format("insert into test_scenarios (scope_kind, scope_id, title) values "
                         "('repo', {}, 'in r1'), "        // id 1 — direct arm only
                         "('global', null, 'touches'), "  // id 2 — touches arm only
                         "('repo', {}, 'in r2'), "        // id 3 — neither
                         "('global', null, 'unrelated')", // id 4 — neither
                         r1, r2));
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('test_scenario', 2, 'repo', {}, 'touches')",
                         r1));

  // No scope set: BOTH arms run and the union is {1, 2}.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{})) == "1,2");
  // Scope = the repo itself: the direct arm survives the all-or-nothing
  // gate, and the touches arm's global row is filtered out. Answer: {1}.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{.scopes = {"repo:r1"}})) == "1");
  // Scope = global: the direct arm is switched OFF entirely (`1 = 0`) and
  // the touches arm keeps its global row. Answer: {2}.
  //
  // {1} and {2} are DISJOINT — neither a subset of the other, and neither
  // equal to the unfiltered {1,2}. An inert scope filter would return
  // "1,2" three times; a uniformly-applied one would collapse the last two
  // to empty. Only the real two-arm behaviour produces these three.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{.scopes = {"global"}})) == "2");
  // A DIFFERENT repo's scope: the direct arm is off (no ref names r1) and
  // the touches arm's row is not r2-scoped either. Empty, and that emptiness
  // is the filter working rather than the fixture being wrong — the three
  // answers above prove the rows are all there.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{.scopes = {"repo:r2"}})).empty());

  // Every row SURVIVES all four queries.
  CHECK(sql_int(conn, "select count(*) from test_scenarios") == 4);
}

TEST_CASE("touching: the status filter applies to BOTH arms") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')");
  auto const r1 = sql_int(conn, "select id from projects where slug = 'r1'");
  exec(conn, std::format("insert into test_scenarios (scope_kind, scope_id, title, status) values "
                         "('repo', {}, 'direct draft', 'draft'), "
                         "('repo', {}, 'direct retired', 'retired'), "
                         "('global', null, 'touch draft', 'draft'), "
                         "('global', null, 'touch retired', 'retired')",
                         r1, r1));
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
                         "('test_scenario', 3, 'repo', {}, 'touches'), ('test_scenario', 4, 'repo', {}, 'touches')",
                         r1, r1));

  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{})) == "1,2,3,4");
  // One id from each arm survives, and one from each arm is excluded — a
  // status clause applied to only one arm returns three ids, not two.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{.statuses = {scenario_status::draft}})) == "1,3");
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{.statuses = {scenario_status::retired}})) == "2,4");
  CHECK(sql_int(conn, "select count(*) from test_scenarios") == 4);
}

TEST_CASE("touching: a `derives-from` edge to the repo does NOT count as touching") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')");
  auto const r1 = sql_int(conn, "select id from projects where slug = 'r1'");
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title) values ('global', null, 'wrong relationship')");
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('test_scenario', 1, 'repo', {}, 'derives-from')",
                         r1));

  // The relationship predicate is real: the row EXISTS and its edge points
  // at the right repo, so an unfiltered relationship would return it.
  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{})).empty());
  CHECK(edge_rows(conn) == std::format("test_scenario|1|repo|{}|derives-from", r1));
}

TEST_CASE("touching: an edge from a DIFFERENT kind with the same id does not count") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into projects (slug, name, root_path) values ('r1', 'R1', '/r1')");
  auto const r1 = sql_int(conn, "select id from projects where slug = 'r1'");
  exec(conn, "insert into test_scenarios (scope_kind, scope_id, title) values ('global', null, 'scenario 1')");
  // A `question:1 touches repo:r1` edge. `from_id` is 1, exactly the
  // scenario's id — a query that forgot `from_kind` would return it.
  exec(conn, std::format("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                         "values ('question', 1, 'repo', {}, 'touches')",
                         r1));

  CHECK(ids_of(*list_scenarios_touching(conn, r1, scenario_list_filter{})).empty());
}

// ===========================================================================
// renderers
// ===========================================================================

namespace {

/// @brief A fully-populated scenario, for the render cases.
auto sample() -> scenario {
  return scenario{
      .id                  = 4,
      .scope_kind          = scenario_scope_kind::global,
      .scope_id            = std::nullopt,
      .title               = "Edge: with body",
      .body                = std::string{"Given X when Y then Z"},
      .status              = scenario_status::draft,
      .related_artifact_id = 1,
      .last_run_at         = std::string{"2026-08-26T20:25:44.590Z"},
      .last_outcome        = scenario_outcome::error_case,
      .created_at          = std::string{"2026-08-26T20:25:33.728Z"},
      .updated_at          = std::string{"2026-08-26T20:25:44.590Z"},
  };
}

} // namespace

TEST_CASE("render_text reproduces the oracle's block byte for byte") {
  // ORACLE, captured from `scenario show 4`. Every value starts at column
  // 13, `last run:` is the only two-word label, and `body:` comes LAST of
  // the four conditional lines — after the run columns, not before them.
  CHECK(render_text(sample()) == "id:         4\n"
                                 "title:      Edge: with body\n"
                                 "status:     draft\n"
                                 "scope:      global\n"
                                 "artifact:   1\n"
                                 "outcome:    error\n"
                                 "last run:   2026-08-26T20:25:44.590Z\n"
                                 "body:       Given X when Y then Z\n"
                                 "created:    2026-08-26T20:25:33.728Z\n"
                                 "updated:    2026-08-26T20:25:44.590Z\n");
}

TEST_CASE("render_text OMITS all four conditional lines when their columns are null") {
  // ORACLE, captured from `scenario show 1` on a freshly-created row. The
  // labels must be ABSENT, not present-with-an-empty-value.
  auto s                = sample();
  s.body                = std::nullopt;
  s.related_artifact_id = std::nullopt;
  s.last_run_at         = std::nullopt;
  s.last_outcome        = std::nullopt;
  CHECK(render_text(s) == "id:         4\n"
                          "title:      Edge: with body\n"
                          "status:     draft\n"
                          "scope:      global\n"
                          "created:    2026-08-26T20:25:33.728Z\n"
                          "updated:    2026-08-26T20:25:44.590Z\n");
}

TEST_CASE("render_text keeps an EMPTY body's line, which a null body loses") {
  // This is the case a raw-SQL row snapshot cannot see: `""` and NULL are
  // both "falsy" to a careless renderer, and the difference is a whole
  // LINE of output. ORACLE-captured from `scenario show 1` after
  // `--body ""`: the label is present with nothing after it.
  auto s                = sample();
  s.body                = std::string{""};
  s.related_artifact_id = std::nullopt;
  s.last_run_at         = std::nullopt;
  s.last_outcome        = std::nullopt;
  CHECK(render_text(s).contains("body:       \n"));
}

TEST_CASE("render_text appends the scope id for a non-global scope") {
  auto s       = sample();
  s.scope_kind = scenario_scope_kind::repo;
  s.scope_id   = 7;
  CHECK(render_text(s).contains("scope:      repo:7\n"));
}

TEST_CASE("render_json field order and null-vs-empty-string are the oracle's") {
  CHECK(render_json(sample()) ==
        R"({"id":4,"scope_kind":"global","scope_id":null,"title":"Edge: with body","body":"Given X when Y then Z",)"
        R"("status":"draft","related_artifact_id":1,"last_run_at":"2026-08-26T20:25:44.590Z","last_outcome":"error",)"
        R"("created_at":"2026-08-26T20:25:33.728Z","updated_at":"2026-08-26T20:25:44.590Z"})");
}

TEST_CASE("render_json emits null for every unset optional, and \"\" for an empty body") {
  auto s                = sample();
  s.body                = std::nullopt;
  s.related_artifact_id = std::nullopt;
  s.last_run_at         = std::nullopt;
  s.last_outcome        = std::nullopt;
  CHECK(render_json(s).contains(R"("body":null)"));
  CHECK(render_json(s).contains(R"("related_artifact_id":null)"));
  CHECK(render_json(s).contains(R"("last_run_at":null)"));
  CHECK(render_json(s).contains(R"("last_outcome":null)"));
  // The DISCRIMINATION: the same field, one value apart, renders different
  // bytes. A renderer that collapsed the optional would pass one of these
  // two and fail the other.
  s.body = std::string{""};
  CHECK(render_json(s).contains(R"("body":"")"));
  CHECK_FALSE(render_json(s).contains(R"("body":null)"));
}

TEST_CASE("render_json escapes a title that would otherwise break the document") {
  auto s  = sample();
  s.title = R"(a "quoted" \ backslash)";
  CHECK(render_json(s).contains(R"("title":"a \"quoted\" \\ backslash")"));
}

TEST_CASE("render_list_text: the empty case has PARENTHESES") {
  // `question` renders `(no questions)`, `decision` renders a bare `no
  // decisions`, `plan` renders `(no plans)`. This one was captured.
  CHECK(render_list_text({}) == "(no scenarios)\n");
  CHECK(render_list_json({}) == "[]");
}

TEST_CASE("render_list_text columns and the null-outcome dash are the oracle's") {
  std::vector<scenario> rows;
  auto                  a = sample();
  a.id                    = 1;
  a.title                 = "T1";
  a.status                = scenario_status::verified;
  a.last_outcome          = scenario_outcome::pass;
  rows.push_back(a);
  auto b         = sample();
  b.id           = 2;
  b.title        = "Edge: with body";
  b.status       = scenario_status::draft;
  b.last_outcome = std::nullopt; // renders as `-`, still padded to eight
  rows.push_back(b);

  // ORACLE, captured from `scenario list --scope global`. Four columns, not
  // three — a copy of `decision`'s `{:>5}  {:<10}  {}` silently drops the
  // outcome and left-shifts every title.
  CHECK(render_list_text(rows) == "    1  verified    pass      T1\n"
                                  "    2  draft       -         Edge: with body\n");
}

TEST_CASE("render_list_json is the comma-joined element objects") {
  std::vector<scenario> rows{sample(), sample()};
  rows[1].id        = 5;
  auto const joined = render_list_json(rows);
  CHECK(joined.starts_with("[{"));
  CHECK(joined.ends_with("}]"));
  CHECK(joined == std::format("[{},{}]", render_json(rows[0]), render_json(rows[1])));
}
