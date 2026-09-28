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

TEST_CASE("spec ingest --apply twice does not duplicate rows (idempotency)", "[cmd][spec][ingest][apply][idempotent]") {
  auto const fx     = make_fixture("apply_idem");
  auto const anchor = seed_anchor(fx);

  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);

  std::string  inventory_after_first;
  std::int64_t tasks_after_first     = 0;
  std::int64_t plans_after_first     = 0;
  std::int64_t decisions_after_first = 0;
  std::int64_t links_after_first     = 0;
  std::int64_t facts_after_first     = 0;
  {
    auto conn_after_first = open_db(fx);
    inventory_after_first = dump_planning_inventory(conn_after_first);
    tasks_after_first     = count(conn_after_first, "tasks");
    plans_after_first     = count(conn_after_first, "plans");
    decisions_after_first = count(conn_after_first, "decisions");
    links_after_first     = count(conn_after_first, "entity_links");
    facts_after_first     = count(conn_after_first, "routing_task_facts");
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
  // `routing_task_facts` is REPLACED wholesale by `materialize::reconcile`
  // on every apply, not appended to; a stable count is the idempotency
  // signature there, same as everywhere else.
  CHECK(count(conn_after_second, "routing_task_facts") == facts_after_first);
}

TEST_CASE("spec ingest --apply replays cleanly when a task hangs directly off the anchor plan",
          "[cmd][spec][ingest][apply][reconcile][regression]") {
  // REGRESSION. `materialize::reconcile` staged facts for a wider set of tasks
  // than it deleted facts for. Most stagers are scoped to
  // `plans.parent_plan_id = <anchor>`, but `stage_reviewed_artifact_facts`
  // walks the RECURSIVE plan tree, whose root is the anchor plan itself -- so a
  // task attached directly to the anchor got rows staged that the delete never
  // cleared. The first apply still worked (nothing stored yet to collide with),
  // and every later apply hit `routing_task_facts`'s unique key
  // (task_id, fact_kind, source kind/id, locator, digest, materializer_version)
  // -- which excludes the value columns, so a byte-identical replay collides
  // with itself. The whole apply then rolled back as `QueryFailed`, and because
  // apply is the only trigger for the routing materializer, every task under
  // the anchor stayed permanently `stale_fact` and undispatchable.
  //
  // The same mismatch also made `fact_sets_equal` compare a staged set against
  // a narrower stored set, so the "nothing changed, write nothing" short-circuit
  // could never fire for such an anchor.
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
    // The first apply really did write for the anchor-level task; without
    // these rows the replay below would pass for the wrong reason.
    CHECK(count(conn, std::format("routing_task_facts where task_id = {}", anchor_task_id)) > 0);
  }

  auto const replay = dispatch(fx, {"spec", "ingest", anchor, "--apply"});
  INFO(replay.err);
  CHECK(replay.code == 0);
  CHECK_FALSE(replay.err.contains("QueryFailed"));

  auto after = open_db(fx);
  // Replacement, not accumulation: the anchor-level task keeps exactly one row
  // per fact identity.
  CHECK(count(after, std::format("routing_task_facts where task_id = {}", anchor_task_id)) ==
        count(after, std::format("(select distinct task_id, fact_kind, source_entity_kind, source_entity_id, source_locator, "
                                 "source_digest, materializer_version from routing_task_facts where task_id = {})",
                                 anchor_task_id)));
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

  // Replay restores generated routing state, stages all four reviewed source
  // documents through the direct task->artifact citation path, and retires
  // only the ingestor-owned duplicate scenario.
  REQUIRE(dispatch(fx, {"spec", "ingest", anchor, "--apply"}).code == 0);
  auto after = open_db(fx);
  CHECK(count(after, "plans where parent_plan_id=1 and status='active'") == 1);
  CHECK(count(after, "entity_links where from_kind='task' and from_id=1 and to_kind='artifact' and relationship='cites'") == 4);
  CHECK(count(after, "routing_task_facts where task_id=1 and fact_kind='cited_artifact_section' and "
                     "source_entity_kind='artifact' and source_locator='body'") == 4);
  CHECK(count(after,
              "routing_task_facts where task_id=1 and fact_kind='validation_gate' and source_entity_kind='test_scenario'") == 1);
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
  auto const* citations = input->find("citations");
  REQUIRE(citations != nullptr);
  REQUIRE(citations->kind == planar::json_dom::json_kind::array);
  CHECK(citations->array.size() == 4);
  for (auto const& citation : citations->array) {
    CHECK(citation.find("freshness")->string == "current");
    CHECK(citation.find("locator")->string == "body");
  }

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

  // Reconciliation is deliberately not repeated here. Removing one cited
  // evidence row leaves a materialized but incomplete routing packet; both
  // dispatch views must now remove the task from availability.
  REQUIRE(after.execute("delete from routing_task_facts where id = (select id from routing_task_facts where task_id=1 "
                        "and fact_kind='cited_artifact_section' limit 1)"));
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
  CHECK(excluded->array[0].find("reason")->string == "excluded by rule 7: routing packet is not ready");
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
  CHECK(result.code != 0);
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
  CHECK(res.code != 0);
  CHECK(res.err == "plan 'no-such-plan-1' not found: NotFound\n"
                   "plan 'no-such-plan-2' not found: NotFound\n"
                   "plan 'no-such-plan-3' not found: NotFound\n"
                   "error: one or more plans failed to ingest\n");
}

TEST_CASE("spec ingest --strict refuses on an uncovered task slug", "[cmd][spec][ingest][strict]") {
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

  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(res.code != 0);
  CHECK(res.err == "plan 1: --strict refused: 1 uncovered task slug(s): needs-coverage\n"
                   "error: one or more plans failed to ingest\n");

  // Nothing was written; --strict refuses BEFORE apply is ever reached
  // (and no --apply flag was passed here regardless).
  auto conn = open_db(fx);
  CHECK(count(conn, "tasks") == 0);
}

TEST_CASE("spec ingest --strict refuses on an orphan scenario", "[cmd][spec][ingest][strict]") {
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

  // No roadmap task is proposed, so the orphan is the sole strict predicate
  // that can refuse this run.
  auto const res = dispatch(fx, {"spec", "ingest", "1", "--strict"});
  CHECK(res.code != 0);
  CHECK(res.err == "plan 1: --strict refused: 1 orphan scenario(s): cites nothing\n"
                   "error: one or more plans failed to ingest\n");

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
  CHECK(res.code != 0);
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
  CHECK(strict.code != 0);
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
