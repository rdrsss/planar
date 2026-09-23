/// @file environment.cpp
/// @brief Shared command environment implementation.
module;
#include <cstdlib>
module planar.cmd.internal.environment;
import std;

namespace planar::cmd::internal {
auto process_env() -> env_lookup {
  return [](std::string_view name) -> std::optional<std::string> {
    std::string const owned(name);
    char const*       value = std::getenv(owned.c_str());
    return value == nullptr ? std::nullopt : std::optional{std::string{value}};
  };
}

auto map_env(std::map<std::string, std::string, std::less<>> vars) -> env_lookup {
  return [table = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto const it = table.find(name);
    return it == table.end() ? std::nullopt : std::optional{it->second};
  };
}

auto resolve_db_path(const env_lookup& env) -> std::optional<std::filesystem::path> {
  if (auto const explicit_path = env("PLANAR_DB"); explicit_path.has_value()) {
    return std::filesystem::path{*explicit_path};
  }
  if (auto const home = env("HOME"); home.has_value()) {
    return std::filesystem::path{*home} / ".planar" / "planar.db";
  }
  return std::nullopt;
}

auto operator_cwd(const env_lookup& env, cwd_policy policy) -> std::filesystem::path {
  if (policy == cwd_policy::pwd_first) {
    if (auto const pwd = env("PWD"); pwd.has_value() && !pwd->empty()) {
      return std::filesystem::path{*pwd};
    }
    std::error_code ec;
    auto const      here = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : here;
  }

  std::error_code ec;
  auto const      here = std::filesystem::current_path(ec);
  if (ec) {
    return {};
  }
  std::error_code real_ec;
  auto const      real_here = std::filesystem::canonical(here, real_ec);
  auto const      identity  = real_ec ? here : real_here;
  if (auto const pwd = env("PWD"); pwd.has_value() && !pwd->empty()) {
    std::filesystem::path const candidate{*pwd};
    if (candidate.is_absolute()) {
      std::error_code pwd_ec;
      auto const      real_pwd = std::filesystem::canonical(candidate, pwd_ec);
      if (!pwd_ec && real_pwd == identity) {
        return candidate;
      }
    }
  }
  return identity;
}
} // namespace planar::cmd::internal
