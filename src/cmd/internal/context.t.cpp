/// @file context.t.cpp
/// @brief The shared context only holds an injected database object.
#include <catch2/catch_test_macros.hpp>
import std;
import planar.cmd.internal.context;
import planar.cmd.internal.environment;

namespace {
struct probe_database {
  std::filesystem::path path_;
  int                   opens = 0;
  [[nodiscard]] auto    path() const -> const std::filesystem::path& {
    return path_;
  }
  auto open() -> void {
    ++opens;
  }
};
} // namespace

/// @brief Context retains the injected object without performing database work.
TEST_CASE("command context retains the caller's database object without opening it", "[cmd][internal][context]") {
  auto                           database = std::make_shared<probe_database>(std::filesystem::path{"/tmp/injected.db"});
  std::ostringstream             out;
  std::ostringstream             err;
  planar::cmd::internal::context ctx{{"planar"}, planar::cmd::internal::map_env({}), "/tmp", database, out, err};

  CHECK(&ctx.db() == database.get());
  CHECK(ctx.db_path() == database->path());
  CHECK(database->opens == 0);
  ctx.db().open();
  CHECK(database->opens == 1);
}
