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
# `terminate.cppm` (task hq-terminate) stops a running entry's command in
# two steps, signalling only outside any transaction; it imports queue,
# history, liveness and poll from this bucket (poll for `poll_and_stop`,
# task hq-orphan-deadline, which signals what a poll marked after it
# commits). poll does not import terminate, so the interfaces form no cycle.
#
# `nested.cppm` (task hq-nested-entry) inserts a nested run's entry after
# checking its parent; it imports queue and liveness from this bucket.
#
# `guard.cppm` (task hq-command-guard) is the pure command guard: it imports
# nothing but the standard library.
#
# `hostqueue.cppm` is the umbrella re-export, matching
# `src/engine/closure/closure.cppm`.

planar_module(engine_hostqueue
  INTERFACE
    queue.cppm
    history.cppm
    liveness.cppm
    poll.cppm
    terminate.cppm
    nested.cppm
    guard.cppm
    hostqueue.cppm
  SOURCES
    queue.cpp
    history.cpp
    liveness.cpp
    poll.cpp
    terminate.cpp
    nested.cpp
    guard.cpp
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
