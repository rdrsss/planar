/// @file ext_adapter_factory.cppm
/// @brief `planar.cmd.planar.handlers.ext_adapter_factory` — builds a
/// Jira/GitHub adapter INSTANCE from an `external_systems` row, resolving the
/// credential first (plan 996, task 6258).
///
/// Behavior-preserving port (D2) of the auth-resolution half of
/// `zig/src/cmd/planar/handlers/ext/adapter_factory.zig`. The Zig file is 256
/// lines; roughly 120 of them are its hand-rolled `std.http.Client` transport
/// (`httpSend`, `waitForGroup`, `FetchTask`), which is ALREADY ported as
/// `planar.http`'s `curl_transport` and is not duplicated here. What was
/// genuinely missing, and what this module is, is the resolution and
/// construction unit: environment/subprocess credential lookup, then the
/// kind-to-adapter switch.
///
/// ## Why this lives at LAYER 3 and could not live anywhere else
///
/// It composes `engine_extsync` (the two adapters it constructs) with
/// `engine_external` (the `external_system` row it constructs them FROM), and
/// D18 FATALs an `engine -> engine` edge at configure time. So the factory
/// belongs to the first layer that may legally name both, which is the cmd
/// layer — exactly where the Zig original sits. This is the same D20 shape
/// `annotate add` and `unlink` already establish, and it required no
/// redesign: the layering was checked against `cmake/architecture.cmake`
/// before a line was written.
///
/// ## THERE IS NO CREDENTIAL PRECEDENCE, AND LOOKING FOR ONE IS THE TRAP
///
/// `auth_method` is a DISCRIMINANT, not a priority order. Exactly one source
/// is consulted per row — the one the column names — and no other source is
/// read on any path. There is no environment-beats-config rule, no config
/// file, no stored-token column, and therefore no "both present" case to
/// resolve. A port that builds a fallback chain (try the env var, else shell
/// `gh`) would accept rows the oracle refuses, and nothing about the type
/// signatures would show it. Derived by probing the oracle across all three
/// methods, not by reading the switch.
///
/// ## THE TWO CREDENTIAL SOURCES DISAGREE ABOUT AN EMPTY TOKEN
///
/// This is the sharpest finding of the cycle and it is invisible in the
/// Zig source unless you already know to look:
///
///   - `token-env` with the variable set to the EMPTY STRING **succeeds**.
///     `getPosix` returns `""`, which is not null, so the factory wires an
///     adapter whose bearer token is empty and `ext test` exits 0. Only an
///     ABSENT variable refuses.
///   - `gh-cli` with empty output **refuses** (`gh_cli_empty_token`), and so
///     does whitespace-only output, because the token is trimmed first.
///
/// So "empty" is a hard error down one path and a clean success down the
/// other. Both were captured from the oracle (present / absent / empty for
/// every source) and both are pinned as ONE case in `ext_factory.t.cpp`;
/// `ext_leaves.t.cpp` pins the `token-env` half again through the whole
/// verb. Harmonizing them — in either direction — is a behavior change this
/// port is not entitled to make.
///
/// ## THE CREDENTIAL IS RESOLVED BEFORE THE KIND IS CHECKED
///
/// A `gitlab-issues` row whose `token-env` variable is also unset reports the
/// MISSING VARIABLE, not the unsupported kind. Oracle-captured against a row
/// that fails both ways, because the ordering is observable only when both
/// checks would fire and nothing in the signature implies which wins.
///
/// ## `oauth-stored` AND THE TWO UNSUPPORTED KINDS ARE UNREACHABLE VIA THE CLI
///
/// `ext register jira` requires `--auth-env` and `ext register github`
/// selects `gh-cli` when it is omitted, so NO register verb can write an
/// `oauth-stored` row; likewise neither writes `gitlab-issues` or `linear`,
/// though the `external_systems` CHECK constraint permits all three. Those
/// arms are reachable only on a row written some other way — the same
/// situation `ext list`'s NULL `base_url` branch is in (see ext.cppm). They
/// are ported and tested anyway, against rows inserted directly, because the
/// oracle serves them and a row written by an older binary or by hand still
/// reaches them.
///
/// ## `gh` MUST BE SPAWNED, NOT SHELLED, OR TWO REFUSALS COLLAPSE INTO ONE
///
/// `popen` runs its argument through `/bin/sh`, which reports a missing
/// program as exit **127** — indistinguishable from `gh` itself failing. The
/// oracle distinguishes them: a spawn failure is `gh binary not on PATH` and
/// a non-zero exit is `run gh auth login`. So this uses `posix_spawnp`, whose
/// return value carries `ENOENT` directly, and `planar.git`'s `popen`-based
/// `run()` is deliberately NOT reused despite being the tree's only existing
/// subprocess helper.
module;

export module planar.cmd.planar.handlers.ext_adapter_factory;

import std;
import planar.adapter;
import planar.http;
import planar.engine.external;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd::handlers {

/// @brief Why an adapter could not be built. One enumerator per refusal the
/// oracle emits; the messages are in `factory_error_message`.
export enum class factory_error : std::uint8_t {
  unsupported_auth_method, ///< `oauth-stored`, which no ported surface serves.
  token_env_var_missing,   ///< `token-env` and the named variable is ABSENT (empty is fine).
  gh_cli_not_found,        ///< `gh` could not be spawned at all.
  gh_cli_failed,           ///< `gh auth token` exited non-zero.
  gh_cli_empty_token,      ///< `gh auth token` exited 0 but printed nothing after trimming.
  unsupported_system_kind, ///< `gitlab-issues` or `linear`.
};

/// @brief What running a credential subprocess produced.
///
/// `spawned` is the field that keeps `gh_cli_not_found` and `gh_cli_failed`
/// apart, and it is separate from `exit_code` precisely because a shell-based
/// runner cannot tell them apart — see this module's header.
export struct token_command_result {
  bool        spawned   = false; ///< Whether the program was executed at all.
  int         exit_code = 0;     ///< Its exit status; meaningful only when `spawned`.
  std::string output;            ///< Everything it wrote to stdout, untrimmed.
};

/// @brief A credential subprocess, injected so the `gh-cli` arm is testable
/// without a `gh` on the test machine's PATH.
export using token_command = std::function<token_command_result()>;

/// @brief Run `program` with `args`, capturing stdout.
///
/// Uses `posix_spawnp`, so a missing program reports `spawned == false`
/// rather than the shell's exit 127. The child's stderr is redirected to
/// `/dev/null`, matching the oracle: it captures the child's stderr into a
/// buffer it then frees, so a failing `gh` never writes to the operator's
/// terminal.
/// @param program The program name, resolved through `PATH`.
/// @param args The arguments AFTER the program name.
/// @return What the child produced.
export auto spawn_capture(std::string_view program, std::span<const std::string_view> args) -> token_command_result;

/// @brief The real `gh auth token` runner.
/// @return A `token_command` that spawns `gh auth token`.
export auto gh_auth_token_command() -> token_command;

/// @brief Builds transports, injected so a test can substitute the in-process
/// fixture server (or a recording stub) for libcurl.
export using transport_factory = std::function<std::unique_ptr<http::transport>()>;

/// @brief The real transport factory: a `curl_transport` at the default
/// 30-second timeout.
/// @return The factory.
export auto default_transport_factory() -> transport_factory;

/// @brief Which concrete adapter a handle holds.
export enum class adapter_kind : std::uint8_t {
  jira,   ///< A `jira_adapter`.
  github, ///< A `github_adapter`.
};

/// @brief An owned adapter plus the transport it borrows.
///
/// The adapter stores a `http::transport*` and copies its base URL and
/// credential, so this handle only has to keep the TRANSPORT alive — unlike
/// the Zig `Handle`, which also owned the token because the Zig adapters hold
/// borrowed slices. Declaration order is load-bearing: `_wire` is declared
/// first so it is destroyed LAST, after the adapter that points at it.
///
/// ## WHY `transport()` AND `token()` EXIST WHEN `instance()` ALREADY DOES
///
/// Added at task 6295 for `ext create`, and they are the WHOLE of that
/// leaf's precondition. The four-operation `external_adapter` interface
/// covers validate / pull / push / render — but NOT create. The oracle's
/// creation path (`ext/remote.zig`) never goes through the adapter at all:
/// it builds the provider URL itself, sends a POST on the RAW transport, and
/// parses the response for the provider's own id field. So it needs the two
/// things the adapter copied privately and the interface does not re-expose.
///
/// Routing create through a fifth virtual on `external_adapter` would have
/// been the tidier-looking option and was rejected: it would put a method on
/// the interface that the Zig original's `requireAdapter` does not enforce,
/// widening a contract this port is not entitled to change.
export class adapter_handle {
public:
  /// @brief Take ownership of a transport and the adapter built over it.
  /// @param wire The transport; must be the one `made` points at.
  /// @param made The adapter.
  /// @param which Which concrete type `made` is.
  /// @param bearer The resolved credential, for the creation path.
  adapter_handle(std::unique_ptr<http::transport> wire, std::unique_ptr<adapter::external_adapter> made, adapter_kind which,
                 std::string bearer)
      : _wire(std::move(wire)), _adapter(std::move(made)), _kind(which), _token(std::move(bearer)) {
  }

  /// @brief The adapter, for dispatch through the abstract interface.
  /// @return The adapter.
  [[nodiscard]] auto instance() const -> const adapter::external_adapter& {
    return *_adapter;
  }
  /// @brief Which concrete adapter this is.
  /// @return The kind.
  [[nodiscard]] auto kind() const -> adapter_kind {
    return _kind;
  }
  /// @brief The raw transport, for the creation path that does not go
  /// through the adapter interface. See this class's header.
  /// @return The transport.
  [[nodiscard]] auto transport() const -> http::transport& {
    return *_wire;
  }
  /// @brief The resolved bearer token, for the creation path's
  /// `Authorization` header.
  ///
  /// May be EMPTY and that is not an error: a `token-env` row whose variable
  /// is set to the empty string resolves successfully — see this module's
  /// header on the two credential sources disagreeing about emptiness.
  /// @return The token.
  [[nodiscard]] auto token() const -> std::string_view {
    return _token;
  }

private:
  std::unique_ptr<http::transport>           _wire;    ///< Destroyed LAST — the adapter points at it.
  std::unique_ptr<adapter::external_adapter> _adapter; ///< The constructed adapter.
  adapter_kind                               _kind;    ///< Which concrete type `_adapter` is.
  std::string                                _token;   ///< The resolved credential.
};

/// @brief The three injected seams `build_adapter` resolves through.
export struct factory_deps {
  env_lookup        env;  ///< Environment lookup for the `token-env` arm.
  token_command     gh;   ///< Credential subprocess for the `gh-cli` arm.
  transport_factory wire; ///< Transport constructor.
};

/// @brief The real seams: process environment, `gh auth token`, libcurl.
/// @param env The context's environment lookup.
/// @return The dependency set.
export auto default_deps(env_lookup env) -> factory_deps;

/// @brief Resolve the credential for `sys` and build its adapter.
///
/// The credential is resolved BEFORE the kind is checked, which is observable
/// on a row that fails both ways — see this module's header.
/// @param sys The registered system row.
/// @param deps The injected seams.
/// @return The owned handle, or why it could not be built.
export auto build_adapter(const engine::external::system::external_system& sys, const factory_deps& deps)
    -> std::expected<std::unique_ptr<adapter_handle>, factory_error>;

/// @brief The operator-facing refusal for `err`, verbatim from the oracle.
///
/// Every arm maps to `invalid_input` (exit 2), including the unsupported
/// KIND, which reads like a not-implemented case but is not one.
/// @param err The failure.
/// @param sys The row it was raised against, for the interpolated values.
/// @return The domain error, message included.
export auto factory_error_message(factory_error err, const engine::external::system::external_system& sys) -> domain_error;

/// @brief What a successful remote creation yielded.
export struct created_remote {
  std::string external_id;  ///< The provider-side id, in Planar's spelling.
  std::string external_url; ///< The ticket URL; EMPTY when the provider gave none.
};

/// @brief POST `payload` to the provider's create endpoint and read back the
/// id it assigned.
///
/// Does NOT go through the adapter interface — see `adapter_handle`'s header.
/// The two providers differ in every part: the URL, the `Accept` header, the
/// response field carrying the id, and how the id is spelled locally (Jira's
/// bare `key`, GitHub's `<project>#<number>`).
///
/// ## Why this is exported rather than TU-local
///
/// It was private to `ext.cpp` while `ext create` was its only caller. Task
/// 6335 added `ext propagate-one` and `workbench publish`, both of which must
/// POST through the IDENTICAL shape — same URL construction, same three
/// headers, same asymmetric id/URL parse. A per-handler copy would drift
/// without any gate noticing: the state differential compares database rows,
/// not request shapes, so two handlers building subtly different URLs would
/// both look correct to it.
///
/// ## The two asymmetries worth not "fixing"
///
///   1. **The id is required, the URL is not.** A 2xx with no `key` (Jira) or
///      no `number` (GitHub) is `ParseFailed`; a missing or non-string
///      `html_url` is the EMPTY STRING and stored as SQL NULL.
///   2. **`trim_slash` trims ONE trailing slash, not all.** `http://h//`
///      keeps a slash. That is observable in the stored `external_url`.
///
/// @param handle The built adapter handle.
/// @param sys The registered system row.
/// @param payload The rendered request body.
/// @return The created ticket, or the Zig error TAG to report.
export auto create_remote(const adapter_handle& handle, const engine::external::system::external_system& sys,
                          std::string_view payload) -> std::expected<created_remote, std::string_view>;

} // namespace planar::cmd::handlers
