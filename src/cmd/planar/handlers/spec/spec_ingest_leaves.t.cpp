// @file spec_ingest_leaves.t.cpp
// @brief End-to-end tests for the `spec ingest` leaf (plan 996, task 6365).
//
// Same shape as `sync_leaves.t.cpp` / `artifact_leaves.t.cpp`: dispatch the
// real tree and table against a scratch root ($PLANAR_DB, $PLANAR_HOME,
// $PLANAR_WORKBENCH_ROOT, $HOME all redirected — never the operator's real
// database or workbench), then assert on both stdout/stderr and the
// resulting database rows.
//
// Fixture artifacts are seeded THROUGH THE CLI: `artifact add --plan
// <id> --body <markdown>` writes the DB row and the `derives-from` edge,
// then `workbench push <plan>` materializes it to the exact on-disk path
// `spec ingest` reads (`engine::workbench::sync`'s renderer wraps the body
// in `## Content`, which `spec_ingest.cpp`'s `extract_content_section`
// strips back off) — no hand-written files, no raw SQL inserts.
//
// ## Preview planning-state is unchanged: a FULL row-level dump, not a count
//
// `dump_planning_inventory` renders every planning table's rows (id-ordered,
// NULL distinguished from empty) into one string. The preview test snapshots
// it before and after a bare `spec ingest <plan>` and asserts byte equality —
// a row-count check alone could not catch a write that updates one row and
// deletes another (net zero), only a full dump can. The session ledger and
// audit log are explicitly outside this inventory: preview creates/reuses the
// ingestor session, appends a read entry, and its first session emits the
// runtime session-start audit row. Those effects are pinned separately.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.json_dom;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_specingest_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "wb", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_WORKBENCH_ROOT", (root / "wb").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Every planning table's rows, id-ordered, NULL distinguished from
/// the empty string, concatenated into one string. Used to prove a preview
/// touched no planning state: a byte-identical dump before and after is
/// strictly stronger evidence than any row-count comparison.
///
/// `schema_migrations` is schema metadata; `sessions`, `session_entries`, and
/// `audit_log` are documented runtime/audit exhaust. They are excluded
/// deliberately: the oracle's preview path (`apply.zig`'s `appendReadEntry`)
/// ensures the `ingestor` session and appends a best-effort `prefix='read'`
/// entry even without `--apply`; the newly-created session records
/// `create|session|...` in `audit_log`. Preview is read-only with respect to
/// this planning inventory; the runtime exhaust is pinned separately below.
/// @param conn An open connection to the fixture database.
/// @return The rendered dump.
auto dump_planning_inventory(planar::db::connection& conn) -> std::string {
  std::vector<std::string> tables;
  {
    auto stmt = conn.prepare("select name from sqlite_master where type = 'table' and name not like 'sqlite_%' "
                             "and name not like '%_fts%' "
                             "and name not in ('schema_migrations', 'sessions', 'session_entries', 'audit_log') order by name");
    REQUIRE(stmt.has_value());
    while (true) {
      auto stepped = stmt->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      tables.push_back(stmt->column_text(0));
    }
  }

  std::string out;
  for (auto const& table : tables) {
    // Column count: probe via a zero-row select is awkward through this
    // API, so instead read every row generically through `pragma
    // table_info`, then select those exact columns by name in order.
    std::vector<std::string> columns;
    auto                     info = conn.prepare(std::format("select name from pragma_table_info('{}') order by cid", table));
    REQUIRE(info.has_value());
    while (true) {
      auto stepped = info->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      columns.push_back(info->column_text(0));
    }
    if (columns.empty()) {
      continue;
    }
    std::string cols_csv;
    for (std::size_t i = 0; i < columns.size(); ++i) {
      if (i > 0) {
        cols_csv += ", ";
      }
      cols_csv += columns[i];
    }
    auto rows = conn.prepare(std::format("select {} from {} order by 1", cols_csv, table));
    REQUIRE(rows.has_value());
    while (true) {
      auto stepped = rows->step();
      REQUIRE(stepped.has_value());
      if (*stepped == planar::db::step_result::done) {
        break;
      }
      out += table;
      out += ':';
      for (std::size_t i = 0; i < columns.size(); ++i) {
        out += '|';
        out += rows->is_null(static_cast<int>(i)) ? std::string{"<NULL>"} : rows->column_text(static_cast<int>(i));
      }
      out += '\n';
    }
  }
  return out;
}

auto count(planar::db::connection& conn, std::string_view table) -> std::int64_t {
  auto stmt = conn.prepare(std::format("select count(*) from {}", table));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seeds a draft anchor plan whose workbench feature directory
/// carries a real `tech_spec` artifact and a real `roadmap` artifact — both
/// written to disk via `artifact add` + `workbench push`, never by hand.
/// @param fx The fixture.
/// @return The anchor plan's numeric id, as a string (spec ingest's
/// positional argument).
auto seed_anchor(const fixture& fx) -> std::string {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Ingest Fixture", "--json"}).code == 0);

  constexpr std::string_view tech_spec_body = "## Decisions\n"
                                              "\n"
                                              "### Use SQLite\n"
                                              "\n"
                                              "We chose SQLite for storage.\n";
  constexpr std::string_view roadmap_body   = "## M1\n"
                                              "\n"
                                              "- Implement the thing [slug: do-the-thing]\n";

  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body",
                        std::string(tech_spec_body), "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", std::string(roadmap_body),
                        "--json"})
              .code == 0);
  auto const pushed = dispatch(fx, {"workbench", "push", "1", "--json"});
  REQUIRE(pushed.code == 0);
  return "1";
}

auto seed_reviewed_reconcile_anchor(const fixture& fx) -> std::string {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Reviewed Reconcile Fixture", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Product", "--kind", "product_spec", "--plan", "1", "--body",
                        "## Intent\n\nDeliver the reviewed task.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech", "--kind", "tech_spec", "--plan", "1", "--body",
                        "## Decisions\n\n### Durable decision\n\nUse the reviewed route.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Deliver the reviewed task [slug: reviewed-root] [touches: planar]\n", "--json"})
              .code == 0);
  // The first apply deliberately has no authored coverage, so it creates the
  // normal ingestor placeholder which the replay below must retire.
  REQUIRE(dispatch(fx, {"artifact", "add", "Tests", "--kind", "test_spec", "--plan", "1", "--body", "## Scenarios\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "update", "2", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "update", "3", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "update", "4", "--status", "active", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  return "1";
}

// @brief Prepares a project, an association and one draft plan in `fx`.
auto seed_plan(const fixture& fx, std::string_view title) -> void {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", std::string(title), "--json"}).code == 0);
}

// @brief Deletes the pushed workbench file of artifact `id`.
auto remove_workbench_file(const fixture& fx, std::int64_t id) -> std::string {
  std::string const prefix = std::format("{}-", id);
  for (auto const& entry : std::filesystem::recursive_directory_iterator(fx.root / "wb")) {
    if (entry.is_regular_file() && entry.path().extension() == ".md" && entry.path().filename().string().starts_with(prefix)) {
      auto const name = entry.path().filename().string();
      std::filesystem::remove(entry.path());
      return name;
    }
  }
  FAIL("no workbench file for artifact " << id);
  return {};
}

// @brief The single integer a one-row query returns.
auto scalar(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_int64(0);
}

} // namespace

// ===========================================================================
// preview is genuinely read-only
// ===========================================================================

TEST_CASE("spec ingest preview preserves planning state while recording one reused ingestor read session",
          "[cmd][spec][ingest][preview][runtime-audit]") {
  auto const fx     = make_fixture("preview");
  auto const anchor = seed_anchor(fx);

  std::string  dump_before;
  std::int64_t audit_before = 0;
  {
    auto conn_before = open_db(fx);
    dump_before      = dump_planning_inventory(conn_before);
    audit_before     = count(conn_before, "audit_log");

    // Let the statement AND its connection destruct before dispatch opens a
    // second connection to mint the session. Leaving a reader live holds
    // SQLite's read lock and would make the best-effort audit path look like
    // it was skipped.
    auto ingestor_before =
        conn_before.prepare("select count(*) from sessions where vendor = 'ingestor' and vendor_session_id is null");
    REQUIRE(ingestor_before.has_value());
    auto ingestor_before_step = ingestor_before->step();
    REQUIRE(ingestor_before_step.has_value());
    REQUIRE(*ingestor_before_step == planar::db::step_result::row);
    CHECK(ingestor_before->column_int64(0) == 0);
  }

  auto const first = dispatch(fx, {"spec", "ingest", anchor});
  // A preview that found nothing to propose is not a REFUSAL — the
  // handler still renders and exits 0. Only `--strict` or `--apply`
  // failures exit non-zero, and neither flag is passed here. This non-empty
  // fixture proposes a child plan, task, and decision, so it reaches the
  // real `apply_diff` -> `session::ensure_active` path.
  CHECK(first.code == 0);
  auto const expected_text = std::format("project:proj/ingest-fixture/\n"
                                         "  + plan       {:<36} (1 tasks)\n"
                                         "  +   task     {:<36}\n"
                                         "  + decision   {:<36}\n"
                                         "\n"
                                         "3 additions, 0 updates, 0 proposed removals.\n"
                                         "coverage: 1 tasks (1 with slug, 0 without); 1 uncovered: do-the-thing\n"
                                         "Run with --apply to commit; add --apply-removals to cancel proposed removals.\n",
                                         "M1", "Implement the thing", "Use SQLite");
  CHECK(first.out == expected_text);

  std::int64_t session_id = 0;
  {
    // Scope every first-preview statement and this connection before the
    // second preview. A lingering SQLite reader would turn the reuse
    // assertion into a false green that never exercised the second append.
    auto       conn_after_first = open_db(fx);
    auto const dump_after_first = dump_planning_inventory(conn_after_first);

    // The strongest possible proof: not "same row counts" (which a paired
    // insert+delete would still pass) but the SAME BYTES.
    CHECK(dump_before == dump_after_first);

    // Scenario 2585: the first non-empty preview creates exactly one
    // ingestor session and one exact start audit. Actor and scope must be
    // SQL NULL, not empty strings. The separate read entry proves the real
    // handler reached `ensure_active`, rather than a unit-level shortcut.
    CHECK(count(conn_after_first, "audit_log") == audit_before + 1);
    CHECK(count(conn_after_first, "session_entries") == 1);

    auto ingestor_session_count =
        conn_after_first.prepare("select count(*) from sessions where vendor = 'ingestor' and vendor_session_id is null");
    REQUIRE(ingestor_session_count.has_value());
    auto ingestor_count_step = ingestor_session_count->step();
    REQUIRE(ingestor_count_step.has_value());
    REQUIRE(*ingestor_count_step == planar::db::step_result::row);
    CHECK(ingestor_session_count->column_int64(0) == 1);

    auto session = conn_after_first.prepare("select id from sessions where vendor = 'ingestor' and vendor_session_id is null");
    REQUIRE(session.has_value());
    auto session_step = session->step();
    REQUIRE(session_step.has_value());
    REQUIRE(*session_step == planar::db::step_result::row);
    session_id             = session->column_int64(0);
    auto no_second_session = session->step();
    REQUIRE(no_second_session.has_value());
    CHECK(*no_second_session == planar::db::step_result::done);

    auto audit = conn_after_first.prepare("select verb, entity_kind, entity_id, summary, actor, scope from audit_log where "
                                          "entity_kind = 'session' and entity_id = ? order by id");
    REQUIRE(audit.has_value());
    REQUIRE(audit->bind_int64(1, session_id).has_value());
    auto audit_step = audit->step();
    REQUIRE(audit_step.has_value());
    REQUIRE(*audit_step == planar::db::step_result::row);
    CHECK(audit->column_text(0) == "create");
    CHECK(audit->column_text(1) == "session");
    CHECK(audit->column_int64(2) == session_id);
    CHECK(audit->column_text(3) == "start session vendor=ingestor");
    CHECK(audit->is_null(4));
    CHECK(audit->is_null(5));
    auto no_second_audit = audit->step();
    REQUIRE(no_second_audit.has_value());
    CHECK(*no_second_audit == planar::db::step_result::done);

    auto read_entry = conn_after_first.prepare("select session_id, prefix, body from session_entries order by id");
    REQUIRE(read_entry.has_value());
    auto read_entry_step = read_entry->step();
    REQUIRE(read_entry_step.has_value());
    REQUIRE(*read_entry_step == planar::db::step_result::row);
    CHECK(read_entry->column_int64(0) == session_id);
    CHECK(read_entry->column_text(1) == "read");
    CHECK(read_entry->column_text(2) == "spec ingest preview plan:1");
  }

  // A second non-empty preview reuses the active ingestor session: no second
  // session/start-audit row, while a second documented read entry lands.
  auto const second = dispatch(fx, {"spec", "ingest", anchor});
  CHECK(second.code == 0);
  CHECK(second.out == expected_text);

  auto       conn_after_second = open_db(fx);
  auto const dump_after_second = dump_planning_inventory(conn_after_second);
  CHECK(dump_before == dump_after_second);
  auto reused_ingestor_count =
      conn_after_second.prepare("select count(*) from sessions where vendor = 'ingestor' and vendor_session_id is null");
  REQUIRE(reused_ingestor_count.has_value());
  auto reused_count_step = reused_ingestor_count->step();
  REQUIRE(reused_count_step.has_value());
  REQUIRE(*reused_count_step == planar::db::step_result::row);
  CHECK(reused_ingestor_count->column_int64(0) == 1);
  CHECK(count(conn_after_second, "audit_log") == audit_before + 1);

  auto entries =
      conn_after_second.prepare("select session_id, prefix, body from session_entries where session_id = ? order by ordinal");
  REQUIRE(entries.has_value());
  REQUIRE(entries->bind_int64(1, session_id).has_value());
  for (std::int64_t ordinal = 1; ordinal <= 2; ++ordinal) {
    auto entry_step = entries->step();
    REQUIRE(entry_step.has_value());
    REQUIRE(*entry_step == planar::db::step_result::row);
    CHECK(entries->column_int64(0) == session_id);
    CHECK(entries->column_text(1) == "read");
    CHECK(entries->column_text(2) == "spec ingest preview plan:1");
  }
  auto no_third_entry = entries->step();
  REQUIRE(no_third_entry.has_value());
  CHECK(*no_third_entry == planar::db::step_result::done);
}

TEST_CASE("spec ingest preview with --json is also read-only", "[cmd][spec][ingest][preview][json]") {
  auto const fx     = make_fixture("preview_json");
  auto const anchor = seed_anchor(fx);

  auto       conn_before = open_db(fx);
  auto const dump_before = dump_planning_inventory(conn_before);

  auto const res = dispatch(fx, {"spec", "ingest", anchor, "--json"});
  CHECK(res.code == 0);
  constexpr std::string_view expected_json = R"json({
  "anchor_plan_id": 1,
  "assoc_slug": "project:proj",
  "anchor_slug": "ingest-fixture",
  "entities": [
    {"op": "add", "kind": "plan", "title": "M1", "scope": "assoc:project:proj", "derives_from": "plan:1"},
    {"op": "add", "kind": "task", "title": "Implement the thing", "scope": "assoc:project:proj", "derives_from": "plan:M1"},
    {"op": "add", "kind": "decision", "title": "Use SQLite", "scope": "assoc:project:proj", "derives_from": "plan:1"}
  ],
  "summary": {
    "additions": 3,
    "updates": 0,
    "removals": 0
  },
  "coverage": {
    "total_tasks": 1,
    "tasks_with_slug": 1,
    "tasks_without_slug": 0,
    "uncovered_task_slugs": ["do-the-thing"],
    "orphan_scenarios": []
  },
  "slug_collisions": []
}
)json";
  CHECK(res.out == expected_json);

  auto       conn_after = open_db(fx);
  auto const dump_after = dump_planning_inventory(conn_after);
  CHECK(dump_before == dump_after);
}

// ===========================================================================
// --apply writes the proposed graph
// ===========================================================================

TEST_CASE("spec ingest --apply creates the child plan and task, and flips the anchor active", "[cmd][spec][ingest][apply]") {
  auto const fx     = make_fixture("apply");
  auto const anchor = seed_anchor(fx);

  auto conn0 = open_db(fx);
  CHECK(count(conn0, "tasks") == 0);

  auto const res = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  CHECK(res.code == 0);
  CHECK(res.err.contains("applied"));

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 1);
  // One child plan under the anchor (the "M1" milestone).
  auto stmt = conn.prepare("select count(*) from plans where parent_plan_id = 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 1);

  auto slug_stmt = conn.prepare("select count(*) from tasks where slug = 'do-the-thing'");
  REQUIRE(slug_stmt.has_value());
  REQUIRE(slug_stmt->step().has_value());
  CHECK(slug_stmt->column_int64(0) == 1);

  // The anchor flipped draft -> active on first apply.
  auto status_stmt = conn.prepare("select status from plans where id = 1");
  REQUIRE(status_stmt.has_value());
  REQUIRE(status_stmt->step().has_value());
  CHECK(status_stmt->column_text(0) == "active");

  // The decision from the tech-spec's `## Decisions` section landed too.
  CHECK(count(conn, "decisions") == 1);
}

TEST_CASE("spec ingest --apply creates task bodies with no Required validation section",
          "[cmd][spec][ingest][apply][validation]") {
  auto const fx     = make_fixture("apply_no_validation");
  auto const anchor = seed_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  auto conn = open_db(fx);
  auto stmt = conn.prepare("select body from tasks where slug = 'do-the-thing'");
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  // The body is the acceptance criterion alone: no validation heading.
  CHECK(stmt->column_text(0) == "## Acceptance Criteria\n\n- Implement the thing\n");
  CHECK(stmt->column_text(0).find("## Required validation") == std::string::npos);
  // Dropping the section must not drop the drafted scenario: a single-criterion
  // task with no touches still gets its auto-drafted Verify scenario.
  CHECK(count(conn,
              "test_scenarios where body like 'Acceptance scenario auto-drafted by the ingestor.%' and status != 'retired'") ==
        1);
}

TEST_CASE("spec ingest re-ingest of a plan written before the validation section was dropped proposes nothing",
          "[cmd][spec][ingest][reingest]") {
  auto const fx     = make_fixture("reingest_old_shape");
  auto const anchor = seed_anchor(fx);
  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  // Seed the stored body in the exact shape the previous build generated:
  // the acceptance criterion plus the appended validation section.
  {
    auto conn = open_db(fx);
    REQUIRE(conn.execute("update tasks set body = '## Acceptance Criteria\n\n- Implement the thing\n\n"
                         "## Required validation\n\n- Run the authored test-spec scenarios covering this task.\n' "
                         "where slug = 'do-the-thing'"));
  }

  auto const preview = dispatch(fx, {"spec", "ingest", anchor});
  CHECK(preview.code == 0);
  CHECK(preview.out.contains("0 additions, 0 updates, 0 proposed removals.\n"));

  // An apply of the same roadmap must not rewrite the stored body either.
  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  auto conn = open_db(fx);
  auto stmt = conn.prepare("select body from tasks where slug = 'do-the-thing'");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_text(0) == "## Acceptance Criteria\n\n- Implement the thing\n\n"
                                "## Required validation\n\n- Run the authored test-spec scenarios covering this task.\n");
}

TEST_CASE("spec ingest --apply twice does not duplicate rows (idempotency)", "[cmd][spec][ingest][apply][idempotent]") {
  auto const fx     = make_fixture("apply_idem");
  auto const anchor = seed_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  std::string  inventory_after_first;
  std::int64_t tasks_after_first     = 0;
  std::int64_t plans_after_first     = 0;
  std::int64_t decisions_after_first = 0;
  std::int64_t links_after_first     = 0;
  {
    auto conn_after_first = open_db(fx);
    inventory_after_first = dump_planning_inventory(conn_after_first);
    tasks_after_first     = count(conn_after_first, "tasks");
    plans_after_first     = count(conn_after_first, "plans");
    decisions_after_first = count(conn_after_first, "decisions");
    links_after_first     = count(conn_after_first, "entity_links");
    // Ingest stages no routing fact: the table stays empty through the first apply.
    CHECK(count(conn_after_first, "routing_task_facts") == 0);
  }

  auto const second = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  CHECK(second.code == 0);

  auto conn_after_second = open_db(fx);
  // A byte-identical planning inventory pins every persisted identity and
  // edge, not only a convenient count subset. The explicit counts below make
  // the operator-facing graph dimensions obvious when this test fails.
  CHECK(dump_planning_inventory(conn_after_second) == inventory_after_first);
  CHECK(count(conn_after_second, "tasks") == tasks_after_first);
  CHECK(count(conn_after_second, "plans") == plans_after_first);
  CHECK(count(conn_after_second, "decisions") == decisions_after_first);
  CHECK(count(conn_after_second, "entity_links") == links_after_first);
  CHECK(count(conn_after_second, "routing_task_facts") == 0);
}

TEST_CASE("spec ingest --apply replays cleanly when a task hangs directly off the anchor plan",
          "[cmd][spec][ingest][apply][reconcile][regression]") {
  // REGRESSION. A materializer used to stage facts for a wider set of tasks
  // than it deleted facts for, so a task attached directly to the anchor plan
  // collided with its own rows on every replay and the whole apply rolled back
  // as `QueryFailed`. No fact is staged any more; the anchor-level task is
  // linked to the reviewed artifacts through `cites` links, and a replay must
  // still succeed and add nothing.
  auto const fx     = make_fixture("anchor_level_task_replay");
  auto const anchor = seed_reviewed_reconcile_anchor(fx);

  REQUIRE(dispatch(fx, {"task", "add", "Anchor-level task", "--plan", "1", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  std::int64_t anchor_task_id = 0;
  {
    auto conn = open_db(fx);
    auto stmt = conn.prepare("select id from tasks where plan_id = 1 order by id");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    anchor_task_id = stmt->column_int64(0);
    // The first apply really did link the anchor-level task to the four
    // reviewed artifacts, and wrote no fact; without these links the replay
    // below would pass for the wrong reason.
    CHECK(count(conn, std::format("entity_links where from_kind='task' and from_id={} and to_kind='artifact' and "
                                  "relationship='cites'",
                                  anchor_task_id)) == 4);
    CHECK(count(conn, "routing_task_facts") == 0);
  }

  auto const replay = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  INFO(replay.err);
  CHECK(replay.code == 0);
  CHECK_FALSE(replay.err.contains("QueryFailed"));

  auto after = open_db(fx);
  // Replacement, not accumulation: the anchor-level task keeps exactly its four links.
  CHECK(count(after, std::format("entity_links where from_kind='task' and from_id={} and to_kind='artifact' and "
                                 "relationship='cites'",
                                 anchor_task_id)) == 4);
  CHECK(count(after, "routing_task_facts") == 0);
}

TEST_CASE("spec ingest re-apply reconciles reviewed routing evidence and authored coverage", "[cmd][spec][ingest][reconcile]") {
  auto const fx     = make_fixture("reviewed_reconcile_full");
  auto const anchor = seed_reviewed_reconcile_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "accept", "1", "--json"}).code == 0);
  auto before = open_db(fx);
  CHECK(count(before,
              "test_scenarios where body like 'Acceptance scenario auto-drafted by the ingestor.%' and status != 'retired'") ==
        1);
  REQUIRE(before.execute("update plans set status='draft' where parent_plan_id=1"));
  REQUIRE(before.execute("update tasks set body='## Acceptance Criteria\n\n- Deliver the reviewed task\n\n## Repository "
                         "Scope\n\n- touches: planar\n' where id=1"));
  REQUIRE(dispatch(fx, {"artifact", "update", "4", "--body",
                        "## Scenarios\n\n### Scenario: reviewed root\n\n**Verifies:** task:reviewed-root\n\n**Acceptance:** The "
                        "reviewed root is dispatchable.\n",
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  // Replay restores generated routing state, links all four reviewed source
  // documents through the direct task->artifact `cites` path (and stages no
  // fact), and retires only the ingestor-owned duplicate scenario.
  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  auto after = open_db(fx);
  CHECK(count(after, "plans where parent_plan_id=1 and status='active'") == 1);
  CHECK(count(after, "entity_links where from_kind='task' and from_id=1 and to_kind='artifact' and relationship='cites'") == 4);
  // The citations arrive as `cites` links, and ingest writes no routing fact.
  CHECK(count(after, "routing_task_facts") == 0);
  CHECK(count(after,
              "entity_links where from_kind='task' and from_id=1 and to_kind='decision' and to_id=1 and relationship='cites'") ==
        1);
  CHECK(count(after,
              "test_scenarios where body like 'Acceptance scenario auto-drafted by the ingestor.%' and status='retired'") == 1);
  CHECK(count(after,
              "test_scenarios where body like '%**Acceptance:** The reviewed root is dispatchable.%' and status != 'retired'") ==
        1);

  auto packet = dispatch(fx, {"task", "packet", "1", "--json"});
  REQUIRE(packet.code == 0);
  auto packet_json = planar::json_dom::parse_json(packet.out);
  REQUIRE(packet_json.has_value());
  REQUIRE(packet_json->find("ready") != nullptr);
  CHECK(packet_json->find("ready")->boolean);
  auto const* input = packet_json->find("input");
  REQUIRE(input != nullptr);
  CHECK(input->find("citations") == nullptr);
  CHECK(input->find("facts") == nullptr);
  auto const* references = input->find("references");
  REQUIRE(references != nullptr);
  REQUIRE(references->kind == planar::json_dom::json_kind::array);
  CHECK(references->array.size() == 4);
  std::set<std::string> kinds;
  for (auto const& reference : references->array) {
    CHECK(reference.find("locator")->string == "body");
    CHECK(reference.find("ref")->string == std::format("artifact:{}", reference.find("id")->integer));
    CHECK(reference.find("digest")->string.size() == 64);
    CHECK_FALSE(reference.find("label")->string.empty());
    kinds.insert(reference.find("kind")->string);
  }
  CHECK(kinds == std::set<std::string>{"product_spec", "tech_spec", "roadmap", "test_spec"});

  auto next = dispatch(fx, {"plan", "next", "1", "--json"});
  REQUIRE(next.code == 0);
  auto next_json = planar::json_dom::parse_json(next.out);
  REQUIRE(next_json.has_value());
  REQUIRE(next_json->find("available") != nullptr);
  CHECK(next_json->find("available")->array.size() == 1);
  CHECK(next_json->find("blocked")->array.empty());

  auto strategy = dispatch(fx, {"plan", "recommend-strategy", "1", "--json"});
  REQUIRE(strategy.code == 0);
  auto strategy_json = planar::json_dom::parse_json(strategy.out);
  REQUIRE(strategy_json.has_value());
  auto const* summary = strategy_json->find("summary");
  REQUIRE(summary != nullptr);
  CHECK(summary->find("open_tasks")->integer == 1);
  CHECK(summary->find("eligible")->integer == 1);
  CHECK(strategy_json->find("parallel_eligible")->array.size() == 1);
  CHECK(strategy_json->find("serialized")->array.empty());

  // Reconciliation is deliberately not repeated here. A citation is not a
  // readiness fact: dropping the roadmap citation leaves the task ready.
  REQUIRE(after.execute("delete from entity_links where from_kind='task' and from_id=1 and to_kind='artifact' and "
                        "relationship='cites' and to_id in (select id from artifacts where kind='roadmap')"));
  {
    auto still_ready = dispatch(fx, {"task", "packet", "1", "--json"});
    REQUIRE(still_ready.code == 0);
    CHECK(still_ready.out.contains(R"("ready":true)"));
    CHECK(still_ready.out.contains(R"("reasons":[])"));
  }
  // A coordination fact is: a dependency on unfinished work in another plan
  // leaves a task whose packet is not ready (`blocked_by_open_task`). Rule 1
  // sees only the plan's own tree, so the strategy view reaches rule 7. Both
  // dispatch views must now remove the task from availability.
  REQUIRE(after.execute("insert into plans (scope_kind, scope_id, title, slug, status) "
                        "values ('global', null, 'Other work', 'other-work', 'active')"));
  REQUIRE(after.execute("insert into tasks (scope_kind, scope_id, plan_id, title, status, priority, slug) "
                        "select 'global', null, id, 'Other task', 'todo', 100, 'other-task' from plans where slug='other-work'"));
  REQUIRE(after.execute("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                        "select 'task', 1, 'task', id, 'depends-on' from tasks where slug='other-task'"));
  {
    auto incomplete_packet = dispatch(fx, {"task", "packet", "1", "--json"});
    REQUIRE(incomplete_packet.code == 0);
    CHECK(incomplete_packet.out.contains(R"("ready":false)"));
    CHECK(incomplete_packet.out.contains(R"("blocked_by_open_task")"));
  }
  auto incomplete_next = dispatch(fx, {"plan", "next", "1", "--json"});
  REQUIRE(incomplete_next.code == 0);
  auto incomplete_next_json = planar::json_dom::parse_json(incomplete_next.out);
  REQUIRE(incomplete_next_json.has_value());
  CHECK(incomplete_next_json->find("available")->array.empty());
  CHECK(incomplete_next_json->find("blocked")->array.size() == 1);

  auto incomplete_strategy = dispatch(fx, {"plan", "recommend-strategy", "1", "--json"});
  REQUIRE(incomplete_strategy.code == 0);
  auto incomplete_strategy_json = planar::json_dom::parse_json(incomplete_strategy.out);
  REQUIRE(incomplete_strategy_json.has_value());
  auto const* incomplete_summary = incomplete_strategy_json->find("summary");
  REQUIRE(incomplete_summary != nullptr);
  CHECK(incomplete_summary->find("open_tasks")->integer == 1);
  CHECK(incomplete_summary->find("eligible")->integer == 0);
  CHECK(incomplete_strategy_json->find("parallel_eligible")->array.empty());
  auto const* serialized = incomplete_strategy_json->find("serialized");
  REQUIRE(serialized != nullptr);
  REQUIRE(serialized->array.size() == 1);
  auto const* excluded = serialized->array[0].find("excluded_by");
  REQUIRE(excluded != nullptr);
  REQUIRE(excluded->array.size() == 1);
  CHECK(excluded->array[0].find("rule")->integer == 7);
  CHECK(excluded->array[0].find("reason")->string == "excluded by rule 7: task packet is not ready");
}

TEST_CASE("spec ingest membership-authorized apply preserves the anchor repository scope on every descendant",
          "[cmd][spec][ingest][apply][scope][membership]") {
  auto const fx = make_fixture("membership_provenance");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  // The anchor belongs directly to the repository. The association scope
  // below authorizes the write through membership, but must not become the
  // provenance scope of the derived graph.
  REQUIRE(dispatch(fx, {"plan", "create", "Repository anchor", "--scope", "repo:proj", "--json"}).code == 0);
  REQUIRE(
      dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body",
                    "## Decisions\n\n### Repository decision\n\nbody\n\n## Open Questions\n\n### Repository question\n\nbody\n",
                    "--json"})
          .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Repository work [slug: repository-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Test Spec", "--kind", "test_spec", "--plan", "1", "--body",
                        "## Scenarios\n\n### Scenario: repository work\n\n**Verifies:** task:repository-work\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  // `project:proj` is broader authorization supplied by the association;
  // it is deliberately different from the anchor's `repo:proj` provenance.
  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply", "--scope", "project:proj"}).code == 0);

  auto conn    = open_db(fx);
  auto repo_id = conn.prepare("select scope_id from plans where id = 1 and scope_kind = 'repo'");
  REQUIRE(repo_id.has_value());
  REQUIRE(repo_id->step().has_value());
  auto const id = repo_id->column_int64(0);
  for (auto const table : {std::string_view{"plans"}, std::string_view{"tasks"}, std::string_view{"decisions"},
                           std::string_view{"test_scenarios"}, std::string_view{"questions"}}) {
    // Only `plans` contains the anchor itself. Each other table's id 1 is a
    // descendant and must be checked rather than accidentally excluded.
    auto const descendants = table == "plans" ? "id != 1" : "1 = 1";
    auto       total       = conn.prepare(std::format("select count(*) from {} where {}", table, descendants));
    REQUIRE(total.has_value());
    REQUIRE(total->step().has_value());
    CHECK(total->column_int64(0) == 1);

    auto wrong = conn.prepare(
        std::format("select count(*) from {} where {} and (scope_kind != 'repo' or scope_id != ?)", table, descendants));
    REQUIRE(wrong.has_value());
    REQUIRE(wrong->bind_int64(1, id));
    REQUIRE(wrong->step().has_value());
    CHECK(wrong->column_int64(0) == 0);
  }
}

TEST_CASE("spec ingest direct repository-scope apply remains authorized", "[cmd][spec][ingest][apply][scope][direct]") {
  auto const fx = make_fixture("direct_repo_provenance");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Direct repository anchor", "--scope", "repo:proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Direct repository work [slug: direct-repository-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply", "--scope", "repo:proj"}).code == 0);

  auto conn  = open_db(fx);
  auto wrong = conn.prepare("select count(*) from tasks where scope_kind != 'repo' or scope_id != "
                            "(select scope_id from plans where id = 1)");
  REQUIRE(wrong.has_value());
  REQUIRE(wrong->step().has_value());
  CHECK(wrong->column_int64(0) == 0);
}

TEST_CASE("spec ingest --apply rolls back every planning write after a mid-apply failure",
          "[cmd][spec][ingest][apply][atomicity]") {
  auto const fx     = make_fixture("apply_atomicity");
  auto const anchor = seed_anchor(fx);

  std::string dump_before;
  {
    auto conn = open_db(fx);
    // The roadmap's child plan/task are written before the tech-spec
    // decision. Force the later decision insert to fail, so a green result
    // proves rollback of ALREADY-ATTEMPTED earlier writes rather than merely
    // a failure before mutation began. This is failure injection only; all
    // fixture entities themselves were seeded through the CLI.
    REQUIRE(conn.execute("create trigger spec_ingest_fail_decision before insert on decisions "
                         "begin select raise(abort, 'forced spec-ingest failure'); end;"));
    dump_before = dump_planning_inventory(conn);
  }

  auto const result = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  CHECK(result.code == 1);
  CHECK(result.err.contains("apply failed: QueryFailed"));

  auto       conn_after = open_db(fx);
  auto const dump_after = dump_planning_inventory(conn_after);
  CHECK(dump_after == dump_before);
  CHECK(count(conn_after, "tasks") == 0);
  CHECK(count(conn_after, "decisions") == 0);
}

// ===========================================================================
// --strict
// ===========================================================================

// Task 6664, closing the other live member of the gate blind spot class task
// 6661 catalogued: `declare_spec`'s trailing `ingest->add_option("extra-plans")
// ->expected(0, -1)->group("")` (handlers/spec_ingest.cpp) is hidden from
// help AND the schema catalog by the empty group — `cliapp::walk.cppm`'s
// visibility rule prunes anything in it — so `make surface-check` cannot see
// whether the line exists at all. Wave 6's Probe C deleted it and the gate
// stayed clean at 312 points with all 3430 cases passing (`grep -rn
// extra-plans src/` finds only the handler and its own comments) while
// operator behaviour genuinely changed: extra positional plan names stopped
// reaching the handler and CLI11 rejected them itself instead.
//
//     with the option:     every plan name — first AND every extra — reaches
//                          spec_ingest and is looked up
//     without the option:  `error: ingest: The following arguments were not
//                          expected: <extras...>` (CLI11's own refusal)
//
// This case proves the FIRST behaviour by using plan names that do not
// exist: each one must produce its own `not found` line, which only happens
// if `cliapp::positional_strings(args, "extra-plans")` actually received
// them. A version of this case that used real plans would still pass if the
// extras were silently dropped, since `spec ingest <plan>` alone succeeds —
// using guaranteed-missing names makes every extra's arrival independently
// observable.
TEST_CASE("spec ingest accepts the trailing hidden `extra-plans` positional and reaches the handler with all of them",
          "[cmd][spec][ingest][extra-plans]") {
  auto const fx = make_fixture("extraplans");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);

  auto const res = dispatch(fx, {"spec", "ingest", "no-such-plan-1", "no-such-plan-2", "no-such-plan-3"});
  // A batch reports the aggregate as invalid input, whatever each plan's reason.
  CHECK(res.code == 2);
  CHECK(res.err == "plan 'no-such-plan-1' not found: NotFound\n"
                   "plan 'no-such-plan-2' not found: NotFound\n"
                   "plan 'no-such-plan-3' not found: NotFound\n"
                   "error: one or more plans failed to ingest\n");
}

TEST_CASE("spec ingest --strict reports an uncovered task slug as a warning and exits 0",
          "[cmd][spec][ingest][strict][coverage-report]") {
  auto const fx = make_fixture("strict");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Strict Fixture", "--json"}).code == 0);

  constexpr std::string_view roadmap_body = "## M1\n"
                                            "\n"
                                            "- Implement the thing [slug: needs-coverage]\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", std::string(roadmap_body),
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  // No scenario cites the slug. The scope policy is report-only, so the
  // strict preview exits 0 and names the gap as a warning, not an error.
  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(res.code == 0);
  CHECK(res.err == "warning: plan 1: 1 task(s) without a scenario: needs-coverage\n");
  CHECK(res.out.contains("1 uncovered: needs-coverage"));

  // Preview only: nothing was written.
  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
}

TEST_CASE("spec ingest --strict --json lists uncovered task slugs under coverage and exits 0",
          "[cmd][spec][ingest][strict][coverage-report]") {
  auto const fx = make_fixture("strict_json");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Strict JSON Fixture", "--json"}).code == 0);

  constexpr std::string_view roadmap_body = "## M1\n"
                                            "\n"
                                            "- Second thing [slug: beta-slug]\n"
                                            "- First thing [slug: alpha-slug]\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", std::string(roadmap_body),
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.err == "warning: plan 1: 2 task(s) without a scenario: alpha-slug, beta-slug\n");

  auto const parsed = planar::json_dom::parse_json(res.out);
  REQUIRE(parsed.has_value());
  auto const* coverage = parsed->find("coverage");
  REQUIRE(coverage != nullptr);
  auto const* uncovered = coverage->find("uncovered_task_slugs");
  REQUIRE(uncovered != nullptr);
  REQUIRE(uncovered->kind == planar::json_dom::json_kind::array);
  REQUIRE(uncovered->array.size() == 2);
  CHECK(uncovered->array[0].string == "alpha-slug");
  CHECK(uncovered->array[1].string == "beta-slug");
  auto const* orphans = coverage->find("orphan_scenarios");
  REQUIRE(orphans != nullptr);
  CHECK(orphans->array.empty());
}

TEST_CASE("spec ingest --strict reports an orphan scenario as a warning and exits 0",
          "[cmd][spec][ingest][strict][coverage-report]") {
  auto const fx = make_fixture("strict_orphan");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Strict orphan fixture", "--json"}).code == 0);

  constexpr std::string_view test_spec_body = "## Scenarios\n\n### Scenario: cites nothing\n\nNo task reference.\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", "## M1\n", "--json"}).code ==
          0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Test Spec", "--kind", "test_spec", "--plan", "1", "--body",
                        std::string(test_spec_body), "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  // No roadmap task is proposed, so the orphan is the only coverage gap. The
  // report-only policy exits 0 and names it as a warning.
  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(res.code == 0);
  CHECK(res.err == "warning: plan 1: 1 orphan scenario(s) cite no task: cites nothing\n");
  CHECK(res.out.contains("1 orphan scenarios: cites nothing"));

  auto conn = open_db(fx);
  CHECK(count(conn, "test_scenarios") == 0);
}

TEST_CASE("spec ingest --strict accepts a fully covered roadmap", "[cmd][spec][ingest][strict]") {
  auto const fx = make_fixture("strict_covered");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Strict covered fixture", "--json"}).code == 0);

  constexpr std::string_view roadmap_body   = "## M1\n\n- Covered work [slug: covered-work]\n";
  constexpr std::string_view test_spec_body = "## Scenarios\n\n### Scenario: covered work\n\n**Verifies:** task:covered-work\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", std::string(roadmap_body),
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Test Spec", "--kind", "test_spec", "--plan", "1", "--body",
                        std::string(test_spec_body), "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(res.code == 0);
  CHECK(res.err.empty());
  CHECK(res.out.contains("coverage: 1 tasks (1 with slug, 0 without)"));

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
  CHECK(count(conn, "test_scenarios") == 0);
}

// ===========================================================================
// --apply-removals requires --apply
// ===========================================================================

TEST_CASE("spec ingest --apply-removals without --apply is refused before touching the database",
          "[cmd][spec][ingest][apply-removals]") {
  auto const fx     = make_fixture("apply_removals_guard");
  auto const anchor = seed_anchor(fx);

  auto       conn_before = open_db(fx);
  auto const dump_before = dump_planning_inventory(conn_before);

  auto const res = dispatch(fx, {"spec", "ingest", anchor, "--apply-removals"});
  CHECK(res.code == 2);
  CHECK(res.err.contains("--apply-removals requires --apply"));

  auto       conn_after = open_db(fx);
  auto const dump_after = dump_planning_inventory(conn_after);
  CHECK(dump_before == dump_after);
}

// ===========================================================================
// slug collisions
// ===========================================================================

TEST_CASE("spec ingest --strict refuses on a global task-slug collision", "[cmd][spec][ingest][strict][slug-collision]") {
  auto const fx = make_fixture("slug_collision");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);

  // Anchor 1: ingested and applied first, so its task claims the slug
  // `shared-slug` in the GLOBAL `tasks.slug` namespace.
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor One", "--json"}).code == 0);
  constexpr std::string_view roadmap_one = "## M1\n\n- First claimant [slug: shared-slug]\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", "1", "--body", "", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body", std::string(roadmap_one),
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply"}).code == 0);

  // Anchor 2: proposes a DIFFERENT task that asks for the SAME slug.
  // `spec ingest 1 --apply` above ALSO created the "M1" child plan, so
  // "Anchor Two" does NOT land at id 2 -- look its id up by slug rather
  // than assuming a fixed sequence.
  REQUIRE(dispatch(fx, {"plan", "create", "Anchor Two", "--json"}).code == 0);
  auto const anchor2 = [&] {
    auto conn = open_db(fx);
    auto stmt = conn.prepare("select id from plans where slug = 'anchor-two'");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    return std::to_string(stmt->column_int64(0));
  }();

  constexpr std::string_view roadmap_two = "## M1\n\n- Second claimant [slug: shared-slug]\n";
  // Cover the proposed task so this fixture isolates the collision branch
  // of the strict predicate. Without it, disabling that clause still
  // refuses for the unrelated uncovered-task coverage gap.
  constexpr std::string_view test_spec_two =
      "## Scenarios\n\n### Scenario: shared slug is covered\n\n**Verifies:** task:shared-slug\n";
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech Spec", "--kind", "tech_spec", "--plan", anchor2, "--body", "", "--json"}).code ==
          0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", anchor2, "--body", std::string(roadmap_two),
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Test Spec", "--kind", "test_spec", "--plan", anchor2, "--body",
                        std::string(test_spec_two), "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", anchor2, "--json"}).code == 0);

  // A bare preview WARNS (not refuses) about the collision.
  auto const preview = dispatch(fx, {"spec", "ingest", anchor2});
  CHECK(preview.code == 0);
  CHECK(preview.err.contains("already exists"));
  CHECK(preview.err.contains("SlugConflict"));

  // --strict REFUSES on the same collision.
  auto const strict = dispatch(fx, {"spec", "ingest", anchor2, "--strict"});
  CHECK(strict.code == 2);
  CHECK(strict.err.contains("--strict refused"));
  CHECK(strict.err.contains("global slug collision"));

  // Nothing under anchor 2 was written by either preview run.
  auto conn = open_db(fx);
  auto stmt = conn.prepare(
      std::format("select count(*) from tasks where plan_id in (select id from plans where parent_plan_id = {})", anchor2));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}

// ===========================================================================
// the draft -> active flip is CONDITIONAL, not unconditional
// ===========================================================================

TEST_CASE("spec ingest --apply does not force a non-draft anchor back to active", "[cmd][spec][ingest][apply][status]") {
  auto const fx     = make_fixture("status_guard");
  auto const anchor = seed_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  // First apply flips draft -> active, as asserted elsewhere. Advance it
  // one step further so the anchor is no longer `draft`.
  REQUIRE(dispatch(fx, {"plan", "update", anchor, "--status", "paused", "--json"}).code == 0);

  // A second apply proposes nothing new (the graph already matches the
  // spec), but it still runs the whole apply pass, including the
  // draft-check. The anchor must stay `paused` -- the flip is guarded on
  // `current_status == draft`, not run unconditionally.
  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  auto conn = open_db(fx);
  auto stmt = conn.prepare(std::format("select status from plans where id = {}", anchor));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_text(0) == "paused");
}

// ===========================================================================
// the work list is the only required input (task 7480)
// ===========================================================================

TEST_CASE("spec ingest with only a roadmap artifact previews and applies cleanly", "[cmd][spec][ingest][work-list]") {
  auto const fx = make_fixture("roadmap_only");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Roadmap only", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Ship the only work [slug: ship-only-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const preview = dispatch(fx, {"spec", "ingest", "1"});
  CHECK(preview.code == 0);
  CHECK_FALSE(preview.err.contains("no work list"));

  auto const applied = dispatch(fx, {"spec", "ingest", "1", "--apply"});
  REQUIRE(applied.code == 0);
  CHECK(applied.err.contains("applied"));

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 1);
  auto slug = conn.prepare("select count(*) from tasks where slug = 'ship-only-work'");
  REQUIRE(slug.has_value());
  REQUIRE(slug->step().has_value());
  CHECK(slug->column_int64(0) == 1);
}

TEST_CASE("spec ingest reads decisions from an artifact of any kind, not only the tech spec",
          "[cmd][spec][ingest][work-list][kinds]") {
  auto const fx = make_fixture("any_kind");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Any kind", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Ship it [slug: ship-it]\n", "--json"})
              .code == 0);
  // A design note carries the decision; no tech_spec exists on this anchor.
  REQUIRE(dispatch(fx, {"artifact", "add", "Notes", "--kind", "design_note", "--plan", "1", "--body",
                        "## Decisions\n\n### Keep the roadmap flat\n\nWe keep it flat.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply"}).code == 0);

  auto conn = open_db(fx);
  CHECK(count(conn, "decisions") == 1);
  auto title = conn.prepare("select count(*) from decisions where title = 'Keep the roadmap flat'");
  REQUIRE(title.has_value());
  REQUIRE(title->step().has_value());
  CHECK(title->column_int64(0) == 1);
}

TEST_CASE("spec ingest with no work list fails, names the missing input and writes nothing", "[cmd][spec][ingest][work-list]") {
  auto const fx = make_fixture("no_work_list");
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "create", "project:proj", "--kind", "project", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"assoc", "add", "project:proj", (fx.root / "proj").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "No work list", "--json"}).code == 0);
  // Decisions alone are not a work list.
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech", "--kind", "tech_spec", "--plan", "1", "--body",
                        "## Decisions\n\n### Use SQLite\n\nWe chose SQLite.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const res = dispatch(fx, {"spec", "ingest", "1", "--apply"});
  CHECK(res.code == 2);
  CHECK(res.err.contains("no work list"));
  CHECK(res.err.contains("roadmap artifact"));

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
  CHECK(count(conn, "decisions") == 0);
  auto status = conn.prepare("select status from plans where id = 1");
  REQUIRE(status.has_value());
  REQUIRE(status->step().has_value());
  CHECK(status->column_text(0) == "draft");
}

TEST_CASE("spec ingest re-ingest of a four-document plan is zero-diff", "[cmd][spec][ingest][work-list][regression]") {
  auto const fx     = make_fixture("zero_diff");
  auto const anchor = seed_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  std::string before;
  {
    auto conn = open_db(fx);
    before    = dump_planning_inventory(conn);
  }
  REQUIRE_FALSE(before.empty());

  // A second pass over the already-ingested plan must write no planning row.
  auto const again = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  CHECK(again.code == 0);
  CHECK(again.err.contains("0 plans created, 0 tasks created, 0 tasks updated"));

  auto conn = open_db(fx);
  CHECK(dump_planning_inventory(conn) == before);
}

TEST_CASE("spec ingest fails when the linked roadmap has no file, even if another artifact parses as a work list",
          "[cmd][spec][ingest][work-list][unreadable]") {
  auto const fx = make_fixture("roadmap_missing");
  seed_plan(fx, "Roadmap missing");
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- The real work [slug: real-work]\n", "--json"})
              .code == 0);
  // This tech spec parses as a work list too; it must not stand in for the roadmap.
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech", "--kind", "tech_spec", "--plan", "1", "--body",
                        "## Decisions\n\n### Use SQLite\n\nWe chose it.\n\n## M9\n\n- Not the work list [slug: impostor]\n",
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  auto const removed = remove_workbench_file(fx, 1);

  for (auto const& args :
       {std::vector<std::string>{"spec", "ingest", "1"}, std::vector<std::string>{"spec", "ingest", "1", "--apply"}}) {
    auto const res = dispatch(fx, args);
    CHECK(res.code == 1);
    CHECK(res.err.contains("reading roadmap artifact"));
    CHECK(res.err.contains(removed));
  }

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
  CHECK(count(conn, "decisions") == 0);
  CHECK(scalar(conn, "select count(*) from plans where status = 'draft'") == 1);
}

TEST_CASE("spec ingest --apply fails on an unreadable non-roadmap artifact and preview only warns",
          "[cmd][spec][ingest][work-list][unreadable]") {
  auto const fx = make_fixture("tech_missing");
  seed_plan(fx, "Tech missing");
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- The real work [slug: real-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech", "--kind", "tech_spec", "--plan", "1", "--body",
                        "## Decisions\n\n### Use SQLite\n\nWe chose it.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  auto const removed = remove_workbench_file(fx, 2);

  auto const preview = dispatch(fx, {"spec", "ingest", "1"});
  CHECK(preview.code == 0);
  CHECK(preview.err.contains("warning:"));
  CHECK(preview.err.contains(removed));

  auto const applied = dispatch(fx, {"spec", "ingest", "1", "--apply"});
  CHECK(applied.code == 1);
  CHECK(applied.err.contains(removed));

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
  CHECK(count(conn, "decisions") == 0);
}

TEST_CASE("spec ingest with no roadmap kind uses the single artifact that parses as a work list and cites it",
          "[cmd][spec][ingest][work-list][cites]") {
  auto const fx = make_fixture("design_note_work_list");
  seed_plan(fx, "Design note work list");
  REQUIRE(dispatch(fx, {"artifact", "add", "Plan notes", "--kind", "design_note", "--plan", "1", "--body",
                        "## M1\n\n- Do the noted work [slug: noted-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tech", "--kind", "tech_spec", "--plan", "1", "--body",
                        "## Decisions\n\n### Use SQLite\n\nWe chose it.\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply"}).code == 0);

  auto conn = open_db(fx);
  REQUIRE(scalar(conn, "select count(*) from tasks where slug = 'noted-work'") == 1);
  auto const task_id = scalar(conn, "select id from tasks where slug = 'noted-work'");
  CHECK(scalar(conn, std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                 "and to_kind = 'artifact' and to_id = 1 and relationship = 'cites'",
                                 task_id)) == 1);
  // The tech spec is one of the four spec kinds, so it is still cited too.
  CHECK(scalar(conn, std::format("select count(*) from entity_links where from_kind = 'task' and from_id = {} "
                                 "and to_kind = 'artifact' and to_id = 2 and relationship = 'cites'",
                                 task_id)) == 1);

  auto const packet = dispatch(fx, {"task", "packet", std::to_string(task_id), "--json"});
  REQUIRE(packet.code == 0);
  CHECK(packet.out.contains("\"ref\":\"artifact:1\""));
  CHECK(packet.out.contains("\"ref\":\"artifact:2\""));
}

TEST_CASE("spec ingest --strict refuses a scenario that verifies an unknown task slug", "[cmd][spec][ingest][strict][refs]") {
  auto const fx = make_fixture("strict_unknown_verifies");
  seed_plan(fx, "Strict refs");
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- The real work [slug: real-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tests", "--kind", "test_spec", "--plan", "1", "--body",
                        "## Scenarios\n\n### Scenario: typo\n\n**Verifies:** task:no-such-slug\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const strict = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(strict.code == 2);
  CHECK(strict.err.contains("--strict refused"));
  CHECK(strict.err.contains("no-such-slug"));
  // Without --strict the same preview still succeeds: the gate is the flag.
  CHECK(dispatch(fx, {"spec", "ingest", "1"}).code == 0);

  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
  CHECK(count(conn, "test_scenarios") == 0);
}

TEST_CASE("spec ingest --strict refuses an unresolved depends slug and accepts resolvable references",
          "[cmd][spec][ingest][strict][refs]") {
  auto const fx = make_fixture("strict_depends");
  seed_plan(fx, "Strict depends");
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- First [slug: first-task]\n- Second [slug: second-task] [depends: ghost-task]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  auto const refused = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(refused.code == 2);
  CHECK(refused.err.contains("ghost-task"));

  // A reference to a slug the roadmap declares resolves, and a scenario
  // verifying it is fine; one that cites nothing stays a warning.
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--body",
                        "## M1\n\n- First [slug: first-task]\n- Second [slug: second-task] [depends: first-task]\n", "--json"})
              .code == 0);
  REQUIRE(
      dispatch(fx,
               {"artifact", "add", "Tests", "--kind", "test_spec", "--plan", "1", "--body",
                "## Scenarios\n\n### Scenario: ok\n\n**Verifies:** task:first-task\n\n### Scenario: orphan\n\nCites nothing.\n",
                "--json"})
          .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  auto const accepted = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(accepted.code == 0);
  CHECK(accepted.err.contains("orphan"));
}

TEST_CASE("spec ingest exits 5 when the write scope does not cover the anchor, 1 for an unknown plan",
          "[cmd][spec][ingest][exit-codes]") {
  auto const fx = make_fixture("exit_codes");
  seed_plan(fx, "Scoped anchor");
  REQUIRE(dispatch(fx, {"assoc", "create", "project:other", "--kind", "project", "--json"}).code == 0);
  std::filesystem::create_directories(fx.root / "other");
  REQUIRE(dispatch(fx, {"assoc", "add", "project:other", (fx.root / "other").string(), "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Repo anchor", "--scope", "repo:proj", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "2", "--body",
                        "## M1\n\n- Scoped work [slug: scoped-work]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "2", "--json"}).code == 0);

  auto const refused = dispatch(fx, {"spec", "ingest", "2", "--apply", "--scope", "project:other"});
  CHECK(refused.code == 5);
  CHECK(refused.err.contains("Refusing cross-scope write"));
  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);

  CHECK(dispatch(fx, {"spec", "ingest", "no-such-plan"}).code == 1);
}

TEST_CASE("spec ingest --strict resolves a reference that only the database knows", "[cmd][spec][ingest][strict][refs]") {
  auto const fx = make_fixture("strict_db_only");
  seed_plan(fx, "Strict db refs");
  REQUIRE(dispatch(fx, {"artifact", "add", "Roadmap", "--kind", "roadmap", "--plan", "1", "--body",
                        "## M1\n\n- Earlier work [slug: earlier-task]\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"spec", "ingest", "1", "--apply"}).code == 0);

  // The roadmap no longer declares `earlier-task`, but the task still exists
  // in the anchor's tree, so a dependency and a scenario may name it.
  REQUIRE(dispatch(fx, {"artifact", "update", "1", "--body", "## M1\n\n- Later work [slug: later-task] [depends: earlier-task]\n",
                        "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"artifact", "add", "Tests", "--kind", "test_spec", "--plan", "1", "--body",
                        "## Scenarios\n\n### Scenario: earlier still holds\n\n**Verifies:** task:earlier-task\n", "--json"})
              .code == 0);
  REQUIRE(dispatch(fx, {"workbench", "push", "1", "--json"}).code == 0);

  auto const strict = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(strict.code == 0);
  CHECK_FALSE(strict.err.contains("--strict refused"));
}
