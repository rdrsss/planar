// @file render.t.cpp
// @brief Byte-exact renderer tests for `planar.engine.workflows.render`
// (plan 996, task 6096).
//
// HOME SAFETY. This file constructs entries in memory and touches no
// filesystem path at all -- there is nothing here that could write outside a
// scratch root because there is nothing here that writes.
//
// ORACLE PROVENANCE. Every expected string is a VERBATIM transcription of
// bytes the Zig binary wrote, read back via
// `python3 -c "print(repr(open(f,'rb').read()))"` so no shell echo or
// terminal could alter them. Fixture: PLANAR_HOME=/tmp/op/wfhome with
// shipped finalize_closeout.lua / bare.lua / partial.lua / notes.txt and
// sandbox sandbox_one.lua.
//
//   $Z workflow list
//     b'name                      kind      phases                description\n
//       aaa-partial               shipped                         Only two fields.\n
//       bare                      shipped                         \n
//       finalize-closeout         shipped   closeout              Deterministic closeout gate.\n
//       mmm-sandbox               local     build,test            A sandbox workflow.\n'
//
//   $Z workflow list --json          [NDJSON -- one object per line, NO array]
//     {"name":"aaa-partial","kind":"shipped","path":"/tmp/op/wfhome/workflows/partial.lua",
//      "filename":"partial.lua","meta_found":true,"description":"Only two fields.",
//      "phases":"","seam":""}
//     {"name":"bare","kind":"shipped","path":"/tmp/op/wfhome/workflows/bare.lua",
//      "filename":"bare.lua","meta_found":false,"description":"","phases":"","seam":""}
//     ...
//
//   $Z workflow show finalize-closeout
//     name:        finalize-closeout
//     kind:        shipped
//     path:        /tmp/op/wfhome/workflows/finalize_closeout.lua
//     meta:        present
//     description: Deterministic closeout gate.
//     phases:      closeout
//     seam:        planar run start/event/finish, planar plan closeout
//
//   $Z workflow show bare --json
//     {"name":"bare",...,"meta_found":false,"description":"","phases":"","seam":""}
//
// --- HAZARD 6, empty input -------------------------------------------------
//   PLANAR_HOME with no workflows/ directory at all:
//   $Z workflow list        -> b'no shipped + sandbox workflows found\n'
//   $Z workflow list --json -> b''            <-- ZERO BYTES, not "[]"
//
// --- HAZARD 6, the shared not-found line -----------------------------------
// The brief warns that two leaves reporting "not found" with different
// quoting is what a careless shared helper gets wrong. Both were captured:
//   $Z workflow show nope --json   -> exit 1, "error: workflow 'nope' not found"
//   $Z workflow run nope --phase x -> exit 1, "error: workflow 'nope' not found"
// Identical, including the single quotes -- so ONE function serves both, and
// this file pins it against both captures.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workflows.catalog;
import planar.engine.workflows.render;

namespace {

namespace cat = planar::engine::workflows::catalog;
namespace rd  = planar::engine::workflows::render;

/// @brief The four entries the oracle transcript above lists, in its order.
auto fixture_entries() -> std::vector<cat::entry> {
  return {
      cat::entry{.path       = "/tmp/op/wfhome/workflows/partial.lua",
                 .filename   = "partial.lua",
                 .meta       = {.name = "aaa-partial", .description = "Only two fields."},
                 .meta_found = true,
                 .is_local   = false},
      cat::entry{.path       = "/tmp/op/wfhome/workflows/bare.lua",
                 .filename   = "bare.lua",
                 .meta       = {},
                 .meta_found = false,
                 .is_local   = false},
      cat::entry{.path       = "/tmp/op/wfhome/workflows/finalize_closeout.lua",
                 .filename   = "finalize_closeout.lua",
                 .meta       = {.name        = "finalize-closeout",
                                .description = "Deterministic closeout gate.",
                                .phases      = "closeout",
                                .seam        = "planar run start/event/finish, planar plan closeout"},
                 .meta_found = true,
                 .is_local   = false},
      cat::entry{.path       = "/tmp/op/wfhome/local/workflows/sandbox_one.lua",
                 .filename   = "sandbox_one.lua",
                 .meta       = {.name = "mmm-sandbox", .description = "A sandbox workflow.", .phases = "build,test"},
                 .meta_found = true,
                 .is_local   = true},
  };
}

} // namespace

TEST_CASE("workflows.render: the list table matches the oracle byte for byte", "[workflows]") {
  // Column widths (24 / 8 / 20, each followed by TWO spaces), the trailing
  // space on `bare`'s otherwise-empty phases column, and the unpadded
  // description are all part of this comparison.
  REQUIRE(rd::list_text(fixture_entries(), false) ==
          "name                      kind      phases                description\n"
          "aaa-partial               shipped                         Only two fields.\n"
          "bare                      shipped                         \n"
          "finalize-closeout         shipped   closeout              Deterministic closeout gate.\n"
          "mmm-sandbox               local     build,test            A sandbox workflow.\n");
}

TEST_CASE("workflows.render: an over-long name pushes the row right, never truncates", "[workflows]") {
  // Oracle-verified with a 48-character name: the row grows rather than the
  // name being cut. Truncating would produce a name that does not resolve
  // when pasted into `workflow show`.
  std::vector<cat::entry> wide{
      cat::entry{.path       = "/x/longname.lua",
                 .filename   = "longname.lua",
                 .meta       = {.name = "an-extremely-long-workflow-name-beyond-the-column", .description = "D", .phases = "p"},
                 .meta_found = true,
                 .is_local   = false}};
  REQUIRE(rd::list_text(wide, false) == "name                      kind      phases                description\n"
                                        "an-extremely-long-workflow-name-beyond-the-column  shipped   p                     D\n");
}

TEST_CASE("workflows.render: an empty catalog names which sources were searched", "[workflows]") {
  // HAZARD 6. The two sentences differ, so an operator who passed --local is
  // not left wondering whether shipped workflows exist.
  REQUIRE(rd::list_text({}, false) == "no shipped + sandbox workflows found\n");
  REQUIRE(rd::list_text({}, true) == "no sandbox workflows found\n");
}

TEST_CASE("workflows.render: list --json is NDJSON, and empty means ZERO BYTES", "[workflows]") {
  const auto entries = fixture_entries();
  const auto json    = rd::list_json(entries);

  // No enclosing array, no separating commas.
  REQUIRE_FALSE(json.starts_with("["));
  REQUIRE_FALSE(json.ends_with("]\n"));
  REQUIRE(json.starts_with("{\"name\":\"aaa-partial\""));
  REQUIRE(json.ends_with("}\n"));
  // Exactly one line per entry.
  REQUIRE(std::ranges::count(json, '\n') == 4);
  REQUIRE(json.find("}{") == std::string::npos);
  REQUIRE(json.find("},{") == std::string::npos);

  // HAZARD 6: an empty catalog under --json is the EMPTY STRING. A renderer
  // emitting "[]" here would be valid JSON and a parity break.
  REQUIRE(rd::list_json({}).empty());

  REQUIRE(json == "{\"name\":\"aaa-partial\",\"kind\":\"shipped\","
                  "\"path\":\"/tmp/op/wfhome/workflows/partial.lua\",\"filename\":\"partial.lua\","
                  "\"meta_found\":true,\"description\":\"Only two fields.\",\"phases\":\"\",\"seam\":\"\"}\n"
                  "{\"name\":\"bare\",\"kind\":\"shipped\",\"path\":\"/tmp/op/wfhome/workflows/bare.lua\","
                  "\"filename\":\"bare.lua\",\"meta_found\":false,\"description\":\"\",\"phases\":\"\","
                  "\"seam\":\"\"}\n"
                  "{\"name\":\"finalize-closeout\",\"kind\":\"shipped\","
                  "\"path\":\"/tmp/op/wfhome/workflows/finalize_closeout.lua\","
                  "\"filename\":\"finalize_closeout.lua\",\"meta_found\":true,"
                  "\"description\":\"Deterministic closeout gate.\",\"phases\":\"closeout\","
                  "\"seam\":\"planar run start/event/finish, planar plan closeout\"}\n"
                  "{\"name\":\"mmm-sandbox\",\"kind\":\"local\","
                  "\"path\":\"/tmp/op/wfhome/local/workflows/sandbox_one.lua\","
                  "\"filename\":\"sandbox_one.lua\",\"meta_found\":true,"
                  "\"description\":\"A sandbox workflow.\",\"phases\":\"build,test\",\"seam\":\"\"}\n");
}

TEST_CASE("workflows.render: show --json is the SAME payload as one list line", "[workflows]") {
  // Oracle-confirmed identical byte for byte -- `workflow show X --json` and
  // the matching line of `workflow list --json` are the same object. One
  // renderer serves both, so they cannot drift.
  const auto entries = fixture_entries();
  const auto listed  = rd::list_json(std::span{entries}.subspan(2, 1));
  REQUIRE(rd::entry_json(entries[2]) == listed);
  REQUIRE(rd::entry_json(entries[2]) == "{\"name\":\"finalize-closeout\",\"kind\":\"shipped\","
                                        "\"path\":\"/tmp/op/wfhome/workflows/finalize_closeout.lua\","
                                        "\"filename\":\"finalize_closeout.lua\",\"meta_found\":true,"
                                        "\"description\":\"Deterministic closeout gate.\",\"phases\":\"closeout\","
                                        "\"seam\":\"planar run start/event/finish, planar plan closeout\"}\n");

  // A metadata-less workflow reports meta_found:false with empty (NOT null)
  // strings for the three optional fields.
  REQUIRE(rd::entry_json(entries[1]) ==
          "{\"name\":\"bare\",\"kind\":\"shipped\",\"path\":\"/tmp/op/wfhome/workflows/bare.lua\","
          "\"filename\":\"bare.lua\",\"meta_found\":false,\"description\":\"\",\"phases\":\"\",\"seam\":\"\"}\n");
}

TEST_CASE("workflows.render: show text omits empty fields entirely", "[workflows]") {
  const auto entries = fixture_entries();
  REQUIRE(rd::show_text(entries[2]) == "name:        finalize-closeout\n"
                                       "kind:        shipped\n"
                                       "path:        /tmp/op/wfhome/workflows/finalize_closeout.lua\n"
                                       "meta:        present\n"
                                       "description: Deterministic closeout gate.\n"
                                       "phases:      closeout\n"
                                       "seam:        planar run start/event/finish, planar plan closeout\n");

  // A metadata-less workflow renders as FOUR lines. A bare `description:`
  // would claim the field exists and is blank, which is a different statement
  // from "this workflow declares no description".
  REQUIRE(rd::show_text(entries[1]) == "name:        bare\n"
                                       "kind:        shipped\n"
                                       "path:        /tmp/op/wfhome/workflows/bare.lua\n"
                                       "meta:        absent\n");

  // A partial block prints only the fields it has, and `kind: local` for a
  // sandbox workflow.
  REQUIRE(rd::show_text(entries[3]) == "name:        mmm-sandbox\n"
                                       "kind:        local\n"
                                       "path:        /tmp/op/wfhome/local/workflows/sandbox_one.lua\n"
                                       "meta:        present\n"
                                       "description: A sandbox workflow.\n"
                                       "phases:      build,test\n");
  REQUIRE(rd::show_text(entries[3]).find("seam:") == std::string::npos);

  // `meta: present` with no fields at all still prints the four header rows.
  cat::entry empty_meta{.path = "/x/e.lua", .filename = "e.lua", .meta = {}, .meta_found = true, .is_local = false};
  REQUIRE(rd::show_text(empty_meta) == "name:        e\n"
                                       "kind:        shipped\n"
                                       "path:        /x/e.lua\n"
                                       "meta:        present\n");
}

TEST_CASE("workflows.render: the not-found line is identical for show and run", "[workflows]") {
  // HAZARD 6. `workflow show nope` and `workflow run nope` were BOTH captured
  // and both emit exactly this, single quotes included -- so one function
  // serves both and they cannot drift.
  REQUIRE(rd::not_found_error("nope") == "error: workflow 'nope' not found\n");
  REQUIRE(rd::not_found_error("with space") == "error: workflow 'with space' not found\n");

  // The empty name is NOT an oracle capture and the comment here used to
  // claim it was (review finding F9). Re-probed against the live binary:
  //
  //   $Z workflow show ""   -> exit 2, `error: MissingRequiredPositional`
  //
  // The parser rejects an empty positional before the catalog is ever
  // consulted, so this leaf can never reach not_found_error(""). The
  // assertion is kept because it pins a real property of a PURE function --
  // the quotes are unconditional, so an empty name still renders as `''`
  // rather than collapsing to `error: workflow  not found` -- but it is
  // labelled as a property, not as provenance.
  REQUIRE(rd::not_found_error("") == "error: workflow '' not found\n");
}

TEST_CASE("workflows.render: JSON escaping survives a hostile path or description", "[workflows]") {
  // A workflow's path and description are arbitrary operator-authored data: a
  // Windows-style path carries backslashes and a description can hold quotes,
  // both of which would produce invalid JSON unescaped.
  cat::entry hostile{.path       = "C:\\wf\\a\"b.lua",
                     .filename   = "a\"b.lua",
                     .meta       = {.name = "q\"n", .description = "line1\nline2\ttabbed", .phases = "p/q"},
                     .meta_found = true,
                     .is_local   = false};
  const auto json = rd::entry_json(hostile);
  REQUIRE(json.find("\"name\":\"q\\\"n\"") != std::string::npos);
  REQUIRE(json.find("\"path\":\"C:\\\\wf\\\\a\\\"b.lua\"") != std::string::npos);
  REQUIRE(json.find("\"description\":\"line1\\nline2\\ttabbed\"") != std::string::npos);
  // `/` stays bare, matching std.json's default.
  REQUIRE(json.find("\"phases\":\"p/q\"") != std::string::npos);

  // Control bytes become LOWERCASE \u00xx. Chosen to include bytes whose hex
  // spelling contains a LETTER (0x0b, 0x1f) -- 0x01 alone would pass under an
  // uppercase implementation too, since `\u0001` has no letter to get wrong.
  cat::entry ctl{.path       = "/x/c.lua",
                 .filename   = "c.lua",
                 .meta       = {.name        = "c",
                                .description = std::string{"a\x0b"
                                                           "b\x1f"
                                                           "c"}},
                 .meta_found = true,
                 .is_local   = false};
  REQUIRE(rd::entry_json(ctl).find("\"description\":\"a\\u000bb\\u001fc\"") != std::string::npos);
}
