-- Downgrade is lossless only while every entity thread is still the migrated
-- one-message page-level form. New replies, edits, or anchors have no legacy
-- representation and therefore refuse rather than discard review history.
-- noqa: disable=PRS
create temp table if not exists m00041_down_guard (
  refusal text
  constraint m00041_down_refused_contextual_or_revised_annotation_thread_present
  check (refusal is null)
);
insert into temp.m00041_down_guard (refusal)
select 'blocked'
where exists (select 1 from annotation_contextual_anchors)
   or exists (
     select 1 from annotation_message_revisions
     group by message_id having count(*) > 1
   )
   or exists (select 1 from annotation_operation_receipts where message_id is not null or message_revision is not null)
   or exists (
     select 1
     from annotations as a
     where a.anchor_kind = 'entity'
       and (select count(*) from annotation_messages as m where m.annotation_id = a.id) <> 1
   )
   or exists (
     select 1
     from annotations as a
     inner join annotation_messages as m on a.id = m.annotation_id
     where a.anchor_kind = 'entity'
       and (m.id <> a.id
         or m.body <> a.body
         or m.revision <> a.revision
         or m.vendor <> a.vendor
         or m.origin is not a.origin
         or m.created_at <> a.created_at
         or m.updated_at <> a.updated_at)
   );
drop table temp.m00041_down_guard;

drop trigger annotation_message_insert;
drop trigger annotation_entity_thread_insert;
drop trigger annotation_legacy_message_update;
drop trigger annotation_anchor_segment_insert;
drop trigger annotation_message_revision;
drop trigger annotation_message_snapshot_insert;
drop trigger annotation_message_snapshot_update;
drop trigger annotation_message_snapshot_immutable;
drop trigger annotation_anchor_revision;
drop trigger annotation_anchor_reanchor_audit;
drop trigger annotation_contextual_anchor_update;
drop trigger annotation_contextual_anchor_insert;
drop table annotation_anchor_segments;
drop table annotation_contextual_anchors;
drop table annotation_message_revisions;
drop table annotation_messages;
alter table annotation_operation_receipts drop column message_revision;
alter table annotation_operation_receipts drop column message_id;
delete from schema_migrations where version = 41;
