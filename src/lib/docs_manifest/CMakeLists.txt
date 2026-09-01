# planar.docs_manifest — layer-1 `.manifest-docs` xxh64 merkle digest (plan
# 996, task 6364).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not
# `cmd_*`/`engine_*` as layer 1), same as `json_text` / `scope_ref`. D15/D18
# forbid an `engine_* -> engine_*` edge, so this is where a hash utility two
# engine buckets might eventually share has to live — see this module's own
# header for the full argument. One consumer today: `engine_workspace`'s
# `regenerate()`.
#
# DEPENDS json_text (layer 1 -> layer 1, the same shape `scope_ref` -> `db`
# already uses): the written `.manifest-docs` JSON needs the identical
# hand-rolled string-escape table every other oracle-pinned renderer in this
# tree shares.
#
# LINKS xxHash::xxhash (layer 0, cmake/dependencies.cmake) PRIVATE: xxh64 is
# an implementation detail of `hash_hex`, not part of this module's exported
# interface.

planar_module(docs_manifest
  INTERFACE docs_manifest.cppm
  SOURCES docs_manifest.cpp
  DEPENDS json_text)

target_link_libraries(planar_docs_manifest PRIVATE xxHash::xxhash)

# `docs_manifest.t.cpp` `#embed`s its oracle-captured `AGENTS.md` fixture
# directly (the production module itself has no `#embed` — only the TEST
# does, to load the fixture byte-for-byte rather than retyping it). The
# `EMBED` option on `planar_module()` above only reaches the PRODUCTION
# target; the `_tests` target needs the identical `-Wno-c23-extensions`
# suppression by hand. See cmake/module.cmake's own comment for why the
# warning fires at all under this toolchain.
if(TARGET planar_docs_manifest_tests)
  target_compile_options(planar_docs_manifest_tests PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-c23-extensions>)
endif()
