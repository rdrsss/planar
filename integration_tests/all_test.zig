//! integration_tests/all_test.zig — umbrella root for the integration suite.
//!
//! `zig build test-integration` uses this single root so local full-suite
//! runs pay one test-binary compile/link cost. The per-file runner remains
//! available as `zig build test-integration-files` for failure isolation.

comptime {
    _ = @import("agents_render_test.zig");
    _ = @import("capability_boundary_test.zig");
    _ = @import("planar_doc_test.zig");
    _ = @import("planar_execute_test.zig");
    _ = @import("planar_execute_doctor_test.zig");
    _ = @import("planar_execute_agent_live_test.zig");
    _ = @import("planar_execute_eligible_test.zig");
    _ = @import("planar_execute_refusal_guard_test.zig");
    _ = @import("planar_execute_quality_spine_test.zig");
    _ = @import("workbench_terminal_filter_test.zig");
    _ = @import("workbench_gc_test.zig");
    _ = @import("config_test.zig");
    _ = @import("cwd_scope_test.zig");
    _ = @import("dashboard_agents_test.zig");
    _ = @import("json_shape_lint_test.zig");
    _ = @import("list_plan_filter_test.zig");
    _ = @import("editflow_diff_review_test.zig");
    _ = @import("editflow_edit_test.zig");
    _ = @import("editflow_view_test.zig");
    _ = @import("editor_add_test.zig");
    _ = @import("editor_validation_test.zig");
    _ = @import("ext_sync_test.zig");
    _ = @import("local_test.zig");
    _ = @import("m16_test.zig");
    _ = @import("m18_import_synthesize_test.zig");
    _ = @import("parent_issue_test.zig");
    _ = @import("parity_bare_parent_help_test.zig");
    _ = @import("parity_exit_code_not_found_test.zig");
    _ = @import("parity_handoff_json_test.zig");
    _ = @import("parity_health_test.zig");
    _ = @import("parity_help_prose_test.zig");
    _ = @import("parity_intentional_divergence_test.zig");
    _ = @import("parity_plan_next_test.zig");
    _ = @import("parity_resume_test.zig");
    _ = @import("parity_tree_cwd_derive_test.zig");
    _ = @import("parity_tree_render_test.zig");
    _ = @import("plan_next_buckets_test.zig");
    _ = @import("plan_recommend_strategy_test.zig");
    _ = @import("plan_update_test.zig");
    _ = @import("schema_test.zig");
    _ = @import("task_transitions_test.zig");
    _ = @import("entity_view_test.zig");
    _ = @import("handoff_resume_validate_test.zig");
    _ = @import("link_lifecycle_test.zig");
    _ = @import("assoc_lifecycle_test.zig");
    _ = @import("remaining_coverage_test.zig");
    _ = @import("entity_create_feed_test.zig");
    _ = @import("m1_heartbeat_status_integration_test.zig");
    _ = @import("m3_ps_feed_test.zig");
    _ = @import("planar_agent_test.zig");
    _ = @import("planar_agent_ingest_test.zig");
    _ = @import("planar_watch_test.zig");
    _ = @import("scope_test.zig");
    _ = @import("search_test.zig");
    _ = @import("skills_render_test.zig");
    _ = @import("smoke_test.zig");
    _ = @import("spec_ingest_test.zig");
    _ = @import("tree_test.zig");
    _ = @import("tree_audit_activity_test.zig");
    _ = @import("workbench_test.zig");
    _ = @import("workspace_test.zig");

    _ = @import("scenarios/scenario_annotations_test.zig");
    _ = @import("scenarios/scenario_audit_trail_test.zig");
    _ = @import("scenarios/scenario_cross_scope_polyrepo_test.zig");
    _ = @import("scenarios/scenario_decision_workflow_test.zig");
    _ = @import("scenarios/scenario_external_plane_test.zig");
    _ = @import("scenarios/scenario_feature_lifecycle_test.zig");
    _ = @import("scenarios/scenario_handoff_resume_test.zig");
    _ = @import("scenarios/scenario_multi_agent_session_test.zig");
    _ = @import("scenarios/scenario_plan_progression_test.zig");
    _ = @import("scenarios/scenario_promote_demote_test.zig");
    _ = @import("scenarios/scenario_question_lifecycle_test.zig");
    _ = @import("scenarios/scenario_templates_test.zig");
    _ = @import("scenarios/scenario_test_spec_authoring_test.zig");
    _ = @import("scenarios/scenario_workbench_sync_test.zig");
}
