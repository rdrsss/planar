# Tech spec — Dispatch lease renewal

## Goal
Keep a long-running agent dispatch alive past the default lease so a slow but
healthy run is not reclaimed mid-flight.

## Design

A dispatched agent renews its lease by calling `renew --claim <token>`. The
server extends `lease_expires_at` by the configured TTL and returns the new
expiry. Renewal is fire-and-forget: the agent does not check the response,
which keeps the hot path cheap.

If a renewal arrives for a claim that has already expired and been reclaimed by
another agent, the server extends it anyway — the renewal proves the original
agent is still alive, so honouring it avoids losing that work.

Renewals are recorded in `lease_events` for audit. The table has no retention
policy; rows accumulate for the life of the database.

The TTL is read from config at process start and cached for the lifetime of the
process, so an operator changing the TTL takes effect on the next restart.

## Acceptance
- An agent that renews within the TTL keeps its claim.
- Renewal latency stays under 50ms at p99.
- The feature works as expected under concurrent load.

## Open questions
None.
