/// @file src/cmd/planar/handlers/config/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.config` (plan 996,
/// task 6259). See config.cppm for the two starter blobs, the ignored
/// `--format` flag, and the three named `config validate` divergences.

module planar.cmd.planar.handlers.config;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.config.effective;
import planar.engine.config.policy;
import planar.engine.config.toml;
import planar.json_text;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.context;
import planar.cmd.planar.editor;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.config.show;
import planar.cmd.planar.handlers.config.edit;
import planar.cmd.planar.handlers.config.validate;
import planar.cmd.planar.handlers.config.path;

namespace planar::cmd::handlers {

namespace cfg = engine::config;

namespace {

/// @brief The starter written by `config edit`.
///
/// The starter written by `config edit` when no file exists. 1048 bytes,
/// byte-matched against the built oracle.
constexpr std::string_view k_starter_edit = R"TOML(# ~/.planar/config.toml — Planar configuration
#
# This file was created by "planar config edit" (or "planar init").
# All keys are optional. Anything you don't set falls through to the
# embedded defaults shipped in the binary.
#
# To see every available key with its current default value, run:
#   planar config show --defaults
#
# To see the fully resolved configuration with provenance per key, run:
#   planar config show --effective
#
# Environment variables override config values. They always take precedence.
# See docs/architecture.md for the full configuration plane reference.
#
# To edit this file in your $EDITOR, run:
#   planar config edit
#
# Example customizations (uncomment and fill in):
#
# [defaults]
# vendor = "claude"
#
# [workbench]
# root = "~/.planar/workbench"
#
# [external.jira]
# base_url = "https://your-org.atlassian.net"
# user_env  = "JIRA_USER"
# token_env = "JIRA_TOKEN"
#
# [associations."org:acme"]
# github_lead_repo = "acme/platform"
#
# [associations."org:acme".external.jira.status]
# done = "Closed"
)TOML";

/// @brief Resolve the config file path or produce the oracle's refusal.
/// @param ctx The invocation context.
/// @return The path, or `error: resolving config path: HomeNotSet` at exit 1.
auto config_file_path(context& ctx) -> std::expected<std::filesystem::path, domain_error> {
  auto path = resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving config path: HomeNotSet"));
  }
  return *path;
}

/// @brief Read a file whole.
/// @param path The file.
/// @return Its bytes, or unset when it could not be opened.
auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// @brief Write `body` to `path`, creating missing parent directories.
///
/// `createDirPath` in the oracle, which ignores an already-existing parent
/// and works for a relative path as well as an absolute one (probed: a
/// relative `$PLANAR_CONFIG_PATH` of `rel/x.toml` created `rel/` under the
/// invocation's cwd and reported the relative path back verbatim).
/// @param path Where.
/// @param body What.
/// @return True on success.
auto write_file(const std::filesystem::path& path, std::string_view body) -> bool {
  if (path.has_parent_path() && !path.parent_path().empty()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // An existing directory reports `ec` clear with a `false` return, so the
    // return value is deliberately ignored; a genuine failure surfaces as
    // the open below failing.
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file << body;
  return file.good();
}

/// @brief An `env_view` reading through the context's environment lookup.
///
/// One line, but it is the seam that keeps this family hermetic under test
/// while still reporting real env provenance in production. See
/// `cfg::env_view::from_lookup`.
/// @param ctx The invocation context.
/// @return The view.
auto env_for(context& ctx) -> cfg::env_view {
  return cfg::env_view::from_lookup(ctx.env());
}

/// @brief The display value for one effective-map entry, masking sensitive
/// names as `***`.
///
/// The oracle passes the FULL DOTTED KEY to `sensitiveName`, not its last
/// segment, even though `sensitiveName`'s own contract is written in terms
/// of a bare key name. Reproduced exactly, because the two readings
/// disagree in practice: `external.jira.token` matches the exact rule
/// "token" under the LAST-SEGMENT reading and matches nothing under the
/// full-key one, and it is the full-key one the oracle prints.
///
/// The rule is reachable at all only through a user-defined role, which is
/// the one key family whose NAME comes from the operator: `[roles]
/// my_token = "medium"` produces the effective key `roles.my_token`, which
/// ends in `_token`. Oracle-confirmed — `roles.my_token = ***  [config
/// file]`. Every key the resolver hard-codes is insensitive under this
/// reading, `external.jira.token_env` included, so a test that only tried
/// the built-in keys would conclude masking was dead code.
/// @param key The effective-map key.
/// @param entry The resolved value.
/// @return The value to print.
auto masked_value(std::string_view key, const cfg::value_with_source& entry) -> std::string_view {
  if (cfg::sensitive_name(key)) {
    return "***";
  }
  if (!entry.env_var_name.empty() && cfg::sensitive_name(entry.env_var_name)) {
    return "***";
  }
  return entry.value;
}

/// @brief The provenance label for one entry.
/// @param entry The resolved value.
/// @param config_path The configuration file, named in the label of a policy value.
/// @return The label, e.g. `config file` or `env: PLANAR_VENDOR`. A policy value
/// read from a file names the file and the table: `config file: <path>, policy` or
/// `per-association override: <path>, associations."<slug>".policy`.
auto provenance_label(const cfg::value_with_source& entry, const std::filesystem::path& config_path) -> std::string {
  if (!entry.section.empty()) {
    return std::format("{}: {}, {}",
                       entry.source_ == cfg::provenance::assoc_override ? "per-association override" : "config file",
                       config_path.string(), entry.section);
  }
  switch (entry.source_) {
  case cfg::provenance::env:
    return std::format("env: {}", entry.env_var_name);
  case cfg::provenance::assoc_override:
    return "per-association override";
  case cfg::provenance::config_file:
    return "config file";
  case cfg::provenance::embedded_default:
    return "embedded default";
  }
  return "embedded default"; // unreachable — every enumerator handled above.
}

/// @brief Strip a `#` comment from a value, respecting quotes.
///
/// Ported arm for arm from `validate.zig`'s `stripInlineComment`: a `#`
/// inside a quoted run is data, and the quote character that opened the run
/// is the only one that closes it.
/// @param value The raw text right of the `=`.
/// @return The value with any trailing comment removed and trimmed.
auto strip_inline_comment(std::string_view value) -> std::string_view {
  bool in_quote   = false;
  char quote_char = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    char const c = value[i];
    if (in_quote) {
      if (c == quote_char) {
        in_quote = false;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      in_quote   = true;
      quote_char = c;
      continue;
    }
    if (c == '#') {
      auto head = value.substr(0, i);
      while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) {
        head.remove_suffix(1);
      }
      return head;
    }
  }
  return value;
}

/// @brief Trim every leading and trailing character that appears in `set`.
///
/// `std::mem.trim`'s semantics, which are a CHARACTER SET and not a prefix
/// — `trim("'\"x\"'", "\"'")` yields `x`. Getting this wrong would let
/// `token = "'secret'"` past the sensitive-literal scan.
/// @param value The text.
/// @param set The characters to strip.
/// @return The trimmed view.
auto trim_any(std::string_view value, std::string_view set) -> std::string_view {
  while (!value.empty() && set.contains(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && set.contains(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

/// @brief The string payload of a flattened TOML value, or `""` for every
/// other kind. Mirrors `validate.zig`'s `strVal`.
/// @param map The flattened document.
/// @param key The dotted key.
/// @return The string, or `""`.
auto str_val(const cfg::toml_map& map, std::string_view key) -> std::string_view {
  auto const it = map.find(key);
  if (it == map.end() || it->second.kind_ != cfg::toml_value::kind::string) {
    return "";
  }
  return it->second.string_;
}

} // namespace

auto config_path(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(args);
  auto const path = config_file_path(ctx);
  if (!path.has_value()) {
    return std::unexpected(path.error());
  }
  ctx.out() << path->string() << '\n';
  return {};
}

auto config_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(args);
  auto const path = config_file_path(ctx);
  if (!path.has_value()) {
    return std::unexpected(path.error());
  }

  std::error_code ec;
  if (!std::filesystem::exists(*path, ec) && !write_file(*path, k_starter_edit)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "initializing config file: FileWriteFailed"));
  }

  // The editor is exec'd ON THE CONFIG FILE, not on a temp copy — so
  // `editor::invoke` (which is temp-file shaped) is deliberately NOT used
  // here, only its resolution and spawn primitives. Same split the oracle
  // makes: `edit.zig` calls `resolveEditor` and then spawns directly.
  auto const               command = resolve_editor(ctx.env(), std::nullopt);
  std::vector<std::string> argv{command, path->string()};
  auto const               status = spawn_inherit(ctx.env(), argv);
  if (!status.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "spawning editor: FileNotFound"));
  }
  if (*status != 0) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("editor exited with code {}", *status)));
  }
  return {};
}

auto config_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `--defaults` returns BEFORE the config path is resolved, so it works on
  // a machine with no `$HOME` at all. Written with `<<` and no terminator:
  // the embedded file ends in its own newline and the oracle prints it with
  // `{s}`, not `{s}\n`.
  if (cliapp::flag_bool(args, "--defaults")) {
    ctx.out() << cfg::defaults_toml();
    return {};
  }

  auto const path = config_file_path(ctx);
  if (!path.has_value()) {
    return std::unexpected(path.error());
  }
  auto const content = read_file(*path);

  // `--raw` prints the operator's own bytes, and ZERO bytes when the file
  // is absent — not an error, and not the defaults.
  if (cliapp::flag_bool(args, "--raw")) {
    if (content.has_value()) {
      ctx.out() << *content;
    }
    return {};
  }

  // An EMPTY `--scope` is no scope, matching the oracle's `if (s.len > 0)`.
  auto const                      scope_flag = cliapp::flag_string(args, "--scope");
  std::optional<std::string_view> scope;
  if (scope_flag.has_value() && !scope_flag->empty()) {
    scope = std::string_view{*scope_flag};
  }

  std::optional<std::string_view> content_view;
  if (content.has_value()) {
    content_view = std::string_view{*content};
  }
  auto const env      = env_for(ctx);
  auto       resolved = cfg::resolve(content_view, env, scope);
  if (!resolved.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving configuration: ParseFailed"));
  }
  bool const as_json = cliapp::flag_bool(args, "--json");
  // `--json` implies provenance; `--format` is declared and never read (see
  // config.cppm).
  bool const with_provenance = cliapp::flag_bool(args, "--effective") || as_json;

  for (auto const& key : cfg::sorted_keys(resolved->effective)) {
    auto const it = resolved->effective.find(key);
    if (it == resolved->effective.end()) {
      continue;
    }
    auto const& entry   = it->second;
    auto const  display = masked_value(key, entry);

    if (!with_provenance) {
      ctx.out() << key << " = " << display << '\n';
      continue;
    }

    auto const label = provenance_label(entry, *path);
    // A single-candidate tier renders exactly like every scalar key; only a
    // genuine multi-candidate list gets the extra clause.
    bool const show_candidates = entry.candidates.size() > 1;

    if (as_json) {
      // Every string goes through the JSON escaper: a value holding `"` or
      // `\` used to be streamed between literal quotes and broke the line
      // (found by plan 1033 task 6494, whose array values are JSON text).
      std::string line = "{\"key\":";
      json_text::append_json_string(line, key);
      line += ",\"value\":";
      json_text::append_json_string(line, display);
      line += ",\"provenance\":";
      json_text::append_json_string(line, label);
      if (show_candidates) {
        line += ",\"candidates\":[";
        for (std::size_t i = 0; i < entry.candidates.size(); ++i) {
          if (i > 0) {
            line += ',';
          }
          json_text::append_json_string(line, entry.candidates[i]);
        }
        line += ']';
      }
      line += "}\n";
      ctx.out() << line;
      continue;
    }

    ctx.out() << key << " = " << display << "  [" << label << ']';
    if (show_candidates) {
      ctx.out() << " (candidates: ";
      for (std::size_t i = 0; i < entry.candidates.size(); ++i) {
        if (i > 0) {
          ctx.out() << ", ";
        }
        ctx.out() << entry.candidates[i];
      }
      ctx.out() << ')';
    }
    ctx.out() << '\n';
  }
  return {};
}

/// @brief Whether the file opens the named table, or a table under it.
///
/// Lexical, like the other line scans here, because a flattened map has no
/// entry for an empty table and a removed table with no keys must still refuse.
/// @param content The whole configuration file.
/// @param table The dotted table name, for example `execute`.
/// @return True when some table header names `table` or `table.<...>`.
auto has_table(std::string_view content, std::string_view table) -> bool {
  std::string_view rest{content};
  while (!rest.empty()) {
    auto const newline = rest.find('\n');
    auto const text    = trim_any(rest.substr(0, newline == std::string_view::npos ? rest.size() : newline), " \t\r");
    if (!text.empty() && text.front() == '[') {
      // A trailing `# comment` after the closing bracket is not part of the
      // table name (task 7057).
      auto const header = text.substr(0, std::min(text.size(), text.find('#')));
      auto const name   = trim_any(trim_any(header, " \t[]"), " \t");
      if (name == table || (name.starts_with(table) && name.size() > table.size() && name[table.size()] == '.')) {
        return true;
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    rest = rest.substr(newline + 1);
  }
  return false;
}

/// @brief Whether the file carries a leftover `[queue]` table, in any form.
///
/// Lexical, like `has_table`: a `[queue]` or `[queue.<...>]` header, or a
/// top-level dotted key `queue.<key> = <value>` that appears before the first
/// table header. Planar no longer reads the queue's settings, so the table's
/// contents are never parsed here.
/// @param content The whole configuration file.
/// @return True when the file names the `queue` table in either form.
auto has_queue_table(std::string_view content) -> bool {
  if (has_table(content, "queue")) {
    return true;
  }
  bool             top_level = true;
  std::string_view rest{content};
  while (!rest.empty()) {
    auto const newline = rest.find('\n');
    auto const text    = trim_any(rest.substr(0, newline == std::string_view::npos ? rest.size() : newline), " \t\r");
    if (text.starts_with('[')) {
      top_level = false;
    } else if (top_level && text.starts_with("queue.")) {
      return true;
    }
    if (newline == std::string_view::npos) {
      break;
    }
    rest = rest.substr(newline + 1);
  }
  return false;
}

auto config_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(args);
  auto const path = config_file_path(ctx);
  if (!path.has_value()) {
    return std::unexpected(path.error());
  }

  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("config file not found: {}", path->string())));
  }
  auto const content = read_file(*path);
  if (!content.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "reading config file: ReadFailed"));
  }

  // ---- leftover `[queue]` table: one warning, no finding ------------------
  //
  // The build queue's settings left planar's configuration. A
  // leftover table is never validated and never changes the exit, so it is
  // a stderr warning written here, before the steps below, and not a
  // `report` finding. Printed once whatever else the file contains.
  if (has_queue_table(*content)) {
    ctx.err() << "warning: config.toml has a [queue] table, which planar ignores; "
                 "set queue settings with planar-build-queue config set\n";
  }

  // ---- step 1: TOML parse. A failure returns IMMEDIATELY -----------------
  //
  // The FORMAT is the oracle's exactly; the line, column and message bytes
  // are the three named divergences (config.cppm).
  auto const parsed = cfg::parse_toml(*content);
  if (!parsed.has_value()) {
    auto const& err = parsed.error();
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("line {}: col {}: TOML parse error: {}", err.line, err.column, err.message)));
  }

  // Steps 2-5 ACCUMULATE. Every finding is a stderr line; the exit is 1
  // once, at the end. `error_from_rendered` carries the whole payload so
  // this handler never writes to `ctx.err()` itself and the reporting site
  // stays the single place that turns a failure into bytes.
  std::string findings;
  auto const  report = [&findings](std::string_view body) { findings += std::format("error: {}\n", body); };

  // ---- step 2: the sensitive-literal scan --------------------------------
  //
  // Line-oriented over the RAW file, deliberately: it reports the line
  // number and the key AS WRITTEN, which a flattened map has thrown away.
  // Skips blanks and comment lines. A `[table]` header is now TRACKED
  // rather than merely skipped (decision 1118, task 6265): the scan used
  // to be purely lexical over the last path segment before `=`, with no
  // table context at all, so `[roles]\nmy_token = "large"` refused even
  // though the value is a tier name -- there is no way to name a role
  // ending in `_token`/`_secret`/`_password`/`_key` and have the config
  // validate at all, and the `*_env` remedy the message suggests is
  // meaningless for a role.
  //
  // Scoped to `external.*` (an ALLOWLIST, not a denylist of roles/
  // role_vendors): that is the only table family `effective.cpp` implements
  // the `*_env` indirection convention for at all
  // (`external.jira.token_env`, `external.github-issues.token_env`), so it
  // is the only place the scan's own suggested remedy could ever apply.
  // Scoping this way closes the whole false-positive CLASS, not just the
  // `[roles]` instance that surfaced it: any future table with an
  // innocuously-named key would be exempt too, the same way `[roles]` now
  // is.
  {
    std::size_t      line_index = 0;
    std::string_view rest{*content};
    std::string      current_table; // "" at document root; dotted, e.g. "external.jira"
    while (true) {
      auto const newline = rest.find('\n');
      auto const line    = rest.substr(0, newline == std::string_view::npos ? rest.size() : newline);
      auto const trimmed = trim_any(line, " \t\r");
      if (!trimmed.empty() && trimmed.front() != '#') {
        if (trimmed.front() == '[') {
          // `[table]` or `[[array-of-tables]]` — strip one bracket from each
          // side regardless of which, so a double-bracket header still
          // yields the bare dotted path.
          auto header   = trim_any(trimmed, "[]");
          current_table = std::string{trim_any(header, " \t")};
        } else if (auto const eq = trimmed.find('='); eq != std::string_view::npos) {
          auto const raw_key = trim_any(trimmed.substr(0, eq), " \t");
          auto const raw_val = strip_inline_comment(trim_any(trimmed.substr(eq + 1), " \t"));
          auto const value   = trim_any(trim_any(raw_val, "\"'"), " \t");
          // This schema never authors an inline dotted key (`a.b = 1`); every
          // nesting level is its own `[table]` header, so `raw_key` is always
          // a single leaf segment and `current_table` alone carries the
          // dotted context.
          auto const in_scope = current_table == "external" || current_table.starts_with("external.");
          if (in_scope && !value.empty() && cfg::sensitive_name(raw_key)) {
            report(std::format("line {}: {}: sensitive key must not carry a literal value in the config file "
                               "(use *_env convention instead)",
                               line_index + 1, raw_key));
          }
        }
      }
      if (newline == std::string_view::npos) {
        break;
      }
      rest = rest.substr(newline + 1);
      ++line_index;
    }
  }

  // ---- step 3: the github-issues auth cross-checks -----------------------
  //
  // Both read the PARSED FILE MAP, not the effective map: a value inherited
  // from the embedded defaults is not something the operator wrote and is
  // not this verb's business. `auth = "gh-cli"` from defaults therefore
  // triggers neither check.
  auto const gh_auth      = str_val(*parsed, "external.github-issues.auth");
  auto const gh_token_env = str_val(*parsed, "external.github-issues.token_env");

  if (gh_auth == "token-env" && gh_token_env.empty()) {
    report("external.github-issues.token_env: auth = \"token-env\" requires token_env to name an env var");
  }
  if (!gh_auth.empty()) {
    constexpr std::array<std::string_view, 3> valid_auth = {"gh-cli", "token-env", "oauth-stored"};
    if (!std::ranges::contains(valid_auth, gh_auth)) {
      report(std::format("external.github-issues.auth: unrecognised auth value \"{}\" (expected: gh-cli, "
                         "token-env, oauth-stored)",
                         gh_auth));
    }
  }

  // ---- step 4: removed tables and keys ----------------------------------
  //
  // planar-execute's selector and profiles left planar's configuration with
  // `[execute]` (plan 1142, task 7459), and the transcript capture settings
  // left with the introspection engines (plan 1142, task 7472). A file that still
  // carries one is refused by name rather than having its keys ignored.
  auto const removed = [&](std::string_view name) {
    return has_table(*content, name) || std::ranges::any_of(*parsed, [&](auto const& kv) {
             return kv.first == name ||
                    (kv.first.starts_with(name) && kv.first.size() > name.size() && kv.first[name.size()] == '.');
           });
  };
  if (removed("execute")) {
    report("execute: unknown key (the [execute] table was removed from planar's configuration)");
  }
  if (removed("introspection.transcripts")) {
    report("introspection.transcripts: unknown key (the transcript capture settings were removed)");
  }
  if (removed("introspection.transcript_scan_bytes")) {
    report("introspection.transcript_scan_bytes: unknown key (the transcript read budget was removed)");
  }

  // ---- step 5: the `[policy]` tables (plan 1142, task mo-policy-config) -----
  //
  // Policy keys are facts Planar can check at one verb. A workflow-shaped key
  // (`after_review`, `then`, `retry`, ...) is refused as an unknown key, by
  // name, and so is a value of the wrong type.
  for (auto const& finding : cfg::validate_policy(*parsed)) {
    report(std::format("{}: {}", finding.key, finding.message));
  }

  if (!findings.empty()) {
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure, std::move(findings)));
  }

  ctx.out() << "config validate: ok\n";
  return {};
}

auto declare_config(CLI::App& root) -> void {
  CLI::App* config = root.add_subcommand(
      "config",
      "Read, inspect, and validate the Planar configuration file.\n\n  The configuration file lives at ~/.planar/config.toml by "
      "default.\n  Set $PLANAR_CONFIG_PATH to use a different path.\n  Resolution order (highest to lowest priority):\n    1. "
      "Explicit --config-path flag\n    2. $PLANAR_CONFIG_PATH\n    3. ~/.planar/config.toml");
  config->require_subcommand(0);

  config_cli::attach_show(config);

  config_cli::attach_edit(config);

  config_cli::attach_validate(config);

  config_cli::attach_path(config);
}

} // namespace planar::cmd::handlers
