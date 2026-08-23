// @file link.t.cpp
// @brief Tests for `planar.engine.local.link` (plan 996, task 6109).
//
// ============================================================================
// HOME SAFETY — this is the file that actually creates symlinks.
// ============================================================================
// Every function under test takes an explicit `home_dir`, and the module reads
// no environment at all. Every test here passes a `scratch_home` root under
// `temp_directory_path()`, removed on destruction. `.claude/`, `.codex/` and
// `.copilot/` are therefore created INSIDE that scratch root, never in a real
// home — and the tests assert exactly that, so a regression that started
// resolving a real path would fail loudly rather than quietly succeed.
//
// The one clock read in the module (`utc_now_stamp`) is deliberately NOT called
// by link()/reconcile(); the stamp is passed in. That is what makes every
// timestamp assertion below deterministic.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Fixture: PLANAR_LOCAL_HOME=HOME=/tmp/pb/h, one skill `alpha` and one agent
// `beta`. Captured with `od -c` on separated streams.
//
//   $Z local link --dry-run
//     alpha (skill)
//       claude   dry-run [symlink]  ->  /tmp/pb/h/.claude/commands/local-alpha.md
//       codex    dry-run [symlink]  ->  /tmp/pb/h/.codex/skills/local-alpha
//       copilot  dry-run [symlink]  ->  /tmp/pb/h/.copilot/skills/local-alpha
//     beta (agent)
//       agents   dry-run [symlink]  ->  /tmp/pb/h/.planar/agents/local-beta.md
//     <blank>
//     dry-run: 4 would-be installs across 2 source(s)
//
//   $Z local link            (first run)  -> every action `created`,
//                                            `done: 4 linked, 0 unchanged, 0 skipped across 2 source(s)`
//   $Z local link            (second run) -> `unchanged`
//
//   $Z local list --json     (NDJSON, one line per install)
//     {"Name":"alpha","Kind":"skill","Record":{"vendor":"claude",
//      "target_path":"/tmp/pb/h/.claude/commands/local-alpha.md",
//      "source_path":"/tmp/pb/h/.planar/local/skills/alpha/SKILL.md",
//      "mode":"symlink","action":"live","linked_at":"2026-08-23T04:12:27Z"}}
//     ... codex and copilot with source_path = the DIRECTORY, no /SKILL.md ...
//     {"Name":"beta","Kind":"agent","Record":{"vendor":"agents", ...}}
//
//   $Z local link --vendor codex --json   (the filter's effect on records)
//     ...,"Records":[{"vendor":"claude",...,"mode":"","action":"skipped"},
//                    {"vendor":"codex", ...,"mode":"symlink","action":"unchanged",
//                     "linked_at":"..."},
//                    {"vendor":"copilot",...,"mode":"","action":"skipped"}]
//     -- note the skipped records carry NEITHER `linked_at` NOR `warning`,
//        and `mode` is the EMPTY STRING rather than null or absent.
//
//   CODEX_HOME was pointed elsewhere throughout and the codex install still
//   landed under $PLANAR_LOCAL_HOME/.codex/skills. It is not consulted.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.local.manifest;
import planar.engine.local.link;

namespace mf = planar::engine::local::manifest;
namespace lk = planar::engine::local::link;

namespace {

constexpr std::string_view k_stamp = "2026-08-23T04:12:27Z";

/// @brief A per-test scratch home, removed on destruction.
///
/// `root_` stands in for `$PLANAR_LOCAL_HOME`. Both the sandbox
/// (`root_/.planar/local`) and every vendor directory (`root_/.claude`, ...)
/// live under it, so nothing this file writes can escape.
struct scratch_home {
  std::filesystem::path root_;

  scratch_home()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_local_link_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(sandbox() / "skills", ec);
    std::filesystem::create_directories(sandbox() / "agents", ec);
  }
  scratch_home(const scratch_home&)            = delete;
  scratch_home& operator=(const scratch_home&) = delete;
  ~scratch_home() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto sandbox() const -> std::filesystem::path {
    return root_ / ".planar" / "local";
  }

  auto write(const std::filesystem::path& path, std::string_view content) const -> void {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
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

} // namespace

// --- targets ----------------------------------------------------------------

TEST_CASE("link targets place each vendor in its own directory shape") {
  const auto vendors = std::vector<std::string>{"claude", "codex", "copilot"};
  const auto tgs     = lk::targets("/h", "/h/.planar/local/skills/alpha", "alpha", mf::kind::skill, false, vendors);
  REQUIRE(tgs.size() == 3);

  // claude: a FILE link to SKILL.md, named `local-<name>.md`.
  REQUIRE(tgs[0].vendor == "claude");
  REQUIRE(tgs[0].layout == lk::layout::flat);
  REQUIRE(tgs[0].target_path == "/h/.claude/commands/local-alpha.md");
  REQUIRE(tgs[0].link_target == "/h/.planar/local/skills/alpha/SKILL.md");

  // codex and copilot: a DIRECTORY link, with no `.md` suffix on either side.
  REQUIRE(tgs[1].vendor == "codex");
  REQUIRE(tgs[1].layout == lk::layout::dir_symlink);
  REQUIRE(tgs[1].target_path == "/h/.codex/skills/local-alpha");
  REQUIRE(tgs[1].link_target == "/h/.planar/local/skills/alpha");

  REQUIRE(tgs[2].vendor == "copilot");
  REQUIRE(tgs[2].layout == lk::layout::dir_symlink);
  REQUIRE(tgs[2].target_path == "/h/.copilot/skills/local-alpha");
}

TEST_CASE("link targets sort by vendor regardless of input order") {
  const auto vendors = std::vector<std::string>{"copilot", "claude"};
  const auto tgs     = lk::targets("/h", "/s/alpha", "alpha", mf::kind::skill, false, vendors);
  REQUIRE(tgs.size() == 2);
  REQUIRE(tgs[0].vendor == "claude");
  REQUIRE(tgs[1].vendor == "copilot");
}

TEST_CASE("link targets for an agent ignore vendors entirely") {
  // ONE target, not into a vendor directory, with the literal vendor `agents`
  // — and the `vendors:` list is not consulted at all.
  const auto vendors = std::vector<std::string>{"claude"};
  const auto tgs     = lk::targets("/h", "/s", "beta", mf::kind::agent, false, vendors);
  REQUIRE(tgs.size() == 1);
  REQUIRE(tgs[0].vendor == "agents");
  REQUIRE(tgs[0].target_path == "/h/.planar/agents/local-beta.md");
  // The EMPTY link_target is the signal link() reads as "point at the source
  // file itself". It reaches the manifest's source_path, so it is observable.
  REQUIRE(tgs[0].link_target.empty());
}

TEST_CASE("link targets drop the local- prefix under shadow") {
  const auto vendors = std::vector<std::string>{"claude", "codex"};
  const auto tgs     = lk::targets("/h", "/s/a", "alpha", mf::kind::skill, true, vendors);
  REQUIRE(tgs[0].target_path == "/h/.claude/commands/alpha.md");
  REQUIRE(tgs[1].target_path == "/h/.codex/skills/alpha");
}

TEST_CASE("link targets skip an unknown vendor rather than failing") {
  const auto vendors = std::vector<std::string>{"claude", "bogus"};
  const auto tgs     = lk::targets("/h", "/s/a", "alpha", mf::kind::skill, false, vendors);
  REQUIRE(tgs.size() == 1);
  REQUIRE(tgs[0].vendor == "claude");
}

TEST_CASE("link targets refuse an empty home or name") {
  REQUIRE(lk::targets("", "/s", "alpha", mf::kind::skill, false, {}).empty());
  REQUIRE(lk::targets("/h", "/s", "", mf::kind::skill, false, {}).empty());
}

// --- link -------------------------------------------------------------------

TEST_CASE("link installs a skill into all three vendors and an agent into one") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: Alpha skill.\nkind: skill\n");
  const auto         agent = home.add_agent("beta", "description: Beta agent.\nkind: agent\n");

  const auto skill_result = lk::link(skill, home.opts());
  REQUIRE(vendors_of(skill_result) == std::vector<std::string>{"claude", "codex", "copilot"});
  for (const auto& rec : skill_result.records) {
    REQUIRE(rec.action == "created");
    REQUIRE(rec.mode == mf::mode::symlink);
    REQUIRE(rec.linked_at == k_stamp);
  }

  const auto agent_result = lk::link(agent, home.opts());
  REQUIRE(vendors_of(agent_result) == std::vector<std::string>{"agents"});

  // HOME SAFETY, asserted rather than assumed: every install landed inside the
  // scratch root.
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".codex" / "skills" / "local-alpha"));
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".copilot" / "skills" / "local-alpha"));
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".planar" / "agents" / "local-beta.md"));
}

TEST_CASE("link points claude at SKILL.md and codex at the DIRECTORY") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  lk::link(skill, home.opts());

  REQUIRE(std::filesystem::read_symlink(home.root_ / ".claude" / "commands" / "local-alpha.md") ==
          home.sandbox() / "skills" / "alpha" / "SKILL.md");
  REQUIRE(std::filesystem::read_symlink(home.root_ / ".codex" / "skills" / "local-alpha") == home.sandbox() / "skills" / "alpha");
}

TEST_CASE("link reports unchanged on a second identical run") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "created");
  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "unchanged");
}

TEST_CASE("link reports updated when the target points somewhere else") {
  const scratch_home home;
  const auto         skill  = home.add_skill("alpha", "description: a\n");
  const auto         target = home.root_ / ".claude" / "commands" / "local-alpha.md";
  std::error_code    ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::filesystem::create_symlink("/elsewhere", target, ec);

  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "updated");
  REQUIRE(std::filesystem::read_symlink(target) == home.sandbox() / "skills" / "alpha" / "SKILL.md");
}

TEST_CASE("link replaces a REAL file standing where the install goes") {
  // Not a symlink at all — classify_current falls through to plain existence
  // and reports `changed`, so the install replaces it.
  const scratch_home home;
  const auto         skill  = home.add_skill("alpha", "description: a\n");
  const auto         target = home.root_ / ".claude" / "commands" / "local-alpha.md";
  home.write(target, "an operator put this here by hand\n");

  REQUIRE(action_of(lk::link(skill, home.opts()), "claude") == "updated");
  REQUIRE(std::filesystem::is_symlink(target));
}

TEST_CASE("link --dry-run writes nothing at all") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  auto               opts  = home.opts();
  opts.dry_run             = true;

  const auto result = lk::link(skill, opts);
  for (const auto& rec : result.records) {
    REQUIRE(rec.action == "dry-run");
    REQUIRE(rec.mode == mf::mode::symlink);
    // No stamp: nothing was linked, so there is no time at which it was.
    REQUIRE(rec.linked_at.empty());
  }
  REQUIRE_FALSE(std::filesystem::exists(home.root_ / ".claude"));
  // And crucially NO manifest — recording a link never made would make `list`
  // report a live install that does not exist.
  REQUIRE_FALSE(std::filesystem::exists(mf::manifest_path(home.root_, mf::kind::skill)));
}

TEST_CASE("link --vendor skips the others, and the skips carry no mode or stamp") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  auto               opts  = home.opts();
  opts.vendor_filter       = "codex";

  const auto result = lk::link(skill, opts);
  REQUIRE(result.records.size() == 3);
  REQUIRE(action_of(result, "claude") == "skipped");
  REQUIRE(action_of(result, "codex") == "created");
  REQUIRE(action_of(result, "copilot") == "skipped");
  for (const auto& rec : result.records) {
    if (rec.action != "skipped") {
      continue;
    }
    // These three facts are what the renderer turns into an absent `linked_at`,
    // an absent `warning`, and `"mode":""`.
    REQUIRE_FALSE(rec.mode.has_value());
    REQUIRE(rec.linked_at.empty());
    REQUIRE(rec.warning.empty());
  }
  REQUIRE_FALSE(std::filesystem::exists(home.root_ / ".claude"));
}

TEST_CASE("link --vendor NARROWS the manifest rather than merging into it") {
  // Oracle-confirmed: after `local link --vendor codex`, the entry held ONLY
  // the codex link even though a previous unfiltered run had recorded three.
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  lk::link(skill, home.opts());
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.at(0).links.size() == 3);

  auto opts          = home.opts();
  opts.vendor_filter = "codex";
  lk::link(skill, opts);

  const auto after = mf::load_manifest(home.root_, mf::kind::skill);
  REQUIRE(after->entries.size() == 1);
  REQUIRE(after->entries[0].links.size() == 1);
  REQUIRE(after->entries[0].links[0].vendor == "codex");
}

TEST_CASE("link attaches a shadow warning naming the installed filename") {
  const scratch_home home;
  const auto         skill  = home.add_skill("alpha", "description: a\nshadow: true\n");
  const auto         result = lk::link(skill, home.opts());
  for (const auto& rec : result.records) {
    REQUIRE(rec.warning == std::format("shadow: linked as \"{}\" (no local- prefix); any canonical install with this "
                                       "name is replaced",
                                       std::filesystem::path{rec.target_path}.filename().string()));
  }
  // The filename differs per vendor, so the warning does too — `alpha.md` for
  // claude and bare `alpha` for the directory-linking vendors.
  REQUIRE(result.records[0].warning.find("\"alpha.md\"") != std::string::npos);
  REQUIRE(result.records[1].warning.find("\"alpha\"") != std::string::npos);
}

TEST_CASE("link force_copy copies instead of linking and warns about it") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  auto               opts  = home.opts();
  opts.force_copy          = true;

  const auto result = lk::link(skill, opts);
  for (const auto& rec : result.records) {
    REQUIRE(rec.mode == mf::mode::copy);
    REQUIRE(rec.warning == "copy fallback: edits to source require re-running `planar local link`");
  }
  // A real file / directory, NOT a symlink — the whole point of the fallback.
  REQUIRE_FALSE(std::filesystem::is_symlink(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(std::filesystem::is_regular_file(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(std::filesystem::is_directory(home.root_ / ".codex" / "skills" / "local-alpha"));
  REQUIRE(std::filesystem::is_regular_file(home.root_ / ".codex" / "skills" / "local-alpha" / "SKILL.md"));
}

TEST_CASE("link's shadow warning WINS over the copy-fallback warning") {
  // The Zig original only sets the copy warning `if (rec.warning.len == 0)`, so
  // a shadowed skill that also fell back to a copy reports only the shadow. The
  // operator loses the copy notice; that is the oracle's behavior.
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\nshadow: true\n");
  auto               opts  = home.opts();
  opts.force_copy          = true;

  const auto result = lk::link(skill, opts);
  REQUIRE(result.records[0].warning.starts_with("shadow: "));
  REQUIRE(result.records[0].warning.find("copy fallback") == std::string::npos);
}

TEST_CASE("link records the timestamp it was GIVEN, never a clock read") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  auto               opts  = home.opts();
  opts.now                 = "1999-12-31T23:59:59Z";
  const auto result        = lk::link(skill, opts);
  REQUIRE(result.records[0].linked_at == "1999-12-31T23:59:59Z");
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries[0].links[0].linked_at == "1999-12-31T23:59:59Z");
}

TEST_CASE("link writes the two kinds into SEPARATE manifests") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());

  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.size() == 1);
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries[0].name == "alpha");
  REQUIRE(mf::load_manifest(home.root_, mf::kind::agent)->entries.size() == 1);
  REQUIRE(mf::load_manifest(home.root_, mf::kind::agent)->entries[0].name == "beta");
}

TEST_CASE("link REPLACES an existing manifest entry rather than appending") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
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
  lk::link(home.add_skill("zeta", "description: z\n"), home.opts());
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());

  const auto rows = lk::list(home.root_);
  REQUIRE(rows->size() == 7); // 3 + 3 skills, 1 agent
  REQUIRE(rows->at(0).name == "alpha");
  REQUIRE(rows->at(0).record.vendor == "claude");
  REQUIRE(rows->at(1).record.vendor == "codex");
  REQUIRE(rows->at(2).record.vendor == "copilot");
  REQUIRE(rows->at(3).name == "zeta");
  // The agent sorts LAST even though `beta` < `zeta`: kind is the primary key.
  REQUIRE(rows->at(6).name == "beta");
  REQUIRE(rows->at(6).kind == mf::kind::agent);
}

TEST_CASE("link list reports live for a healthy install") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  for (const auto& row : *lk::list(home.root_)) {
    REQUIRE(row.record.action == "live");
  }
}

TEST_CASE("link list reports missing when the install is gone") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove(home.root_ / ".claude" / "commands" / "local-alpha.md", ec);

  const auto rows = lk::list(home.root_);
  REQUIRE(rows->at(0).record.vendor == "claude");
  REQUIRE(rows->at(0).record.action == "missing");
  REQUIRE(rows->at(1).record.action == "live");
}

TEST_CASE("link list reports broken when the SOURCE is gone") {
  // The symlink still points where the manifest says; the thing it points AT
  // has vanished. `missing` would be wrong — the install is there, it just
  // dangles.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove_all(home.sandbox() / "skills" / "alpha", ec);
  for (const auto& row : *lk::list(home.root_)) {
    REQUIRE(row.record.action == "broken");
  }
}

TEST_CASE("link list reports broken when the link was repointed") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  const auto      target = home.root_ / ".claude" / "commands" / "local-alpha.md";
  std::error_code ec;
  std::filesystem::remove(target, ec);
  std::filesystem::create_symlink("/somewhere/else", target, ec);
  REQUIRE(lk::list(home.root_)->at(0).record.action == "broken");
}

TEST_CASE("link list reports LIVE for a plain file that is not a symlink") {
  // The trap. `readlink` fails, so the check falls back to plain existence and
  // an operator-authored file reports healthy. Pinned rather than corrected —
  // "fixing" it would be a parity break.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  const auto      target = home.root_ / ".claude" / "commands" / "local-alpha.md";
  std::error_code ec;
  std::filesystem::remove(target, ec);
  home.write(target, "a hand-written replacement\n");
  REQUIRE(lk::list(home.root_)->at(0).record.action == "live");
}

TEST_CASE("link list fails rather than lying when a manifest is corrupt") {
  const scratch_home home;
  home.write(mf::manifest_path(home.root_, mf::kind::skill), "{ not json");
  REQUIRE_FALSE(lk::list(home.root_).has_value());
}

// --- unlink -----------------------------------------------------------------

TEST_CASE("link unlink removes every recorded install and the manifest entry") {
  const scratch_home home;
  const auto         skill = home.add_skill("alpha", "description: a\n");
  lk::link(skill, home.opts());

  const auto result = lk::unlink("alpha", mf::kind::skill, {home.root_, false});
  REQUIRE(result.has_value());
  REQUIRE(result->removed.size() == 3);
  for (const auto& rec : result->removed) {
    REQUIRE(rec.action == "removed");
  }
  REQUIRE_FALSE(std::filesystem::exists(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.empty());
  // The SOURCE survives without --purge.
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "alpha" / "SKILL.md"));
  REQUIRE(result->purged_file.empty());
}

TEST_CASE("link unlink through a directory symlink does NOT delete the source") {
  // remove() unlinks the link itself. If this ever started following the codex
  // directory link, unlinking a skill would delete the operator's source.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  lk::unlink("alpha", mf::kind::skill, {home.root_, false});
  REQUIRE_FALSE(std::filesystem::exists(home.root_ / ".codex" / "skills" / "local-alpha"));
  REQUIRE(std::filesystem::exists(home.sandbox() / "skills" / "alpha" / "SKILL.md"));
}

TEST_CASE("link unlink is idempotent and reports nothing the second time") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  REQUIRE(lk::unlink("alpha", mf::kind::skill, {home.root_, false})->removed.size() == 3);
  REQUIRE(lk::unlink("alpha", mf::kind::skill, {home.root_, false})->removed.empty());
}

TEST_CASE("link unlink still reports installs whose targets already vanished") {
  // Driven by the MANIFEST, not a filesystem scan — which is what makes it
  // possible to clean up a half-deleted install.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove(home.root_ / ".claude" / "commands" / "local-alpha.md", ec);
  REQUIRE(lk::unlink("alpha", mf::kind::skill, {home.root_, false})->removed.size() == 3);
}

TEST_CASE("link unlink --purge deletes the skill directory") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  const auto result = lk::unlink("alpha", mf::kind::skill, {home.root_, true});
  REQUIRE(result->purged_file == (home.sandbox() / "skills" / "alpha").string());
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "skills" / "alpha"));
}

TEST_CASE("link unlink --purge deletes the agent FILE, not a directory") {
  const scratch_home home;
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());
  const auto result = lk::unlink("beta", mf::kind::agent, {home.root_, true});
  REQUIRE(result->purged_file == (home.sandbox() / "agents" / "beta.md").string());
  REQUIRE_FALSE(std::filesystem::exists(home.sandbox() / "agents" / "beta.md"));
}

TEST_CASE("link unlink --purge reports a path it did not actually delete") {
  // The delete's error is DISCARDED, so `purged_file` means "the path purge
  // targeted", not "the path that was deleted". A name that never existed
  // still reports one, and the CLI prints `purged source file: <path>`.
  const scratch_home home;
  const auto         result = lk::unlink("never-existed", mf::kind::skill, {home.root_, true});
  REQUIRE(result.has_value());
  REQUIRE(result->removed.empty());
  REQUIRE(result->purged_file == (home.sandbox() / "skills" / "never-existed").string());
}

// --- reconcile --------------------------------------------------------------

TEST_CASE("link reconcile does nothing when everything is healthy") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  const auto actions = lk::reconcile({home.root_, false, std::string{k_stamp}});
  REQUIRE(actions.has_value());
  REQUIRE(actions->empty());
}

TEST_CASE("link reconcile drops an entry whose SOURCE is gone") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove_all(home.sandbox() / "skills" / "alpha", ec);

  const auto actions = lk::reconcile({home.root_, false, std::string{k_stamp}});
  REQUIRE(actions->size() == 1);
  REQUIRE(actions->at(0).reason == "source-missing");
  REQUIRE(actions->at(0).removed_targets.size() == 3);
  REQUIRE_FALSE(std::filesystem::exists(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.empty());
}

TEST_CASE("link reconcile RE-CREATES a missing install and keeps the entry") {
  // The second of the two repairs, and the one whose field names do not fit:
  // these paths were repaired, not removed.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove(home.root_ / ".claude" / "commands" / "local-alpha.md", ec);

  const auto actions = lk::reconcile({home.root_, false, "2030-01-01T00:00:00Z"});
  REQUIRE(actions->size() == 1);
  REQUIRE(actions->at(0).reason == "target-missing");
  REQUIRE(actions->at(0).removed_targets.size() == 1);
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".claude" / "commands" / "local-alpha.md"));

  const auto after = mf::load_manifest(home.root_, mf::kind::skill);
  REQUIRE(after->entries.size() == 1);
  // The repaired row got a FRESH stamp; its healthy siblings kept the old one.
  REQUIRE(after->entries[0].links[0].linked_at == "2030-01-01T00:00:00Z");
  REQUIRE(after->entries[0].links[1].linked_at == k_stamp);
}

TEST_CASE("link reconcile --dry-run reports without touching anything") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove_all(home.sandbox() / "skills" / "alpha", ec);

  const auto actions = lk::reconcile({home.root_, true, std::string{k_stamp}});
  REQUIRE(actions->size() == 1);
  // The installs are STILL THERE and the manifest is untouched — this is the
  // assertion that distinguishes a real rehearsal from a reporting bug.
  //
  // `is_symlink`, NOT `exists`: with the source deleted the install is now a
  // DANGLING link, and `exists()` follows it and reports false. Asserting with
  // `exists` here made this test fail against a correct implementation — the
  // same distinction the module's own `path_exists` helper has to make, and the
  // reason it consults `symlink_status` rather than `exists` alone.
  REQUIRE(std::filesystem::is_symlink(home.root_ / ".claude" / "commands" / "local-alpha.md"));
  REQUIRE(mf::load_manifest(home.root_, mf::kind::skill)->entries.size() == 1);
}

TEST_CASE("link reconcile repairs a codex DIRECTORY link with the right layout") {
  // If the layout were computed as `flat` here, the repair would write a single
  // file where a directory link belongs, and codex would stop seeing the
  // skill's auxiliary files.
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  const auto      target = home.root_ / ".codex" / "skills" / "local-alpha";
  std::error_code ec;
  std::filesystem::remove(target, ec);

  lk::reconcile({home.root_, false, std::string{k_stamp}});
  REQUIRE(std::filesystem::is_symlink(target));
  REQUIRE(std::filesystem::read_symlink(target) == home.sandbox() / "skills" / "alpha");
}

TEST_CASE("link reconcile walks both kinds in one pass") {
  const scratch_home home;
  lk::link(home.add_skill("alpha", "description: a\n"), home.opts());
  lk::link(home.add_agent("beta", "description: b\n"), home.opts());
  std::error_code ec;
  std::filesystem::remove_all(home.sandbox() / "skills" / "alpha", ec);
  std::filesystem::remove(home.sandbox() / "agents" / "beta.md", ec);

  const auto actions = lk::reconcile({home.root_, false, std::string{k_stamp}});
  REQUIRE(actions->size() == 2);
  REQUIRE(actions->at(0).kind == mf::kind::skill);
  REQUIRE(actions->at(1).kind == mf::kind::agent);
}

// --- timestamps -------------------------------------------------------------

TEST_CASE("link format_utc_stamp matches the manifest's format") {
  // Second precision, always Z, zero-padded throughout — the shape the oracle
  // writes into `linked_at`.
  REQUIRE(lk::format_utc_stamp(0) == "1970-01-01T00:00:00Z");
  REQUIRE(lk::format_utc_stamp(1'787'458'358) == "2026-08-23T04:12:38Z");
}

TEST_CASE("link format_utc_stamp refuses a negative epoch") {
  // The Zig original returns error.InvalidData for a pre-epoch clock rather
  // than formatting a negative year.
  REQUIRE(lk::format_utc_stamp(-1).empty());
}

TEST_CASE("link utc_now_stamp produces a well-formed stamp") {
  const auto stamp = lk::utc_now_stamp();
  REQUIRE(stamp.size() == 20);
  REQUIRE(stamp[4] == '-');
  REQUIRE(stamp[10] == 'T');
  REQUIRE(stamp.back() == 'Z');
}
