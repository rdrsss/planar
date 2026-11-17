"""Read-only schema-2 admission handoff; never activates or consumes a gate.

Integration MUST revalidate frozen bytes, complete runtime configuration and
actual image identities under the existing shared account lock, atomically
activate the reviewed execution ID, and consume the validated gate using the
existing exclusive marker store before reading auth. This module is not an
admission, lock, image-inspection, gate-consumption or dispatch implementation.
"""
import hashlib
import json
from pathlib import Path
import schema2


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise schema2.Invalid('duplicate gate field')
        result[key] = value
    return result


def prepare(root, identity, reservation_id, gate_path, *, reviews_root, account_scope_id):
    """Produce reviewable pointers only; caller must not treat result as approval."""
    root, gate_path, reviews_root = Path(root), Path(gate_path), Path(reviews_root)
    if reviews_root.is_symlink() or not reviews_root.is_dir():
        raise schema2.Invalid('unsafe reviews root')
    if gate_path.is_symlink() or not gate_path.is_file():
        raise schema2.Invalid('unsafe gate path')
    reviews = reviews_root.resolve(strict=True)
    resolved_gate = gate_path.resolve(strict=True)
    if not resolved_gate.is_relative_to(reviews) or resolved_gate == reviews:
        raise schema2.Invalid('gate outside reviews root')
    data = gate_path.read_bytes()
    if len(data) > 1024*1024: raise schema2.Invalid('oversized gate')
    try:
        gate = json.loads(data, object_pairs_hook=no_duplicates)
    except (UnicodeDecodeError, ValueError) as exc:
        raise schema2.Invalid('invalid gate JSON') from exc
    selected = schema2.validate_gate(root, identity, reservation_id, gate)
    reservation = schema2.read_record(root/'reservations'/f'{reservation_id}.json')
    if not isinstance(account_scope_id,str) or not account_scope_id or reservation['account_scope_id'] != account_scope_id:
        raise schema2.Invalid('account scope mismatch')
    # Exactly the existing launch.consume_review_once digest algorithm (no LF).
    gate_marker = hashlib.sha256(json.dumps(gate, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    scope = reviews/'planar-account-admission-v1'
    consumed = reviews/'harbor-t044-consumed'
    for path in (scope, scope/'records', scope/'lock', consumed, consumed/gate_marker):
        if path.is_symlink(): raise schema2.Invalid('unsafe admission control path')
    return {'status':'review-only', 'authorization':'not granted', 'gate_consumed':False,
        'experiment_identity':identity, 'logical_id':reservation['logical_id'],
        'reservation_id':reservation_id, 'execution_id':reservation['execution_id'],
        'reservation_sha256':schema2.h((root/'reservations'/f'{reservation_id}.json').read_bytes()),
        'gate_path':str(resolved_gate), 'gate_raw_sha256':schema2.h(data), 'gate_marker_sha256':gate_marker,
        'gate_marker_exists':(consumed/gate_marker).exists(), 'account_scope_id':account_scope_id,
        'admission_scope':str(scope), 'admission_lock':str(scope/'lock'),
        'admission_records':str(scope/'records'), 'consumed_root':str(consumed),
        'candidate':selected['candidate'], 'instance':selected['instance'],
        'requirements':['revalidate under existing shared lock', 'inspect actual agent and verifier images and runtime configuration',
            'activate exact reserved execution ID under account admission', 'consume gate exclusively before authentication',
            'reconcile native identity, separate verifier and cleanup']}
