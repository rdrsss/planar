/// @file runner.cpp
/// @brief Implementation of `planar.process.runner`.

module planar.process.runner;

import std;

namespace planar::process::runner {

auto resolve(const env_lookup& /*env*/, std::string_view /*program*/) -> std::expected<std::string, error> {
  return std::unexpected{error::spawn_failed};
}

auto start(const env_lookup& /*env*/, std::span<const std::string> /*argv*/, const start_options& /*options*/)
    -> std::expected<child, error> {
  return std::unexpected{error::spawn_failed};
}

auto poll(const child& /*target*/) -> std::expected<status, error> {
  return std::unexpected{error::wait_failed};
}

auto signal(const child& /*target*/, int /*sig*/) -> std::expected<void, error> {
  return std::unexpected{error::signal_failed};
}

} // namespace planar::process::runner
