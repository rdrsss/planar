"""Exact clean retained continuation gate; no auth reads or publication."""
from datetime import datetime, timedelta, timezone
import os
import re


def validate(gate, snapshot, policy, execution_id, deadline, auth_path, account_scope, verifier_image, *, now=None):
    if not isinstance(gate,dict) or not re.fullmatch(r'planar-t044-[0-9a-f]{32}',execution_id or ''):
        raise ValueError('invalid retained clean execution identity')
    if policy.get('worker_role_delivery') != 'unverified' or policy.get('source_semantics') != 'packet-read-causal-write-completion-only':
        raise ValueError('source policy overclaims semantics')
    pre=snapshot['prerequisites']
    expected={'verdict':'approve','purpose':'one-diagnostic-attempt','case':'clean-retained-a-continuation',
        'max_attempts':1,'execution_id':execution_id,'absolute_deadline':deadline,
        'candidate_sha256':snapshot['candidate_sha256'],'case_sha256':snapshot['case_sha256'],
        'implementation_sha256':snapshot['implementation_sha256'],'configuration_sha256':snapshot['configuration_sha256'],
        'effective_configuration_sha256':pre['effective_configuration_sha256'],
        'source_packet_sha256':pre['source_packet_sha256'],'source_packet_root':pre['source_packet_root'],
        'source_account_scope_id':pre['source_account_scope_id'],
        'source_execution_id':policy['execution'],'source_online_status':'behavioral_failure','source_online_reward':0,
        'source_assessor_image':policy['assessor_image'],'source_assessment_sha256':policy['files']['evidence/offline-regrade.json']['sha256'],
        'source_semantic_audit_sha256':policy['files']['evidence/semantic-audit.md']['sha256'],
        'worker_role_delivery':'unverified','source_semantics':'packet-read-causal-write-completion-only',
        'handoff':'no-fault','model_name':'gpt-6.1-terra','reasoning_effort':'medium','image_id':snapshot['image_id'],'verifier_image_id':verifier_image,
        'account_scope_id':str(account_scope),'runtime_budget_seconds':1050,'step_agent_seconds':400,
        'step_verifier_seconds':60,'cleanup_grace_seconds':15,'result_boundary':'incomplete-pending-independent-bound-raw-audit'}
    if any(type(gate.get(k)) is not type(v) or gate.get(k)!=v for k,v in expected.items()):
        raise ValueError('retained clean gate binding mismatch')
    if not os.path.isabs(pre['source_account_scope_id']) or pre['source_account_scope_id'] == str(account_scope):
        raise ValueError('source and target account locks must be distinct')
    auth=str(auth_path)
    if not os.path.isabs(auth) or os.path.normpath(auth)!=auth or gate.get('auth_path')!=auth:
        raise ValueError('retained clean auth path binding mismatch')
    try:
        expiry=datetime.fromisoformat(deadline); current=datetime.now(timezone.utc) if now is None else now
        if expiry.tzinfo is None or current.tzinfo is None or expiry-current < timedelta(seconds=1050): raise ValueError()
    except (TypeError,ValueError,OverflowError):
        raise ValueError('retained clean gate expired or insufficient time') from None
    return {'execution_id':execution_id,'source_packet_sha256':pre['source_packet_sha256'],'expires_at':deadline}
