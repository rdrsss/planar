// @file sync.t.cpp
// @brief Tests for `planar.engine.external.sync` — the field-level conflict
// engine (plan 996, task 6041).
//
// HOME SAFETY. Every database comes from `scratch_db.hpp`, whose path can
// only be under `std::filesystem::temp_directory_path()`. Nothing here reads
// PLANAR_DB / PLANAR_HOME / HOME. This matters more here than in most files:
// `apply_all` MIGRATES whatever it is handed, and this branch carries a
// schema no installed binary supports.
//
// NETWORK SAFETY. Not a socket in the file. Every case drives
// `stub_adapter`, an in-process `planar::adapter::external_adapter` that
// returns canned values — which is possible at all because the sync engine
// takes the LAYER-1 interface rather than a concrete Jira/GitHub adapter (see
// engine/external/CMakeLists.txt). The real HTTP round trip is proven
// elsewhere, in `src/lib/http/http.t.cpp` and `engine/extsync/jira.t.cpp`.
//
// ORACLE PROVENANCE — how the conflict rules below were derived.
//
// NOT by reading the resolver. Each divergence was SEEDED and then run
// through the real binary. Setup: a loopback Python fixture serving the Jira
// issue endpoint on 127.0.0.1:18041 whose returned summary/status/updated
// come from a file the harness rewrites between runs; a scratch database
// under a fully pinned environment (`cd <work> && env PLANAR_DB=... HOME=...
// <binary>` — the ordering parity_harness.hpp documents); a task linked
// two-way to `sync-jira:SYNC-1`. Then `sync pull` / `sync resolve` were run
// and the task row, the link row, `sync_events` and the fixture's request log
// were dumped after each.
//
// The captures each test cites, verbatim:
//
//   D1 first pull, remote == local, no baseline
//      stdout {"link_id":1,"outcome":"noop","fields_changed":[]}  exit 0
//      link   1|Baseline|todo|ok            events 1|pull|noop||
//
//   D2 remote-only change (Baseline/To Do -> Remote edit/In Progress)
//      stdout {"link_id":1,"outcome":"ok","fields_changed":["title","status"]}
//      link   1|Remote edit|doing|ok        task 1|Remote edit|doing
//
//   D3 local-only change (task title -> "Local edit"), remote unchanged
//      stdout {"link_id":1,"outcome":"noop","fields_changed":[]}
//      link   1|Remote edit|doing|ok        task 1|Local edit|doing
//      -- the BASELINE DID NOT MOVE. That is the whole rule.
//
//   D4 both changed (local "Local edit", remote "Remote edit 2"/Blocked)
//      stderr error: one or more sync conflicts; use 'sync resolve' to settle
//      exit   3
//      events 4|pull|conflict|["title"]|local and remote changed since the
//             last successful sync
//      link   1|Remote edit|doing|conflict  task 1|Local edit|doing
//      -- fields_changed is ["title"] ALONE even though status ALSO differed
//         remotely, and NEITHER field was applied.
//      context_json {"version":1,"token":"cab1bdfefb4c09d68dcf14f56a284ab41
//         30d56bfd1415e67108e71a1fab0ca11","observed_at":"2026-08-24T02:12:
//         10.812Z","local":{"title":"Local edit","status":"doing",
//         "updated_at":"2026-08-24T02:12:03.194Z","source":"planar entity"},
//         "remote":{"title":"Remote edit 2","status":"blocked","version":
//         "2026-07-13T15:00:00Z","source":"external adapter pull"}}
//
//   D5 resolve refusals, each leaving task/link/events/PUT-count unchanged
//      no evidence flags        -> exit 2
//      wrong token              -> exit 3
//      wrong local updated_at   -> exit 3
//      stale event (a newer conflict exists)
//                               -> exit 3, stderr "sync event 14 is stale or
//                                  no longer the latest event for its link"
//
//   D6 resolve --keep local, correct evidence
//      stdout {"ok":true,"event_id":4,"new_event_id":5,"keep":"local"}
//      fixture log  GET /rest/api/3/issue/SYNC-1
//                   PUT /rest/api/3/issue/SYNC-1 {"fields":{"summary":"Local edit"}}
//      events 5|push|ok||resolved=local; from sync_event=4
//      link   1|Local edit|doing|ok
//
//   D7 resolve --keep remote, correct evidence
//      stdout {"ok":true,"event_id":6,"new_event_id":7,"keep":"remote"}
//      fixture log  GET /rest/api/3/issue/SYNC-1        <- and NO PUT
//      events 7|pull|ok||resolved=remote; from sync_event=6
//      link   1|Remote A|done|ok            task 1|Remote A|done
//
//   D9 unmappable remote status ("Backlog" -> empty)
//      stdout {"link_id":1,"outcome":"ok","fields_changed":["title"]}
//
//   D10 sync push
//      fixture log  PUT ... {"fields":{"summary":"Remote B"}}
//      events 9|push|ok|["title"]|          link 1|Remote B|done|ok
//
//   D11 direction gating
//      write-back + pull -> {"outcome":"noop"}, event pull|noop, nothing moved
//      read-only  + push -> exit 1
//      read-only  + pull -> {"outcome":"ok","fields_changed":["title","status"]}
//                           over a locally-edited task: NO conflict check runs
//
//   D13 adapter failure
//      bad external id  -> {"outcome":"error","detail":"InvalidExternalId"}, exit 0
//      dead port        -> {"outcome":"error","detail":"TransportFailed"},   exit 0
//      link 1|error, event <n>|pull|error|<tag>
//
// The evidence token in D4 was reproduced independently in Python from
// sha256("v1\0" + "1" + "\0Local edit\0doing\0" + ... ) and matched
// cab1bdfe…ca11 exactly, which is what pins the token formula rather than
// leaving it read off the Zig source. The digest itself now comes from
// `planar.sha256` (task 6407); this bucket's own copy is gone, and its
// padding-boundary vectors moved to `src/lib/sha256/sha256.t.cpp`.

import std;
import planar.db;
import planar.db.migrate;
import planar.adapter;
import planar.engine.external;

#include "scratch_db.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {

namespace link_ns   = planar::engine::external::link;
namespace sync_ns   = planar::engine::external::sync;
namespace system_ns = planar::engine::external::system;
using planar::engine::external::testing::err;
using planar::engine::external::testing::insert_task;
using planar::engine::external::testing::open_migrated;
using planar::engine::external::testing::read_task;
using planar::engine::external::testing::scratch_db_path;

/// @brief An in-process adapter that returns canned values and counts calls.
///
/// The equivalent of the loopback Python fixture the oracle runs were driven
/// against, minus the socket: it is the same substitution point, one layer
/// further in.
class stub_adapter final : public planar::adapter::external_adapter {
public:
  planar::adapter::remote_state                            remote;
  std::optional<planar::adapter::adapter_error>            pull_error;
  std::optional<planar::adapter::adapter_error>            push_error;
  std::vector<std::string>                                 push_applies{"title"};
  mutable std::size_t                                      pulls  = 0;
  mutable std::size_t                                      pushes = 0;
  mutable std::optional<planar::adapter::field_change_set> last_push;

  auto validate(std::string_view) const -> std::expected<void, planar::adapter::adapter_error> override {
    return {};
  }

  auto pull(std::string_view) const -> std::expected<planar::adapter::remote_state, planar::adapter::adapter_error> override {
    ++pulls;
    if (pull_error.has_value()) {
      return std::unexpected(*pull_error);
    }
    return remote;
  }

  auto push(std::string_view, const planar::adapter::field_change_set& fields) const
      -> std::expected<planar::adapter::update_outcome, planar::adapter::adapter_error> override {
    ++pushes;
    last_push = fields;
    if (push_error.has_value()) {
      return std::unexpected(*push_error);
    }
    return planar::adapter::update_outcome{.fields_applied = push_applies};
  }

  auto render(const planar::adapter::local_entity&, const planar::adapter::create_options&) const
      -> std::expected<std::string, planar::adapter::adapter_error> override {
    return std::string{"{}"};
  }
};

/// @brief One task linked two-way to one registered system.
struct rig {
  scratch_db_path        scratch;
  planar::db::connection conn;
  std::int64_t           task_id = 0;
  link_ns::ext_link      row;

  explicit rig(link_ns::sync_direction direction = link_ns::sync_direction::two_way, std::string_view task_title = "Baseline",
               std::string_view task_status = "todo")
      : conn(open_migrated(scratch)) {
    auto const registered = system_ns::register_jira(
        conn, {.slug = "sync-jira", .base_url = "http://127.0.0.1:18041", .project = "SYNC", .auth_env = "PLANAR_SYNC_TOKEN"});
    REQUIRE(registered.has_value());
    task_id      = insert_task(conn, task_title, task_status);
    auto created = link_ns::create(conn, {.entity_kind = link_ns::external_entity_kind::task,
                                          .entity_id   = task_id,
                                          .system_id   = registered->id,
                                          .external_id = "SYNC-1",
                                          .role        = link_ns::link_role::mirror,
                                          .direction   = direction});
    REQUIRE(created.has_value());
    row = *created;
  }

  /// @brief Re-read the link so `direction` / `last_sync_status` are current.
  auto reload() -> void {
    auto fresh = link_ns::show(conn, row.id);
    REQUIRE(fresh.has_value());
    row = *fresh;
  }

  /// @brief Set the task's title, bumping `updated_at` the way the CLI does.
  auto set_task(std::string_view title, std::string_view status) -> void {
    auto stmt = conn.prepare("update tasks set title = ?, status = ?, "
                             "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, title).has_value());
    REQUIRE(stmt->bind_text(2, status).has_value());
    REQUIRE(stmt->bind_int64(3, task_id).has_value());
    REQUIRE(stmt->step().has_value());
  }
};

/// @brief A remote state shaped like the fixture's Jira response.
auto remote_of(std::string_view title, std::string_view status, std::string_view version) -> planar::adapter::remote_state {
  return {.external_id = "SYNC-1", .title = std::string(title), .status = std::string(status), .version = std::string(version)};
}

/// @brief The `sync_events` rows for one link, oldest first, as
/// `(direction, outcome, fields_changed, detail)`.
auto event_tuples(planar::db::connection& conn, std::int64_t link_id)
    -> std::vector<std::tuple<std::string, std::string, std::string, std::string>> {
  auto const events = sync_ns::events_for_link(conn, link_id);
  REQUIRE(events.has_value());
  std::vector<std::tuple<std::string, std::string, std::string, std::string>> out;
  for (auto const& event : *events) {
    out.emplace_back(event.direction, event.event_outcome, event.fields_changed.value_or(std::string{}),
                     event.detail.value_or(std::string{}));
  }
  return out;
}

} // namespace

// --- The digest and the token -----------------------------------------------

TEST_CASE("evidence_token reproduces the token from the captured oracle conflict", "[engine][external][sync]") {
  // Capture D4. Every one of these seven inputs is read off that run's
  // context_json, and the expected digest is the token that run WROTE.
  CHECK(sync_ns::evidence_token(1, "Local edit", "doing", "2026-08-24T02:12:03.194Z", "Remote edit 2", "blocked",
                                "2026-07-13T15:00:00Z") == "cab1bdfefb4c09d68dcf14f56a284ab4130d56bfd1415e67108e71a1fab0ca11");
  // Any single field moving must move the token, or it is not a CAS key.
  CHECK(sync_ns::evidence_token(2, "Local edit", "doing", "2026-08-24T02:12:03.194Z", "Remote edit 2", "blocked",
                                "2026-07-13T15:00:00Z") != "cab1bdfefb4c09d68dcf14f56a284ab4130d56bfd1415e67108e71a1fab0ca11");
  // The NUL separators are load-bearing: without them these two distinct
  // conflicts would hash identically.
  CHECK(sync_ns::evidence_token(1, "ab", "c", "d", "e", "f", "g") != sync_ns::evidence_token(1, "a", "bc", "d", "e", "f", "g"));
}

// --- The conflict rule, one divergence per case -----------------------------

TEST_CASE("D1: the first pull on a fresh link is noop and records a baseline", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Baseline", "todo", "2026-07-13T13:00:00Z");

  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::noop);
  CHECK(result->fields_changed.empty());

  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  REQUIRE(base.has_value());
  CHECK(base->title == std::optional<std::string>{"Baseline"});
  CHECK(base->status == std::optional<std::string>{"todo"});
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::ok);
  CHECK(event_tuples(fixture.conn, fixture.row.id) ==
        decltype(event_tuples(fixture.conn, fixture.row.id)){{"pull", "noop", "", ""}});
}

TEST_CASE("D2: a remote-only change EMITS both fields and still moves the baseline; decision 996 does not apply the task",
          "[engine][external][sync]") {
  // D2 divergence (decision 996, plan 996 task 6419): the oracle writes the
  // remote's title/status onto the task. This binary instead reports them
  // on the result (`remote_title`/`remote_status`) and leaves the task
  // untouched — `planar-ext`'s write authorizer (decision 995) would
  // refuse the oracle's `update tasks …` anyway. The BASELINE (stored in
  // `external_links`, an allowed table) still advances to what the task
  // would now hold had it been applied — see `pull_link`'s header for why
  // that part is unchanged.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Baseline", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  provider.remote   = remote_of("Remote edit", "doing", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::ok);
  CHECK(result->fields_changed == std::vector<std::string>{"title", "status"});
  CHECK(result->remote_title == std::optional<std::string>{"Remote edit"});
  CHECK(result->remote_status == std::optional<std::string>{"doing"});

  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Baseline");
  CHECK(status == "todo");
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Remote edit"});
  CHECK(base->status == std::optional<std::string>{"doing"});
}

TEST_CASE("D3: a local-only change is noop AND the baseline does not move", "[engine][external][sync]") {
  // This is the single most important case in the file. If the baseline
  // absorbed the local edit here, the NEXT remote change would look like a
  // remote-only change and would silently overwrite the local one — the
  // conflict would never be detected at all.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Remote edit", "doing", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  fixture.set_task("Local edit", "doing");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::noop);
  CHECK(result->fields_changed.empty());

  // The local edit SURVIVES.
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Local edit");
  // The baseline still holds the last AGREED value, not the local one.
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Remote edit"});
}

TEST_CASE("D4: both sides changed is a conflict that applies NOTHING", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Remote edit", "doing", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.set_task("Local edit", "doing");

  // Title: remote moved, local moved, and they differ  -> conflict.
  // Status: remote moved (doing -> blocked), local did NOT -> no conflict.
  provider.remote   = remote_of("Remote edit 2", "blocked", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::conflict);
  // ["title"] ALONE — the per-field rule, exactly as captured.
  CHECK(result->fields_changed == std::vector<std::string>{"title"});
  CHECK(result->detail == "local and remote changed since the last successful sync");

  // Abort is whole-link: the non-conflicting status change is NOT applied.
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Local edit");
  CHECK(status == "doing");
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Remote edit"});
  CHECK(base->status == std::optional<std::string>{"doing"});
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::conflict);
}

TEST_CASE("D4b: both sides making the SAME edit is not a conflict", "[engine][external][sync]") {
  // Clause 3 of the rule (`remote != local`). Without it, two sides that
  // independently converged on the same value would deadlock forever.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Baseline", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  fixture.set_task("Agreed", "doing");
  provider.remote   = remote_of("Agreed", "doing", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::noop);
  // And the baseline DOES advance here, because both `allow_*` gates are
  // true (the remote genuinely changed) — so the convergence is recorded.
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Agreed"});
  CHECK(base->status == std::optional<std::string>{"doing"});
}

TEST_CASE("D4c: a status-only conflict reports status alone", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Same", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  fixture.set_task("Same", "doing");
  provider.remote   = remote_of("Same", "blocked", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::conflict);
  CHECK(result->fields_changed == std::vector<std::string>{"status"});
}

TEST_CASE("D4d: both fields in conflict report title then status, in that order", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Base", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  fixture.set_task("Local", "doing");
  provider.remote   = remote_of("Remote", "blocked", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->fields_changed == std::vector<std::string>{"title", "status"});
}

TEST_CASE("D9: an unmappable remote status is 'no opinion', not a status change", "[engine][external][sync]") {
  // Capture D9: the oracle's Jira adapter maps "Backlog" to the EMPTY string,
  // and the pull reported fields_changed:["title"] rather than clearing the
  // local status.
  //
  // The seeding pull's remote MATCHES the fixture's initial local values
  // ("Baseline"/"todo") deliberately, unlike the oracle capture. Since
  // decision 996 (task 6419) never applies a diff to the task, a seeding
  // pull whose remote genuinely differs from local would advance the
  // baseline to a value the task itself never reaches (nothing writes
  // it) — the SECOND pull below would then see local trailing that
  // phantom baseline and misreport a two-way conflict. A true noop seed
  // sidesteps that and isolates what this test actually checks: the
  // empty-status mapping.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Baseline", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  provider.remote   = remote_of("Remote B", "", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->fields_changed == std::vector<std::string>{"title"});
  CHECK(result->remote_title == std::optional<std::string>{"Remote B"});
  CHECK_FALSE(result->remote_status.has_value());
  // Decision 996 (task 6419): EMITTED, not applied — the task still holds
  // its ORIGINAL value; even the first pull's "Base" never got written.
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Baseline");
  CHECK(status == "todo");
}

TEST_CASE("an empty remote title is likewise never reported as changed", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote   = remote_of("", "doing", "v1");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->fields_changed == std::vector<std::string>{"status"});
  CHECK_FALSE(result->remote_title.has_value());
  CHECK(result->remote_status == std::optional<std::string>{"doing"});
  // Decision 996 (task 6419): the task is never written; only the status
  // differed, and even that was EMITTED above, not applied.
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Baseline");
  CHECK(status == "todo");
}

// --- Direction gating -------------------------------------------------------

TEST_CASE("D11a: a write-back link pulls as noop and touches nothing", "[engine][external][sync]") {
  rig          fixture(link_ns::sync_direction::write_back);
  stub_adapter provider;
  provider.remote = remote_of("Remote C", "doing", "v1");

  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::noop);
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Baseline");
  CHECK(status == "todo");
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK_FALSE(base->present());
  // The attempt is still RECORDED — the link's sync state moves to ok and one
  // event is written even though nothing was applied.
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::ok);
  CHECK(event_tuples(fixture.conn, fixture.row.id).size() == 1);
}

TEST_CASE("D11b: a read-only link refuses a push before anything is sent", "[engine][external][sync]") {
  rig          fixture(link_ns::sync_direction::read_only);
  stub_adapter provider;

  auto const result = sync_ns::push_link(fixture.conn, fixture.row, provider);
  REQUIRE_FALSE(result.has_value());
  CHECK(err(result) == std::optional{sync_ns::sync_error::read_only});
  CHECK(provider.pushes == 0);
  CHECK(event_tuples(fixture.conn, fixture.row.id).empty());
}

TEST_CASE("D11c: a read-only link diffs the remote against a locally-edited task with NO conflict check",
          "[engine][external][sync]") {
  // Captured: a read-only pull over a locally-edited task overwrote it,
  // fields_changed ["title","status"], no conflict. Conflict detection is
  // gated on `two-way` and this is the case that proves the gate is real —
  // unaffected by decision 996, which only changes whether the WRITE that
  // used to happen here still happens (it does not: the task is EMITTED,
  // not applied — see `pull_link`'s header).
  rig          fixture(link_ns::sync_direction::read_only);
  stub_adapter provider;
  provider.remote = remote_of("Remote", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  fixture.set_task("Local edit", "doing");
  provider.remote   = remote_of("Remote C", "todo", "v2");
  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::ok);
  CHECK(result->fields_changed == std::vector<std::string>{"title", "status"});
  CHECK(result->remote_title == std::optional<std::string>{"Remote C"});
  CHECK(result->remote_status == std::optional<std::string>{"todo"});
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Local edit");
  CHECK(status == "doing");
}

// --- Push -------------------------------------------------------------------

TEST_CASE("D10: push sends the local title and status and rebaselines to them", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  fixture.set_task("Pushed title", "doing");

  auto const result = sync_ns::push_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::ok);
  CHECK(result->fields_changed == std::vector<std::string>{"title"});
  CHECK(provider.pushes == 1);
  // A push does NOT read the remote first — that is what makes it
  // unconditional, and it is the difference from resolve --keep local.
  CHECK(provider.pulls == 0);
  REQUIRE(provider.last_push.has_value());
  CHECK(provider.last_push->title == std::optional<std::string>{"Pushed title"});
  CHECK(provider.last_push->status == std::optional<std::string>{"doing"});
  CHECK_FALSE(provider.last_push->assignee.has_value());

  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Pushed title"});
  CHECK(base->status == std::optional<std::string>{"doing"});
  CHECK(event_tuples(fixture.conn, fixture.row.id) ==
        decltype(event_tuples(fixture.conn, fixture.row.id)){{"push", "ok", R"(["title"])", ""}});
}

// --- Adapter failure --------------------------------------------------------

TEST_CASE("D13: an adapter failure is an outcome carrying the Zig error tag", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.pull_error = planar::adapter::adapter_error::transport_failed;

  auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  // A RESULT, not a sync_error — the captured run exited 0.
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::error);
  CHECK(result->detail == "TransportFailed");
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::error);
  CHECK(event_tuples(fixture.conn, fixture.row.id) ==
        decltype(event_tuples(fixture.conn, fixture.row.id)){{"pull", "error", "", "TransportFailed"}});
}

TEST_CASE("D13b: a push adapter failure records the tag under direction push", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.push_error = planar::adapter::adapter_error::invalid_external_id;

  auto const result = sync_ns::push_link(fixture.conn, fixture.row, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::error);
  CHECK(result->detail == "InvalidExternalId");
  CHECK(event_tuples(fixture.conn, fixture.row.id) ==
        decltype(event_tuples(fixture.conn, fixture.row.id)){{"push", "error", "", "InvalidExternalId"}});
}

// --- The evidence blob ------------------------------------------------------

TEST_CASE("the conflict evidence blob matches the captured oracle bytes in shape and order", "[engine][external][sync]") {
  // Capture D4's context_json. `observed_at` and `token` are the two values
  // that legitimately differ per run (a timestamp and a digest over it is
  // not — the token does not include observed_at), so `observed_at` is
  // matched structurally and everything else byte for byte.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Remote edit", "doing", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.set_task("Local edit", "doing");
  provider.remote = remote_of("Remote edit 2", "blocked", "2026-07-13T15:00:00Z");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  auto const events = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  REQUIRE(events.has_value());
  auto const& conflict = events->back();
  REQUIRE(conflict.context_json.has_value());
  auto const& blob = *conflict.context_json;

  auto const [ignored_title, ignored_status, updated_at] = read_task(fixture.conn, fixture.task_id);
  auto const token = sync_ns::evidence_token(fixture.row.id, "Local edit", "doing", updated_at, "Remote edit 2", "blocked",
                                             "2026-07-13T15:00:00Z");

  CHECK(blob.starts_with(std::format(R"({{"version":1,"token":"{}","observed_at":")", token)));
  CHECK(blob.ends_with(
      std::format(R"(","local":{{"title":"Local edit","status":"doing","updated_at":"{}","source":"planar entity"}},)"
                  R"("remote":{{"title":"Remote edit 2","status":"blocked","version":"2026-07-13T15:00:00Z",)"
                  R"("source":"external adapter pull"}}}})",
                  updated_at)));
}

TEST_CASE("evidence blob text is escaped through planar.json_text", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Base", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.set_task(R"(say "hi")", "doing");
  provider.remote = remote_of("other\ttitle", "blocked", "v2");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  auto const  events = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  auto const& blob   = *events->back().context_json;
  CHECK(blob.find(R"("title":"say \"hi\"")") != std::string::npos);
  CHECK(blob.find(R"("title":"other\ttitle")") != std::string::npos);
}

// --- resolve_conflict, one guard per case -----------------------------------

namespace {

/// @brief Seed a conflict and hand back everything a resolution needs.
struct seeded_conflict {
  std::int64_t event_id = 0;     ///< The conflict event.
  std::string  token;            ///< Its evidence token.
  std::string  local_updated_at; ///< The entity's `updated_at` at conflict time.
};

auto seed_conflict(rig& fixture, stub_adapter& provider) -> seeded_conflict {
  provider.remote = remote_of("Remote edit", "doing", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.set_task("Local edit", "doing");
  provider.remote   = remote_of("Remote edit 2", "blocked", "2026-07-13T15:00:00Z");
  auto const pulled = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(pulled.has_value());
  REQUIRE(pulled->result == sync_ns::outcome::conflict);
  fixture.reload();

  auto const events = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  REQUIRE(events.has_value());
  auto const [ignored_title, ignored_status, updated_at] = read_task(fixture.conn, fixture.task_id);
  return {.event_id = events->back().id,
          .token    = sync_ns::evidence_token(fixture.row.id, "Local edit", "doing", updated_at, "Remote edit 2", "blocked",
                                              "2026-07-13T15:00:00Z"),
          .local_updated_at = updated_at};
}

} // namespace

TEST_CASE("D6: resolve --keep local re-reads, pushes, and records a push event", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  auto const pulls_before  = provider.pulls;
  auto const pushes_before = provider.pushes;
  auto const resolved      = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, seeded.token,
                                                       seeded.local_updated_at, provider);
  REQUIRE(resolved.has_value());
  CHECK(resolved->ok);
  CHECK(resolved->event_id == seeded.event_id);
  CHECK(resolved->new_event_id > seeded.event_id);
  CHECK(resolved->keep == sync_ns::resolve_keep::local);
  // One fresh read (guard 4) and one push. Captured: GET then PUT.
  CHECK(provider.pulls == pulls_before + 1);
  CHECK(provider.pushes == pushes_before + 1);

  // The local entity is untouched; the baseline becomes the local values.
  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Local edit");
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Local edit"});

  auto const events = event_tuples(fixture.conn, fixture.row.id);
  CHECK(std::get<0>(events.back()) == "push");
  CHECK(std::get<1>(events.back()) == "ok");
  CHECK(std::get<3>(events.back()) == std::format("resolved=local; from sync_event={}", seeded.event_id));
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::ok);
}

TEST_CASE("D7: resolve --keep remote sends NOTHING and, per decision 996, no longer rewrites the entity",
          "[engine][external][sync]") {
  // D2-shaped divergence (decision 996, plan 996 task 6419): the oracle
  // writes the remote's title/status onto the task here. This binary
  // clears the conflict (`sync_status::ok`, baseline reset to the
  // CURRENT — unmodified — local values) without touching the task; an
  // agent applies the remote's values through `planar`, and the next
  // `sync pull` re-emits them as a plain, non-conflicting `ok` once it
  // does (see `resolve_conflict`'s header).
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  auto const pushes_before = provider.pushes;
  auto const resolved      = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::remote, seeded.token,
                                                       seeded.local_updated_at, provider);
  REQUIRE(resolved.has_value());
  // Zero PUTs. Captured: the fixture's request log held a GET and nothing else.
  CHECK(provider.pushes == pushes_before);

  auto const [title, status, _] = read_task(fixture.conn, fixture.task_id);
  CHECK(title == "Local edit");
  CHECK(status == "doing");
  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  CHECK(base->title == std::optional<std::string>{"Local edit"});
  CHECK(base->status == std::optional<std::string>{"doing"});
  fixture.reload();
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::ok);
  auto const events = event_tuples(fixture.conn, fixture.row.id);
  // Direction `pull`, not `push` — the resolution moved data the other way.
  CHECK(std::get<0>(events.back()) == "pull");
  CHECK(std::get<3>(events.back()) == std::format("resolved=remote; from sync_event={}", seeded.event_id));
}

TEST_CASE("D5: guard 2 — a wrong evidence token refuses and mutates nothing", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  auto const before_events = event_tuples(fixture.conn, fixture.row.id);
  auto const before_task   = read_task(fixture.conn, fixture.task_id);
  auto const pushes_before = provider.pushes;

  auto const refused = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, "deadbeef",
                                                 seeded.local_updated_at, provider);
  REQUIRE_FALSE(refused.has_value());
  CHECK(err(refused) == std::optional{sync_ns::sync_error::evidence_changed});
  CHECK(event_tuples(fixture.conn, fixture.row.id) == before_events);
  CHECK(read_task(fixture.conn, fixture.task_id) == before_task);
  CHECK(provider.pushes == pushes_before);
  // Refused BEFORE the fresh read — the cheapest guard runs first.
  CHECK(provider.pulls == 2);
}

TEST_CASE("D5: guard 3 — a moved local entity refuses and mutates nothing", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  auto const refused = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, seeded.token,
                                                 "2000-01-01T00:00:00.000Z", provider);
  REQUIRE_FALSE(refused.has_value());
  CHECK(err(refused) == std::optional{sync_ns::sync_error::evidence_changed});
  CHECK(provider.pushes == 0);
}

TEST_CASE("D5: guard 4 — a remote that moved since the evidence refuses", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  // Same field VALUES, new version. This is the case value-only comparison
  // misses entirely, and it is why the version is compared at all.
  provider.remote    = remote_of("Remote edit 2", "blocked", "2026-07-13T15:00:01Z");
  auto const refused = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, seeded.token,
                                                 seeded.local_updated_at, provider);
  REQUIRE_FALSE(refused.has_value());
  CHECK(err(refused) == std::optional{sync_ns::sync_error::evidence_changed});
  CHECK(provider.pushes == 0);
}

TEST_CASE("D5: guard 4 — a versionless provider can never authorize a resolution", "[engine][external][sync]") {
  // Even when every field value matches. A provider with no version marker
  // cannot prove it did not move and come back.
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Remote edit", "doing", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.set_task("Local edit", "doing");
  provider.remote = remote_of("Remote edit 2", "blocked", "");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());
  fixture.reload();

  auto const events                                      = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  auto const [ignored_title, ignored_status, updated_at] = read_task(fixture.conn, fixture.task_id);
  auto const token = sync_ns::evidence_token(fixture.row.id, "Local edit", "doing", updated_at, "Remote edit 2", "blocked", "");

  auto const refused =
      sync_ns::resolve_conflict(fixture.conn, events->back().id, sync_ns::resolve_keep::remote, token, updated_at, provider);
  REQUIRE_FALSE(refused.has_value());
  CHECK(err(refused) == std::optional{sync_ns::sync_error::evidence_changed});
}

TEST_CASE("D5: guard 1 — a stale conflict event refuses even with correct evidence", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   first = seed_conflict(fixture, provider);

  // A second pull records a NEWER conflict; the first is now stale.
  provider.remote   = remote_of("Remote edit 3", "blocked", "2026-07-13T16:00:00Z");
  auto const second = sync_ns::pull_link(fixture.conn, fixture.row, provider);
  REQUIRE(second.has_value());
  REQUIRE(second->result == sync_ns::outcome::conflict);

  auto const refused = sync_ns::resolve_conflict(fixture.conn, first.event_id, sync_ns::resolve_keep::local, first.token,
                                                 first.local_updated_at, provider);
  REQUIRE_FALSE(refused.has_value());
  CHECK(err(refused) == std::optional{sync_ns::sync_error::stale_conflict});
  CHECK(provider.pushes == 0);
}

TEST_CASE("resolving a non-conflict event is not_conflict", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Base", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  auto const events = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  REQUIRE(events->size() == 1);
  auto const refused =
      sync_ns::resolve_conflict(fixture.conn, events->front().id, sync_ns::resolve_keep::local, "t", "u", provider);
  REQUIRE_FALSE(refused.has_value());
  // A noop event has NULL context_json, so evidence loading refuses first —
  // and `evidence_changed` is the right answer for it: the event exists, but
  // nothing in it can authorize anything.
  CHECK(err(refused) == std::optional{sync_ns::sync_error::evidence_changed});
}

TEST_CASE("resolving after the conflict was already resolved is stale_conflict", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  auto const   seeded = seed_conflict(fixture, provider);

  REQUIRE(sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, seeded.token,
                                    seeded.local_updated_at, provider)
              .has_value());
  auto const again = sync_ns::resolve_conflict(fixture.conn, seeded.event_id, sync_ns::resolve_keep::local, seeded.token,
                                               seeded.local_updated_at, provider);
  REQUIRE_FALSE(again.has_value());
  CHECK(err(again) == std::optional{sync_ns::sync_error::stale_conflict});
}

// --- Link surfaces the sync engine depends on -------------------------------

TEST_CASE("pullable and pushable partition the three sync directions", "[engine][external][link]") {
  scratch_db_path const scratch;
  auto                  conn       = open_migrated(scratch);
  auto const            registered = system_ns::register_github(conn, {.slug = "gh", .project = "o/r"});
  REQUIRE(registered.has_value());
  auto const task_id = insert_task(conn, "T", "todo");

  auto const make = [&](std::string_view external_id, link_ns::sync_direction direction) {
    auto created = link_ns::create(conn, {.entity_kind = link_ns::external_entity_kind::task,
                                          .entity_id   = task_id,
                                          .system_id   = registered->id,
                                          .external_id = external_id,
                                          .direction   = direction});
    REQUIRE(created.has_value());
    return created->id;
  };
  auto const ro = make("o/r#1", link_ns::sync_direction::read_only);
  auto const wb = make("o/r#2", link_ns::sync_direction::write_back);
  auto const tw = make("o/r#3", link_ns::sync_direction::two_way);

  auto const ids = [](const std::vector<link_ns::ext_link>& rows) {
    std::vector<std::int64_t> out;
    for (auto const& row : rows) {
      out.push_back(row.id);
    }
    return out;
  };
  // two-way is in BOTH, and each other direction is in exactly one.
  CHECK(ids(*link_ns::all_pullable(conn)) == std::vector<std::int64_t>{ro, tw});
  CHECK(ids(*link_ns::all_pushable(conn)) == std::vector<std::int64_t>{wb, tw});
}

TEST_CASE("list filters conjunctively and binds in clause order", "[engine][external][link]") {
  // The bind indexes are positional, so a filter combination that exercises
  // MORE THAN ONE optional clause is the only thing that catches a
  // mis-ordered bind — a single-filter test passes either way.
  scratch_db_path const scratch;
  auto                  conn  = open_migrated(scratch);
  auto const            alpha = system_ns::register_github(conn, {.slug = "alpha", .project = "o/r"});
  auto const            beta  = system_ns::register_github(conn, {.slug = "beta", .project = "o/r"});
  REQUIRE(alpha.has_value());
  REQUIRE(beta.has_value());
  auto const task_a = insert_task(conn, "A", "todo");
  auto const task_b = insert_task(conn, "B", "todo");

  auto const make = [&](std::int64_t system_id, std::int64_t entity_id, std::string_view external_id) {
    auto created = link_ns::create(conn, {.entity_kind = link_ns::external_entity_kind::task,
                                          .entity_id   = entity_id,
                                          .system_id   = system_id,
                                          .external_id = external_id});
    REQUIRE(created.has_value());
    return created->id;
  };
  auto const wanted = make(alpha->id, task_a, "o/r#1");
  make(alpha->id, task_b, "o/r#2");
  make(beta->id, task_a, "o/r#3");

  auto const filtered =
      link_ns::list(conn, {.entity_kind = link_ns::external_entity_kind::task, .entity_id = task_a, .system_slug = "alpha"});
  REQUIRE(filtered.has_value());
  REQUIRE(filtered->size() == 1);
  CHECK((*filtered)[0].id == wanted);

  CHECK(link_ns::list(conn, {}).value().size() == 3);
  CHECK(link_ns::links_for_entity(conn, link_ns::external_entity_kind::task, task_a).value().size() == 2);
  CHECK(link_ns::list(conn, {.system_id = beta->id}).value().size() == 1);
  CHECK(link_ns::list(conn, {.system_slug = "absent"}).value().empty());
}

TEST_CASE("update_sync_state stamps last_synced_at on every outcome including error", "[engine][external][link]") {
  rig fixture;
  CHECK_FALSE(fixture.row.last_synced_at.has_value());
  REQUIRE(link_ns::update_sync_state(fixture.conn, fixture.row.id, link_ns::sync_status::error).has_value());
  fixture.reload();
  // The column records the last ATTEMPT, not the last success.
  CHECK(fixture.row.last_synced_at.has_value());
  CHECK(fixture.row.last_sync_status == link_ns::sync_status::error);
  CHECK(err(link_ns::update_sync_state(fixture.conn, 9999, link_ns::sync_status::ok)) ==
        std::optional{link_ns::link_error::not_found});
}

TEST_CASE("store_baseline writes an EMPTY STRING, never SQL NULL", "[engine][external][link]") {
  // `planar.db`'s bind_text forwards `value.data()`, which is a NULL POINTER
  // for a default-constructed string_view, and SQLite binds SQL NULL for it
  // (src/lib/db/db.cpp:98; task 6097 owns the root fix). That turns "the
  // agreed value is the empty string" into "there is no baseline", and
  // `baseline::present()` then reports false — which silently DISABLES
  // conflict detection for that link, because the two-way arm only runs when
  // a baseline is present.
  //
  // Every production caller happens to pass a view into a live std::string,
  // so a break-probe removing the guard survived the whole suite. This case
  // exercises the API the way a future caller legitimately might.
  rig fixture;
  REQUIRE(link_ns::store_baseline(fixture.conn, fixture.row.id, std::string_view{}, std::string_view{}).has_value());

  auto const base = link_ns::load_baseline(fixture.conn, fixture.row.id);
  REQUIRE(base.has_value());
  REQUIRE(base->title.has_value());
  REQUIRE(base->status.has_value());
  CHECK(base->title->empty());
  CHECK(base->status->empty());
  // The distinction that actually matters downstream.
  CHECK(base->present());
}

TEST_CASE("update_sync_direction returns the prior direction and logs an event", "[engine][external][link]") {
  rig        fixture;
  auto const prior = link_ns::update_sync_direction(fixture.conn, fixture.row.id, link_ns::sync_direction::read_only);
  REQUIRE(prior.has_value());
  CHECK(*prior == link_ns::sync_direction::two_way);
  fixture.reload();
  CHECK(fixture.row.direction == link_ns::sync_direction::read_only);

  auto const events = sync_ns::events_for_link(fixture.conn, fixture.row.id);
  REQUIRE(events->size() == 1);
  CHECK((*events)[0].direction == "push");
  CHECK((*events)[0].event_outcome == "ok");
  CHECK((*events)[0].fields_changed == std::optional<std::string>{R"(["sync_direction"])"});
  CHECK((*events)[0].detail ==
        std::optional<std::string>{
            R"({"old_direction":"two-way","new_direction":"read-only","operation":"sync_direction_update"})"});

  CHECK(err(link_ns::update_sync_direction(fixture.conn, 9999, link_ns::sync_direction::two_way)) ==
        std::optional{link_ns::link_error::not_found});
}

TEST_CASE("status reports one row per matching link", "[engine][external][sync]") {
  rig          fixture;
  stub_adapter provider;
  provider.remote = remote_of("Base", "todo", "v1");
  REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

  auto const rows = sync_ns::status(fixture.conn, {});
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].link_id == fixture.row.id);
  CHECK((*rows)[0].entity_kind == link_ns::external_entity_kind::task);
  CHECK((*rows)[0].entity_id == fixture.task_id);
  CHECK((*rows)[0].external_id == "SYNC-1");
  CHECK((*rows)[0].last_sync_status == link_ns::sync_status::ok);
  CHECK((*rows)[0].last_synced_at.has_value());
}

TEST_CASE("a link on an entity kind with no syncable triple pulls as noop", "[engine][external][sync]") {
  // Three of the seven entity kinds (test_scenario, decision, session) have no
  // title/status/updated_at triple. The Zig original returns an empty change
  // set rather than propagating the error, so the pull records a noop instead
  // of failing.
  scratch_db_path const scratch;
  auto                  conn       = open_migrated(scratch);
  auto const            registered = system_ns::register_github(conn, {.slug = "gh", .project = "o/r"});
  REQUIRE(registered.has_value());
  auto created = link_ns::create(conn, {.entity_kind = link_ns::external_entity_kind::decision,
                                        .entity_id   = 1,
                                        .system_id   = registered->id,
                                        .external_id = "o/r#1"});
  REQUIRE(created.has_value());

  stub_adapter provider;
  provider.remote   = remote_of("Anything", "doing", "v1");
  auto const result = sync_ns::pull_link(conn, *created, provider);
  REQUIRE(result.has_value());
  CHECK(result->result == sync_ns::outcome::noop);

  // A PUSH on the same link is a real failure, because push_link reads the
  // entity fields directly rather than through apply_remote_to_local.
  auto const pushed = sync_ns::push_link(conn, *created, provider);
  REQUIRE_FALSE(pushed.has_value());
  CHECK(err(pushed) == std::optional{sync_ns::sync_error::unsupported_entity_kind});
}

// --- break-probe survivors closed by task 6784 ---------------------------

TEST_CASE("D4d: an EMPTY remote field is 'no opinion' on the CONFLICT path too", "[engine][external][sync]") {
  // `!remote->title.empty()` is clause 1 of `*_remote_changed`, and an
  // empty remote field means the adapter expressed NO OPINION -- not that
  // the field was cleared. The existing empty-remote cases only exercise
  // the no-local-change path, where the outcome is the same either way.
  //
  // This is the path that discriminates it: the local side HAS changed, so
  // if an empty remote counted as "changed" the two would be declared in
  // conflict and the operator would be asked to resolve an edit the remote
  // never made. Closes break-probe survivors X03/X05 (task 6784).
  SECTION("an empty remote TITLE against a local title change is not a conflict") {
    rig          fixture;
    stub_adapter provider;
    provider.remote = remote_of("Baseline", "todo", "v1");
    REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

    fixture.set_task("Locally renamed", "todo");
    provider.remote   = remote_of("", "todo", "v2");
    auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
    REQUIRE(result.has_value());
    CHECK(result->result != sync_ns::outcome::conflict);
    CHECK(std::ranges::find(result->fields_changed, "title") == result->fields_changed.end());
  }

  SECTION("an empty remote STATUS against a local status change is not a conflict") {
    rig          fixture;
    stub_adapter provider;
    provider.remote = remote_of("Baseline", "todo", "v1");
    REQUIRE(sync_ns::pull_link(fixture.conn, fixture.row, provider).has_value());

    fixture.set_task("Baseline", "doing");
    provider.remote   = remote_of("Baseline", "", "v2");
    auto const result = sync_ns::pull_link(fixture.conn, fixture.row, provider);
    REQUIRE(result.has_value());
    CHECK(result->result != sync_ns::outcome::conflict);
    CHECK(std::ranges::find(result->fields_changed, "status") == result->fields_changed.end());
  }
}
