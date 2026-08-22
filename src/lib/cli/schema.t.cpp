// @file schema.t.cpp
// @brief Unit + parity tests for `planar.cli.schema::schema_json` (plan
// 996, task cpp-cli-schema-catalog).
//
// Three tiers of proof, matching the task brief's "prove SHAPE equality
// for the modeled subset" + "prove against the REAL consumer, not a
// mock":
//
//   1. `[unit]` — structural sanity on a synthetic fixture (nesting,
//      inherited flags, flag groups, env metadata, hidden-flag exclusion,
//      the always-null `deprecated` / always-empty `docs` / always-`none`
//      `completion` divergences this port is forced into — see
//      schema.cppm's file comment).
//   2. `[parity]` — deep-equality of the C++ emitter's output for the
//      task/task-add/task-done/planar-agent-fail modeled subset against
//      the REAL `./zig/zig-out/bin/planar` / `planar-agent schema`
//      catalog, parsed generically (glz::generic) so the comparison is a
//      structural JSON diff, not a string-equality assumption. SKIPs
//      (not fails) when the reference binaries are not built, matching
//      help.t.cpp/parser.t.cpp's established pattern.
//   3. `[lint-parity]` — runs the actual, UNMODIFIED
//      `zig/tools/cli_usage_lint` tool (compiled standalone at test time
//      via `zig build-exe`, never through zig/build.zig) against
//      `planar_cli_schema_stub`'s live `schema` output, proving the real
//      consumer accepts C++-emitted JSON both on the happy path (a valid
//      doc reference is clean) and the enforcement path (an invalid flag
//      reference is caught) — so the "clean" result isn't just because
//      the tool never actually parsed anything. SKIPs when `zig` is not
//      on PATH or the lint tool source is missing.
#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS

#include <glaze/glaze.hpp>

import std;
import planar.cli;

namespace {

using planar::cli::cmd;
using planar::cli::flag;
using planar::cli::flag_group;
using planar::cli::flag_group_mode;
using planar::cli::positional;

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

/// @brief Same modeled subset as help.t.cpp/parser.t.cpp (see those files'
/// header comments for why these leaves were chosen); duplicated here
/// because each `*.t.cpp` compiles as its own translation unit with no
/// shared test header in this module.
auto make_planar_root() -> cmd {
  cmd add{
      .name = "add",
      .desc = "Create a new task.",
      .flags =
          {
              flag{.long_name = "--body"},
              flag{.long_name = "--scope"},
              flag{.long_name = "--next-action"},
              flag{.long_name = "--due"},
              flag{.long_name = "--plan", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--parent", .value_kind = planar::cli::kind::integer},
              flag{.long_name = "--slug"},
              flag{.long_name = "--priority", .value_kind = planar::cli::kind::integer, .default_value = std::int64_t{100}},
              flag{.long_name = "--editor", .value_kind = planar::cli::kind::boolean, .default_value = true},
              flag{.long_name = "--no-auto-promote", .value_kind = planar::cli::kind::boolean, .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "title", .required = true}},
  };
  cmd done{
      .name = "done",
      .desc = "Mark a task as done (single-arg form; Go supports variadic).",
      .flags =
          {
              flag{.long_name = "--scope"},
              flag{.long_name     = "--force",
                   .desc          = "Override active-claim guard and flip status anyway.",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
      .positionals = {positional{.name = "task-id", .required = true}},
  };
  cmd task{
      .name = "task",
      .desc = "Manage tasks.",
      .cmds = {std::move(add), std::move(done)},
  };
  return cmd{.name = "planar", .cmds = {std::move(task)}};
}

auto make_planar_agent_root() -> cmd {
  cmd fail{
      .name = "fail",
      .desc = "Atomically fail the work session: task \xe2\x86\x92 todo, claim \xe2\x86\x92 aborted.",
      .flags =
          {
              flag{.long_name = "--claim", .desc = "Claim token returned by pull/claim", .required = true},
              flag{.long_name = "--reason", .desc = "Failure reason recorded on the claim and action", .required = true},
              flag{
                  .long_name     = "--category",
                  .desc          = "Closed failure category (default: unknown)",
                  .value_kind    = planar::cli::kind::choice,
                  .choices       = {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"},
                  .default_value = std::string("unknown"),
              },
              flag{.long_name     = "--no-locality-probe",
                   .desc          = "Skip the git locality probe and commit collection",
                   .value_kind    = planar::cli::kind::boolean,
                   .default_value = false},
              flag{.long_name = "--json", .value_kind = planar::cli::kind::boolean, .default_value = false},
          },
  };
  return cmd{.name = "planar-agent", .cmds = {std::move(fail)}};
}

/// @brief A small synthetic tree (independent of any real Planar verb)
/// exercising shapes the task/task-add/task-done/fail subset above never
/// touches: a parent-declared flag inherited by a child (`"source":
/// "inherited"` vs `"local"`), an env-backed flag, a flag group, a hidden
/// flag (must be excluded), and aliases.
auto make_synthetic_root() -> cmd {
  cmd run{
      .name      = "run",
      .aliases   = {"exec"},
      .desc      = "Run command",
      .long_desc = "Run command with docs.",
      .flags     = {flag{.long_name = "--name", .desc = "Name value", .value_name = "NAME", .required = true, .env = "TOOL_NAME"},
                    flag{.long_name = "--secret", .hidden = true}},
      .positionals = {positional{.name = "target", .desc = "Target value"}},
      .flag_groups = {flag_group{.name  = "run-input",
                                 .mode  = flag_group_mode::required_one,
                                 .flags = {"--verbose", "--name"},
                                 .desc  = "Choose a run input."}},
  };
  cmd tool{
      .name  = "tool",
      .desc  = "Test tool",
      .flags = {flag{.long_name = "--verbose", .short_name = 'v', .default_value = false}},
      .cmds  = {std::move(run)},
  };
  return tool;
}

// ---------------------------------------------------------------------------
// Subprocess + generic-JSON helpers.
// ---------------------------------------------------------------------------

/// @brief Run `bin arg1 arg2 ...` with `env_assignment` (may be empty)
/// prefixed as a shell `KEY=VALUE` export, redirecting stdout+stderr to a
/// scratch file, and return its contents plus the raw `std::system` exit
/// status. Mirrors help.t.cpp's `capture_stdout` idiom, extended with an
/// env-var prefix for `PLANAR_DB` (cli_usage_lint.zig's own
/// `loadSchema` sets this per-invocation to isolate the schema dump from
/// any real database).
auto capture(std::string const& env_assignment, std::string const& bin, std::vector<std::string> const& args)
    -> std::pair<std::string, int> {
  auto const out_path =
      std::filesystem::temp_directory_path() /
      std::format("planar_cli_schema_capture_{}.txt", std::chrono::steady_clock::now().time_since_epoch().count());
  std::string cmd_str;
  if (!env_assignment.empty()) {
    cmd_str += env_assignment + " ";
  }
  cmd_str += "'" + bin + "'";
  for (auto const& a : args) {
    cmd_str += " '" + a + "'";
  }
  cmd_str += " > " + out_path.string() + " 2>&1";
  int const status = std::system(cmd_str.c_str());

  std::ifstream   in(out_path, std::ios::binary);
  std::string     contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::error_code ec;
  std::filesystem::remove(out_path, ec);
  return {contents, status};
}

/// @brief Run `<bin> schema` the same way cli_usage_lint.zig's
/// `loadSchema` does (a fresh scratch `PLANAR_DB`) and return stdout.
/// FAILs the current test if the binary does not exit 0.
auto capture_reference_schema(std::string const& bin) -> std::string {
  auto const db_path =
      std::filesystem::temp_directory_path() /
      std::format("planar_cli_schema_capture_db_{}.sqlite", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const [out, status] = capture("PLANAR_DB='" + db_path.string() + "'", bin, {"schema"});
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(std::filesystem::path(db_path.string() + "-wal"), ec);
  std::filesystem::remove(std::filesystem::path(db_path.string() + "-shm"), ec);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    FAIL("reference `" << bin << " schema` did not exit 0 (raw status " << status << "):\n" << out);
  }
  return out;
}

auto parse_generic(std::string const& text) -> glz::generic {
  auto parsed = glz::read_json<glz::generic>(text);
  if (!parsed) {
    FAIL("failed to parse JSON: " << glz::format_error(parsed.error(), text));
  }
  return *parsed;
}

/// @brief Find the `commands[]` entry whose `"command"` field equals
/// `command_path`, or nullopt.
auto find_command(glz::generic const& catalog, std::string_view command_path) -> std::optional<glz::generic> {
  if (!catalog.contains("commands")) {
    return std::nullopt;
  }
  auto const& commands = catalog.at("commands").get<glz::generic::array_t>();
  for (auto const& c : commands) {
    if (c.contains("command") && c.at("command").get<std::string>() == command_path) {
      return c;
    }
  }
  return std::nullopt;
}

/// @brief Structural deep-equality over the generic JSON tree. Object
/// comparison is key-set + per-key recursive equality (order-independent,
/// since JSON object member order is not semantically load-bearing);
/// array comparison is order-sensitive (declaration order IS load-bearing
/// for `flags`/`positionals`/`subcommands`). On mismatch, appends a
/// human-readable path to `diff` and returns false.
auto deep_equal(glz::generic const& a, glz::generic const& b, std::string const& path, std::vector<std::string>& diff) -> bool {
  if (a.is_object() && b.is_object()) {
    auto const& oa = a.get<glz::generic::object_t>();
    auto const& ob = b.get<glz::generic::object_t>();
    bool        ok = true;
    if (oa.size() != ob.size()) {
      diff.push_back(std::format("{}: key count {} vs {}", path, oa.size(), ob.size()));
      ok = false;
    }
    for (auto const& [k, v] : oa) {
      if (!b.contains(k)) {
        diff.push_back(std::format("{}.{}: missing on right side", path, k));
        ok = false;
        continue;
      }
      ok = deep_equal(v, b.at(k), path + "." + k, diff) && ok;
    }
    for (auto const& [k, v] : ob) {
      if (!a.contains(k)) {
        diff.push_back(std::format("{}.{}: missing on left side", path, k));
        ok = false;
      }
    }
    return ok;
  }
  if (a.is_array() && b.is_array()) {
    auto const& aa = a.get<glz::generic::array_t>();
    auto const& ab = b.get<glz::generic::array_t>();
    if (aa.size() != ab.size()) {
      diff.push_back(std::format("{}: array length {} vs {}", path, aa.size(), ab.size()));
      return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < aa.size(); ++i) {
      ok = deep_equal(aa[i], ab[i], std::format("{}[{}]", path, i), diff) && ok;
    }
    return ok;
  }
  if (a.is_null() && b.is_null()) {
    return true;
  }
  if (a.is_boolean() && b.is_boolean()) {
    if (a.get<bool>() != b.get<bool>()) {
      diff.push_back(std::format("{}: {} vs {}", path, a.get<bool>(), b.get<bool>()));
      return false;
    }
    return true;
  }
  if (a.is_string() && b.is_string()) {
    if (a.get<std::string>() != b.get<std::string>()) {
      diff.push_back(std::format("{}: \"{}\" vs \"{}\"", path, a.get<std::string>(), b.get<std::string>()));
      return false;
    }
    return true;
  }
  if (a.is_number() && b.is_number()) {
    if (a.get<double>() != b.get<double>()) {
      diff.push_back(std::format("{}: {} vs {}", path, a.get<double>(), b.get<double>()));
      return false;
    }
    return true;
  }
  diff.push_back(std::format("{}: type mismatch", path));
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// [unit] structural sanity.
// ---------------------------------------------------------------------------

TEST_CASE("schema_json: emits the flat-catalog envelope", "[schema][unit]") {
  auto const root    = make_planar_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  CHECK(catalog.at("schemaVersion").get<double>() == 1.0);
  CHECK(catalog.at("layout").get<std::string>() == "flat");
  CHECK(catalog.at("root").get<std::string>() == "planar");
  CHECK_FALSE(catalog.contains("commandTree")); // include_command_tree defaults false
}

TEST_CASE("schema_json: leaf command carries flags, positionals, command path", "[schema][unit]") {
  auto const root    = make_planar_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  auto const add     = find_command(catalog, "planar task add");
  REQUIRE(add.has_value());
  CHECK(add->at("name").get<std::string>() == "add");
  CHECK(add->at("summary").get<std::string>() == "Create a new task.");
  CHECK(add->at("description").get<std::string>() == "Create a new task.");
  CHECK(add->at("path").get<glz::generic::array_t>().size() == 2);
  CHECK(add->at("positionals").get<glz::generic::array_t>().size() == 1);
  CHECK(add->at("positionals")[0].at("name").get<std::string>() == "title");
  CHECK(add->at("flags").get<glz::generic::array_t>().size() == 11);

  auto const priority_it = std::ranges::find_if(add->at("flags").get<glz::generic::array_t>(), [](glz::generic const& f) {
    return f.at("long").get<std::string>() == "--priority";
  });
  REQUIRE(priority_it != add->at("flags").get<glz::generic::array_t>().end());
  CHECK(priority_it->at("kind").get<std::string>() == "int");
  CHECK(priority_it->at("default").get<double>() == 100.0);
  CHECK(priority_it->at("valueName").get<std::string>() == "N");
  CHECK(priority_it->at("source").get<std::string>() == "local");
}

TEST_CASE("schema_json: parent node lists only its modeled subcommands", "[schema][unit]") {
  auto const root    = make_planar_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  auto const task    = find_command(catalog, "planar task");
  REQUIRE(task.has_value());
  auto const subs = task->at("subcommands").get<glz::generic::array_t>();
  REQUIRE(subs.size() == 2);
  CHECK(subs[0].get<std::string>() == "add");
  CHECK(subs[1].get<std::string>() == "done");
  CHECK(task->at("flags").get<glz::generic::array_t>().empty());
}

TEST_CASE("schema_json: choice flag carries choices + string default", "[schema][unit]") {
  auto const root    = make_planar_agent_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  auto const fail    = find_command(catalog, "planar-agent fail");
  REQUIRE(fail.has_value());
  auto const& flags = fail->at("flags").get<glz::generic::array_t>();
  auto const  category_it =
      std::ranges::find_if(flags, [](glz::generic const& f) { return f.at("long").get<std::string>() == "--category"; });
  REQUIRE(category_it != flags.end());
  CHECK(category_it->at("kind").get<std::string>() == "choice");
  CHECK(category_it->at("default").get<std::string>() == "unknown");
  CHECK(category_it->at("choices").get<glz::generic::array_t>().size() == 6);
  auto const claim_it =
      std::ranges::find_if(flags, [](glz::generic const& f) { return f.at("long").get<std::string>() == "--claim"; });
  REQUIRE(claim_it != flags.end());
  CHECK(claim_it->at("required").get<bool>() == true);
}

TEST_CASE("schema_json: divergence fields this port cannot source are always the Zig empty-default shape",
          "[schema][unit][divergence]") {
  auto const root    = make_planar_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  auto const add     = find_command(catalog, "planar task add");
  REQUIRE(add.has_value());
  CHECK(add->at("deprecated").is_null());
  auto const& docs = add->at("docs");
  CHECK(docs.at("examples").get<glz::generic::array_t>().empty());
  CHECK(docs.at("homepage").get<std::string>().empty());
  auto const& flags = add->at("flags").get<glz::generic::array_t>();
  for (auto const& f : flags) {
    CHECK(f.at("deprecated").is_null());
    CHECK(f.at("completion").at("kind").get<std::string>() == "none");
  }
  auto const& positionals = add->at("positionals").get<glz::generic::array_t>();
  for (auto const& p : positionals) {
    CHECK(p.at("completion").at("kind").get<std::string>() == "none");
  }
}

TEST_CASE("schema_json: inherited flags, flag groups, env metadata, hidden exclusion, aliases", "[schema][unit][synthetic]") {
  auto const root    = make_synthetic_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));
  auto const run     = find_command(catalog, "tool run");
  REQUIRE(run.has_value());
  REQUIRE(run->at("aliases").get<glz::generic::array_t>().size() == 1);
  CHECK(run->at("aliases")[0].get<std::string>() == "exec");
  CHECK(run->at("description").get<std::string>() == "Run command with docs."); // long_desc wins over desc

  auto const& flags = run->at("flags").get<glz::generic::array_t>();
  // --secret is hidden on `run` and must be excluded entirely.
  CHECK(std::ranges::find_if(flags, [](glz::generic const& f) { return f.at("long").get<std::string>() == "--secret"; }) ==
        flags.end());
  // --verbose is inherited from `tool`; --name is declared locally on `run`.
  auto const verbose_it =
      std::ranges::find_if(flags, [](glz::generic const& f) { return f.at("long").get<std::string>() == "--verbose"; });
  REQUIRE(verbose_it != flags.end());
  CHECK(verbose_it->at("source").get<std::string>() == "inherited");
  auto const name_it =
      std::ranges::find_if(flags, [](glz::generic const& f) { return f.at("long").get<std::string>() == "--name"; });
  REQUIRE(name_it != flags.end());
  CHECK(name_it->at("source").get<std::string>() == "local");
  CHECK(name_it->at("env").get<std::string>() == "TOOL_NAME");
  CHECK(name_it->at("envBehavior").get<std::string>() == "cli-run-fallback");

  auto const& groups = run->at("flagGroups").get<glz::generic::array_t>();
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].at("name").get<std::string>() == "run-input");
  CHECK(groups[0].at("mode").get<std::string>() == "required_one");

  // Root itself: aliases surface on the cmd's OWN command entry.
  auto const tool_root = find_command(catalog, "tool");
  REQUIRE(tool_root.has_value());
  auto const run_entry = find_command(catalog, "tool run");
  REQUIRE(run_entry.has_value());
}

/// @brief B3 (M2 boundary review, plan 996 task 6066): a VISIBLE child of a
/// HIDDEN parent must be OMITTED from the catalog entirely, matching
/// zig/vendor/etcli/src/cli/schema.zig's `renderDescendantCommands`, which
/// `continue`s past a hidden child WITHOUT recursing into it — the whole
/// subtree is pruned, not just the hidden node itself. Before the fix,
/// `schema_json` walked via `all_nodes` (which flattens unconditionally)
/// and filtered per-node, so `secret visible-child` slipped through even
/// though `secret` itself was correctly excluded — this is the
/// non-vacuous proof: the pre-fix code passes the "hidden node itself
/// excluded" half of this test and fails the "descendant excluded" half.
auto make_hidden_subtree_root() -> cmd {
  cmd visible_child{
      .name = "visible-child",
      .desc = "A visible leaf nested under a hidden parent.",
  };
  cmd hidden_parent{
      .name   = "secret",
      .hidden = true,
      .desc   = "A hidden group whose children must not surface either.",
      .cmds   = {std::move(visible_child)},
  };
  cmd visible_sibling{
      .name = "public",
      .desc = "An ordinary visible leaf, sibling of the hidden group.",
  };
  return cmd{.name = "root", .cmds = {std::move(hidden_parent), std::move(visible_sibling)}};
}

TEST_CASE("schema_json: a visible child of a hidden parent is pruned along with the hidden subtree",
          "[schema][unit][break-probe]") {
  auto const root    = make_hidden_subtree_root();
  auto const catalog = parse_generic(planar::cli::schema_json(root));

  // The hidden node itself is excluded (this half held even before the fix).
  CHECK_FALSE(find_command(catalog, "root secret").has_value());
  // The VISIBLE child of that hidden node must ALSO be excluded — this is
  // the assertion that fails against the pre-fix `all_nodes`-then-filter
  // implementation.
  CHECK_FALSE(find_command(catalog, "root secret visible-child").has_value());
  // An ordinary visible sibling, unaffected by the hidden subtree, must
  // still be present — proves the walk isn't just pruning everything.
  CHECK(find_command(catalog, "root public").has_value());

  // Cross-check against the `commands` array directly: no entry's `path`
  // should ever start with "secret".
  auto const& commands = catalog.at("commands").get<glz::generic::array_t>();
  for (auto const& c : commands) {
    auto const& path = c.at("path").get<glz::generic::array_t>();
    if (!path.empty()) {
      CHECK(path[0].get<std::string>() != "secret");
    }
  }
}

// ---------------------------------------------------------------------------
// [parity] structural equality against the real reference binaries.
// ---------------------------------------------------------------------------

TEST_CASE("parity: planar task add / task done schema entries structurally match the reference binary", "[schema][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_PLANAR_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }
  auto const reference = parse_generic(capture_reference_schema(zig_bin.string()));
  auto const mine      = parse_generic(planar::cli::schema_json(make_planar_root()));

  for (auto const* leaf : {"planar task add", "planar task done"}) {
    auto const ref_entry = find_command(reference, leaf);
    auto const my_entry  = find_command(mine, leaf);
    REQUIRE(ref_entry.has_value());
    REQUIRE(my_entry.has_value());
    std::vector<std::string> diff;
    bool const               equal = deep_equal(*my_entry, *ref_entry, leaf, diff);
    INFO(leaf);
    for (auto const& d : diff) {
      UNSCOPED_INFO(d);
    }
    CHECK(equal);
  }

  // The parent `task` node necessarily diverges on two fields, both
  // because the fixture in this file (shared with help.t.cpp/
  // parser.t.cpp, neither of which needed the parent node's own prose or
  // full subcommand list) only models the leaves those files needed:
  //   - `subcommands`: this port's tree only has `add`/`done` as children;
  //     the real `planar task` node has fifteen.
  //   - `description`: the real `task` node declares a multi-paragraph
  //     `long_desc`; the fixture's `task` node never sets one, so
  //     `description` here falls back to `summary` ("Manage tasks.")
  //     instead of the real long-form prose. This is a fixture-content
  //     gap, not an emitter defect — `schema_json` reads `long_desc`
  //     correctly (see the "inherited flags..." unit test's `tool run`
  //     assertion, which DOES declare and observe a `long_desc`).
  // Both divergences are asserted as EXPECTED here (not silently
  // dropped): the subset check proves the real value is a superset, and
  // the description check proves ours is exactly the fixture's own
  // no-long_desc fallback rather than some other unexpected string.
  // Everything else on this entry matches the reference exactly.
  auto const ref_task = find_command(reference, "planar task");
  auto const my_task  = find_command(mine, "planar task");
  REQUIRE(ref_task.has_value());
  REQUIRE(my_task.has_value());
  auto const& ref_subs = ref_task->at("subcommands").get<glz::generic::array_t>();
  auto const& my_subs  = my_task->at("subcommands").get<glz::generic::array_t>();
  for (auto const& s : my_subs) {
    CHECK(std::ranges::any_of(ref_subs, [&](glz::generic const& r) { return r.get<std::string>() == s.get<std::string>(); }));
  }
  CHECK(my_subs.size() < ref_subs.size()); // proves this really is the expected subset divergence, not silent breakage
  CHECK(my_task->at("description").get<std::string>() == my_task->at("summary").get<std::string>());
  CHECK(ref_task->at("description").get<std::string>() != ref_task->at("summary").get<std::string>());

  std::vector<std::string> task_diff;
  auto                     ref_task_no_subs = *ref_task;
  auto                     my_task_no_subs  = *my_task;
  ref_task_no_subs.at("subcommands")        = glz::generic::array_t{};
  my_task_no_subs.at("subcommands")         = glz::generic::array_t{};
  ref_task_no_subs.at("description")        = std::string{};
  my_task_no_subs.at("description")         = std::string{};
  bool const task_equal = deep_equal(my_task_no_subs, ref_task_no_subs, "planar task (minus subcommands/description)", task_diff);
  for (auto const& d : task_diff) {
    UNSCOPED_INFO(d);
  }
  CHECK(task_equal);
}

TEST_CASE("parity: planar-agent fail schema entry structurally matches the reference binary", "[schema][parity]") {
  const std::filesystem::path zig_bin{PLANAR_ZIG_PLANAR_AGENT_BIN};
  if (!std::filesystem::exists(zig_bin)) {
    SKIP(std::format("zig reference binary not built at {} — build it under zig/ (zig build) first", zig_bin.string()));
  }
  auto const reference = parse_generic(capture_reference_schema(zig_bin.string()));
  auto const mine      = parse_generic(planar::cli::schema_json(make_planar_agent_root()));

  auto const ref_entry = find_command(reference, "planar-agent fail");
  auto const my_entry  = find_command(mine, "planar-agent fail");
  REQUIRE(ref_entry.has_value());
  REQUIRE(my_entry.has_value());
  std::vector<std::string> diff;
  bool const               equal = deep_equal(*my_entry, *ref_entry, "planar-agent fail", diff);
  for (auto const& d : diff) {
    UNSCOPED_INFO(d);
  }
  CHECK(equal);
}

// ---------------------------------------------------------------------------
// [lint-parity] the UNMODIFIED zig/tools/cli_usage_lint tool, run live.
// ---------------------------------------------------------------------------

namespace {

/// @brief Distinguishes WHY `build_lint_tool` didn't hand back a usable
/// binary. B5 (M2 boundary review, plan 996 task 6066): the original
/// version of this helper returned `nullopt` for three entirely different
/// situations — no `zig` on PATH, the source file missing, AND a non-zero
/// `zig build-exe` — and the caller SKIPped identically in every case. That
/// collapses "the reference tool isn't available in this environment" (a
/// legitimate reason to SKIP) into "the reference tool IS available and its
/// build is BROKEN" (a real regression — e.g. a zig toolchain bump breaking
/// `cli_usage_lint.zig` itself — that must FAIL the suite, not go green by
/// silently skipping). `unavailable` keeps the original SKIP behavior;
/// `broken` is new and must propagate to a FAIL.
enum class lint_tool_build_outcome : std::uint8_t { unavailable, broken, built };

/// @brief The outcome of `build_lint_tool`, plus enough evidence to act on
/// it: `bin_path` is set only when `outcome == built`; `log` carries
/// `build.log`'s contents whenever a build was actually attempted (i.e.
/// `outcome == broken` or `built`) — the original version wrote this file
/// and never read it back, so a broken build's diagnostic was silently
/// discarded even when SKIP was wrong.
struct lint_tool_build_result {
  lint_tool_build_outcome              outcome = lint_tool_build_outcome::unavailable;
  std::optional<std::filesystem::path> bin_path;
  std::string                          log;
};

/// @brief Compile `src` standalone (never through zig/build.zig — this
/// task must not touch anything under `zig/`) into a scratch binary.
/// `src` is a parameter (not hardcoded to `PLANAR_ZIG_CLI_USAGE_LINT_SRC`)
/// specifically so a test can inject a deliberately-broken source file and
/// observe `outcome == broken` without needing the real tool to be broken.
auto build_lint_tool(std::filesystem::path const& src) -> lint_tool_build_result {
  if (std::system("command -v zig > /dev/null 2>&1") != 0) {
    return {.outcome = lint_tool_build_outcome::unavailable};
  }
  if (!std::filesystem::exists(src)) {
    return {.outcome = lint_tool_build_outcome::unavailable};
  }
  auto const work_dir = std::filesystem::temp_directory_path() /
                        std::format("planar_cli_lint_build_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(work_dir);
  auto const        out_bin   = work_dir / "cli_usage_lint";
  auto const        cache_dir = work_dir / ".zig-cache";
  auto const        log_path  = work_dir / "build.log";
  std::string const cmd_str =
      std::format("zig build-exe '{}' -O Debug --name cli_usage_lint -femit-bin='{}' --cache-dir '{}' > '{}' 2>&1", src.string(),
                  out_bin.string(), cache_dir.string(), log_path.string());
  int const status = std::system(cmd_str.c_str());

  std::string log_text;
  {
    std::ifstream in(log_path, std::ios::binary);
    log_text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::exists(out_bin)) {
    // `zig` is on PATH and the source file exists — the environment IS
    // capable of building the reference tool, so a build failure here is a
    // real regression to fail on, not an unavailable-environment SKIP.
    return {.outcome = lint_tool_build_outcome::broken, .bin_path = std::nullopt, .log = log_text};
  }
  return {.outcome = lint_tool_build_outcome::built, .bin_path = out_bin, .log = log_text};
}

/// @brief Convenience overload for the real caller: builds the actual,
/// unmodified `zig/tools/cli_usage_lint.zig`.
auto build_lint_tool() -> lint_tool_build_result {
  return build_lint_tool(std::filesystem::path{PLANAR_ZIG_CLI_USAGE_LINT_SRC});
}

} // namespace

// ---------------------------------------------------------------------------
// Break-probe: prove `build_lint_tool` actually distinguishes "unavailable"
// from "broken" — the exact trichotomy the real [lint-parity] test below
// relies on to FAIL rather than SKIP on a broken reference build.
// ---------------------------------------------------------------------------

TEST_CASE("build_lint_tool: a missing source path is 'unavailable', not 'broken'", "[schema][lint-parity][break-probe]") {
  auto const result = build_lint_tool(std::filesystem::path{"/nonexistent/planar_cli_lint_does_not_exist.zig"});
  CHECK(result.outcome == lint_tool_build_outcome::unavailable);
  CHECK_FALSE(result.bin_path.has_value());
}

TEST_CASE("build_lint_tool: a zig source file that fails to compile is 'broken', not 'unavailable' — must FAIL, not SKIP",
          "[schema][lint-parity][break-probe]") {
  if (std::system("command -v zig > /dev/null 2>&1") != 0) {
    SKIP("`zig` not on PATH — cannot exercise the broken-build path (this SKIP itself is the 'unavailable' case, covered "
         "by the previous test)");
  }
  auto const scratch_dir =
      std::filesystem::temp_directory_path() /
      std::format("planar_cli_lint_broken_src_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(scratch_dir);
  auto const broken_src = scratch_dir / "broken.zig";
  {
    std::ofstream out(broken_src, std::ios::trunc);
    out << "this is not valid zig source at all {{{ syntax error\n";
  }

  auto const result = build_lint_tool(broken_src);
  // Non-vacuous: `zig` IS on PATH and the source file DOES exist, so a
  // pre-fix implementation (which never distinguished this from
  // "unavailable") would also have returned nullopt/unavailable here —
  // this assertion is exactly what would have failed before B5's fix.
  CHECK(result.outcome == lint_tool_build_outcome::broken);
  CHECK_FALSE(result.bin_path.has_value());
  CHECK_FALSE(result.log.empty()); // build.log was actually captured and read back, not just written and discarded.

  std::error_code ec;
  std::filesystem::remove_all(scratch_dir, ec);
}

TEST_CASE("lint-parity: the unmodified zig cli_usage_lint tool accepts and enforces C++-emitted schema output",
          "[schema][lint-parity]") {
  const std::filesystem::path stub_bin{PLANAR_CLI_SCHEMA_STUB_BIN};
  if (!std::filesystem::exists(stub_bin)) {
    SKIP(std::format("schema stub binary not built at {}", stub_bin.string()));
  }
  auto const lint_build = build_lint_tool();
  if (lint_build.outcome == lint_tool_build_outcome::unavailable) {
    SKIP("`zig` not on PATH, or zig/tools/cli_usage_lint.zig is missing — cannot build the reference lint tool");
  }
  if (lint_build.outcome == lint_tool_build_outcome::broken) {
    // B5: `zig` IS on PATH and the source file DOES exist, so this is a
    // real regression (e.g. a zig toolchain bump breaking
    // cli_usage_lint.zig itself), not an unavailable-environment SKIP.
    FAIL("zig/tools/cli_usage_lint.zig failed to build even though `zig` is on PATH and the source exists — build.log:\n"
         << lint_build.log);
  }
  auto const& lint_tool = lint_build.bin_path;

  // Sanity: the stub really does answer `<bin> schema` the way a real
  // Planar binary would (this is the shape the lint tool's own
  // `loadSchema` expects: exit 0, JSON on stdout).
  auto const [stub_out, stub_status] = capture("", stub_bin.string(), {"schema"});
  REQUIRE(WIFEXITED(stub_status));
  REQUIRE(WEXITSTATUS(stub_status) == 0);
  auto const stub_catalog = parse_generic(stub_out);
  REQUIRE(find_command(stub_catalog, "planar task add").has_value());

  auto const scratch_root =
      std::filesystem::temp_directory_path() /
      std::format("planar_cli_lint_scratch_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(scratch_root / "agents");
  std::filesystem::create_directories(scratch_root / "skills" / "src");
  std::filesystem::create_directories(scratch_root / "docs");

  auto write_doc = [&](std::string_view text) {
    std::ofstream out(scratch_root / "docs" / "sample.md", std::ios::trunc);
    out << text;
  };

  SECTION("clean: a doc referencing a real modeled flag passes") {
    write_doc("Run `planar task add --json` to create a task.\n");
    auto const [out, status] = capture("", lint_tool->string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(out.find("clean") != std::string::npos);
  }

  SECTION("enforcement: a doc referencing a flag the C++ catalog does not expose is caught") {
    write_doc("Run `planar task add --this-flag-does-not-exist` to create a task.\n");
    auto const [out, status] = capture("", lint_tool->string(), {scratch_root.string(), stub_bin.string()});
    INFO(out);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(out.find("planar task add") != std::string::npos);
    CHECK(out.find("--this-flag-does-not-exist") != std::string::npos);
  }

  std::error_code ec;
  std::filesystem::remove_all(scratch_root, ec);
}
