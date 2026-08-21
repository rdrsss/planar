# planar.core — M0 scaffold seed module (plan 996, task cpp-scaffold-presets).
#
# Deliberately no *.t.cpp yet: Catch2 is not vendored until task 6023
# (cmake/dependencies.cmake). cmake/module.cmake's planar_module() degrades
# gracefully when *.t.cpp files exist without Catch2 present, but this
# module simply has none yet rather than shipping a placeholder test file
# that would need rewriting the moment Catch2 lands — add version.t.cpp in
# task 6023's cycle instead.

planar_module(core
  INTERFACE version.cppm)
