"""Closed account-only review for one clean-work execution."""
import json
import re
from datetime import datetime
from pathlib import Path
from terminal_failure_schema import unique

FIELDS = {'schema','decision','execution_id','job_id','trial_id','gate_path','gate_sha256',
          'start_sha256','case_sha256','candidate_sha256','configuration_sha256',
          'effective_configuration_sha256','historical_commit','historical_tree',
          'historical_implementation_sha256','resolver_commit','resolver_implementation_sha256',
          'authority_path','authority_sha256','raw_manifest_path','raw_manifest_sha256',
          'job_result_sha256','trial_result_sha256','terminal_class','exception',
          'steps','missing_artifacts','trace_limitation','reviewer','independence_basis',
          'reviewed_at','independent_review'}


def parse(raw):
    doc=json.loads(raw,object_pairs_hook=unique)
    if not isinstance(doc,dict) or set(doc) not in (FIELDS,FIELDS|{'raw_audit_path','raw_audit_sha256'}):
        raise ValueError('clean-work release schema mismatch')
    if doc['schema']!='planar-clean-work-account-v1' or doc['decision']!='release-account':
        raise ValueError('unsupported account disposition')
    if isinstance(doc.get('steps'),dict) and doc['steps'].get('review') is None and not {'raw_audit_path','raw_audit_sha256'}<=set(doc): raise ValueError('one-step independent raw audit missing')
    if 'raw_audit_path' in doc:
        if type(doc['raw_audit_path']) is not str or not Path(doc['raw_audit_path']).is_absolute() or '..' in Path(doc['raw_audit_path']).parts or type(doc['raw_audit_sha256']) is not str or not re.fullmatch(r'[0-9a-f]{64}',doc['raw_audit_sha256']): raise ValueError('invalid raw audit binding')
    strings=FIELDS-{'steps','missing_artifacts','exception','independent_review'}
    if any(type(doc[key]) is not str or not doc[key].strip() for key in strings):
        raise ValueError('invalid disposition string')
    paths=[doc[k] for k in ('gate_path','authority_path','raw_manifest_path')] + ([doc['raw_audit_path']] if 'raw_audit_path' in doc else [])
    if len(set(paths))!=len(paths) or any(not Path(p).is_absolute() or '..' in Path(p).parts for p in paths):
        raise ValueError('unsafe or repeated review paths')
    if doc['independent_review'] is not True:
        raise ValueError('independent review missing')
    if not re.fullmatch(r'planar-t044-[0-9a-f]{32}',doc['execution_id']):
        raise ValueError('invalid execution ID')
    if any(not re.fullmatch(r'[0-9a-f]{64}',doc[k]) for k in FIELDS if k.endswith('_sha256')):
        raise ValueError('invalid digest')
    if any(not re.fullmatch(r'[0-9a-f]{40}',doc[k]) for k in ('historical_commit','historical_tree','resolver_commit')):
        raise ValueError('invalid commit identity')
    try:
        if datetime.fromisoformat(doc['reviewed_at']).tzinfo is None: raise ValueError()
    except ValueError: raise ValueError('invalid review time') from None
    if doc['terminal_class'] not in ('completed','incomplete','partial_failure'):
        raise ValueError('unsupported terminal class')
    if doc['exception'] is not None and (type(doc['exception']) is not str or not doc['exception'].strip()):
        raise ValueError('invalid exception')
    if doc['terminal_class']=='partial_failure' and doc['exception'] is None:
        raise ValueError('partial failure requires exception')
    missing=doc['missing_artifacts']
    if not isinstance(missing,list) or any(type(x) is not str or not x for x in missing) or len(missing)!=len(set(missing)):
        raise ValueError('invalid missing artifacts')
    allowed_missing={'final host capture','post host capture','worker native logs','review native logs','worker verifier output','review verifier output'}
    if not set(missing)<=allowed_missing: raise ValueError('unsupported missing artifact')
    steps=doc['steps']
    if not isinstance(steps,dict) or set(steps)!={'worker','review'}:
        raise ValueError('invalid step inventory')
    for step in steps.values():
        if step is None: continue
        if not isinstance(step,dict) or set(step)!={'status','reason','reward','exception','status_sha256','reward_sha256'}:
            raise ValueError('invalid step outcome')
        if step['status'] not in ('pass','incomplete','behavioral_failure') or (step['reason'] is not None and (type(step['reason']) is not str or not step['reason'])):
            raise ValueError('invalid step status')
        if step['exception'] is not None and not isinstance(step['exception'],dict):
            raise ValueError('invalid step exception')
        if type(step['reward']) is not int or step['reward'] not in (0,1):
            raise ValueError('invalid step reward')
        if any(type(step[k]) is not str or not re.fullmatch(r'[0-9a-f]{64}',step[k]) for k in ('status_sha256','reward_sha256')):
            raise ValueError('invalid step digest')
    return doc
