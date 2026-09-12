// @file render.t.cpp
// @brief Directive-substitution tests for
// `planar.engine.templates.render` (plan 996, task 6190).
//
// HOME SAFETY. Pure string work over an in-memory context. No file is
// opened, no environment variable is read, no path is constructed.
//
// ORACLE PROVENANCE. Every expectation here was captured by running the
// built Zig binary against a probe template in a pinned scratch arena
// (PLANAR_DB / PLANAR_HOME / PLANAR_CONFIG_PATH / PLANAR_LOCAL_HOME / HOME
// all redirected into /tmp), stderr taken through a PIPE — see
// src/cmd/parity_harness.hpp for why a `2> file` capture corrupts
// multi-write Zig output.
//
// The probe set lived at `<arena>/fakehome/.planar/templates/probe/px/` and
// was exercised through `templates render` and `templates validate`.
//
// THE ONE THING TO READ BEFORE EDITING THIS FILE: the `{{if}}` 128-byte
// cases below are NOT testing an implementation detail. They pin a REAL,
// operator-visible Planar defect that this port reproduces on purpose. See
// render.cppm's header and src/lib/engine/templates/CMakeLists.txt.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.json_dom;
import planar.engine.templates.context;
import planar.engine.templates.render;

// `tpl`, not `tm` -- `import std;` brings `std::tm` into scope and a
// namespace alias by that name is a redefinition.
namespace tpl = planar::engine::templates;
namespace jd  = planar::json_dom;

namespace {

/// @brief A context with a few identifiable values, for reference tests.
/// @return The context.
auto sample() -> tpl::render_context {
  return tpl::render_context{
      .feature      = {.id         = 7,
                       .slug       = "feat-slug",
                       .title      = "Feature Title",
                       .body       = "feature body",
                       .scope_kind = "association",
                       .scope_id   = 3,
                       .status     = "active"},
      .plan         = {.id         = 9,
                       .slug       = "plan-slug",
                       .title      = "Plan Title",
                       .body       = "plan body",
                       .scope_kind = "association",
                       .scope_id   = 3,
                       .status     = "draft"},
      .task         = {.id         = 11,
                       .title      = "Task Title",
                       .body       = "task body",
                       .status     = "todo",
                       .priority   = 100,
                       .scope_kind = "association",
                       .scope_id   = 3},
      .scenario     = {.id = 13, .title = "Scenario Title", .body = "scenario body"},
      .touches      = {"org/repo-a", "org/repo-b"},
      .assoc        = {.slug = "project:proj", .name = "Proj"},
      .external_key = "GH-42",
      .children     = {{.title = "child-one", .external_key = "GH-11"}, {.title = "child-two", .external_key = ""}},
  };
}

/// @brief Render `src` against `ctx`, or return the zig error NAME.
///
/// Returning the name rather than the enumerator keeps every assertion in
/// the same vocabulary `templates validate` prints, which is the vocabulary
/// the oracle was captured in.
/// @param src The template string.
/// @param ctx The context.
/// @return The rendered text, or the error name.
auto render_or_name(std::string_view src, const tpl::render_context& ctx) -> std::string {
  auto const out = tpl::exec_string(src, ctx);
  if (!out.has_value()) {
    return std::string{tpl::error_name(out.error())};
  }
  return *out;
}

} // namespace

// --- field references -------------------------------------------------

TEST_CASE("render substitutes a nested field reference", "[templates][render]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{.Task.Title}}", ctx) == "Task Title");
  CHECK(render_or_name("{{.Feature.Title}}", ctx) == "Feature Title");
  CHECK(render_or_name("{{.Plan.Title}}", ctx) == "Plan Title");
  CHECK(render_or_name("{{.Scenario.Title}}", ctx) == "Scenario Title");
  CHECK(render_or_name("{{.Assoc.Slug}}", ctx) == "project:proj");
  CHECK(render_or_name("{{.ExternalKey}}", ctx) == "GH-42");
}

TEST_CASE("render distinguishes Feature from Plan", "[templates][render]") {
  // They are the SAME row for a task context and DIFFERENT for a child-plan
  // context, so a template that confused them would still look right half
  // the time. See builder.cppm.
  auto const ctx = sample();
  CHECK(render_or_name("{{.Feature.ID}}|{{.Plan.ID}}", ctx) == "7|9");
}

TEST_CASE("render prints integer fields as decimal", "[templates][render]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{.Task.ID}}", ctx) == "11");
  CHECK(render_or_name("{{.Task.Priority}}", ctx) == "100");
  CHECK(render_or_name("{{.Feature.ScopeID}}", ctx) == "3");
}

TEST_CASE("render keeps surrounding literal text", "[templates][render]") {
  auto const ctx = sample();
  CHECK(render_or_name("before {{.Task.Title}} after", ctx) == "before Task Title after");
  CHECK(render_or_name("no directives at all", ctx) == "no directives at all");
  CHECK(render_or_name("", ctx).empty());
}

TEST_CASE("render trims spaces and tabs inside a directive but not newlines", "[templates][render]") {
  // zig trims with `" \t"` only. A newline-padded directive therefore does
  // not start with `.` and is UnsupportedDirective — NOT UnknownField,
  // which is what this test asserted until the oracle disagreed:
  //   templates validate probe px edge -> `[nlpad]: UnsupportedDirective`
  auto const ctx = sample();
  CHECK(render_or_name("{{  .Task.Title\t}}", ctx) == "Task Title");
  CHECK(render_or_name("{{\n.Task.Title\n}}", ctx) == "UnsupportedDirective");
}

// --- refusals, in the oracle's own vocabulary -------------------------

TEST_CASE("render refuses an unknown field by NAME", "[templates][render][refusal]") {
  // Oracle: `templates validate probe px broken` printed
  // `[a]: UnknownField` for `{{.Nope}}`.
  auto const ctx = sample();
  CHECK(render_or_name("{{.Nope}}", ctx) == "UnknownField");
  CHECK(render_or_name("{{.Task.Nope}}", ctx) == "UnknownField");
  CHECK(render_or_name("{{.Feature.Nope}}", ctx) == "UnknownField");
}

TEST_CASE("render refuses a bare slice reference as UnsupportedDirective", "[templates][render][refusal]") {
  // Oracle: `[b]: UnsupportedDirective` for `{{.Touches}}`. Note this is a
  // DIFFERENT error from `{{.Nope}}` — `text/template` would have printed
  // `[a b]`, and the oracle refuses rather than inventing a spelling.
  auto const ctx = sample();
  CHECK(render_or_name("{{.Touches}}", ctx) == "UnsupportedDirective");
}

TEST_CASE("render refuses a bare STRUCT reference as UnsupportedDirective", "[templates][render][refusal]") {
  // "You named a struct where a scalar goes" is distinct from "you named
  // nothing at all", and validate prints the difference.
  auto const ctx = sample();
  CHECK(render_or_name("{{.Task}}", ctx) == "UnsupportedDirective");
  CHECK(render_or_name("{{.Assoc}}", ctx) == "UnsupportedDirective");
}

TEST_CASE("render refuses an unclosed if/range as UnclosedDirective", "[templates][render][refusal]") {
  // Oracle: `[c]: UnclosedDirective` for `{{if .ExternalKey}}x` with no
  // `{{end}}`.
  auto const ctx = sample();
  CHECK(render_or_name("{{if .ExternalKey}}x", ctx) == "UnclosedDirective");
  CHECK(render_or_name("{{range .Touches}}x", ctx) == "UnclosedDirective");
  CHECK(render_or_name("{{.Task.Title", ctx) == "UnclosedDirective");
}

TEST_CASE("render refuses a stray end/else as UnsupportedDirective", "[templates][render][refusal]") {
  // Oracle: `[d]: UnsupportedDirective` for a lone `{{end}}`.
  auto const ctx = sample();
  CHECK(render_or_name("{{end}}", ctx) == "UnsupportedDirective");
  CHECK(render_or_name("{{else}}", ctx) == "UnsupportedDirective");
  CHECK(render_or_name("{{}}", ctx) == "UnsupportedDirective");
}

TEST_CASE("render refuses `with` and other text/template surface", "[templates][render][refusal]") {
  // Oracle: `[e]: UnsupportedDirective` for `{{with .X}}y{{end}}`. Failing
  // loudly matters here — a silently-ignored directive renders a
  // plausible payload that then gets pushed to Jira.
  auto const ctx = sample();
  CHECK(render_or_name("{{with .X}}y{{end}}", ctx) == "UnsupportedDirective");
  CHECK(render_or_name("{{printf \"%s\" .Task.Title}}", ctx) == "UnsupportedDirective");
}

TEST_CASE("render refuses a bare {{.}} outside a range", "[templates][render][refusal]") {
  // Go binds `.` to the top-level context here; this port does not, and
  // neither does the oracle.
  auto const ctx = sample();
  CHECK(render_or_name("{{.}}", ctx) == "UnknownField");
}

TEST_CASE("render refuses to iterate anything but the two named slices", "[templates][render][refusal]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Nope}}x{{end}}", ctx) == "UnknownField");
  CHECK(render_or_name("{{range .Task}}x{{end}}", ctx) == "UnknownField");
}

// --- range ------------------------------------------------------------

TEST_CASE("render iterates Touches with {{.}} bound to each slug", "[templates][render][range]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Touches}}- {{.}}\n{{end}}", ctx) == "- org/repo-a\n- org/repo-b\n");
}

TEST_CASE("render emits NOTHING for a range over an empty slice", "[templates][render][range]") {
  // The shipped `issue.json` guards its repository-scope section this way,
  // and the oracle rendered an empty section for a task with no touches
  // rather than refusing.
  tpl::render_context ctx = sample();
  ctx.touches.clear();
  CHECK(render_or_name("{{range .Touches}}- {{.}}\n{{end}}", ctx).empty());
  CHECK(render_or_name("head\n{{range .Touches}}x{{end}}tail", ctx) == "head\ntail");
}

TEST_CASE("render binds a child's OWN ExternalKey inside a Children range", "[templates][render][range]") {
  // The child's key SHADOWS the top-level one — `GH-11` and then nothing,
  // never the context's `GH-42`.
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Children}}- {{.Title}}{{if .ExternalKey}} (#{{.ExternalKey}}){{end}}\n{{end}}", ctx) ==
        "- child-one (#GH-11)\n- child-two\n");
}

TEST_CASE("render binds {{.}} to a child's TITLE", "[templates][render][range]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Children}}[{{.}}]{{end}}", ctx) == "[child-one][child-two]");
}

TEST_CASE("render refuses {{.Title}} inside a TOUCHES range", "[templates][render][range]") {
  // `{{.Title}}` is only meaningful for a child element. A Touches element
  // has no title, and the oracle does not silently render the empty string.
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Touches}}{{.Title}}{{end}}", ctx) == "UnknownField");
}

TEST_CASE("render nests an if inside a range body", "[templates][render][range]") {
  auto const ctx = sample();
  CHECK(render_or_name("{{range .Touches}}{{if .Assoc.Slug}}<{{.}}>{{end}}{{end}}", ctx) == "<org/repo-a><org/repo-b>");
}

// --- if ---------------------------------------------------------------

TEST_CASE("render emits an if body only when the field is non-empty", "[templates][render][if]") {
  tpl::render_context ctx = sample();
  CHECK(render_or_name("{{if .ExternalKey}}yes{{end}}", ctx) == "yes");
  ctx.external_key.clear();
  CHECK(render_or_name("{{if .ExternalKey}}yes{{end}}", ctx).empty());
}

TEST_CASE("render treats an UNKNOWN field as falsy rather than an error", "[templates][render][if]") {
  // `eval_truthy` swallows `unknown_field` specifically and propagates
  // every other error. Asserting the negative matters: if it swallowed
  // everything, the 128-byte case below would silently render empty
  // instead of failing.
  auto const ctx = sample();
  CHECK(render_or_name("{{if .Nope}}yes{{end}}", ctx).empty());
  CHECK(render_or_name("{{if .Touches}}yes{{end}}", ctx) == "UnsupportedDirective");
}

TEST_CASE("render treats a ZERO integer as TRUTHY", "[templates][render][if]") {
  // Not Go's rule — the oracle's. Truthiness is "renders to a non-empty
  // string", and `0` renders as `"0"`.
  tpl::render_context ctx = sample();
  ctx.task.priority       = 0;
  CHECK(render_or_name("{{if .Task.Priority}}yes{{end}}", ctx) == "yes");
}

TEST_CASE("render's {{else}} handling is ASYMMETRIC — silent when false, loud when true",
          "[templates][render][if][oracle-defect]") {
  // Captured, because the obvious expectation ("`else` is unsupported, so
  // it always refuses") is WRONG and this test asserted it until the
  // oracle disagreed.
  //
  //   templates render probe px edge3  ({{if .Nope}}a{{else}}b{{end}})
  //     -> exit 0, `"elsefalse": ""`
  //   templates render probe px edge2  (same, plus a TRUTHY {{if}})
  //     -> exit 1, `error: rendering template: UnsupportedDirective`
  //
  // The mechanism: `{{else}}` is not counted while scanning for the
  // matching `{{end}}`, so a FALSY test skips the whole body — the
  // `{{else}}` and everything after it — and never dispatches the
  // directive. A TRUTHY test renders the body, hits `{{else}}`, and
  // refuses.
  //
  // The consequence is the bad one: an operator who writes an `{{else}}`
  // gets the `b` branch SILENTLY DROPPED whenever the condition is false,
  // and a hard failure whenever it is true. Neither is what they asked
  // for. Reproduced under D2; needs its own task against the oracle.
  tpl::render_context ctx = sample();
  CHECK(render_or_name("{{if .Nope}}a{{else}}b{{end}}", ctx).empty());
  CHECK(render_or_name("{{if .Task.Title}}a{{else}}b{{end}}", ctx) == "UnsupportedDirective");
}

// --- the removed 128-byte defect (task 6210) --------------------------

TEST_CASE("render's {{if}} accepts a value of exactly 128 bytes", "[templates][render][if][6210]") {
  // The old boundary's lower half, kept: it never failed, and it is what
  // makes the case below a BOUNDARY rather than a single point.
  tpl::render_context ctx = sample();
  ctx.task.body           = std::string(128, 'q');
  CHECK(render_or_name("{{if .Task.Body}}HAS-BODY{{end}}", ctx) == "HAS-BODY");
}

TEST_CASE("render's {{if}} accepts a value of 129 bytes — the removed ceiling", "[templates][render][if][6210]") {
  // INVERTED AT TASK 6210 (decision 1067's FIX set). This case used to
  // assert `== "OutOfMemory"`: 129 bytes failed the ENTIRE render, exit 1,
  // reproducing zig's `var tmp: [128]u8` FixedBufferAllocator. Bisected to
  // exactly 128 pass / 129 fail.
  //
  // Reproducing it was right while the oracle existed; decision 1067 ended
  // that rule and named this row in its FIX set. The blast radius is why:
  // truthiness does not depend on length, and every task whose body exceeded
  // 128 bytes was unrenderable through any `{{if}}`-guarded template --
  // including the shipped `templates/defaults/github-issues/issue.json`.
  //
  // THE LENGTH STAYS A LITERAL 129, not `k_truthy_budget + 1`. The original
  // case learned this the hard way: written in terms of the constant, a
  // break-probe widening the budget widened the test's own input and it
  // stayed green. The literal is the point, and it still is -- it pins the
  // exact byte the old ceiling rejected.
  tpl::render_context ctx = sample();
  ctx.task.body           = std::string(129, 'q');
  CHECK(render_or_name("{{if .Task.Body}}HAS-BODY{{end}}", ctx) == "HAS-BODY");
}

TEST_CASE("render's {{if}} accepts a value far past any former ceiling", "[templates][render][if][6210]") {
  // Non-vacuity: a "fix" that merely bumped the constant would satisfy the
  // 129-byte case. 8 KiB would not.
  tpl::render_context ctx = sample();
  ctx.task.body           = std::string(8192, 'q');
  CHECK(render_or_name("{{if .Task.Body}}HAS-BODY{{end}}", ctx) == "HAS-BODY");
}

TEST_CASE("an EMPTY value is still falsy — length was never the rule", "[templates][render][if][6210]") {
  // The property the ceiling obscured: `{{if}}` asks whether the rendered
  // text is non-empty, and nothing else. Removing a LENGTH ceiling must not
  // make everything truthy.
  //
  // No `{{else}}` here: this renderer does not support it (there is a case
  // above pinning `{{else}}` as UnsupportedDirective), so the falsy body
  // simply emits nothing.
  tpl::render_context ctx = sample();
  ctx.task.body           = "";
  CHECK(render_or_name("[{{if .Task.Body}}HAS-BODY{{end}}]", ctx) == "[]");
}

TEST_CASE("a long value that is merely PRINTED still renders whole", "[templates][render][if][6210]") {
  // Carried over from the old suite. The budget lived in `eval_truthy`, so a
  // port that had put a ceiling in `resolve_reference` instead would break
  // every long body while the `{{if}}` cases stayed green.
  tpl::render_context ctx = sample();
  ctx.task.body           = std::string(4096, 'z');
  CHECK(render_or_name("{{.Task.Body}}", ctx) == std::string(4096, 'z'));
}

// --- whole-tree rendering ---------------------------------------------

TEST_CASE("render_template substitutes only STRING leaves", "[templates][render][tree]") {
  auto const ctx    = sample();
  auto const parsed = jd::parse_json(R"({"s":"{{.Task.Title}}","n":42,"b":true,"z":null})");
  REQUIRE(parsed.has_value());
  auto const out = tpl::render_template(*parsed, ctx);
  REQUIRE(out.has_value());
  CHECK(jd::stringify_indent2(*out) == "{\n  \"s\": \"Task Title\",\n  \"n\": 42,\n  \"b\": true,\n  \"z\": null\n}");
}

TEST_CASE("render_template does NOT substitute object KEYS", "[templates][render][tree]") {
  // zig's `renderValue` dupes the key and recurses only into the value.
  auto const ctx    = sample();
  auto const parsed = jd::parse_json(R"({"{{.Task.Title}}":"v"})");
  REQUIRE(parsed.has_value());
  auto const out = tpl::render_template(*parsed, ctx);
  REQUIRE(out.has_value());
  REQUIRE(out->object.size() == 1);
  CHECK(out->object[0].first == "{{.Task.Title}}");
}

TEST_CASE("render_template preserves key ORDER through the substitution", "[templates][render][tree]") {
  auto const ctx    = sample();
  auto const parsed = jd::parse_json(R"({"title":"{{.Task.Title}}","body":"b","labels":[],"assignees":[]})");
  REQUIRE(parsed.has_value());
  auto const out = tpl::render_template(*parsed, ctx);
  REQUIRE(out.has_value());
  REQUIRE(out->object.size() == 4);
  CHECK(out->object[0].first == "title");
  CHECK(out->object[3].first == "assignees");
}

TEST_CASE("render_template recurses into nested arrays and objects", "[templates][render][tree]") {
  auto const ctx    = sample();
  auto const parsed = jd::parse_json(R"({"o":{"a":["{{.Task.Title}}","lit"]}})");
  REQUIRE(parsed.has_value());
  auto const out = tpl::render_template(*parsed, ctx);
  REQUIRE(out.has_value());
  CHECK(jd::stringify_indent2(*out) == "{\n  \"o\": {\n    \"a\": [\n      \"Task Title\",\n      \"lit\"\n    ]\n  }\n}");
}

TEST_CASE("render_template fails the WHOLE tree on one bad leaf", "[templates][render][tree]") {
  // Partial output would be the silent-degradation shape: a payload that
  // is structurally valid and semantically wrong.
  auto const ctx    = sample();
  auto const parsed = jd::parse_json(R"({"ok":"fine","bad":"{{.Nope}}"})");
  REQUIRE(parsed.has_value());
  auto const out = tpl::render_template(*parsed, ctx);
  REQUIRE_FALSE(out.has_value());
  CHECK(tpl::error_name(out.error()) == "UnknownField");
}

// --- the error-name wire format ---------------------------------------

TEST_CASE("error_name returns the zig @errorName spellings verbatim", "[templates][render]") {
  // `templates validate` prints these straight into stdout and into its
  // `--json` envelope, so they are pinned contract rather than diagnostics.
  CHECK(tpl::error_name(tpl::render_error::unsupported_directive) == "UnsupportedDirective");
  CHECK(tpl::error_name(tpl::render_error::unknown_field) == "UnknownField");
  CHECK(tpl::error_name(tpl::render_error::unclosed_directive) == "UnclosedDirective");
  CHECK(tpl::error_name(tpl::render_error::unexpected_end) == "UnexpectedEnd");
  CHECK(tpl::error_name(tpl::render_error::out_of_memory) == "OutOfMemory");
}
