// @file surface.t.cpp
// @brief THE FROZEN-SURFACE LOCK (plan 996, task 6042).
//
// This file is the reason the milestone was scoped the way it was. The risk
// `planar-execute` carries is not that `git.clean` gains an off-by-one — it
// is that the host surface quietly WIDENS, one convenient helper at a time,
// until the binary is a harness again. It has happened once already, which
// is why the thing was extracted to a separate project and why the revival
// exists in its current shape at all (plan 633 D5).
//
// So the tests below assert on the surface itself rather than on any
// function's behaviour, and they do it against a LIVE `lua_State` with the
// registrar run over it — not against the manifest constant. That distinction
// is the whole value:
//
//   * `manifest.t.cpp` asks "is the DECLARATION well formed?"
//   * this file asks "is what a WORKFLOW CAN REACH exactly the declaration?"
//
// A hand-written `lua_setfield` added beside the manifest-driven loop passes
// the first and fails the second. So does a function moved between tables, a
// sixth global table, a dropped dispatch arm, and a `spawn` global registered
// outside the five tables entirely.
//
// The sandbox half works the same way: every nil'd name below was derived by
// RUNNING the oracle and enumerating its live `_G` and its live `math`, never
// by reading `openSandboxedLibs`. Task 6541 retired the live oracle
// invocation this file used to make at test time (plan 996, decision
// 963/982's oracle-retirement gate) — the enumeration below is now a pinned
// transcription of that comparison's last agreement rather than a
// re-derived one; see the TEST_CASE for the commit it was taken at.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine_execute;

namespace {

namespace ex = planar::engine::execute;

/// @brief Names present on a live sandboxed `_G`, as a set.
/// @return The global names.
auto global_names() -> std::set<std::string> {
  std::set<std::string> names;
  for (auto const& entry : ex::sandbox_globals()) {
    names.insert(entry.name);
  }
  return names;
}

/// @brief Names present on one live sandboxed table, as a set.
/// @param table The table name.
/// @return Its keys.
auto table_names(std::string_view table) -> std::set<std::string> {
  std::set<std::string> names;
  for (auto const& entry : ex::sandbox_table_entries(table)) {
    names.insert(entry.name);
  }
  return names;
}

} // namespace

TEST_CASE("the live host surface is exactly the frozen manifest", "[engine][execute][surface]") {
  auto const live = ex::registered_host_surface();

  std::vector<std::pair<std::string, std::string>> declared;
  for (auto const& entry : ex::allowed_host_fns()) {
    declared.emplace_back(entry.table, entry.name);
  }
  std::ranges::sort(declared);

  // Element-wise, so a failure names the offending entry rather than
  // reporting "two containers differ".
  REQUIRE(live.size() == declared.size());
  for (std::size_t i = 0; i < live.size(); ++i) {
    INFO("entry " << i << ": live " << live[i].first << "." << live[i].second << " vs declared " << declared[i].first << "."
                  << declared[i].second);
    CHECK(live[i] == declared[i]);
  }
}

TEST_CASE("no spawn-shaped name is reachable from a workflow, anywhere", "[engine][execute][surface]") {
  // Not just "not on the five tables" — a `spawn` GLOBAL would be exactly as
  // reachable as a `cli.spawn`, and the manifest says nothing about globals.
  // So the globals are swept too.
  auto const denied  = ex::denied_host_fns();
  auto const globals = global_names();
  for (auto const& name : denied) {
    INFO("denied name " << name);
    CHECK_FALSE(globals.contains(std::string{name}));
    for (auto const table : ex::host_tables()) {
      CHECK_FALSE(table_names(table).contains(std::string{name}));
    }
  }
}

TEST_CASE("the sandbox nils every escape hatch", "[engine][execute][surface]") {
  auto const globals = global_names();

  // Derived by enumerating the ORACLE's live `_G`, not by reading its source.
  //
  //   os / io      — os.execute, os.getenv, io.open, io.popen. The clock too:
  //                  with os gone, ctx.now is the only time source a workflow
  //                  can see, which is what makes a run reproducible.
  //   package      — and therefore package.loadlib, the dlopen door that
  //                  LUA_USE_MACOSX/LUA_USE_LINUX compile in.
  //   require      — the loader that would reach it.
  //   load /
  //   loadfile /
  //   dofile       — arbitrary chunk loading, which would make every other
  //                  restriction here advisory.
  //   debug        — debug.getregistry reaches the loaded-module table and
  //                  from there anything the host put in the registry.
  //   coroutine    — removed rather than trimmed: the hand-back model is one
  //                  clean process per phase, and a coroutine parked awaiting
  //                  an external worker is precisely where re-entrant
  //                  spawning regrew last time.
  for (auto const& name : {"os", "io", "package", "require", "load", "loadfile", "dofile", "debug", "coroutine", "loadstring"}) {
    INFO("global " << name);
    CHECK_FALSE(globals.contains(name));
  }

  // And the sandbox is not merely empty — the curated libraries really are
  // open, or every check above would pass vacuously.
  for (auto const& name : {"pcall", "error", "type", "pairs", "ipairs", "tostring", "select", "setmetatable", "math", "string",
                           "table", "utf8", "_VERSION"}) {
    INFO("global " << name);
    CHECK(globals.contains(name));
  }
  for (auto const table : ex::host_tables()) {
    INFO("host table " << table);
    CHECK(globals.contains(std::string{table}));
  }

  // math keeps everything deterministic and loses exactly the two sources
  // that are not.
  auto const maths = table_names("math");
  CHECK_FALSE(maths.contains("random"));
  CHECK_FALSE(maths.contains("randomseed"));
  CHECK(maths.contains("floor"));
  CHECK(maths.contains("type")); // Used to tell an integer from a float.
}

TEST_CASE("ctx carries the injected determinism fields and nothing else", "[engine][execute][surface]") {
  auto const                         entries = ex::sandbox_table_entries("ctx");
  std::map<std::string, std::string> typed;
  for (auto const& entry : entries) {
    typed[entry.name] = entry.type;
  }
  // Six functions plus exactly three data fields. `ctx` is the one host table
  // that legitimately carries non-functions, which is why the frozen-surface
  // comparison above filters on type — and why this case pins the
  // non-function set separately instead of letting it drift unwatched.
  CHECK(typed["now"] == "number");
  CHECK(typed["seed"] == "number");
  CHECK(typed["args"] == "table");
  CHECK(typed.size() == 9);
}

TEST_CASE("the linked Lua is the version the oracle links", "[engine][execute][surface]") {
  // Version-sensitive in ways that are easy to miss: 5.5 adds table.create,
  // and its math carries acos/asin/atan/frexp/ldexp where a default 5.4 build
  // does not. A silent bump would change what the enumerations above see
  // while every host function still appeared to work.
  CHECK(ex::lua_version() == "Lua 5.5");
  CHECK(table_names("table").contains("create"));
}

TEST_CASE("the sandbox enumeration agrees with the oracle's own", "[engine][execute][surface][parity]") {
  // Retired off the Zig oracle by task 6541 (plan 996, decision 963/982's
  // third gating condition). This case used to run the SAME enumeration
  // workflow through the oracle binary, live at test time, and compare its
  // reported `_G`/`math`/`ctx`/`cli`/`git`/`fs`/`flow` key sets against this
  // engine's own `sandbox_globals()` / `sandbox_table_entries()` — the
  // strongest form of the sandbox claim, because the expected lists were a
  // CHECKED transcription rather than a remembered one.
  //
  // The set below is that transcription, taken from a live run of the
  // oracle's `planar-execute run wf.lua --phase probe` at commit
  // 4fc09c0777cb, over the identical enumeration workflow this case used to
  // send it (see the comment on each name set below for the exact command).
  // Every entry matched this engine's own enumeration at that commit — this
  // pins that agreement rather than re-deriving it, so the case keeps
  // running once `zig/` is gone. Not loosened to a subset/superset check:
  // the whole point is exact set equality, so a name silently added or
  // dropped from either side stops matching.
  //
  // The oracle's `_G` also carried the workflow's own `probe` function,
  // which this side has no equivalent of (nothing was loaded into the
  // introspection state) — "probe" is excluded from the transcription
  // below for that reason, the same way the live comparison used to erase
  // it before diffing.
  static const std::set<std::string> k_oracle_globals{
      "_G",    "_VERSION",     "assert",   "cli",    "collectgarbage", "ctx",    "error",        "flow",
      "fs",    "getmetatable", "git",      "ipairs", "math",           "next",   "pairs",        "pcall",
      "print", "rawequal",     "rawget",   "rawlen", "rawset",         "select", "setmetatable", "string",
      "table", "tonumber",     "tostring", "type",   "utf8",           "warn",   "xpcall"};
  static const std::map<std::string, std::set<std::string>> k_oracle_tables{
      {"math", {"abs",  "acos",  "asin", "atan",  "ceil", "cos", "deg",        "exp",  "floor",
                "fmod", "frexp", "huge", "ldexp", "log",  "max", "maxinteger", "min",  "mininteger",
                "modf", "pi",    "rad",  "sin",   "sqrt", "tan", "tointeger",  "type", "ult"}},
      {"ctx", {"args", "brief", "context", "now", "plan_show", "recommend_strategy", "seed", "task_show", "task_touches"}},
      {"cli", {"planar", "planar_agent", "planar_agent_json", "planar_json", "planar_watch", "planar_watch_json"}},
      {"git", {"checkout", "clean", "diff_name_only", "head_sha", "reset_hard"}},
      {"fs", {"exists", "mkdir", "read", "write"}},
      {"flow", {"fail", "log", "phase", "result"}},
  };

  CHECK(k_oracle_globals == global_names());
  for (auto const& [table, names] : k_oracle_tables) {
    INFO("table " << table);
    CHECK(names == table_names(table));
  }
}
