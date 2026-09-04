/// @file ext_adapter_factory.cpp
/// @brief Implementation of
/// `planar.cmd.planar_ext.handlers.ext_adapter_factory`. See the module
/// interface for the no-precedence rule, the empty-token disagreement
/// between the two credential sources, and why `gh` is spawned rather than
/// shelled.

module;

// Glaze is not a module, so it comes in through the global module fragment
// rather than an import. `create_remote`'s response parse is this file's
// only consumer — see ext_adapter_factory.cppm on why the creation path
// reads the provider's response itself instead of going through the adapter
// interface.
//
// `engine/extsync/json_read.hpp` has the same three accessors and is
// deliberately NOT reused: it belongs to a different TARGET, and including
// a private header across that boundary is the `cmd_* -> engine_*` file
// edge D18 exists to keep out. The guards below reproduce its semantics —
// a wrong-typed value reads as ABSENT, never as a parse failure.
#include <glaze/glaze.hpp>

module planar.cmd.planar_ext.handlers.ext_adapter_factory;

import std;
import planar.process;
import planar.adapter;
import planar.http;
import planar.engine.external;
import planar.engine.extsync.github;
import planar.engine.extsync.jira;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext::handlers {

namespace system_ns = engine::external::system;

namespace {

/// @brief The whitespace the oracle trims from `gh auth token`'s stdout.
///
/// Exactly `std.mem.trim(u8, result.stdout, " \t\r\n")` — four characters,
/// NOT `std::isspace` (which would also strip a vertical tab and a form
/// feed). Whitespace-only output therefore trims to empty and is refused,
/// which is captured.
constexpr std::string_view k_trim_set = " \t\r\n";

/// @brief Trim `k_trim_set` from both ends.
/// @param value The raw text.
/// @return The trimmed view.
auto trim(std::string_view value) -> std::string_view {
  auto const first = value.find_first_not_of(k_trim_set);
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = value.find_last_not_of(k_trim_set);
  return value.substr(first, last - first + 1);
}

/// @brief Resolve the bearer token for `sys`.
///
/// One source per row and no fallback — see the module header. `token-env`
/// accepts an EMPTY value and refuses only an ABSENT one; `gh-cli` refuses
/// empty. That asymmetry is the oracle's, reproduced deliberately.
/// @param sys The row.
/// @param deps The injected seams.
/// @return The token, or the refusal.
auto resolve_credential(const system_ns::external_system& sys, const factory_deps& deps)
    -> std::expected<std::string, factory_error> {
  switch (sys.auth) {
  case system_ns::auth_method::token_env: {
    auto value = deps.env(sys.auth_ref);
    if (!value.has_value()) {
      return std::unexpected(factory_error::token_env_var_missing);
    }
    // NOT trimmed and NOT rejected when empty: the oracle dupes whatever
    // the variable holds, so an empty variable wires an adapter with an
    // empty bearer token and exits 0.
    return std::move(*value);
  }
  case system_ns::auth_method::gh_cli: {
    auto const ran = deps.gh();
    if (!ran.spawned) {
      return std::unexpected(factory_error::gh_cli_not_found);
    }
    if (ran.exit_code != 0) {
      return std::unexpected(factory_error::gh_cli_failed);
    }
    auto const token = trim(ran.output);
    if (token.empty()) {
      return std::unexpected(factory_error::gh_cli_empty_token);
    }
    return std::string{token};
  }
  case system_ns::auth_method::oauth_stored:
    // Accepted by the schema's CHECK constraint and documented, but served
    // by nothing. The oracle refuses with a message naming the deferral
    // rather than pretending the method works.
    return std::unexpected(factory_error::unsupported_auth_method);
  }
  return std::unexpected(factory_error::unsupported_auth_method);
}

} // namespace

auto spawn_capture(std::string_view program, std::span<const std::string_view> args) -> token_command_result {
  // The body moved to `planar.process::capture` (layer 1) at task 6272,
  // unchanged. BOTH of the measured, load-bearing properties documented on
  // this function travelled with it and are commented at the new site:
  // `posix_spawnp` reports ENOENT directly (rc=2, pid=0) rather than through
  // `waitpid`, and the early return on `rc != 0` must not be folded into the
  // `waitpid` path even though a break-probe against it SURVIVES. See
  // `src/lib/process/process.cpp` and `process.t.cpp`'s header.
  //
  // This wrapper stays because `token_command_result` is the factory's own
  // vocabulary — `resolve_credential` switches on `spawned` to keep
  // `gh_cli_not_found` and `gh_cli_failed` apart — and because
  // `ext_factory.t.cpp` pins `spawn_capture` itself.
  auto ran = planar::process::capture(program, args);
  return {.spawned = ran.spawned, .exit_code = ran.exit_code, .output = std::move(ran.output)};
}

auto gh_auth_token_command() -> token_command {
  return [] {
    constexpr std::array<std::string_view, 2> k_args{"auth", "token"};
    return spawn_capture("gh", k_args);
  };
}

auto default_transport_factory() -> transport_factory {
  return [] -> std::unique_ptr<http::transport> { return std::make_unique<http::curl_transport>(); };
}

auto default_deps(env_lookup env) -> factory_deps {
  return {.env = std::move(env), .gh = gh_auth_token_command(), .wire = default_transport_factory()};
}

auto build_adapter(const engine::external::system::external_system& sys, const factory_deps& deps)
    -> std::expected<std::unique_ptr<adapter_handle>, factory_error> {
  // ORDER IS OBSERVABLE: the credential resolves BEFORE the kind is
  // checked, so a `linear` row with an absent env var reports the missing
  // variable. Captured against a row that fails both ways.
  auto token = resolve_credential(sys, deps);
  if (!token) {
    return std::unexpected(token.error());
  }

  if (sys.kind != system_ns::system_kind::jira && sys.kind != system_ns::system_kind::github_issues) {
    return std::unexpected(factory_error::unsupported_system_kind);
  }

  // A NULL `base_url` becomes the EMPTY string rather than a refusal — the
  // oracle's `sys.base_url orelse ""`. `ext test` on such a row exits 0.
  auto const base_url = sys.base_url.value_or(std::string{});
  // The credential is COPIED into the handle as well as moved into the
  // adapter: `ext create` sends its POST on the raw transport and has to
  // build its own `Authorization` header, and the adapter does not re-expose
  // what it copied. See `adapter_handle`'s header.
  adapter::auth_credential const cred{.kind = adapter::auth_kind::bearer, .token = *token};

  auto wire = deps.wire();
  if (!wire) {
    return std::unexpected(factory_error::unsupported_system_kind);
  }

  if (sys.kind == system_ns::system_kind::jira) {
    auto made = std::make_unique<engine::extsync::jira::jira_adapter>(base_url, cred, *wire);
    return std::make_unique<adapter_handle>(std::move(wire), std::move(made), adapter_kind::jira, std::move(*token));
  }
  auto made = std::make_unique<engine::extsync::github::github_adapter>(base_url, cred, *wire);
  return std::make_unique<adapter_handle>(std::move(wire), std::move(made), adapter_kind::github, std::move(*token));
}

auto adapter_handle::post_comment(std::string_view external_id, std::string_view body) const
    -> std::expected<void, adapter::adapter_error> {
  if (_kind == adapter_kind::jira) {
    return static_cast<const engine::extsync::jira::jira_adapter&>(*_adapter).post_comment(external_id, body);
  }
  return static_cast<const engine::extsync::github::github_adapter&>(*_adapter).post_comment(external_id, body);
}

auto adapter_handle::create_issue(std::string_view owner, std::string_view repo, std::string_view title, std::string_view body,
                                  std::span<const std::string> labels) const
    -> std::expected<engine::extsync::github::created_issue, adapter::adapter_error> {
  // GitHub-only: the parent-issue strategy never builds a handle over a
  // Jira system, so `_kind` is always `github` here. See this method's
  // header.
  assert(_kind == adapter_kind::github);
  return static_cast<const engine::extsync::github::github_adapter&>(*_adapter).create_issue(owner, repo, title, body, labels);
}

auto adapter_handle::link_sub_issue(std::string_view owner, std::string_view repo, std::int64_t parent_number,
                                    std::int64_t child_number) const -> std::expected<void, adapter::adapter_error> {
  assert(_kind == adapter_kind::github);
  return static_cast<const engine::extsync::github::github_adapter&>(*_adapter).link_sub_issue(owner, repo, parent_number,
                                                                                               child_number);
}

auto adapter_handle::link_sub_issue_probe(std::string_view owner, std::string_view repo) const
    -> std::expected<void, adapter::adapter_error> {
  assert(_kind == adapter_kind::github);
  return static_cast<const engine::extsync::github::github_adapter&>(*_adapter).link_sub_issue_probe(owner, repo);
}

auto factory_error_message(factory_error err, const engine::external::system::external_system& sys) -> domain_error {
  switch (err) {
  case factory_error::token_env_var_missing:
    return error_from_body(domain_error_kind::invalid_input, std::format("token env var '{}' is not set", sys.auth_ref));
  case factory_error::unsupported_auth_method:
    return error_from_body(domain_error_kind::invalid_input, "oauth-stored auth not yet supported (matches Go: deferred)");
  case factory_error::gh_cli_not_found:
    return error_from_body(domain_error_kind::invalid_input, "gh-cli auth: `gh` binary not on PATH");
  case factory_error::gh_cli_failed:
    return error_from_body(domain_error_kind::invalid_input,
                           "gh-cli auth: `gh auth token` returned non-zero (run `gh auth login`)");
  case factory_error::gh_cli_empty_token:
    return error_from_body(domain_error_kind::invalid_input, "gh-cli auth: `gh auth token` returned empty output");
  case factory_error::unsupported_system_kind:
    // "in the zig port" is the ORACLE'S wording and is reproduced verbatim
    // even though this binary is not the zig port. parity.t.cpp diffs these
    // bytes against the oracle, so "correcting" the sentence would BREAK
    // parity rather than improve it. It is a wording item for after M10
    // deletes zig/, not a port-time fix.
    return error_from_body(domain_error_kind::invalid_input, std::format("system kind '{}' is not supported in the zig port",
                                                                         system_ns::system_kind_to_text(sys.kind)));
  }
  return error_from_body(domain_error_kind::generic_failure, "ext test: building adapter");
}

namespace {

/// @brief Trim ONE trailing slash, as the oracle's `trimSlash` does.
///
/// One, not all: `http://h//` keeps a slash. Preserved rather than
/// "improved" because the resulting URL is observable in `external_url`.
/// @param text The base URL.
/// @return The trimmed view.
auto trim_slash(std::string_view text) -> std::string_view {
  if (!text.empty() && text.back() == '/') {
    return text.substr(0, text.size() - 1);
  }
  return text;
}

/// @brief POST `payload` to the provider's create endpoint and read back the
/// id it assigned.
///
/// Does NOT go through the adapter interface — see `adapter_handle`'s
/// header. The two providers differ in every part: the URL, the `Accept`
/// header, the response field carrying the id, and how the id is spelled
/// locally (Jira's bare `key`, GitHub's `<project>#<number>`).
/// @param handle The built adapter handle.
/// @param sys The registered system row.
/// @param payload The rendered request body.
/// @return The created ticket, or the Zig error TAG to report.
} // namespace

// The parameter type is spelled in FULL rather than through the `system_ns`
// alias, matching the declaration in ext_adapter_factory.cppm exactly.
// Doxygen matches a definition to its declaration textually, so the aliased
// spelling reads as a second, undocumented entity and fails the lint —
// `build_adapter` above spells it the same way for the same reason.
auto create_remote(const adapter_handle& handle, const engine::external::system::external_system& sys, std::string_view payload)
    -> std::expected<created_remote, std::string_view> {
  bool const is_jira = handle.kind() == adapter_kind::jira;

  // Jira has NO default base URL and refuses without one; GitHub falls back
  // to the public API host. GitHub additionally requires a project, Jira
  // does not (its project rides inside the rendered payload).
  std::string base;
  if (is_jira) {
    if (!sys.base_url.has_value()) {
      return std::unexpected(std::string_view{"InvalidInput"});
    }
    base = *sys.base_url;
  } else {
    base = sys.base_url.value_or("https://api.github.com");
  }

  std::string url;
  if (is_jira) {
    url = std::format("{}/rest/api/3/issue", trim_slash(base));
  } else {
    if (!sys.default_project.has_value()) {
      return std::unexpected(std::string_view{"InvalidInput"});
    }
    url = std::format("{}/repos/{}/issues", trim_slash(base), *sys.default_project);
  }

  http::request req{
      .verb    = http::method::post,
      .url     = url,
      .headers = {http::header{.name = "Content-Type", .value = "application/json"},
                  http::header{.name = "Authorization", .value = std::format("Bearer {}", handle.token())},
                  http::header{.name = "Accept", .value = is_jira ? "application/json" : "application/vnd.github+json"}},
      .body    = std::string{payload}};

  auto sent = handle.transport().send(req);
  if (!sent) {
    return std::unexpected(std::string_view{"TransportFailed"});
  }
  if (sent->status < 200 || sent->status >= 300) {
    return std::unexpected(std::string_view{"UnexpectedStatus"});
  }

  auto parsed = glz::read_json<glz::generic>(sent->body);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(std::string_view{"ParseFailed"});
  }
  glz::generic const& root = *parsed;

  if (is_jira) {
    // The `key` must be PRESENT and a STRING. A Jira 2xx carrying no key is
    // `ParseFailed`, not an empty id.
    if (!root.contains("key") || !root.at("key").is_string()) {
      return std::unexpected(std::string_view{"ParseFailed"});
    }
    auto const key = root.at("key").get<std::string>();
    return created_remote{.external_id = key, .external_url = std::format("{}/browse/{}", trim_slash(base), key)};
  }

  if (!root.contains("number") || !root.at("number").is_number()) {
    return std::unexpected(std::string_view{"ParseFailed"});
  }
  auto const number = static_cast<std::int64_t>(root.at("number").get<double>());

  // A MISSING or non-string `html_url` is the EMPTY string, NOT a failure —
  // the oracle's nested `if`. The empty URL is then stored as SQL NULL. So
  // the two fields are asymmetric: the id is required, the URL is not.
  std::string html_url;
  if (root.contains("html_url") && root.at("html_url").is_string()) {
    html_url = root.at("html_url").get<std::string>();
  }
  return created_remote{.external_id = std::format("{}#{}", *sys.default_project, number), .external_url = std::move(html_url)};
}

} // namespace planar::cmd::ext::handlers
