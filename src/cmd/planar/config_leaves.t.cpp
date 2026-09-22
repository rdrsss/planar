// @file config_leaves.t.cpp
// @brief In-process tests for the five `planar config` leaves wired by
// plan 996, task 6259.
//
// ## HOME SAFETY, AND WHY IT IS SHARPER HERE THAN ANYWHERE ELSE
//
// This family WRITES the operator's `~/.planar/config.toml` (`init` and
// `edit` both create it) and READS it in the other three. Every fixture
// builds an explicit environment map with `HOME` and `PLANAR_CONFIG_PATH`
// under its own uniquely-named scratch root and dispatches through
// `map_env`, so nothing here reads the process environment.
//
// `PLANAR_HOME` is deliberately ABSENT from every fixture, and its absence
// is load-bearing rather than tidy: this family never consults it — the
// path is `$PLANAR_CONFIG_PATH` else `$HOME/.planar/config.toml` —
// so leaving it unset proves these tests cannot be passing for the wrong
// reason. Oracle-confirmed in a pinned arena with the two pointing at
// different directories: `config path` printed the one under `HOME`.
//
// ## NONE OF THE FIVE MAY OPEN SQLITE
//
// `ctx.db_opened()` is asserted FALSE after every case. That is a real
// contract, not a property of the current implementation: `config init`
// and `config path` are what an operator runs BEFORE `planar init`, and
// the runtime applies pending migrations on first use — so a `config path`
// that opened the database could migrate the operator's live one past what
// every installed binary supports.
//
// ## EVERY EXPECTATION HERE WAS CAPTURED FROM THE BUILT ORACLE
//
// `zig/zig-out/bin/planar` in a pinned scratch arena, both streams always
// through pipes (the Zig writer's positional writes silently eat an
// earlier write through a shared capture file). Nothing below is derived
// by reading the C++ implementation, and the two starter blobs in
// particular are asserted by their ORACLE-MEASURED BYTE COUNTS (1267 and
// 1048) and by content the two do not share — never against the
// `k_starter_*` constants they came from, which would make the test pass
// for any pair of values at all.
//
// ## THE DIVERGENCE SECTION AT THE BOTTOM IS THE POINT OF THIS FILE
//
// `config validate` is byte-identical to the oracle on every semantic path
// (27 differential cases). It is NOT byte-identical on TOML DIAGNOSTICS,
// and cannot be, because the two trees do not share a TOML parser: Glaze
// owns it here (D12) and `zig/src/engine/config/parse.zig` is a hand-rolled
// subset there. Four classes of difference were MEASURED, each is pinned
// below in a test that names it and carries the oracle's exact bytes in its
// comment, and each is a deliberate NAMED DIVERGENCE rather than a chase.
// See handlers/config.cppm for the reasoning.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

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
  std::filesystem::path                           cfg;     ///< The config file this fixture resolves to.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< Never created — no leaf here opens it.
};

/// @brief Build a fixture under a unique scratch directory.
///
/// `HOME` and `PLANAR_CONFIG_PATH` are both set, and both point inside the
/// scratch root. `PLANAR_HOME` is deliberately absent — see this file's
/// header.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_cfgleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  std::filesystem::create_directories(root / "fakehome" / ".planar", ec);
  auto const cfg = root / "fakehome" / ".planar" / "config.toml";
  return fixture{
      .root = root,
      .cfg  = cfg,
      .vars = {{"HOME", (root / "fakehome").string()}, {"PLANAR_CONFIG_PATH", cfg.string()}, {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Remove a fixture's scratch tree.
/// @param fx The fixture.
auto cleanup(const fixture& fx) -> void {
  std::error_code ec;
  std::filesystem::remove_all(fx.root, ec);
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
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

/// @brief Write `fx`'s config file.
///
/// REQUIREs the write and REQUIREs the result is non-empty when `body` is,
/// so a case can never go green against a fixture that silently wrote
/// nothing — the vacuous-pass shape task 6256 closed.
/// @param fx The fixture.
/// @param body The TOML to write.
auto write_config(const fixture& fx, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(fx.cfg.parent_path(), ec);
  {
    std::ofstream file(fx.cfg, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << body;
  }
  REQUIRE(std::filesystem::exists(fx.cfg));
  REQUIRE(std::filesystem::file_size(fx.cfg) == body.size());
}

/// @brief Read a file whole.
/// @param path The file.
/// @return Its bytes, empty when absent.
auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @brief The lines of `text`, without their terminators.
/// @param text The text.
/// @return One entry per line; a trailing newline yields no empty tail.
auto lines_of(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> out;
  while (!text.empty()) {
    auto const newline = text.find('\n');
    if (newline == std::string_view::npos) {
      out.emplace_back(text);
      break;
    }
    out.emplace_back(text.substr(0, newline));
    text = text.substr(newline + 1);
  }
  return out;
}

/// @brief The one line of `text` that starts with `key + " ="`.
///
/// REQUIREs exactly one match, so a case asserting on a key that stopped
/// being emitted fails LOUDLY instead of comparing two empty strings.
/// @param text The listing.
/// @param key The dotted key.
/// @return The whole line.
auto line_for(std::string_view text, std::string_view key) -> std::string {
  auto const               prefix = std::format("{} =", key);
  std::vector<std::string> hits;
  for (auto const& line : lines_of(text)) {
    if (line.starts_with(prefix)) {
      hits.push_back(line);
    }
  }
  INFO("looking for key '" << key << "' in:\n" << text);
  REQUIRE(hits.size() == 1);
  return hits.front();
}

/// @brief The one JSON line of `text` whose `"key"` is `key`.
/// @param text The NDJSON listing.
/// @param key The dotted key.
/// @return The whole line.
auto json_line_for(std::string_view text, std::string_view key) -> std::string {
  auto const               needle = std::format("{{\"key\":\"{}\",", key);
  std::vector<std::string> hits;
  for (auto const& line : lines_of(text)) {
    if (line.starts_with(needle)) {
      hits.push_back(line);
    }
  }
  INFO("looking for key '" << key << "' in:\n" << text);
  REQUIRE(hits.size() == 1);
  return hits.front();
}

/// @brief A config exercising every shape `config show` renders: a file
/// value, an array-valued model tier (the only `candidates` producer), a
/// scalar tier, routing keys both valid and orphaned, a user-defined role
/// whose NAME is sensitive, and a per-association Jira status override.
///
/// One fixture rather than six, because the ORDERING and the interaction
/// between them is part of what is being pinned.
///
/// NOT VALIDATE-CLEAN, deliberately: `roles.my_token` exists so the
/// masking rule has something to reach, and the same name trips `config
/// validate`'s lexical sensitive-literal scan. Anything asserting a clean
/// validate must use its own document — see "config validate accepts a
/// clean file at exit 0".
constexpr std::string_view k_rich_config = R"TOML([defaults]
vendor = "codex"

[models.claude]
medium = ["claude-sonnet-5", "claude-fable-5"]
large = "claude-opus-5"

[routing.claude.medium]
cli = "claude-fable-5"
feature = "claude-sonnet-5"

[routing.codex.medium]
cli = "gpt-nope"

[roles]
coder = "medium"
my_token = "large"

[external.jira]
base_url = "https://x.atlassian.net"

[associations."org:acme".external.jira.status]
done = "Closed"
)TOML";

} // namespace

// --- config path ------------------------------------------------------

TEST_CASE("config path prints the resolved path and opens no database", "[cmd][config][path]") {
  auto const fx = make_fixture("path");
  auto const r  = dispatch(fx, {"config", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == fx.cfg.string() + "\n");
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

TEST_CASE("config path does NOT require the file to exist", "[cmd][config][path]") {
  // An absent config still has a path, and printing it is how an operator
  // finds out where to create one. Oracle-captured: exit 0 with the path,
  // on a HOME containing no `.planar` directory at all.
  auto fx = make_fixture("pathabsent");
  fx.vars.erase("PLANAR_CONFIG_PATH");
  REQUIRE_FALSE(std::filesystem::exists(fx.cfg));
  auto const r = dispatch(fx, {"config", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == fx.cfg.string() + "\n");
  cleanup(fx);
}

TEST_CASE("config path expands a leading tilde in PLANAR_CONFIG_PATH", "[cmd][config][path]") {
  auto fx = make_fixture("pathtilde");
  fx.vars.insert_or_assign("PLANAR_CONFIG_PATH", "~/elsewhere.toml");
  auto const r = dispatch(fx, {"config", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == (fx.root / "fakehome" / "elsewhere.toml").string() + "\n");
  cleanup(fx);
}

TEST_CASE("config path treats an EMPTY PLANAR_CONFIG_PATH as unset", "[cmd][config][path]") {
  // The empty-value hazard, probed directly rather than assumed: set-but-
  // empty falls through to `$HOME/.planar/config.toml`, it does not resolve
  // to the empty path. Oracle-captured.
  auto fx = make_fixture("pathempty");
  fx.vars.insert_or_assign("PLANAR_CONFIG_PATH", "");
  auto const r = dispatch(fx, {"config", "path"});
  CHECK(r.code == 0);
  CHECK(r.out == (fx.root / "fakehome" / ".planar" / "config.toml").string() + "\n");
  cleanup(fx);
}

TEST_CASE("config path refuses at exit 1 when neither variable yields a path", "[cmd][config][path]") {
  auto fx = make_fixture("pathnohome");
  fx.vars.erase("HOME");
  fx.vars.erase("PLANAR_CONFIG_PATH");
  auto const r = dispatch(fx, {"config", "path"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: resolving config path: HomeNotSet\n");
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

// --- config init ------------------------------------------------------

TEST_CASE("config init creates the starter file and reports the path", "[cmd][config][init]") {
  auto const fx = make_fixture("init");
  REQUIRE_FALSE(std::filesystem::exists(fx.cfg));
  auto const r = dispatch(fx, {"config", "init"});
  CHECK(r.code == 0);
  CHECK(r.out == "config init: created " + fx.cfg.string() + "\n");
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  REQUIRE(std::filesystem::exists(fx.cfg));
  cleanup(fx);
}

TEST_CASE("config init is idempotent and reports already-exists at exit 0", "[cmd][config][init]") {
  // NOT an error, which is the whole `D-init-idempotent` shape: the Go
  // original returns `(false, nil)` and Zig mirrors it. A port that made
  // this exit 1 would break `planar init`, which calls the same path.
  auto const fx = make_fixture("initagain");
  write_config(fx, "vendor = \"kept\"\n");
  auto const r = dispatch(fx, {"config", "init"});
  CHECK(r.code == 0);
  CHECK(r.out == "config init: already exists " + fx.cfg.string() + "\n");
  CHECK(r.err.empty());
  // The existing file is NOT overwritten — asserted, because "already
  // exists" printed while the file was clobbered would still pass on
  // stdout alone.
  CHECK(read_file(fx.cfg) == "vendor = \"kept\"\n");
  cleanup(fx);
}

TEST_CASE("config init creates missing parent directories", "[cmd][config][init]") {
  auto fx = make_fixture("initdeep");
  fx.cfg  = fx.root / "a" / "b" / "c.toml";
  fx.vars.insert_or_assign("PLANAR_CONFIG_PATH", fx.cfg.string());
  REQUIRE_FALSE(std::filesystem::exists(fx.root / "a"));
  auto const r = dispatch(fx, {"config", "init"});
  CHECK(r.code == 0);
  CHECK(r.out == "config init: created " + fx.cfg.string() + "\n");
  CHECK(std::filesystem::exists(fx.cfg));
  cleanup(fx);
}

TEST_CASE("config init writes the LONGER of the two starter blobs", "[cmd][config][init][oracle-defect]") {
  // ORACLE-MEASURED, not read off the constant: `config init` writes 1267
  // bytes and `config edit` writes 1048, and the 219-byte difference is an
  // eight-line `[models.codex]` / `[roles]` example block that plan 540
  // added to `init.zig`'s copy and never back-ported to `edit.zig`'s —
  // whose comment still claims it "mirrors init.zig's starter_config
  // exactly". Reproduced rather than unified (D2): merging them would
  // change what one of the two verbs writes to the operator's disk.
  auto const fx = make_fixture("initblob");
  REQUIRE(dispatch(fx, {"config", "init"}).code == 0);
  auto const body = read_file(fx.cfg);
  REQUIRE_FALSE(body.empty());
  CHECK(body.size() == 1267);
  CHECK(body.starts_with("# ~/.planar/config.toml"));
  CHECK(body.ends_with("# done = \"Closed\"\n"));
  CHECK(body.contains("# [models.codex]"));
  CHECK(body.contains("# [roles]"));
  // The file it writes must itself validate cleanly — a starter that the
  // very next `config validate` rejects would be a poor welcome.
  auto const validated = dispatch(fx, {"config", "validate"});
  CHECK(validated.code == 0);
  CHECK(validated.out == "config validate: ok\n");
  cleanup(fx);
}

// --- config edit ------------------------------------------------------

TEST_CASE("config edit execs the editor on the config file itself", "[cmd][config][edit]") {
  // ON THE FILE, never a temp copy — so there is no write-back step and an
  // editor crash cannot lose the operator's config. The stub records its
  // argv so the path is asserted rather than assumed.
  auto const fx     = make_fixture("edit");
  auto const marker = fx.root / "editor-argv.txt";
  auto const stub   = fx.root / "stub-editor.sh";
  {
    std::ofstream file(stub);
    REQUIRE(file.is_open());
    file << "#!/bin/sh\nprintf '%s' \"$1\" > " << marker.string() << "\nexit 0\n";
  }
  std::filesystem::permissions(stub, std::filesystem::perms::owner_all);
  auto vars = fx.vars;
  vars.emplace("PLANAR_EDITOR", stub.string());
  fixture with_editor{.root = fx.root, .cfg = fx.cfg, .vars = std::move(vars), .db_path = fx.db_path};

  auto const r = dispatch(with_editor, {"config", "edit"});
  CHECK(r.code == 0);
  CHECK(r.out.empty());
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  CHECK(read_file(marker) == fx.cfg.string());
  cleanup(fx);
}

TEST_CASE("config edit writes the SHORTER starter blob when the file is absent", "[cmd][config][edit][oracle-defect]") {
  // 1048 bytes, oracle-measured, and WITHOUT the `[models.codex]` /
  // `[roles]` block `config init`'s copy carries. Both halves are asserted:
  // the size, and the absence of the block — a size check alone would pass
  // against any 1048-byte file.
  auto fx = make_fixture("editblob");
  fx.vars.emplace("PLANAR_EDITOR", "/usr/bin/true");
  REQUIRE_FALSE(std::filesystem::exists(fx.cfg));
  auto const r = dispatch(fx, {"config", "edit"});
  CHECK(r.code == 0);
  auto const body = read_file(fx.cfg);
  REQUIRE_FALSE(body.empty());
  CHECK(body.size() == 1048);
  CHECK(body.starts_with("# ~/.planar/config.toml"));
  CHECK(body.ends_with("# done = \"Closed\"\n"));
  CHECK_FALSE(body.contains("# [models.codex]"));
  CHECK_FALSE(body.contains("# [roles]"));
  cleanup(fx);
}

TEST_CASE("config edit leaves an existing config file untouched before editing", "[cmd][config][edit]") {
  auto fx = make_fixture("editkeep");
  fx.vars.emplace("PLANAR_EDITOR", "/usr/bin/true");
  write_config(fx, "[defaults]\nvendor = \"mine\"\n");
  CHECK(dispatch(fx, {"config", "edit"}).code == 0);
  CHECK(read_file(fx.cfg) == "[defaults]\nvendor = \"mine\"\n");
  cleanup(fx);
}

TEST_CASE("config edit reports a non-zero editor exit as a failure", "[cmd][config][edit]") {
  // The OPPOSITE of `planar.cmd.planar.editor`'s `invoke` contract, which
  // treats a non-zero exit as "the caller decides". Here the oracle exits
  // 1 and says so, and that is what an operator scripting `config edit`
  // depends on.
  auto fx = make_fixture("editfail");
  fx.vars.emplace("PLANAR_EDITOR", "/usr/bin/false");
  auto const r = dispatch(fx, {"config", "edit"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: editor exited with code 1\n");
  cleanup(fx);
}

TEST_CASE("config edit reports an unresolvable editor as a spawn failure", "[cmd][config][edit]") {
  auto fx = make_fixture("editmissing");
  fx.vars.emplace("PLANAR_EDITOR", "planar-no-such-editor-exists");
  auto const r = dispatch(fx, {"config", "edit"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: spawning editor: FileNotFound\n");
  cleanup(fx);
}

// --- config show ------------------------------------------------------

TEST_CASE("config show lists every resolved key with no provenance", "[cmd][config][show]") {
  auto const fx = make_fixture("show");
  auto const r  = dispatch(fx, {"config", "show"});
  CHECK(r.code == 0);
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  // A file-free HOME still lists the embedded defaults — 23 keys, and the
  // count is asserted so a resolver that silently stopped emitting a family
  // cannot pass on the two keys the case happens to name. (22 until plan 1033
  // task 6485 added `execute.engine`, which `planar-execute` reads from this
  // very listing.)
  CHECK(lines_of(r.out).size() == 23);
  CHECK(line_for(r.out, "defaults.vendor") == "defaults.vendor = claude");
  CHECK(line_for(r.out, "execute.engine") == "execute.engine = embedded");
  CHECK(line_for(r.out, "workbench.root") == "workbench.root = ~/.planar/workbench");
  cleanup(fx);
}

TEST_CASE("config show renders provenance under --effective", "[cmd][config][show][effective]") {
  auto const fx = make_fixture("showeff");
  write_config(fx, k_rich_config);
  auto const r = dispatch(fx, {"config", "show", "--effective"});
  CHECK(r.code == 0);
  CHECK_FALSE(r.db_open);
  CHECK(line_for(r.out, "defaults.vendor") == "defaults.vendor = codex  [config file]");
  CHECK(line_for(r.out, "defaults.scope") == "defaults.scope = global  [embedded default]");
  // An array-valued tier renders the ordered candidate list; the tier
  // DEFAULT stays `candidates[0]`.
  CHECK(line_for(r.out, "models.claude.medium") ==
        "models.claude.medium = claude-sonnet-5  [config file] (candidates: claude-sonnet-5, claude-fable-5)");
  // A SCALAR tier is a one-element list and must render with NO candidates
  // clause — the difference is `candidates.size() > 1`, not `> 0`.
  CHECK(line_for(r.out, "models.claude.large") == "models.claude.large = claude-opus-5  [config file]");
  cleanup(fx);
}

TEST_CASE("config show reports the environment variable that supplied a value", "[cmd][config][show][effective]") {
  auto fx = make_fixture("showenv");
  fx.vars.emplace("PLANAR_VENDOR", "gemini");
  write_config(fx, k_rich_config);
  auto const r = dispatch(fx, {"config", "show", "--effective"});
  CHECK(r.code == 0);
  // The env layer beats the config file's `codex`, and the label NAMES the
  // variable — which is the whole reason this handler hands the resolver a
  // view over the real environment rather than a hard-coded variable list.
  CHECK(line_for(r.out, "defaults.vendor") == "defaults.vendor = gemini  [env: PLANAR_VENDOR]");
  cleanup(fx);
}

TEST_CASE("config show applies a per-association override only under a matching --scope", "[cmd][config][show][effective]") {
  auto const fx = make_fixture("showscope");
  write_config(fx, k_rich_config);

  auto const none = dispatch(fx, {"config", "show", "--effective"});
  CHECK(line_for(none.out, "external.jira.status.done") == "external.jira.status.done = Done  [embedded default]");

  auto const matched = dispatch(fx, {"config", "show", "--effective", "--scope", "org:acme"});
  CHECK(line_for(matched.out, "external.jira.status.done") == "external.jira.status.done = Closed  [per-association override]");

  // Both halves of the filter: an unknown slug falls back rather than
  // matching anything.
  auto const unknown = dispatch(fx, {"config", "show", "--effective", "--scope", "org:nope"});
  CHECK(line_for(unknown.out, "external.jira.status.done") == "external.jira.status.done = Done  [embedded default]");
  cleanup(fx);
}

TEST_CASE("config show treats an EMPTY --scope as no scope at all", "[cmd][config][show][effective]") {
  // The empty-value hazard again, and it reaches the code under test here:
  // `--scope ""` is a single string the handler inspects, not a
  // comma-split list that would yield zero tokens and never arrive.
  //
  // THE FIXTURE CARRIES AN `[associations.""]` OVERRIDE ON PURPOSE, and
  // that is what makes this case discriminating rather than decorative. A
  // handler that passed the empty string straight through as a real slug
  // would build the lookup key `associations..external.jira.status.done`,
  // which this document DOES define — so it would resolve to
  // `EmptySlugWins`. Against `k_rich_config`, which has no such section,
  // both the correct and the broken handler fall back to the embedded
  // default and the case passes either way: a break-probe flipping the
  // emptiness test to `false` SURVIVED the earlier version of this test.
  //
  // Oracle-confirmed against exactly this document: `--scope ""` prints the
  // embedded default, not `EmptySlugWins`.
  auto const fx = make_fixture("showscopeempty");
  write_config(fx, R"TOML([associations.""]
placeholder = "x"

[associations."".external.jira.status]
done = "EmptySlugWins"

[associations."org:acme".external.jira.status]
done = "Closed"
)TOML");
  auto const empty_scope = dispatch(fx, {"config", "show", "--effective", "--scope", ""});
  CHECK(empty_scope.code == 0);
  CHECK(line_for(empty_scope.out, "external.jira.status.done") == "external.jira.status.done = Done  [embedded default]");
  // ...and an omitted `--scope` behaves the same way, so the case is not
  // silently asserting that overrides never apply at all.
  CHECK(line_for(dispatch(fx, {"config", "show", "--effective"}).out, "external.jira.status.done") ==
        "external.jira.status.done = Done  [embedded default]");
  // The proof that the fixture's overrides ARE reachable: a real slug picks
  // one up. Without this the two assertions above would pass against a
  // resolver that had stopped applying association overrides entirely.
  CHECK(line_for(dispatch(fx, {"config", "show", "--effective", "--scope", "org:acme"}).out, "external.jira.status.done") ==
        "external.jira.status.done = Closed  [per-association override]");
  cleanup(fx);
}

TEST_CASE("config show masks a sensitively-named key as three asterisks", "[cmd][config][show]") {
  // The rule runs against the FULL DOTTED KEY, and it is reachable only
  // through a user-defined role — the one key family whose name comes from
  // the operator. `roles.my_token` ends in `_token` and masks;
  // `external.jira.token_env`, the key one would reach for first, does NOT
  // (it ends in `_env`). Both are asserted, because a test that only tried
  // the built-in keys would conclude masking was dead code.
  auto const fx = make_fixture("showmask");
  write_config(fx, k_rich_config);
  auto const eff = dispatch(fx, {"config", "show", "--effective"});
  CHECK(line_for(eff.out, "roles.my_token") == "roles.my_token = ***  [config file]");
  CHECK(line_for(eff.out, "roles.coder") == "roles.coder = medium  [config file]");
  CHECK(line_for(eff.out, "external.jira.token_env") == "external.jira.token_env = JIRA_TOKEN  [embedded default]");
  // Masking is NOT an --effective-only concern.
  auto const plain = dispatch(fx, {"config", "show"});
  CHECK(line_for(plain.out, "roles.my_token") == "roles.my_token = ***");
  cleanup(fx);
}

TEST_CASE("config show --json emits one object per line, with provenance implied", "[cmd][config][show][json]") {
  // `--json` alone implies `--effective`: there is no un-provenanced JSON
  // shape. Pinned because the two flags LOOK independent in the tree.
  auto const fx = make_fixture("showjson");
  write_config(fx, k_rich_config);
  auto const r = dispatch(fx, {"config", "show", "--json"});
  CHECK(r.code == 0);
  CHECK(json_line_for(r.out, "defaults.vendor") == R"({"key":"defaults.vendor","value":"codex","provenance":"config file"})");
  CHECK(json_line_for(r.out, "models.claude.medium") ==
        R"({"key":"models.claude.medium","value":"claude-sonnet-5","provenance":"config file",)"
        R"("candidates":["claude-sonnet-5","claude-fable-5"]})");
  // A scalar tier omits the `candidates` member entirely rather than
  // emitting a one-element array.
  CHECK(json_line_for(r.out, "models.claude.large") ==
        R"({"key":"models.claude.large","value":"claude-opus-5","provenance":"config file"})");
  CHECK(json_line_for(r.out, "roles.my_token") == R"({"key":"roles.my_token","value":"***","provenance":"config file"})");
  cleanup(fx);
}

TEST_CASE("config show IGNORES --format entirely", "[cmd][config][show][oracle-defect]") {
  // `--format` is declared with a `"text"` default and the Zig handler
  // never reads it: `--format json` prints the PLAIN TEXT listing. Only
  // `--json` switches the encoding. Oracle-captured and reproduced — an
  // operator whose pipeline passes `--format json` is parsing text today,
  // and "fixing" it would silently break them.
  auto const fx = make_fixture("showformat");
  write_config(fx, k_rich_config);
  auto const plain     = dispatch(fx, {"config", "show"});
  auto const formatted = dispatch(fx, {"config", "show", "--format", "json"});
  REQUIRE_FALSE(plain.out.empty());
  CHECK(formatted.out == plain.out);
  CHECK_FALSE(formatted.out.starts_with("{"));
  cleanup(fx);
}

TEST_CASE("config show --raw prints the operator's own bytes verbatim", "[cmd][config][show][raw]") {
  auto const fx = make_fixture("showraw");
  // Deliberately NOT valid against the resolver's schema and deliberately
  // unsorted: `--raw` is a passthrough, so neither should matter.
  constexpr std::string_view body = "# hand written\n[zzz]\nqqq = 1\n";
  write_config(fx, body);
  auto const r = dispatch(fx, {"config", "show", "--raw"});
  CHECK(r.code == 0);
  CHECK(r.out == body);
  CHECK(r.err.empty());
  cleanup(fx);
}

TEST_CASE("config show --raw prints ZERO BYTES when the file is absent", "[cmd][config][show][raw]") {
  // Not an error, and NOT the defaults — the two plausible wrong answers.
  auto const fx = make_fixture("showrawabsent");
  REQUIRE_FALSE(std::filesystem::exists(fx.cfg));
  auto const r = dispatch(fx, {"config", "show", "--raw"});
  CHECK(r.code == 0);
  CHECK(r.out.empty());
  CHECK(r.err.empty());
  cleanup(fx);
}

TEST_CASE("config show --defaults prints the embedded file and beats every other flag", "[cmd][config][show][defaults]") {
  auto const fx = make_fixture("showdefaults");
  write_config(fx, k_rich_config);
  auto const r = dispatch(fx, {"config", "show", "--defaults"});
  CHECK(r.code == 0);
  REQUIRE_FALSE(r.out.empty());
  // Oracle-measured byte count (3057) plus the 309-byte `[execute]` block
  // plan 1033 task 6485 added, and the content markers that prove it is the
  // DEFAULTS file rather than the operator's (which is what `--raw` would
  // have printed).
  CHECK(r.out.size() == 3366);
  CHECK(r.out.contains("[execute]\n"));
  CHECK(r.out.starts_with("# ~/.planar/config.toml"));
  CHECK_FALSE(r.out.contains("claude-fable-5"));
  // `--defaults` returns before anything else is consulted.
  CHECK(dispatch(fx, {"config", "show", "--defaults", "--raw", "--json", "--effective"}).out == r.out);
  cleanup(fx);
}

TEST_CASE("config show --defaults works with no HOME at all", "[cmd][config][show][defaults]") {
  // It returns BEFORE the config path is resolved, so the one flag that
  // needs no filesystem must not fail on a machine that has no `$HOME`.
  auto fx = make_fixture("showdefaultsnohome");
  fx.vars.erase("HOME");
  fx.vars.erase("PLANAR_CONFIG_PATH");
  auto const r = dispatch(fx, {"config", "show", "--defaults"});
  CHECK(r.code == 0);
  CHECK(r.out.size() == 3366);
  cleanup(fx);
}

TEST_CASE("config show refuses at exit 1 when the config file does not parse", "[cmd][config][show]") {
  auto const fx = make_fixture("showbroken");
  write_config(fx, "[defaults\nvendor = \"a\"\n");
  auto const r = dispatch(fx, {"config", "show"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: resolving configuration: ParseFailed\n");
  // ...but `--raw` still works, which is the point of having it: it is how
  // an operator SEES the file the resolver just rejected.
  auto const raw = dispatch(fx, {"config", "show", "--raw"});
  CHECK(raw.code == 0);
  CHECK(raw.out == "[defaults\nvendor = \"a\"\n");
  cleanup(fx);
}

// --- config validate --------------------------------------------------

TEST_CASE("config validate accepts a clean file at exit 0", "[cmd][config][validate]") {
  // `k_rich_config` deliberately is NOT used here: its `roles.my_token`
  // entry trips the sensitive-literal scan (see the false-positive case
  // below), so a clean-file assertion built on it would be asserting the
  // wrong thing. That is not hypothetical — this case was written against
  // `k_rich_config` first and failed, which is how the false positive was
  // found.
  auto const fx = make_fixture("valok");
  write_config(fx, R"TOML([defaults]
vendor = "codex"

[models.claude]
medium = ["claude-sonnet-5", "claude-fable-5"]

[routing.claude.medium]
cli = "claude-fable-5"

[roles]
coder = "medium"

[external.jira]
base_url = "https://x.atlassian.net"
)TOML");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 0);
  CHECK(r.out == "config validate: ok\n");
  CHECK(r.err.empty());
  CHECK_FALSE(r.db_open);
  cleanup(fx);
}

TEST_CASE("config validate no longer false-positives on a role whose name ends in _token", "[cmd][config][validate][6265]") {
  // Was: "config validate FALSE-POSITIVES on a role whose name ends in
  // _token" [oracle-defect], reproduced under D2. Decision 1118 (task
  // 6265) fixed the C++ tree only: the scan is now scoped to `external.*`,
  // the one table family that actually holds credentials and the only
  // place its own `*_env` remedy could ever apply. `[roles] my_token =
  // "large"` — where the value is a TIER NAME and could not be a secret —
  // now validates clean. `config show` still masks the same key to `***`,
  // which is the same rule applied where it is harmless, and is unchanged.
  auto const fx = make_fixture("valnofalsepos");
  write_config(fx, "[roles]\ncoder = \"medium\"\nmy_token = \"large\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 0);
  CHECK(r.out == "config validate: ok\n");
  CHECK(r.err.empty());
  cleanup(fx);
}

TEST_CASE("config validate still refuses a literal external credential", "[cmd][config][validate][6265]") {
  // The non-vacuity arm: scoping the scan to external.* must not have also
  // widened it into a no-op. A literal value on the exact key the scan
  // exists to catch is still refused, unchanged.
  auto const fx = make_fixture("valexternalstill");
  write_config(fx, "[external.jira]\ntoken = \"literal-secret-value\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: line 2: token: sensitive key must not carry a literal value in the config file "
                 "(use *_env convention instead)\n");
  cleanup(fx);
}

TEST_CASE("config validate scopes by the nearest table header, not merely by name anywhere in the file",
          "[cmd][config][validate][6265]") {
  // A role AND a genuine external credential in the same file: the role's
  // literal must be ignored and the credential's must still be caught,
  // proving the scope is per-table-context rather than a whole-file
  // on/off switch keyed by whether "external" appears anywhere.
  auto const fx = make_fixture("valmixedscope");
  write_config(fx, "[roles]\nmy_token = \"large\"\n\n[external.jira]\ntoken = \"literal-secret-value\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK_FALSE(r.err.contains("my_token"));
  CHECK(r.err.contains("line 5: token:"));
  cleanup(fx);
}

TEST_CASE("config validate refuses at exit 1 when the file is absent", "[cmd][config][validate]") {
  auto const fx = make_fixture("valabsent");
  REQUIRE_FALSE(std::filesystem::exists(fx.cfg));
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: config file not found: " + fx.cfg.string() + "\n");
  cleanup(fx);
}

TEST_CASE("config validate flags a sensitive key carrying a literal, by source line", "[cmd][config][validate]") {
  // The scan is LINE-ORIENTED over the raw file, not over the flattened
  // map, because it reports the line number and the key AS WRITTEN. Every
  // arm of the skip logic is exercised in one fixture and each is asserted
  // both ways — that the flagged ones appear and the skipped ones do not.
  auto const fx = make_fixture("valsensitive");
  write_config(fx, R"TOML([external.jira]
token = ""
secret = "x" # trailing comment
password = 'y'
API_KEY = "z"
# token = "commented out"
notsensitive = "q"
)TOML");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  constexpr std::string_view tail = ": sensitive key must not carry a literal value in the config file "
                                    "(use *_env convention instead)\n";
  CHECK(r.err == std::format("error: line 3: secret{0}error: line 4: password{0}error: line 5: API_KEY{0}", tail));
  // Named individually so the reason each is ABSENT is recorded:
  //   line 2  `token = ""`         empty after quote-stripping
  //   line 6  `# token = "..."`    a comment line
  //   line 7  `notsensitive`       name does not match the rule
  CHECK_FALSE(r.err.contains("line 2"));
  CHECK_FALSE(r.err.contains("line 6"));
  CHECK_FALSE(r.err.contains("notsensitive"));
  cleanup(fx);
}

TEST_CASE("config validate sees through quotes nested inside quotes", "[cmd][config][validate]") {
  // `trim` is a CHARACTER SET, not a prefix: `"'wrapped'"` strips to
  // `wrapped`, which is non-empty and therefore a finding. A prefix-shaped
  // implementation would let this literal secret through.
  auto const fx = make_fixture("valnested");
  write_config(fx, "[external.jira]\ntoken = \"'wrapped'\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.err.starts_with("error: line 2: token: sensitive key must not carry a literal"));
  cleanup(fx);
}

TEST_CASE("config validate cross-checks the github-issues auth value", "[cmd][config][validate]") {
  auto const fx = make_fixture("valauth");

  write_config(fx, "[external.github-issues]\nauth = \"token-env\"\n");
  auto const missing = dispatch(fx, {"config", "validate"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: external.github-issues.token_env: auth = \"token-env\" requires token_env to name an "
                       "env var\n");

  write_config(fx, "[external.github-issues]\nauth = \"nope\"\n");
  auto const bogus = dispatch(fx, {"config", "validate"});
  CHECK(bogus.code == 1);
  CHECK(bogus.err == "error: external.github-issues.auth: unrecognised auth value \"nope\" (expected: gh-cli, "
                     "token-env, oauth-stored)\n");

  write_config(fx, "[external.github-issues]\nauth = \"gh-cli\"\n");
  CHECK(dispatch(fx, {"config", "validate"}).code == 0);
  cleanup(fx);
}

TEST_CASE("config validate reads auth from the FILE, never from the embedded defaults", "[cmd][config][validate]") {
  // `external.github-issues.auth` defaults to `gh-cli`, so an empty config
  // has a resolved value — and neither check fires, because the checks read
  // the PARSED FILE MAP. A version that read the effective map would still
  // pass the `gh-cli` case above by luck; this is the case that separates
  // them.
  auto const fx = make_fixture("valauthdefault");
  write_config(fx, "[defaults]\nvendor = \"claude\"\n");
  auto const shown = dispatch(fx, {"config", "show"});
  REQUIRE(line_for(shown.out, "external.github-issues.auth") == "external.github-issues.auth = gh-cli");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 0);
  CHECK(r.err.empty());
  cleanup(fx);
}

TEST_CASE("config validate rejects a routed model that is not in its tier's candidate list", "[cmd][config][validate][routing]") {
  // The finding ORDER is observable and is vendor x tier x work-type, NOT
  // sorted key order: `schema` precedes `cli` precedes `feature` in the
  // work-type enum, so `schema` reports before `feature` even though
  // `feature` sorts earlier. Oracle-captured; pinned as one exact payload
  // rather than two `contains` checks, which would pass in either order.
  auto const fx = make_fixture("valrouting");
  write_config(fx, R"TOML([models.claude]
medium = ["claude-sonnet-5", "claude-fable-5"]

[routing.claude.medium]
cli = "claude-fable-5"
feature = "bogus-model"
schema = "another-bogus"
)TOML");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: routing.claude.medium.schema: routed model id \"another-bogus\" is not in the "
                 "models.claude.medium candidate list\n"
                 "error: routing.claude.medium.feature: routed model id \"bogus-model\" is not in the "
                 "models.claude.medium candidate list\n");
  cleanup(fx);
}

TEST_CASE("config validate does NOT validate a routing key whose tier has no models entry", "[cmd][config][validate][routing]") {
  // The oracle's `orelse continue`, and it is the single most
  // counter-intuitive rule in this verb: `routing.codex.medium.cli` names a
  // model that exists nowhere at all, and the file is CLEAN — because
  // `models.codex.medium` is absent, so there is no candidate list to check
  // against. An implementation that validated it would refuse configs the
  // oracle accepts.
  //
  // The claude half of the same file is present so the case cannot pass
  // vacuously: it proves the cross-check is running and simply skipping
  // codex.
  auto const fx = make_fixture("valrouteorphan");
  write_config(fx, R"TOML([models.claude]
medium = ["claude-sonnet-5"]

[routing.claude.medium]
cli = "claude-sonnet-5"

[routing.codex.medium]
cli = "a-model-that-exists-nowhere"
)TOML");
  auto const shown = dispatch(fx, {"config", "show"});
  REQUIRE(line_for(shown.out, "routing.codex.medium.cli") == "routing.codex.medium.cli = a-model-that-exists-nowhere");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 0);
  CHECK(r.out == "config validate: ok\n");
  CHECK(r.err.empty());
  cleanup(fx);
}

TEST_CASE("config validate treats a scalar model tier as a one-element candidate list", "[cmd][config][validate][routing]") {
  auto const fx = make_fixture("valroutescalar");
  write_config(fx, R"TOML([models.claude]
medium = "m1"

[routing.claude.medium]
cli = "m1"
feature = "m2"
)TOML");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: routing.claude.medium.feature: routed model id \"m2\" is not in the models.claude.medium "
                 "candidate list\n");
  cleanup(fx);
}

TEST_CASE("config validate ACCUMULATES findings across steps two through four", "[cmd][config][validate]") {
  // Three different rules, one invocation, one exit — and in the fixed
  // step order: the sensitive scan, then the auth cross-check, then
  // routing. A handler that returned on the first finding would report
  // only the first line and still exit 1, which no exit-code assertion
  // would catch.
  auto const fx = make_fixture("valmulti");
  write_config(fx, R"TOML([external.github-issues]
auth = "token-env"

[external.jira]
token = "literal"

[models.claude]
small = ["a"]

[routing.claude.small]
mechanical = "b"
)TOML");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err == "error: line 5: token: sensitive key must not carry a literal value in the config file "
                 "(use *_env convention instead)\n"
                 "error: external.github-issues.token_env: auth = \"token-env\" requires token_env to name an env var\n"
                 "error: routing.claude.small.mechanical: routed model id \"b\" is not in the models.claude.small "
                 "candidate list\n");
  cleanup(fx);
}

TEST_CASE("config validate stops at a parse failure and reports nothing else", "[cmd][config][validate]") {
  // Step 1 returns IMMEDIATELY. A file that is BOTH unparseable and
  // carries a literal secret reports only the parse error — asserted
  // directly, because the accumulating shape of steps 2-4 makes "report
  // everything" the tempting generalisation.
  auto const fx = make_fixture("valparsefirst");
  write_config(fx, "[defaults\ntoken = \"abc\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err.starts_with("error: line 1: col 10: TOML parse error: "));
  CHECK_FALSE(r.err.contains("sensitive key"));
  CHECK(std::ranges::count(r.err, '\n') == 1);
  cleanup(fx);
}

// --- NAMED DIVERGENCES from the Zig oracle ----------------------------
//
// Four measured classes, all of them in `config validate`'s TOML
// diagnostics and all of them consequences of D12 (Glaze owns TOML here;
// `parse.zig` is a hand-rolled subset there). Each is pinned in the C++
// direction with the oracle's exact bytes recorded in the comment, so the
// difference is ASSERTED rather than tolerated and a future Glaze bump that
// closed one would fail here rather than pass silently.

TEST_CASE("DIVERGENCE decision 964: a duplicate key in one table is a hard error here", "[cmd][config][validate][divergence]") {
  // ORACLE: exit 0, stdout `config validate: ok\n` — `parse.zig:359-367`
  // takes last-wins.
  // HERE:   exit 1, a TOML parse error.
  //
  // The TOML specification makes a duplicate key an error; Glaze is
  // conformant and the oracle is not. Decision 964 keeps this strict, and
  // `config validate` is exactly where an operator should SEE it. Left
  // deliberately divergent — NOT "fixed" back.
  auto const fx = make_fixture("divdupkey");
  write_config(fx, "[defaults]\nvendor = \"a\"\nvendor = \"b\"\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.out.empty());
  CHECK(r.err.starts_with("error: line 3: col 10: TOML parse error: "));
  cleanup(fx);
}

TEST_CASE("DIVERGENCE semantic-rejection columns are one character left of the oracle's", "[cmd][config][validate][divergence]") {
  // ORACLE: `error: line 2: col 6: TOML parse error: float values are not supported`
  // HERE:   `error: line 2: col 5: TOML parse error: float values are not supported`
  //
  // Same line, same message, same exit. The oracle points at the OFFENDING
  // CHARACTER (the `.` of `1.5`) and this tree at the VALUE'S FIRST
  // CHARACTER (the `1`). `toml_error` calls this "equivalent, not
  // identical". Chasing it would mean re-deriving Glaze's byte offsets
  // against a grammar this tree does not have.
  auto const fx = make_fixture("divcolumn");
  write_config(fx, "[defaults]\nx = 1.5\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: line 2: col 5: TOML parse error: float values are not supported\n");
  cleanup(fx);
}

TEST_CASE("the array rejection names the offending element (task 6266)", "[cmd][config][validate]") {
  // Was: "DIVERGENCE the array rejection drops the offending element from
  // its message", pinning the message gap as it stood. `toml.cppm`
  // documents its semantic rejections as reusing "zig's exact wording";
  // for this one it did not — the `; got '1'` suffix naming the offending
  // element was absent. Fixed in `planar.engine.config.toml` (the message
  // is composed there, not a parser-split consequence like the column
  // offset one line below, which stays a genuine, accepted divergence and
  // is NOT touched here).
  //
  // ORACLE: `error: line 2: col 6: TOML parse error: only string arrays are supported; got '1'`
  // HERE:   `error: line 2: col 5: TOML parse error: only string arrays are supported; got '1'`
  auto const fx = make_fixture("arraymsg");
  write_config(fx, "[defaults]\nx = [1, 2]\n");
  auto const r = dispatch(fx, {"config", "validate"});
  CHECK(r.code == 1);
  CHECK(r.err == "error: line 2: col 5: TOML parse error: only string arrays are supported; got '1'\n");
  cleanup(fx);
}

TEST_CASE("DIVERGENCE syntax-error prose is Glaze's error_code name, and the location can differ too",
          "[cmd][config][validate][divergence]") {
  // Three measured pairs. The FORMAT, the stream and the exit code are the
  // oracle's exactly; the prose cannot be, because the two parsers do not
  // share a diagnostic vocabulary, and for an unterminated string the LINE
  // differs as well — which the task brief flagged only as a message-text
  // difference.
  //
  //   `[defaults]\nvendor = "abc\n`
  //     ORACLE: line 3, col 1, `unterminated string (newline in string)`
  //     HERE:   line 2, col 14, `syntax_error`
  //   `[defaults\nvendor = "a"\n`
  //     ORACLE: line 1, col 10, `expected '.' or ']' in table header, got '\n'`
  //     HERE:   line 1, col 10, `syntax_error`
  //   `this is not toml\n`
  //     ORACLE: line 1, col 7, `expected '=', got 'i'`
  //     HERE:   line 1, col 6, `syntax_error`
  //
  // Matching would require reimplementing `parse.zig`'s grammar, which is
  // the work D12 exists to avoid.
  auto const fx = make_fixture("divsyntax");

  write_config(fx, "[defaults]\nvendor = \"abc\n");
  auto const unterminated = dispatch(fx, {"config", "validate"});
  CHECK(unterminated.code == 1);
  CHECK(unterminated.err == "error: line 2: col 14: TOML parse error: syntax_error\n");

  write_config(fx, "[defaults\nvendor = \"a\"\n");
  auto const bracket = dispatch(fx, {"config", "validate"});
  CHECK(bracket.code == 1);
  CHECK(bracket.err == "error: line 1: col 10: TOML parse error: syntax_error\n");

  write_config(fx, "this is not toml\n");
  auto const garbage = dispatch(fx, {"config", "validate"});
  CHECK(garbage.code == 1);
  CHECK(garbage.err == "error: line 1: col 6: TOML parse error: syntax_error\n");
  cleanup(fx);
}

TEST_CASE("DIVERGENCE this tree ACCEPTS three TOML forms the oracle rejects", "[cmd][config][validate][divergence]") {
  // The other direction, and the one a byte-for-byte stderr pin would also
  // hit. Glaze is a full TOML parser; `parse.zig` is a restricted subset,
  // so all three of these are LEGAL TOML that the oracle refuses:
  //
  //   inline table       ORACLE line 2 col 8  `inline tables are not supported`
  //   multi-line string  ORACLE line 2 col 7  `multi-line strings ("""...""") are not supported`
  //   numeric underscore ORACLE line 3 col 1  `expected '=', got '\n'`
  //
  // Accepting them is a SUPERSET: no config an operator could previously
  // load stops loading, and the alternative — teaching Glaze to refuse
  // valid TOML — would be a genuine regression rather than parity.
  auto const fx = make_fixture("divpermissive");
  for (auto const& body : {"[defaults]\nopts = {a = 1}\n", "[defaults]\nx = \"\"\"hi\"\"\"\n", "[defaults]\nx = 1_000\n"}) {
    INFO("accepted-here document: " << body);
    write_config(fx, body);
    auto const r = dispatch(fx, {"config", "validate"});
    CHECK(r.code == 0);
    CHECK(r.out == "config validate: ok\n");
    CHECK(r.err.empty());
  }
  cleanup(fx);
}

TEST_CASE("both table-header shapes the oracle accepts are accepted here too", "[cmd][config][validate]") {
  // The counterpart to the divergence list, and the reason it is short:
  // repeated table headers and a super-table following its own sub-table
  // are LEGAL TOML that Glaze rejects on its own and `normalize_sections`
  // (task 6082) rewrites before Glaze sees them. Both match the oracle
  // exactly. Named here so a Glaze bump that broke the normalisation shows
  // up as a config-plane failure rather than only as a toml.t.cpp one.
  auto const fx = make_fixture("valheaders");

  write_config(fx, "[defaults]\nvendor = \"a\"\n\n[defaults]\nscope = \"g\"\n");
  auto const repeated = dispatch(fx, {"config", "validate"});
  CHECK(repeated.code == 0);
  CHECK(repeated.out == "config validate: ok\n");

  write_config(fx, "[external.jira.status]\ntodo = \"T\"\n\n[external.jira]\nbase_url = \"u\"\n");
  auto const super_after_sub = dispatch(fx, {"config", "validate"});
  CHECK(super_after_sub.code == 0);
  CHECK(super_after_sub.out == "config validate: ok\n");
  // Both halves actually landed in the resolved map — a normalisation that
  // silently dropped one would still validate clean.
  auto const shown = dispatch(fx, {"config", "show"});
  CHECK(line_for(shown.out, "external.jira.status.todo") == "external.jira.status.todo = T");
  CHECK(line_for(shown.out, "external.jira.base_url") == "external.jira.base_url = u");
  cleanup(fx);
}

// --- the family-wide invariant ----------------------------------------

TEST_CASE("no config leaf opens the database", "[cmd][config][db]") {
  // Asserted once across all five as a family invariant, rather than only
  // per case: `config init` and `config path` are what an operator runs
  // BEFORE `planar init`, and the runtime applies pending migrations on
  // first use — so a leaf that opened the database could migrate the
  // operator's live one past what every installed binary supports.
  auto fx = make_fixture("nodb");
  fx.vars.emplace("PLANAR_EDITOR", "/usr/bin/true");
  write_config(fx, k_rich_config);
  for (auto const& args : std::vector<std::vector<std::string>>{{"config", "path"},
                                                                {"config", "init"},
                                                                {"config", "edit"},
                                                                {"config", "show"},
                                                                {"config", "show", "--effective"},
                                                                {"config", "show", "--json"},
                                                                {"config", "show", "--raw"},
                                                                {"config", "show", "--defaults"},
                                                                {"config", "validate"}}) {
    INFO("leaf: " << std::format("{}", args));
    CHECK_FALSE(dispatch(fx, args).db_open);
  }
  // ...and the database file was never even created.
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
  cleanup(fx);
}
