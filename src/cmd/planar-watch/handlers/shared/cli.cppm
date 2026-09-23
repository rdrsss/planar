/// @file cli.cppm
/// @brief Shared flag declarations for the planar-watch command families.
module;
export module planar.cmd.planar_watch.handlers.shared.cli;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
namespace planar::cmd::watch::handlers::shared {
/// @brief Register this CLI declaration.
/// @param app Input app.
export auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}

/// @brief The `--follow` / `--interval` pair the streaming verbs declare.
///
/// DECLARED but REFUSED at the handler — see
/// `planar.cmd.planar_watch.handlers.ledger`'s header for why a loud exit
/// 64 beats a silent single-shot. They are declared anyway because
/// `src/cmd/catalog_parity.hpp` compares this tree's flag set against the
/// oracle's, and a flag missing from the declaration is exactly the
/// transcription slip that comparison exists to catch.
/// @param app The node to declare them on.
/// @param follow_desc The verb's own wording for `--follow`.
/// @param interval_desc The verb's own wording for `--interval`.
export auto add_follow(CLI::App& app, std::string follow_desc, std::string interval_desc) -> void {
  cliapp::add_bool_flag(app, "--follow", follow_desc);
  app.add_option("--interval")->description(std::move(interval_desc));
}

/// @brief The `--vendor` flag, whose wording differs between verbs.
/// @param app The node to declare it on.
/// @param desc The verb's own wording.
export auto add_vendor(CLI::App& app, std::string desc) -> void {
  app.add_option("--vendor")->description(std::move(desc));
}

/// @brief An integer-valued option, through the shared Zig `parseInt`
/// validator so `--plan 1_0` means plan 10 here exactly as it does on the
/// reference binary.
/// @param app The node to declare it on.
/// @param name The flag's long name.
/// @param desc The description.
export auto add_int(CLI::App& app, std::string name, std::string desc) -> void {
  app.add_option(std::move(name))->description(std::move(desc))->check(cliapp::zig_int_validator());
}

} // namespace planar::cmd::watch::handlers::shared
