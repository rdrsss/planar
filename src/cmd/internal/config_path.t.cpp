/// @file config_path.t.cpp
/// @brief Config-path precedence shared by the operator and external binary.
#include <catch2/catch_test_macros.hpp>
import std;
import planar.cmd.internal.environment;
import planar.cmd.internal.config_path;

TEST_CASE("config path resolves explicit and home-relative forms", "[cmd][internal][config]") {
  namespace ci = planar::cmd::internal;
  CHECK(ci::resolve_config_path(ci::map_env({{"HOME", "/home/operator"}})) ==
        std::filesystem::path{"/home/operator/.planar/config.toml"});
  CHECK(ci::resolve_config_path(ci::map_env({{"HOME", "/home/operator"}, {"PLANAR_CONFIG_PATH", "~/custom.toml"}})) ==
        std::filesystem::path{"/home/operator/custom.toml"});
  CHECK(ci::resolve_config_path(ci::map_env({{"PLANAR_CONFIG_PATH", "/scratch/config.toml"}})) ==
        std::filesystem::path{"/scratch/config.toml"});
  CHECK_FALSE(ci::resolve_config_path(ci::map_env({{"PLANAR_CONFIG_PATH", "~/missing-home.toml"}})).has_value());
}
