/// @file selector.cpp
/// @brief Implementation of `planar.cmd.planar_execute.selector`.

module;

#include <glaze/glaze.hpp>

module planar.cmd.planar_execute.selector;

import std;
import planar.engine_execute;
import planar.cmd.planar_execute.profile;

namespace planar::cmd::execute {

// Named rather than anonymous: Glaze's reflection needs the type to have
// linkage (same shape as `engine/local`'s `wire`).
namespace wire {

/// @brief `profile show --json`'s document. Member order is key order.
struct engine_document {
  std::string engine;        ///< `embedded` or `centurion`.
  std::string engine_source; ///< The provenance that chose it.
};

} // namespace wire

namespace {

/// @brief The env var the selector reads, and its provenance label — the
/// same label `planar config show` prints for it.
constexpr std::string_view k_env_var = "PLANAR_EXECUTE_ENGINE";

} // namespace

auto engine_name(engine_kind engine) -> std::string_view {
  switch (engine) {
  case engine_kind::embedded:
    return "embedded";
  case engine_kind::centurion:
    return "centurion";
  }
  return "embedded";
}

auto parse_engine(std::string_view text) -> std::optional<engine_kind> {
  if (text == "embedded") {
    return engine_kind::embedded;
  }
  if (text == "centurion") {
    return engine_kind::centurion;
  }
  return std::nullopt;
}

auto resolve_engine(std::optional<std::string_view> flag, std::optional<std::string_view> env, config_reader const& read_config)
    -> std::expected<engine_choice, std::string> {
  if (flag.has_value()) {
    // parse_run_args already refused an unknown value with the usage text;
    // re-checking here keeps this function total on its own inputs.
    if (auto const engine = parse_engine(*flag)) {
      return engine_choice{.engine = *engine, .source = "flag: --engine"};
    }
    return std::unexpected{std::format("--engine must be embedded or centurion, got: {}", *flag)};
  }
  if (env.has_value() && !env->empty()) {
    if (auto const engine = parse_engine(*env)) {
      return engine_choice{.engine = *engine, .source = std::format("env: {}", k_env_var)};
    }
    return std::unexpected{std::format("{} must be embedded or centurion, got: {}", k_env_var, *env)};
  }
  auto const configured = read_config();
  if (!configured.has_value()) {
    return std::unexpected{std::format("cannot read execute.engine from planar config: {}", configured.error())};
  }
  if (!configured->has_value()) {
    // A sibling `planar` older than the key: the documented default.
    return engine_choice{.engine = engine_kind::embedded, .source = "embedded default"};
  }
  auto const& entry = **configured;
  if (auto const engine = parse_engine(entry.value)) {
    return engine_choice{.engine = *engine, .source = entry.provenance};
  }
  return std::unexpected{
      std::format("execute.engine must be embedded or centurion, got: {} ({})", entry.value, entry.provenance)};
}

auto sibling_config_reader(std::string bin_dir) -> config_reader {
  return [bin_dir = std::move(bin_dir)] { return engine::execute::read_planar_config(bin_dir, "execute.engine"); };
}

auto entries_config_reader(std::span<const engine::execute::config_entry> entries) -> config_reader {
  return [entries]() -> std::expected<std::optional<engine::execute::config_value>, std::string> {
    for (auto const& entry : entries) {
      if (entry.key == "execute.engine") {
        return engine::execute::config_value{.value = entry.value, .provenance = entry.provenance};
      }
    }
    return std::optional<engine::execute::config_value>{};
  };
}

auto render_profile(engine_choice const& choice, const profile& resolved, bool json) -> std::string {
  if (!json) {
    return std::format("engine: {}\nengine_source: {}\n", engine_name(choice.engine), choice.source) + profile_text(resolved);
  }
  wire::engine_document const doc{.engine = std::string{engine_name(choice.engine)}, .engine_source = choice.source};
  std::string                 out;
  if (glz::write_json(doc, out) || !out.ends_with('}')) {
    // Two plain strings cannot fail to serialize; if Glaze ever says
    // otherwise, an empty object is still a parseable document.
    return "{}\n";
  }
  // `{"engine":…,"engine_source":…}` with the profile object spliced in as
  // its last member.
  out.pop_back();
  out += ",\"profile\":";
  out += profile_json(resolved);
  out += "}\n";
  return out;
}

} // namespace planar::cmd::execute
