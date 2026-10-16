// queue_rule.t.cpp: the agent rule text and `planar-agent queue rule` (plan
// 1080, tasks hq-rule-text and hq-queue-rule-verb; tech spec 647 § CLI surface
// and § The rule text has one authored source; test spec 649 scenarios citing
// either task; plan 1142, task mo-queue-rule-opt-in, tech spec 693 component 8).
//
// The rule text is two authored files in `src/lib/queuerule/`, embedded into
// `planar-agent` at configure time: `queue-rule.md`, how to use the queue,
// always printed, and `queue-mandate.md`, the instruction that every build and
// test goes through the queue, printed only when the caller's scope has
// `use_queue = true` in `config.toml`. These cases read the files from the
// repository and hold the built binary to them: with no policy `queue rule`
// prints the how-to file byte for byte and none of the mandate; the policy
// (top level, association table, association override, no environment
// override, an invalid file) decides the mandate and its `Source:` line; the
// verb opens no database unless an association table sets `use_queue`, and
// never fails on configuration; the texts carry every element the tasks name;
// and every command they show exists in the binary's own command tree with
// the flags it uses.
//
// Nothing here compares the text to a copy of itself. The content cases name
// what an agent needs to find in the rule and look for those words; they are
// not tied to one phrasing.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.main;
import planar.db;
import planar.db.migrate;

#include "parity_harness.hpp"

namespace {

namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto read_source(char const* path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

/// @brief The authored how-to file, read from the repository.
auto rule_source() -> std::string {
  return read_source(PLANAR_QUEUE_RULE_SOURCE);
}

/// @brief The authored mandate file, read from the repository.
auto mandate_source() -> std::string {
  return read_source(PLANAR_QUEUE_MANDATE_SOURCE);
}

auto contains(std::string_view text, std::string_view needle) -> bool {
  return text.find(needle) != std::string_view::npos;
}

/// @brief The text under one `###` heading that contains `key`, up to the next
/// heading of the same or higher level; empty when no heading matches.
auto section(std::string_view text, std::string_view key) -> std::string {
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto const nl   = text.find('\n', pos);
    auto const line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    if (line.starts_with("### ") && contains(line, key)) {
      auto const start = pos;
      auto       next  = nl == std::string_view::npos ? text.size() : nl + 1;
      while (next < text.size()) {
        auto const e  = text.find('\n', next);
        auto const ln = text.substr(next, e == std::string_view::npos ? std::string_view::npos : e - next);
        if (ln.starts_with("## ") || ln.starts_with("### ")) {
          break;
        }
        next = e == std::string_view::npos ? text.size() : e + 1;
      }
      return std::string{text.substr(start, next - start)};
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  return {};
}

/// @brief Lower-cased text with every run of white space collapsed to one
/// space, so a check does not depend on where the text wraps.
auto lower(std::string_view text) -> std::string {
  std::string out;
  bool        space = false;
  for (auto const c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

/// @brief Every command the text shows: the lines of fenced blocks and the
/// inline code spans, each kept when it starts with `planar-agent`.
auto shown_commands(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> found;
  bool                     fenced = false;
  std::size_t              pos    = 0;
  while (pos <= text.size()) {
    auto const nl   = text.find('\n', pos);
    auto const line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    if (line.starts_with("```")) {
      fenced = !fenced;
    } else if (fenced) {
      if (line.starts_with("planar-agent ")) {
        found.emplace_back(line);
      }
    } else {
      std::size_t i = 0;
      while (i < line.size()) {
        if (line[i] != '`') {
          ++i;
          continue;
        }
        auto const j = line.find('`', i + 1);
        if (j == std::string_view::npos) {
          break;
        }
        auto const span = line.substr(i + 1, j - i - 1);
        if (span.starts_with("planar-agent ")) {
          found.emplace_back(span);
        }
        i = j + 1;
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  return found;
}

auto words(std::string_view line) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::istringstream       in{std::string{line}};
  for (std::string w; in >> w;) {
    out.push_back(w);
  }
  return out;
}

/// @brief The flag and positional names an `App` declares, plus `--help`.
auto declared_names(const CLI::App& app) -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> names;
  for (auto const* option : app.get_options()) {
    names.insert(option->get_name(false, true));
  }
  names.insert("--help");
  return names;
}

auto run_rule(const parity::arena& arena, std::string_view tag, const std::vector<pinned_var>& env,
              std::vector<std::string> args = {"queue", "rule"}) -> capture {
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

} // namespace

TEST_CASE("queue rule with no policy prints the how-to file byte for byte and no mandate", "[cmd][agent][queue][rule]") {
  auto const arena  = parity::make_arena("queue-rule-bytes");
  auto const source = rule_source();
  REQUIRE_FALSE(source.empty());

  // No config file exists in the arena: the shipped default `use_queue = false`.
  auto const got = run_rule(arena, "bytes", parity::pinned_env(arena.cpp_root));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == source);
  CHECK(contains(got.out, "exit 125"));
  CHECK_FALSE(contains(lower(got.out), "do not run it yourself"));
  CHECK_FALSE(contains(got.out, "Builds and tests go through the host queue"));
  CHECK_FALSE(contains(got.out, "Source:"));
}

TEST_CASE("queue rule has no --json and takes no argument", "[cmd][agent][queue][rule]") {
  auto const arena = parity::make_arena("queue-rule-args");
  auto const env   = parity::pinned_env(arena.cpp_root);

  auto const json = run_rule(arena, "json", env, {"queue", "rule", "--json"});
  CHECK(json.code != 0);
  CHECK(json.out.empty());

  auto const extra = run_rule(arena, "extra", env, {"queue", "rule", "now"});
  CHECK(extra.code != 0);
  CHECK(extra.out.empty());
}

TEST_CASE("queue rule opens no database: an unusable database path changes neither its output nor the disk",
          "[cmd][agent][queue][rule]") {
  auto const arena  = parity::make_arena("queue-rule-nodb");
  auto const source = rule_source();
  auto const root   = arena.cpp_root;

  SECTION("the database path names a place that does not exist") {
    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB") {
        var.value = (root / "absent" / "deeper" / "planar.db").string();
      }
    }
    auto const got = run_rule(arena, "absent", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    // Opening the store would have refused it, and creating it would have made
    // its parent directory.
    CHECK_FALSE(std::filesystem::exists(root / "absent"));
  }

  SECTION("the database path sits in a directory that cannot be written") {
    auto const locked = root / "locked";
    std::filesystem::create_directories(locked);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    struct restore {
      std::filesystem::path path;
      ~restore() {
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
      }
    } const guard{locked};

    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB") {
        var.value = (locked / "planar.db").string();
      }
    }
    auto const got = run_rule(arena, "locked", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    CHECK_FALSE(std::filesystem::exists(locked / "planar.db"));
  }

  SECTION("no database path can be resolved at all") {
    auto env = parity::pinned_env(root);
    for (auto& var : env) {
      if (var.name == "PLANAR_DB" || var.name == "HOME") {
        var.unset = true;
      }
    }
    auto const got = run_rule(arena, "unresolved", env);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(got.out == source);
    CHECK_FALSE(std::filesystem::exists(root / "planar.db"));
  }
}

TEST_CASE("the rule text carries every element an agent needs", "[cmd][agent][queue][rule]") {
  auto const source = rule_source();
  auto const text   = lower(source);

  SECTION("the mandate: every build and test goes through the queue, and what counts as one, with examples") {
    auto const mandate = lower(mandate_source());
    CHECK(contains(mandate, "do not run it yourself"));
    CHECK(contains(mandate, "operator's policy"));
    CHECK(contains(mandate, "compiles or links"));
    CHECK(contains(mandate, "test suite"));
    CHECK(contains(mandate, "more than one core"));
    for (auto const* example : {"`make`", "`make test`", "`cmake --build`", "`ninja`", "`ctest`", "`cargo test`", "`go test`",
                                "`npm test`", "`pytest`"}) {
      INFO("example: " << example);
      CHECK(contains(mandate, example));
    }
    // What is not queued, and what to do when unsure.
    CHECK(contains(mandate, "not queued"));
    CHECK(contains(mandate, "unsure"));
    // None of it is in the how-to.
    CHECK_FALSE(contains(text, "compiles or links"));
    CHECK_FALSE(contains(text, "unsure"));
  }

  SECTION("the detached submit and finite observation procedure") {
    CHECK(contains(source, "planar-agent queue run --detach"));
    CHECK(contains(source, "planar-agent queue wait <seq> --timeout 3h --json"));
    CHECK(contains(text, "backlog"));
    CHECK(contains(text, "runtime"));
    CHECK(contains(text, "same sequence number"));
    CHECK(contains(text, "unbounded retry loop"));
    CHECK_FALSE(contains(text, "every 30 seconds"));
    CHECK(contains(text, "sequence number"));
    CHECK(contains(text, "output file"));
  }

  SECTION("what a non-zero exit from the submit means") {
    // No ticket exists then, and each code has its own route.
    CHECK(contains(text, "a non-zero exit from the submit means no ticket was issued"));
    CHECK(contains(text, "126 or 127"));
    CHECK(contains(text, "1 or 2"));
    CHECK(contains(text, "with 125"));
  }

  SECTION("the instruction to pass vendor and role") {
    CHECK(contains(source, "--vendor <vendor>"));
    CHECK(contains(source, "--role <role>"));
    CHECK(contains(text, "always pass"));
  }

  SECTION("what to do on each outcome") {
    for (auto const* outcome : {"exited", "signaled", "timeout", "cancelled", "wait_timeout", "not_started", "abandoned"}) {
      INFO("outcome: " << outcome);
      // A table row names it; the bare word also appears in prose.
      CHECK(contains(source, std::format("| `{}`", outcome)));
    }
    CHECK(contains(text, "| `abandoned` |"));
    CHECK(contains(text, "never assume completion or resubmit automatically"));
  }

  SECTION("the submitter renews a claim while the observer does not") {
    CHECK(contains(source, "`--claim <token>`"));
    CHECK(contains(text, "queue wait` is read-only and never renews a claim"));
    CHECK(contains(text, "renews a claim"));
    std::size_t mentions = 0;
    for (std::size_t at = source.find("`--claim <token>`"); at != std::string::npos;
         at             = source.find("`--claim <token>`", at + 1)) {
      ++mentions;
    }
    CHECK(mentions >= 1);
    // The flag it names is a real flag of `queue run`.
    auto const  root = planar::cmd::agent::root_app();
    auto const* run  = root->get_subcommand_no_throw("queue")->get_subcommand_no_throw("run");
    REQUIRE(run != nullptr);
    CHECK(declared_names(*run).contains("--claim"));
  }

  SECTION("the exit codes of the foreground form") {
    for (auto const* code : {"124", "125", "126", "127", "128"}) {
      INFO("code: " << code);
      CHECK(contains(source, code));
    }
  }
}

TEST_CASE("the rule text cannot be read as permission to bypass the queue on exit 125", "[cmd][agent][queue][rule]") {
  auto const source  = rule_source();
  auto const mandate = mandate_source();

  // The how-to says what a refusal means and to stop and report it.
  auto const refusal = lower(section(source, "exit 125"));
  REQUIRE_FALSE(refusal.empty());
  CHECK(contains(refusal, "stop"));
  CHECK(contains(refusal, "report"));
  // The mandate adds that the command is then never run directly.
  auto const bypass = lower(section(mandate, "never bypass"));
  REQUIRE_FALSE(bypass.empty());
  CHECK(contains(bypass, "must not be run directly"));
  CHECK(contains(bypass, "refused, not bypassed"));
  CHECK(contains(bypass, "`wait_timeout`"));

  auto const fallback = lower(section(mandate, "has no queue"));
  REQUIRE_FALSE(fallback.empty());
  CHECK(contains(fallback, "verifying the installed planar lacks the `queue` verb"));
  CHECK(contains(fallback, "nonzero `queue rule` alone does not establish absence"));
  CHECK(contains(fallback, "needs upgrading"));
  CHECK(contains(fallback, "finite compatibility path"));

  // The refusal names no `queue rule` check, so it cannot be read as the fallback.
  CHECK_FALSE(contains(refusal, "queue rule"));
  CHECK_FALSE(contains(refusal, "--help"));
  // It covers a command that itself exits 125, and tells how to tell the two apart.
  CHECK(contains(refusal, "--notices"));
  // A wait limit or a cancellation is an outcome of a ticket, not this case.
  CHECK(contains(refusal, "`wait_timeout`"));
  // The how-to alone never says to run the command directly, or to refuse to.
  CHECK_FALSE(contains(refusal, "must not be run directly"));

  // The check is never `--help`: an older Planar answers an unknown command's
  // `--help` with its general help and exits 0.
  for (auto const& command : shown_commands(source)) {
    INFO("command: " << command);
    CHECK(command != "planar-agent queue --help");
  }

  // The two are also set side by side in one labelled table.
  auto const confused = lower(section(mandate, "Do not confuse"));
  REQUIRE_FALSE(confused.empty());
  CHECK(contains(confused, "refuses"));
  CHECK(contains(confused, "catalog"));
}

TEST_CASE("the no-queue check discriminates: it passes on this binary and fails where the verb is unknown",
          "[cmd][agent][queue][rule]") {
  // The rule's check is `planar-agent queue rule >/dev/null`. It is sound only
  // if an unknown domain followed by `rule` fails, and if `--help` could not
  // serve instead. An older Planar is modelled by an unknown domain: to it,
  // `queue` is exactly as unknown as `nosuch-domain`.
  auto const arena = parity::make_arena("queue-rule-check");
  auto const env   = parity::pinned_env(arena.cpp_root);

  auto const present = run_rule(arena, "present", env, {"queue", "rule"});
  CHECK(present.code == 0);

  auto const absent = run_rule(arena, "absent", env, {"nosuch-domain", "rule"});
  INFO("stderr:\n" << absent.err);
  CHECK(absent.code != 0);
  CHECK(absent.out.empty());

  // The check the TEXT gives is the one held to that: take the command from
  // "Before the first submission", run it as written, then run it with the
  // `queue` domain made unknown, which is what an older Planar is to it.
  auto const before = section(rule_source(), "Before the first submission");
  REQUIRE_FALSE(before.empty());
  auto const commands = shown_commands(before);
  REQUIRE(commands.size() == 1);
  std::vector<std::string> as_written;
  for (auto const& word : words(commands.front())) {
    if (word == "planar-agent") {
      continue;
    }
    if (word.starts_with(">")) {
      break; // the redirection to /dev/null, not an argument
    }
    as_written.push_back(word);
  }
  REQUIRE(as_written.size() >= 2);
  REQUIRE(as_written.front() == "queue");
  auto const checked = run_rule(arena, "as-written", env, as_written);
  CHECK(checked.code == 0);

  auto old_planar    = as_written;
  old_planar.front() = "nosuch-domain";
  auto const fails   = run_rule(arena, "as-written-old", env, old_planar);
  INFO("the check as written, on a Planar that does not know `queue`: exit " << fails.code);
  CHECK(fails.code != 0);

  // The reason `--help` is not the check: an unknown command's `--help` is
  // answered with the general help and exit 0, so it would pass with no queue.
  auto const help = run_rule(arena, "help", env, {"nosuch-domain", "--help"});
  CHECK(help.code == 0);
  CHECK_FALSE(help.out.empty());
}

TEST_CASE("every planar-agent command the rule shows exists, with the flags it uses", "[cmd][agent][queue][rule]") {
  auto const source   = rule_source();
  auto const commands = shown_commands(source);
  // A parser that found nothing would pass every check below.
  REQUIRE(commands.size() >= 5);

  auto const  root  = planar::cmd::agent::root_app();
  auto const* queue = root->get_subcommand_no_throw("queue");
  REQUIRE(queue != nullptr);

  std::size_t verbs_checked = 0;
  for (auto const& command : commands) {
    INFO("command: " << command);
    auto const tokens = words(command);
    REQUIRE(tokens.size() >= 2);
    REQUIRE(tokens[0] == "planar-agent");
    REQUIRE(tokens[1] == "queue");

    const CLI::App* target = queue;
    std::size_t     next   = 2;
    if (tokens.size() > 2 && !tokens[2].starts_with("-")) {
      target = queue->get_subcommand_no_throw(tokens[2]);
      INFO("verb: " << tokens[2]);
      REQUIRE(target != nullptr);
      next = 3;
      ++verbs_checked;
    }
    auto const names = declared_names(*target);
    for (std::size_t i = next; i < tokens.size(); ++i) {
      if (tokens[i] == "--") {
        break; // what follows belongs to the queued command
      }
      if (tokens[i].starts_with("--")) {
        auto const name = tokens[i].substr(0, tokens[i].find('='));
        INFO("flag: " << name);
        CHECK(names.contains(name));
      }
    }
  }
  CHECK(verbs_checked >= 2);
}

namespace {

/// @brief Write the arena's config file, the one `pinned_env` names.
auto write_config(const parity::arena& arena, std::string_view text) -> void {
  std::ofstream out(arena.cpp_root / "config.toml", std::ios::binary | std::ios::trunc);
  out << text;
}

/// @brief Create and migrate the arena's `planar.db`, with a project rooted at the pinned working directory
/// (`<root>/proj`) that belongs to association `slug`, when one is given.
auto seed_project(const parity::arena& arena, std::optional<std::string_view> slug) -> void {
  auto conn = planar::db::connection::open((arena.cpp_root / "planar.db").string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  auto exec = [&](std::string const& sql) {
    INFO(sql);
    REQUIRE(conn->execute(sql).has_value());
  };
  exec(std::format("insert into projects (id, slug, name, root_path) values (1, 'proj', 'proj', '{}')",
                   (arena.cpp_root / "proj").string()));
  if (slug.has_value()) {
    exec(std::format("insert into associations (id, slug, name, kind) values (1, '{0}', '{0}', 'org')", *slug));
    exec("insert into project_associations (project_id, association_id, source) values (1, 1, 'user')");
  }
}

/// @brief Whether `out` carries the mandate's heading and its instruction.
auto has_mandate(std::string_view out) -> bool {
  return contains(out, "Builds and tests go through the host queue") && contains(lower(out), "do not run it yourself");
}

/// @brief What a printed rule with the mandate looks like: the how-to, the mandate file, then the source line.
auto with_mandate(std::string_view where) -> std::string {
  return rule_source() + "\n" + mandate_source() + std::format("\nSource: operator policy use_queue = true ({})\n", where);
}

} // namespace

TEST_CASE("queue rule prints the mandate and its source when the top-level policy sets use_queue",
          "[cmd][agent][queue][rule][policy]") {
  auto const arena = parity::make_arena("queue-rule-top");
  write_config(arena, "[policy]\nuse_queue = true\n");

  auto const got = run_rule(arena, "top", parity::pinned_env(arena.cpp_root));
  INFO("stderr:\n" << got.err << "stdout:\n" << got.out);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(got.out == with_mandate((arena.cpp_root / "config.toml").string() + ", [policy]"));
  CHECK(has_mandate(got.out));
  // The how-to is still all there, first.
  CHECK(got.out.starts_with(rule_source()));
  CHECK(contains(got.out, "exit 125"));
  // Only a top-level policy: no database was needed.
  CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
}

TEST_CASE("queue rule prints no mandate when use_queue is false or only other policy keys are set",
          "[cmd][agent][queue][rule][policy]") {
  auto const arena = parity::make_arena("queue-rule-off");
  auto const env   = parity::pinned_env(arena.cpp_root);

  write_config(arena, "[policy]\nuse_queue = false\n");
  auto const off = run_rule(arena, "off", env);
  CHECK(off.code == 0);
  CHECK(off.out == rule_source());

  write_config(arena, "[policy]\nreview_required = true\n");
  auto const other = run_rule(arena, "other", env);
  CHECK(other.code == 0);
  CHECK(other.out == rule_source());
}

TEST_CASE("queue rule does not read PLANAR_USE_QUEUE or any other environment override", "[cmd][agent][queue][rule][policy]") {
  auto const arena = parity::make_arena("queue-rule-env");
  auto       env   = parity::pinned_env(arena.cpp_root);
  for (auto const* name : {"PLANAR_USE_QUEUE", "PLANAR_POLICY_USE_QUEUE", "PLANAR_QUEUE_MANDATE", "USE_QUEUE"}) {
    env.push_back(parity::pinned_var{.name = name, .value = "1"});
  }
  SECTION("no config file") {
    auto const got = run_rule(arena, "env-default", env);
    CHECK(got.code == 0);
    CHECK(got.out == rule_source());
  }
  SECTION("a config file that says false") {
    write_config(arena, "[policy]\nuse_queue = false\n");
    auto const got = run_rule(arena, "env-false", env);
    CHECK(got.code == 0);
    CHECK(got.out == rule_source());
  }
}

TEST_CASE("queue rule honors use_queue set only in the caller's association table", "[cmd][agent][queue][rule][policy]") {
  SECTION("a working directory in that association gets the mandate and the association table as its source") {
    auto const arena = parity::make_arena("queue-rule-assoc-yes");
    write_config(arena, "[associations.\"acme\".policy]\nuse_queue = true\n");
    seed_project(arena, "acme");
    auto const got = run_rule(arena, "assoc-yes", parity::pinned_env(arena.cpp_root));
    INFO("stderr:\n" << got.err << "stdout:\n" << got.out);
    CHECK(got.code == 0);
    CHECK(got.out == with_mandate((arena.cpp_root / "config.toml").string() + ", [associations.\"acme\".policy]"));
  }
  SECTION("a working directory in another association does not") {
    auto const arena = parity::make_arena("queue-rule-assoc-other");
    write_config(arena, "[associations.\"acme\".policy]\nuse_queue = true\n");
    seed_project(arena, "elsewhere");
    auto const got = run_rule(arena, "assoc-other", parity::pinned_env(arena.cpp_root));
    CHECK(got.code == 0);
    CHECK(got.out == rule_source());
  }
  SECTION("a project in no association does not") {
    auto const arena = parity::make_arena("queue-rule-assoc-none");
    write_config(arena, "[associations.\"acme\".policy]\nuse_queue = true\n");
    seed_project(arena, std::nullopt);
    auto const got = run_rule(arena, "assoc-none", parity::pinned_env(arena.cpp_root));
    CHECK(got.code == 0);
    CHECK(got.out == rule_source());
  }
  SECTION("an unreachable database falls back to the top-level policy and says so") {
    auto const arena = parity::make_arena("queue-rule-assoc-nodb");
    write_config(arena, "[associations.\"acme\".policy]\nuse_queue = true\n");
    auto const got = run_rule(arena, "assoc-nodb", parity::pinned_env(arena.cpp_root));
    INFO("stderr:\n" << got.err << "stdout:\n" << got.out);
    CHECK(got.code == 0);
    CHECK(got.out.starts_with(rule_source()));
    CHECK_FALSE(contains(got.out, "Source:"));
    CHECK(contains(got.out, "\nscope not resolved (database not usable"));
    CHECK(contains(got.out, "only the top-level [policy] table was consulted"));
    // The lookup opened nothing for writing and created nothing.
    CHECK_FALSE(std::filesystem::exists(arena.cpp_root / "planar.db"));
  }
}

TEST_CASE("queue rule: an association's false overrides a top-level true", "[cmd][agent][queue][rule][policy]") {
  auto const config = std::string{"[policy]\nuse_queue = true\n\n[associations.\"acme\".policy]\nuse_queue = false\n"};
  auto const where  = [](const parity::arena& arena) { return (arena.cpp_root / "config.toml").string() + ", [policy]"; };

  SECTION("in the association: no mandate") {
    auto const arena = parity::make_arena("queue-rule-override-in");
    write_config(arena, config);
    seed_project(arena, "acme");
    auto const got = run_rule(arena, "override-in", parity::pinned_env(arena.cpp_root));
    CHECK(got.code == 0);
    CHECK(got.out == rule_source());
  }
  SECTION("in another association: the top-level true applies") {
    auto const arena = parity::make_arena("queue-rule-override-out");
    write_config(arena, config);
    seed_project(arena, "elsewhere");
    auto const got = run_rule(arena, "override-out", parity::pinned_env(arena.cpp_root));
    CHECK(got.code == 0);
    CHECK(got.out == with_mandate(where(arena)));
  }
}

TEST_CASE("queue rule never fails on configuration: it prints the how-to and a policy note",
          "[cmd][agent][queue][rule][policy]") {
  struct bad_config {
    char const* tag;
    char const* text;
    char const* note;
  };
  for (auto const& bad : {
           bad_config{"parse", "[policy\nuse_queue = true\n", "policy unreadable: line 1"},
           bad_config{"unknown", "[policy]\nuse_queue = true\nafter_review = true\n", "policy unreadable: policy.after_review: "},
           bad_config{"type", "[policy]\nuse_queue = \"yes\"\n", "policy unreadable: policy.use_queue: must be "},
       }) {
    INFO("config: " << bad.tag);
    auto const arena = parity::make_arena(std::format("queue-rule-bad-{}", bad.tag));
    write_config(arena, bad.text);
    auto const got = run_rule(arena, bad.tag, parity::pinned_env(arena.cpp_root));
    INFO("stderr:\n" << got.err << "stdout:\n" << got.out);
    CHECK(got.code == 0);
    CHECK(got.err.empty());
    CHECK(got.out.starts_with(rule_source()));
    CHECK(contains(got.out, std::string{"\n"} + bad.note));
    // Whatever the file said, an unreadable policy grants no mandate.
    CHECK_FALSE(contains(got.out, "Source:"));
    CHECK_FALSE(contains(got.out, "Builds and tests go through the host queue"));
    // The note is the last line, and one line.
    auto const tail = got.out.substr(rule_source().size());
    CHECK(std::ranges::count(tail, '\n') == 2);
  }

  SECTION("a config path that is a directory") {
    auto const arena = parity::make_arena("queue-rule-bad-dir");
    std::filesystem::create_directories(arena.cpp_root / "config.toml");
    auto const got = run_rule(arena, "dir", parity::pinned_env(arena.cpp_root));
    CHECK(got.code == 0);
    CHECK(got.out.starts_with(rule_source()));
    CHECK(contains(got.out, "\npolicy unreadable: config path is not a regular file"));
  }
}
