// @file parity.t.cpp
// @brief `planar-execute` (C++) surface pins (plan 996, task 6107; retired
// off the Zig oracle by task 6541).
//
// Harness in `../parity_harness.hpp`.
//
// This binary's parity coverage is the most COMPLETE of the four, because
// its entire argument surface is ported: five of the six argv shapes below
// are compared byte for byte on stdout, stderr AND exit code. That is
// possible here and not elsewhere because planar-execute has no command
// tree to be partially ported — its surface is one usage banner and one
// verb.
//
// Two shapes a reasonable port gets wrong, and both are pinned:
//
//   THE USAGE TEXT GOES TO STDERR, ALWAYS, INCLUDING ON `--help`, where
//   stdout stays completely empty and the exit code is 0. Every other
//   binary in this tree writes help to stdout. A port that "fixed" this
//   would break every caller redirecting the JSON result channel.
//
//   A BARE INVOCATION AND `--help` EMIT IDENTICAL BYTES BUT DIFFERENT
//   CODES — 2 and 0. Comparing only output would pass a port that
//   collapsed them.
//
// `run <file> --phase <p>` where the file EXISTS IS compared below — the Lua
// engine (task 6042) and all twenty-five host functions, `ctx.brief`
// included as of task 6125, are ported. The comment this replaced described
// an earlier state (task 6107, before `engine_execute` existed) where this
// binary exited 64 unconditionally; that exit code and its NOT-compared
// carve-out are both gone.
//
// ## Task 6541: retired off the Zig oracle
//
// Every case below used to run BOTH binaries and assert `cpp == zig`,
// gated `SKIP` when the oracle was absent (D6). That comparison passed —
// the two agreed — for as long as `zig/` was buildable, which is exactly
// the evidence this task preserves: the expected bytes below were
// transcribed from a live cpp/zig diff at commit 4fc09c0777cb (all ten
// argv shapes, all fourteen workflow shapes, the fs-confinement run and
// the `ctx.brief` failure message matched byte for byte at that commit).
// Follows the pattern task 6123 set in
// `src/cmd/planar/parity.t.cpp`'s "the CLI surface is CLI11's now, and
// pinned": pin the bytes the two binaries already agreed on, assert them
// against the C++ binary ALONE, and stop needing `zig/` to run at all.
// Pinned EXACTLY — no `contains`, no prefix, no regex; a dropped or
// reworded message stops matching.
//
// SKIP, not fail, when the oracle is absent — this no longer applies to
// any case in this file (D6 is now moot here).

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"

namespace {

using planar::cmd::parity::capture;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::run_pinned;

/// @brief Path to the built C++ binary (set by this target's CMakeLists).
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Run the C++ binary over `args` in a freshly-pinned scratch root.
/// @param tag A short discriminator naming the case.
/// @param args The arguments (excluding argv[0]).
/// @return The capture.
auto run_cpp(std::string_view tag, std::vector<std::string> args) -> capture {
  auto const arena = make_arena(tag);
  return run_pinned(cpp_bin(), args, arena.cpp_root, "cpp");
}

} // namespace

TEST_CASE("planar-execute parity: every ported argv shape matches byte for byte", "[cmd][execute][parity]") {
  // Bytes transcribed from a live cpp/zig diff at commit 4fc09c0777cb (task
  // 6541) — see this file's header. Runs without the oracle: an assertion
  // about this binary, not a comparison.
  static constexpr std::string_view k_usage = "planar-execute — deterministic, spawn-free Lua workflow engine.\n"
                                              "\n"
                                              "Usage:\n"
                                              "  planar-execute run <workflow.lua> --phase <name> [--args <json>]\n"
                                              "                     [--worktree <dir>] [--sandbox-root <dir>]\n"
                                              "                     [--engine <embedded|centurion>]\n"
                                              "  planar-execute profile show [--profile <name>] [--json]\n"
                                              "  planar-execute submit <bundle> [--input <json>] [--profile <name>]\n"
                                              "  planar-execute status [<run-id>] [--profile <name>] [--json]\n"
                                              "  planar-execute cancel <run-id> [--profile <name>] [--json]\n"
                                              "  planar-execute host status|drain|stop [--profile <name>] [--json]\n"
                                              "  planar-execute follow <run-id> [--from <cursor>] [--profile <name>]\n"
                                              "  planar-execute schema\n"
                                              "\n"
                                              "Loads the workflow in the sandbox, registers the deterministic host\n"
                                              "surface (cli/git/fs/flow/ctx), calls the named phase, and prints the\n"
                                              "workflow's flow.result(table) payload as JSON on stdout.\n";

  struct shape {
    std::string_view         tag;
    std::vector<std::string> args;
    int                      expected_code;
    std::string              expected_err;
  };
  // Exit codes and stderr bytes named explicitly, not derived from a
  // live oracle diff, per task 6541's retirement of this file's oracle
  // dependency.
  std::vector<shape> const shapes{
      {"bare", {}, 2, std::string{k_usage}},
      {"help_long", {"--help"}, 0, std::string{k_usage}},
      {"help_short", {"-h"}, 0, std::string{k_usage}},
      {"help_word", {"help"}, 0, std::string{k_usage}},
      {"unknown", {"bogus"}, 2, "planar-execute: unknown verb: bogus\n" + std::string{k_usage}},
      {"unknown_dashed", {"--version"}, 2, "planar-execute: unknown verb: --version\n" + std::string{k_usage}},
      {"run_noargs", {"run"}, 2, std::string{k_usage}},
      {"run_nophase", {"run", "x.lua"}, 2, std::string{k_usage}},
      {"run_badflag", {"run", "x.lua", "--phase", "p", "--nope"}, 2, std::string{k_usage}},
      {"run_missing_file", {"run", "x.lua", "--phase", "p"}, 1, "planar-execute: cannot read workflow: x.lua\n"},
  };

  for (auto const& s : shapes) {
    auto const cpp = run_cpp(s.tag, s.args);
    INFO("shape: " << s.tag);
    CHECK(cpp.code == s.expected_code);
    CHECK(cpp.err == s.expected_err);
    // stdout is the clean JSON result channel and stays EMPTY on every one
    // of these — including `--help`, which is the shape most likely to be
    // "fixed" onto stdout by a well-meaning port.
    CHECK(cpp.out.empty());
  }
}

TEST_CASE("planar-execute schema prints the catalog on STDOUT, nothing on stderr, exit 0", "[cmd][execute][schema][6486]") {
  // The one verb whose payload is stdout. Every other shape in this file
  // keeps stdout empty; a `schema` that leaked to stderr would be invisible
  // to `cli_usage_lint`, which captures stdout only.
  auto const cpp = run_cpp("schema", {"schema"});
  CHECK(cpp.code == 0);
  CHECK(cpp.err.empty());
  CHECK(cpp.out.starts_with("{"));
  CHECK(cpp.out.ends_with("}\n"));
  CHECK(cpp.out.contains(R"("root":"planar-execute")"));
  CHECK(cpp.out.contains(R"("command":"planar-execute run")"));
  CHECK(cpp.out.contains(R"("command":"planar-execute schema")"));
  CHECK(cpp.out.contains(R"("long":"--sandbox-root")"));
  // The other binaries' catalogs share this envelope; the lint tool keys
  // on these two fields before it reads a single command.
  CHECK(cpp.out.contains(R"("schemaVersion":)"));
  CHECK(cpp.out.contains(R"("layout":)"));
  // `schema` takes no arguments: extra tokens are ignored by the
  // classifier, exactly as `--help extra` is, so the catalog still prints.
  auto const noisy = run_cpp("schema_extra", {"schema", "--json"});
  CHECK(noisy.code == 0);
  CHECK(noisy.out == cpp.out);
}

TEST_CASE("planar-execute parity: --help and a bare invocation differ ONLY in exit code", "[cmd][execute][parity][exitcode]") {
  // Bytes transcribed at commit 4fc09c0777cb (task 6541) — see file header.
  auto const cpp_help = run_cpp("h", {"--help"});
  auto const cpp_bare = run_cpp("b", {});

  // Identical bytes on stderr…
  CHECK(cpp_help.err == cpp_bare.err);
  // …and DIFFERENT codes. Output-only comparison would pass a port that
  // collapsed these two.
  CHECK(cpp_help.code == 0);
  CHECK(cpp_bare.code == 2);
}

namespace {

/// @brief Seed a scratch arena with the given workflow file and run the
/// C++ binary over it.
/// @param tag A short discriminator naming the case.
/// @param workflow The Lua source.
/// @param extra Arguments after `run wf.lua --phase p`.
/// @return The capture.
auto run_cpp_over_workflow(std::string_view tag, std::string_view workflow, std::vector<std::string> const& extra = {})
    -> capture {
  auto const arena = make_arena(tag);
  {
    std::ofstream file(arena.cpp_root / "proj" / "wf.lua", std::ios::binary);
    file << workflow;
  }
  std::vector<std::string> args{"run", "wf.lua", "--phase", "p"};
  args.insert(args.end(), extra.begin(), extra.end());
  return run_pinned(cpp_bin(), args, arena.cpp_root, "cpp");
}

} // namespace

TEST_CASE("planar-execute parity: a workflow runs identically in both engines", "[cmd][execute][parity][workflow]") {
  // Task 6107 could not write this case: the Lua half was deferred and this
  // binary exited 64 over any readable workflow. Task 6042 landed the engine,
  // so the deferral test it replaced is gone and the real comparison is here.
  //
  // The cases are chosen for the places two independent implementations
  // diverge WITHOUT either looking wrong on its own — number formatting,
  // object key order, integer width, the stdout/stderr split, and which
  // failures land on which exit code.
  //
  // Retired off the oracle by task 6541: every expected byte string below
  // was transcribed from a live cpp/zig diff at commit 4fc09c0777cb (they
  // agreed), and is now asserted against the C++ binary alone — see this
  // file's header.
  struct shape {
    std::string_view         tag;
    std::string_view         workflow;
    std::vector<std::string> extra;
    int                      expected_code;
    std::string_view         expected_out;
    std::string_view         expected_err;
  };
  std::vector<shape> const shapes{
      // A payload exercising every marshalled type at once. Object key order
      // is the interesting part: Lua's traversal order depends on the
      // per-state hash seed, so an implementation that emitted raw order
      // would differ from the oracle AND from itself run to run — the
      // renderer sorts keys, which is why this is stable to pin.
      {"payload",
       "function p() flow.result({ zebra = 1, apple = 'a\"b\\nc', list = {1, 2, 3}, nested = { deep = true }, empty = {}, "
       "no = false }) end",
       {},
       0,
       R"({"apple":"a\"b\nc","empty":{},"list":[1,2,3],"nested":{"deep":true},"no":false,"zebra":1})"
       "\n",
       ""},
      // Float spellings. Zig's `{d}` is shortest-round-trip digits with NO
      // exponent, so 1e300 is three hundred and one characters and 3e-7 is
      // `0.0000003`. Both of the obvious C++ formatters get this wrong in a
      // different direction — the port matches the oracle's spelling
      // exactly, which is what this pins.
      {"floats",
       "function p() flow.result({ a = 1/3, b = 1e300, c = -0.0, d = 1.0, e = 1e21, f = 3.0e-7, g = 1/0 }) end",
       {},
       0,
       "{\"a\":0.3333333333333333,\"b\":"
       "1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
       "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
       "00000000000000000000000000000000000000000000000000000000000,\"c\":-0,\"d\":1,\"e\":1000000000000000000000,\"f\":0."
       "0000003,\"g\":inf}\n",
       ""},
      // Integer width through --args and back out again.
      {"integers",
       "function p() flow.result({ n = ctx.args.n, big = ctx.args.big, t = math.type(ctx.args.n) }) end",
       {"--args", R"({"n":5,"big":9007199254740993})"},
       0,
       R"({"big":9007199254740993,"n":5,"t":"integer"})"
       "\n",
       ""},
      // --args need not be an object: an array or a string becomes ctx.args
      // as-is. Pinned as observed at plan 1033 M0 (task 6483), which freezes
      // the contract rather than tightening it; see golden.t.cpp for the
      // malformed-JSON refusal.
      {"args_array",
       "function p() flow.result({ t = type(ctx.args), n = #ctx.args }) end",
       {"--args", "[1,2]"},
       0,
       R"({"n":2,"t":"table"})"
       "\n",
       ""},
      {"args_string",
       "function p() flow.result({ t = type(ctx.args), v = ctx.args }) end",
       {"--args", R"("str")"},
       0,
       R"({"t":"string","v":"str"})"
       "\n",
       ""},
      // The sandbox, enumerated live. This is the case that would catch a
      // library opened here that the oracle leaves closed.
      {"sandbox",
       "function p() local r = {} for _, n in ipairs({'os','io','debug','coroutine','package','require','load','loadfile',"
       "'dofile','print','pcall'}) do r[n] = (rawget(_G, n) == nil) and 'NIL' or type(rawget(_G, n)) end "
       "r.random = tostring(math.random) flow.result(r) end",
       {},
       0,
       R"({"coroutine":"NIL","debug":"NIL","dofile":"NIL","io":"NIL","load":"NIL","loadfile":"NIL","os":"NIL","package":"NIL","pcall":"function","print":"function","random":"nil","require":"NIL"})"
       "\n",
       ""},
      // The five host tables and their exact contents, from a live state.
      {"surface",
       "local function keys(t) local r = {} for k, v in pairs(t) do r[#r+1] = k .. ':' .. type(v) end table.sort(r) "
       "return table.concat(r, ',') end "
       "function p() flow.result({ cli = keys(cli), git = keys(git), fs = keys(fs), flow = keys(flow), ctx = keys(ctx) }) end",
       {},
       0,
       R"({"cli":"planar:function,planar_agent:function,planar_agent_json:function,planar_json:function,planar_watch:function,planar_watch_json:function","ctx":"args:table,brief:function,context:function,now:number,plan_show:function,recommend_strategy:function,seed:number,task_show:function,task_touches:function","flow":"fail:function,log:function,phase:function,result:function","fs":"exists:function,mkdir:function,read:function,write:function","git":"checkout:function,clean:function,diff_name_only:function,head_sha:function,reset_hard:function"})"
       "\n",
       ""},
      // stdout stays clean while a diagnostic is being written to stderr.
      {"log",
       "function p() flow.log('hello') flow.result({ok = true}) end",
       {},
       0,
       "{\"ok\":true}\n",
       "[planar-execute] hello\n"},
      // No result at all is still a JSON document.
      {"empty", "function p() end", {}, 0, "{}\n", ""},
      // The four failure stages, each on its own exit code path (all 1 —
      // which is itself the thing being pinned, since only BadUsage is 2).
      {"syntax", "this is not lua", {}, 1, "", "planar-execute: load error: workflow:1: syntax error near 'is'\n"},
      {"init", "error('boom')", {}, 1, "", "planar-execute: init error: workflow:1: boom\n"},
      {"raise", "function p() error('kaboom') end", {}, 1, "", "planar-execute: phase error: workflow:1: kaboom\n"},
      {"fail", "function p() flow.fail('nope') end", {}, 1, "", "planar-execute: phase failed: nope\n"},
      {"sparse",
       "function p() local t = {} t[1] = 'a' t[3] = 'c' flow.result({v = t}) end",
       {},
       1,
       "",
       "planar-execute: phase error: workflow:1: sparse Lua arrays are not supported\n"},
      // git and fs with no configuration: the refusal message and the exit
      // code, not a fallback to cwd.
      {"unconfigured_git",
       "function p() git.head_sha() end",
       {},
       1,
       "",
       "planar-execute: phase error: workflow:1: git.* requires a configured worktree (--worktree)\n"},
      {"unconfigured_fs",
       "function p() fs.read('a.txt') end",
       {},
       1,
       "",
       "planar-execute: phase error: workflow:1: fs.read rejected or failed: a.txt\n"},
  };

  for (auto const& s : shapes) {
    auto const cpp = run_cpp_over_workflow(s.tag, s.workflow, s.extra);
    INFO("workflow: " << s.tag);
    CHECK(cpp.code == s.expected_code);
    CHECK(cpp.out == s.expected_out);
    CHECK(cpp.err == s.expected_err);
  }
}

TEST_CASE("planar-execute parity: fs confinement behaves identically in both engines", "[cmd][execute][parity][workflow]") {
  // Retired off the oracle by task 6541 — the JSON payload below was
  // transcribed from a live cpp/zig diff at commit 4fc09c0777cb, where the
  // two agreed byte for byte; see this file's header.
  static constexpr std::string_view k_workflow = R"(
local function try(f, ...)
  local ok, v = pcall(f, ...)
  if not ok then return "ERR" end
  if v == nil then return "OK" end
  return v
end
function p()
  flow.result({
    inside = try(fs.read, "inside.txt"),
    deep = try(fs.read, "sub/deep.txt"),
    parent = try(fs.read, "../outside.txt"),
    absolute = try(fs.read, "/etc/hosts"),
    dot = try(fs.read, "./inside.txt"),
    symlink = try(fs.read, "escape.txt"),
    through_dirlink = try(fs.read, "dirlink/deep.txt"),
    exists_symlink = try(fs.exists, "escape.txt"),
    mkdir_over_symlink = try(fs.mkdir, "dirlink"),
    write_symlink = try(fs.write, "escape.txt", "nope"),
    mkdir_nested = try(fs.mkdir, "made/deeper"),
    write_new = try(fs.write, "made/deeper/out.txt", "written"),
    write_back = try(fs.read, "made/deeper/out.txt"),
  })
end
)";

  // The sandbox root is the arena's own `proj` directory, seeded
  // identically to the way the two-binary version did. The symlinked
  // DIRECTORY is the case a text-only `..` check misses: `dirlink/deep.txt`
  // is neither absolute nor dotted.
  auto const      arena = make_arena("confine");
  std::error_code ec;
  auto const      proj = arena.cpp_root / "proj";
  std::filesystem::create_directories(proj / "sub", ec);
  {
    std::ofstream file(proj / "wf.lua", std::ios::binary);
    file << k_workflow;
  }
  {
    std::ofstream file(proj / "inside.txt", std::ios::binary);
    file << "visible";
  }
  {
    std::ofstream file(proj / "sub" / "deep.txt", std::ios::binary);
    file << "deep";
  }
  // A REAL file one level above the sandbox root, so `../outside.txt`
  // would succeed if the `..` component were ever accepted. Pointing the
  // case at a path that does not exist either way would make it pass for
  // the wrong reason.
  {
    std::ofstream file(arena.cpp_root / "outside.txt", std::ios::binary);
    file << "forbidden";
  }
  std::filesystem::create_symlink("/etc/hosts", proj / "escape.txt", ec);
  std::filesystem::create_directory_symlink("sub", proj / "dirlink", ec);

  std::vector<std::string> const args{"run", "wf.lua", "--phase", "p", "--sandbox-root", proj.string()};
  auto const                     cpp = run_pinned(cpp_bin(), args, arena.cpp_root, "cpp");

  CHECK(cpp.code == 0);
  CHECK(cpp.err.empty());
  // Pinned exactly — this is the whole document, not a substring check, so
  // a field silently dropped or renamed fails here too.
  CHECK(
      cpp.out ==
      R"({"absolute":"ERR","deep":"deep","dot":"ERR","exists_symlink":false,"inside":"visible","mkdir_nested":"OK","mkdir_over_symlink":"ERR","parent":"ERR","symlink":"ERR","through_dirlink":"ERR","write_back":"written","write_new":"OK","write_symlink":"ERR"})"
      "\n");
  // Not vacuous: the run really did read the file it was allowed to read and
  // refuse the ones it was not, rather than erroring out early and matching
  // on two identical failures.
  CHECK(cpp.out.contains("\"inside\":\"visible\""));
  CHECK(cpp.out.contains("\"symlink\":\"ERR\""));
  CHECK(cpp.out.contains("\"absolute\":\"ERR\""));
  CHECK(cpp.out.contains("\"parent\":\"ERR\""));
  // And the link was not followed out of the sandbox.
  CHECK(std::filesystem::read_symlink(proj / "escape.txt") == std::filesystem::path{"/etc/hosts"});
}

TEST_CASE("planar-execute: ctx.brief maps a missing plan to 'not found', byte-identically to the oracle",
          "[cmd][execute][parity][workflow]") {
  // Task 6125 ported the brief compiler; this is no longer a declared
  // divergence (see the retired test this replaced, and
  // src/engine/execute/CMakeLists.txt). A full happy-path differential —
  // real plan/task/claim state seeded through both binaries' own `plan
  // add`/`task add` verbs, then diffing the compiled brief body — is a
  // heavier fixture than this file's other cases build; that positive-path
  // coverage lives instead in `workflow.t.cpp`'s fake-bin_dir unit test,
  // which drives real `compile_brief` output deterministically. What THIS
  // pins is the failure-message mapping `run_allowlisted` and the oracle's
  // `state.zig` `StateError` catch agreed on byte-for-byte at commit
  // 4fc09c0777cb (task 6541 retired the live oracle comparison; the exact
  // bytes below are the transcription): a plan `ctx.brief` cannot find is
  // not the same failure shape as a process that crashed, and a port that
  // stringified the wrong exception would show up here.
  auto const arena = make_arena("brief");
  {
    std::ofstream file(arena.cpp_root / "proj" / "wf.lua", std::ios::binary);
    file << "function p() ctx.brief({plan_id = 1, task_id = 1, claim_token = 'x', problem_statement = 'y'}) end";
  }
  std::vector<std::string> const args{"run", "wf.lua", "--phase", "p"};
  auto const                     cpp = run_pinned(cpp_bin(), args, arena.cpp_root, "cpp");

  CHECK(cpp.code == 1);
  CHECK(cpp.out.empty());
  // Not vacuous: this is "plan not found" from a fresh, plan-less DB, not
  // "the brief compiler is not ported yet" or a generic subprocess failure.
  CHECK(cpp.err == "planar-execute: phase error: workflow:1: ctx.brief: plan 1 not found\n");
}

TEST_CASE("planar-execute schema --command and --compact select from the catalog", "[cmd][execute][schema][7204]") {
  auto const full = run_cpp("schema7204full", {"schema"});
  REQUIRE(full.code == 0);

  // One command, by full path or relative to the root, in either flag spelling.
  auto const one = run_cpp("schema7204one", {"schema", "--command", "planar-execute run"});
  CHECK(one.code == 0);
  CHECK(one.err.empty());
  CHECK(one.out.starts_with(R"({"name":"run",)"));
  CHECK(one.out.contains(R"("command":"planar-execute run")"));
  CHECK(one.out.size() < 4096);
  CHECK(full.out.contains(one.out.substr(0, one.out.size() - 1)));
  CHECK(run_cpp("schema7204rel", {"schema", "--command=run"}).out == one.out);

  // An unknown path: exit 2, named on stderr, nothing on stdout.
  auto const bad = run_cpp("schema7204bad", {"schema", "--command", "planar-execute run nonesuch"});
  CHECK(bad.code == 2);
  CHECK(bad.out.empty());
  CHECK(bad.err.contains("planar-execute run nonesuch"));

  // Compact: one two-key row per command, so far fewer bytes.
  auto const compact = run_cpp("schema7204compact", {"schema", "--compact"});
  CHECK(compact.code == 0);
  auto const rows = [](std::string_view text, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
      ++n;
    }
    return n;
  };
  CHECK(rows(compact.out, R"({"command":")") == rows(full.out, R"("path":[)"));
  CHECK_FALSE(compact.out.contains(R"("flags")"));
  CHECK(compact.out.size() < 40 * 1024);

  // Both: the one row, bare.
  auto const row = run_cpp("schema7204row", {"schema", "--compact", "--command", "run"});
  CHECK(row.code == 0);
  CHECK(row.out.starts_with(R"({"command":"planar-execute run","summary":")"));
}
