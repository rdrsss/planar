// @file vendor_probe.cpp
// @brief Task 6023 smoke probe: proves glaze, spdlog, and the vendored
// SQLite amalgamation are consumable from a first-party target under the
// same warnings-as-errors setting planar_module() applies (PLANAR_WARNINGS_
// AS_ERRORS), and that CPM's `SYSTEM YES` keeps their own headers exempt
// from it (cmake/dependencies.cmake). Not a planar_module() target — it is
// deliberately excluded from PLANAR_MODULE_TARGETS (cmake/architecture.cmake
// only walks planar_* targets) since it exists purely to prove the vendored
// dependencies compile and link, not to be a real first-party module.
//
// Traditional #include, not `import` — glaze, spdlog, and the SQLite
// amalgamation are not C++ modules; module.cmake's CXX_MODULE_STD opt-in is
// per-target and not required here.
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstdio>
#include <string>

// Deliberately NOT in an anonymous namespace: glaze's compile-time
// reflection (glz::write_json) needs external linkage on the reflected
// type to compute stable field names via `extern const T external;`
// (glaze/reflection/get_name.hpp) — an anonymous-namespace type fails to
// compile with "used but not defined ... cannot be defined in any other
// translation unit because its type does not have linkage".
namespace planar::core::probe {

struct probe_payload {
  std::string message{};
  int value{0};
};

}  // namespace planar::core::probe

// @brief Exercises glaze's compile-time reflection, spdlog's logger
// construction, and the SQLite amalgamation's version accessor, then
// returns 0 on success. Deliberately not wired into ctest — cycle 3's
// version.t.cpp (Catch2) is the test-discovery proof; this binary is the
// build/link consumability proof (task 6023's scope item 4).
auto main() -> int {
  planar::core::probe::probe_payload payload{.message = "planar-vendor-probe", .value = 6023};
  const std::string json = glz::write_json(payload).value_or("");
  if (json.empty()) {
    return 1;
  }

  auto logger = spdlog::default_logger();
  if (!logger) {
    return 1;
  }
  logger->info("vendor probe: glaze json = {}", json);
  logger->info("vendor probe: sqlite version = {}", sqlite3_libversion());

  if (std::string_view{sqlite3_libversion()}.empty()) {
    return 1;
  }

  std::printf("vendor probe ok: %s\n", json.c_str());
  return 0;
}
