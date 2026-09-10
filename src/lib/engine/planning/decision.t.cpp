// @file decision.t.cpp
// @brief Unit tests for `planar.engine.planning.decision` (plan 996
// roadmap M12 item 8, task 6194).
//
// ## What these tests assert, and why they are shaped this way
//
// Twelve wiring cycles into this milestone, every one has surfaced a
// latent engine bug that was unit-tested and green until a caller existed:
// a `list` with no status predicate that resurrected terminal plans, a
// renderer that emitted invalid JSON, a `--scope` accepted and ignored on a
// destructive sweep, an `add`/`remove` pair that wrote no audit rows for
// four cycles because the READER was cut too. The common shape is a test
// that asserts a COUNT, or asserts against the engine's own read path, and
// therefore cannot see the engine and the test agreeing on the same wrong
// thing.
//
// So, four rules here:
//
//   1. **Row assertions go through raw SQL**, not through `show_decision`.
//      A test that reads back what it wrote with the same decoder cannot
//      catch a decoder that collapses SQL NULL onto the empty string.
//      `sql_text_or_null` renders NULL as the sentinel `<NULL>`.
//   2. **Every filter is proven to EXCLUDE**, by asserting the
//      out-of-scope rows SURVIVE in the table afterwards and by asserting
//      the returned ID SET rather than its size.
//   3. **The status and scope filters get MULTI-WAY discriminations.** The
//      status fixture holds one row in each of the four statuses and is
//      queried five ways; the scope fixture holds one row in each of the
//      three scope kinds and is queried five ways. An inert filter returns
//      the SAME set every time; a filter applied where it should not be
//      collapses to empty. Only real branch behaviour produces five
//      different answers.
//   4. **Every REFUSAL is followed by an after-state snapshot.** A refusal
//      that half-wrote is the worse bug and only the after-state
//      distinguishes it. `supersede`'s `link_exists` rollback is the
//      sharpest case: the status UPDATE runs BEFORE the edge INSERT that
//      fails, so a missing rollback leaves a decision marked superseded by
//      nothing.
//
// One class of defect a raw-SQL row snapshot still cannot see: an engine
// READ that collapses NULL onto a default before the value ever reaches
// SQL. For that class the assertion is on the RENDERED form, where `null`
// and `""` are different bytes — see the render cases at the bottom.
//
// `decisions.body` is the inverse trap: it is `not null`, so the engine
// must never bind NULL there and the empty string is a legal value. The
// create cases assert `''` explicitly against the `<NULL>` sentinel.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.decision;

namespace {

using planar::engine::planning::accept_decision;
using planar::engine::planning::create_decision;
using planar::engine::planning::decision;
using planar::engine::planning::decision_create_args;
using planar::engine::planning::decision_error;
using planar::engine::planning::decision_list_filter;
using planar::engine::planning::decision_scope_kind;
using planar::engine::planning::decision_status;
using planar::engine::planning::decision_status_from_text;
using planar::engine::planning::decision_status_is_terminal;
using planar::engine::planning::decision_status_to_text;
using planar::engine::planning::decisions_for_task;
using planar::engine::planning::list_decisions;
using planar::engine::planning::render_json;
using planar::engine::planning::render_list_json;
using planar::engine::planning::render_list_text;
using planar::engine::planning::render_text;
using planar::engine::planning::show_decision;
using planar::engine::planning::supersede_decision;
using planar::engine::planning::withdraw_decision;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_planning_decision_test_{}_{}.db",
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
/// absent optional into an empty string — or, on `body`, the reverse.
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

/// @brief Every `decisions` row rendered as
/// `id|scope_kind|scope_id|title|body|rationale|status|decided_at|session_id`,
/// rows joined by `;`, SQL NULL as `<NULL>`.
///
/// `decided_at` is a wall-clock stamp, so it is projected as the two-state
/// `<NULL>` / `SET`; whether the column is null is the contract, its value
/// is not.
auto decision_rows(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select id, scope_kind, scope_id, title, body, rationale, status, "
                           "case when decided_at is null then null else 'SET' end, session_id "
                           "from decisions order by id");
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
    for (int col = 0; col < 9; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `audit_log` row as `verb|entity_kind|entity_id|summary|actor|scope`.
///
/// `actor` and `scope` are included precisely because they are ALWAYS NULL
/// on this path: a port that helpfully filled them in would be writing rows
/// the oracle does not, invisibly to every other assertion.
auto audit_rows(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select verb, entity_kind, entity_id, summary, actor, scope from audit_log order by id");
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
    for (int col = 0; col < 6; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->is_null(col) ? std::string{"<NULL>"} : stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `entity_links` row as
/// `from_kind:from_id|to_kind:to_id|relationship`.
auto edge_rows(planar::db::connection& conn) -> std::string {
  auto stmt = conn.prepare("select from_kind, from_id, to_kind, to_id, relationship from entity_links order by id");
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
    joined += std::format("{}:{}|{}:{}|{}", stmt->column_text(0), stmt->column_int64(1), stmt->column_text(2),
                          stmt->column_int64(3), stmt->column_text(4));
  }
  return joined;
}

auto ids_of(const std::vector<decision>& rows) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  out.reserve(rows.size());
  for (const auto& d : rows) {
    out.push_back(d.id);
  }
  return out;
}

auto list_ids(planar::db::connection& conn, const decision_list_filter& filter) -> std::vector<std::int64_t> {
  auto rows = list_decisions(conn, filter);
  REQUIRE(rows.has_value());
  return ids_of(*rows);
}

/// @brief Create a decision, requiring success, and return it.
auto must_create(planar::db::connection& conn, decision_create_args args) -> decision {
  auto made = create_decision(conn, args);
  REQUIRE(made.has_value());
  return *made;
}

} // namespace

// ===========================================================================
// create
// ===========================================================================

TEST_CASE("create_decision defaults to proposed and writes NULL, not '', for every absent optional") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const made = must_create(conn, {.title = "Use C++26", .body = "Reasons..."});
  CHECK(made.id == 1);
  CHECK(made.status == decision_status::proposed);
  CHECK(made.scope_kind == decision_scope_kind::global);
  CHECK_FALSE(made.decided_at.has_value());
  CHECK_FALSE(made.rationale.has_value());
  CHECK_FALSE(made.session_id.has_value());

  // Raw SQL, not the engine's own reader: `<NULL>` and `''` are different
  // answers here and only this projection can tell them apart.
  CHECK(decision_rows(conn) == "1|global|<NULL>|Use C++26|Reasons...|<NULL>|proposed|<NULL>|<NULL>");
  CHECK(audit_rows(conn) == "create|decision|1|create decision 'Use C++26'|<NULL>|<NULL>");
  CHECK(edge_rows(conn).empty());
}

TEST_CASE("create_decision writes an EMPTY BODY as '', never as NULL") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // `decisions.body` is `not null`, so this is the mirror image of every
  // other optional on the row: the engine must bind `''`. A port that
  // treated an empty body as "absent" and bound NULL would fail the column
  // constraint outright — and one that substituted a placeholder would pass
  // a title-only assertion while silently inventing operator text.
  must_create(conn, {.title = "empty", .body = ""});
  CHECK(sql_text_or_null(conn, "select body from decisions where id = 1") == "");
  CHECK(sql_text_or_null(conn, "select rationale from decisions where id = 1") == "<NULL>");
}

TEST_CASE("create_decision distinguishes an ABSENT rationale from an EMPTY one") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  must_create(conn, {.title = "absent", .body = "b"});
  must_create(conn, {.title = "empty", .body = "b", .rationale = std::string{""}});

  CHECK(sql_text_or_null(conn, "select rationale from decisions where id = 1") == "<NULL>");
  CHECK(sql_text_or_null(conn, "select rationale from decisions where id = 2") == "");
}

TEST_CASE("create_decision stamps session_id when given one and leaves it NULL otherwise") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into sessions (vendor) values ('test')");

  must_create(conn, {.title = "with", .body = "b", .session_id = 1});
  must_create(conn, {.title = "without", .body = "b"});

  CHECK(sql_text_or_null(conn, "select session_id from decisions where id = 1") == "1");
  CHECK(sql_text_or_null(conn, "select session_id from decisions where id = 2") == "<NULL>");
}

TEST_CASE("create_decision resolves every scope grammar the oracle accepts") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  exec(conn, "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')");

  must_create(conn, {.title = "g", .body = "b", .scope = std::string{"global"}});
  must_create(conn, {.title = "a", .body = "b", .scope = std::string{"acme"}});
  must_create(conn, {.title = "r", .body = "b", .scope = std::string{"repo:foo"}});
  // No `scope` argument at all folds to global — a DIFFERENT code path from
  // the literal string "global", and the two must agree.
  must_create(conn, {.title = "n", .body = "b"});

  CHECK(decision_rows(conn) == "1|global|<NULL>|g|b|<NULL>|proposed|<NULL>|<NULL>;"
                               "2|association|1|a|b|<NULL>|proposed|<NULL>|<NULL>;"
                               "3|repo|1|r|b|<NULL>|proposed|<NULL>|<NULL>;"
                               "4|global|<NULL>|n|b|<NULL>|proposed|<NULL>|<NULL>");
}

TEST_CASE("create_decision refuses an unresolvable scope slug and writes NOTHING") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const made = create_decision(conn, {.title = "x", .body = "y", .scope = std::string{"no-such-slug"}});
  REQUIRE_FALSE(made.has_value());
  CHECK(made.error() == decision_error::slug_not_found);
  // The refusal's AFTER-STATE. The scope resolves before the INSERT, so a
  // half-written refusal would leave a global-scoped row behind.
  CHECK(decision_rows(conn).empty());
  CHECK(audit_rows(conn).empty());
}

TEST_CASE("create_decision --plan writes a DANGLING edge without validating the plan") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // The oracle does NOT check the plan's existence here — `question add
  // --plan 9999` refuses before writing anything, `decision add --plan
  // 9999` succeeds and leaves an edge to a plan that does not exist.
  // Captured by running both. This test pins the divergence so a future
  // "consistency" fix is a deliberate, visible change.
  auto const made = create_decision(conn, {.title = "dangling", .body = "b", .plan_id = 9999});
  REQUIRE(made.has_value());
  CHECK(edge_rows(conn) == "decision:1|plan:9999|derives-from");
  // And exactly ONE audit row, verb `create`. The edge gets no `link` row —
  // which is what makes `supersede`'s edge (also unaudited) consistent with
  // this one and inconsistent with `decision link`'s.
  CHECK(audit_rows(conn) == "create|decision|1|create decision 'dangling'|<NULL>|<NULL>");
}

// ===========================================================================
// show
// ===========================================================================

TEST_CASE("show_decision reports not_found for an id that does not exist") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      found = show_decision(conn, 9999);
  REQUIRE_FALSE(found.has_value());
  CHECK(found.error() == decision_error::not_found);
}

// ===========================================================================
// list — the status filter, five ways
// ===========================================================================

TEST_CASE("list_decisions: the UNSET status filter means {proposed, accepted}, not everything") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const proposed = must_create(conn, {.title = "p", .body = "b"});
  auto const accepted = must_create(conn, {.title = "a", .body = "b"});
  auto const target   = must_create(conn, {.title = "s", .body = "b"});
  auto const by       = must_create(conn, {.title = "by", .body = "b"});
  auto const drawn    = must_create(conn, {.title = "w", .body = "b"});
  REQUIRE(accept_decision(conn, accepted.id).has_value());
  REQUIRE(supersede_decision(conn, target.id, by.id).has_value());
  REQUIRE(withdraw_decision(conn, drawn.id).has_value());

  // FIVE queries over ONE fixture. An inert filter returns the same
  // five-element set every time; a filter that always applied `status = ?`
  // with an empty binding would collapse the first to empty. Only real
  // branch behaviour yields five different non-empty answers.
  CHECK(list_ids(conn, {}) == std::vector<std::int64_t>{proposed.id, accepted.id, by.id});
  CHECK(list_ids(conn, {.status = decision_status::proposed}) == std::vector<std::int64_t>{proposed.id, by.id});
  CHECK(list_ids(conn, {.status = decision_status::accepted}) == std::vector<std::int64_t>{accepted.id});
  CHECK(list_ids(conn, {.status = decision_status::superseded}) == std::vector<std::int64_t>{target.id});
  CHECK(list_ids(conn, {.status = decision_status::withdrawn}) == std::vector<std::int64_t>{drawn.id});

  // The excluded rows SURVIVE. A "filter" that deleted them would satisfy
  // every set assertion above.
  CHECK(sql_int(conn, "select count(*) from decisions") == 5);
  CHECK(sql_text_or_null(conn, "select status from decisions where id = 3") == "superseded");
  CHECK(sql_text_or_null(conn, "select status from decisions where id = 5") == "withdrawn");
}

// ===========================================================================
// list — the scope filter, five ways
// ===========================================================================

TEST_CASE("list_decisions: the scope filter EXCLUDES, and an empty scope set applies no predicate") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  exec(conn, "insert into projects (slug, name, root_path) values ('foo', 'Foo', '/work/foo')");

  auto const g = must_create(conn, {.title = "g", .body = "b", .scope = std::string{"global"}});
  auto const a = must_create(conn, {.title = "a", .body = "b", .scope = std::string{"acme"}});
  auto const r = must_create(conn, {.title = "r", .body = "b", .scope = std::string{"repo:foo"}});

  // No scope member at all: NO predicate, so all three.
  CHECK(list_ids(conn, {}) == std::vector<std::int64_t>{g.id, a.id, r.id});
  CHECK(list_ids(conn, {.scope = std::string{"global"}}) == std::vector<std::int64_t>{g.id});
  CHECK(list_ids(conn, {.scope = std::string{"acme"}}) == std::vector<std::int64_t>{a.id});
  CHECK(list_ids(conn, {.scope = std::string{"repo:foo"}}) == std::vector<std::int64_t>{r.id});
  // `scope` and `scopes` are OR-ed into one disjunction, in that order.
  CHECK(list_ids(conn, {.scope = std::string{"global"}, .scopes = {"repo:foo"}}) == std::vector<std::int64_t>{g.id, r.id});
  CHECK(list_ids(conn, {.scopes = {"acme", "repo:foo"}}) == std::vector<std::int64_t>{a.id, r.id});

  // Survivors.
  CHECK(decision_rows(conn) == "1|global|<NULL>|g|b|<NULL>|proposed|<NULL>|<NULL>;"
                               "2|association|1|a|b|<NULL>|proposed|<NULL>|<NULL>;"
                               "3|repo|1|r|b|<NULL>|proposed|<NULL>|<NULL>");
}

TEST_CASE("list_decisions fails the WHOLE call when any scope member does not resolve") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  must_create(conn, {.title = "a", .body = "b", .scope = std::string{"acme"}});

  // Skipping the bad member and listing the rest would return a SHORT list
  // that looks complete — the silent-filter defect this milestone keeps
  // closing.
  auto const rows = list_decisions(conn, {.scopes = {"acme", "no-such-slug"}});
  REQUIRE_FALSE(rows.has_value());
  CHECK(rows.error() == decision_error::slug_not_found);
}

TEST_CASE("list_decisions: the --plan filter EXCLUDES unlinked decisions, which survive") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'P1', 'p1', 'draft')");
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'P2', 'p2', 'draft')");

  auto const linked   = must_create(conn, {.title = "linked", .body = "b", .plan_id = 1});
  auto const other    = must_create(conn, {.title = "other", .body = "b", .plan_id = 2});
  auto const unlinked = must_create(conn, {.title = "unlinked", .body = "b"});

  CHECK(list_ids(conn, {.plan_id = 1}) == std::vector<std::int64_t>{linked.id});
  CHECK(list_ids(conn, {.plan_id = 2}) == std::vector<std::int64_t>{other.id});
  // A filter that matched on the mere PRESENCE of an edge would return two.
  CHECK(list_ids(conn, {.plan_id = 3}).empty());
  CHECK(list_ids(conn, {}) == std::vector<std::int64_t>{linked.id, other.id, unlinked.id});
  CHECK(sql_int(conn, "select count(*) from decisions") == 3);
}

TEST_CASE("list_decisions: the plan filter only matches derives-from decision->plan edges") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global', 'P1', 'p1', 'draft')");
  auto const d = must_create(conn, {.title = "cites-only", .body = "b"});
  // The SAME endpoints under a different relationship must NOT match — a
  // predicate that dropped the `relationship` conjunct would still pass
  // every case above.
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('decision', 1, 'plan', 1, 'cites')");
  CHECK(list_ids(conn, {.plan_id = 1}).empty());
  CHECK(list_ids(conn, {}) == std::vector<std::int64_t>{d.id});
}

// ===========================================================================
// accept / withdraw
// ===========================================================================

TEST_CASE("accept_decision flips the status, stamps decided_at, and writes one audit row") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      made = must_create(conn, {.title = "x", .body = "y"});

  auto const accepted = accept_decision(conn, made.id);
  REQUIRE(accepted.has_value());
  CHECK(accepted->status == decision_status::accepted);
  CHECK(accepted->decided_at.has_value());
  CHECK(decision_rows(conn) == "1|global|<NULL>|x|y|<NULL>|accepted|SET|<NULL>");
  CHECK(audit_rows(conn) == "create|decision|1|create decision 'x'|<NULL>|<NULL>;"
                            "status_change|decision|1|accept|<NULL>|<NULL>");
}

TEST_CASE("accept_decision on an ALREADY-accepted decision succeeds and audits again") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      made = must_create(conn, {.title = "x", .body = "y"});
  REQUIRE(accept_decision(conn, made.id).has_value());

  // `accepted -> accepted` short-circuits on the identity check BEFORE the
  // decision matrix is consulted, so it is not a refusal. Oracle-confirmed
  // by running `decision accept` twice.
  auto const again = accept_decision(conn, made.id);
  REQUIRE(again.has_value());
  CHECK(sql_int(conn, "select count(*) from audit_log where verb = 'status_change'") == 2);
}

TEST_CASE("withdraw_decision leaves decided_at exactly as it found it") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const never    = must_create(conn, {.title = "never", .body = "b"});
  auto const accepted = must_create(conn, {.title = "accepted", .body = "b"});
  REQUIRE(accept_decision(conn, accepted.id).has_value());

  REQUIRE(withdraw_decision(conn, never.id).has_value());
  auto const after = withdraw_decision(conn, accepted.id);
  REQUIRE(after.has_value());

  // A withdraw that stamped `decided_at` would be indistinguishable from
  // accept on row 1 and would silently rewrite row 2's real stamp. Both
  // halves matter, so both are asserted.
  CHECK(sql_text_or_null(conn, "select decided_at from decisions where id = 1") == "<NULL>");
  CHECK(sql_text_or_null(conn, "select case when decided_at is null then null else 'SET' end from decisions where id = 2") ==
        "SET");
  CHECK(decision_rows(conn) == "1|global|<NULL>|never|b|<NULL>|withdrawn|<NULL>|<NULL>;"
                               "2|global|<NULL>|accepted|b|<NULL>|withdrawn|SET|<NULL>");
}

TEST_CASE("accept_decision REFUSES a terminal source and changes nothing") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      made = must_create(conn, {.title = "x", .body = "y"});
  REQUIRE(withdraw_decision(conn, made.id).has_value());

  auto const before_rows  = decision_rows(conn);
  auto const before_audit = audit_rows(conn);
  auto const refused      = accept_decision(conn, made.id);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == decision_error::terminal_status);
  // The refusal's AFTER-STATE, including `updated_at` by way of the whole
  // row snapshot: a refusal that ran the UPDATE first would show here.
  CHECK(decision_rows(conn) == before_rows);
  CHECK(audit_rows(conn) == before_audit);
}

TEST_CASE("withdraw_decision REFUSES a superseded source and changes nothing") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});
  auto const      new_d = must_create(conn, {.title = "new", .body = "y"});
  REQUIRE(supersede_decision(conn, old_d.id, new_d.id).has_value());

  auto const before_rows  = decision_rows(conn);
  auto const before_audit = audit_rows(conn);
  auto const refused      = withdraw_decision(conn, old_d.id);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == decision_error::terminal_status);
  CHECK(decision_rows(conn) == before_rows);
  CHECK(audit_rows(conn) == before_audit);
}

TEST_CASE("withdraw_decision on an already-withdrawn decision SUCCEEDS by the identity short-circuit") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      made = must_create(conn, {.title = "x", .body = "y"});
  REQUIRE(withdraw_decision(conn, made.id).has_value());

  // `withdrawn` is terminal, and yet `withdrawn -> withdrawn` succeeds:
  // `check_transition` returns before consulting the decision arm. That is
  // the oracle's behaviour and it is why "terminal" alone does not predict
  // the outcome — the TARGET matters too.
  auto const again = withdraw_decision(conn, made.id);
  REQUIRE(again.has_value());
  CHECK(sql_int(conn, "select count(*) from audit_log where summary = 'withdraw'") == 2);
}

TEST_CASE("a transition on a missing decision reports not_found and writes nothing") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  for (auto const& refused : {accept_decision(conn, 9999), withdraw_decision(conn, 9999)}) {
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == decision_error::not_found);
  }
  CHECK(audit_rows(conn).empty());
}

// ===========================================================================
// supersede
// ===========================================================================

TEST_CASE("supersede_decision flips the old status, writes the edge, and audits ONCE") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});
  auto const      new_d = must_create(conn, {.title = "new", .body = "z"});

  auto const done = supersede_decision(conn, old_d.id, new_d.id);
  REQUIRE(done.has_value());
  CHECK(done->status == decision_status::superseded);
  // The NEW decision is untouched — it does not become "accepted" or
  // anything else, and nothing is written against its id.
  CHECK(decision_rows(conn) == "1|global|<NULL>|old|y|<NULL>|superseded|<NULL>|<NULL>;"
                               "2|global|<NULL>|new|z|<NULL>|proposed|<NULL>|<NULL>");
  // The edge points NEW -> OLD, which is the reverse of the call's argument
  // order. Getting this backwards would still produce one row and pass a
  // count assertion.
  CHECK(edge_rows(conn) == "decision:2|decision:1|supersedes");
  // Exactly ONE new audit row, and NO `link|entity_link|…` row: the oracle
  // writes the edge raw, where `decision link` on the same pair would audit
  // it. That absence is what forbids composing `engine_entitylink` here.
  CHECK(audit_rows(conn) == "create|decision|1|create decision 'old'|<NULL>|<NULL>;"
                            "create|decision|2|create decision 'new'|<NULL>|<NULL>;"
                            "status_change|decision|1|supersede: decision 1 superseded by decision 2|<NULL>|<NULL>");
}

TEST_CASE("supersede_decision refuses a missing NEW decision before writing anything") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});

  auto const before_rows = decision_rows(conn);
  auto const refused     = supersede_decision(conn, old_d.id, 9999);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == decision_error::not_found);
  CHECK(decision_rows(conn) == before_rows);
  CHECK(edge_rows(conn).empty());
  CHECK(sql_int(conn, "select count(*) from audit_log where verb = 'status_change'") == 0);
}

TEST_CASE("supersede_decision ROLLS BACK the status flip when the edge already exists") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});
  auto const      new_d = must_create(conn, {.title = "new", .body = "z"});
  // Pre-seed the edge the way an operator would, through `decision link`.
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
             "values ('decision', 2, 'decision', 1, 'supersedes')");

  auto const before_rows  = decision_rows(conn);
  auto const before_audit = audit_rows(conn);
  auto const before_edges = edge_rows(conn);

  auto const refused = supersede_decision(conn, old_d.id, new_d.id);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == decision_error::link_exists);

  // THE POINT OF THIS CASE. The status UPDATE runs BEFORE the INSERT that
  // fails. Without the rollback, decision 1 is left `superseded` — marked
  // as replaced by a link the operation did not create — and its
  // `updated_at` is rewritten. The whole-row snapshot catches both. This
  // was confirmed against the oracle by exactly this sequence: the row
  // stayed `proposed`.
  CHECK(decision_rows(conn) == before_rows);
  CHECK(sql_text_or_null(conn, "select status from decisions where id = 1") == "proposed");
  CHECK(edge_rows(conn) == before_edges);
  CHECK(audit_rows(conn) == before_audit);
}

TEST_CASE("supersede_decision refuses a WITHDRAWN old decision and changes nothing") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});
  auto const      new_d = must_create(conn, {.title = "new", .body = "z"});
  REQUIRE(withdraw_decision(conn, old_d.id).has_value());

  // `withdrawn` is the ONLY source `supersede` can refuse as terminal — see
  // the case below for why `superseded` is not one.
  auto const before_rows  = decision_rows(conn);
  auto const before_edges = edge_rows(conn);
  auto const refused      = supersede_decision(conn, old_d.id, new_d.id);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == decision_error::terminal_status);
  CHECK(decision_rows(conn) == before_rows);
  CHECK(edge_rows(conn) == before_edges);
}

TEST_CASE("supersede_decision on an ALREADY-SUPERSEDED decision succeeds and adds a SECOND edge") {
  scratch_db_path scratch;
  auto            conn   = open_migrated(scratch);
  auto const      old_d  = must_create(conn, {.title = "old", .body = "y"});
  auto const      first  = must_create(conn, {.title = "first", .body = "z"});
  auto const      second = must_create(conn, {.title = "second", .body = "z"});
  REQUIRE(supersede_decision(conn, old_d.id, first.id).has_value());

  // `superseded` is terminal, and this still SUCCEEDS: the target status is
  // also `superseded`, so `check_transition`'s identity short-circuit
  // returns before the decision matrix is consulted. A decision therefore
  // accumulates one `supersedes` edge per superseding decision, and
  // "terminal" does not predict the outcome on its own — the TARGET does.
  //
  // Oracle-confirmed by running `decision supersede 3 --by 2` then
  // `decision supersede 3 --by 4`: both exit 0, two edges, two audit rows.
  // The first version of this case asserted `terminal_status` here from the
  // matrix alone and was WRONG; the engine caught it.
  auto const again = supersede_decision(conn, old_d.id, second.id);
  REQUIRE(again.has_value());
  CHECK(again->status == decision_status::superseded);
  CHECK(edge_rows(conn) == "decision:2|decision:1|supersedes;decision:3|decision:1|supersedes");
  CHECK(sql_int(conn, "select count(*) from audit_log where verb = 'status_change'") == 2);
  // The SAME pair twice is still `link_exists`, so the second edge is the
  // only thing that made this succeed.
  auto const dup = supersede_decision(conn, old_d.id, second.id);
  REQUIRE_FALSE(dup.has_value());
  CHECK(dup.error() == decision_error::link_exists);
}

TEST_CASE("supersede_decision permits a WITHDRAWN decision to be the superseding one") {
  scratch_db_path scratch;
  auto            conn  = open_migrated(scratch);
  auto const      old_d = must_create(conn, {.title = "old", .body = "y"});
  auto const      new_d = must_create(conn, {.title = "new", .body = "z"});
  REQUIRE(withdraw_decision(conn, new_d.id).has_value());

  // Only the OLD decision's status is validated. A port that checked both
  // would refuse this, plausibly and wrongly.
  auto const done = supersede_decision(conn, old_d.id, new_d.id);
  REQUIRE(done.has_value());
  CHECK(done->status == decision_status::superseded);
  CHECK(sql_text_or_null(conn, "select status from decisions where id = 2") == "withdrawn");
}

// ===========================================================================
// decisions_for_task
// ===========================================================================

TEST_CASE("decisions_for_task returns the task's session decisions PLUS every global one") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  exec(conn, "insert into associations (slug, name, kind) values ('acme', 'Acme', 'org')");
  exec(conn, "insert into tasks (scope_kind, title, status) values ('global', 't1', 'todo')");
  exec(conn, "insert into tasks (scope_kind, title, status) values ('global', 't2', 'todo')");
  exec(conn, "insert into sessions (vendor, task_id) values ('test', 1)");
  exec(conn, "insert into sessions (vendor, task_id) values ('test', 2)");

  // Association-scoped so the `global` arm cannot be what admits them.
  auto const mine   = must_create(conn, {.title = "mine", .body = "b", .session_id = 1, .scope = std::string{"acme"}});
  auto const theirs = must_create(conn, {.title = "theirs", .body = "b", .session_id = 2, .scope = std::string{"acme"}});
  auto const global = must_create(conn, {.title = "global", .body = "b"});

  auto const rows = decisions_for_task(conn, 1);
  REQUIRE(rows.has_value());
  CHECK(ids_of(*rows) == std::vector<std::int64_t>{mine.id, global.id});
  // The other task's decision SURVIVES — it is excluded, not absent.
  CHECK(sql_int(conn, "select count(*) from decisions") == 3);
  CHECK(sql_text_or_null(conn, "select title from decisions where id = 2") == "theirs");

  // The `global` arm is UNCONDITIONAL: a task id that does not exist still
  // gets every global decision. This is why the reader cannot be used as a
  // membership test, and asserting it stops a future "optimisation" from
  // quietly ANDing the two arms.
  auto const none = decisions_for_task(conn, 999);
  REQUIRE(none.has_value());
  CHECK(ids_of(*none) == std::vector<std::int64_t>{global.id});
  CHECK(theirs.id == 2);
}

// ===========================================================================
// status vocabulary
// ===========================================================================

TEST_CASE("decision status text round-trips and only the two terminal members report terminal") {
  for (auto const s :
       {decision_status::proposed, decision_status::accepted, decision_status::superseded, decision_status::withdrawn}) {
    CHECK(decision_status_from_text(decision_status_to_text(s)) == s);
  }
  CHECK_FALSE(decision_status_from_text("bogus").has_value());
  // `blocks`-style near-misses and the sibling families' vocabulary must
  // NOT parse: `open` is a question status, `draft` an artifact/plan one.
  CHECK_FALSE(decision_status_from_text("open").has_value());
  CHECK_FALSE(decision_status_from_text("draft").has_value());
  CHECK_FALSE(decision_status_is_terminal(decision_status::proposed));
  CHECK_FALSE(decision_status_is_terminal(decision_status::accepted));
  CHECK(decision_status_is_terminal(decision_status::superseded));
  CHECK(decision_status_is_terminal(decision_status::withdrawn));
}

// ===========================================================================
// renderers — where a NULL collapsed by an engine READ becomes visible
// ===========================================================================

TEST_CASE("render_text emits the oracle's block with every optional line absent") {
  const decision d{
      .id         = 1,
      .scope_kind = decision_scope_kind::global,
      .scope_id   = std::nullopt,
      .title      = "first",
      .body       = "because",
      .rationale  = std::nullopt,
      .status     = decision_status::proposed,
      .decided_at = std::nullopt,
      .session_id = std::nullopt,
      .created_at = "2026-08-26T19:10:33.135Z",
      .updated_at = "2026-08-26T19:10:33.135Z",
  };
  // Captured from `planar decision add first --body because` against a
  // scratch DB. Values start at column 13 — ONE wider than `question`'s
  // block and TWO wider than `plan`'s.
  CHECK(render_text(d) == "id:         1\n"
                          "title:      first\n"
                          "status:     proposed\n"
                          "scope:      global\n"
                          "body:       because\n"
                          "created:    2026-08-26T19:10:33.135Z\n"
                          "updated:    2026-08-26T19:10:33.135Z\n");
}

TEST_CASE("render_text emits all four conditional lines in the oracle's order") {
  const decision d{
      .id         = 7,
      .scope_kind = decision_scope_kind::repo,
      .scope_id   = 1,
      .title      = "repo-scoped",
      .body       = "rb",
      .rationale  = "r2",
      .status     = decision_status::accepted,
      .decided_at = "2026-08-26T19:11:39.283Z",
      .session_id = 3,
      .created_at = "2026-08-26T19:11:22.324Z",
      .updated_at = "2026-08-26T19:11:39.283Z",
  };
  // `scope:` carries `:<id>` only when `scope_id` is set; the three
  // conditional lines sit between `body:` and `created:` in this order.
  CHECK(render_text(d) == "id:         7\n"
                          "title:      repo-scoped\n"
                          "status:     accepted\n"
                          "scope:      repo:1\n"
                          "body:       rb\n"
                          "rationale:  r2\n"
                          "decided:    2026-08-26T19:11:39.283Z\n"
                          "session:    3\n"
                          "created:    2026-08-26T19:11:22.324Z\n"
                          "updated:    2026-08-26T19:11:39.283Z\n");
}

TEST_CASE("render_text keeps the body line for an EMPTY body") {
  const decision d{
      .id         = 3,
      .scope_kind = decision_scope_kind::global,
      .scope_id   = std::nullopt,
      .title      = "E",
      .body       = "",
      .rationale  = std::nullopt,
      .status     = decision_status::proposed,
      .decided_at = std::nullopt,
      .session_id = std::nullopt,
      .created_at = "T1",
      .updated_at = "T1",
  };
  // `body:` is UNCONDITIONAL — the column is `not null`, so unlike
  // `rationale:` it does not disappear when empty. It renders as a label
  // with nothing after it, INCLUDING the trailing space run.
  CHECK(render_text(d).contains("body:       \n"));
}

TEST_CASE("render_json distinguishes SQL NULL from the empty string on every nullable field") {
  const decision absent{
      .id         = 1,
      .scope_kind = decision_scope_kind::global,
      .scope_id   = std::nullopt,
      .title      = "E",
      .body       = "",
      .rationale  = std::nullopt,
      .status     = decision_status::proposed,
      .decided_at = std::nullopt,
      .session_id = std::nullopt,
      .created_at = "T1",
      .updated_at = "T1",
  };
  // This is the assertion a raw-SQL snapshot CANNOT make: an engine read
  // that folded NULL onto "" would leave the table correct and this line
  // wrong. Note `"body":""` beside `"rationale":null` in the SAME object.
  CHECK(render_json(absent) == R"({"id":1,"scope_kind":"global","scope_id":null,"title":"E","body":"","rationale":null,)"
                               R"("status":"proposed","decided_at":null,"session_id":null,"created_at":"T1","updated_at":"T1"})");

  const decision present{
      .id         = 2,
      .scope_kind = decision_scope_kind::association,
      .scope_id   = 4,
      .title      = "second",
      .body       = "b2",
      .rationale  = "",
      .status     = decision_status::accepted,
      .decided_at = "T2",
      .session_id = 1,
      .created_at = "T1",
      .updated_at = "T2",
  };
  CHECK(render_json(present) == R"({"id":2,"scope_kind":"association","scope_id":4,"title":"second","body":"b2","rationale":"",)"
                                R"("status":"accepted","decided_at":"T2","session_id":1,"created_at":"T1","updated_at":"T2"})");
}

TEST_CASE("render_json escapes a title that would otherwise break the object") {
  const decision d{
      .id         = 1,
      .scope_kind = decision_scope_kind::global,
      .scope_id   = std::nullopt,
      .title      = R"(say "hi"\)",
      .body       = "line1\nline2",
      .rationale  = std::nullopt,
      .status     = decision_status::proposed,
      .decided_at = std::nullopt,
      .session_id = std::nullopt,
      .created_at = "T1",
      .updated_at = "T1",
  };
  auto const out = render_json(d);
  CHECK(out.contains(R"("title":"say \"hi\"\\")"));
  CHECK(out.contains(R"("body":"line1\nline2")"));
}

TEST_CASE("render_list_text renders 'no decisions' for the empty case, WITHOUT parentheses") {
  // `question` renders `(no questions)`, `plan` renders `(no plans)`, and
  // this one renders neither shape. Captured from `decision list --scope
  // global` against an empty database.
  CHECK(render_list_text({}) == "no decisions\n");
}

TEST_CASE("render_list_text pads id right in five and status left in ten") {
  const std::vector<decision> items{
      {.id         = 1,
       .scope_kind = decision_scope_kind::global,
       .scope_id   = std::nullopt,
       .title      = "first",
       .body       = "b",
       .rationale  = std::nullopt,
       .status     = decision_status::accepted,
       .decided_at = std::nullopt,
       .session_id = std::nullopt,
       .created_at = "T",
       .updated_at = "T"},
      {.id         = 123456,
       .scope_kind = decision_scope_kind::global,
       .scope_id   = std::nullopt,
       .title      = "planned",
       .body       = "b",
       .rationale  = std::nullopt,
       .status     = decision_status::superseded,
       .decided_at = std::nullopt,
       .session_id = std::nullopt,
       .created_at = "T",
       .updated_at = "T"},
  };
  // Row two proves the id column OVERFLOWS rather than truncating, and that
  // `superseded` (ten characters) leaves the status column exactly full
  // with the two-space gutter intact.
  CHECK(render_list_text(items) == "    1  accepted    first\n"
                                   "123456  superseded  planned\n");
}

TEST_CASE("render_list_json renders [] for the empty case and comma-joins otherwise") {
  CHECK(render_list_json({}) == "[]");
  const std::vector<decision> items{
      {.id         = 1,
       .scope_kind = decision_scope_kind::global,
       .scope_id   = std::nullopt,
       .title      = "a",
       .body       = "b",
       .rationale  = std::nullopt,
       .status     = decision_status::proposed,
       .decided_at = std::nullopt,
       .session_id = std::nullopt,
       .created_at = "T",
       .updated_at = "T"},
      {.id         = 2,
       .scope_kind = decision_scope_kind::global,
       .scope_id   = std::nullopt,
       .title      = "b",
       .body       = "b",
       .rationale  = std::nullopt,
       .status     = decision_status::proposed,
       .decided_at = std::nullopt,
       .session_id = std::nullopt,
       .created_at = "T",
       .updated_at = "T"},
  };
  CHECK(render_list_json(items) == std::format("[{},{}]", render_json(items[0]), render_json(items[1])));
}
