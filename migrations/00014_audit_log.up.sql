-- 00014_audit_log.up.sql
--
-- Add audit_log table — append-only trail of every data-plane mutation.
-- Underpins `planar audit trail`, session capture correlation, and the
-- ext-sync reconciliation path. One row per mutation; CRUD functions
-- in engine.planning, engine.runtime, engine.workbench call
-- engine.policy.audit.record at the end of every mutation.


-- ============================================================
-- audit_log: append-only mutation trail
-- ============================================================

-- audit_log: one row per data-plane mutation. The `verb` column is a
-- closed enum (CHECK-constrained) covering the major mutation shapes.
-- `entity_kind` is free-text rather than an enum so new entity kinds
-- can be added without a schema change; the canonical strings live in
-- src-zig/src/engine/policy/audit.zig.
create table audit_log (
  id           integer primary key autoincrement,
  verb         text not null check(verb in (
    'create', 'update', 'delete', 'status_change', 'link', 'unlink'
  )),
  entity_kind  text not null,
  entity_id    integer not null,
  actor        text,
  scope        text,
  summary      text,
  recorded_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

create index ix_audit_log_entity      on audit_log(entity_kind, entity_id);
create index ix_audit_log_recorded_at on audit_log(recorded_at);
create index ix_audit_log_scope       on audit_log(scope)
  where scope is not null;
create index ix_audit_log_actor       on audit_log(actor)
  where actor is not null;


-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (14, 'audit_log: append-only data-plane mutation trail');
