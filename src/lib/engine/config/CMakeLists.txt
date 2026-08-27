# planar_engine_config — engine/config bucket (plan 996, task 6032). Layer-2
# `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1 `db`
# (D15). Behavior-preserving port (D2) of the config-plane, embedded-templates,
# and init-verb subset of zig/src/engine/config.zig + zig/src/engine/config/
# {parse,effective}.zig + zig/src/engine/templates/loader.zig +
# zig/src/engine/init.zig — see each *.cppm's own header comment for exactly
# what is (and, for the models/routing/roles precedence surface and the
# render/validate template surfaces, deliberately is not) ported.
#
# `planar.db.migrations` / `planar.db.migrate`'s precedent (src/lib/db/
# CMakeLists.txt's header comment) is followed here too: `toml`,
# `effective`, `templates_embed`, `templates`, and `init` are five distinct
# C++ module names compiling into this ONE `planar_engine_config` CMake
# target — architecture.cmake's D15 layering classifies every non-cmd_/
# engine_ module as layer 1 / layer 2 by TARGET name, so a separate
# `planar_engine_config_templates_embed` target would sit in the same
# layer as `planar_engine_config` and a same-layer dependency edge between
# them would need the D17 layer-1-cycle exception it doesn't actually
# qualify for (D17 is scoped to layer 1, not layer 2). Multiple distinct
# C++ module names compiling into one static library target is fine — only
# the CMake target graph is what architecture.cmake walks.
#
# `templates_embed.generated.cpp` is generated at configure time by
# cmake/generate_templates.cmake, straight into the build tree (never
# src/) — see that file for the enumeration + explicit (system, kind) sort
# contract. `defaults.toml` (the embedded config defaults) is a single
# fixed file `#embed`'d directly from effective.cpp — no configure-time
# enumeration needed for one file (unlike templates_embed's directory
# walk).

include(generate_templates)

set(_planar_engine_config_templates_generated
    "${CMAKE_CURRENT_BINARY_DIR}/generated/templates_embed.generated.cpp")
planar_generate_templates(
  TEMPLATES_DIR "${CMAKE_SOURCE_DIR}/templates/defaults"
  OUT_FILE "${_planar_engine_config_templates_generated}"
  MODULE_NAME "planar.engine.config.templates_embed")

planar_module(engine_config
  INTERFACE
    toml.cppm
    effective.cppm
    templates_embed.cppm
    templates.cppm
    init.cppm
    config.cppm
  SOURCES
    toml.cpp
    effective.cpp
    templates.cpp
    init.cpp
    "${_planar_engine_config_templates_generated}"
  DEPENDS
    db
    json_text
    # `templates.cpp`'s validity probe. It used `glz::validate_json`, which
    # ACCEPTS duplicate object keys where `std.json` rejects them — a
    # captured divergence on `templates show`/`validate`/`render` (task
    # 6190). `json_dom` is the tree's `std.json`-faithful parser; see
    # src/lib/json_dom/CMakeLists.txt. Glaze is still linked below for
    # `toml.cpp`, which is its real user here.
    json_dom
  EMBED)

target_link_libraries(planar_engine_config PRIVATE glaze::glaze)

if(TARGET planar_engine_config_tests)
  target_link_libraries(planar_engine_config_tests PRIVATE glaze::glaze SQLite::SQLite3)
endif()
