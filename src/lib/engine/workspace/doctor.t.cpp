// @file doctor.t.cpp
// @brief Tests for `planar.engine.workspace.doctor` and
// `planar.engine.workspace.identity` (plan 996, task 6110).
//
// ============================================================================
// HOME SAFETY — and why redirecting the environment is NOT enough here
// ============================================================================
// `identity::planar_home` takes an explicit env-lookup callable, so nothing in
// these modules can find a real `~/.planar` on its own, and every test here
// hands it a map over a `scratch_tree` under `temp_directory_path()`.
//
// That alone would NOT make this bucket safe, and the difference was
// established by probing rather than assumed. **`workspace doctor` writes to
// the association's recorded `config_json.root_path`, not to the current
// working directory.** Oracle-verified: run from `/tmp/ws/elsewhere`, doctor
// installed `AGENTS.md` / `CLAUDE.md` into `/tmp/ws/cwd` — the root recorded at
// `workspace init` time — and left the cwd empty:
//
//   $ cd /tmp/ws/elsewhere && $Z workspace doctor --json
//     ... "reinstalled symlink /tmp/ws/cwd/AGENTS.md → ..."
//   $ ls -a /tmp/ws/elsewhere    ->  .  ..          (nothing written)
//   $ ls -a /tmp/ws/cwd          ->  AGENTS.md  CLAUDE.md  repo-a  repo-b
//
// So cwd isolation buys nothing: the destination comes out of the DATABASE.
// The protection is a scratch DB, and every test below writes its own
// `root_path` INTO its own scratch tree before running doctor. A test that
// seeded a real path would edit the developer's machine no matter what the
// environment said.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Fixture: PLANAR_DB / PLANAR_CONFIG_PATH / PLANAR_HOME / PLANAR_LOCAL_HOME /
// HOME all under /tmp/ws; a workspace root at /tmp/ws/cwd holding two child git
// repos; `workspace init --no-scan --name Acme --slug acme` to register org 1.
// Captured with `python3 -c "print(repr(open(f,'rb').read()))"` on separated
// streams.
//
// --- no orgs at all --------------------------------------------------------
//   $Z workspace doctor --json          exit 0, b'{"orgs":[]}\n'
//   $Z workspace doctor                 exit 0, b''            <-- ZERO BYTES
//   $Z workspace routing show --json    exit 1, stderr
//       b'error: no org associations registered; create one with `planar workspace init`\n'
//     (doctor does NOT refuse; its three siblings do.)
//
// --- one org, state dir present, links absent ------------------------------
//   {"orgs":[{"slug":"acme","org_id":1,"issues_found":4,"issues_repaired":[
//     {"kind":"missing","detail":"<PH>/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)"},
//     {"kind":"missing","detail":"<PH>/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)"},
//     {"kind":"fix","detail":"reinstalled symlink /tmp/ws/cwd/AGENTS.md → <PH>/workspaces/1/AGENTS.md"},
//     {"kind":"fix","detail":"reinstalled symlink /tmp/ws/cwd/CLAUDE.md → <PH>/workspaces/1/AGENTS.md"}]}]}
//
// --- state dir absent too --------------------------------------------------
//   issues_found 5, with `{"kind":"fix","detail":"created state dir <PH>/workspaces/1"}` FIRST.
//
// --- links already correct -------------------------------------------------
//   {"orgs":[{"slug":"acme","org_id":1,"issues_found":2,"issues_repaired":[
//     ...the two `missing` rows only...]}]}
//   text: b'missing: <PH>/workspaces/1/AGENTS.md (run ...)\n'
//         b'missing: <PH>/workspaces/1/routing-table.json (run ...)\n'
//         b'org:acme repaired 2 issues\n'
//   -- "repaired 2 issues" having repaired NOTHING. `issues_found` is a length.
//
// --- workspace_shape: "meta-repo" ------------------------------------------
//   issues_found 2 (the two `missing` rows); NO symlinks installed at all.
//
// --- config_json 'not json' ------------------------------------------------
//   ...+ {"kind":"error","detail":"workspace config_json is malformed; skipping root guidance repair"}
//
// --- config_json '{"a":1}' -------------------------------------------------
//   ...+ {"kind":"error","detail":"workspace config_json has no root_path; skipping root guidance repair"}
//
// The arrow in every `fix` detail is U+2192 (e2 86 92).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workspace.identity;
import planar.engine.workspace.doctor;

namespace id = planar::engine::workspace::identity;
namespace dr = planar::engine::workspace::doctor;

namespace {

/// @brief A per-test scratch tree holding the DB, the `$PLANAR_HOME`, the
/// workspace root, and an unrelated cwd — all removed on destruction.
struct scratch_tree {
  std::filesystem::path root_;

  scratch_tree()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_ws_doctor_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(planar_home(), ec);
    std::filesystem::create_directories(workspace_root(), ec);
    std::filesystem::create_directories(root_ / "elsewhere", ec);
  }
  scratch_tree(const scratch_tree&)            = delete;
  scratch_tree& operator=(const scratch_tree&) = delete;
  ~scratch_tree() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto planar_home() const -> std::filesystem::path {
    return root_ / "home" / ".planar";
  }
  /// @brief Stands in for the association's recorded `root_path`.
  [[nodiscard]] auto workspace_root() const -> std::filesystem::path {
    return root_ / "ws";
  }
  [[nodiscard]] auto db_path() const -> std::filesystem::path {
    return root_ / "w.db";
  }
  [[nodiscard]] auto state_dir(std::int64_t org_id) const -> std::filesystem::path {
    return planar_home() / "workspaces" / std::format("{}", org_id);
  }

  /// @brief The ONLY environment these tests ever supply.
  [[nodiscard]] auto env() const -> id::env_lookup {
    return [home = planar_home().string()](std::string_view name) -> std::optional<std::string> {
      if (name == "PLANAR_HOME") {
        return home;
      }
      return std::nullopt;
    };
  }
};

auto env_of(std::map<std::string, std::string, std::less<>> vars) -> id::env_lookup {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(name);
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

auto open_migrated(const scratch_tree& tree) -> planar::db::connection {
  auto conn = planar::db::connection::open(tree.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

auto add_org(planar::db::connection& conn, std::string_view slug, std::string_view name,
             std::optional<std::string_view> config_json) -> void {
  auto stmt = conn.prepare("insert into associations (kind, slug, name, config_json) "
                           "values ('org', ?, ?, ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, name).has_value());
  if (config_json) {
    REQUIRE(stmt->bind_text(3, *config_json).has_value());
  } else {
    REQUIRE(stmt->bind_null(3).has_value());
  }
  REQUIRE(stmt->step().has_value());
}

auto kinds_of(const dr::org_report& report) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& item : report.issues) {
    out.push_back(item.kind);
  }
  return out;
}

auto has_detail(const dr::org_report& report, std::string_view needle) -> bool {
  return std::ranges::any_of(report.issues, [&](const dr::issue& item) { return item.detail.find(needle) != std::string::npos; });
}

} // namespace

// ===========================================================================
// identity: PLANAR_HOME resolution
// ===========================================================================

TEST_CASE("workspace planar_home prefers PLANAR_HOME") {
  REQUIRE(id::planar_home(env_of({{"PLANAR_HOME", "/scratch/ph"}, {"HOME", "/scratch/home"}})) ==
          std::filesystem::path{"/scratch/ph"});
}

TEST_CASE("workspace planar_home falls back to HOME/.planar") {
  // Note this bucket DOES consult PLANAR_HOME, unlike engine/local, which never
  // does. The two surfaces genuinely disagree about the same conceptual home.
  REQUIRE(id::planar_home(env_of({{"HOME", "/scratch/home"}})) == std::filesystem::path{"/scratch/home/.planar"});
}

TEST_CASE("workspace planar_home treats an EMPTY PLANAR_HOME as unset") {
  REQUIRE(id::planar_home(env_of({{"PLANAR_HOME", ""}, {"HOME", "/scratch/home"}})) ==
          std::filesystem::path{"/scratch/home/.planar"});
}

TEST_CASE("workspace planar_home expands a leading tilde") {
  REQUIRE(id::planar_home(env_of({{"PLANAR_HOME", "~"}, {"HOME", "/scratch/home"}})) == std::filesystem::path{"/scratch/home"});
  REQUIRE(id::planar_home(env_of({{"PLANAR_HOME", "~/custom"}, {"HOME", "/scratch/home"}})) ==
          std::filesystem::path{"/scratch/home/custom"});
}

TEST_CASE("workspace planar_home leaves a NON-LEADING tilde alone") {
  // Only the two exact shapes `~` and `~/...` expand. A `~` elsewhere is a
  // literal character in a path, not a home reference.
  REQUIRE(id::planar_home(env_of({{"PLANAR_HOME", "/a/~/b"}, {"HOME", "/scratch/home"}})) == std::filesystem::path{"/a/~/b"});
}

TEST_CASE("workspace planar_home fails when neither variable is set") {
  REQUIRE_FALSE(id::planar_home(env_of({})).has_value());
  // And a tilde with no HOME to expand it against is a failure, not a literal.
  REQUIRE_FALSE(id::planar_home(env_of({{"PLANAR_HOME", "~/x"}})).has_value());
}

// ===========================================================================
// identity: layout
// ===========================================================================

TEST_CASE("workspace load_layout names the four canonical files") {
  const auto value = id::load_layout(env_of({{"PLANAR_HOME", "/ph"}}), 7);
  REQUIRE(value.has_value());
  REQUIRE(value->dir == std::filesystem::path{"/ph/workspaces/7"});
  REQUIRE(value->agents_md == std::filesystem::path{"/ph/workspaces/7/AGENTS.md"});
  REQUIRE(value->routing_table == std::filesystem::path{"/ph/workspaces/7/routing-table.json"});
  REQUIRE(value->config_toml == std::filesystem::path{"/ph/workspaces/7/config.toml"});
  REQUIRE(value->readme_md == std::filesystem::path{"/ph/workspaces/7/README.md"});
}

TEST_CASE("workspace load_layout refuses a non-positive org id") {
  REQUIRE_FALSE(id::load_layout(env_of({{"PLANAR_HOME", "/ph"}}), 0).has_value());
  REQUIRE_FALSE(id::load_layout(env_of({{"PLANAR_HOME", "/ph"}}), -1).has_value());
}

TEST_CASE("workspace load_layout does NOT create the directory but ensure_layout does") {
  const scratch_tree tree;
  REQUIRE(id::load_layout(tree.env(), 1).has_value());
  REQUIRE_FALSE(std::filesystem::exists(tree.state_dir(1)));
  REQUIRE(id::ensure_layout(tree.env(), 1).has_value());
  REQUIRE(std::filesystem::is_directory(tree.state_dir(1)));
}

// ===========================================================================
// identity: root_path parsing fails soft
// ===========================================================================

TEST_CASE("workspace parse_root_path reads a usable value") {
  REQUIRE(id::parse_root_path(R"({"root_path":"/tmp/ws/cwd"})") == "/tmp/ws/cwd");
}

TEST_CASE("workspace parse_root_path fails SOFT on everything unusable") {
  // None of these is an error: the row is still a usable workspace, it just has
  // no root to install guidance into.
  REQUIRE_FALSE(id::parse_root_path("").has_value());
  REQUIRE_FALSE(id::parse_root_path("not json").has_value());
  REQUIRE_FALSE(id::parse_root_path("[1,2,3]").has_value());
  REQUIRE_FALSE(id::parse_root_path(R"({"a":1})").has_value());
  REQUIRE_FALSE(id::parse_root_path(R"({"root_path":42})").has_value());
  REQUIRE_FALSE(id::parse_root_path(R"({"root_path":""})").has_value());
}

// ===========================================================================
// identity: org selection
// ===========================================================================

TEST_CASE("workspace resolve_org selects the only org when no target is given") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", R"({"root_path":"/tmp/x"})");

  const auto org = id::resolve_org(conn, std::nullopt);
  REQUIRE(org.has_value());
  REQUIRE(org->slug == "acme");
  REQUIRE(org->name == "Acme");
  REQUIRE(org->root_path == "/tmp/x");
}

TEST_CASE("workspace resolve_org refuses an empty database") {
  const scratch_tree tree;
  auto               conn     = open_migrated(tree);
  const auto         resolved = id::resolve_org(conn, std::nullopt);
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error() == id::resolve_error::not_found);
}

TEST_CASE("workspace resolve_org refuses when several orgs exist") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", std::nullopt);
  add_org(conn, "b", "B", std::nullopt);

  // REQUIRE_FALSE(has_value()) FIRST, then the error. Calling `.error()` on a
  // value-holding `std::expected` is undefined behaviour, and a mutant proved
  // that matters: deleting the ambiguity check entirely left the original
  // one-line form of this test PASSING, because it read the error channel of a
  // successful result and happened to find the right bit pattern. An assertion
  // that cannot fail is worse than no assertion.
  const auto resolved = id::resolve_org(conn, std::nullopt);
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error() == id::resolve_error::ambiguous);
}

TEST_CASE("workspace resolve_org accepts an id, a slug, and an org: prefix") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::nullopt);
  add_org(conn, "other", "Other", std::nullopt);

  REQUIRE(id::resolve_org(conn, "1")->slug == "acme");
  REQUIRE(id::resolve_org(conn, "acme")->slug == "acme");
  REQUIRE(id::resolve_org(conn, "org:1")->slug == "acme");
  REQUIRE(id::resolve_org(conn, "org:acme")->slug == "acme");
  // Whitespace is trimmed off the target before anything else looks at it.
  REQUIRE(id::resolve_org(conn, "  acme  ")->slug == "acme");
}

TEST_CASE("workspace resolve_org treats a WHITESPACE-ONLY target as absent") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", std::nullopt);
  add_org(conn, "b", "B", std::nullopt);
  // Two orgs and a blank target is AMBIGUOUS, not a failed slug lookup — the
  // trim happens before the empty check.
  const auto resolved = id::resolve_org(conn, "   ");
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error() == id::resolve_error::ambiguous);
}

TEST_CASE("workspace resolve_org reports an UNMATCHED slug as not_found") {
  // Which the CLI then renders as "no org associations registered" — the same
  // line an EMPTY database produces, even though an org exists. Oracle-verified
  // with `acme` present. The message is written for the empty case only, and
  // preserved rather than corrected.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::nullopt);
  const auto by_slug = id::resolve_org(conn, "nosuch");
  REQUIRE_FALSE(by_slug.has_value());
  REQUIRE(by_slug.error() == id::resolve_error::not_found);
  const auto by_id = id::resolve_org(conn, "999");
  REQUIRE_FALSE(by_id.has_value());
  REQUIRE(by_id.error() == id::resolve_error::not_found);
}

TEST_CASE("workspace list_orgs returns every org in id order and never refuses") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  REQUIRE(id::list_orgs(conn)->empty());
  add_org(conn, "zeta", "Z", std::nullopt);
  add_org(conn, "alpha", "A", std::nullopt);
  const auto orgs = id::list_orgs(conn);
  REQUIRE(orgs->size() == 2);
  // ID order, NOT slug order: `zeta` was inserted first and comes first.
  REQUIRE(orgs->at(0).slug == "zeta");
  REQUIRE(orgs->at(1).slug == "alpha");
}

// ===========================================================================
// identity: symlink install
// ===========================================================================

TEST_CASE("workspace install_symlinks installs BOTH names at the same target") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  std::ofstream(value.agents_md, std::ios::binary) << "guidance\n";

  REQUIRE(id::install_symlinks(tree.workspace_root(), value) == "symlink");
  for (const auto name : id::link_names) {
    const auto path = tree.workspace_root() / std::string{name};
    REQUIRE(std::filesystem::is_symlink(path));
    // CLAUDE.md has no separate content — both point at the one AGENTS.md.
    REQUIRE(std::filesystem::read_symlink(path) == value.agents_md);
  }
}

TEST_CASE("workspace install_symlinks creates the workspace root if absent") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  const auto         root  = tree.root_ / "brand-new-root";
  REQUIRE(id::install_symlinks(root, value) == "symlink");
  REQUIRE(std::filesystem::is_symlink(root / "AGENTS.md"));
}

TEST_CASE("workspace install_symlinks is idempotent and leaves a correct link alone") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  REQUIRE(id::install_symlinks(tree.workspace_root(), value) == "symlink");
  REQUIRE(id::install_symlinks(tree.workspace_root(), value) == "symlink");
  REQUIRE(std::filesystem::read_symlink(tree.workspace_root() / "AGENTS.md") == value.agents_md);
}

TEST_CASE("workspace install_symlinks REPOINTS a link aimed elsewhere") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  std::error_code    ec;
  std::filesystem::create_symlink("/somewhere/else", tree.workspace_root() / "AGENTS.md", ec);
  REQUIRE(id::install_symlinks(tree.workspace_root(), value) == "symlink");
  REQUIRE(std::filesystem::read_symlink(tree.workspace_root() / "AGENTS.md") == value.agents_md);
}

TEST_CASE("workspace install_symlinks leaves no .symlink.tmp behind") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  REQUIRE(id::install_symlinks(tree.workspace_root(), value).has_value());
  REQUIRE_FALSE(std::filesystem::exists(tree.workspace_root() / ".AGENTS.md.symlink.tmp"));
}

TEST_CASE("workspace remove_symlinks clears both names and tolerates absence") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);
  REQUIRE(id::install_symlinks(tree.workspace_root(), value).has_value());
  REQUIRE(id::remove_symlinks(tree.workspace_root()));
  REQUIRE_FALSE(std::filesystem::exists(tree.workspace_root() / "AGENTS.md"));
  // A second pass over an already-clean root is not an error.
  REQUIRE(id::remove_symlinks(tree.workspace_root()));
}

TEST_CASE("workspace dirty_links reports absent, repointed and hand-written names") {
  const scratch_tree tree;
  const auto         value = *id::ensure_layout(tree.env(), 1);

  // Both absent.
  REQUIRE(id::dirty_links(tree.workspace_root(), value.agents_md) == std::vector<std::string>{"AGENTS.md", "CLAUDE.md"});

  REQUIRE(id::install_symlinks(tree.workspace_root(), value).has_value());
  REQUIRE(id::dirty_links(tree.workspace_root(), value.agents_md).empty());

  // A link pointing SOMEWHERE ELSE is dirty. This case had no test until a
  // mutant deleted the target comparison and nothing failed — the other two
  // cases both go through the readlink-FAILED path, so between them they never
  // exercised the readlink-SUCCEEDED-but-wrong branch at all.
  std::error_code ec;
  std::filesystem::remove(tree.workspace_root() / "CLAUDE.md", ec);
  std::filesystem::create_symlink("/somewhere/else", tree.workspace_root() / "CLAUDE.md", ec);
  REQUIRE(id::dirty_links(tree.workspace_root(), value.agents_md) == std::vector<std::string>{"CLAUDE.md"});

  // A REAL FILE where a link belongs is dirty too: readlink fails, and the
  // collapsed branch (see identity.cppm) treats every readlink failure alike.
  std::filesystem::remove(tree.workspace_root() / "AGENTS.md", ec);
  std::ofstream(tree.workspace_root() / "AGENTS.md", std::ios::binary) << "hand written\n";
  REQUIRE(id::dirty_links(tree.workspace_root(), value.agents_md) == std::vector<std::string>{"AGENTS.md", "CLAUDE.md"});
}

// ===========================================================================
// doctor: shape classification
// ===========================================================================

TEST_CASE("workspace classify_shape names EVERY unreadable config distinctly") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", "not json");
  add_org(conn, "b", "B", "[1,2,3]");
  add_org(conn, "c", "C", R"({"a":1})");
  add_org(conn, "d", "D", R"({"root_path":42})");
  add_org(conn, "e", "E", R"({"root_path":"/r","workspace_shape":7})");
  add_org(conn, "f", "F", R"({"root_path":"/r","workspace_shape":"nonsense"})");
  add_org(conn, "g", "G", std::nullopt);
  add_org(conn, "h", "H", "");

  const auto reason = [&conn](std::int64_t org_id) { return dr::classify_shape(conn, org_id)->reason; };
  REQUIRE(reason(1) == "workspace config_json is malformed; skipping root guidance repair");
  REQUIRE(reason(2) == "workspace config_json is not an object; skipping root guidance repair");
  REQUIRE(reason(3) == "workspace config_json has no root_path; skipping root guidance repair");
  REQUIRE(reason(4) == "workspace config_json has invalid root_path; skipping root guidance repair");
  REQUIRE(reason(5) == "workspace config_json has invalid workspace_shape; skipping root guidance repair");
  REQUIRE(reason(6) == "workspace config_json has unknown workspace_shape; skipping root guidance repair");
  REQUIRE(reason(7) == "workspace config_json is missing; skipping root guidance repair");
  REQUIRE(reason(8) == "workspace config_json is missing; skipping root guidance repair");
  REQUIRE(reason(99) == "workspace config row missing; skipping root guidance repair");
}

TEST_CASE("workspace classify_shape treats an ABSENT workspace_shape as sibling") {
  // The polyrepo layout is the default and predates the key, so omitting it
  // must not stop root-guidance repair.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", R"({"root_path":"/r"})");
  REQUIRE(dr::classify_shape(conn, 1)->shape == dr::workspace_shape::non_meta);
  REQUIRE(dr::classify_shape(conn, 1)->reason.empty());
}

TEST_CASE("workspace classify_shape validates root_path BEFORE workspace_shape") {
  // A meta-repo with no root_path reports the root_path problem, not
  // `meta_repo` — which is what makes `non_meta` always imply a usable root.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", R"({"workspace_shape":"meta-repo"})");
  REQUIRE(dr::classify_shape(conn, 1)->shape == dr::workspace_shape::uncertain);
  REQUIRE(dr::classify_shape(conn, 1)->reason.starts_with("workspace config_json has no root_path"));
}

TEST_CASE("workspace classify_shape recognises both known shapes") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "a", "A", R"({"root_path":"/r","workspace_shape":"meta-repo"})");
  add_org(conn, "b", "B", R"({"root_path":"/r","workspace_shape":"sibling"})");
  REQUIRE(dr::classify_shape(conn, 1)->shape == dr::workspace_shape::meta_repo);
  REQUIRE(dr::classify_shape(conn, 2)->shape == dr::workspace_shape::non_meta);
}

// ===========================================================================
// doctor: the pass itself
// ===========================================================================

TEST_CASE("workspace doctor on an empty database reports nothing and never refuses") {
  // Its three sibling leaves refuse here. Doctor does not — it takes no
  // positional and reports on every org, of which there are none.
  const scratch_tree tree;
  auto               conn    = open_migrated(tree);
  const auto         reports = dr::run(conn, tree.env());
  REQUIRE(reports.has_value());
  REQUIRE(reports->empty());
}

TEST_CASE("workspace doctor creates the state dir and reports it FIRST") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));

  const auto reports = dr::run(conn, tree.env());
  REQUIRE(reports->size() == 1);
  const auto& report = reports->at(0);
  // created dir, two missing files, two reinstalled links — in this order.
  REQUIRE(kinds_of(report) == std::vector<std::string>{"fix", "missing", "missing", "fix", "fix"});
  REQUIRE(report.issues[0].detail == std::format("created state dir {}", tree.state_dir(1).string()));
  REQUIRE(std::filesystem::is_directory(tree.state_dir(1)));
}

TEST_CASE("workspace doctor's reinstalled-link detail is byte-exact") {
  // The `fix` details had no exactness assertion until a mutant swapped the
  // U+2192 arrow for ASCII `->` and every test still passed: the byte-exact
  // renderer tests build their reports by hand, so nothing pinned what
  // `diagnose()` actually writes into one.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));

  const auto  reports = dr::run(conn, tree.env());
  const auto& issues  = reports->at(0).issues;
  REQUIRE(issues.size() == 5);
  REQUIRE(issues[3].kind == "fix");
  REQUIRE(issues[3].detail == std::format("reinstalled symlink {}/AGENTS.md \u2192 {}", tree.workspace_root().string(),
                                          (tree.state_dir(1) / "AGENTS.md").string()));
  REQUIRE(issues[4].detail == std::format("reinstalled symlink {}/CLAUDE.md \u2192 {}", tree.workspace_root().string(),
                                          (tree.state_dir(1) / "AGENTS.md").string()));
  // Both names point at the ONE generated AGENTS.md; CLAUDE.md has no separate
  // target, and a detail naming `CLAUDE.md` on both sides would be wrong.
  REQUIRE(issues[4].detail.find("CLAUDE.md \u2192") != std::string::npos);
}

TEST_CASE("workspace doctor REPAIRS as a side effect of diagnosing") {
  // There is no --dry-run. Asking what is wrong installs the links.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));

  REQUIRE(dr::run(conn, tree.env()).has_value());
  REQUIRE(std::filesystem::is_symlink(tree.workspace_root() / "AGENTS.md"));
  REQUIRE(std::filesystem::is_symlink(tree.workspace_root() / "CLAUDE.md"));
}

TEST_CASE("workspace doctor writes to root_path, NOT to the current directory") {
  // THE correction this bucket exists around. The test changes the process's
  // working directory to an unrelated scratch subdirectory and asserts that
  // nothing lands there — proving cwd isolation is not what protects a real
  // machine from this verb.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));

  const auto      original = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::current_path(tree.root_ / "elsewhere", ec);
  REQUIRE_FALSE(ec);
  const auto reports = dr::run(conn, tree.env());
  std::filesystem::current_path(original, ec);

  REQUIRE(reports->size() == 1);
  REQUIRE(std::filesystem::is_symlink(tree.workspace_root() / "AGENTS.md"));
  REQUIRE_FALSE(std::filesystem::exists(tree.root_ / "elsewhere" / "AGENTS.md"));
  REQUIRE_FALSE(std::filesystem::exists(tree.root_ / "elsewhere" / "CLAUDE.md"));
}

TEST_CASE("workspace doctor leaves a healthy workspace with only the missing-file rows") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));

  REQUIRE(dr::run(conn, tree.env()).has_value()); // first pass installs
  const auto reports = dr::run(conn, tree.env()); // second pass finds them live
  REQUIRE(kinds_of(reports->at(0)) == std::vector<std::string>{"missing", "missing"});
  REQUIRE(reports->at(0).issues_found == 2);
}

TEST_CASE("workspace doctor's issues_found is a LENGTH, not a repair count") {
  // Two `missing` rows, nothing repaired, and the text still says
  // "repaired 2 issues". Preserved rather than corrected.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));
  REQUIRE(dr::run(conn, tree.env()).has_value());

  const auto reports = dr::run(conn, tree.env());
  REQUIRE(reports->at(0).issues_found == static_cast<std::int64_t>(reports->at(0).issues.size()));
  REQUIRE(dr::doctor_text(*reports).ends_with("org:acme repaired 2 issues\n"));
}

TEST_CASE("workspace doctor installs links pointing at a target it just called MISSING") {
  // The dangling-symlink window: one pass reports AGENTS.md missing AND creates
  // links to it. Recoverable — `regenerate` writes the target — but a real,
  // reachable state, so it is pinned rather than left implicit.
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", std::format(R"({{"root_path":"{}"}})", tree.workspace_root().string()));
  const auto reports = dr::run(conn, tree.env());

  REQUIRE(has_detail(reports->at(0), "AGENTS.md (run `planar workspace regenerate` after M3)"));
  const auto link = tree.workspace_root() / "AGENTS.md";
  REQUIRE(std::filesystem::is_symlink(link));
  // `exists` FOLLOWS the link, and the target is not there: it dangles.
  REQUIRE_FALSE(std::filesystem::exists(link));
}

TEST_CASE("workspace doctor skips root guidance entirely for a meta-repo") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme",
          std::format(R"({{"root_path":"{}","workspace_shape":"meta-repo"}})", tree.workspace_root().string()));

  const auto reports = dr::run(conn, tree.env());
  // The leading `fix` is the state directory, which doctor creates for EVERY
  // org including a meta repo — only the ROOT is left alone.
  REQUIRE(kinds_of(reports->at(0)) == std::vector<std::string>{"fix", "missing", "missing"});
  REQUIRE(reports->at(0).issues[0].detail.starts_with("created state dir "));
  // A meta repo owns its own root-level instruction files. Nothing installed.
  REQUIRE_FALSE(std::filesystem::exists(tree.workspace_root() / "AGENTS.md"));
  REQUIRE_FALSE(std::filesystem::exists(tree.workspace_root() / "CLAUDE.md"));
}

TEST_CASE("workspace doctor reports an unreadable config as an error and repairs nothing") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "acme", "Acme", "not json");

  const auto reports = dr::run(conn, tree.env());
  // Again the leading `fix` is the state directory: an unreadable config stops
  // ROOT repair, not the state-directory check that runs before it.
  REQUIRE(kinds_of(reports->at(0)) == std::vector<std::string>{"fix", "missing", "missing", "error"});
  REQUIRE(reports->at(0).issues[3].detail == "workspace config_json is malformed; skipping root guidance repair");
  REQUIRE_FALSE(std::filesystem::exists(tree.workspace_root() / "AGENTS.md"));
}

TEST_CASE("workspace doctor reports every org in id order") {
  const scratch_tree tree;
  auto               conn = open_migrated(tree);
  add_org(conn, "zeta", "Z", std::nullopt);
  add_org(conn, "alpha", "A", std::nullopt);
  const auto reports = dr::run(conn, tree.env());
  REQUIRE(reports->size() == 2);
  REQUIRE(reports->at(0).slug == "zeta");
  REQUIRE(reports->at(1).slug == "alpha");
}

// ===========================================================================
// renderers — byte exactness
// ===========================================================================

TEST_CASE("workspace doctor_json on an empty database is the oracle's bytes") {
  REQUIRE(dr::doctor_json({}) == "{\"orgs\":[]}\n");
}

TEST_CASE("workspace doctor_text on an empty database is ZERO BYTES") {
  // Where --json emits `{"orgs":[]}`. The two modes disagree, and both are
  // pinned — a text renderer that emitted a "nothing to do" sentence here
  // would be friendlier and a parity break.
  REQUIRE(dr::doctor_text({}).empty());
}

TEST_CASE("workspace doctor_json reproduces the oracle's populated envelope") {
  // Verbatim from the fixture in this file's header, with the scratch paths
  // substituted for /tmp/ws. The wire key is `issues_repaired`, not `issues`.
  const std::vector<dr::org_report> reports{
      {"acme",
       1,
       4,
       {{"missing", "/PH/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)"},
        {"missing", "/PH/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)"},
        {"fix", "reinstalled symlink /WS/AGENTS.md → /PH/workspaces/1/AGENTS.md"},
        {"fix", "reinstalled symlink /WS/CLAUDE.md → /PH/workspaces/1/AGENTS.md"}}}};

  REQUIRE(dr::doctor_json(reports) ==
          "{\"orgs\":[{\"slug\":\"acme\",\"org_id\":1,\"issues_found\":4,\"issues_repaired\":["
          "{\"kind\":\"missing\",\"detail\":\"/PH/workspaces/1/AGENTS.md (run `planar workspace "
          "regenerate` after M3)\"},"
          "{\"kind\":\"missing\",\"detail\":\"/PH/workspaces/1/routing-table.json (run `planar workspace "
          "regenerate` after M3)\"},"
          "{\"kind\":\"fix\",\"detail\":\"reinstalled symlink /WS/AGENTS.md → "
          "/PH/workspaces/1/AGENTS.md\"},"
          "{\"kind\":\"fix\",\"detail\":\"reinstalled symlink /WS/CLAUDE.md → "
          "/PH/workspaces/1/AGENTS.md\"}]}]}\n");
}

TEST_CASE("workspace doctor_text reproduces the oracle's populated bytes") {
  const std::vector<dr::org_report> reports{
      {"acme",
       1,
       2,
       {{"missing", "/PH/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)"},
        {"missing", "/PH/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)"}}}};
  REQUIRE(dr::doctor_text(reports) ==
          "missing: /PH/workspaces/1/AGENTS.md (run `planar workspace regenerate` after M3)\n"
          "missing: /PH/workspaces/1/routing-table.json (run `planar workspace regenerate` after M3)\n"
          "org:acme repaired 2 issues\n");
}

TEST_CASE("workspace doctor_text says ok only when nothing was found") {
  REQUIRE(dr::doctor_text(std::vector<dr::org_report>{{"acme", 1, 0, {}}}) == "org:acme ok\n");
}

TEST_CASE("workspace doctor_json escapes a detail's quotes and backslashes") {
  // Details carry filesystem paths, which on some platforms carry backslashes.
  const std::vector<dr::org_report> reports{{"a", 1, 1, {{"error", "C:\\ws\\\"x\""}}}};
  REQUIRE(dr::doctor_json(reports).find("\"detail\":\"C:\\\\ws\\\\\\\"x\\\"\"") != std::string::npos);
}

TEST_CASE("workspace the three refusing leaves share ONE message each") {
  // Byte-identical across `routing show` / `routing build` / `regenerate`, and
  // identical in text and --json modes (there is no JSON error envelope
  // anywhere in this verb family). `doctor` emits neither.
  REQUIRE(dr::no_orgs_error() == "error: no org associations registered; create one with `planar workspace init`\n");
  REQUIRE(dr::ambiguous_orgs_error() ==
          "error: multiple org associations registered; pass the workspace slug or id explicitly\n");
}
