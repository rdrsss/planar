# planar.queuerule — layer-1 home of the host queue's agent rule text (plan
# 1080, task 7110).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1). DEPENDS nothing: one embedded file behind one
# function.
#
# WHY IT EXISTS. Two engine buckets need the rule's bytes: `engine_hostqueue`
# (`planar-agent queue rule`) and `engine_workspace` (the generated workspace
# guide). D15/D18 forbid an `engine_* -> engine_*` edge, and the workaround
# that preceded this module, a `#embed "../hostqueue/queue-rule.md"` from
# `engine_workspace`, was the first embed to cross a bucket boundary: the
# target-graph walk cannot see a file path, so the coupling was invisible to
# it. Moving the one authored file down to layer 1 makes both buckets depend
# on it through a real target edge. `engine_hostqueue`'s
# `planar.engine.hostqueue.rule` forwards here and keeps its API.
#
# EMBED scopes the `-Wno-c23-extensions` `#embed` needs to this target
# (cmake/module.cmake).
planar_module(queuerule
  INTERFACE queuerule.cppm
  SOURCES queuerule.cpp
  EMBED)
