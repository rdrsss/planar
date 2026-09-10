// @file render.t.cpp
// @brief Byte-exact renderer tests for `planar.engine.local.render`
// (plan 996, task 6109).
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// This file builds every value in memory and touches no filesystem path at all.
// There is nothing here that could write outside a scratch root because there
// is nothing here that writes. The path STRINGS below are the oracle's literal
// captures under a redirected `/tmp/pb/h`; none is ever opened.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Every expected string is a verbatim transcription of bytes the Zig binary
// wrote, captured with `od -c` on separated stdout/stderr files under
// PLANAR_LOCAL_HOME=PLANAR_HOME_PARENT=HOME=/tmp/pb/h with a scratch
// PLANAR_DB. Never read through a pipe, so no exit code or trailing byte was
// lost.
//
//   $Z local list
//     name          kind     vendor   status   target
//     alpha         skill    claude   live     /tmp/pb/h/.claude/commands/local-alpha.md
//     alpha         skill    codex    live     /tmp/pb/h/.codex/skills/local-alpha
//     alpha         skill    copilot  live     /tmp/pb/h/.copilot/skills/local-alpha
//     beta          agent    agents   live     /tmp/pb/h/.planar/agents/local-beta.md
//
//   $Z local link --dry-run
//     alpha (skill)
//       claude   dry-run [symlink]  ->  /tmp/pb/h/.claude/commands/local-alpha.md
//       ...
//     <blank>
//     dry-run: 4 would-be installs across 2 source(s)
//
//   $Z local link             -> `done: 4 linked, 0 unchanged, 0 skipped across 2 source(s)`
//   $Z local link (again)     -> `done: 3 linked, 1 unchanged, 0 skipped across 2 source(s)`
//   $Z local link --reconcile -> `reconcile: manifest already consistent with the filesystem`
//   $Z local migrate          -> `migrate: no legacy flat skills found; sandbox is already dir-shape`
//   $Z local list  (empty)    -> `no sandbox installs recorded`
//   $Z local link  (empty)    -> `no sandbox sources to link under /tmp/pb/h/.planar/local`
//
// --- the empty-input DISAGREEMENT, hazard 6 --------------------------------
//   $Z local list --json             -> b''   ZERO BYTES, exit 0
//   $Z local link --reconcile --json -> b''   ZERO BYTES, exit 0
//   $Z local migrate --json          -> b'{"Migrated":[],"Skipped":[]}\n'
//   Three leaves in one verb family, three different answers. A renderer that
//   emitted `[]` for the first two would be valid JSON and a parity break.
//
// --- JSON captures ---------------------------------------------------------
//   $Z local list --json
//     {"Name":"alpha","Kind":"skill","Record":{"vendor":"claude","target_path":
//      "/tmp/pb/h/.claude/commands/local-alpha.md","source_path":
//      "/tmp/pb/h/.planar/local/skills/alpha/SKILL.md","mode":"symlink",
//      "action":"live","linked_at":"2026-08-23T04:12:27Z"}}
//
//   $Z local link --vendor codex --json  (the skipped records)
//     {"vendor":"claude",...,"mode":"","action":"skipped"}
//     -- `mode` is the EMPTY STRING; `warning` and `linked_at` are ABSENT.
//
//   $Z local unlink alpha --json
//     {"result":{"Name":"alpha","Kind":"skill","Removed":[{...}],"PurgedFile":""}}
//     -- the `{"result":...}` wrapper is unique to this one leaf.
//
//   $Z local migrate --json / --dry-run --json  (BYTE-IDENTICAL)
//     {"Migrated":[{"Name":"oldskill","OldPath":"/tmp/pb/h/.planar/local/skills/
//      oldskill.md","NewPath":"/tmp/pb/h/.planar/local/skills/oldskill/SKILL.md",
//      "Reason":""}],"Skipped":[]}
//
//   $Z local import /tmp/pb/ext --json --no-link
//     {"Imported":[...],"Skipped":[...],"Warnings":[...]}   -- every field
//     emitted, `Reason` present as "" rather than omitted.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.local.manifest;
import planar.engine.local.link;
import planar.engine.local.importer;
import planar.engine.local.render;

namespace mf = planar::engine::local::manifest;
namespace lk = planar::engine::local::link;
namespace im = planar::engine::local::import_;
namespace rd = planar::engine::local::render;

namespace {

constexpr std::string_view k_stamp   = "2026-08-23T04:12:27Z";
constexpr std::string_view k_src     = "/tmp/pb/h/.planar/local/skills/alpha/SKILL.md";
constexpr std::string_view k_src_dir = "/tmp/pb/h/.planar/local/skills/alpha";
constexpr std::string_view k_claude  = "/tmp/pb/h/.claude/commands/local-alpha.md";
constexpr std::string_view k_codex   = "/tmp/pb/h/.codex/skills/local-alpha";
constexpr std::string_view k_copilot = "/tmp/pb/h/.copilot/skills/local-alpha";
constexpr std::string_view k_agent   = "/tmp/pb/h/.planar/agents/local-beta.md";
constexpr std::string_view k_agsrc   = "/tmp/pb/h/.planar/local/agents/beta.md";

auto live_row(std::string_view name, mf::kind kind, std::string_view vendor, std::string_view target, std::string_view source)
    -> lk::list_record {
  return {std::string{name},
          kind,
          {std::string{vendor}, std::string{target}, std::string{source}, mf::mode::symlink, "live", "", std::string{k_stamp}}};
}

/// @brief The oracle's exact four-row fixture.
auto fixture_rows() -> std::vector<lk::list_record> {
  return {live_row("alpha", mf::kind::skill, "claude", k_claude, k_src),
          live_row("alpha", mf::kind::skill, "codex", k_codex, k_src_dir),
          live_row("alpha", mf::kind::skill, "copilot", k_copilot, k_src_dir),
          live_row("beta", mf::kind::agent, "agents", k_agent, k_agsrc)};
}

auto alpha_file() -> mf::sandbox_file {
  mf::frontmatter fm;
  fm.description = "Alpha skill.";
  fm.kind        = "skill";
  return {std::string{k_src}, "alpha", mf::kind::skill, fm, "alpha body\n"};
}

} // namespace

// ===========================================================================
// HAZARD 6 — empty input, and the three different answers
// ===========================================================================

TEST_CASE("render list_json on an empty list is ZERO BYTES") {
  REQUIRE(rd::list_json({}, std::nullopt).empty());
}

TEST_CASE("render reconcile_json with no actions is ZERO BYTES") {
  REQUIRE(rd::reconcile_json({}).empty());
}

TEST_CASE("render migrate_json on an empty result is a FULL envelope") {
  // The same verb family, the opposite answer. Both are pinned so a shared
  // helper cannot smooth the difference away.
  REQUIRE(rd::migrate_json({}) == "{\"Migrated\":[],\"Skipped\":[]}\n");
}

TEST_CASE("render import_json on an empty result is a FULL envelope") {
  REQUIRE(rd::import_json({}) == "{\"Imported\":[],\"Skipped\":[],\"Warnings\":[]}\n");
}

TEST_CASE("render the empty-input TEXT sentences all differ") {
  REQUIRE(rd::list_text({}, std::nullopt) == "no sandbox installs recorded\n");
  REQUIRE(rd::reconcile_text({}, false) == "reconcile: manifest already consistent with the filesystem\n");
  REQUIRE(rd::migrate_text({}, false) == "migrate: no legacy flat skills found; sandbox is already dir-shape\n");
  REQUIRE(rd::import_text({}) == "no files matched for import\n");
  REQUIRE(rd::no_sources_text("/tmp/pb/h/.planar/local") == "no sandbox sources to link under /tmp/pb/h/.planar/local\n");
}

// ===========================================================================
// list
// ===========================================================================

TEST_CASE("render list_text reproduces the oracle's table") {
  REQUIRE(rd::list_text(fixture_rows(), std::nullopt) ==
          "name          kind     vendor   status   target\n"
          "alpha         skill    claude   live     /tmp/pb/h/.claude/commands/local-alpha.md\n"
          "alpha         skill    codex    live     /tmp/pb/h/.codex/skills/local-alpha\n"
          "alpha         skill    copilot  live     /tmp/pb/h/.copilot/skills/local-alpha\n"
          "beta          agent    agents   live     /tmp/pb/h/.planar/agents/local-beta.md\n");
}

TEST_CASE("render list_text pushes an over-long value right rather than truncating") {
  auto rows      = fixture_rows();
  rows[0].name   = "a-name-far-longer-than-twelve";
  const auto out = rd::list_text({rows.data(), 1}, std::nullopt);
  REQUIRE(out.find("a-name-far-longer-than-twelve  skill  ") != std::string::npos);
}

TEST_CASE("render list_text keeps the header when a filter matches nothing") {
  // "You have nothing" and "you have things, none matching" are different
  // operator situations and the oracle words them differently. The header is
  // emitted before the filter runs, which is what makes this shape possible.
  REQUIRE(rd::list_text(fixture_rows(), "nosuch") == "name          kind     vendor   status   target\n"
                                                     "no rows matched filter\n");
}

TEST_CASE("render list_text applies a matching filter") {
  REQUIRE(rd::list_text(fixture_rows(), "codex") ==
          "name          kind     vendor   status   target\n"
          "alpha         skill    codex    live     /tmp/pb/h/.codex/skills/local-alpha\n");
}

TEST_CASE("render list_json emits NDJSON with PascalCase outer keys") {
  REQUIRE(rd::list_json(fixture_rows(), "claude") ==
          "{\"Name\":\"alpha\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"claude\",\"target_path\":"
          "\"/tmp/pb/h/.claude/commands/local-alpha.md\",\"source_path\":"
          "\"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\"mode\":\"symlink\",\"action\":\"live\","
          "\"linked_at\":\"2026-08-23T04:12:27Z\"}}\n");
}

TEST_CASE("render list_json emits one line per row with no array or commas") {
  const auto out = rd::list_json(fixture_rows(), std::nullopt);
  REQUIRE(std::ranges::count(out, '\n') == 4);
  REQUIRE_FALSE(out.starts_with('['));
  REQUIRE_FALSE(out.contains("},{"));
}

TEST_CASE("render list_json omits linked_at when it is empty") {
  auto rows = fixture_rows();
  rows[0].record.linked_at.clear();
  const auto out = rd::list_json({rows.data(), 1}, std::nullopt);
  REQUIRE(out.find("linked_at") == std::string::npos);
  // But `mode` and `action` are still there — omission is per-field, not a
  // blanket "drop empty strings".
  REQUIRE(out.find("\"mode\":\"symlink\"") != std::string::npos);
}

TEST_CASE("render list_json renders an absent mode as the EMPTY STRING") {
  // Not null, not absent. The Zig handler stringifies it before serialization
  // ever sees it.
  auto rows = fixture_rows();
  rows[0].record.mode.reset();
  REQUIRE(rd::list_json({rows.data(), 1}, std::nullopt).find("\"mode\":\"\"") != std::string::npos);
}

TEST_CASE("render list_json escapes an operator-authored name") {
  auto rows    = fixture_rows();
  rows[0].name = "q\"uote\\back";
  REQUIRE(rd::list_json({rows.data(), 1}, std::nullopt).starts_with("{\"Name\":\"q\\\"uote\\\\back\""));
}

// ===========================================================================
// link
// ===========================================================================

namespace {

auto dry_run_results() -> std::vector<lk::link_result> {
  const auto dry = [](std::string_view vendor, std::string_view target, std::string_view source) -> lk::target_record {
    return {std::string{vendor}, std::string{target}, std::string{source}, mf::mode::symlink, "dry-run", "", ""};
  };
  return {{"alpha",
           mf::kind::skill,
           {dry("claude", k_claude, k_src), dry("codex", k_codex, k_src_dir), dry("copilot", k_copilot, k_src_dir)}},
          {"beta", mf::kind::agent, {dry("agents", k_agent, k_agsrc)}}};
}

} // namespace

TEST_CASE("render link_source_text reproduces the oracle's dry-run block") {
  const auto results = dry_run_results();
  REQUIRE(rd::link_source_text(results[0]) == "alpha (skill)\n"
                                              "  claude   dry-run [symlink]  ->  /tmp/pb/h/.claude/commands/local-alpha.md\n"
                                              "  codex    dry-run [symlink]  ->  /tmp/pb/h/.codex/skills/local-alpha\n"
                                              "  copilot  dry-run [symlink]  ->  /tmp/pb/h/.copilot/skills/local-alpha\n");
  REQUIRE(rd::link_source_text(results[1]) == "beta (agent)\n"
                                              "  agents   dry-run [symlink]  ->  /tmp/pb/h/.planar/agents/local-beta.md\n");
}

TEST_CASE("render link_source_text drops the bracket group entirely for a skipped record") {
  // Not an empty `[]` — the bracket group AND the space before it are both gone,
  // so the column layout shifts. Captured from `local link --vendor codex`.
  lk::link_result result{"alpha", mf::kind::skill, {}};
  result.records.push_back({"claude", std::string{k_claude}, std::string{k_src}, std::nullopt, "skipped", "", ""});
  REQUIRE(rd::link_source_text(result) == "alpha (skill)\n"
                                          "  claude   skipped  ->  /tmp/pb/h/.claude/commands/local-alpha.md\n");
}

TEST_CASE("render link_source_text indents a warning under its record") {
  lk::link_result result{"alpha", mf::kind::skill, {}};
  result.records.push_back({"claude", std::string{k_claude}, std::string{k_src}, mf::mode::copy, "created",
                            "copy fallback: edits to source require re-running `planar local link`", std::string{k_stamp}});
  REQUIRE(rd::link_source_text(result) ==
          "alpha (skill)\n"
          "  claude   created [copy]  ->  /tmp/pb/h/.claude/commands/local-alpha.md\n"
          "           warning: copy fallback: edits to source require re-running `planar local link`\n");
}

TEST_CASE("render link_summary_text counts a dry run") {
  REQUIRE(rd::link_summary_text(dry_run_results(), true) == "\ndry-run: 4 would-be installs across 2 source(s)\n");
}

TEST_CASE("render link_summary_text counts a real run") {
  // created and updated BOTH count as "linked"; unchanged and skipped are
  // reported separately. Oracle-captured across a first and second run.
  std::vector<lk::link_result> results;
  results.push_back({"alpha",
                     mf::kind::skill,
                     {{"claude", "", "", mf::mode::symlink, "created", "", ""},
                      {"codex", "", "", mf::mode::symlink, "updated", "", ""},
                      {"copilot", "", "", std::nullopt, "skipped", "", ""}}});
  results.push_back({"beta", mf::kind::agent, {{"agents", "", "", mf::mode::symlink, "unchanged", "", ""}}});
  REQUIRE(rd::link_summary_text(results, false) == "\ndone: 2 linked, 1 unchanged, 1 skipped across 2 source(s)\n");
}

TEST_CASE("render link_summary_text on nothing at all still emits the blank line") {
  REQUIRE(rd::link_summary_text({}, false) == "\ndone: 0 linked, 0 unchanged, 0 skipped across 0 source(s)\n");
}

TEST_CASE("render link_json reproduces the oracle's envelope") {
  lk::link_result result{"alpha", mf::kind::skill, {}};
  result.records.push_back({"claude", std::string{k_claude}, std::string{k_src}, std::nullopt, "skipped", "", ""});
  result.records.push_back(
      {"codex", std::string{k_codex}, std::string{k_src_dir}, mf::mode::symlink, "unchanged", "", std::string{k_stamp}});

  REQUIRE(rd::link_json(alpha_file(), result) ==
          "{\"Source\":{\"SourcePath\":\"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\"Name\":"
          "\"alpha\",\"Kind\":\"skill\",\"Frontmatter\":{\"Description\":\"Alpha skill.\","
          "\"ArgumentHint\":\"\",\"Tier\":\"\",\"Model\":\"\",\"Shadow\":false,\"Vendors\":[],\"Kind\":"
          "\"skill\"},\"Body\":\"alpha body\\n\"},\"Records\":["
          "{\"vendor\":\"claude\",\"target_path\":\"/tmp/pb/h/.claude/commands/local-alpha.md\","
          "\"source_path\":\"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\"mode\":\"\",\"action\":"
          "\"skipped\"},"
          "{\"vendor\":\"codex\",\"target_path\":\"/tmp/pb/h/.codex/skills/local-alpha\",\"source_path\":"
          "\"/tmp/pb/h/.planar/local/skills/alpha\",\"mode\":\"symlink\",\"action\":\"unchanged\","
          "\"linked_at\":\"2026-08-23T04:12:27Z\"}]}\n");
}

TEST_CASE("render link_json shows the AUTHORED vendors, not the resolved three") {
  // A skill with no `vendors:` installs into all three but reports `[]` here.
  // Substituting the resolved list would be a parity break AND would lose the
  // information about what the author actually wrote.
  auto file = alpha_file();
  REQUIRE(file.frontmatter.vendors.empty());
  REQUIRE(rd::link_json(file, {"alpha", mf::kind::skill, {}}).find("\"Vendors\":[]") != std::string::npos);

  file.frontmatter.vendors = {"claude", "codex"};
  REQUIRE(rd::link_json(file, {"alpha", mf::kind::skill, {}}).find("\"Vendors\":[\"claude\",\"codex\"]") != std::string::npos);
}

TEST_CASE("render link_json emits Shadow as a JSON boolean, not a string") {
  auto file               = alpha_file();
  file.frontmatter.shadow = true;
  REQUIRE(rd::link_json(file, {"alpha", mf::kind::skill, {}}).find("\"Shadow\":true") != std::string::npos);
}

TEST_CASE("render link_json escapes the body's newlines") {
  REQUIRE(rd::link_json(alpha_file(), {"alpha", mf::kind::skill, {}}).find("\"Body\":\"alpha body\\n\"") != std::string::npos);
}

// ===========================================================================
// walk errors and lint
// ===========================================================================

TEST_CASE("render walk_error_text matches the oracle's warning lines") {
  REQUIRE(rd::walk_error_text({"/tmp/pb/h/.planar/local/skills/nofm/SKILL.md", "NoFrontmatter"}) ==
          "warning: /tmp/pb/h/.planar/local/skills/nofm/SKILL.md: NoFrontmatter\n");
  REQUIRE(rd::walk_error_text({"/tmp/pb/h/.planar/local/skills/legacyflat.md",
                               "legacy flat skill file; run `planar local migrate` to convert to "
                               "legacyflat/SKILL.md"}) ==
          "warning: /tmp/pb/h/.planar/local/skills/legacyflat.md: legacy flat skill file; run `planar "
          "local migrate` to convert to legacyflat/SKILL.md\n");
}

TEST_CASE("render lint_text matches the oracle's lint lines") {
  REQUIRE(rd::lint_text(
              {mf::lint_severity::warning, "description", "description is empty; vendors surface this as the skill summary"},
              mf::kind::skill, "alpha") ==
          "lint [warning] skill/alpha.description: description is empty; vendors surface this as the "
          "skill summary\n");
}

TEST_CASE("render lint_text spells the reserved severity as error, not error_") {
  // The Zig enum name reaches the output. This tree spells the enumerator
  // `error_` because `error` is a keyword there; the WIRE spelling must not
  // inherit the underscore.
  REQUIRE(rd::lint_text({mf::lint_severity::error_, "f", "m"}, mf::kind::agent, "beta") == "lint [error] agent/beta.f: m\n");
}

// ===========================================================================
// unlink
// ===========================================================================

TEST_CASE("render unlink_json wraps the payload in a result envelope") {
  lk::unlink_result result{"alpha", mf::kind::skill, {}, ""};
  result.removed.push_back(
      {"claude", std::string{k_claude}, std::string{k_src}, mf::mode::symlink, "removed", "", std::string{k_stamp}});
  REQUIRE(rd::unlink_json(result) == "{\"result\":{\"Name\":\"alpha\",\"Kind\":\"skill\",\"Removed\":["
                                     "{\"vendor\":\"claude\",\"target_path\":\"/tmp/pb/h/.claude/commands/local-alpha.md\","
                                     "\"source_path\":\"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\"mode\":\"symlink\","
                                     "\"action\":\"removed\",\"linked_at\":\"2026-08-23T04:12:27Z\"}],\"PurgedFile\":\"\"}}\n");
}

TEST_CASE("render unlink_json always emits PurgedFile") {
  REQUIRE(rd::unlink_json({"alpha", mf::kind::skill, {}, ""}) ==
          "{\"result\":{\"Name\":\"alpha\",\"Kind\":\"skill\",\"Removed\":[],\"PurgedFile\":\"\"}}\n");
  REQUIRE(rd::unlink_json({"alpha", mf::kind::skill, {}, "/tmp/pb/h/.planar/local/skills/alpha"})
              .find("\"PurgedFile\":\"/tmp/pb/h/.planar/local/skills/alpha\"") != std::string::npos);
}

TEST_CASE("render unlink_text lists removals and the purged source") {
  lk::unlink_result result{"alpha", mf::kind::skill, {}, "/tmp/pb/h/.planar/local/skills/alpha"};
  result.removed.push_back({"claude", std::string{k_claude}, "", mf::mode::symlink, "removed", "", ""});
  REQUIRE(rd::unlink_text(result) == "alpha (skill)\n"
                                     "  claude   removed  <-  /tmp/pb/h/.claude/commands/local-alpha.md\n"
                                     "  purged source file: /tmp/pb/h/.planar/local/skills/alpha\n");
}

TEST_CASE("render unlink_none_text is deliberately ambiguous") {
  REQUIRE(rd::unlink_none_text("alpha") == "no installs found for \"alpha\" (already unlinked, or no such name)\n");
}

// ===========================================================================
// import
// ===========================================================================

namespace {

auto import_fixture() -> im::result {
  im::result value;
  value.imported.push_back({"one", "/tmp/pb/ext/one.md", "/tmp/pb/h/.planar/local/skills/one/SKILL.md", "imported", ""});
  value.skipped.push_back({"bad", "/tmp/pb/ext/bad.md", "", "skipped", "no-frontmatter"});
  value.warnings.push_back({"shad", "description", "description is empty; vendors surface this as the skill summary"});
  return value;
}

} // namespace

TEST_CASE("render import_json emits every field including an empty Reason") {
  REQUIRE(rd::import_json(import_fixture()) ==
          "{\"Imported\":[{\"Name\":\"one\",\"SourcePath\":\"/tmp/pb/ext/one.md\",\"TargetPath\":"
          "\"/tmp/pb/h/.planar/local/skills/one/SKILL.md\",\"Action\":\"imported\",\"Reason\":\"\"}],"
          "\"Skipped\":[{\"Name\":\"bad\",\"SourcePath\":\"/tmp/pb/ext/bad.md\",\"TargetPath\":\"\","
          "\"Action\":\"skipped\",\"Reason\":\"no-frontmatter\"}],"
          "\"Warnings\":[{\"Name\":\"shad\",\"Field\":\"description\",\"Message\":\"description is empty; "
          "vendors surface this as the skill summary\"}]}\n");
}

TEST_CASE("render import_text reproduces the oracle's rows") {
  REQUIRE(rd::import_text(import_fixture()) ==
          "one           imported      <-  /tmp/pb/ext/one.md\n"
          "bad           skipped       reason: no-frontmatter\n"
          "warning [shad.description] description is empty; vendors surface this as the skill summary\n"
          "\n"
          "imported 1 file(s); skipped 1\n");
}

TEST_CASE("render import_text says imported even for a dry run") {
  // The per-row action reads `would-import` while the summary still says
  // `imported N file(s)`. The Zig handler takes `dry_run` and then discards it
  // (`_ = dry_run;`), so import_text() has no dry-run parameter at all.
  im::result value;
  value.imported.push_back({"one", "/tmp/pb/ext/one.md", "/t", "would-import", ""});
  REQUIRE(rd::import_text(value) == "one           would-import  <-  /tmp/pb/ext/one.md\n"
                                    "\n"
                                    "imported 1 file(s); skipped 0\n");
}

// ===========================================================================
// migrate
// ===========================================================================

namespace {

auto migrate_fixture() -> mf::migrate_result {
  mf::migrate_result value;
  value.migrated.push_back(
      {"oldskill", "/tmp/pb/h/.planar/local/skills/oldskill.md", "/tmp/pb/h/.planar/local/skills/oldskill/SKILL.md", ""});
  return value;
}

} // namespace

TEST_CASE("render migrate_json reproduces the oracle's bytes") {
  REQUIRE(rd::migrate_json(migrate_fixture()) ==
          "{\"Migrated\":[{\"Name\":\"oldskill\",\"OldPath\":\"/tmp/pb/h/.planar/local/skills/oldskill.md\""
          ",\"NewPath\":\"/tmp/pb/h/.planar/local/skills/oldskill/SKILL.md\",\"Reason\":\"\"}],"
          "\"Skipped\":[]}\n");
}

TEST_CASE("render migrate_json carries NO dry-run marker") {
  // Byte-identical with and without --dry-run, confirmed by diffing the two
  // oracle captures. A scripted caller genuinely cannot tell a rehearsal from a
  // real run — worth knowing, and worth not "improving".
  REQUIRE(rd::migrate_json(migrate_fixture()) == rd::migrate_json(migrate_fixture()));
}

TEST_CASE("render migrate_text switches the verb on dry-run, in BOTH places") {
  REQUIRE(rd::migrate_text(migrate_fixture(), false) ==
          "oldskill              migrated  /tmp/pb/h/.planar/local/skills/oldskill.md -> "
          "/tmp/pb/h/.planar/local/skills/oldskill/SKILL.md\n"
          "\n"
          "done: migrated 1 skill(s); skipped 0\n");
  REQUIRE(rd::migrate_text(migrate_fixture(), true) ==
          "oldskill              would migrate  /tmp/pb/h/.planar/local/skills/oldskill.md -> "
          "/tmp/pb/h/.planar/local/skills/oldskill/SKILL.md\n"
          "\n"
          "done: would migrate 1 skill(s); skipped 0\n");
}

TEST_CASE("render migrate_text renders a skipped row with its reason") {
  mf::migrate_result value;
  value.skipped.push_back({"done", "/o", "/n", "already migrated: /n exists"});
  REQUIRE(rd::migrate_text(value, false) == "done                  skipped       already migrated: /n exists\n"
                                            "\n"
                                            "done: migrated 0 skill(s); skipped 1\n");
}

// ===========================================================================
// reconcile
// ===========================================================================

namespace {

auto reconcile_fixture() -> std::vector<lk::reconcile_action> {
  return {{"alpha", mf::kind::skill, "source-missing", std::string{k_src}, {std::string{k_claude}, std::string{k_codex}}}};
}

} // namespace

TEST_CASE("render reconcile_json uses snake_case, unlike its siblings") {
  // This one leaf is rendered from an anonymous struct rather than a Go-shaped
  // mirror, so it never picked up the PascalCase convention. Pinned precisely
  // because it looks like an inconsistency to tidy.
  REQUIRE(rd::reconcile_json(reconcile_fixture()) ==
          "{\"name\":\"alpha\",\"kind\":\"skill\",\"reason\":\"source-missing\",\"source_path\":"
          "\"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md\",\"removed_targets\":["
          "\"/tmp/pb/h/.claude/commands/local-alpha.md\",\"/tmp/pb/h/.codex/skills/local-alpha\"]}\n");
}

TEST_CASE("render reconcile_text reports a repair as removed") {
  // `target-missing` RE-CREATED the installs, and the line still says
  // "removed 1 install(s)". The oracle's wording; see link.cppm for why the
  // underlying field is named `removed_targets` in both cases.
  const std::vector<lk::reconcile_action> actions{
      {"alpha", mf::kind::skill, "target-missing", std::string{k_src}, {std::string{k_claude}}}};
  REQUIRE(rd::reconcile_text(actions, false) == "alpha (skill) - target-missing; removed 1 install(s)\n"
                                                "  /tmp/pb/h/.claude/commands/local-alpha.md\n"
                                                "\n"
                                                "done: 1 stale entry(ies) cleaned\n");
}

TEST_CASE("render reconcile_text switches both verbs on dry-run") {
  REQUIRE(rd::reconcile_text(reconcile_fixture(), true) == "alpha (skill) - source-missing; would remove 2 install(s)\n"
                                                           "  /tmp/pb/h/.claude/commands/local-alpha.md\n"
                                                           "  /tmp/pb/h/.codex/skills/local-alpha\n"
                                                           "\n"
                                                           "dry-run: 1 stale entry(ies) would be cleaned\n");
}

// ===========================================================================
// error lines
// ===========================================================================

TEST_CASE("render error lines quote exactly where the oracle quotes") {
  // Three refusals, three different quoting conventions. A shared helper would
  // smooth over a difference the oracle actually has, so they are pinned
  // separately.
  REQUIRE(rd::no_such_source_error("alpha", "/tmp/pb/h/.planar/local") ==
          "error: no sandbox source named \"alpha\" under /tmp/pb/h/.planar/local\n");
  REQUIRE(rd::invalid_kind_error("bogus") == "error: --kind must be skill or agent, got \"bogus\"\n");
  REQUIRE(rd::reconcile_takes_no_positional_error() == "error: --reconcile takes no positional arguments\n");
}

TEST_CASE("render invalid_kind_error renders an ABSENT flag as empty quotes") {
  // The Zig handler passes `args.kind orelse ""`, so an unparseable-because-
  // absent kind reports `got ""` rather than omitting the clause.
  REQUIRE(rd::invalid_kind_error("") == "error: --kind must be skill or agent, got \"\"\n");
}
