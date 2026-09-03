// @file parse.t.cpp
// @brief Unit tests for `planar.engine.workbench.parse` (plan 996, task 6037).
//
// THIS FILE IS THE REJECTION BOUNDARY. `workbench pull` refuses a file whose
// front matter does not parse, so what the parser REFUSES is as much the
// contract as what it accepts, and every case below was derived by RUNNING
// the oracle over a corpus of deliberately malformed files -- never by
// reading parse.zig and never from `--help`.
//
// ORACLE PROVENANCE
//
//   W=/tmp/wb/o1
//   mkdir -p $W/proj $W/home $W/fakehome $W/wb
//   Z=./zig/zig-out/bin/planar
//   o() { cd $W/proj && env PLANAR_DB=$W/planar.db PLANAR_HOME=$W/home \
//           PLANAR_CONFIG_PATH=$W/config.toml PLANAR_LOCAL_HOME=$W/localhome \
//           PLANAR_WORKBENCH_ROOT=$W/wb HOME=$W/fakehome $Z "$@"; }
//   o init --name demo --slug demo
//   o assoc create project:demo --kind project
//   o assoc add project:demo $W/proj
//   o plan create "Demo Feature" --slug demo-feature      -> plan 1
//   # then, for each fixture file:
//   o workbench lint --path <file> --json ; echo $?
//
// `cd` FIRST, then `env`. `VAR=x cd dir && binary` does NOT export the
// assignments past the `&&` in this platform's /bin/sh; getting that wrong
// is how two earlier cycles wrote into the operator's live database.
//
// 59 fixtures were run through BOTH binaries in identical pinned arenas and
// diffed on stdout, stderr and exit code. All 59 matched. The subset pinned
// below is the one that discriminates -- each case names a distinct branch.
//
// --- REFUSED (exit 1, `code` as named) -----------------------------------
//
//   no_open          malformed_frontmatter  L1  opening delimiter is missing
//   crlf_open        malformed_frontmatter  L1  ...  (a CRLF file is refused
//                                                OUTRIGHT: it does not start
//                                                with the four bytes "---\n")
//   bom              malformed_frontmatter  L1  ...  (same, for a UTF-8 BOM)
//   empty            malformed_frontmatter  L1  ...
//   no_close         malformed_frontmatter  L5  closing delimiter is missing
//   just_dashes      malformed_frontmatter  L2  closing delimiter is missing
//   tab_indent       malformed_frontmatter  L3  tab indentation ...
//   unquoted_colon   malformed_frontmatter  L4  an unquoted scalar contains ': '
//   dash_scalar      malformed_frontmatter  L4  ... begins with '- '
//   orphan_dash      malformed_frontmatter  L4  ... begins with '- '
//   indented_list    malformed_frontmatter  L8  YAML syntax is invalid
//   unbalanced_quote malformed_frontmatter  L4  YAML syntax is invalid
//   no_colon_line    malformed_frontmatter  L4  YAML syntax is invalid
//   empty_key        malformed_frontmatter  L4  YAML syntax is invalid
//   bad_int          malformed_frontmatter  L3  an integer field is invalid
//   id_hex           malformed_frontmatter  L3  ... (0x10 under base 10)
//   bad_priority     malformed_frontmatter  L6  ... (field: priority)
//   bad_ref          malformed_frontmatter  L7  an entity reference is invalid
//   bad_ref_zero     malformed_frontmatter  L7  ... (task:0 -- id must be > 0)
//   missing_kind     missing_required_field L2  entity_kind
//   missing_id       missing_required_field L3  entity_id
//   zero_id          missing_required_field L3  entity_id  (0 reads as MISSING)
//   neg_int          missing_required_field L3  entity_id  (-5 likewise)
//   missing_title    missing_required_field L5  title
//   empty_title      missing_required_field L5  title (an empty value == absent)
//   missing_status   missing_required_field L6  status
//   artifact_no_kind missing_required_field L7  artifact_kind
//   bad_kind         invalid_entity_kind    L2
//   scenario_alias   invalid_entity_kind    L2  (`test_scenario` is NOT accepted
//                                                here, though sync aliases it)
//   bad_status       invalid_field_value    L5  status
//   artifact_bad_kind invalid_field_value   L6  artifact_kind
//
// --- ACCEPTED (exit 0, or only the unrelated anchor_plan_not_found warning)
//
//   close_at_eof, no_space_colon, spaced_key, blank_in_fm, dquote,
//   quoted_colon, colon_no_space, trailing_colon, trailing_ws, dup_key,
//   unknown_key, touches_free, derives_dash, list_after_scalar,
//   dashes_in_body, double_wrapped, underscore_int (1_0), dbl_us (1__0),
//   id_plus (+5), maxint
//
// --- the integer grammar, probed at its edges ----------------------------
//   1_0 -> 10   1__0 -> 10   +5 -> 5   9223372036854775807 -> ok
//   _1 / 1_ / + / _ / "1 2" / 0x10 / 9223372036854775808 -> refused
//
// Include-before-import per db.t.cpp / plan.t.cpp precedent.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.parse;

namespace {

namespace wp = planar::engine::workbench::parse;

using wp::diagnostic_reason;
using wp::parse_error_kind;

/// @brief The diagnostic for `content`, requiring that there IS one.
auto refused(std::string_view content) -> wp::diagnostic {
  auto const bad = wp::diagnose(content);
  REQUIRE(bad.has_value());
  // `parse` and `diagnose` share the gate; a file one refuses the other
  // must refuse too, with the same category. Checked on every refusal
  // rather than in one place, because a divergence here means `lint`
  // passes a file `pull` rejects.
  auto const parsed = wp::parse(content);
  REQUIRE_FALSE(parsed.has_value());
  CHECK(parsed.error() == bad->err);
  return *bad;
}

/// @brief The parse result for `content`, requiring that it is accepted.
///
/// CALLER OWNS `content` FOR THE LIFETIME OF THE RESULT. `parse_result::body`
/// is a `string_view` BORROWED from `content` (and a `diagnostic`'s `field`
/// can be too, for the invalid-integer case), so a caller that passes a
/// `std::format` temporary reads freed memory the moment the full-expression
/// ends. That is not hypothetical -- it is how the first draft of the two
/// body tests below failed, comparing against a buffer of spaces. Every
/// non-literal fixture in this file is therefore bound to a named
/// `std::string const` first.
auto accepted(std::string_view content) -> wp::parse_result {
  auto const bad = wp::diagnose(content);
  if (bad.has_value()) {
    FAIL(std::format("expected acceptance, got reason {} on line {}", static_cast<int>(bad->reason), bad->line));
  }
  auto parsed = wp::parse(content);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

constexpr std::string_view k_minimal_task = "---\n"
                                            "entity_kind: task\n"
                                            "entity_id: 1\n"
                                            "anchor_plan_id: 1\n"
                                            "title: T\n"
                                            "status: todo\n"
                                            "---\n";

} // namespace

// --- delimiters -----------------------------------------------------------

TEST_CASE("a file with no opening delimiter is refused at line 1", "[workbench][parse][reject]") {
  auto const bad = refused("entity_kind: task\nentity_id: 1\n");
  CHECK(bad.err == parse_error_kind::malformed_frontmatter);
  CHECK(bad.reason == diagnostic_reason::missing_open_delimiter);
  CHECK(bad.line == 1);
}

TEST_CASE("an empty file is refused as missing its opening delimiter", "[workbench][parse][reject]") {
  CHECK(refused("").reason == diagnostic_reason::missing_open_delimiter);
}

TEST_CASE("a CRLF file is refused outright", "[workbench][parse][reject]") {
  // The opener must be the exact four bytes "---\n". "---\r\n" is not.
  // Oracle-probed with a wholly CRLF fixture: exit 1,
  // `malformed_frontmatter` / opening delimiter is missing. Worth pinning
  // because a Windows-authored workbench file fails at the FIRST line with a
  // message about delimiters rather than anywhere near the real cause.
  auto const bad = refused("---\r\nentity_kind: task\r\nentity_id: 1\r\ntitle: T\r\nstatus: todo\r\n---\r\n");
  CHECK(bad.reason == diagnostic_reason::missing_open_delimiter);
  CHECK(bad.line == 1);
}

TEST_CASE("a UTF-8 BOM is refused outright", "[workbench][parse][reject]") {
  CHECK(refused("\xef\xbb\xbf---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n---\n").reason ==
        diagnostic_reason::missing_open_delimiter);
}

TEST_CASE("a missing closing delimiter is reported at the file's LAST line", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n");
  CHECK(bad.reason == diagnostic_reason::missing_close_delimiter);
  CHECK(bad.line == 5);
}

TEST_CASE("`---` at EOF with no trailing newline closes the block", "[workbench][parse][accept]") {
  auto const result = accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n---");
  CHECK(result.frontmatter.entity_id == 1);
  CHECK(result.body.empty());
}

TEST_CASE("`---\\n---\\n` alone reports a MISSING CLOSE, not an empty block", "[workbench][parse][reject]") {
  // Counter-intuitive and worth pinning: the closer is searched for in the
  // text AFTER the opener, and "---\n" leaves nothing containing "\n---\n".
  auto const bad = refused("---\n---\n");
  CHECK(bad.reason == diagnostic_reason::missing_close_delimiter);
  CHECK(bad.line == 2);
}

// --- the double-wrap case -------------------------------------------------

TEST_CASE("a nested front-matter block in the body is VALID and stays in the body", "[workbench][parse][accept][double-wrap]") {
  // This is the live hazard: `artifact update --body @<canonical workbench
  // file>` hands the artifact a body that already carries front matter, and
  // the next `workbench push` renders front matter INSIDE front matter. The
  // parser does not complain, because the closer is the FIRST "\n---\n" --
  // so the defect is silent, which is exactly why it goes unnoticed.
  // Reproduced end-to-end against both binaries; the resulting file is
  // byte-identical and still lints clean.
  constexpr std::string_view wrapped = "---\n"
                                       "entity_kind: artifact\n"
                                       "entity_id: 1\n"
                                       "anchor_plan_id: 1\n"
                                       "title: T\n"
                                       "status: draft\n"
                                       "artifact_kind: tech_spec\n"
                                       "---\n"
                                       "\n"
                                       "## Content\n"
                                       "\n"
                                       "---\n"
                                       "entity_kind: artifact\n"
                                       "entity_id: 1\n"
                                       "status: draft\n"
                                       "---\n"
                                       "\n"
                                       "inner body\n";
  auto const                 result  = accepted(wrapped);
  CHECK(result.frontmatter.entity_kind == "artifact");
  CHECK(result.frontmatter.title == "T");
  CHECK(result.body.starts_with("## Content\n"));
  CHECK(result.body.find("entity_kind: artifact") != std::string_view::npos);
}

// --- YAML shape -----------------------------------------------------------

TEST_CASE("a tab in a line's indentation is refused", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\n\tentity_id: 1\ntitle: T\nstatus: todo\n---\n");
  CHECK(bad.reason == diagnostic_reason::tab_indentation);
  CHECK(bad.line == 3);
}

TEST_CASE("a tab OUTSIDE the indentation run is accepted", "[workbench][parse][accept]") {
  // The scan stops at the first non-whitespace byte, so a tab inside a
  // value never reaches the check. The complement of the case above.
  auto const result = accepted("---\nentity_kind: task\nentity_id: 1\ntitle: a\tb\nstatus: todo\n---\n");
  CHECK(result.frontmatter.title == "a\tb");
}

TEST_CASE("an unquoted value containing ': ' is refused", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: Tech Spec: Auth\nstatus: todo\n---\n");
  CHECK(bad.reason == diagnostic_reason::unquoted_colon);
  CHECK(bad.line == 4);
}

TEST_CASE("the same value QUOTED is accepted", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: 'Tech Spec: Auth'\nstatus: todo\n---\n").frontmatter.title ==
        "Tech Spec: Auth");
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: \"Tech Spec: Auth\"\nstatus: todo\n---\n").frontmatter.title ==
        "Tech Spec: Auth");
}

TEST_CASE("a colon with NO following space is not the ': ' case", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: Ratio 3:4\nstatus: todo\n---\n").frontmatter.title == "Ratio 3:4");
}

TEST_CASE("a value ENDING in a colon is accepted by the parser", "[workbench][parse][accept]") {
  // Asymmetric with the RENDERER, which quotes it. A hand-written file with
  // `title: Note:` parses, and the next push normalizes it to `'Note:'`.
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: Note:\nstatus: todo\n---\n").frontmatter.title == "Note:");
}

TEST_CASE("an unbalanced opening quote is refused", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: 'Unclosed\nstatus: todo\n---\n");
  CHECK(bad.reason == diagnostic_reason::malformed_yaml);
  CHECK(bad.line == 4);
}

TEST_CASE("a line with no colon, and a line with an empty key, are both refused", "[workbench][parse][reject]") {
  auto const no_colon = refused("---\nentity_kind: task\nentity_id: 1\ngarbage line\ntitle: T\nstatus: todo\n---\n");
  CHECK(no_colon.reason == diagnostic_reason::malformed_yaml);
  CHECK(no_colon.line == 4);
  auto const empty_key = refused("---\nentity_kind: task\nentity_id: 1\n: value\ntitle: T\nstatus: todo\n---\n");
  CHECK(empty_key.reason == diagnostic_reason::malformed_yaml);
  CHECK(empty_key.line == 4);
}

TEST_CASE("keys need no space after the colon, may be space-indented, and may repeat", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind:task\nentity_id:1\ntitle:T\nstatus:todo\n---\n").frontmatter.entity_kind == "task");
  CHECK(accepted("---\n  entity_kind: task\n  entity_id: 1\n  title: T\n  status: todo\n---\n").frontmatter.entity_kind ==
        "task");
  // Last one wins.
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\nentity_id: 2\ntitle: T\nstatus: todo\n---\n").frontmatter.entity_id == 2);
}

TEST_CASE("a blank line inside the block is skipped but still advances the line counter", "[workbench][parse][accept]") {
  // The counter matters: every diagnostic after a blank line would name the
  // wrong line if blanks were dropped instead of skipped.
  CHECK(accepted("---\nentity_kind: task\n\nentity_id: 1\ntitle: T\nstatus: todo\n---\n").frontmatter.entity_id == 1);
  auto const bad = refused("---\nentity_kind: task\n\nentity_id: 1\ntitle: T\nstatus: todo\npriority: high\n---\n");
  CHECK(bad.reason == diagnostic_reason::invalid_integer);
  CHECK(bad.line == 7);
}

TEST_CASE("an unknown key is silently ignored", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\nmystery: yes\n---\n").frontmatter.title == "T");
}

TEST_CASE("trailing whitespace on a line is trimmed", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind: task   \nentity_id: 1   \ntitle: T   \nstatus: todo   \n---\n").frontmatter.status == "todo");
}

// --- lists ----------------------------------------------------------------

TEST_CASE("an orphan `- ` item, with no list key before it, is refused", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\n- orphan\ntitle: T\nstatus: todo\n---\n");
  CHECK(bad.reason == diagnostic_reason::leading_dash_scalar);
  CHECK(bad.line == 4);
}

TEST_CASE("an INDENTED list item is refused as malformed YAML", "[workbench][parse][reject]") {
  // The trap in this parser: `  - demo` is perfectly ordinary YAML, and it
  // is REFUSED -- the list check is `starts_with("- ")` on the right-trimmed
  // line, so leading spaces defeat it and the line then fails the key/value
  // check. Oracle-probed; pinned because it is the one rejection a
  // reasonable author would call a bug.
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ntouches:\n  - demo\n---\n");
  CHECK(bad.reason == diagnostic_reason::malformed_yaml);
  CHECK(bad.line == 7);
}

TEST_CASE("`derives_from` with an underscore is an unknown key, so its items become orphans", "[workbench][parse][reject]") {
  // The canonical key is `derives-from` with a DASH. The underscore spelling
  // fails loudly rather than silently dropping the refs, which is the better
  // of the two failure modes and is worth locking.
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\nderives_from:\n- plan:1\n---\n");
  CHECK(bad.reason == diagnostic_reason::leading_dash_scalar);
  CHECK(bad.line == 7);
}

TEST_CASE("`derives-from` with a dash parses its refs", "[workbench][parse][accept]") {
  auto const result = accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\nderives-from:\n- plan:7\n---\n");
  REQUIRE(result.frontmatter.derives_from.size() == 1);
  CHECK(result.frontmatter.derives_from[0].kind == "plan");
  CHECK(result.frontmatter.derives_from[0].id == 7);
}

TEST_CASE("a ref item that is not <kind>:<positive-id> is refused", "[workbench][parse][reject]") {
  auto const not_a_ref = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ncites:\n- notaref\n---\n");
  CHECK(not_a_ref.reason == diagnostic_reason::invalid_entity_ref);
  CHECK(not_a_ref.line == 7);
  // Zero is refused: the id must be strictly positive.
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ncites:\n- task:0\n---\n").reason ==
        diagnostic_reason::invalid_entity_ref);
  // An EMPTY kind (a bare ":5") is refused -- distinct from "notaref" above,
  // which has no colon at all and never reaches the empty-kind check.
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ncites:\n- :5\n---\n").reason ==
        diagnostic_reason::invalid_entity_ref);
  // An EMPTY id_text ("task:") is refused -- distinct from "task:0" above,
  // which has a present-but-zero id and never reaches the empty-id_text
  // check on its own (it fails the later `*id > 0` test instead).
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ncites:\n- task:\n---\n").reason ==
        diagnostic_reason::invalid_entity_ref);
  // A non-numeric id_text is refused via `parse_int64_zig` returning
  // nullopt, not via the `*id > 0` clause -- distinct from "task:0".
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ncites:\n- task:abc\n---\n").reason ==
        diagnostic_reason::invalid_entity_ref);
}

TEST_CASE("`touches` items are NOT validated as entity refs", "[workbench][parse][accept]") {
  auto const result =
      accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\ntouches:\n- anything at all\n---\n");
  REQUIRE(result.frontmatter.touches.size() == 1);
  CHECK(result.frontmatter.touches[0] == "anything at all");
}

TEST_CASE("multiple ref items accumulate in order", "[workbench][parse][accept]") {
  auto const result = accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\n"
                               "cites:\n- task:1\n- plan:2\n---\n");
  REQUIRE(result.frontmatter.cites.size() == 2);
  CHECK(result.frontmatter.cites[0] == wp::entity_ref{.kind = "task", .id = 1});
  CHECK(result.frontmatter.cites[1] == wp::entity_ref{.kind = "plan", .id = 2});
}

// --- integers -------------------------------------------------------------

TEST_CASE("parse_int64_zig reproduces std.fmt.parseInt's digit separators", "[workbench][parse][int]") {
  // The underscore cases are the ones a reasonable port drops, and they are
  // reachable: a workbench file saying `entity_id: 1_0` really does address
  // entity 10 on the reference binary.
  CHECK(wp::parse_int64_zig("12") == 12);
  CHECK(wp::parse_int64_zig("+5") == 5);
  CHECK(wp::parse_int64_zig("-5") == -5);
  CHECK(wp::parse_int64_zig("007") == 7);
  CHECK(wp::parse_int64_zig("1_0") == 10);
  CHECK(wp::parse_int64_zig("1__0") == 10);
  CHECK(wp::parse_int64_zig("9223372036854775807") == 9223372036854775807LL);
  CHECK_FALSE(wp::parse_int64_zig("_1").has_value());
  CHECK_FALSE(wp::parse_int64_zig("1_").has_value());
  CHECK_FALSE(wp::parse_int64_zig("_").has_value());
  CHECK_FALSE(wp::parse_int64_zig("+").has_value());
  CHECK_FALSE(wp::parse_int64_zig("").has_value());
  CHECK_FALSE(wp::parse_int64_zig("0x10").has_value());
  CHECK_FALSE(wp::parse_int64_zig("1 2").has_value());
  CHECK_FALSE(wp::parse_int64_zig("12abc").has_value());
  CHECK_FALSE(wp::parse_int64_zig("9223372036854775808").has_value());
}

TEST_CASE("an entity_id with digit separators is ACCEPTED and parses to its value", "[workbench][parse][accept][int]") {
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1_0\ntitle: T\nstatus: todo\n---\n").frontmatter.entity_id == 10);
}

TEST_CASE("a non-base-10 integer field is refused with its field name", "[workbench][parse][reject][int]") {
  auto const id = refused("---\nentity_kind: task\nentity_id: abc\ntitle: T\nstatus: todo\n---\n");
  CHECK(id.reason == diagnostic_reason::invalid_integer);
  CHECK(id.field == "entity_id");
  CHECK(id.line == 3);
  auto const priority = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\npriority: high\n---\n");
  CHECK(priority.reason == diagnostic_reason::invalid_integer);
  CHECK(priority.field == "priority");
  CHECK(priority.line == 6);
  CHECK(refused("---\nentity_kind: task\nentity_id: 0x10\ntitle: T\nstatus: todo\n---\n").reason ==
        diagnostic_reason::invalid_integer);
}

// --- required fields, and their SYNTHETIC line numbers --------------------

TEST_CASE("missing-field diagnostics carry a SYNTHETIC line, not the real one", "[workbench][parse][reject][synthetic]") {
  // entity_kind 2, entity_id 3, title 5, status 6, artifact_kind 7 --
  // regardless of the file's actual shape. Pinned on a file whose real
  // lines hold something else entirely, so a port that "fixed" this to
  // report a true line would fail here.
  auto const bad = refused("---\n"
                           "status: todo\n" // real line 2
                           "entity_id: 1\n" // real line 3
                           "title: T\n"     // real line 4
                           "---\n");
  CHECK(bad.err == parse_error_kind::missing_required_field);
  CHECK(bad.field == "entity_kind");
  CHECK(bad.line == 2); // synthetic: entity_kind is absent, so there is no real line
}

TEST_CASE("each required field reports its own synthetic line", "[workbench][parse][reject]") {
  CHECK(refused("---\nentity_id: 1\ntitle: T\nstatus: todo\n---\n").field == "entity_kind");
  CHECK(refused("---\nentity_kind: task\ntitle: T\nstatus: todo\n---\n").line == 3);
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\nstatus: todo\n---\n").line == 5);
  CHECK(refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\n---\n").line == 6);
  CHECK(refused("---\nentity_kind: artifact\nentity_id: 1\ntitle: T\nstatus: draft\n---\n").line == 7);
}

TEST_CASE("an EMPTY value counts as a missing field", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle:\nstatus: todo\n---\n");
  CHECK(bad.err == parse_error_kind::missing_required_field);
  CHECK(bad.field == "title");
}

TEST_CASE("a non-positive entity_id reports as MISSING, not as invalid", "[workbench][parse][reject]") {
  // `0` and `-5` both PARSE as integers, so the integer check passes and the
  // required-field check catches them instead. The distinction is visible:
  // the operator is told to add the field, not to fix its syntax.
  for (auto const value : {"0", "-5"}) {
    std::string const fixture = std::format("---\nentity_kind: task\nentity_id: {}\ntitle: T\nstatus: todo\n---\n", value);
    auto const        bad     = refused(fixture);
    CHECK(bad.err == parse_error_kind::missing_required_field);
    CHECK(bad.field == "entity_id");
    CHECK(bad.line == 3);
  }
}

// --- kinds and per-kind value sets ---------------------------------------

TEST_CASE("an unrecognized entity_kind is refused with the six accepted values", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: widget\nentity_id: 1\ntitle: T\nstatus: todo\n---\n");
  CHECK(bad.err == parse_error_kind::invalid_entity_kind);
  CHECK(bad.line == 2);
  CHECK(bad.expected == "plan, task, artifact, scenario, decision, or question");
}

TEST_CASE("`test_scenario` is NOT an accepted entity_kind here", "[workbench][parse][reject]") {
  // Even though `terminal::kind_from_string` and the sync layer's entity
  // stream both accept it. The two layers genuinely disagree; a file must
  // say `scenario`.
  CHECK(refused("---\nentity_kind: test_scenario\nentity_id: 1\ntitle: T\nstatus: draft\n---\n").err ==
        parse_error_kind::invalid_entity_kind);
  CHECK(wp::is_entity_kind("scenario"));
  CHECK_FALSE(wp::is_entity_kind("test_scenario"));
}

TEST_CASE("a status outside the kind's set is refused, and the sets differ per kind", "[workbench][parse][reject]") {
  auto const bad = refused("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: bogus\n---\n");
  CHECK(bad.err == parse_error_kind::invalid_field_value);
  CHECK(bad.field == "status");
  CHECK(bad.line == 5);
  CHECK(bad.expected == "todo, doing, blocked, done, or cancelled");
  // `todo` is valid for a task and invalid for a plan -- the check is
  // per-kind, not a union.
  CHECK(refused("---\nentity_kind: plan\nentity_id: 1\ntitle: T\nstatus: todo\n---\n").err ==
        parse_error_kind::invalid_field_value);
  CHECK(wp::statuses_for_kind("plan") == "draft, active, paused, done, or abandoned");
  CHECK(wp::statuses_for_kind("question") == "open, answered, or wontfix");
}

TEST_CASE("an artifact needs a recognized artifact_kind", "[workbench][parse][reject]") {
  auto const missing = refused("---\nentity_kind: artifact\nentity_id: 1\ntitle: T\nstatus: draft\n---\n");
  CHECK(missing.err == parse_error_kind::missing_required_field);
  CHECK(missing.field == "artifact_kind");
  auto const bad = refused("---\nentity_kind: artifact\nentity_id: 1\ntitle: T\nstatus: draft\nartifact_kind: nonsense\n---\n");
  CHECK(bad.err == parse_error_kind::invalid_field_value);
  CHECK(bad.field == "artifact_kind");
  CHECK(bad.line == 6);
  CHECK(bad.expected == wp::artifact_kinds());
  // The prose list carries an Oxford `or` on its last element, and the
  // check must still accept that element.
  CHECK(wp::artifact_kinds().find("or test_spec") != std::string_view::npos);
  CHECK(wp::diagnose("---\nentity_kind: artifact\nentity_id: 1\ntitle: T\nstatus: draft\n"
                     "artifact_kind: test_spec\n---\n") == std::nullopt);
}

TEST_CASE("a NON-artifact kind ignores artifact_kind entirely", "[workbench][parse][accept]") {
  CHECK(accepted("---\nentity_kind: task\nentity_id: 1\ntitle: T\nstatus: todo\nartifact_kind: nonsense\n---\n")
            .frontmatter.artifact_kind == "nonsense");
}

// --- body extraction ------------------------------------------------------

TEST_CASE("the body is everything after the closing delimiter, minus one blank separator", "[workbench][parse][accept]") {
  std::string const fixture = std::string{k_minimal_task} + "\n# Body\n\ntext\n";
  auto const        result  = accepted(fixture);
  CHECK(result.body == "# Body\n\ntext\n");
}

TEST_CASE("a body with NO leading blank line is kept byte-for-byte", "[workbench][parse][accept]") {
  // Distinct from the case above: the leading-newline strip is
  // CONDITIONAL on the body actually starting with '\n'. A body that
  // starts directly with real content must not lose its first byte.
  std::string const fixture = std::string{k_minimal_task} + "text\n";
  auto const        result  = accepted(fixture);
  CHECK(result.body == "text\n");
}

TEST_CASE("a UTF-8 body and title survive unchanged", "[workbench][parse][accept]") {
  constexpr std::string_view title   = "Héllo Wörld";
  constexpr std::string_view body    = "Ünïcödé body 🎉\n";
  std::string const          fixture = std::format("---\nentity_kind: question\nentity_id: 3\nanchor_plan_id: 1\n"
                                                   "title: {}\nstatus: open\n---\n\n{}",
                                                   title, body);
  auto const                 result  = accepted(fixture);
  CHECK(result.frontmatter.title == title);
  CHECK(result.body == body);
}

// --- error tags -----------------------------------------------------------

TEST_CASE("error_name emits the Zig tags the oracle interpolates", "[workbench][parse]") {
  // These bytes reach the operator through `workbench pull --json`'s
  // `malformed_files[].parse_error` and the verbose `MALFORMED: <path>
  // (MalformedFrontmatter)` line.
  CHECK(wp::error_name(parse_error_kind::malformed_frontmatter) == "MalformedFrontmatter");
  CHECK(wp::error_name(parse_error_kind::missing_required_field) == "MissingRequiredField");
  CHECK(wp::error_name(parse_error_kind::invalid_entity_kind) == "InvalidEntityKind");
  CHECK(wp::error_name(parse_error_kind::invalid_field_value) == "InvalidFieldValue");
}

TEST_CASE("strip_yaml_quotes removes exactly one matching pair", "[workbench][parse]") {
  CHECK(wp::strip_yaml_quotes("'foo'") == "foo");
  CHECK(wp::strip_yaml_quotes("\"foo\"") == "foo");
  CHECK(wp::strip_yaml_quotes("foo") == "foo");
  CHECK(wp::strip_yaml_quotes("'foo\"") == "'foo\"");
  CHECK(wp::strip_yaml_quotes("''") == "");
  // Back is a single-quote but front is NOT -- the mirror image of
  // "'foo\"" above, isolating the FRONT half of the single-quote match.
  CHECK(wp::strip_yaml_quotes("abc'") == "abc'");
  // Front is a double-quote but back is NOT -- isolates the BACK half of
  // the double-quote match.
  CHECK(wp::strip_yaml_quotes("\"abc") == "\"abc");
  // Back is a double-quote but front is NOT -- isolates the FRONT half of
  // the double-quote match.
  CHECK(wp::strip_yaml_quotes("abc\"") == "abc\"");
}
