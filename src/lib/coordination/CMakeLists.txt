# planar.coordination — layer-1 home of the predicates that decide whether a
# task is held up by something other than a claim: an unfinished dependency,
# an open question, a proposed decision (plan 1142, task 7477).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1). DEPENDS `db` only.
#
# WHY IT EXISTS. `engine_ingest` (the task packet's `ready` and its reasons)
# and `engine_planning` (`recommend-strategy` rules 5 and 6) must agree on what
# "an open question is linked" means. D15/D18 forbid an `engine_* -> engine_*`
# edge, so neither bucket can import the other's predicate. Each reaches this
# module through a target edge the architecture walk can see, so the two
# verdicts cannot drift apart.
planar_module(coordination
  INTERFACE coordination.cppm
  SOURCES coordination.cpp
  DEPENDS db)
