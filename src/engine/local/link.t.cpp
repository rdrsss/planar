// @file link.t.cpp
// @brief Tests for `planar.engine.local.link` (plan 996 task 6109, retargeted
// by plan 1104 task 7220).
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// Every function under test takes an explicit `home_dir`, and the module reads
// no environment at all. Every test here passes a `scratch_home` root under
// `temp_directory_path()`, removed on destruction. Vendor directories are
// created INSIDE that scratch root, never in a real home.
//
// The projection is a COPY, so these tests assert bytes rather than link
// targets: the destination holds the source with `name` rewritten, the source
// stays byte-identical, and nothing is written under the old command and skill
// directories. Presence markers (`.claude/`, `.codex/`, ...) are created by the
// test, because a vendor directory without its marker is correctly ignored.
//
// The one clock read in the module (`utc_now_stamp`) is NOT called by
// link()/reconcile(); the stamp is passed in.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.local.manifest;
import planar.engine.local.link;

namespace mf = planar::engine::local::manifest;
namespace lk = planar::engine::local::link;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view k_stamp = "2026-08-23T04:12:27Z";

/// @brief A per-test scratch home, removed on destruction.
///
/// `root_` stands in for `$PLANAR_LOCAL_HOME`. Both the sandbox
/// (`root_/.planar/local`) and every vendor directory (`root_/.claude`, ...)
/// live under it, so nothing this file writes can escape.
struct scratch_home {
  fs::path root_;

  scratch_home()
      : root_(fs::temp_directory_path() / std::format("planar_local_link_{}_{}",
                                                      std::chrono::steady_clock::now().time_since_epoch().count(),
                                                      reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    fs::create_directories(sandbox() / "skills", ec);
    fs::create_directories(sandbox() / "agents", ec);
  }
  scratch_home(const scratch_home&)            = delete;
  scratch_home& operator=(const scratch_home&) = delete;
  ~scratch_home() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  [[nodiscard]] auto sandbox() const -> fs::path {
    return root_ / ".planar" / "local";
  }

  auto write(const fs::path& path, std::string_view content) const -> void {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  /// @brief Create presence markers: directories, or a file when the name ends `.json`.
  auto mark(std::initializer_list<std::string_view> markers) const -> void {
    for (const auto marker : markers) {
      const auto path = root_ / std::string{marker};
      if (marker.ends_with(".json")) {
        write(path, "{}\n");
      } else {
        std::error_code ec;
        fs::create_directories(path, ec);
      }
    }
  }

  [[nodiscard]] auto add_skill(std::string_view name, std::string_view frontmatter) const -> mf::sandbox_file {
    const auto path = sandbox() / "skills" / std::string{name} / "SKILL.md";
    write(path, std::format("---\n{}---\n{} body\n", frontmatter, name));
    auto parsed = mf::parse_file(path, mf::kind::skill);
    REQUIRE(parsed.has_value());
    return *parsed;
  }

  [[nodiscard]] auto add_agent(std::string_view name, std::string_view frontmatter) const -> mf::sandbox_file {
    const auto path = sandbox() / "agents" / std::format("{}.md", name);
    write(path, std::format("---\n{}---\n{} body\n", frontmatter, name));
    auto parsed = mf::parse_file(path, mf::kind::agent);
    REQUIRE(parsed.has_value());
    return *parsed;
  }

  [[nodiscard]] auto opts() const -> lk::link_options {
    return lk::link_options{.home_dir = root_, .now = std::string{k_stamp}};
  }
};

auto read(const fs::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "<ABSENT>";
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

auto action_of(const lk::link_result& result, std::string_view vendor) -> std::string {
  for (const auto& rec : result.records) {
    if (rec.vendor == vendor) {
      return rec.action;
    }
  }
  return "<absent>";
}

auto vendors_of(const lk::link_result& result) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& rec : result.records) {
    out.push_back(rec.vendor);
  }
  return out;
}

auto state_of_row(const fs::path& home, std::string_view name, std::string_view vendor) -> std::string {
  const auto rows = lk::list(home);
  REQUIRE(rows.has_value());
  for (const auto& row : *rows) {
    if (row.name == name && row.record.vendor == vendor) {
      return row.record.action;
    }
  }
  return "<absent>";
}

auto query_for(const fs::path& home, std::string_view name, mf::kind kind,
               std::vector<std::string> vendors = {"claude", "codex", "copilot"}) -> lk::target_query {
  return {home, std::nullopt, "/src/" + std::string{name}, std::string{name}, kind, std::move(vendors)};
}

} // namespace

// --- text forms -------------------------------------------------------------

TEST_CASE("link rewrite_name replaces the name, and only the name") {
  REQUIRE(lk::rewrite_name("---\nname: mine\ndescription: d\n---\nbody\nname: keep\n", "planar-local-mine") ==
          "---\nname: planar-local-mine\ndescription: d\n---\nbody\nname: keep\n");
}

TEST_CASE("link rewrite_name adds a name when the block has none") {
  REQUIRE(lk::rewrite_name("---\ndescription: d\n---\nbody\n", "planar-local-x") ==
          "---\nname: planar-local-x\ndescription: d\n---\nbody\n");
}

TEST_CASE("link rewrite_name leaves nested and indented name keys alone") {
  REQUIRE(lk::rewrite_name("---\nname: a\nplanar:\n  name: inner\n---\n", "p") == "---\nname: p\nplanar:\n  name: inner\n---\n");
}

TEST_CASE("link rewrite_name returns content with no frontmatter unchanged") {
  REQUIRE(lk::rewrite_name("just text\n", "p") == "just text\n");
  REQUIRE(lk::rewrite_name("---\nname: a\nnever closed\n", "p") == "---\nname: a\nnever closed\n");
}

TEST_CASE("link opencode_form reduces the frontmatter and keeps the body") {
  REQUIRE(
      lk::opencode_form("---\nname: a\ndescription: Does a thing.\nmodel: x\nplanar:\n  kind: agent\n---\n\n# Body\ntext\n") ==
      "---\ndescription: Does a thing.\nmode: subagent\n---\n\n# Body\ntext\n");
}

TEST_CASE("link opencode_form quotes a description YAML would misread") {
  REQUIRE(lk::opencode_form("---\ndescription: use: this\n---\nB\n").value() ==
          "---\ndescription: \"use: this\"\nmode: subagent\n---\nB\n");
  REQUIRE(lk::opencode_form("---\ndescription: a \\ b \"c\": d\n---\nB\n").value() ==
          "---\ndescription: \"a \\\\ b \\\"c\\\": d\"\nmode: subagent\n---\nB\n");
  // Already quoted: left alone.
  REQUIRE(lk::opencode_form("---\ndescription: \"x: y\"\n---\nB\n").value() ==
          "---\ndescription: \"x: y\"\nmode: subagent\n---\nB\n");
}

TEST_CASE("link opencode_form refuses a source it cannot derive from") {
  REQUIRE_FALSE(lk::opencode_form("---\nname: a\n---\nB\n").has_value());
  REQUIRE_FALSE(lk::opencode_form("no frontmatter\n").has_value());
  REQUIRE_FALSE(lk::opencode_form("---\ndescription: d\nnever closed\n").has_value());
}

TEST_CASE("link opencode_form ends a body that lacks a final newline with one") {
  REQUIRE(lk::opencode_form("---\ndescription: d\n---\nlast").value() == "---\ndescription: d\nmode: subagent\n---\nlast\n");
}

TEST_CASE("link codex_toml writes the three keys, fully escaped") {
  const auto got =
      lk::codex_toml("planar-local-h", "---\nname: h\ndescription: \"Quoted \\ desc\"\n---\n\nLine1\n\t\"q\" \\\nend\x01\x7f");
  REQUIRE(got.has_value());
  REQUIRE(*got == "name = \"planar-local-h\"\n"
                  "description = \"Quoted \\\\ desc\"\n"
                  "developer_instructions = \"Line1\\n\\t\\\"q\\\" \\\\\\nend\\u0001\\u007F\"\n");
}

TEST_CASE("link codex_toml strips at most one leading newline from the body") {
  REQUIRE(lk::codex_toml("n", "---\ndescription: d\n---\n\n\nB").value().ends_with("developer_instructions = \"\\nB\"\n"));
  REQUIRE(lk::codex_toml("n", "---\r\ndescription: d\r\n---\r\n\r\nB").value().ends_with("developer_instructions = \"B\"\n"));
}

TEST_CASE("link codex_toml refuses a source with no description or no closed block") {
  REQUIRE_FALSE(lk::codex_toml("n", "---\nname: a\n---\nB").has_value());
  REQUIRE_FALSE(lk::codex_toml("n", "---\ndescription: d\nB").has_value());
  REQUIRE_FALSE(lk::codex_toml("n", "plain").has_value());
}

TEST_CASE("link codex_toml is byte-identical to scripts/render-codex-agents.py") {
  // The engine reimplements the script so it does not depend on a checkout.
  // This case is what keeps the two honest: the script renders the fifteen
  // shipped agents plus a hostile fixture, and every output must match.
  const scratch_home home;
  const fs::path     src = home.root_ / "parity-src";
  const fs::path     out = home.root_ / "parity-out";

  std::size_t shipped = 0;
  for (const auto& entry : fs::directory_iterator(PLANAR_AGENTS_DIR)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with("planar-") && name.ends_with(".md")) {
      home.write(src / name, read(entry.path()));
      ++shipped;
    }
  }
  REQUIRE(shipped == 15);

  home.write(
      src / "planar-hostile.md",
      std::string{
          "---\nname: planar-hostile\ndescription: \"Say \\\"hi\\\" \\ back\"\nmodel: ignored\nplanar:\n  kind: agent\n"
          "  slug: planar-hostile\n---\n\r\n\"\"\"triple\"\"\"\ttab\\\n\x01 ctl \x7f del \b \f caf\xc3\xa9 \xe2\x9c\x93 end\\"});

  const auto command = std::format("\"{}\" \"{}\" \"{}\" \"{}\" > /dev/null 2>&1", PLANAR_PYTHON, PLANAR_RENDER_CODEX_SCRIPT,
                                   src.string(), out.string());
  REQUIRE(std::system(command.c_str()) == 0);

  std::size_t compared = 0;
  for (const auto& entry : fs::directory_iterator(src)) {
    const auto stem     = entry.path().stem().string();
    const auto expected = read(out / (stem + ".toml"));
    REQUIRE(expected != "<ABSENT>");
    const auto got = lk::codex_toml(stem, read(entry.path()));
    INFO(stem);
    REQUIRE(got.has_value());
    REQUIRE(*got == expected);
    ++compared;
  }
  REQUIRE(compared == 16);
}

// --- targets ----------------------------------------------------------------

TEST_CASE("link targets are empty when no vendor marker exists") {
  const scratch_home home;
  REQUIRE(lk::targets(query_for(home.root_, "mine", mf::kind::skill)).empty());
  REQUIRE(lk::targets(query_for(home.root_, "mine", mf::kind::agent)).empty());
}

TEST_CASE("link targets place a skill in the claude and shared roots") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto tgs = lk::targets(query_for(home.root_, "mine", mf::kind::skill));
  REQUIRE(tgs.size() == 2);
  REQUIRE(tgs[1].vendor == "shared");
  REQUIRE(tgs[1].layout == lk::layout::skill_dir);
  REQUIRE(tgs[1].target_path == (home.root_ / ".agents" / "skills" / "planar-local-mine").string());
  REQUIRE(tgs[0].vendor == "claude");
  REQUIRE(tgs[0].target_path == (home.root_ / ".claude" / "skills" / "planar-local-mine").string());
}

TEST_CASE("link targets write the shared root when any one of its readers is present") {
  for (const std::string_view marker : {".codex", ".copilot", ".gemini/settings.json", ".config/opencode"}) {
    const scratch_home home;
    home.mark({marker});
    const auto tgs = lk::targets(query_for(home.root_, "mine", mf::kind::skill));
    INFO(marker);
    REQUIRE(tgs.size() == 1);
    REQUIRE(tgs[0].vendor == "shared");
  }
  // A claude-only home gets no shared root; an antigravity home gets its own.
  const scratch_home claude_only;
  claude_only.mark({".claude"});
  REQUIRE(lk::targets(query_for(claude_only.root_, "mine", mf::kind::skill)).size() == 1);
  const scratch_home anti;
  anti.mark({".gemini/antigravity-cli"});
  const auto tgs = lk::targets(query_for(anti.root_, "mine", mf::kind::skill));
  REQUIRE(tgs.size() == 1);
  REQUIRE(tgs[0].vendor == "antigravity");
  REQUIRE(tgs[0].target_path == (anti.root_ / ".gemini" / "antigravity-cli" / "skills" / "planar-local-mine").string());
}

TEST_CASE("link targets honour a skill's vendors list") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto only_claude = lk::targets(query_for(home.root_, "mine", mf::kind::skill, {"claude"}));
  REQUIRE(only_claude.size() == 1);
  REQUIRE(only_claude[0].vendor == "claude");
  const auto only_copilot = lk::targets(query_for(home.root_, "mine", mf::kind::skill, {"copilot"}));
  REQUIRE(only_copilot.size() == 1);
  REQUIRE(only_copilot[0].vendor == "shared");
}

TEST_CASE("link targets place an agent in every present vendor's directory") {
  const scratch_home home;
  home.mark({".claude", ".codex", ".copilot", ".gemini/settings.json", ".gemini/antigravity-cli", ".config/opencode"});
  // The vendors list is not consulted for an agent.
  const auto tgs = lk::targets(query_for(home.root_, "helper", mf::kind::agent, {"claude"}));
  std::map<std::string, std::pair<std::string, lk::layout>> by_vendor;
  for (const auto& tgt : tgs) {
    by_vendor[tgt.vendor] = {fs::path{tgt.target_path}.lexically_relative(home.root_).generic_string(), tgt.layout};
  }
  REQUIRE(by_vendor.size() == 6);
  REQUIRE(by_vendor["claude"] ==
          std::pair<std::string, lk::layout>{".claude/agents/planar-local-helper.md", lk::layout::agent_md});
  REQUIRE(by_vendor["codex"] ==
          std::pair<std::string, lk::layout>{".codex/agents/planar-local-helper.toml", lk::layout::agent_toml});
  REQUIRE(by_vendor["copilot"] ==
          std::pair<std::string, lk::layout>{".copilot/agents/planar-local-helper.agent.md", lk::layout::agent_copilot});
  REQUIRE(by_vendor["gemini"] ==
          std::pair<std::string, lk::layout>{".gemini/agents/planar-local-helper.md", lk::layout::agent_md});
  REQUIRE(by_vendor["antigravity"] ==
          std::pair<std::string, lk::layout>{".gemini/antigravity-cli/agents/planar-local-helper.md", lk::layout::agent_md});
  REQUIRE(by_vendor["opencode"] ==
          std::pair<std::string, lk::layout>{".config/opencode/agents/planar-local-helper.md", lk::layout::agent_opencode});
}

TEST_CASE("link targets use an explicit CODEX_HOME for Codex, and count it as presence") {
  const scratch_home home;
  auto               query = query_for(home.root_, "helper", mf::kind::agent);
  query.codex_home         = home.root_ / "elsewhere";
  const auto tgs           = lk::targets(query);
  REQUIRE(tgs.size() == 1);
  REQUIRE(tgs[0].vendor == "codex");
  REQUIRE(tgs[0].target_path == (home.root_ / "elsewhere" / "agents" / "planar-local-helper.toml").string());
}

TEST_CASE("link targets refuse an empty home or name") {
  REQUIRE(lk::targets(query_for("", "mine", mf::kind::skill)).empty());
  REQUIRE(lk::targets(query_for("/h", "", mf::kind::skill)).empty());
}

TEST_CASE("link vendor_matches selects the shared root for the vendors that read it") {
  REQUIRE(lk::vendor_matches("claude", "claude"));
  REQUIRE_FALSE(lk::vendor_matches("claude", "codex"));
  REQUIRE(lk::vendor_matches("shared", "codex"));
  REQUIRE(lk::vendor_matches("shared", "copilot"));
  REQUIRE(lk::vendor_matches("shared", "opencode"));
  REQUIRE_FALSE(lk::vendor_matches("shared", "claude"));
  REQUIRE_FALSE(lk::vendor_matches("claude", ""));
}

// --- link: skills -----------------------------------------------------------

TEST_CASE("link projects a skill as a renamed copy and leaves the source alone") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill  = home.add_skill("mine", "name: mine\ndescription: Mine.\n");
  const auto before = read(skill.source_path);

  const auto result = lk::link(skill, home.opts());
  REQUIRE(vendors_of(result) == std::vector<std::string>{"claude", "shared"});
  for (const auto& rec : result.records) {
    REQUIRE(rec.action == "created");
    REQUIRE(rec.mode == mf::mode::copy);
    REQUIRE(rec.linked_at == k_stamp);
    REQUIRE(rec.source_path == skill.source_path);
  }

  const std::string expected = "---\nname: planar-local-mine\ndescription: Mine.\n---\nmine body\n";
  const auto        claude   = home.root_ / ".claude" / "skills" / "planar-local-mine";
  const auto        shared   = home.root_ / ".agents" / "skills" / "planar-local-mine";
  REQUIRE(read(claude / "SKILL.md") == expected);
  REQUIRE(read(shared / "SKILL.md") == expected);
  REQUIRE_FALSE(fs::is_symlink(claude));
  REQUIRE(read(skill.source_path) == before);

  // HOME SAFETY, and the retarget: nothing under the old locations.
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "commands"));
  REQUIRE_FALSE(fs::exists(home.root_ / ".codex" / "skills"));
  REQUIRE_FALSE(fs::exists(home.root_ / ".copilot"));
}

TEST_CASE("link copies a skill's auxiliary files and keeps the executable bit") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("mine", "description: Mine.\n");
  home.write(home.sandbox() / "skills" / "mine" / "scripts" / "run.sh", "#!/bin/sh\necho hi\n");
  fs::permissions(home.sandbox() / "skills" / "mine" / "scripts" / "run.sh", fs::perms::owner_all | fs::perms::group_read,
                  fs::perm_options::replace);

  lk::link(skill, home.opts());
  const auto copy = home.root_ / ".claude" / "skills" / "planar-local-mine" / "scripts" / "run.sh";
  REQUIRE(read(copy) == "#!/bin/sh\necho hi\n");
  REQUIRE((fs::status(copy).permissions() & fs::perms::owner_exec) != fs::perms::none);
}

TEST_CASE("link adds a name when the skill's frontmatter has none") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("mine", "description: Mine.\n"), home.opts());
  REQUIRE(read(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md")
              .starts_with("---\nname: planar-local-mine\ndescription: Mine.\n"));
}

TEST_CASE("link reports unchanged on a second identical run and touches nothing") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("mine", "description: a\n");
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "created");

  const auto file    = home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md";
  const auto mtime   = fs::last_write_time(file);
  const auto stamped = read(mf::manifest_path(home.root_, mf::kind::skill));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  auto opts = home.opts();
  opts.now  = "2030-01-01T00:00:00Z";
  REQUIRE(action_of(lk::link(skill, opts), "claude") == "unchanged");
  REQUIRE((fs::last_write_time(file) == mtime));
  // An unchanged record keeps the stamp it already had, so the manifest is byte-stable.
  REQUIRE(read(mf::manifest_path(home.root_, mf::kind::skill)) == stamped);
}

TEST_CASE("link updates a projection whose source changed") {
  const scratch_home home;
  home.mark({".claude"});
  auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  home.write(skill.source_path, "---\ndescription: a\n---\nnew body\n");
  skill = *mf::parse_file(skill.source_path, mf::kind::skill);

  REQUIRE(state_of_row(home.root_, "mine", "claude") == "stale");
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "updated");
  REQUIRE(read(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md").ends_with("new body\n"));
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "live");
}

TEST_CASE("link refuses a foreign destination and names the file inside it") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill   = home.add_skill("mine", "description: a\n");
  const auto foreign = home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md";
  home.write(foreign, "not planar's\n");

  const std::array files{skill};
  REQUIRE(lk::find_conflict(files, home.opts()) == foreign.string());

  const auto result = lk::link(skill, home.opts());
  REQUIRE(action_of(result, "claude") == "refused");
  REQUIRE(read(foreign) == "not planar's\n");
  // A refused destination is never recorded as Planar's.
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.at(0).links.empty());
}

TEST_CASE("link adopts a byte-identical destination and replaces a recorded one") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("mine", "description: a\n");
  const auto dest  = home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md";
  // Identical to what would be written: not a conflict even with no record.
  home.write(dest, "---\nname: planar-local-mine\ndescription: a\n---\nmine body\n");
  const std::array files{skill};
  REQUIRE_FALSE(lk::find_conflict(files, home.opts()).has_value());
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "unchanged");

  // Now differing, but recorded by the manifest the run above wrote: replaced.
  home.write(dest, "someone edited the copy\n");
  REQUIRE_FALSE(lk::find_conflict(files, home.opts()).has_value());
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "updated");
}

TEST_CASE("link replaces a symlink into the sandbox") {
  const scratch_home home;
  home.mark({".claude"});
  const auto      skill = home.add_skill("mine", "description: a\n");
  const auto      dest  = home.root_ / ".claude" / "skills" / "planar-local-mine";
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  fs::create_directory_symlink(skill.source_path.substr(0, skill.source_path.rfind('/')), dest, ec);
  REQUIRE_FALSE(ec);

  const std::array files{skill};
  REQUIRE_FALSE(lk::find_conflict(files, home.opts()).has_value());
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "updated");
  REQUIRE_FALSE(fs::is_symlink(dest));
  REQUIRE(fs::is_regular_file(dest / "SKILL.md"));
}

TEST_CASE("link --dry-run writes nothing at all") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  auto       opts  = home.opts();
  opts.dry_run     = true;

  const auto result = lk::link(skill, opts);
  REQUIRE(result.records.size() == 2);
  for (const auto& rec : result.records) {
    REQUIRE(rec.action == "dry-run");
    REQUIRE(rec.mode == mf::mode::copy);
    REQUIRE(rec.linked_at.empty());
  }
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills"));
  REQUIRE_FALSE(fs::exists(home.root_ / ".agents"));
  REQUIRE_FALSE(fs::exists(mf::manifest_path(home.root_, mf::kind::skill)));
}

TEST_CASE("link --vendor skips the others, and the skips carry no mode or stamp") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill   = home.add_skill("mine", "description: a\n");
  auto       opts    = home.opts();
  opts.vendor_filter = "codex";

  const auto result = lk::link(skill, opts);
  REQUIRE(result.records.size() == 2);
  REQUIRE(action_of(result, "claude") == "skipped");
  REQUIRE(action_of(result, "shared") == "created");
  for (const auto& rec : result.records) {
    if (rec.action != "skipped") {
      continue;
    }
    REQUIRE_FALSE(rec.mode.has_value());
    REQUIRE(rec.linked_at.empty());
    REQUIRE(rec.warning.empty());
  }
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills"));
}

TEST_CASE("link --vendor NARROWS the manifest rather than merging into it") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.at(0).links.size() == 2);

  auto opts          = home.opts();
  opts.vendor_filter = "claude";
  lk::link(skill, opts);
  const auto after = mf::load_manifest(home.root_, mf::kind::skill);
  REQUIRE(after->entries.size() == 1);
  REQUIRE(after->entries[0].links.size() == 1);
  REQUIRE(after->entries[0].links[0].vendor == "claude");
}

TEST_CASE("link records the timestamp it was GIVEN, never a clock read") {
  const scratch_home home;
  home.mark({".claude"});
  auto opts         = home.opts();
  opts.now          = "1999-12-31T23:59:59Z";
  const auto result = lk::link(home.add_skill("mine", "description: a\n"), opts);
  REQUIRE(result.records[0].linked_at == "1999-12-31T23:59:59Z");
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries[0].links[0].linked_at == "1999-12-31T23:59:59Z");
}

// --- link: agents -----------------------------------------------------------

TEST_CASE("link projects an agent into every present vendor in that vendor's form") {
  const scratch_home home;
  home.mark({".claude", ".codex", ".copilot", ".gemini/settings.json", ".gemini/antigravity-cli", ".config/opencode"});
  const auto agent  = home.add_agent("helper", "description: Helps: a lot.\nmodel: x\n");
  const auto before = read(agent.source_path);

  const auto result = lk::link(agent, home.opts());
  REQUIRE(result.records.size() == 6);
  for (const auto& rec : result.records) {
    INFO(rec.vendor);
    REQUIRE(rec.action == "created");
  }
  const std::string md = "---\nname: planar-local-helper\ndescription: Helps: a lot.\nmodel: x\n---\nhelper body\n";
  REQUIRE(read(home.root_ / ".claude" / "agents" / "planar-local-helper.md") == md);
  REQUIRE(read(home.root_ / ".gemini" / "agents" / "planar-local-helper.md") == md);
  REQUIRE(read(home.root_ / ".gemini" / "antigravity-cli" / "agents" / "planar-local-helper.md") == md);
  REQUIRE(read(home.root_ / ".copilot" / "agents" / "planar-local-helper.agent.md") == md);
  REQUIRE(read(home.root_ / ".config" / "opencode" / "agents" / "planar-local-helper.md") ==
          "---\ndescription: \"Helps: a lot.\"\nmode: subagent\n---\nhelper body\n");
  REQUIRE(read(home.root_ / ".codex" / "agents" / "planar-local-helper.toml") ==
          "name = \"planar-local-helper\"\ndescription = \"Helps: a lot.\"\ndeveloper_instructions = \"helper body\\n\"\n");
  REQUIRE(read(agent.source_path) == before);
  REQUIRE_FALSE(fs::exists(home.root_ / ".planar" / "agents"));
}

TEST_CASE("link skips the vendors an agent without a description cannot be projected to") {
  const scratch_home home;
  home.mark({".claude", ".codex", ".config/opencode"});
  const auto result = lk::link(home.add_agent("bare", "model: x\n"), home.opts());
  REQUIRE(action_of(result, "claude") == "created");
  REQUIRE(action_of(result, "codex") == "skipped");
  REQUIRE(action_of(result, "opencode") == "skipped");
  REQUIRE_FALSE(fs::exists(home.root_ / ".codex" / "agents"));
}

TEST_CASE("link writes the two kinds into SEPARATE manifests") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.size() == 1);
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries[0].name == "alpha");
  REQUIRE(mf::load_manifest(home.root_, mf::kind::agent)->entries.size() == 1);
  REQUIRE(mf::load_manifest(home.root_, mf::kind::agent)->entries[0].name == "beta");
}

TEST_CASE("link REPLACES an existing manifest entry rather than appending") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("alpha", "description: a\n");
  lk::link(skill, home.opts());
  lk::link(skill, home.opts());
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.size() == 1);
}

// --- list -------------------------------------------------------------------

TEST_CASE("link list is empty on a fresh home") {
  const scratch_home home;
  const auto         rows = lk::list(home.root_);
  REQUIRE(rows.has_value());
  REQUIRE(rows->empty());
}

TEST_CASE("link list sorts skills before agents, then name, then vendor") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());
  lk::link(home.add_skill("zeta", "description: z\n"), home.opts());
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());

  const auto               rows = lk::list(home.root_);
  std::vector<std::string> order;
  for (const auto& row : *rows) {
    order.push_back(std::format("{}:{}:{}", mf::kind_name(row.kind), row.name, row.record.vendor));
  }
  REQUIRE(order == std::vector<std::string>{"skill:alpha:claude", "skill:alpha:shared", "skill:zeta:claude", "skill:zeta:shared",
                                            "agent:beta:claude", "agent:beta:codex"});
}

TEST_CASE("link list reports live, stale, missing and broken") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "live");
  REQUIRE(state_of_row(home.root_, "mine", "shared") == "live");

  // stale: a copy edited by hand.
  home.write(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md", "edited\n");
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "stale");
  // stale: an auxiliary file appears in the copy.
  home.write(home.root_ / ".agents" / "skills" / "planar-local-mine" / "extra.txt", "x");
  REQUIRE(state_of_row(home.root_, "mine", "shared") == "stale");

  // missing.
  std::error_code ec;
  fs::remove_all(home.root_ / ".agents" / "skills" / "planar-local-mine", ec);
  REQUIRE(state_of_row(home.root_, "mine", "shared") == "missing");

  // broken: the source is gone.
  fs::remove_all(home.sandbox() / "skills" / "mine", ec);
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "broken");
}

TEST_CASE("link list reports a changed source as stale on every projection") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  std::ofstream(skill.source_path, std::ios::app) << "one more line\n";
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "stale");
  REQUIRE(state_of_row(home.root_, "mine", "shared") == "stale");
}

TEST_CASE("link list reports an old symlink projection as legacy") {
  const scratch_home home;
  const auto         skill = home.add_skill("mine", "description: a\n");
  const auto         old   = home.root_ / ".claude" / "commands" / "local-mine.md";
  std::error_code    ec;
  fs::create_directories(old.parent_path(), ec);
  fs::create_symlink(skill.source_path, old, ec);
  mf::link_manifest manifest{
      1, {{"mine", skill.source_path, {{"claude", old.string(), skill.source_path, mf::mode::symlink, std::string{k_stamp}}}}}};
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, manifest));
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "legacy");
}

TEST_CASE("link list fails rather than lying when a manifest is corrupt") {
  const scratch_home home;
  home.write(mf::manifest_path(home.root_, mf::kind::skill), "{not json");
  REQUIRE_FALSE(lk::list(home.root_).has_value());
}

// --- unlink -----------------------------------------------------------------

TEST_CASE("link unlink removes every recorded copy and the manifest entry, not the source") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());

  const auto result = lk::unlink("mine", mf::kind::skill, {home.root_, false});
  REQUIRE(result.has_value());
  REQUIRE(result->removed.size() == 2);
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills" / "planar-local-mine"));
  REQUIRE_FALSE(fs::exists(home.root_ / ".agents" / "skills" / "planar-local-mine"));
  REQUIRE(fs::exists(skill.source_path));
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.empty());
}

TEST_CASE("link unlink is idempotent and reports nothing the second time") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  REQUIRE(lk::unlink("mine", mf::kind::skill, {home.root_, false})->removed.size() == 1);
  REQUIRE(lk::unlink("mine", mf::kind::skill, {home.root_, false})->removed.empty());
}

TEST_CASE("link unlink still reports installs whose targets already vanished") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  std::error_code ec;
  fs::remove_all(home.root_ / ".claude" / "skills" / "planar-local-mine", ec);
  REQUIRE(lk::unlink("mine", mf::kind::skill, {home.root_, false})->removed.size() == 1);
}

TEST_CASE("link unlink --purge deletes the skill directory, or the agent FILE") {
  const scratch_home home;
  const auto         skill = home.add_skill("mine", "description: a\n");
  const auto         agent = home.add_agent("helper", "description: a\n");
  const auto         s     = lk::unlink("mine", mf::kind::skill, {home.root_, true});
  REQUIRE(s->purged_file == (home.sandbox() / "skills" / "mine").string());
  REQUIRE_FALSE(fs::exists(skill.source_path));
  const auto a = lk::unlink("helper", mf::kind::agent, {home.root_, true});
  REQUIRE(a->purged_file == (home.sandbox() / "agents" / "helper.md").string());
  REQUIRE_FALSE(fs::exists(agent.source_path));
}

TEST_CASE("link unlink --purge reports a path it did not actually delete") {
  const scratch_home home;
  const auto         result = lk::unlink("ghost", mf::kind::skill, {home.root_, true});
  REQUIRE(result->removed.empty());
  REQUIRE(result->purged_file == (home.sandbox() / "skills" / "ghost").string());
}

// --- reconcile --------------------------------------------------------------

namespace {

auto reconcile_opts(const scratch_home& home, std::string_view now = k_stamp) -> lk::reconcile_options {
  return lk::reconcile_options{.home_dir = home.root_, .dry_run = false, .now = std::string{now}};
}

auto total_paths(const std::vector<lk::reconcile_action>& actions) -> std::size_t {
  std::size_t total = 0;
  for (const auto& action : actions) {
    total += action.removed_targets.size();
  }
  return total;
}

} // namespace

TEST_CASE("link reconcile does not resurrect a source that was never linked or was unlinked") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  static_cast<void>(home.add_skill("fresh", "description: a\n"));
  const auto unlinked = home.add_skill("gone", "description: a\n");
  lk::link(unlinked, home.opts());
  lk::unlink("gone", mf::kind::skill, {home.root_, false});

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions.has_value());
  REQUIRE(actions->empty());
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills" / "planar-local-fresh"));
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills" / "planar-local-gone"));
}

TEST_CASE("link reconcile refreshes stale copies, then a second run changes nothing") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  std::ofstream(skill.source_path, std::ios::app) << "another line\n";
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "stale");

  const auto first = lk::reconcile(reconcile_opts(home, "2030-01-01T00:00:00Z"));
  REQUIRE(first.has_value());
  REQUIRE(first->size() == 1);
  REQUIRE(first->at(0).reason == "stale");
  REQUIRE(total_paths(*first) == 2);
  REQUIRE(read(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md").ends_with("another line\n"));
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "live");

  const auto copy     = home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md";
  const auto mtime    = fs::last_write_time(copy);
  const auto manifest = read(mf::manifest_path(home.root_, mf::kind::skill));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  const auto second = lk::reconcile(reconcile_opts(home, "2031-01-01T00:00:00Z"));
  REQUIRE(second.has_value());
  REQUIRE(second->empty());
  REQUIRE((fs::last_write_time(copy) == mtime));
  REQUIRE(read(mf::manifest_path(home.root_, mf::kind::skill)) == manifest);
}

TEST_CASE("link reconcile migrates the old projections it owns and leaves a foreign file") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  const auto agent = home.add_agent("helper", "description: h\n");

  std::error_code ec;
  const auto      old_claude = home.root_ / ".claude" / "commands" / "local-mine.md";
  const auto      old_codex  = home.root_ / ".codex" / "skills" / "local-mine";
  const auto      old_agent  = home.root_ / ".planar" / "agents" / "local-helper.md";
  const auto      foreign    = home.root_ / ".claude" / "commands" / "local-theirs.md";
  fs::create_directories(old_claude.parent_path(), ec);
  fs::create_directories(old_codex.parent_path(), ec);
  fs::create_directories(old_agent.parent_path(), ec);
  fs::create_symlink(skill.source_path, old_claude, ec);
  fs::create_directory_symlink(fs::path{skill.source_path}.parent_path(), old_codex, ec);
  fs::create_symlink(agent.source_path, old_agent, ec);
  home.write(foreign, "theirs\n");

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions.has_value());
  REQUIRE_FALSE(fs::is_symlink(fs::symlink_status(old_claude)));
  REQUIRE_FALSE(fs::exists(fs::symlink_status(old_claude)));
  REQUIRE_FALSE(fs::exists(fs::symlink_status(old_codex)));
  REQUIRE_FALSE(fs::exists(fs::symlink_status(old_agent)));
  REQUIRE(read(foreign) == "theirs\n");
  REQUIRE(fs::is_regular_file(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md"));
  REQUIRE(fs::is_regular_file(home.root_ / ".agents" / "skills" / "planar-local-mine" / "SKILL.md"));
  REQUIRE(fs::is_regular_file(home.root_ / ".claude" / "agents" / "planar-local-helper.md"));
  // The source is intact: removing a directory symlink never reached through it.
  REQUIRE(fs::exists(skill.source_path));

  bool saw_legacy = false;
  for (const auto& action : *actions) {
    saw_legacy = saw_legacy || action.reason == "legacy";
  }
  REQUIRE(saw_legacy);

  const auto again = lk::reconcile(reconcile_opts(home));
  REQUIRE(again->empty());
}

TEST_CASE("link reconcile migrates a RECORDED legacy copy only when it equals the source") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("mine", "description: a\n");
  const auto old   = home.root_ / ".claude" / "commands" / "local-mine.md";
  home.write(old, read(skill.source_path));
  mf::link_manifest manifest{
      1, {{"mine", skill.source_path, {{"claude", old.string(), skill.source_path, mf::mode::copy, std::string{k_stamp}}}}}};
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, manifest));
  REQUIRE(state_of_row(home.root_, "mine", "claude") == "legacy");

  REQUIRE(lk::reconcile(reconcile_opts(home)).has_value());
  REQUIRE_FALSE(fs::exists(old));

  // A hand-edited legacy copy is not provably Planar's: left in place.
  home.write(old, "hand edited\n");
  mf::link_manifest again{
      1, {{"mine", skill.source_path, {{"claude", old.string(), skill.source_path, mf::mode::copy, std::string{k_stamp}}}}}};
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, again));
  REQUIRE(lk::reconcile(reconcile_opts(home)).has_value());
  REQUIRE(read(old) == "hand edited\n");
}

TEST_CASE("link reconcile drops an entry whose SOURCE is gone and removes its copies") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  std::error_code ec;
  fs::remove_all(home.sandbox() / "skills" / "mine", ec);

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions->size() == 1);
  REQUIRE(actions->at(0).reason == "source-missing");
  REQUIRE(actions->at(0).removed_targets.size() == 1);
  REQUIRE_FALSE(fs::exists(home.root_ / ".claude" / "skills" / "planar-local-mine"));
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.empty());
}

TEST_CASE("link reconcile re-creates a missing copy and keeps its healthy siblings' stamps") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  std::error_code ec;
  fs::remove_all(home.root_ / ".claude" / "skills" / "planar-local-mine", ec);

  const auto actions = lk::reconcile(reconcile_opts(home, "2030-01-01T00:00:00Z"));
  REQUIRE(actions->size() == 1);
  REQUIRE(actions->at(0).reason == "target-missing");
  REQUIRE(actions->at(0).removed_targets.size() == 1);

  const auto after = mf::load_manifest(home.root_, mf::kind::skill);
  REQUIRE(after->entries[0].links.size() == 2);
  std::map<std::string, std::string> stamps;
  for (const auto& row : after->entries[0].links) {
    stamps[row.vendor] = row.linked_at;
  }
  REQUIRE(stamps["claude"] == "2030-01-01T00:00:00Z");
  REQUIRE(stamps["shared"] == k_stamp);
}

TEST_CASE("link reconcile --dry-run reports without touching anything") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("mine", "description: a\n");
  lk::link(skill, home.opts());
  std::ofstream(skill.source_path, std::ios::app) << "more\n";
  const auto before = read(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md");

  auto opts          = reconcile_opts(home);
  opts.dry_run       = true;
  const auto actions = lk::reconcile(opts);
  REQUIRE(actions->size() == 1);
  REQUIRE(read(home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md") == before);
}

TEST_CASE("link reconcile refuses a foreign destination and writes nothing") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  const auto skill = home.add_skill("mine", "description: a\n");
  // A tracked source (it has a manifest entry) whose destination is something foreign.
  mf::link_manifest manifest{1, {{"mine", skill.source_path, {}}}};
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, manifest));
  const auto foreign = home.root_ / ".claude" / "skills" / "planar-local-mine" / "SKILL.md";
  home.write(foreign, "foreign\n");

  const auto result = lk::reconcile(reconcile_opts(home));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().why == lk::reconcile_error::cause::conflict);
  REQUIRE(result.error().path == foreign.string());
  REQUIRE(read(foreign) == "foreign\n");
  REQUIRE_FALSE(fs::exists(home.root_ / ".agents"));
}

TEST_CASE("link reconcile walks both kinds in one pass") {
  const scratch_home home;
  home.mark({".claude"});
  const auto skill = home.add_skill("alpha", "description: a\n");
  const auto agent = home.add_agent("beta", "description: b\n");
  lk::link(skill, home.opts());
  lk::link(agent, home.opts());
  std::ofstream(skill.source_path, std::ios::app) << "more\n";
  std::ofstream(agent.source_path, std::ios::app) << "more\n";

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions->size() == 2);
  REQUIRE(actions->at(0).kind == mf::kind::skill);
  REQUIRE(actions->at(1).kind == mf::kind::agent);
  REQUIRE(actions->at(1).reason == "stale");
}

TEST_CASE("link reconcile fails rather than guessing when a manifest is corrupt") {
  const scratch_home home;
  home.write(mf::manifest_path(home.root_, mf::kind::agent), "{not json");
  const auto result = lk::reconcile(reconcile_opts(home));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().why == lk::reconcile_error::cause::manifest_unreadable);
}

// --- ownership of removals, write failures, shared gate ---------------------

TEST_CASE("link unlink leaves a foreign replacement in place and reports it") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  const auto      claude = home.root_ / ".claude" / "skills" / "planar-local-mine";
  std::error_code ec;
  fs::remove_all(claude, ec);
  home.write(claude / "SKILL.md", "---\nname: theirs\n---\ntheir own skill\n");

  const auto result = lk::unlink("mine", mf::kind::skill, {home.root_, false});
  REQUIRE(result.has_value());
  REQUIRE(result->removed.size() == 1);
  REQUIRE(result->removed[0].vendor == "shared");
  REQUIRE(result->skipped.size() == 1);
  REQUIRE(result->skipped[0].target_path == claude.string());
  REQUIRE(result->skipped[0].warning == "skipped: not owned");
  REQUIRE(read(claude / "SKILL.md") == "---\nname: theirs\n---\ntheir own skill\n");
  REQUIRE_FALSE(fs::exists(home.root_ / ".agents" / "skills" / "planar-local-mine"));
}

TEST_CASE("link reconcile source-missing leaves a foreign replacement in place and reports it") {
  const scratch_home home;
  home.mark({".claude", ".codex"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  const auto      claude = home.root_ / ".claude" / "skills" / "planar-local-mine";
  std::error_code ec;
  fs::remove_all(claude, ec);
  home.write(claude / "SKILL.md", "---\nname: theirs\n---\ntheir own skill\n");
  fs::remove_all(home.sandbox() / "skills" / "mine", ec);

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions.has_value());
  std::vector<std::string> removed;
  std::vector<std::string> kept;
  for (const auto& action : *actions) {
    auto& into = action.reason == "source-missing" ? removed : kept;
    REQUIRE((action.reason == "source-missing" || action.reason == "not-owned"));
    into.insert(into.end(), action.removed_targets.begin(), action.removed_targets.end());
  }
  REQUIRE(removed == std::vector<std::string>{(home.root_ / ".agents" / "skills" / "planar-local-mine").string()});
  REQUIRE(kept == std::vector<std::string>{claude.string()});
  REQUIRE(read(claude / "SKILL.md") == "---\nname: theirs\n---\ntheir own skill\n");
  REQUIRE_FALSE(fs::exists(home.root_ / ".agents" / "skills" / "planar-local-mine"));
}

TEST_CASE("link reconcile reports a failed write, does not record it, and leaves the manifest unchanged") {
  const scratch_home home;
  home.mark({".claude"});
  lk::link(home.add_skill("mine", "description: a\n"), home.opts());
  const auto      skills = home.root_ / ".claude" / "skills";
  std::error_code ec;
  fs::remove_all(skills / "planar-local-mine", ec);
  const auto manifest_before = read(mf::manifest_path(home.root_, mf::kind::skill));
  fs::permissions(skills, fs::perms::owner_read | fs::perms::owner_exec, ec);
  // A privileged user can write regardless of the mode, so the failure cannot be provoked.
  if (std::ofstream(skills / "probe").is_open()) {
    fs::permissions(skills, fs::perms::owner_all, ec);
    return;
  }

  const auto actions = lk::reconcile(reconcile_opts(home, "2030-01-01T00:00:00Z"));
  fs::permissions(skills, fs::perms::owner_all, ec);
  REQUIRE(actions.has_value());
  const auto failed = std::ranges::find(*actions, "write-failed", &lk::reconcile_action::reason);
  REQUIRE(failed != actions->end());
  REQUIRE(failed->removed_targets == std::vector<std::string>{(skills / "planar-local-mine").string()});
  REQUIRE(read(mf::manifest_path(home.root_, mf::kind::skill)) == manifest_before);
  REQUIRE_FALSE(fs::exists(skills / "planar-local-mine"));
}

TEST_CASE("link targets write the shared root for a skill limited to gemini or opencode") {
  for (const std::string_view vendor : {"gemini", "opencode"}) {
    const scratch_home home;
    home.mark({vendor == "gemini" ? ".gemini/settings.json" : ".config/opencode"});
    const auto skill  = home.add_skill("mine", std::format("description: a\nvendors: [{}]\n", vendor));
    const auto result = lk::link(skill, home.opts());
    INFO(vendor);
    REQUIRE(vendors_of(result) == std::vector<std::string>{"shared"});
    REQUIRE(fs::is_regular_file(home.root_ / ".agents" / "skills" / "planar-local-mine" / "SKILL.md"));
  }
}

TEST_CASE("link reconcile skips a by-name legacy candidate that is a foreign copy") {
  const scratch_home home;
  home.mark({".claude"});
  const auto        skill = home.add_skill("mine", "description: a\n");
  mf::link_manifest manifest{1, {{"mine", skill.source_path, {}}}};
  REQUIRE(mf::save_manifest(home.root_, mf::kind::skill, manifest));
  const auto foreign = home.root_ / ".claude" / "commands" / "local-mine.md";
  home.write(foreign, "their own command\n");

  const auto actions = lk::reconcile(reconcile_opts(home));
  REQUIRE(actions.has_value());
  REQUIRE(read(foreign) == "their own command\n");
  for (const auto& action : *actions) {
    REQUIRE(action.reason != "legacy");
  }
}

TEST_CASE("link rewrite_name keeps a CRLF line ending") {
  // The fences are literal LF (a CR in a fence is not frontmatter at all, per
  // the manifest contract); CR is tolerated only on the key lines inside.
  REQUIRE(lk::rewrite_name("---\nname: mine\r\ndescription: d\r\n---\nbody\r\n", "planar-local-mine") ==
          "---\nname: planar-local-mine\r\ndescription: d\r\n---\nbody\r\n");
}

// --- timestamps -------------------------------------------------------------

TEST_CASE("link format_utc_stamp matches the manifest's format") {
  // Second precision, always Z, zero-padded throughout.
  REQUIRE(lk::format_utc_stamp(0) == "1970-01-01T00:00:00Z");
  REQUIRE(lk::format_utc_stamp(1'787'458'358) == "2026-08-23T04:12:38Z");
}

TEST_CASE("link format_utc_stamp refuses a negative epoch") {
  REQUIRE(lk::format_utc_stamp(-1).empty());
}

TEST_CASE("link utc_now_stamp produces a well-formed stamp") {
  const auto stamp = lk::utc_now_stamp();
  REQUIRE(stamp.size() == 20);
  REQUIRE(stamp[4] == '-');
  REQUIRE(stamp[10] == 'T');
  REQUIRE(stamp.back() == 'Z');
}
