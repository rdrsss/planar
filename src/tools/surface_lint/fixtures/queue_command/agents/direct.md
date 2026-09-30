---
role: fixture
---
# Queue rule fixture

Span: `make test` is run directly.

```sh
ctest --test-dir build
```

  ```
  $ cmake --build build/debug
  ```

Two on one line: `ninja` and `cargo test --release`.

Queued: `planar-agent queue run -- make test` and

```
planar-agent queue run --detach --vendor claude --role coder -- ctest
```

Prose about ctest and make test outside any span passes, and so do `cmake --preset debug`, `make: *** Error 1`, `go vet ./...` and `npm install`.

Marked: `make cpp-lint` names a failure signature. queue-lint-ignore

<!-- queue-lint-ignore-begin: a listing of program names -->
Examples: `make`, `pytest`.
```
tox
```
<!-- queue-lint-ignore-end -->

After the region: `gradle build` fires again.
