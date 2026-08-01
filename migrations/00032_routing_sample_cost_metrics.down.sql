drop index if exists ix_routing_terminal_samples_measured;

alter table routing_terminal_samples drop column total_tokens;
alter table routing_terminal_samples drop column cost_micros;
alter table routing_terminal_samples drop column latency_ms;

delete from schema_migrations where version = 32;
