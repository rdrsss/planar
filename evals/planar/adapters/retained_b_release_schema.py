"""Closed independently reviewed release-document schema."""
import json
from datetime import datetime

FIELDS={'type','decision','reviewer','independence_basis','reviewed_at','claim','rationale','execution_id','job_id','trial_id','gate_sha256','source_packet_sha256','case_sha256','candidate_sha256','configuration_sha256','historical_implementation_sha256','resolver_commit','resolver_implementation_sha256','audit_sha256','wrapper_audit_sha256','trace_sha256','verifier_status_sha256','reward_sha256','trace_complete','unlogged_work_assessed','independent_review'}
BOOLEANS={'trace_complete','unlogged_work_assessed','independent_review'}
CLAIM='complete trace and state corroborate no B check, review or repair; unlogged work assessed'


def _unique(pairs):
    obj={}
    for key,value in pairs:
        if key in obj: raise ValueError('duplicate release key')
        obj[key]=value
    return obj


def parse(raw):
    doc=json.loads(raw,object_pairs_hook=_unique)
    if not isinstance(doc,dict) or set(doc)!=FIELDS: raise ValueError('release schema mismatch')
    if any(type(doc[k]) is not str or not doc[k].strip() for k in FIELDS-BOOLEANS): raise ValueError('release string type mismatch')
    if any(type(doc[k]) is not bool or doc[k] is not True for k in BOOLEANS): raise ValueError('release provenance is incomplete')
    if doc['type']!='aborted-before-work' or doc['decision']!='release-account' or doc['claim']!=CLAIM: raise ValueError('release claim mismatch')
    try:
        if datetime.fromisoformat(doc['reviewed_at']).tzinfo is None: raise ValueError()
    except ValueError: raise ValueError('release review time invalid') from None
    return doc
