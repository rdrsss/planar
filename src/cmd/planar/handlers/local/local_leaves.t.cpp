// @file local_leaves.t.cpp
// @brief In-process tests for the five `planar local` leaves wired by plan
// 996, task 6189.
//
// ## EVERY CASE ASSERTS PERSISTED STATE, NOT ONLY STDOUT
//
// The same discipline `runs_leaves.t.cpp` and `annotate_leaves.t.cpp`
// established, translated to the state this family actually owns. `local`
// touches NO database: its durable state is two JSON manifests
// (`<sandbox>/{skills,agents}/.link-manifest.json`) and the vendor copies
// under `<home>/.claude/`, `<home>/.agents/`, `<home>/.codex/` and the other
// presence-marked vendor directories.
// So `manifest_rows` and `install_rows` play the part `run_snapshot` plays
// there, and they preserve exactly the distinction row assertions exist for:
//
//   - `manifest_rows` renders an ABSENT manifest file as the literal
//     `<NO-MANIFEST>` and an EXISTING-but-empty one as `<EMPTY>`. Those are
//     genuinely different states — `local link --vendor nope` writes an
//     entry with zero links, while a source never linked at all leaves the
//     file untouched — and every leaf here prints identical stdout in the
//     two cases.
//   - `install_state` renders a path that is a symlink, a regular file, or
//     absent as three DIFFERENT tokens, because `local list` reports
//     `live` for a non-symlink install too (see link.cppm's
//     `classify_existing`). Reading the manifest alone cannot tell a live
//     symlink from a regular file left behind by a failed unlink.
//
// ## THE `--vendor` FILTER IS PROVEN TO EXCLUDE, THREE WAYS
//
// `local list --vendor` is the shape the brief's inert-filter defect takes
// in this family: a filter that silently did not filter would return
// plausible rows at exit 0 and no assertion on a COUNT would notice. So the
// filter case builds a fixture where three answers must all differ and all
// be non-empty in different ways:
//
//   unfiltered      two rows, one per projection  (the survival baseline)
//   --vendor codex  exactly the shared-root row SURVIVES, the Claude one is gone
//   --vendor nope   the header line, then `no rows matched filter`
//
// An inert filter returns the three-row answer all three times. A filter
// applied uniformly (say, one that dropped everything) returns the third
// answer all three times. Only a filter with the real branch asymmetry
// produces three different answers, and the middle one is asserted by
// naming the surviving row rather than by counting.
//
// The same shape is asserted for `local link --vendor`, where exclusion is
// not a display concern but a WRITE: a filtered link NARROWS the manifest
// (skipped records are excluded from it) rather than merging into it, so
// the manifest row assertion is what proves the filter reached disk.
//
// ## NULL IS DISTINGUISHED FROM EMPTY AT THE FLAG LAYER TOO
//
// `--vendor ""` is NOT the same as an absent `--vendor`, and the difference
// is observable: the empty string matches no vendor, so `local link
// --vendor ""` skips every target and writes an entry with zero links. A
// `flag_string` that collapsed an empty value to `nullopt` would silently
// link everything instead. Pinned directly.
//
// ## HOME / DB SAFETY
//
// Every fixture builds an explicit environment map with `PLANAR_LOCAL_HOME`
// and `HOME` under its own scratch root; nothing here reads the process
// environment. And because none of these five leaves opens SQLite,
// `ctx.db().opened()` is asserted false after each — which is both a real
// contract and a second, structural guarantee that the operator's
// `~/.planar/planar.db` is unreachable from this file.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment every case dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Never created; see the file header.
};

/// @brief Build a fixture under a unique scratch directory.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_local_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "localhome" / ".planar" / "local" / "skills", ec);
  std::filesystem::create_directories(root / "localhome" / ".planar" / "local" / "agents", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief The sandbox root for a fixture.
/// @param fx The fixture.
/// @return `<root>/localhome/.planar/local`.
auto sandbox(const fixture& fx) -> std::filesystem::path {
  return fx.root / "localhome" / ".planar" / "local";
}

/// @brief Dispatch `args` against the real tree and table inside `fx`.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Write a file, creating its parent directories.
/// @param path Where.
/// @param body What.
auto write_file(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << body;
}

/// @brief Read a file whole, or `<ABSENT>`.
/// @param path The file.
/// @return Its bytes, or the sentinel.
auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "<ABSENT>";
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// @brief Replace the fixture root with a stable token so a comparison does
/// not depend on the temp directory's name.
/// @param text The text.
/// @param fx The fixture.
/// @return The normalized text.
auto normalize(std::string_view text, const fixture& fx) -> std::string {
  std::string const needle = fx.root.string();
  std::string       out;
  std::string_view  rest = text;
  for (;;) {
    auto const at = rest.find(needle);
    if (at == std::string_view::npos) {
      out.append(rest);
      return out;
    }
    out.append(rest.substr(0, at));
    out.append("<ROOT>");
    rest.remove_prefix(at + needle.size());
  }
}

/// @brief One manifest's recorded links, as `name|vendor|mode|stamp` rows.
///
/// THE FOUR-STATE RENDERING IS THE POINT, and every state is reachable:
///
///   `<NO-MANIFEST>`      the manifest FILE does not exist — nothing was ever
///                        linked for this kind.
///   `<EMPTY>`            the file exists and records no entries at all — what
///                        an `unlink` of the last source leaves behind.
///   `<name>|<NO-LINKS>`  an entry exists with an EMPTY `links` array — what a
///                        fully-filtered `local link --vendor nope` writes.
///   `<name>|<vendor>|…`  a real recorded install.
///
/// The first three print identical stdout on the leaves that produce them,
/// so only a state read separates them — the same reason `runs_leaves.t.cpp`
/// renders SQL NULL as `<NULL>` rather than as the empty string. `mode` and
/// `linked_at` are each rendered `<NULL>` when the writer omitted the key,
/// which keeps "absent" distinguishable from "present and empty"; the stamp
/// is folded to the token `<STAMP>` because it is wall-clock.
///
/// This helper SCRAPES the bytes rather than going through
/// `manifest::load_manifest`. That is deliberate: the point is to observe
/// what was WRITTEN, and a reader shared with the writer cannot catch a
/// writer that drops a field.
/// @param fx The fixture.
/// @param kind_dir `skills` or `agents`.
/// @return The rendered rows.
auto manifest_rows(const fixture& fx, std::string_view kind_dir) -> std::string {
  auto const raw = read_file(sandbox(fx) / kind_dir / ".link-manifest.json");
  if (raw == "<ABSENT>") {
    return "<NO-MANIFEST>";
  }

  auto const field = [](std::string_view line, std::string_view key) -> std::optional<std::string> {
    auto const tag = std::format("\"{}\":", key);
    auto const at  = line.find(tag);
    if (at == std::string_view::npos) {
      return std::nullopt;
    }
    auto rest = line.substr(at + tag.size());
    auto open = rest.find('"');
    if (open == std::string_view::npos) {
      return std::nullopt;
    }
    rest       = rest.substr(open + 1);
    auto close = rest.find('"');
    return std::string{rest.substr(0, close)};
  };

  std::string rows;
  std::string current_name;
  std::string vendor;
  std::string mode;
  std::string stamp;
  bool        pending           = false;
  std::size_t links_for_current = 0;

  auto const flush_link = [&] {
    if (!pending) {
      return;
    }
    if (!rows.empty()) {
      rows += '\n';
    }
    rows +=
        std::format("{}|{}|{}|{}", current_name, vendor, mode.empty() ? "<NULL>" : mode, stamp.empty() ? "<NULL>" : "<STAMP>");
    ++links_for_current;
    pending = false;
  };
  auto const flush_entry = [&] {
    flush_link();
    if (!current_name.empty() && links_for_current == 0) {
      if (!rows.empty()) {
        rows += '\n';
      }
      rows += std::format("{}|<NO-LINKS>", current_name);
    }
  };

  for (auto const chunk : std::views::split(raw, '\n')) {
    std::string_view line{chunk.begin(), chunk.end()};
    // The `name` key appears ONLY at entry level in this schema; a link
    // object carries `vendor`, `target_path`, `source_path`, `mode` and
    // `linked_at` and never a `name`. That is what makes a line-oriented
    // scrape unambiguous here.
    if (auto const got = field(line, "name"); got.has_value()) {
      flush_entry();
      current_name      = *got;
      links_for_current = 0;
      continue;
    }
    if (auto const got = field(line, "vendor"); got.has_value()) {
      flush_link();
      vendor = *got;
      mode.clear();
      stamp.clear();
      pending = true;
      continue;
    }
    if (auto const got = field(line, "mode"); got.has_value()) {
      mode = *got;
      continue;
    }
    if (auto const got = field(line, "linked_at"); got.has_value()) {
      stamp = *got;
      continue;
    }
  }
  flush_entry();
  return rows.empty() ? std::string{"<EMPTY>"} : rows;
}

/// @brief What is actually on disk at one install path.
///
/// A SYMLINK, a REGULAR file and an ABSENT path are three different states
/// and `local list` reports the first two identically (`live`). Only this
/// distinguishes an install that was created from one that was copied, or
/// from a path an unlink left behind.
/// @param path The install path.
/// @return `symlink` / `regular` / `dir` / `absent`.
auto install_state(const std::filesystem::path& path) -> std::string {
  std::error_code ec;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
    return "symlink";
  }
  if (!std::filesystem::exists(path, ec)) {
    return "absent";
  }
  return std::filesystem::is_directory(path, ec) ? "dir" : "regular";
}

/// @brief Create vendor presence markers under the fixture's sandbox home.
/// @param fx The fixture.
/// @param markers Directories, or a file when the name ends `.json`.
auto mark(const fixture& fx, std::initializer_list<std::string_view> markers) -> void {
  for (auto const marker : markers) {
    auto const path = fx.root / "localhome" / std::string{marker};
    if (marker.ends_with(".json")) {
      write_file(path, "{}\n");
    } else {
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
    }
  }
}

/// @brief The path of a projected skill directory under the Claude skills root.
/// @param fx The fixture.
/// @param name The unprefixed name.
/// @return `<home>/.claude/skills/planar-local-<name>`.
auto claude_skill(const fixture& fx, std::string_view name) -> std::filesystem::path {
  return fx.root / "localhome" / ".claude" / "skills" / std::format("planar-local-{}", name);
}

/// @brief Seed a three-vendor skill under the sandbox.
/// @param fx The fixture.
/// @param name The skill's name.
auto seed_three_vendor_skill(const fixture& fx, std::string_view name) -> void {
  write_file(sandbox(fx) / "skills" / name / "SKILL.md",
             std::format("---\nname: {}\ndescription: Three vendors.\nvendors: [claude, codex, copilot]\n---\n\nBody.\n", name));
}

} // namespace

TEST_CASE("local list on a virgin sandbox says so, and --json says NOTHING", "[cmd][local][list]") {
  auto const fx = make_fixture("listempty");

  auto const text = dispatch(fx, {"local", "list"});
  CHECK(text.code == 0);
  CHECK(text.out == "no sandbox installs recorded\n");
  CHECK(text.err.empty());
  // The lazy-database rule, for real: this family never opens SQLite.
  CHECK_FALSE(text.db_open);

  // ZERO BYTES, not `[]` and not a bare newline. A handler that appended a
  // terminator to the renderer's payload would break exactly here, and
  // nowhere else in the family.
  auto const json = dispatch(fx, {"local", "list", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.empty());
  CHECK_FALSE(json.db_open);

  // ...and nothing was written while reading.
  CHECK(manifest_rows(fx, "skills") == "<NO-MANIFEST>");
  CHECK(manifest_rows(fx, "agents") == "<NO-MANIFEST>");
}

TEST_CASE("local link writes the manifest AND the copies, and list reads them back", "[cmd][local][link]") {
  auto const fx = make_fixture("linkwrite");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "alpha");
  write_file(sandbox(fx) / "agents" / "beta.md", "---\nname: beta\ndescription: An agent.\n---\n\nAgent.\n");
  auto const alpha_source = read_file(sandbox(fx) / "skills" / "alpha" / "SKILL.md");

  // BASELINE FIRST: nothing recorded, nothing installed.
  REQUIRE(manifest_rows(fx, "skills") == "<NO-MANIFEST>");
  REQUIRE(install_state(claude_skill(fx, "alpha")) == "absent");

  auto const dry = dispatch(fx, {"local", "link", "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.out.contains("dry-run: 4 would-be installs across 2 source(s)\n"));
  // A DRY RUN WRITES NOTHING: a handler that passed `dry_run` to the renderer
  // and not to the engine would print `dry-run` and still write every copy.
  CHECK(manifest_rows(fx, "skills") == "<NO-MANIFEST>");
  CHECK(manifest_rows(fx, "agents") == "<NO-MANIFEST>");
  CHECK(install_state(claude_skill(fx, "alpha")) == "absent");

  auto const real = dispatch(fx, {"local", "link"});
  CHECK(real.code == 0);
  CHECK(real.out.contains("done: 4 linked, 0 unchanged, 0 skipped across 2 source(s)\n"));
  CHECK_FALSE(real.db_open);

  CHECK(manifest_rows(fx, "skills") == "alpha|claude|copy|<STAMP>\nalpha|shared|copy|<STAMP>");
  CHECK(manifest_rows(fx, "agents") == "beta|claude|copy|<STAMP>\nbeta|codex|copy|<STAMP>");
  // The projection is a COPY under the new names, with `name` rewritten...
  CHECK(install_state(claude_skill(fx, "alpha")) == "dir");
  CHECK(install_state(fx.root / "localhome" / ".agents" / "skills" / "planar-local-alpha") == "dir");
  CHECK(read_file(claude_skill(fx, "alpha") / "SKILL.md").contains("name: planar-local-alpha\n"));
  CHECK(install_state(fx.root / "localhome" / ".claude" / "agents" / "planar-local-beta.md") == "regular");
  CHECK(read_file(fx.root / "localhome" / ".codex" / "agents" / "planar-local-beta.toml")
            .starts_with("name = \"planar-local-beta\"\n"));
  // ...the source is untouched, and the OLD locations are never written.
  CHECK(read_file(sandbox(fx) / "skills" / "alpha" / "SKILL.md") == alpha_source);
  CHECK(install_state(fx.root / "localhome" / ".claude" / "commands") == "absent");
  CHECK(install_state(fx.root / "localhome" / ".codex" / "skills") == "absent");
  CHECK(install_state(fx.root / "localhome" / ".copilot") == "absent");
  CHECK(install_state(fx.root / "localhome" / ".planar" / "agents") == "absent");

  // `list --json` reports each projection live.
  auto const listed = dispatch(fx, {"local", "list", "--json"});
  CHECK(listed.code == 0);
  CHECK(listed.out.contains("\"Name\":\"alpha\",\"Kind\":\"skill\",\"Record\":{\"vendor\":\"shared\""));
  std::size_t live = 0;
  for (std::size_t at = listed.out.find("\"action\":\"live\""); at != std::string::npos;
       at             = listed.out.find("\"action\":\"live\"", at + 1)) {
    ++live;
  }
  CHECK(live == 4);

  // A SECOND run is `unchanged`, not `created`.
  auto const again = dispatch(fx, {"local", "link"});
  CHECK(again.code == 0);
  CHECK(again.out.contains("done: 0 linked, 4 unchanged, 0 skipped across 2 source(s)\n"));
  CHECK(manifest_rows(fx, "skills") == "alpha|claude|copy|<STAMP>\nalpha|shared|copy|<STAMP>");
}

TEST_CASE("local link projects an agent into every present vendor directory", "[cmd][local][link]") {
  auto const fx = make_fixture("agentfan");
  mark(fx, {".claude", ".codex", ".copilot", ".config/opencode"});
  write_file(sandbox(fx) / "agents" / "helper.md", "---\ndescription: Helps.\n---\n\nHelp body.\n");

  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);
  auto const home = fx.root / "localhome";
  CHECK(read_file(home / ".claude" / "agents" / "planar-local-helper.md").contains("name: planar-local-helper\n"));
  CHECK(read_file(home / ".copilot" / "agents" / "planar-local-helper.agent.md").contains("name: planar-local-helper\n"));
  CHECK(read_file(home / ".config" / "opencode" / "agents" / "planar-local-helper.md") ==
        "---\ndescription: Helps.\nmode: subagent\n---\n\nHelp body.\n");
  CHECK(read_file(home / ".codex" / "agents" / "planar-local-helper.toml") ==
        "name = \"planar-local-helper\"\ndescription = \"Helps.\"\ndeveloper_instructions = \"Help body.\\n\"\n");
  // A vendor with no marker gets nothing.
  CHECK(install_state(home / ".gemini") == "absent");
}

TEST_CASE("local list reports a stale copy, and --reconcile refreshes it exactly once", "[cmd][local][link][reconcile]") {
  auto const fx = make_fixture("stale");
  mark(fx, {".claude", ".codex"});
  write_file(sandbox(fx) / "skills" / "mine" / "SKILL.md", "---\nname: mine\ndescription: Mine.\n---\n\nBody.\n");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);

  {
    std::ofstream(sandbox(fx) / "skills" / "mine" / "SKILL.md", std::ios::app) << "An added line.\n";
  }
  auto const listed = dispatch(fx, {"local", "list"});
  CHECK(listed.out.contains("mine          skill    shared   stale"));
  CHECK(listed.out.contains("mine          skill    claude   stale"));

  auto const first = dispatch(fx, {"local", "link", "--reconcile"});
  CHECK(first.code == 0);
  CHECK(first.out.contains("mine (skill) - stale; wrote 2 path(s)\n"));
  CHECK(first.out.contains("\ndone: 2 change(s)\n"));
  CHECK(read_file(claude_skill(fx, "mine") / "SKILL.md").contains("An added line."));
  CHECK(dispatch(fx, {"local", "list"}).out.contains("mine          skill    claude   live"));

  auto const copy  = claude_skill(fx, "mine") / "SKILL.md";
  auto const mtime = std::filesystem::last_write_time(copy);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto const second = dispatch(fx, {"local", "link", "--reconcile"});
  CHECK(second.code == 0);
  CHECK(second.out == "reconcile: manifest already consistent with the filesystem\n");
  CHECK((std::filesystem::last_write_time(copy) == mtime));
}

TEST_CASE("local link --reconcile migrates the old projections and leaves a foreign file", "[cmd][local][link][reconcile]") {
  auto const fx = make_fixture("migrateold");
  mark(fx, {".claude", ".codex"});
  write_file(sandbox(fx) / "skills" / "mine" / "SKILL.md", "---\nname: mine\ndescription: Mine.\n---\n\nBody.\n");
  auto const home    = fx.root / "localhome";
  auto const foreign = home / ".claude" / "commands" / "local-theirs.md";
  write_file(foreign, "theirs\n");
  std::error_code ec;
  std::filesystem::create_symlink(sandbox(fx) / "skills" / "mine" / "SKILL.md", home / ".claude" / "commands" / "local-mine.md",
                                  ec);
  std::filesystem::create_directories(home / ".codex" / "skills", ec);
  std::filesystem::create_directory_symlink(sandbox(fx) / "skills" / "mine", home / ".codex" / "skills" / "local-mine", ec);

  auto const got = dispatch(fx, {"local", "link", "--reconcile"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("mine (skill) - legacy; removed 2 path(s)\n"));
  CHECK(install_state(home / ".claude" / "commands" / "local-mine.md") == "absent");
  CHECK(install_state(home / ".codex" / "skills" / "local-mine") == "absent");
  CHECK(read_file(foreign) == "theirs\n");
  CHECK(install_state(claude_skill(fx, "mine")) == "dir");
  CHECK(install_state(home / ".agents" / "skills" / "planar-local-mine") == "dir");
  // The source survived: removing a directory symlink never reached through it.
  CHECK(install_state(sandbox(fx) / "skills" / "mine" / "SKILL.md") == "regular");
  CHECK(dispatch(fx, {"local", "link", "--reconcile"}).out == "reconcile: manifest already consistent with the filesystem\n");
}

TEST_CASE("local link refuses a foreign destination at exit 6 and writes nothing else", "[cmd][local][link]") {
  auto const fx = make_fixture("foreign");
  mark(fx, {".claude", ".codex"});
  write_file(sandbox(fx) / "skills" / "mine" / "SKILL.md", "---\nname: mine\ndescription: Mine.\n---\n\nBody.\n");
  auto const foreign = claude_skill(fx, "mine") / "SKILL.md";
  write_file(foreign, "not planar's\n");

  for (auto const& args : {std::vector<std::string>{"local", "link"}, std::vector<std::string>{"local", "link", "--reconcile"}}) {
    if (args.back() == "--reconcile") {
      // Reconcile acts on tracked sources: an old symlink projection is what makes this one tracked.
      std::error_code ec;
      std::filesystem::create_directories(fx.root / "localhome" / ".codex" / "skills", ec);
      std::filesystem::create_directory_symlink(sandbox(fx) / "skills" / "mine",
                                                fx.root / "localhome" / ".codex" / "skills" / "local-mine", ec);
    }
    auto const got = dispatch(fx, args);
    CHECK(got.code == 6);
    CHECK(got.out.empty());
    CHECK(got.err.contains(foreign.string()));
    CHECK(read_file(foreign) == "not planar's\n");
    CHECK(install_state(fx.root / "localhome" / ".agents") == "absent");
    CHECK(manifest_rows(fx, "skills") == "<NO-MANIFEST>");
  }
}

TEST_CASE("the --vendor filter EXCLUDES, proven three ways with the survivor named", "[cmd][local][list][filter]") {
  auto const fx = make_fixture("vendorfilter");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "gamma");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);

  // 1. UNFILTERED — the survival baseline. All three vendors are present, so
  //    an absence below means something.
  auto const all = dispatch(fx, {"local", "list"});
  REQUIRE(all.code == 0);
  CHECK(all.out.contains("gamma         skill    shared   live"));
  CHECK(all.out.contains("gamma         skill    claude   live"));

  // 2. FILTERED TO ONE — the named row SURVIVES and the other two are gone.
  //    Asserted by name, never by counting: a count of one would also pass
  //    against a filter that kept the wrong row.
  auto const codex = dispatch(fx, {"local", "list", "--vendor", "codex"});
  REQUIRE(codex.code == 0);
  CHECK(codex.out.contains("gamma         skill    shared   live"));
  CHECK_FALSE(codex.out.contains("claude"));

  // 3. FILTERED TO NOTHING — a DIFFERENT sentence from the empty-sandbox one,
  //    and printed AFTER the header row, which is still emitted. "You have
  //    nothing" and "you have things, none matching" are different operator
  //    situations.
  auto const none = dispatch(fx, {"local", "list", "--vendor", "nope"});
  REQUIRE(none.code == 0);
  CHECK(none.out == "name          kind     vendor   status   target\nno rows matched filter\n");
  CHECK(none.out != "no sandbox installs recorded\n");

  // The three answers are pairwise different. An inert filter makes 1 == 2
  // == 3; a uniformly-applied one makes 2 == 3. Only the real branch
  // asymmetry produces three.
  CHECK(all.out != codex.out);
  CHECK(codex.out != none.out);
  CHECK(all.out != none.out);
}

TEST_CASE("local link --vendor NARROWS what reaches the manifest", "[cmd][local][link][filter]") {
  auto const fx = make_fixture("linkfilter");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "delta");

  auto const filtered = dispatch(fx, {"local", "link", "--vendor", "claude"});
  CHECK(filtered.code == 0);
  CHECK(filtered.out.contains("done: 1 linked, 0 unchanged, 1 skipped across 1 source(s)\n"));

  // THE WRITE SIDE OF THE FILTER. A `skipped` record is excluded from the
  // manifest entirely, so a filtered link narrows the recorded install set
  // rather than merging into it.
  CHECK(manifest_rows(fx, "skills") == "delta|claude|copy|<STAMP>");
  CHECK(install_state(claude_skill(fx, "delta")) == "dir");
  CHECK(install_state(fx.root / "localhome" / ".agents" / "skills" / "planar-local-delta") == "absent");
}

TEST_CASE("an EMPTY --vendor is not an ABSENT --vendor", "[cmd][local][link][filter]") {
  auto const fx = make_fixture("emptyvendor");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "epsilon");

  // The empty string matches no vendor, so every target is skipped and the
  // manifest records an entry with ZERO links. A `flag_string` that collapsed
  // an empty value to `nullopt` would link everything instead.
  auto const empty = dispatch(fx, {"local", "link", "--vendor", ""});
  CHECK(empty.code == 0);
  CHECK(empty.out.contains("done: 0 linked, 0 unchanged, 2 skipped across 1 source(s)\n"));
  CHECK(manifest_rows(fx, "skills") == "epsilon|<NO-LINKS>");
  CHECK(install_state(claude_skill(fx, "epsilon")) == "absent");

  auto const absent = dispatch(fx, {"local", "link"});
  CHECK(absent.code == 0);
  CHECK(absent.out.contains("done: 2 linked, 0 unchanged, 0 skipped across 1 source(s)\n"));
  CHECK(manifest_rows(fx, "skills") == "epsilon|claude|copy|<STAMP>\nepsilon|shared|copy|<STAMP>");
}

TEST_CASE("local link refuses a named source that does not exist, but not an empty sandbox", "[cmd][local][link]") {
  auto const fx = make_fixture("linknames");

  // An EMPTY sandbox is exit 0 and a sentence. An operator with no personal
  // skills yet is not in a failure state.
  auto const empty = dispatch(fx, {"local", "link"});
  CHECK(empty.code == 0);
  CHECK(empty.err.empty());
  CHECK(empty.out == std::format("no sandbox sources to link under {}\n", sandbox(fx).string()));

  // A NAMED source that does not exist is exit 1 and names the root searched.
  // Collapsing the two would make an empty sandbox look like a failure.
  auto const named = dispatch(fx, {"local", "link", "nosuch"});
  CHECK(named.code == 1);
  CHECK(named.out.empty());
  CHECK(named.err == std::format("error: no sandbox source named \"nosuch\" under {}\n", sandbox(fx).string()));
}

TEST_CASE("local link --reconcile refuses a positional at exit 2", "[cmd][local][link][reconcile]") {
  auto const fx  = make_fixture("reconcilepos");
  auto const got = dispatch(fx, {"local", "link", "--reconcile", "stray"});
  CHECK(got.code == 2);
  CHECK(got.out.empty());
  CHECK(got.err == "error: --reconcile takes no positional arguments\n");

  // Discrimination: the same flag WITHOUT the positional succeeds, so exit 2
  // is the positional's doing and not simply what `--reconcile` does.
  auto const ok = dispatch(fx, {"local", "link", "--reconcile"});
  CHECK(ok.code == 0);
  CHECK(ok.out == "reconcile: manifest already consistent with the filesystem\n");
}

TEST_CASE("local link --reconcile drops entries whose source is gone", "[cmd][local][link][reconcile]") {
  auto const fx = make_fixture("reconcilereal");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "zeta");
  seed_three_vendor_skill(fx, "eta");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);
  REQUIRE(manifest_rows(fx, "skills") == "eta|claude|copy|<STAMP>\neta|shared|copy|<STAMP>\n"
                                         "zeta|claude|copy|<STAMP>\nzeta|shared|copy|<STAMP>");

  std::error_code ec;
  std::filesystem::remove_all(sandbox(fx) / "skills" / "zeta", ec);

  // A DRY RUN reports and changes nothing.
  auto const dry = dispatch(fx, {"local", "link", "--reconcile", "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.out.contains("zeta (skill) - source-missing; would remove 2 path(s)\n"));
  CHECK(dry.out.contains("\ndry-run: 2 change(s) would be made\n"));
  CHECK(manifest_rows(fx, "skills").contains("zeta|claude|copy|<STAMP>"));
  CHECK(install_state(claude_skill(fx, "zeta")) == "dir");

  auto const real = dispatch(fx, {"local", "link", "--reconcile"});
  CHECK(real.code == 0);
  CHECK(real.out.contains("zeta (skill) - source-missing; removed 2 path(s)\n"));
  CHECK(real.out.contains("\ndone: 2 change(s)\n"));

  // THE SURVIVOR IS NAMED.
  CHECK(manifest_rows(fx, "skills") == "eta|claude|copy|<STAMP>\neta|shared|copy|<STAMP>");
  CHECK(install_state(claude_skill(fx, "eta")) == "dir");
  CHECK(install_state(claude_skill(fx, "zeta")) == "absent");
}

TEST_CASE("local unlink removes installs, and --purge deletes the source", "[cmd][local][unlink]") {
  auto const fx = make_fixture("unlink");
  mark(fx, {".claude", ".codex"});
  seed_three_vendor_skill(fx, "theta");
  seed_three_vendor_skill(fx, "iota");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);

  auto const gone = dispatch(fx, {"local", "unlink", "theta"});
  CHECK(gone.code == 0);
  CHECK(gone.out.contains("theta (skill)\n"));
  CHECK_FALSE(gone.db_open);

  // The removed installs are gone from BOTH the manifest and the disk, and
  // the untouched sibling SURVIVES in both.
  CHECK(manifest_rows(fx, "skills") == "iota|claude|copy|<STAMP>\niota|shared|copy|<STAMP>");
  CHECK(install_state(claude_skill(fx, "theta")) == "absent");
  CHECK(install_state(claude_skill(fx, "iota")) == "dir");
  // ...and `--purge` was NOT passed, so the SOURCE is still there.
  CHECK(install_state(sandbox(fx) / "skills" / "theta" / "SKILL.md") == "regular");

  auto const twice = dispatch(fx, {"local", "unlink", "theta"});
  CHECK(twice.code == 0);
  CHECK(twice.out == "no installs found for \"theta\" (already unlinked, or no such name)\n");

  auto const purged = dispatch(fx, {"local", "unlink", "iota", "--purge"});
  CHECK(purged.code == 0);
  CHECK(purged.out.contains("purged source file: "));
  CHECK(install_state(sandbox(fx) / "skills" / "iota" / "SKILL.md") == "absent");
  CHECK(manifest_rows(fx, "skills") == "<EMPTY>");
}

TEST_CASE("local unlink on a cross-kind name collision touches only the SKILL", "[cmd][local][unlink]") {
  // `lookup_kind_for_name` checks skills FIRST, so a name that exists as both
  // resolves to the skill and the agent projection is left live.
  auto const fx = make_fixture("dupname");
  mark(fx, {".claude"});
  seed_three_vendor_skill(fx, "dup");
  write_file(sandbox(fx) / "agents" / "dup.md", "---\nname: dup\ndescription: Agent dup.\n---\n\nA.\n");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);
  REQUIRE(manifest_rows(fx, "agents") == "dup|claude|copy|<STAMP>");

  auto const got = dispatch(fx, {"local", "unlink", "dup"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("dup (skill)\n"));
  CHECK_FALSE(got.out.contains("dup (agent)\n"));

  CHECK(manifest_rows(fx, "skills") == "<EMPTY>");
  // THE ORPHAN. Still recorded, still installed.
  CHECK(manifest_rows(fx, "agents") == "dup|claude|copy|<STAMP>");
  CHECK(install_state(fx.root / "localhome" / ".claude" / "agents" / "planar-local-dup.md") == "regular");
}

TEST_CASE("local migrate promotes a flat skill into directory shape", "[cmd][local][migrate]") {
  auto const fx = make_fixture("migrate");
  write_file(sandbox(fx) / "skills" / "flat.md", "---\nname: flat\ndescription: Legacy flat.\n---\n\nFlat body.\n");

  auto const empty_first = dispatch(fx, {"local", "migrate", "--dry-run"});
  CHECK(empty_first.code == 0);
  CHECK(empty_first.out.contains("would migrate"));
  // A DRY RUN MOVES NOTHING. The verb differs on stdout, but a handler that
  // dropped `dry_run` on the way to the engine would print the same line and
  // move the file.
  CHECK(install_state(sandbox(fx) / "skills" / "flat.md") == "regular");
  CHECK(install_state(sandbox(fx) / "skills" / "flat" / "SKILL.md") == "absent");

  auto const real = dispatch(fx, {"local", "migrate"});
  CHECK(real.code == 0);
  CHECK(real.out.contains("done: migrated 1 skill(s); skipped 0\n"));
  CHECK(install_state(sandbox(fx) / "skills" / "flat.md") == "absent");
  CHECK(install_state(sandbox(fx) / "skills" / "flat" / "SKILL.md") == "regular");
  CHECK(read_file(sandbox(fx) / "skills" / "flat" / "SKILL.md").contains("Flat body."));

  // Nothing left to do is its own sentence, NOT an empty list.
  auto const again = dispatch(fx, {"local", "migrate"});
  CHECK(again.code == 0);
  CHECK(again.out == "migrate: no legacy flat skills found; sandbox is already dir-shape\n");

  // The `--json` form carries NO dry-run marker at all: the two runs are
  // byte-identical. Pinned so a later "improvement" that added one is a
  // deliberate parity break rather than an accident.
  auto const json_dry  = dispatch(fx, {"local", "migrate", "--json", "--dry-run"});
  auto const json_real = dispatch(fx, {"local", "migrate", "--json"});
  CHECK(json_dry.out == json_real.out);
  CHECK(json_dry.out == "{\"Migrated\":[],\"Skipped\":[]}\n");
}

TEST_CASE("local import copies sources in, promotes flat skills, then links them", "[cmd][local][import]") {
  auto const fx = make_fixture("import");
  mark(fx, {".claude"});
  auto const src = fx.root / "src";
  write_file(src / "one.md", "---\nname: one\ndescription: First.\nvendors: [claude]\n---\n\nOne.\n");
  write_file(src / "notes.txt", "not markdown\n");

  auto const dry = dispatch(fx, {"local", "import", src.string(), "--dry-run"});
  CHECK(dry.code == 0);
  CHECK(dry.out.contains("imported 1 file(s); skipped 0\n"));
  // A dry-run imports NOTHING and — this is the part stdout cannot show —
  // does not run the link phase either.
  CHECK(install_state(sandbox(fx) / "skills" / "one" / "SKILL.md") == "absent");
  CHECK(manifest_rows(fx, "skills") == "<NO-MANIFEST>");

  auto const real = dispatch(fx, {"local", "import", src.string()});
  CHECK(real.code == 0);
  CHECK(real.out.contains("\nLinking imported files into vendor surfaces:\n"));
  CHECK_FALSE(real.db_open);

  // A FLAT source lands in DIRECTORY shape, so `local migrate` never has to
  // run on something imported...
  CHECK(install_state(sandbox(fx) / "skills" / "one" / "SKILL.md") == "regular");
  CHECK(install_state(sandbox(fx) / "skills" / "one.md") == "absent");
  // ...and the link phase recorded the DESTINATION, not the origin.
  CHECK(manifest_rows(fx, "skills") == "one|claude|copy|<STAMP>");
  CHECK(normalize(read_file(sandbox(fx) / "skills" / ".link-manifest.json"), fx)
            .contains("<ROOT>/localhome/.planar/local/skills/one/SKILL.md"));
  CHECK_FALSE(normalize(read_file(sandbox(fx) / "skills" / ".link-manifest.json"), fx).contains("<ROOT>/src/one.md"));

  // `--no-link` runs phase one ONLY: the file arrives, nothing is recorded.
  auto const nolink = dispatch(fx, {"local", "import", src.string(), "--force", "--no-link"});
  CHECK(nolink.code == 0);
  CHECK_FALSE(nolink.out.contains("Linking imported files"));
}

TEST_CASE("local import maps its two failures to DIFFERENT exit codes", "[cmd][local][import]") {
  auto const fx  = make_fixture("importfail");
  auto const src = fx.root / "src";
  write_file(src / "notes.txt", "not markdown\n");

  // A non-`.md` FILE is invalid input — exit 2.
  auto const not_md = dispatch(fx, {"local", "import", (src / "notes.txt").string()});
  CHECK(not_md.code == 2);
  CHECK(not_md.err == std::format("error: importing {} failed: InvalidInput\n", (src / "notes.txt").string()));

  // A directory that resolves to ZERO importable entries is not-found —
  // exit 1. Collapsing the two would make "you pointed at a .txt" and "that
  // directory held nothing" indistinguishable to a script.
  auto const nothing = dispatch(fx, {"local", "import", src.string()});
  CHECK(nothing.code == 1);
  CHECK(nothing.err == std::format("error: importing {} failed: NotFound\n", src.string()));

  // And a bad `--kind` refuses BEFORE either, with its own wording. Note the
  // value is double-quoted here where the reconcile refusal quotes nothing.
  auto const bad_kind = dispatch(fx, {"local", "import", src.string(), "--kind", "bogus"});
  CHECK(bad_kind.code == 2);
  CHECK(bad_kind.err == "error: --kind must be skill or agent, got \"bogus\"\n");
  auto const empty_kind = dispatch(fx, {"local", "import", src.string(), "--kind", ""});
  CHECK(empty_kind.code == 2);
  CHECK(empty_kind.err == "error: --kind must be skill or agent, got \"\"\n");
}

TEST_CASE("local link's walk errors and lint findings are TEXT-MODE ONLY", "[cmd][local][link]") {
  auto const fx = make_fixture("diagnostics");
  // Parses, but lints: an empty description.
  write_file(sandbox(fx) / "skills" / "nodesc" / "SKILL.md", "---\nname: nodesc\n---\n\nBody.\n");
  // Does NOT parse: a skill directory with no SKILL.md. Exactly ONE walk
  // error, deliberately — `walk_errors` is appended in directory-iteration
  // order, which is unspecified, so the SET is the contract and not the
  // order. One entry makes the two agree trivially.
  std::error_code ec;
  std::filesystem::create_directories(sandbox(fx) / "skills" / "broken", ec);

  auto const text = dispatch(fx, {"local", "link"});
  CHECK(text.code == 0);
  CHECK(text.out.contains("skill directory missing SKILL.md\n"));
  CHECK(text.out.contains("lint [warning] skill/nodesc.description: description is empty"));

  auto const json = dispatch(fx, {"local", "link", "--json", "--dry-run"});
  CHECK(json.code == 0);
  // THE ASYMMETRY. Under `--json` a malformed sandbox source is invisible to
  // a scripted caller and so is every lint finding. That is the oracle's
  // behaviour, pinned rather than corrected — and pinned as an absence in
  // the SAME fixture that proves the presence, so a handler that emitted
  // neither would fail the case above.
  CHECK_FALSE(json.out.contains("skill directory missing SKILL.md"));
  CHECK_FALSE(json.out.contains("lint ["));
  CHECK(json.out.contains("\"Name\":\"nodesc\""));
}

TEST_CASE("local unlink falls back to BOTH kinds when the source is gone", "[cmd][local][unlink]") {
  // THE REASON THE FALLBACK EXISTS. `lookup_kind_for_name` guesses the kind
  // by looking for the SOURCE on disk, so an install whose source has
  // already been deleted resolves to neither kind — and if the handler
  // refused, or guessed `skill` and stopped, an orphaned AGENT install would
  // be unremovable through the CLI. The oracle unlinks both manifests
  // instead. This case is the one a `kinds.push_back(agent)` that quietly
  // went missing would fail.
  auto const fx = make_fixture("unlinkorphan");
  mark(fx, {".claude"});
  write_file(sandbox(fx) / "agents" / "orphan.md", "---\nname: orphan\ndescription: An agent.\n---\n\nA.\n");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);
  REQUIRE(manifest_rows(fx, "agents") == "orphan|claude|copy|<STAMP>");

  // Delete the SOURCE, leaving the manifest entry and the install behind.
  std::error_code ec;
  std::filesystem::remove(sandbox(fx) / "agents" / "orphan.md", ec);
  REQUIRE(install_state(sandbox(fx) / "agents" / "orphan.md") == "absent");
  REQUIRE(install_state(fx.root / "localhome" / ".claude" / "agents" / "planar-local-orphan.md") == "regular");

  auto const got = dispatch(fx, {"local", "unlink", "orphan"});
  CHECK(got.code == 0);
  // The AGENT pass is what found it. The skill pass ran first, matched
  // nothing, and printed nothing — an empty block for the kind that did not
  // match would be a visible regression.
  CHECK(got.out.contains("orphan (agent)\n"));
  CHECK_FALSE(got.out.contains("orphan (skill)\n"));
  CHECK_FALSE(got.out.contains("no installs found"));

  CHECK(manifest_rows(fx, "agents") == "<EMPTY>");
  CHECK(install_state(fx.root / "localhome" / ".claude" / "agents" / "planar-local-orphan.md") == "absent");
}

TEST_CASE("local unlink leaves a foreign replacement alone and says so; reconcile exits 1 on a failed write",
          "[cmd][local][unlink][reconcile]") {
  auto const fx   = make_fixture("ownership");
  auto const home = fx.root / "localhome";
  mark(fx, {".claude", ".codex"});
  write_file(sandbox(fx) / "skills" / "mine" / "SKILL.md", "---\nname: mine\ndescription: Mine.\n---\n\nBody.\n");
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);

  auto const      claude = home / ".claude" / "skills" / "planar-local-mine";
  std::error_code ec;
  std::filesystem::remove_all(claude, ec);
  write_file(claude / "SKILL.md", "---\nname: theirs\n---\n\nTheirs.\n");

  auto const got = dispatch(fx, {"local", "unlink", "mine"});
  CHECK(got.code == 0);
  CHECK(got.out.contains("skipped: not owned  <-  " + claude.string()));
  CHECK(read_file(claude / "SKILL.md") == "---\nname: theirs\n---\n\nTheirs.\n");
  CHECK(install_state(home / ".agents" / "skills" / "planar-local-mine") == "absent");

  // The foreign replacement is still in place, so a fresh link refuses it
  // (exit 6, not owned); clear it, link again, then break the destination.
  REQUIRE(dispatch(fx, {"local", "link"}).code == 6);
  std::filesystem::remove_all(claude, ec);
  REQUIRE(dispatch(fx, {"local", "link"}).code == 0);
  // Reconcile: a destination that cannot be written fails the verb.
  std::filesystem::remove_all(claude, ec);
  auto const skills = home / ".claude" / "skills";
  std::filesystem::permissions(skills, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec, ec);
  if (!std::ofstream(skills / "probe").is_open()) {
    auto const failed = dispatch(fx, {"local", "link", "--reconcile"});
    std::filesystem::permissions(skills, std::filesystem::perms::owner_all, ec);
    CHECK(failed.code == 1);
    CHECK(failed.out.contains("error: could not write 1 path(s); not recorded"));
  }
  std::filesystem::permissions(skills, std::filesystem::perms::owner_all, ec);
}

TEST_CASE("local unlink --purge on an unknown name reports BOTH targeted paths", "[cmd][local][unlink]") {
  // Oracle-verified and pinned as-is rather than corrected: `purged_file`
  // means "the path purge TARGETED", not "this was deleted". The delete's
  // error is discarded, so a name that never existed still reports two
  // purge lines — one per kind, because the kind lookup found neither.
  // Preserving this is what keeps the port faithful; the wording is a wart
  // in the reference, not in this layer.
  auto const fx = make_fixture("purgeghost");

  auto const got = dispatch(fx, {"local", "unlink", "ghost", "--purge"});
  CHECK(got.code == 0);
  CHECK(got.out == std::format("ghost (skill)\n  purged source file: {}\n"
                               "ghost (agent)\n  purged source file: {}\n"
                               "no installs found for \"ghost\" (already unlinked, or no such name)\n",
                               (sandbox(fx) / "skills" / "ghost").string(), (sandbox(fx) / "agents" / "ghost.md").string()));

  // Nothing was created by saying so, and the totals still count ZERO
  // removals — which is why the trailing sentence is printed at all.
  CHECK(manifest_rows(fx, "skills") == "<NO-MANIFEST>");
  CHECK(manifest_rows(fx, "agents") == "<NO-MANIFEST>");

  // Discrimination: WITHOUT `--purge` the same invocation prints only the
  // sentence, so the two purge lines above are the flag's doing.
  auto const plain = dispatch(fx, {"local", "unlink", "ghost"});
  CHECK(plain.code == 0);
  CHECK(plain.out == "no installs found for \"ghost\" (already unlinked, or no such name)\n");
}
