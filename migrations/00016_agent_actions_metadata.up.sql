-- agent_actions metadata: free-form JSON-shaped text column on the
-- agent_actions row. Nullable. Stores per-action context that the
-- caller wants durably attached to the action without claiming a
-- dedicated column. Primary first consumer is the orchestrator
-- strategy-persistence model (plan 297 task 2905) which writes a
-- `{"strategy":"<name>","axes":{...},"dispatch_shape":"<shape>",
-- "rationale":"<text>"}` blob on the dispatch row so the next cycle's
-- strategy gate can read the prior cycle's choice and apply the
-- recommendation algorithm's stickiness rule. Engine and CLI treat
-- the column as opaque text; only the orchestrator gate parses it.
-- The text is validated as well-formed JSON at the CLI parse layer
-- (planar-agent --metadata) but not at the schema layer — keeping the
-- type as TEXT means we can write probe / debug values that aren't
-- JSON when needed.
alter table agent_actions add column metadata text;

insert into schema_migrations (version, description) values (16, 'agent_actions metadata: nullable JSON-shaped text column for caller-attached per-action context');
