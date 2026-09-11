-- SQLite cannot drop a column without rebuilding this receipt table. Downgrade
-- is refused while receipts exist, preserving the replay window. With no
-- receipts, rebuild it so up/down/up returns to the migration-34 schema.
select case when exists (select 1 from annotation_operation_receipts)
  then raise(abort, 'cannot roll back annotation receipt count while receipts exist') end;

create table annotation_operation_receipts_old (
  operation_uuid text primary key,
  source_uuid text not null references annotation_source_identity(source_uuid),
  payload_digest text not null,
  annotation_id integer references annotations(id),
  revision integer,
  outcome text not null,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
insert into annotation_operation_receipts_old (
  operation_uuid, source_uuid, payload_digest, annotation_id, revision, outcome, created_at
) select operation_uuid, source_uuid, payload_digest, annotation_id, revision, outcome, created_at
  from annotation_operation_receipts;
drop table annotation_operation_receipts;
alter table annotation_operation_receipts_old rename to annotation_operation_receipts;

delete from schema_migrations where version = 35;
