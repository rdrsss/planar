# Security Policy

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it privately
through GitHub's
[private vulnerability reporting](../../security/advisories/new) on this
repository.

Include the Planar version (`planar version`), your platform, and the steps or
input that reproduce the problem. This is a personal project maintained as
time allows, so there is no response-time commitment, but reports are read and
taken seriously.

## Scope

Planar is a local command-line tool. It runs as your user, reads and writes a
local SQLite database, and shells out to programs you have installed. Reports
about the following are in scope:

- a way for crafted input (a plan or document, a database file, a template,
  an external-system response) to cause code execution, or reads or writes
  outside the paths Planar is meant to touch;
- a bypass of the write boundaries between the binaries (for example the
  `sqlite3_set_authorizer` allowlist that confines `planar-ext`);
- credentials or other sensitive data written to logs, the database, or
  generated files.

Vulnerabilities in vendored third-party code under `vendor/` are best reported
to the upstream project; please also tell us so the pin can be bumped.

## Supported versions

Only the latest release and the current `master` branch receive fixes.
