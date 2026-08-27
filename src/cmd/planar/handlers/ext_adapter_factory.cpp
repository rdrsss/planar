/// @file ext_adapter_factory.cpp
/// @brief Implementation of
/// `planar.cmd.planar.handlers.ext_adapter_factory`. See the module
/// interface for the no-precedence rule, the empty-token disagreement
/// between the two credential sources, and why `gh` is spawned rather than
/// shelled.

module;

module planar.cmd.planar.handlers.ext_adapter_factory;

import std;
import planar.process;
import planar.adapter;
import planar.http;
import planar.engine.external;
import planar.engine.extsync.github;
import planar.engine.extsync.jira;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd::handlers {

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
  auto const                     base_url = sys.base_url.value_or(std::string{});
  adapter::auth_credential const cred{.kind = adapter::auth_kind::bearer, .token = std::move(*token)};

  auto wire = deps.wire();
  if (!wire) {
    return std::unexpected(factory_error::unsupported_system_kind);
  }

  if (sys.kind == system_ns::system_kind::jira) {
    auto made = std::make_unique<engine::extsync::jira::jira_adapter>(base_url, cred, *wire);
    return std::make_unique<adapter_handle>(std::move(wire), std::move(made), adapter_kind::jira);
  }
  auto made = std::make_unique<engine::extsync::github::github_adapter>(base_url, cred, *wire);
  return std::make_unique<adapter_handle>(std::move(wire), std::move(made), adapter_kind::github);
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

} // namespace planar::cmd::handlers
