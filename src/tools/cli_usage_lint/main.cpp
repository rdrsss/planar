/// @file main.cpp
/// @brief `cli_usage_lint` — validate authored CLI invocations against the
/// live command schema (C++ port of `zig/tools/cli_usage_lint.zig`, plan
/// 996, task 6402).
///
/// Every Planar binary exposes `<bin> schema`, a deterministic flat JSON
/// catalog of its command tree (commands, subcommands, and per-command
/// flags with inherited flags already merged in). This tool dumps that
/// schema for each binary, then scans the authored workflow surfaces
/// (agents/, skills/src/, docs/) for command invocations inside code
/// spans and fenced blocks, and reports any `--flag` referenced on a
/// command that the binary does not actually expose.
///
/// This is a DELIBERATE, LINE-FOR-LINE port of the zig original. Every
/// helper below mirrors its zig namesake so behavior parity is checkable
/// by inspection, not just by differential run. See that file's own
/// header for the drift class this catches and the exit-code contract:
/// 0 = clean, 1 = violations found, 2 = usage / internal error.
///
/// Usage:
///   cli_usage_lint <repo-root> <bin-path> [<bin-path> ...]
///
/// KEPT EXACTLY: the `cli-lint-ignore` escape hatch (a line containing that
/// marker is skipped entirely), and the subprocess contract — this tool
/// shells `<bin-path> schema`, so it lints the ACTUAL SHIPPED BINARY
/// rather than any in-process command declaration. Since decision 1030
/// (D18, task 6486) `planar-execute schema` exists too, so all FIVE
/// binaries are passed as bin-paths; the binary's Lua host-function
/// manifest is a separate surface, still covered by unit tests, not by
/// this tool.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.json_dom;

namespace {

namespace fs = std::filesystem;
using planar::json_dom::json_kind;
using planar::json_dom::json_value;
using planar::json_dom::parse_json;

/// Flags the parser auto-provides on every command but does not list in
/// the schema. Referencing these is always valid.
constexpr std::array<std::string_view, 2> k_global_ok_flags{"--help", "-h"};

/// Directories under the repo root that hold authored CLI prose.
constexpr std::array<std::string_view, 3> k_scan_dirs{"agents", "skills/src", "docs"};

/// Single authored files, outside those directories, that carry commands
/// agents are told to run.
constexpr std::array<std::string_view, 1> k_scan_files{"src/lib/queuerule/queue-rule.md"};

/// The commands a bare flag span (`` `--wait-timeout` ``, `` `--timeout <duration>` ``)
/// in one of `k_scan_files` is judged against: the rule text names flags in
/// running prose without the command beside them, and they are the flags of
/// the queue's submit and poll verbs. A span is valid when at least one of
/// these commands has the flag. Spans in `k_scan_dirs` are not judged this
/// way: there a bare flag belongs to whatever tool the sentence is about.
constexpr std::array<std::string_view, 2> k_flag_span_homes{"planar-agent queue run", "planar-agent queue status"};

// ---------------------------------------------------------------------------
// Schema model.
// ---------------------------------------------------------------------------

/// One resolved command: its allowed flag tokens and whether it is a leaf.
struct command_entry {
  std::unordered_set<std::string> flags;
  bool                            is_leaf = true;
};

struct catalog_t {
  /// "planar workbench list" -> command_entry.
  std::unordered_map<std::string, command_entry> commands;

  [[nodiscard]] auto contains(std::string const& key) const -> bool {
    return commands.contains(key);
  }
  [[nodiscard]] auto get(std::string const& key) -> command_entry* {
    auto it = commands.find(key);
    return it == commands.end() ? nullptr : &it->second;
  }
};

struct violation_t {
  std::string                file;
  std::size_t                line = 0;
  std::string                command;
  std::string                flag;
  std::optional<std::string> suggestion;
  /// True for a bare flag span judged against `k_flag_span_homes`; `command`
  /// is then empty.
  bool span = false;
};

// ---------------------------------------------------------------------------
// Subprocess: run `<bin> schema` with an isolated PLANAR_DB.
// ---------------------------------------------------------------------------

struct subprocess_result {
  std::string stdout_text;
  bool        exited_zero = false;
};

/// @brief Run `bin schema`, capturing stdout, with `PLANAR_DB` pointed at a
/// scratch path so the subprocess never touches the operator's real
/// database (see CLAUDE.md, "Never run a from-source binary against the
/// real database"). Mirrors `SchemaSubprocessEnv` in the zig original.
auto run_schema_subprocess(std::string const& bin_path) -> subprocess_result {
  std::random_device rd;
  std::string        hex;
  {
    std::uniform_int_distribution<int> dist(0, 15);
    for (int i = 0; i < 16; ++i) {
      static constexpr char digits[] = "0123456789abcdef";
      hex.push_back(digits[dist(rd)]);
    }
  }
  fs::path const tmp_root = [] {
    if (char const* t = std::getenv("TMPDIR"))
      return fs::path{t};
    if (char const* t = std::getenv("TEMP"))
      return fs::path{t};
    if (char const* t = std::getenv("TMP"))
      return fs::path{t};
    return fs::path{"."};
  }();
  fs::path const db_path = tmp_root / std::format("planar-cli-schema-{}.db", hex);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0)
    return {};

  pid_t const pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return {};
  }
  if (pid == 0) {
    // Child.
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    // Redirect stderr away — only stdout is expected to be JSON.
    int const devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    setenv("PLANAR_DB", db_path.c_str(), 1);
    execlp(bin_path.c_str(), bin_path.c_str(), "schema", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Parent.
  close(pipe_fds[1]);
  std::string out;
  char        buf[4096];
  ssize_t     n;
  while ((n = read(pipe_fds[0], buf, sizeof(buf))) > 0)
    out.append(buf, static_cast<std::size_t>(n));
  close(pipe_fds[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  std::error_code ec;
  fs::remove(db_path, ec);
  fs::remove(fs::path{db_path.string() + "-wal"}, ec);
  fs::remove(fs::path{db_path.string() + "-shm"}, ec);

  return {.stdout_text = std::move(out), .exited_zero = WIFEXITED(status) && WEXITSTATUS(status) == 0};
}

// ---------------------------------------------------------------------------
// Schema loading.
// ---------------------------------------------------------------------------

/// @brief Populate `catalog` from `bin_path`'s `schema` output. Consumes
/// only the six keys the zig original reads: `commands[].command`,
/// `.subcommands`, and per-flag `long` / `aliases` / `short`.
/// @return false on subprocess failure or malformed JSON.
auto load_schema(catalog_t& catalog, std::string const& bin_path) -> bool {
  auto const [stdout_text, ok] = run_schema_subprocess(bin_path);
  if (!ok)
    return false;
  auto parsed = parse_json(stdout_text);
  if (!parsed.has_value())
    return false;
  json_value const* commands = parsed->find("commands");
  if (commands == nullptr || commands->kind != json_kind::array)
    return false;

  for (auto const& c : commands->array) {
    json_value const* command_field = c.find("command");
    if (command_field == nullptr || command_field->kind != json_kind::string)
      continue;
    command_entry entry;
    if (json_value const* subs = c.find("subcommands"); subs != nullptr && subs->kind == json_kind::array) {
      entry.is_leaf = subs->array.empty();
    }
    if (json_value const* flags = c.find("flags"); flags != nullptr && flags->kind == json_kind::array) {
      for (auto const& f : flags->array) {
        if (json_value const* long_field = f.find("long"); long_field != nullptr && long_field->kind == json_kind::string)
          entry.flags.insert(long_field->string);
        if (json_value const* aliases = f.find("aliases"); aliases != nullptr && aliases->kind == json_kind::array) {
          for (auto const& a : aliases->array)
            if (a.kind == json_kind::string)
              entry.flags.insert(a.string);
        }
        if (json_value const* short_field = f.find("short");
            short_field != nullptr && short_field->kind == json_kind::string && !short_field->string.empty())
          entry.flags.insert(std::format("-{}", short_field->string));
      }
    }
    catalog.commands[command_field->string] = std::move(entry);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Token classification helpers.
// ---------------------------------------------------------------------------

auto is_trailing_junk(char c) -> bool {
  switch (c) {
  case '.':
  case ',':
  case ';':
  case ':':
  case ')':
  case ']':
  case '}':
  case '"':
  case '\'':
  case '`':
    return true;
  default:
    return false;
  }
}
auto is_leading_junk(char c) -> bool {
  switch (c) {
  case '(':
  case '[':
  case '{':
  case '"':
  case '\'':
  case '`':
  case '$':
    return true;
  default:
    return false;
  }
}

/// Strip surrounding prose punctuation/quotes that can cling to a token.
auto clean_token(std::string_view t) -> std::string_view {
  while (!t.empty() && is_trailing_junk(t.back()))
    t.remove_suffix(1);
  while (!t.empty() && is_leading_junk(t.front()))
    t.remove_prefix(1);
  return t;
}

auto is_flag(std::string_view t) -> bool {
  return t.size() >= 2 && t[0] == '-';
}

/// The flag name with any `=value` suffix removed.
auto flag_name(std::string_view t) -> std::string_view {
  if (auto eq = t.find('='); eq != std::string_view::npos)
    return t.substr(0, eq);
  return t;
}

/// Placeholder/value token where a subcommand could go: `<id>`, `...`,
/// `{x}`, `NNN`, `$VAR`, all-caps METAVAR, or a number.
auto is_placeholder(std::string_view t) -> bool {
  if (t.empty())
    return true;
  if (t[0] == '<' || t[0] == '{' || t[0] == '$')
    return true;
  if (t == "...")
    return true;
  if (t == "[flags]" || t == "[options]")
    return true;
  bool all_upper = true;
  bool all_digit = true;
  for (char c : t) {
    if (!(std::isupper(static_cast<unsigned char>(c)) || c == '_'))
      all_upper = false;
    if (!std::isdigit(static_cast<unsigned char>(c)))
      all_digit = false;
  }
  return all_upper || all_digit;
}

/// A flag token that is itself a placeholder, not a literal flag.
auto is_placeholder_flag(std::string_view name) -> bool {
  for (char c : name)
    switch (c) {
    case '<':
    case '>':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
      return true;
    default:
      break;
    }
  return false;
}

auto is_global_ok(std::string_view name) -> bool {
  for (auto g : k_global_ok_flags)
    if (name == g)
      return true;
  return false;
}

auto levenshtein(std::string_view a, std::string_view b) -> std::size_t {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j)
    row[j] = j;
  for (std::size_t i = 0; i < a.size(); ++i) {
    std::size_t prev = row[0];
    row[0]           = i + 1;
    for (std::size_t j = 0; j < b.size(); ++j) {
      std::size_t const tmp  = row[j + 1];
      std::size_t const cost = a[i] == b[j] ? 0 : 1;
      row[j + 1]             = std::min({row[j] + 1, row[j + 1] + 1, prev + cost});
      prev                   = tmp;
    }
  }
  return row[b.size()];
}

/// Closest allowed flag by Levenshtein distance, when within threshold.
auto suggest(command_entry const& cmd, std::string_view name) -> std::optional<std::string> {
  std::optional<std::string> best;
  std::size_t                best_d = std::numeric_limits<std::size_t>::max();
  for (auto const& k : cmd.flags) {
    std::size_t const d = levenshtein(name, k);
    if (d < best_d) {
      best_d = d;
      best   = k;
    }
  }
  if (best.has_value() && best_d <= 3 && best_d < name.size())
    return best;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Command scanning.
// ---------------------------------------------------------------------------

/// Tokenize on runs of space/tab, matching zig's `tokenizeAny(u8, text, " \t")`.
auto tokenize_ws(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> toks;
  std::size_t                   i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
      ++i;
    std::size_t const start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t')
      ++i;
    if (i > start)
      toks.push_back(text.substr(start, i - start));
  }
  return toks;
}

/// Scan a single shell-command-ish run of text for a binary invocation.
void scan_command(catalog_t& catalog, std::vector<std::string> const& bin_names, std::string const& file, std::size_t line_no,
                  std::string_view text, std::vector<violation_t>& violations) {
  std::vector<std::string> toks;
  for (auto raw : tokenize_ws(text))
    toks.emplace_back(clean_token(raw));

  std::optional<std::size_t> bi;
  std::string                bin_name;
  for (std::size_t idx = 0; idx < toks.size(); ++idx) {
    for (auto const& name : bin_names) {
      if (toks[idx] == name) {
        bi       = idx;
        bin_name = name;
        break;
      }
    }
    if (bi.has_value())
      break;
  }
  if (!bi.has_value())
    return;
  std::vector<std::string> const rest(toks.begin() + static_cast<long>(*bi) + 1, toks.end());

  std::string resolved                                                          = bin_name;
  std::size_t k                                                                 = 0;
  enum class stop_t : std::uint8_t { end, flag, placeholder, word } stop_reason = stop_t::end;
  for (; k < rest.size(); ++k) {
    std::string const& t = rest[k];
    if (is_flag(t)) {
      stop_reason = stop_t::flag;
      break;
    }
    if (is_placeholder(t)) {
      stop_reason = stop_t::placeholder;
      break;
    }
    std::string const candidate = resolved + " " + t;
    if (catalog.contains(candidate)) {
      resolved = candidate;
    } else {
      stop_reason = stop_t::word;
      break;
    }
  }

  command_entry* cmd = catalog.get(resolved);
  if (cmd == nullptr)
    return;

  // Ambiguity guard: if we stopped at a placeholder/value while the
  // resolved command still has subcommands, its flag set is unknown.
  if (!cmd->is_leaf && (stop_reason == stop_t::placeholder || stop_reason == stop_t::word))
    return;

  for (std::string const& t : rest) {
    // A bare `--` ends the resolved command's own flags: what follows is
    // another program's command line (`planar-agent queue run -- make -j8`),
    // and its flags are not this catalog's to check.
    if (t == "--")
      break;
    if (!is_flag(t))
      continue;
    std::string_view name = flag_name(t);
    if (name.empty())
      continue;
    if (is_placeholder_flag(name))
      continue;
    if (is_global_ok(name))
      continue;
    if (cmd->flags.contains(std::string{name}))
      continue;
    violations.push_back(
        {.file = file, .line = line_no, .command = resolved, .flag = std::string{name}, .suggestion = suggest(*cmd, name)});
  }
}

/// A code segment may contain several shell commands joined by pipes /
/// separators. Split on those, then look for binary invocations.
void scan_code_segment(catalog_t& catalog, std::vector<std::string> const& bin_names, std::string const& file,
                       std::size_t line_no, std::string_view segment, std::vector<violation_t>& violations) {
  constexpr std::string_view delims = "|;&()`";
  std::size_t                i      = 0;
  while (i < segment.size()) {
    while (i < segment.size() && delims.find(segment[i]) != std::string_view::npos)
      ++i;
    std::size_t const start = i;
    while (i < segment.size() && delims.find(segment[i]) == std::string_view::npos)
      ++i;
    if (i > start)
      scan_command(catalog, bin_names, file, line_no, segment.substr(start, i - start), violations);
  }
}

/// Judge a code span that begins with a flag against `k_flag_span_homes`.
/// A span that does not begin with a flag, a placeholder flag, `--help` and a
/// bare `--` are not judged.
void check_flag_span(catalog_t& catalog, std::string const& file, std::size_t line_no, std::string_view span,
                     std::vector<violation_t>& violations) {
  auto const toks = tokenize_ws(span);
  if (toks.empty())
    return;
  std::string_view const first = clean_token(toks[0]);
  if (!is_flag(first))
    return;
  std::string_view const name = flag_name(first);
  if (name.empty() || name == "--" || is_placeholder_flag(name) || is_global_ok(name))
    return;
  std::optional<std::string> suggestion;
  for (auto const home : k_flag_span_homes) {
    command_entry* entry = catalog.get(std::string{home});
    if (entry == nullptr)
      continue;
    if (entry->flags.contains(std::string{name}))
      return;
    if (!suggestion.has_value())
      suggestion = suggest(*entry, name);
  }
  violations.push_back(
      {.file = file, .line = line_no, .command = {}, .flag = std::string{name}, .suggestion = suggestion, .span = true});
}

/// Extract `...` inline code spans from a prose line and scan each. With
/// `flag_spans`, a span that begins with a flag is also judged on its own.
void scan_inline_spans(catalog_t& catalog, std::vector<std::string> const& bin_names, std::string const& file,
                       std::size_t line_no, std::string_view line, std::vector<violation_t>& violations, bool flag_spans) {
  std::size_t i = 0;
  while (i < line.size()) {
    if (line[i] != '`') {
      ++i;
      continue;
    }
    std::size_t const start = i + 1;
    std::size_t       j     = start;
    while (j < line.size() && line[j] != '`')
      ++j;
    if (j >= line.size())
      break;
    scan_code_segment(catalog, bin_names, file, line_no, line.substr(start, j - start), violations);
    if (flag_spans)
      check_flag_span(catalog, file, line_no, line.substr(start, j - start), violations);
    i = j + 1;
  }
}

void scan_file(catalog_t& catalog, std::vector<std::string> const& bin_names, std::string const& file, std::string const& content,
               std::vector<violation_t>& violations, bool flag_spans = false) {
  bool        in_fence = false;
  std::size_t line_no  = 0;
  std::size_t pos      = 0;
  while (pos <= content.size()) {
    std::size_t const      nl = content.find('\n', pos);
    std::string_view const line =
        nl == std::string::npos ? std::string_view{content}.substr(pos) : std::string_view{content}.substr(pos, nl - pos);
    ++line_no;

    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      ++start;
    std::string_view const trimmed = line.substr(start);
    if (trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
      in_fence = !in_fence;
    } else if (line.find("cli-lint-ignore") == std::string_view::npos) {
      if (in_fence) {
        scan_code_segment(catalog, bin_names, file, line_no, line, violations);
      } else {
        scan_inline_spans(catalog, bin_names, file, line_no, line, violations, flag_spans);
      }
    }

    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
}

void scan_dir(catalog_t& catalog, std::vector<std::string> const& bin_names, fs::path const& dir_path,
              std::vector<violation_t>& violations, std::size_t& files_scanned) {
  std::error_code ec;
  if (!fs::exists(dir_path, ec))
    return;
  for (auto const& entry : fs::recursive_directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".md")
      continue;
    ++files_scanned;
    std::ifstream in(entry.path(), std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Matches zig's `std.fs.path.join(repo_root, rel, entry.name)` exactly:
    // the file path reported in a violation carries whatever prefix the
    // caller passed as repo-root, not a path relativized against it.
    scan_file(catalog, bin_names, entry.path().string(), content, violations);
  }
}

} // namespace

/// @brief Lint `workflows/command-policy.json` (plan 1033 task 6707): every
/// entry whose binary is one of the linted binaries must name a LEAF command
/// in that binary's schema catalog, so the closed policy Centurion enforces
/// can never list a verb path the binary does not have. Entries for other
/// programs (`git`) are skipped: they have no catalog.
/// @param catalog The loaded catalogs.
/// @param bin_names The linted binaries' basenames.
/// @param repo_root The repository root.
/// @return One message per bad entry (empty when clean), plus the number of
/// entries checked; or a reason the file could not be read or parsed.
auto check_command_policy(catalog_t const& catalog, std::vector<std::string> const& bin_names, fs::path const& repo_root)
    -> std::expected<std::pair<std::vector<std::string>, std::size_t>, std::string> {
  constexpr std::string_view rel = "workflows/command-policy.json";
  // A tree without a policy has nothing to check (the lint also runs over
  // synthetic roots); this repository's own file is required to exist by
  // src/cmd/planar-execute/policy.t.cpp.
  std::error_code exists_ec;
  if (!fs::exists(repo_root / rel, exists_ec)) {
    return std::pair{std::vector<std::string>{}, std::size_t{0}};
  }
  std::ifstream in(repo_root / rel, std::ios::binary);
  if (!in) {
    return std::unexpected{std::format("{}: cannot read", rel)};
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  auto const parsed = parse_json(buf.str());
  if (!parsed.has_value()) {
    return std::unexpected{std::format("{}: not valid JSON", rel)};
  }
  json_value const* entries = parsed->find("entries");
  if (entries == nullptr || entries->kind != json_kind::array) {
    return std::unexpected{std::format("{}: no \"entries\" array", rel)};
  }
  std::vector<std::string> problems;
  std::size_t              checked = 0;
  for (auto const& e : entries->array) {
    json_value const* binary = e.find("binary");
    json_value const* path   = e.find("path");
    if (binary == nullptr || binary->kind != json_kind::string || path == nullptr || path->kind != json_kind::array) {
      problems.push_back(std::format("{}: an entry lacks a string \"binary\" or an array \"path\"", rel));
      continue;
    }
    if (std::ranges::find(bin_names, binary->string) == bin_names.end()) {
      continue;
    }
    std::string command = binary->string;
    for (auto const& token : path->array) {
      command += " " + (token.kind == json_kind::string ? token.string : std::string{"?"});
    }
    ++checked;
    auto const it = catalog.commands.find(command);
    if (it == catalog.commands.end()) {
      problems.push_back(std::format("{}: `{}` is not a command in the schema catalogs", rel, command));
    } else if (!it->second.is_leaf) {
      problems.push_back(std::format("{}: `{}` is a command group, not a runnable leaf", rel, command));
    }
  }
  return std::pair{std::move(problems), checked};
}

/// @brief Entry point: validate authored CLI invocations under
/// `<repo-root>/{agents,skills/src,docs}` against the live `<bin> schema`
/// catalog of every `<bin-path>` given.
/// @param argc Argument count; must be at least 3 (`<repo-root> <bin-path>...`).
/// @param argv Argument vector: `argv[1]` is the repo root, `argv[2..]` are
/// one or more binary paths whose basenames name the binaries authored prose
/// may invoke.
/// @return 0 when no violation is found; 1 when at least one authored
/// `--flag` reference does not exist on its resolved command; 2 on a usage
/// error or a schema-loading failure.
auto main(int argc, char** argv) -> int {
  if (argc < 3) {
    std::println(stderr, "usage: {} <repo-root> <bin-path> [<bin-path> ...]", argc > 0 ? argv[0] : "cli_usage_lint");
    return 2;
  }
  std::string const              repo_root = argv[1];
  std::vector<std::string> const bin_paths(argv + 2, argv + argc);

  catalog_t catalog;
  for (auto const& bin_path : bin_paths) {
    if (!load_schema(catalog, bin_path)) {
      std::println(stderr, "error: failed to load schema from {}", bin_path);
      return 2;
    }
  }

  std::vector<std::string> bin_names;
  bin_names.reserve(bin_paths.size());
  for (auto const& p : bin_paths)
    bin_names.push_back(fs::path{p}.filename().string());

  std::vector<violation_t> violations;
  std::size_t              files_scanned = 0;
  for (auto rel : k_scan_dirs)
    scan_dir(catalog, bin_names, fs::path{repo_root} / rel, violations, files_scanned);

  // CLAUDE.md (task 6932): a single top-level file rather than a directory,
  // so it needs its own entry point instead of a `k_scan_dirs` walk. It was
  // absent from the scan set entirely -- the file every agent in this
  // repository reads first, dense with claim-ritual and verb examples, and
  // no schema check had ever validated one of them against the live
  // catalogs. Task 6869 found a documented `planar-agent claim --metadata`
  // that fails at parse time; a wrong flag here propagates into every
  // agent's behaviour.
  //
  // `AGENTS.md` is a SYMLINK to `CLAUDE.md` (see § Operating Rules) and is
  // deliberately NOT scanned: walking it would report every finding twice
  // at a second path. The same reasoning and the same fixture shape as
  // `surface_lint`'s own CLAUDE.md entry point (task 6930).
  if (std::error_code ec;
      fs::exists(fs::path{repo_root} / "CLAUDE.md", ec) && fs::is_regular_file(fs::path{repo_root} / "CLAUDE.md", ec)) {
    auto const    claude_md = fs::path{repo_root} / "CLAUDE.md";
    std::ifstream in(claude_md, std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ++files_scanned;
    scan_file(catalog, bin_names, claude_md.string(), content, violations);
  }

  // The queue rule (plan 1080, task hq-rule-text) is authored beside the code
  // that embeds it, not under one of the directories above, and it shows
  // `planar-agent queue` commands that agents copy. Each such file is scanned
  // like CLAUDE.md is: as a single file with its own entry point.
  for (auto const rel : k_scan_files) {
    auto const path = fs::path{repo_root} / rel;
    if (std::error_code ec; fs::exists(path, ec) && fs::is_regular_file(path, ec)) {
      std::ifstream in(path, std::ios::binary);
      std::string   content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      ++files_scanned;
      scan_file(catalog, bin_names, path.string(), content, violations, true);
    }
  }

  auto const policy = check_command_policy(catalog, bin_names, fs::path{repo_root});
  if (!policy.has_value()) {
    std::println(stderr, "error: {}", policy.error());
    return 2;
  }
  for (auto const& problem : policy->first) {
    std::println("{}", problem);
  }

  if (violations.empty() && policy->first.empty()) {
    std::println("cli-usage-lint: clean ({} files, {} commands, {} command-policy entries)", files_scanned,
                 catalog.commands.size(), policy->second);
    return 0;
  }

  std::ranges::sort(violations, [](violation_t const& a, violation_t const& b) {
    if (a.file != b.file)
      return a.file < b.file;
    return a.line < b.line;
  });
  for (auto const& v : violations) {
    if (v.span) {
      std::string const homes = std::format("`{}` nor `{}`", k_flag_span_homes[0], k_flag_span_homes[1]);
      if (v.suggestion.has_value())
        std::println("{}:{}: inline flag span `{}` is on neither {} (did you mean `{}`?)", v.file, v.line, v.flag, homes,
                     *v.suggestion);
      else
        std::println("{}:{}: inline flag span `{}` is on neither {}", v.file, v.line, v.flag, homes);
    } else if (v.suggestion.has_value()) {
      std::println("{}:{}: `{}` has no flag `{}` (did you mean `{}`?)", v.file, v.line, v.command, v.flag, *v.suggestion);
    } else {
      std::println("{}:{}: `{}` has no flag `{}`", v.file, v.line, v.command, v.flag);
    }
  }
  std::println("cli-usage-lint: {} violation(s) across {} files, {} command-policy problem(s)", violations.size(), files_scanned,
               policy->first.size());
  return 1;
}
