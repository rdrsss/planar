---
role: fixture
---
# Direct build fixture

Span: `make test` is run directly.

```sh
ctest --test-dir build
```

  ```
  $ cmake --build build/debug
  ```

Two on one line: `ninja` and `cargo test --release`.

Queued: `planar-agent queue run -- make test` is fine too.

Former exemption markers are plain comments, balanced or not:

<!-- queue-lint-ignore-end -->
`gradle build` after a stray end marker.
<!-- queue-lint-ignore-begin -->
`tox` inside a region that is never closed.
