# planar_engine_hostqueue — engine/hostqueue bucket (plan 1080, task
# hq-enqueue): the host-wide build and test queue's engine over the agent
# database (decision 1181).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18). Tech spec 647 § Components makes that explicit for this bucket:
# it does not import `engine_config`; the `queue` handlers read the `[queue]`
# table and pass slot counts and intervals in as arguments, and process
# identity (host id, pid, start time) and both clocks arrive as plain values
# too, so the engine is testable with a scratch store and controlled
# identities.
#
# `db` supplies the connection wrapper and, through the same target, the
# agent database's open path (`planar.db.agentdb`) the tests use to create a
# scratch store at head. `json_text` is the one shared RFC 8259 string
# escape used to write the `argv` and `cancelled_by` columns; `json_dom`
# parses them back.
#
# `history.cppm` (task hq-history) ends entries and prunes history; it
# imports `queue.cppm` for the shared error and argv types, and `queue.cpp`
# imports it back for the pruning enqueue (an implementation-unit import,
# so the interfaces form no cycle).
#
# `process` supplies `planar.process.identity`, which the liveness
# module's default probe forwards to (task hq-liveness); tests replace the
# probe with a fake.
#
# `poll.cppm` (task hq-poll-transaction) is the poll transaction; it
# imports queue, history and liveness from this bucket.
#
# `hostqueue.cppm` is the umbrella re-export, matching
# `src/engine/closure/closure.cppm`.

planar_module(engine_hostqueue
  INTERFACE
    queue.cppm
    history.cppm
    liveness.cppm
    poll.cppm
    hostqueue.cppm
  SOURCES
    queue.cpp
    history.cpp
    liveness.cpp
    poll.cpp
  DEPENDS
    db
    json_dom
    json_text
    process)

if(TARGET planar_engine_hostqueue_tests)
  # The test binary opens real on-disk scratch stores through
  # planar.db.agentdb and probes the schema with raw SQL, exactly as
  # engine/closure's suite does.
  target_link_libraries(planar_engine_hostqueue_tests PRIVATE SQLite::SQLite3)
endif()
