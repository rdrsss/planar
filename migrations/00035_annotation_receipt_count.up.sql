alter table annotation_operation_receipts add column affected_count integer;

insert into schema_migrations (version, description)
values (35, 'annotations: durable bulk receipt affected count');
