"""Classify a verified review without treating structure as semantic approval."""


def pending_review(verdict, *, packet_bound, native_child_bound, check_bound):
    """Accept only explicit, independently bound structure; never grant reward.

    Bindings are produced by the future native evidence verifier, not by the
    target's own prose. Unsupported or unavailable evidence stays incomplete.
    """
    if verdict not in ('approve', 'request-changes', 'open-question'):
        return {'status': 'incomplete', 'reason': 'review verdict unavailable', 'reward': 0}
    if any(flag is not True for flag in (packet_bound, native_child_bound, check_bound)):
        return {'status': 'incomplete', 'reason': 'native structural evidence unavailable', 'reward': 0}
    return {'status': 'incomplete', 'reason': 'independent bound raw semantic audit required',
            'review_verdict': verdict, 'reward': 0, 'semantic_audit': 'required'}
