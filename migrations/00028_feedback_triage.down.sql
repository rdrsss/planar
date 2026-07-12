delete from schema_migrations where version = 28;

drop index ix_feedback_triage_filters;
drop index ux_feedback_triage_question;
drop index ux_feedback_triage_task;
drop table feedback_triage;
