/// @file declare.cpp
/// @brief Implementation of `planar.cmd.planar.declare`.

module planar.cmd.planar.declare;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd {

auto add_json(CLI::App& app, std::string_view desc) -> void {
  cliapp::add_bool_flag(app, "--json", desc);
}

auto add_bool(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  cliapp::add_bool_flag(app, name, desc);
}

auto add_bool_default_true(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  cliapp::add_bool_flag(app, name, desc)->default_str("true");
}

auto add_string(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name});
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
}

auto add_string_default(CLI::App& app, std::string_view name, std::string_view default_value, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name});
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
  opt->default_str(std::string{default_value});
}

auto add_string_required(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name});
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
  opt->required();
}

auto add_string_list(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name})->expected(1, -1);
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
}

auto add_int(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name})->check(cliapp::zig_int_validator());
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
}

auto add_int_default(CLI::App& app, std::string_view name, std::string_view default_value, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name})->check(cliapp::zig_int_validator());
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
  opt->default_str(std::string{default_value});
}

auto add_int_required(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name})->check(cliapp::zig_int_validator());
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
  opt->required();
}

auto add_positional(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name});
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
  opt->required();
}

auto add_positional_optional(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  CLI::Option* opt = app.add_option(std::string{name});
  if (!desc.empty()) {
    opt->description(std::string{desc});
  }
}

auto set_allow_extras(CLI::App& app) -> void {
  app.allow_extras();
}

} // namespace planar::cmd
