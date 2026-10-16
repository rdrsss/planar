// @file routing_tables_dropped.t.cpp
// @brief The verbs planar-explorer and the claim ritual write through still work on a
// database at migration 00043, which dropped the ten routing_* tables.
//
// One scratch database, migrated to head by the first verb. `planar` runs in process against
// it; `planar-agent` runs as the built binary against the same file (PLANAR_DB pinned to it by
// the parity harness), so a claim and its terminal verb are the real ones. The case asserts the
// database is at 43 with no routing object left, then drives, in order: `annotate add` (a file
// anchor), `annotate capabilities`, `annotate command` (an entity anchor on a plan),
// `annotate receipt`, `annotate update` on both anchors, `annotate remove` on the file anchor and
// `annotate archive` on the entity anchor, then `planar-agent claim` and `planar-agent complete` on
// a task.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.db;
import planar.json_dom;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

/// @brief One in-process `planar` invocation's result.
struct invocation {
  int         code = 0; ///< The exit code.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief A parity arena: `root/planar.db` is the database both binaries use.
struct world {
  parity::arena         arena = parity::make_arena("routing_tables_dropped");
  std::filesystem::path root  = arena.cpp_root;
  std::filesystem::path db    = root / "planar.db";
  int                   seq   = 0;

  /// @brief `planar <args>` in process, from `root/proj`, with `stdin_text` on standard input.
  auto planar(std::vector<std::string> args, std::string_view stdin_text = {}) -> invocation {
    std::vector<std::string> argv{"planar"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::istringstream input{std::string{stdin_text}};
    struct cin_restore {
      std::streambuf* previous;
      ~cin_restore() {
        std::cin.rdbuf(previous);
      }
    } restore{std::cin.rdbuf(input.rdbuf())};
    std::ostringstream   out;
    std::ostringstream   err;
    planar::cmd::context ctx{std::move(argv),
                             planar::cmd::map_env({{"HOME", (root / "fakehome").string()},
                                                   {"PLANAR_HOME", (root / "home").string()},
                                                   {"PLANAR_WORKBENCH_ROOT", (root / "workbench").string()},
                                                   {"PWD", (root / "proj").string()}}),
                             root / "proj",
                             std::make_shared<planar::cmd::database>(db, err),
                             out,
                             err};
    auto const           tree  = planar::cmd::root_app();
    auto const           table = planar::cmd::make_handler_table(*tree);
    invocation           got{.code = planar::cmd::run(ctx, *tree, table), .out = out.str(), .err = err.str()};
    INFO("planar " << args.front() << ": " << got.err << got.out);
    REQUIRE(got.code == 0);
    return got;
  }

  /// @brief The built `planar-agent` against the same database, required to succeed.
  auto agent(std::vector<std::string> args) -> parity::capture {
    auto got = parity::run_pinned(std::filesystem::path{PLANAR_AGENT_CPP_BIN}, args, root, std::format("a{}", ++seq));
    INFO("planar-agent " << args.front() << ": " << got.err << got.out);
    REQUIRE(got.code == 0);
    return got;
  }

  /// @brief One value of a one-row, one-column query, as text.
  auto text(std::string_view query) -> std::string {
    auto conn = planar::db::connection::open(db.string());
    REQUIRE(conn.has_value());
    auto stmt = conn->prepare(query);
    INFO(query);
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().value() == planar::db::step_result::row);
    return stmt->column_text(0);
  }
};

/// @brief The string member `key` of a JSON object.
auto member(std::string const& json, std::string_view key) -> std::string {
  auto parsed = planar::json_dom::parse_json(json);
  INFO(json);
  REQUIRE(parsed.has_value());
  auto const* value = parsed->find(key);
  REQUIRE(value != nullptr);
  return value->string;
}

} // namespace

TEST_CASE("annotate, its command and receipt, and a claim with its terminal verb work on a database without routing tables",
          "[cmd][migrate][drop-routing-tables][integration]") {
  world w;
  w.planar({"plan", "create", "Target plan", "--scope", "global", "--json"});
  w.planar({"task", "add", "Target task", "--plan", "1", "--scope", "global", "--editor=false", "--json"});

  CHECK(w.text("select max(version) from schema_migrations") == "43");
  CHECK(w.text("select count(*) from sqlite_master where name like 'routing\\_%' escape '\\' "
               "or tbl_name like 'routing\\_%' escape '\\' or name like '%\\_routing\\_%' escape '\\'") == "0");

  // A file anchor through `annotate add`.
  w.planar({"annotate", "add", "--anchor-path", "src/a.cpp", "--title", "File note", "--body", "file body", "--json"});
  auto const file_id = w.text("select id from annotations where anchor_kind = 'file'");

  // An entity anchor through the receipt-backed command, and its receipt.
  auto const source = member(w.planar({"annotate", "capabilities", "--json"}).out, "source_uuid");
  w.planar({"annotate", "command", "--request", "@-"},
           std::format(R"({{"operation":"create","operation_id":"routing-drop-entity","source_uuid":"{}","target_kind":"plan",)"
                       R"("target_id":1,"title":"Entity note","body":"entity body"}})",
                       source));
  auto const entity_id = w.text("select id from annotations where anchor_kind = 'entity'");
  auto const receipt =
      w.planar({"annotate", "receipt", "--source-uuid", source, "--operation-id", "routing-drop-entity", "--json"}).out;
  CHECK(receipt.contains(R"("found":true)"));

  // Update on both anchors.
  for (auto const& id : {file_id, entity_id}) {
    INFO("annotation " << id);
    w.planar({"annotate", "update", id, "--title", "Revised", "--json"});
    CHECK(w.text(std::format("select title from annotations where id = {}", id)) == "Revised");
  }
  // Remove on the file anchor. The entity anchor was created through `annotate command`, and its
  // create receipt references the row (annotation_operation_receipts.annotation_id, no ON DELETE,
  // since migration 00034), so a hard remove of it is refused at any schema version; it is ended
  // by archiving instead.
  auto const revision = w.text(std::format("select revision from annotations where id = {}", file_id));
  w.planar({"annotate", "remove", file_id, "--expected-revision", revision, "--json"});
  CHECK(w.text(std::format("select count(*) from annotations where id = {}", file_id)) == "0");
  w.planar({"annotate", "archive", entity_id, "--json"});
  CHECK(w.text(std::format("select status from annotations where id = {}", entity_id)) == "archived");

  // A claim and its terminal verb.
  auto const claimed =
      w.agent({"claim", "--entity", "task:1", "--vendor", "claude", "--role", "coder", "--ttl", "30m", "--no-locality-probe"});
  auto const at = claimed.out.find("claim:");
  REQUIRE(at != std::string::npos);
  auto const token = claimed.out.substr(at + 6, claimed.out.find(' ', at + 6) - (at + 6));
  w.agent({"complete", "--claim", token, "--no-locality-probe"});
  CHECK(w.text("select status from agent_work_claims where claim_token = '" + token + "'") == "completed");
  CHECK(w.text("select status from tasks where id = 1") == "done");
}
