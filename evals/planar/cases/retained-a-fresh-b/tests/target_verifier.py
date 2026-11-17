"""Case-local two-root candidate verifier; host admission is intentionally pending.

This module cannot grant reward until separately reviewed shared integration
binds target gate, exact reserved execution, images and host provenance.
"""
from pathlib import Path

from native import MissingEvidence, InvalidBehavior
from assess_worker import Incomplete, Contradiction


def assess(packet, candidate, target):
    from source_packet import validate_inprocess
    from verify_restore import _verify as verify_restoration
    from capture import load
    from chronology import receipt
    from review_structure import verify as verify_structure
    from target_native import correlate_target, load_target
    from check_binding import verify as verify_checks
    try:
        packet, target = Path(packet), Path(target)
        source_binding = validate_inprocess(packet,candidate)
        verify_restoration(packet,target,source_binding)
        source=load(packet/'source/pre'); final=load(target/'final')
        structure=verify_structure(source,final)
        post_time,final_time=receipt(target,'post'),receipt(target,'final')
        lineage=correlate_target(target/'logs/revision',post_time,final_time,source_binding['assessment']['lineage'])
        parent,children=load_target(target/'logs/revision')
        checks=verify_checks(parent,final,children,target,candidate,post_time)
        return {'status':'incomplete', 'reason':'target host provenance and independent bound raw audit required',
                'source_packet_sha256':source_binding['source_packet_sha256'], 'target_lineage':lineage,
                'structural_checks':checks, 'file_structure':structure['file_structure'], 'semantic_audit':'required'}
    except (Incomplete,MissingEvidence,FileNotFoundError,OSError,UnicodeError,KeyError) as error:
        return {'status':'incomplete','reason':str(error)}
    except (Contradiction,InvalidBehavior) as error:
        return {'status':'behavioral_failure','reason':str(error)}
    except ValueError as error:
        # Unsupported formats, source/receipt drift and malformed evidence are
        # not automatically attributed to the target agent's behavior.
        return {'status':'incomplete','reason':str(error)}
