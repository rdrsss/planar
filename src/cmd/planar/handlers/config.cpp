/// @file config.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.config` (plan 996,
/// task 6259). See config.cppm for the two starter blobs, the ignored
/// `--format` flag, and the three named `config validate` divergences.

module planar.cmd.planar.handlers.config;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.config.effective;
import planar.engine.config.toml;
import planar.cmd.planar.cli_log;
import planar.cmd.planar.context;
import planar.cmd.planar.editor;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace cfg = engine::config;

namespace {

/// @brief The starter written by `config init`.
///
/// The LONGER of the two blobs — it carries the eight-line
/// `[models.codex]` / `[roles]` example the `config edit` copy never
/// received. 1267 bytes, byte-matched against the built oracle. See
/// config.cppm for why the two are not merged.
constexpr std::string_view k_starter_init = R"TOML(# ~/.planar/config.toml — Planar configuration
#
# This file was created by "planar config init" (or "planar init").
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
# # Per-vendor model tier maps + role→tier routing (plan 540). Override a
# # tier to re-route every role at that tier; see `planar models`.
# [models.codex]
# medium = "gpt-5.6-terra"
#
# [roles]
# coder = "large"
#
# [associations."org:acme"]
# github_lead_repo = "acme/platform"
#
# [associations."org:acme".external.jira.status]
# done = "Closed"
)TOML";

/// @brief The starter written by `config edit`.
///
/// The SHORTER blob: identical to `k_starter_init` except that it lacks the
/// `[models.codex]` / `[roles]` block. 1048 bytes, byte-matched against the
/// built oracle. `edit.zig`'s own comment claims this "mirrors init.zig's
/// starter_config exactly" — it does not, and reproducing the drift is D2.
constexpr std::string_view k_starter_edit = R"TOML(# ~/.planar/config.toml — Planar configuration
#
# This file was created by "planar config init" (or "planar init").
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
/// @return The label, e.g. `config file` or `env: PLANAR_VENDOR`.
auto provenance_label(const cfg::value_with_source& entry) -> std::string {
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

auto config_init(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(args);
  auto const path = config_file_path(ctx);
  if (!path.has_value()) {
    return std::unexpected(path.error());
  }

  std::error_code ec;
  if (std::filesystem::exists(*path, ec)) {
    ctx.out() << "config init: already exists " << path->string() << '\n';
    return {};
  }
  if (!write_file(*path, k_starter_init)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "initializing config file: FileWriteFailed"));
  }
  ctx.out() << "config init: created " << path->string() << '\n';
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

    auto const label = provenance_label(entry);
    // A single-candidate tier renders exactly like every scalar key; only a
    // genuine multi-candidate list gets the extra clause.
    bool const show_candidates = entry.candidates.size() > 1;

    if (as_json) {
      ctx.out() << "{\"key\":\"" << key << "\",\"value\":\"" << display << "\",\"provenance\":\"" << label << '"';
      if (show_candidates) {
        ctx.out() << ",\"candidates\":[";
        for (std::size_t i = 0; i < entry.candidates.size(); ++i) {
          if (i > 0) {
            ctx.out() << ',';
          }
          ctx.out() << '"' << entry.candidates[i] << '"';
        }
        ctx.out() << ']';
      }
      ctx.out() << "}\n";
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

  // Steps 2-4 ACCUMULATE. Every finding is a stderr line; the exit is 1
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

  // ---- step 4: the work-type routing cross-check -------------------------
  //
  // Resolution order is vendor x tier x work-type — NOT sorted key order —
  // and it is observable, because it is the order the findings print in.
  // Oracle-confirmed: with `schema` and `feature` both mis-routed, `schema`
  // reports first even though `feature` sorts earlier.
  {
    auto const env      = env_for(ctx);
    auto const resolved = cfg::resolve(std::string_view{*content}, env, std::nullopt);
    if (!resolved.has_value()) {
      // Step 1 already parsed the same bytes, so this is a resolver bug
      // rather than an operator error. The oracle marks the arm
      // `unreachable`; refusing loudly is the same statement without the
      // undefined behaviour.
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving configuration: ParseFailed"));
    }

    for (auto const& vendor : cfg::vendors) {
      for (auto const& tier : cfg::tiers) {
        auto const model_key = std::format("models.{}.{}", vendor, tier);
        auto const model_it  = resolved->effective.find(model_key);
        // A tier with NO `models.*` entry is NOT validated — the oracle's
        // `orelse continue`. Confirmed against the built binary: a
        // `routing.codex.medium.cli` naming a model that exists nowhere
        // exits 0 when `models.codex.medium` is absent.
        if (model_it == resolved->effective.end()) {
          continue;
        }
        std::vector<std::string> candidates = model_it->second.candidates;
        if (candidates.empty()) {
          candidates.push_back(model_it->second.value);
        }

        for (auto const& work_type : cfg::work_types) {
          auto const routing_key = std::format("routing.{}.{}.{}", vendor, tier, work_type);
          auto const routing_it  = resolved->effective.find(routing_key);
          if (routing_it == resolved->effective.end() || routing_it->second.value.empty()) {
            continue;
          }
          if (!std::ranges::contains(candidates, routing_it->second.value)) {
            report(std::format("{}: routed model id \"{}\" is not in the {} candidate list", routing_key,
                               routing_it->second.value, model_key));
          }
        }
      }
    }
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

  CLI::App* show = config->add_subcommand("show", "Print the resolved configuration.");
  add_bool(*show, "--effective");
  add_bool(*show, "--raw");
  add_bool(*show, "--defaults");
  add_string(*show, "--scope");
  add_string_default(*show, "--format", "text");
  add_json(*show);

  config->add_subcommand("edit", "Edit the configuration file in $EDITOR.");

  config->add_subcommand("validate", "Validate configuration file syntax.");

  config->add_subcommand("init", "Initialize the configuration file.");

  config->add_subcommand("path", "Show the configuration file path.");
}

} // namespace planar::cmd::handlers
