/// @file hostqueue.cppm
/// @brief `planar.engine.hostqueue` — umbrella re-export of the host-wide
/// build and test queue engine (plan 1080). One import point for the whole
/// bucket, matching `planar.engine.closure` and `planar.engine.identity`.
/// The bucket imports only `src/lib/` modules; configuration and process
/// identity are passed in as values by the command handlers.

module;

export module planar.engine.hostqueue;

export import planar.engine.hostqueue.queue;
export import planar.engine.hostqueue.history;
export import planar.engine.hostqueue.liveness;
export import planar.engine.hostqueue.poll;
export import planar.engine.hostqueue.terminate;
export import planar.engine.hostqueue.nested;
