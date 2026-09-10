// @file render_cli.t.cpp
// @brief Unit tests for `planar.engine.workbench.render_cli` (plan 996, task
// 6037): the exact operator-facing bytes.
//
// ORACLE PROVENANCE. Every literal below was captured from a real run
// against a scratch database and workbench root, redirected to its own file
// (the Zig runtime uses POSITIONAL writes, so two invocations sharing one
// redirect target overwrite each other). Representative captures:
//
//   $Z workbench push 1
//     workbench push: plan 1 (demo-feature) - 0 applied, 0 pending,
//       1 filtered (mode=failures), 0 conflict(s)
//       1 pre-existing terminal file(s) on disk — run 'planar workbench gc 1'
//       to remove, or re-push with --apply-cleanup
//   $Z workbench status 1
//     workbench status: plan 1 (demo-feature) - 0 applied, 0 pending, 0 conflict(s)
//   $Z workbench status 1 --json
//     {"applied":0,...,"filter_mode":"failures","entries":[{"class":"no_op",...}]}
//   $Z workbench status --json          [NO plan: a SHORTER payload]
//     {"applied":0,"pending":0,"conflicts":0,"malformed":0,"malformed_files":[],
//      "filtered":0,"pre_existing_terminal":0,"cleaned":0}
//   $Z workbench list
//     p1-demo-feature                           draft       tree        project:demo
//   $Z workbench list --json
//     [{"plan":1,"slug":"demo-feature","status":"draft","assoc":"project:demo",
//       "plan_key":"p1","has_fs_tree":true}]
//   $Z workbench gc 1 --dry-run
//     workbench gc (--dry-run): would remove 1, keep 6, drifted-skipped 0, errors 0 (mode=failures)
//   $Z workbench lint --path bad_status.md
//     bad_status.md:5:
//       error[invalid_field_value]: front matter field 'status' has an unsupported value
//       hint: use one of: todo, doing, blocked, done, or cancelled
//     1 files scanned, 1 errors, 0 warnings.
//   $Z workbench archive 1 --json
//     {"archived":true,"plan":1,"feature_dir":"…"}
//   $Z workbench resolve 1 --prefer db
//     resolved conflict event 1 (preferred db)
//
// Every one of these was ALSO diffed live between the two binaries in
// identical pinned arenas: stdout, stderr and exit code matched on all of
// them.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.gc;
import planar.engine.workbench.lint;
import planar.engine.workbench.render_cli;
import planar.engine.workbench.sync;
import planar.engine.workbench.terminal;

namespace {

namespace rc = planar::engine::workbench::render_cli;
namespace ws = planar::engine::workbench::sync;
namespace wg = planar::engine::workbench::gc;
namespace wl = planar::engine::workbench::lint;
namespace wt = planar::engine::workbench::terminal;

auto no_op_entry(std::string_view path, std::string_view kind, std::int64_t id) -> ws::entry {
  return ws::entry{
      .value = ws::classification::no_op, .file_path = std::string{path}, .entity_kind = std::string{kind}, .entity_id = id};
}

} // namespace

// --- the sync summary line -----------------------------------------------

TEST_CASE("a non-push summary omits the filtered clause", "[workbench][render_cli]") {
  ws::result value;
  CHECK(rc::render_sync_result_text(1, "demo-feature", ws::mode::status, "status", value, false) ==
        "workbench status: plan 1 (demo-feature) - 0 applied, 0 pending, 0 conflict(s)\n");
}

TEST_CASE("a push summary carries filtered and the active mode", "[workbench][render_cli]") {
  ws::result value;
  value.filtered = 1;
  CHECK(rc::render_sync_result_text(1, "demo-feature", ws::mode::push, "push", value, false) ==
        "workbench push: plan 1 (demo-feature) - 0 applied, 0 pending, 1 filtered (mode=failures), 0 conflict(s)\n");
  value.filter_mode = "all";
  CHECK(rc::render_sync_result_text(1, "demo-feature", ws::mode::push, "push", value, false).find("(mode=all)") !=
        std::string::npos);
}

TEST_CASE("a malformed count is appended BEFORE the newline", "[workbench][render_cli]") {
  ws::result value;
  value.malformed = 2;
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::pull, "pull", value, false) ==
        "workbench pull: plan 1 (s) - 0 applied, 0 pending, 0 conflict(s), 2 MALFORMED\n");
}

TEST_CASE("the pre-existing-terminal remediation line appears only when nothing was cleaned", "[workbench][render_cli]") {
  ws::result value;
  value.filtered              = 1;
  value.pre_existing_terminal = 1;
  auto const warned           = rc::render_sync_result_text(1, "s", ws::mode::push, "push", value, false);
  CHECK(warned.ends_with("  1 pre-existing terminal file(s) on disk \xe2\x80\x94 run 'planar workbench gc 1' to "
                         "remove, or re-push with --apply-cleanup\n"));
  value.cleaned      = 1;
  auto const cleaned = rc::render_sync_result_text(1, "s", ws::mode::push, "push", value, false);
  CHECK(cleaned.ends_with("  1 pre-existing terminal file(s) cleaned\n"));
  CHECK(cleaned.find("re-push with --apply-cleanup") == std::string::npos);
}

TEST_CASE("the remediation line is push-only", "[workbench][render_cli]") {
  ws::result value;
  value.pre_existing_terminal = 1;
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::pull, "pull", value, false).find("pre-existing") == std::string::npos);
}

TEST_CASE("conflicts print one line each plus the remediation footer", "[workbench][render_cli]") {
  ws::result value;
  value.conflicts = 1;
  value.pending   = 1;
  value.entries.push_back(ws::entry{
      .value = ws::classification::conflict, .file_path = "a/b/2-t.md", .entity_kind = "task", .entity_id = 2, .conflict_id = 7});
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::pull, "pull", value, false) ==
        "workbench pull: plan 1 (s) - 0 applied, 1 pending, 1 conflict(s)\n"
        "  CONFLICT [7]: a/b/2-t.md (task 2)\n"
        "  1 conflict(s) - run 'workbench resolve <event-id> --prefer fs|db'\n");
}

// --- verbose --------------------------------------------------------------

TEST_CASE("verbose prints a header and one line per non-no_op entry", "[workbench][render_cli][verbose]") {
  ws::result value;
  value.entries = {
      no_op_entry("a/README.md", "plan", 1),
      ws::entry{.value = ws::classification::db_to_fs, .file_path = "a/x.md", .entity_kind = "task", .entity_id = 1},
      ws::entry{.value = ws::classification::fs_to_db, .file_path = "a/y.md", .entity_kind = "task", .entity_id = 2},
      ws::entry{.value = ws::classification::deleted_on_fs, .file_path = "a/z.md", .entity_kind = "task", .entity_id = 3},
      ws::entry{.value = ws::classification::new_on_fs, .file_path = "a/n.md", .entity_kind = "task", .entity_id = 4},
      ws::entry{.value       = ws::classification::malformed,
                .file_path   = "a/m.md",
                .entity_kind = "",
                .entity_id   = 0,
                .parse_error = "MalformedFrontmatter"},
  };
  // Under PUSH: FS-writing, not DB-writing.
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::push, "push", value, true) ==
        "workbench push: plan 1 (s)\n"
        "  applied DB->FS: a/x.md\n"
        "  pending FS->DB: a/y.md  [run pull to apply]\n"
        "  missing: a/z.md\n"
        "  new on FS:  a/n.md\n"
        "  MALFORMED: a/m.md (MalformedFrontmatter)\n");
  // Under PULL: the mirror image.
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::pull, "pull", value, true) ==
        "workbench pull: plan 1 (s)\n"
        "  pending DB->FS: a/x.md  [run push to apply]\n"
        "  applied FS->DB: a/y.md\n"
        "  deleted: a/z.md -> task 3 cancelled\n"
        "  new entity: a/n.md -> task 4\n"
        "  MALFORMED: a/m.md (MalformedFrontmatter)\n");
  // Under SYNC: both directions are WRITTEN, unlike push/pull which each
  // only write one side. Closes a break-probe SURVIVOR (task 6423): a
  // mutant that dropped the `|| run_mode == sync::mode::sync` half of
  // EITHER `writes_fs` or `writes_db` passed every existing fixture here,
  // because push and pull each only ever probed their OWN clause -- neither
  // exercised `mode::sync` at all (mirroring task 6416's headline find that
  // `mode::sync` had zero coverage in sync.cpp's own core).
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::sync, "sync", value, true) ==
        "workbench sync: plan 1 (s)\n"
        "  applied DB->FS: a/x.md\n"
        "  applied FS->DB: a/y.md\n"
        "  deleted: a/z.md -> task 3 cancelled\n"
        "  new entity: a/n.md -> task 4\n"
        "  MALFORMED: a/m.md (MalformedFrontmatter)\n");
}

TEST_CASE("verbose SUPPRESSES the pre-existing-terminal line", "[workbench][render_cli][verbose]") {
  // Oracle-confirmed: `push --verbose` on a tree with a pre-existing
  // terminal file prints no remediation line at all, because the Zig
  // handler returns early into a different printer.
  ws::result value;
  value.pre_existing_terminal = 1;
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::push, "push", value, true) == "workbench push: plan 1 (s)\n");
}

TEST_CASE("a malformed entry with no parse_error omits the parenthetical", "[workbench][render_cli][verbose]") {
  ws::result value;
  value.entries.push_back(
      ws::entry{.value = ws::classification::malformed, .file_path = "a/m.md", .entity_kind = "", .entity_id = 0});
  CHECK(rc::render_sync_result_text(1, "s", ws::mode::pull, "pull", value, true) ==
        "workbench pull: plan 1 (s)\n  MALFORMED: a/m.md\n");
}

// --- JSON -----------------------------------------------------------------

TEST_CASE("the single-plan JSON payload matches the oracle's field order", "[workbench][render_cli][json]") {
  ws::result value;
  value.entries.push_back(no_op_entry("project_demo/p1-demo-feature/README.md", "plan", 1));
  CHECK(rc::render_sync_result_json(value) ==
        "{\"applied\":0,\"pending\":0,\"conflicts\":0,\"malformed\":0,\"malformed_files\":[],\"filtered\":0,"
        "\"pre_existing_terminal\":0,\"cleaned\":0,\"filter_mode\":\"failures\",\"entries\":["
        "{\"class\":\"no_op\",\"file_path\":\"project_demo/p1-demo-feature/README.md\",\"entity_kind\":\"plan\","
        "\"entity_id\":1,\"conflict_id\":0,\"parse_error\":\"\"}]}\n");
}

TEST_CASE("malformed_files carries the path and the Zig error tag", "[workbench][render_cli][json]") {
  ws::result value;
  value.malformed = 1;
  value.malformed_files.push_back(ws::malformed_file{.path = "a/junk.md", .parse_error = "MalformedFrontmatter"});
  CHECK(rc::render_sync_result_json(value).find(
            "\"malformed_files\":[{\"path\":\"a/junk.md\",\"parse_error\":\"MalformedFrontmatter\"}]") != std::string::npos);
}

TEST_CASE("the NO-PLAN status payload is a SHORTER shape", "[workbench][render_cli][json]") {
  // It stops after `cleaned` -- neither `filter_mode` nor `entries`.
  // Oracle-captured; not an oversight to be tidied into the other shape.
  CHECK(rc::render_status_totals_json(rc::status_totals{}) ==
        "{\"applied\":0,\"pending\":0,\"conflicts\":0,\"malformed\":0,\"malformed_files\":[],\"filtered\":0,"
        "\"pre_existing_terminal\":0,\"cleaned\":0}\n");
}

TEST_CASE("JSON strings are escaped through the tree's one escaper", "[workbench][render_cli][json]") {
  // `planar.json_text`, never a local copy -- ten hand-rolled duplicates
  // were swept in 5728133 and three of them dropped \b and \f.
  ws::result value;
  value.entries.push_back(no_op_entry("a/\"quoted\"\\path\n.md", "plan", 1));
  auto const out = rc::render_sync_result_json(value);
  CHECK(out.find("\"file_path\":\"a/\\\"quoted\\\"\\\\path\\n.md\"") != std::string::npos);
}

// --- list -----------------------------------------------------------------

TEST_CASE("list --json uses the key `assoc`, not `assoc_slug`", "[workbench][render_cli][json]") {
  std::vector<ws::active_feature> const items{ws::active_feature{.plan_id     = 1,
                                                                 .slug        = "demo-feature",
                                                                 .status      = "draft",
                                                                 .assoc_slug  = "project:demo",
                                                                 .plan_key    = "p1",
                                                                 .has_fs_tree = true}};
  CHECK(rc::render_list_json(items) == "[{\"plan\":1,\"slug\":\"demo-feature\",\"status\":\"draft\","
                                       "\"assoc\":\"project:demo\",\"plan_key\":\"p1\",\"has_fs_tree\":true}]\n");
}

TEST_CASE("list --json on an empty database is `[]`, not zero bytes", "[workbench][render_cli][json]") {
  CHECK(rc::render_list_json({}) == "[]\n");
}

TEST_CASE("the list text table pads to 40/10/10 and never truncates", "[workbench][render_cli]") {
  std::vector<ws::active_feature> const items{ws::active_feature{.plan_id     = 1,
                                                                 .slug        = "demo-feature",
                                                                 .status      = "draft",
                                                                 .assoc_slug  = "project:demo",
                                                                 .plan_key    = "p1",
                                                                 .has_fs_tree = true}};
  CHECK(rc::render_list_text(items) == "p1-demo-feature                           draft       tree        project:demo\n");

  // A value wider than its column runs wide rather than being cut -- the
  // table is for humans and a truncated slug would be unusable.
  std::vector<ws::active_feature> const wide{ws::active_feature{
      .plan_id = 2, .slug = std::string(45, 'x'), .status = "draft", .assoc_slug = "a", .plan_key = "p2", .has_fs_tree = false}};
  CHECK(rc::render_list_text(wide) == std::format("p2-{}  draft       no-tree     a\n", std::string(45, 'x')));
}

TEST_CASE("an empty list prints `no features found`", "[workbench][render_cli]") {
  CHECK(rc::render_list_text({}) == "no features found\n");
  CHECK(rc::render_no_active_features() == "no active features found\n");
}

// --- gc -------------------------------------------------------------------

TEST_CASE("gc's two text forms differ in verb and preamble", "[workbench][render_cli]") {
  wg::summary value{.removed = 1, .kept = 6};
  CHECK(rc::render_gc_text(value, true, wt::mode::failures) ==
        "workbench gc (--dry-run): would remove 1, keep 6, drifted-skipped 0, errors 0 (mode=failures)\n");
  CHECK(rc::render_gc_text(value, false, wt::mode::all) ==
        "workbench gc: removed 1, kept 6, drifted-skipped 0, errors 0 (mode=all)\n");
}

TEST_CASE("gc --json carries dry_run as a bare boolean", "[workbench][render_cli][json]") {
  wg::summary value{.removed = 0, .kept = 7};
  CHECK(rc::render_gc_json(value, true, wt::mode::failures) ==
        "{\"removed\":0,\"kept\":7,\"drifted_skipped\":0,\"errors\":0,\"dry_run\":true,\"filter_mode\":\"failures\"}\n");
  CHECK(rc::render_gc_json(value, false, wt::mode::all).find("\"dry_run\":false") != std::string::npos);
}

TEST_CASE("the gc drift refusal names every held-back file", "[workbench][render_cli]") {
  // The handler emits this on stderr and exits 1. The ORACLE does not: it
  // loses the same message to an unflushed buffer before exiting (task 6122
  // and the comment in src/cmd/planar/handlers/workbench.cpp), so this is a
  // sanctioned divergence rather than a parity gap.
  wg::summary value{.drifted_skipped = 2, .drifted_paths = {"/wb/a.md", "/wb/b.md"}};
  CHECK(rc::render_gc_drift_refusal(value) ==
        "workbench gc: refused to remove 2 file(s) with FS-content drift from DB; re-run with --yes to discard, or "
        "'workbench pull' first\n"
        "  drift: /wb/a.md\n"
        "  drift: /wb/b.md\n");
}

// --- lint -----------------------------------------------------------------

TEST_CASE("lint text prints path:line, then the coded message, then the hint", "[workbench][render_cli]") {
  wl::result value{.files_scanned = 1, .errors = 1, .warnings = 0};
  value.issues.push_back(wl::issue{.path    = "/tmp/bad_status.md",
                                   .line    = 5,
                                   .level   = wl::severity::error,
                                   .code    = "invalid_field_value",
                                   .message = "front matter field 'status' has an unsupported value",
                                   .hint    = "use one of: todo, doing, blocked, done, or cancelled"});
  CHECK(rc::render_lint_text(value) == "/tmp/bad_status.md:5:\n"
                                       "  error[invalid_field_value]: front matter field 'status' has an unsupported value\n"
                                       "  hint: use one of: todo, doing, blocked, done, or cancelled\n"
                                       "1 files scanned, 1 errors, 0 warnings.\n");
}

TEST_CASE("a hintless issue omits the hint line", "[workbench][render_cli]") {
  wl::result value{.files_scanned = 1, .errors = 1, .warnings = 0};
  value.issues.push_back(
      wl::issue{.path = "a.md", .line = 1, .level = wl::severity::error, .code = "c", .message = "m", .hint = ""});
  CHECK(rc::render_lint_text(value) == "a.md:1:\n  error[c]: m\n1 files scanned, 1 errors, 0 warnings.\n");
}

TEST_CASE("a clean lint still prints the tally", "[workbench][render_cli]") {
  CHECK(rc::render_lint_text(wl::result{.files_scanned = 6}) == "6 files scanned, 0 errors, 0 warnings.\n");
}

TEST_CASE("lint --json is NDJSON and is ZERO BYTES when clean", "[workbench][render_cli][json]") {
  // The per-RECORD terminator case. A blanket "append one newline to the
  // document" rule would put a stray newline here on a clean run.
  CHECK(rc::render_lint_json({}).empty());
  std::vector<wl::issue> const issues{wl::issue{
      .path = "a.md", .line = 5, .level = wl::severity::warning, .code = "anchor_plan_not_found", .message = "m", .hint = "h"}};
  CHECK(rc::render_lint_json(issues) ==
        "{\"path\":\"a.md\",\"line\":5,\"severity\":\"warning\",\"code\":\"anchor_plan_not_found\","
        "\"message\":\"m\",\"hint\":\"h\"}\n");
}

// --- the one-line verbs ---------------------------------------------------

TEST_CASE("archive, restore and resolve each render both forms", "[workbench][render_cli]") {
  CHECK(rc::render_archive(1, "/wb/a/p1-s", false) == "archived: /wb/a/p1-s (plan 1)\n");
  CHECK(rc::render_archive(1, "/wb/a/p1-s", true) == "{\"archived\":true,\"plan\":1,\"feature_dir\":\"/wb/a/p1-s\"}\n");
  CHECK(rc::render_restore(1, "/wb/a/p1-s", false) == "restored: /wb/a/p1-s (plan 1)\n");
  CHECK(rc::render_restore(1, "/wb/a/p1-s", true) == "{\"restored\":true,\"plan\":1,\"feature_dir\":\"/wb/a/p1-s\"}\n");
  CHECK(rc::render_resolve(1, ws::conflict_resolution::db, false) == "resolved conflict event 1 (preferred db)\n");
  CHECK(rc::render_resolve(1, ws::conflict_resolution::fs, true) == "{\"resolved\":true,\"event_id\":1,\"prefer\":\"fs\"}\n");
}

// --- error bodies (FRAGMENTS: no prefix, no terminator) -------------------

TEST_CASE("the error bodies carry no `error: ` prefix and no newline", "[workbench][render_cli]") {
  auto const malformed = rc::error_body_malformed(1, 1);
  CHECK(malformed == "1 malformed workbench file(s); run 'planar workbench lint 1' for details");
  CHECK_FALSE(malformed.starts_with("error: "));
  CHECK_FALSE(malformed.ends_with("\n"));
  // With no plan (the no-argument `status` path) the remediation names --all.
  CHECK(rc::error_body_malformed(3, std::nullopt) ==
        "3 malformed workbench file(s); run 'planar workbench lint --all' for details");
  CHECK(rc::error_body_conflicts(2) == "2 conflict(s) require 'workbench resolve <event-id> --prefer fs|db'");
  CHECK(rc::error_body_lint_issues(1, 0) == "workbench lint found 1 error(s) and 0 warning(s)");
}
