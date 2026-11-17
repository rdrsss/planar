"""Closed timeout/partial-work account-only release review schema."""
import json
from datetime import datetime

FIELDS={'schema','type','decision','execution_id','job_id','trial_id','gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','effective_configuration_sha256','historical_commit','historical_tree','historical_implementation_sha256','resolver_commit','resolver_implementation_sha256','authority_path','authority_sha256','parent_audit_path','parent_audit_sha256','child_audit_path','child_audit_sha256','raw_manifest_path','raw_manifest_sha256','exception_sha256','status_sha256','reward_sha256','reviewer','independence_basis','reviewed_at','rationale','partial_work_reviewed','missing_final_acknowledged','independent_review','attempt2_unresolved','claim'}
BOOLS={'partial_work_reviewed','missing_final_acknowledged','independent_review','attempt2_unresolved'}

def unique(pairs):
    result={}
    for k,v in pairs:
        if k in result: raise ValueError('duplicate key')
        result[k]=v
    return result

def parse(raw):
    doc=json.loads(raw,object_pairs_hook=unique)
    if not isinstance(doc,dict) or set(doc)!=FIELDS: raise ValueError('release schema mismatch')
    if any(type(doc[k]) is not str or not doc[k].strip() for k in FIELDS-BOOLS): raise ValueError('release string type mismatch')
    if any(type(doc[k]) is not bool or doc[k] is not True for k in BOOLS): raise ValueError('release provenance incomplete')
    if doc['claim']!='partial work occurred; final snapshot missing; attempt 2 outcome unresolved': raise ValueError('unsupported partial-work claim')
    if doc['schema']!='planar-terminal-failure-v1' or doc['type']!='terminal-failure-v1' or doc['decision']!='release-account': raise ValueError('release type mismatch')
    if any(len(doc[k])!=64 or any(c not in '0123456789abcdef' for c in doc[k]) for k in FIELDS if k.endswith('_sha256')): raise ValueError('invalid digest')
    if any(len(doc[k])!=40 or any(c not in '0123456789abcdef' for c in doc[k]) for k in ('historical_commit','historical_tree','resolver_commit')): raise ValueError('invalid git identity')
    try:
        if datetime.fromisoformat(doc['reviewed_at']).tzinfo is None: raise ValueError()
    except ValueError: raise ValueError('invalid review timestamp') from None
    return doc
