## Summary

<!-- What changes and why. Link the Planar plan/task (e.g. plan 1065, task 6936) when there is one. -->

## Verification

<!-- What you ran and what it reported. Paste the ctest count. A filtered or partial run says so. -->

- [ ] `make test-all` passes, or the gates not run are listed above with a reason
- [ ] New or changed CLI verbs, flags, JSON shapes, or exit codes have tests that exercise them in a realistic workflow
- [ ] Bug fixes land a red test first ("Red test: …" commit), then the fix
- [ ] Reference docs under `docs/` are updated in the same change as any behaviour, schema, or architecture change
- [ ] A new migration follows the Schema Change checklist in `CLAUDE.md` (reversible pair, `schema_migrations` row, `docs/architecture.md` updated)
- [ ] A new external tool dependency is added to both `README.md` § Prerequisites and `install.sh` `BUILD_DEPS` / `RUN_DEPS`

## Developer Certificate of Origin

- [ ] Every commit carries a `Signed-off-by:` line (`git commit -s`), certifying the
      [Developer Certificate of Origin 1.1](https://developercertificate.org) as described in
      [CONTRIBUTING.md](https://github.com/rdrsss/planar/blob/master/CONTRIBUTING.md)
