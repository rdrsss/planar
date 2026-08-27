// @file output.t.cpp
// @brief Byte-exact leaf-output tests for
// `planar.engine.templates.output` (plan 996, task 6190).
//
// HOME SAFETY. Pure string work. No file, no environment variable, no path
// construction.
//
// ORACLE PROVENANCE. Every literal below is bytes the built Zig binary
// wrote, captured under a pinned scratch arena with stderr taken through a
// PIPE (see src/cmd/parity_harness.hpp).
//
// THE FOUR EMPTY-OUTPUT SPELLINGS are the reason this file is worth its
// length. They are all different, none is derivable from another, and
// three of the four are the kind of thing a plausible implementation gets
// wrong by emitting `[]` or a bare newline:
//
//   templates list        (no matches)  -> "no templates found\n"
//   templates list --json (no matches)  -> ZERO BYTES
//   templates init        (all present) -> "templates init: nothing to do (all templates already present)\n"
//   templates init --json (all present) -> ZERO BYTES

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.templates.output;
import planar.engine.templates.validate;

namespace tpl = planar::engine::templates;

namespace {

/// @brief A disk row.
/// @param set The set name.
/// @param system The system slug.
/// @param kind The kind.
/// @return The row.
auto disk_row(std::string set, std::string system, std::string kind) -> tpl::list_row {
  auto path = std::format("/root/{}/{}/{}.json", set, system, kind);
  return {.set_name = std::move(set),
          .system   = std::move(system),
          .kind     = std::move(kind),
          .source   = "disk",
          .path     = std::move(path)};
}

/// @brief An embedded row.
/// @param system The system slug.
/// @param kind The kind.
/// @return The row.
auto embedded_row(std::string system, std::string kind) -> tpl::list_row {
  auto path = std::format("embedded:{}/{}.json", system, kind);
  return {
      .set_name = "default", .system = std::move(system), .kind = std::move(kind), .source = "embedded", .path = std::move(path)};
}

} // namespace

// --- merge, filter, sort ----------------------------------------------

TEST_CASE("merge_list_rows lets a DISK row supersede an embedded row with the same triple", "[templates][output][list]") {
  // Oracle: before `templates init` every row reads `embedded`; after it,
  // every row reads `disk`. That word is the operator's signal that they
  // now own the files, so the dedup direction is observable.
  std::vector<tpl::list_row> const disk     = {disk_row("default", "jira", "epic")};
  std::vector<tpl::list_row> const embedded = {embedded_row("jira", "epic"), embedded_row("jira", "story")};

  auto const rows = tpl::merge_list_rows(disk, embedded, std::nullopt, std::nullopt);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].kind == "epic");
  CHECK(rows[0].source == "disk");
  // The SURVIVING embedded row proves the dedup removed one row rather
  // than emptying the embedded list.
  CHECK(rows[1].kind == "story");
  CHECK(rows[1].source == "embedded");
}

TEST_CASE("merge_list_rows dedups on the full TRIPLE, not on kind alone", "[templates][output][list]") {
  // A disk `jira/epic` must not shadow an embedded `github-issues/epic`.
  std::vector<tpl::list_row> const disk     = {disk_row("default", "jira", "epic")};
  std::vector<tpl::list_row> const embedded = {embedded_row("github-issues", "epic")};
  auto const                       rows     = tpl::merge_list_rows(disk, embedded, std::nullopt, std::nullopt);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].system == "github-issues");
  CHECK(rows[1].system == "jira");
}

TEST_CASE("merge_list_rows sorts by (set, system, kind)", "[templates][output][list]") {
  std::vector<tpl::list_row> const disk = {
      disk_row("zed", "jira", "story"),
      disk_row("alpha", "jira", "story"),
      disk_row("alpha", "github-issues", "issue"),
      disk_row("alpha", "github-issues", "aaa"),
  };
  auto const rows = tpl::merge_list_rows(disk, {}, std::nullopt, std::nullopt);
  REQUIRE(rows.size() == 4);
  CHECK(rows[0].set_name == "alpha");
  CHECK(rows[0].kind == "aaa");
  CHECK(rows[1].kind == "issue");
  CHECK(rows[2].system == "jira");
  CHECK(rows[3].set_name == "zed");
}

TEST_CASE("merge_list_rows treats an EMPTY filter as NO filter", "[templates][output][list][filter]") {
  // Oracle: `templates list --system ""` listed all ten embedded
  // templates, identically to `templates list`. This is the empty-value
  // hazard that produced a false-positive defect report elsewhere in this
  // milestone — an empty flag reaching a filter means "unset", not
  // "match the empty string".
  std::vector<tpl::list_row> const disk = {disk_row("default", "jira", "epic")};
  CHECK(tpl::merge_list_rows(disk, {}, std::string_view{""}, std::nullopt).size() == 1);
  CHECK(tpl::merge_list_rows(disk, {}, std::nullopt, std::string_view{""}).size() == 1);
  CHECK(tpl::merge_list_rows(disk, {}, std::string_view{""}, std::string_view{""}).size() == 1);
}

TEST_CASE("merge_list_rows's --system filter actually EXCLUDES", "[templates][output][list][filter]") {
  // Both directions: the matching row survives AND the non-matching row is
  // gone. Asserting only the survivor would pass for a filter that does
  // nothing.
  std::vector<tpl::list_row> const disk = {disk_row("default", "jira", "epic"), disk_row("default", "github-issues", "issue")};
  auto const                       rows = tpl::merge_list_rows(disk, {}, std::string_view{"jira"}, std::nullopt);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].system == "jira");
  // And a filter matching nothing empties it entirely rather than falling
  // back to "everything".
  CHECK(tpl::merge_list_rows(disk, {}, std::string_view{"nosuch"}, std::nullopt).empty());
}

TEST_CASE("merge_list_rows's --set filter actually EXCLUDES", "[templates][output][list][filter]") {
  std::vector<tpl::list_row> const disk = {disk_row("probe", "px", "a"), disk_row("default", "jira", "epic")};
  auto const                       rows = tpl::merge_list_rows(disk, {}, std::nullopt, std::string_view{"probe"});
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].set_name == "probe");
  CHECK(tpl::merge_list_rows(disk, {}, std::nullopt, std::string_view{"nosuch"}).empty());
}

TEST_CASE("merge_list_rows applies BOTH filters conjunctively", "[templates][output][list][filter]") {
  std::vector<tpl::list_row> const disk = {disk_row("probe", "px", "a"), disk_row("probe", "jira", "b"),
                                           disk_row("default", "px", "c")};
  auto const                       rows = tpl::merge_list_rows(disk, {}, std::string_view{"px"}, std::string_view{"probe"});
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].kind == "a");
}

// --- list output ------------------------------------------------------

TEST_CASE("list_text is a 20-wide three-column table under a 70-dash rule", "[templates][output][list]") {
  // Oracle bytes from `templates list`, reduced to two rows.
  auto const rows = tpl::merge_list_rows(std::vector<tpl::list_row>{disk_row("default", "github-issues", "issue")},
                                         std::vector<tpl::list_row>{embedded_row("jira", "epic")}, std::nullopt, std::nullopt);
  auto const out  = tpl::list_text(rows);

  auto const expected = std::string{"SET                  SYSTEM               KIND                 SOURCE\n"} +
                        std::string(70, '-') + "\n" +
                        "default              github-issues        issue                disk\n"
                        "default              jira                 epic                 embedded\n";
  CHECK(out == expected);
}

TEST_CASE("list_text never TRUNCATES an over-long value", "[templates][output][list]") {
  // It pushes the row right instead. A truncating implementation would
  // still look plausible on the shipped set, whose longest value is 14
  // characters.
  std::vector<tpl::list_row> const rows = {disk_row("a-set-name-far-longer-than-twenty-characters", "sys", "kind")};
  auto const                       out  = tpl::list_text(rows);
  CHECK(out.find("a-set-name-far-longer-than-twenty-characters sys") != std::string::npos);
}

TEST_CASE("list_text on NO rows is one sentence, not an empty table", "[templates][output][list][empty]") {
  // Oracle: `templates list --system nosuch` -> `no templates found\n`,
  // exit 0. NOT a header with zero rows.
  CHECK(tpl::list_text({}) == "no templates found\n");
}

TEST_CASE("list_json is NDJSON, one complete object per line", "[templates][output][list]") {
  // Oracle bytes from `templates list --json`: no enclosing array, no
  // commas between records, a newline after each.
  std::vector<tpl::list_row> const rows = {embedded_row("jira", "epic")};
  CHECK(tpl::list_json(rows) ==
        R"({"set":"default","system":"jira","kind":"epic","source":"embedded","path":"embedded:jira/epic.json"})"
        "\n");
}

TEST_CASE("list_json on NO rows is ZERO BYTES, not []", "[templates][output][list][empty]") {
  // Oracle: `templates list --system nosuch --json` wrote 0 bytes at exit
  // 0. `[]` would be valid JSON and a parity break.
  CHECK(tpl::list_json({}).empty());
}

TEST_CASE("list_json escapes its string values", "[templates][output][list]") {
  std::vector<tpl::list_row> const rows = {{.set_name = "a\"b", .system = "s", .kind = "k", .source = "disk", .path = "/p\\q"}};
  auto const                       out  = tpl::list_json(rows);
  CHECK(out.find(R"("set":"a\"b")") != std::string::npos);
  CHECK(out.find(R"("path":"/p\\q")") != std::string::npos);
}

// --- show -------------------------------------------------------------

TEST_CASE("show_text emits the RAW bytes and adds a newline only when missing", "[templates][output][show]") {
  // Oracle: `templates show default github-issues issue` reproduced the
  // file's own formatting — `"labels": ["planar-managed", "type:task"]`
  // stayed on ONE line, which a DOM round trip would have broken across
  // four.
  CHECK(tpl::show_text("{\"a\":1}") == "{\"a\":1}\n");
  CHECK(tpl::show_text("{\"a\":1}\n") == "{\"a\":1}\n");
  CHECK(tpl::show_text("{\"a\":1}\n\n") == "{\"a\":1}\n\n");
}

TEST_CASE("show_text on an EMPTY template is zero bytes, not a bare newline", "[templates][output][show][empty]") {
  // zig guards on `raw.len > 0` before appending.
  CHECK(tpl::show_text("").empty());
}

// --- init -------------------------------------------------------------

TEST_CASE("init_text reports a count then one indented path per file", "[templates][output][init]") {
  // Oracle bytes from the first `templates init`.
  std::vector<std::string> const created = {"/r/default/jira/epic.json", "/r/default/jira/story.json"};
  CHECK(tpl::init_text(created) == "templates init: wrote 2 file(s)\n"
                                   "  /r/default/jira/epic.json\n"
                                   "  /r/default/jira/story.json\n");
}

TEST_CASE("init_text says `file(s)` even for exactly one", "[templates][output][init]") {
  // No pluralization logic. The oracle's format string is literal.
  std::vector<std::string> const created = {"/r/default/jira/epic.json"};
  CHECK(tpl::init_text(created) == "templates init: wrote 1 file(s)\n  /r/default/jira/epic.json\n");
}

TEST_CASE("init_text on NOTHING WRITTEN is the nothing-to-do sentence", "[templates][output][init][empty]") {
  // Oracle: the SECOND `templates init` (and `--force`) printed exactly
  // this at exit 0.
  CHECK(tpl::init_text({}) == "templates init: nothing to do (all templates already present)\n");
}

TEST_CASE("init_json is NDJSON and ZERO BYTES when nothing was written", "[templates][output][init][empty]") {
  std::vector<std::string> const created = {"/r/a.json"};
  CHECK(tpl::init_json(created) == "{\"path\":\"/r/a.json\"}\n");
  CHECK(tpl::init_json({}).empty());
}

// --- validate ---------------------------------------------------------

TEST_CASE("validate_ok renders both spellings of a clean template", "[templates][output][validate]") {
  // Oracle bytes from `templates validate default github-issues issue`
  // with and without `--json`.
  CHECK(tpl::validate_ok("default", "github-issues", "issue", false) == "ok: default/github-issues/issue\n");
  CHECK(tpl::validate_ok("default", "github-issues", "issue", true) ==
        R"({"ok":true,"set":"default","system":"github-issues","kind":"issue","issues":[]})"
        "\n");
}

TEST_CASE("validate_ok reports the REQUESTED set, not the resolved one", "[templates][output][validate]") {
  // Oracle: `templates validate myset jira epic --json` on a machine with
  // no `myset` on disk still said `"set":"myset"` — the template came from
  // the embedded defaults and the loader stamped the requested name onto
  // it.
  CHECK(tpl::validate_ok("myset", "jira", "epic", true) ==
        R"({"ok":true,"set":"myset","system":"jira","kind":"epic","issues":[]})"
        "\n");
}

TEST_CASE("validate_issues text leads every line with the RESOLVED path", "[templates][output][validate]") {
  // Oracle bytes from `templates validate probe px broken`: the full path
  // is repeated on each line, not printed once as a header.
  std::vector<tpl::validation_issue> const issues = {{.json_path = "a", .message = "UnknownField"},
                                                     {.json_path = "b", .message = "UnsupportedDirective"}};
  CHECK(tpl::validate_issues("probe", "px", "broken", "/r/probe/px/broken.json", issues, false) ==
        "ISSUE: /r/probe/px/broken.json [a]: UnknownField\n"
        "ISSUE: /r/probe/px/broken.json [b]: UnsupportedDirective\n");
}

TEST_CASE("validate_issues json is one envelope with an ESCAPED issues array", "[templates][output][validate]") {
  // Oracle bytes from `templates validate probe px broken --json`.
  std::vector<tpl::validation_issue> const issues = {
      {.json_path = "a", .message = "UnknownField"},
      {.json_path = "(smoke-render)", .message = "smoke render failed: UnknownField"}};
  // Custom `J(...)J` delimiters: the payload contains the literal
  // `(smoke-render)`, and a default-delimited raw string would END at the
  // `)"` inside it.
  CHECK(tpl::validate_issues("probe", "px", "broken", "/ignored", issues, true) ==
        R"J({"ok":false,"set":"probe","system":"px","kind":"broken","issues":[)J"
        R"J({"json_path":"a","message":"UnknownField"},)J"
        R"J({"json_path":"(smoke-render)","message":"smoke render failed: UnknownField"}]})J"
        "\n");
}

TEST_CASE("validate_issues json does NOT escape set/system/kind — reproduced defect",
          "[templates][output][validate][oracle-defect]") {
  // The zig handler builds this envelope with `{s}` inside a string
  // literal rather than through `encodeJsonString`, so an identifier
  // containing a double quote produces MALFORMED JSON. The `issues` array
  // beside it IS escaped.
  //
  // Reproduced under D2 rather than corrected: escaping here would change
  // the bytes a scripted caller parses, on a surface where the two trees
  // must agree. Named as a defect, not asserted as a virtue.
  std::vector<tpl::validation_issue> const issues = {{.json_path = "a", .message = "UnknownField"}};
  auto const                               out    = tpl::validate_issues("bad\"set", "px", "k", "/p", issues, true);
  CHECK(out.find(R"("set":"bad"set")") != std::string::npos);
}

TEST_CASE("validate_summary and not_found_message are BODIES with no prefix or terminator", "[templates][output][validate]") {
  // The cmd layer composes `error: ` and the newline. Oracle stderr was
  // `error: 6 issue(s) in probe/px/broken\n` and
  // `error: template default/github-issues/nosuch not found\n`.
  CHECK(tpl::validate_summary(6, "probe", "px", "broken") == "6 issue(s) in probe/px/broken");
  CHECK(tpl::not_found_message("default", "github-issues", "nosuch") == "template default/github-issues/nosuch not found");
  // Singular still says `issue(s)`.
  CHECK(tpl::validate_summary(1, "s", "y", "k") == "1 issue(s) in s/y/k");
}
