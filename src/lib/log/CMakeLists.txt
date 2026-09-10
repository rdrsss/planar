# planar.log — spdlog-backed text/JSON dual-mode logging setup (plan 996,
# task cpp-cli-output-logging, D11). Layer-1 base library: no DEPENDS on
# engine/cmd (D15); depends on nothing else in-tree (layer-1 modules may
# depend on each other per the D17 exception in cmake/architecture.cmake,
# but this module has no such need yet).

planar_module(log
  INTERFACE log.cppm
  SOURCES log.cpp)

target_link_libraries(planar_log PUBLIC spdlog::spdlog glaze::glaze)

if(TARGET planar_log_tests)
  target_link_libraries(planar_log_tests PRIVATE spdlog::spdlog glaze::glaze)
endif()
