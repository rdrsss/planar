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
// by reading `openSandboxedLibs`. When the oracle binary is present the
// enumeration is re-derived from it at test time and compared, which turns
// the pinned lists from a transcription into a checked one.

#include <sys/wait.h>

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

/// @brief Path to the Zig oracle binary (set by this target's CMakeLists).
/// @return The path.
auto oracle_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_ZIG_EXECUTE_BIN};
}

/// @brief Run the oracle over a workflow and return its stdout.
///
/// The environment is pinned entirely to a scratch directory. `cd` FIRST and
/// then `env` — `VAR=x cd dir && binary` does NOT export the assignment past
/// the `&&` on this platform's /bin/sh, and getting that backwards is how a
/// previous cycle wrote ten rows into the operator's live database.
/// planar-execute holds no handle itself, but it SHELLS `planar`, which
/// migrates on first use.
/// @param workflow The Lua source.
/// @param phase The phase to call.
/// @return The oracle's stdout, or unset when it is not present or failed.
auto run_oracle(std::string_view workflow, std::string_view phase) -> std::optional<std::string> {
  if (!std::filesystem::exists(oracle_bin())) {
    return std::nullopt;
  }
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_execute_surface_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "fakehome", ec);
  {
    std::ofstream file(root / "wf.lua", std::ios::binary);
    file << workflow;
  }
  auto const command = std::format("cd {} && env PLANAR_DB={}/scratch.db PLANAR_HOME={}/fakehome PLANAR_CONFIG_PATH={}/c.toml "
                                   "HOME={}/fakehome {} run wf.lua --phase {} > {}/out 2> {}/err",
                                   root.string(), root.string(), root.string(), root.string(), root.string(),
                                   oracle_bin().string(), phase, root.string(), root.string());
  int const  status  = std::system(command.c_str());
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  std::ifstream      out(root / "out", std::ios::binary);
  std::ostringstream buffer;
  buffer << out.rdbuf();
  return buffer.str();
}

/// @brief Split a comma-joined `name:type` listing (what the enumeration
/// workflows below emit) into a name set.
/// @param listing The joined listing.
/// @return The names.
auto names_from_listing(std::string_view listing) -> std::set<std::string> {
  std::set<std::string> names;
  for (auto const chunk : std::views::split(listing, ',')) {
    std::string_view const entry{chunk.begin(), chunk.end()};
    auto const             colon = entry.rfind(':');
    if (colon != std::string_view::npos) {
      names.emplace(entry.substr(0, colon));
    }
  }
  return names;
}

/// @brief Pull one `"key":"value"` string field out of a flat JSON object.
///
/// Deliberately crude — the documents it reads are produced by the two
/// enumeration workflows below and contain no escapes.
/// @param json The document.
/// @param key The field.
/// @return The raw value.
auto flat_field(std::string_view json, std::string_view key) -> std::string {
  auto const marker = std::format("\"{}\":\"", key);
  auto const at     = json.find(marker);
  if (at == std::string_view::npos) {
    return {};
  }
  auto const start = at + marker.size();
  return std::string{json.substr(start, json.find('"', start) - start)};
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
  // The strongest form of the sandbox claim: derive the list from the oracle
  // AT TEST TIME and compare, so the pinned lists above are a checked
  // transcription rather than a remembered one.
  //
  // SKIP, not fail, when the oracle is absent — zig/zig-out/bin/* are build
  // artifacts, not checked-in files (D6).
  static constexpr std::string_view k_enumerate = R"(
local function keys(t)
  local r = {}
  for k, v in pairs(t) do r[#r+1] = k .. ":" .. type(v) end
  table.sort(r)
  return table.concat(r, ",")
end
function probe()
  flow.result({ G = keys(_G), math = keys(math), ctx = keys(ctx), cli = keys(cli),
                git = keys(git), fs = keys(fs), flow = keys(flow) })
end
)";

  auto const oracle = run_oracle(k_enumerate, "probe");
  if (!oracle.has_value()) {
    SKIP("Zig oracle not built (zig/zig-out/bin/planar-execute)");
  }

  // The oracle's `_G` also carries the workflow's own `probe` function, which
  // this side has no equivalent of (nothing was loaded into the introspection
  // state), so it is removed before comparing.
  auto oracle_globals = names_from_listing(flat_field(*oracle, "G"));
  oracle_globals.erase("probe");
  CHECK(oracle_globals == global_names());

  for (auto const table : {"math", "ctx", "cli", "git", "fs", "flow"}) {
    INFO("table " << table);
    CHECK(names_from_listing(flat_field(*oracle, table)) == table_names(table));
  }
}
