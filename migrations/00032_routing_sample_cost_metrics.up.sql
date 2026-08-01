-- Optional comparable cost metrics on a terminal sample (plan 950 task 5530,
-- criterion 2 — deferred until now because the columns did not exist).
--
-- All three are NULLABLE on purpose. A sample recorded before a host reported
-- these, or by a host that cannot report them, genuinely has no value — and a
-- zero would be a lie in the worst direction: it would rank an unmeasured
-- candidate as instant and free, which is exactly the ordering error the
-- quality floor exists to prevent. Ranking therefore compares these only when
-- BOTH sides carry them, and reports them as unavailable otherwise.
--
-- No CHECK ties them together: a host may know latency but not cost, and
-- requiring all-or-nothing would discard the half it does know.
alter table routing_terminal_samples add column latency_ms integer
  check (latency_ms is null or latency_ms >= 0);

alter table routing_terminal_samples add column cost_micros integer
  check (cost_micros is null or cost_micros >= 0);

-- Total tokens billed, kept separate from cost because a price change would
-- otherwise silently re-scale historical samples.
alter table routing_terminal_samples add column total_tokens integer
  check (total_tokens is null or total_tokens >= 0);

create index ix_routing_terminal_samples_measured
on routing_terminal_samples (experiment_id)
where latency_ms is not null or cost_micros is not null;

insert into schema_migrations (version, description)
values (32, 'optional comparable latency and cost metrics on terminal samples');
