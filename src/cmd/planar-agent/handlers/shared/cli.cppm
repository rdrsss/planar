/// @file cli.cppm
/// @brief Shared CLI option declarations for planar-agent command families.
module;
export module planar.cmd.planar_agent.handlers.shared.cli;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
namespace planar::cmd::agent::handlers::shared {
export auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}

/// @brief The `--claim <token>` flag the eight token-addressed verbs carry.
/// @param app The node to declare it on.
/// @param desc The verb's own wording for it — these genuinely differ
/// ("returned by pull/claim", "to refresh", "to force-release"...).
export auto add_claim(CLI::App& app, std::string desc) -> void {
  app.add_option("--claim")->description(std::move(desc))->required();
}

/// @brief The closed failure taxonomy shared by `fail`, `abort` and
/// `reconcile` — the same six values in the same order in all three.
///
/// Enforced by CLI11's `IsMember` validator, which also renders the set
/// into the option's type name; `planar.cliapp.schema` reads it back out
/// of there, so the catalog still reports the choice set.
/// @return The choice set.
export auto failure_categories() -> std::vector<std::string> {
  return {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"};
}

/// @brief The `--no-locality-probe` flag.
/// @param app The node to declare it on.
/// @param desc The verb's own wording (the terminal verbs mention commit
/// collection; the acquisition verbs do not).
export auto add_no_locality_probe(CLI::App& app, std::string desc) -> void {
  cliapp::add_bool_flag(app, "--no-locality-probe", desc);
}

/// @brief The `--ttl` flag, whose wording differs by one word between the
/// acquisition verbs ("Lease TTL") and `heartbeat` ("New TTL").
///
/// Declared as a plain string with a DEFAULT, not as an integer: the value
/// is a duration (`600`, `10m`, `500ms`) that
/// `planar.cmd.planar_agent.args::parse_ttl_seconds` interprets. The
/// default string is load-bearing beyond help — `planar.cliapp.args::
/// harvest` materializes a declared default into the parsed result, so an
/// omitted `--ttl` still reaches the handler as "600".
/// @param app The node to declare it on.
/// @param desc The verb's own wording.
export auto add_ttl(CLI::App& app, std::string desc) -> void {
  app.add_option("--ttl")->description(std::move(desc))->default_str("600");
}

/// @brief The engine-supervision flags (plan 1033 D3/D4, task 6488).
///
/// `--as engine --attempt <id>` is how the engine supervisor identifies
/// itself on a claim it has been handed with `claim-associate --supervisor
/// engine`. Absent, the verb acts as the caller, exactly as before plan
/// 1033. `--override-supervisor` (terminal verbs only) is operator recovery:
/// a caller terminal verb on an engine claim, logged as a
/// `supervisor_override` action.
/// @param app The verb.
/// @param terminal Whether to add `--override-supervisor`.
export auto add_supervision(CLI::App& app, bool terminal) -> void {
  app.add_option("--as")
      ->description("Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)")
      ->check(CLI::IsMember{std::vector<std::string>{"caller", "engine"}});
  app.add_option("--attempt")
      ->description("The Centurion attempt the engine acts for; must match the claim's associated attempt");
  if (terminal) {
    cliapp::add_bool_flag(
        app, "--override-supervisor",
        "Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)");
  }
}

/// @brief The `heartbeat` verb's `--ttl`, declared WITHOUT a default.
///
/// Deliberately not `add_ttl`. That helper's `default_str("600")` is
/// load-bearing (see above): `harvest` materializes it, so an omitted
/// `--ttl` would reach the handler as "600" and be indistinguishable from
/// an explicit `--ttl 600`. For `heartbeat` that distinction IS the
/// contract — omitting `--ttl` renews the lease length the claim already
/// holds, rather than resetting it to a fixed default and truncating every
/// long lease (Planar task 6093). Leaving the default off is what lets the
/// handler see `std::nullopt`.
/// @param app The node to declare it on.
export auto add_heartbeat_ttl(CLI::App& app) -> void {
  app.add_option("--ttl")->description(
      "Set a new TTL absolutely (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms). "
      "When omitted, the claim's CURRENT lease length is renewed from now — a heartbeat never "
      "shortens the lease it was sent to preserve.");
}

/// @brief The `--vendor` / `--vendor-session` pair.
/// @param app The node to declare them on.
/// @param vendor_desc The verb's wording for `--vendor`.
/// @param session_desc The verb's wording for `--vendor-session`.
export auto add_vendor(CLI::App& app, std::string vendor_desc, std::string session_desc) -> void {
  app.add_option("--vendor")->description(std::move(vendor_desc))->default_str("planar-agent");
  app.add_option("--vendor-session")->description(std::move(session_desc));
}

/// @brief The `--run` / `--stage` pair carried by `pull` and `claim`
/// (NOT by `claim-associate`, whose versions are required and worded
/// differently).
/// @param app The node to declare them on.
export auto add_run_stage(CLI::App& app) -> void {
  app.add_option("--run")
      ->description("workflow_runs.id to associate with this claim (populated by an external workflow harness; omit "
                    "for interactive claims)")
      ->check(cliapp::zig_int_validator());
  app.add_option("--stage")->description("Workflow stage name (e.g. code, review) to record on the claim; requires --run");
}

} // namespace planar::cmd::agent::handlers::shared
