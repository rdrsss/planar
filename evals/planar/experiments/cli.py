"""Frozen experiment controls and individually review-gated Codex executions."""
import argparse
import json
from pathlib import Path
import controls


def main():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest='command', required=True)
    init = commands.add_parser('plan', help='resolve a version-1 JSON definition')
    init.add_argument('definition', type=Path)
    init.add_argument('output', type=Path)
    run = commands.add_parser('run', help='run or continue the no-model controls')
    run.add_argument('experiment', type=Path)
    run.add_argument('identity')
    run.add_argument('--max-new', type=int)
    run.add_argument('--retry-of')
    run.add_argument('--retry-reason')
    preflight = commands.add_parser('codex-plan', help='print review inputs for one Codex trial')
    preflight.add_argument('experiment', type=Path)
    preflight.add_argument('identity')
    preflight.add_argument('logical_id')
    live = commands.add_parser('codex-run', help='execute one separately reviewed Codex trial')
    live.add_argument('experiment', type=Path)
    live.add_argument('identity')
    live.add_argument('logical_id')
    live.add_argument('--auth-file', type=Path, required=True)
    live.add_argument('--review-file', type=Path, required=True)
    live.add_argument('--retry-of')
    live.add_argument('--retry-reason')
    batch = commands.add_parser('codex-batch', help='run unreserved trials with individually reviewed gates')
    batch.add_argument('experiment', type=Path)
    batch.add_argument('identity')
    batch.add_argument('--auth-file', type=Path, required=True)
    batch.add_argument('--gates-file', type=Path, required=True, help='JSON object mapping each pending logical ID to its gate file')
    resolve = commands.add_parser('resolve-account-stop', help='record explicit account-stop resolution')
    resolve.add_argument('experiment', type=Path)
    resolve.add_argument('identity')
    resolve.add_argument('execution_id')
    resolve.add_argument('--reason', required=True)
    report = commands.add_parser('report', help='print all-trial report')
    report.add_argument('experiment', type=Path)
    report.add_argument('identity')
    report.add_argument('--markdown', action='store_true')
    report.add_argument('--output', type=Path, help='create a new report file without overwriting')
    v2plan = commands.add_parser('plan-v2', help='freeze explicit portable schema-2 inputs; no model or approval')
    v2plan.add_argument('definition', type=Path)
    v2plan.add_argument('output', type=Path)
    v2pre = commands.add_parser('preflight-v2', help='inspect one frozen variant without authorizing a run')
    v2pre.add_argument('experiment', type=Path)
    v2pre.add_argument('identity')
    v2pre.add_argument('logical_id')
    v2reserve = commands.add_parser('reserve-v2', help='reserve an exact future execution for independent review')
    v2reserve.add_argument('experiment', type=Path)
    v2reserve.add_argument('identity')
    v2reserve.add_argument('logical_id')
    v2reserve.add_argument('--expires-at', required=True)
    v2reserve.add_argument('--retry-of')
    v2reserve.add_argument('--retry-reason')
    runnable_prepare = commands.add_parser('prepare-runnable-v2', help='build and inspect verifier without authentication or model work')
    runnable_prepare.add_argument('experiment', type=Path)
    runnable_prepare.add_argument('identity')
    runnable_prepare.add_argument('logical_id')
    runnable_prepare.add_argument('--output', type=Path, required=True)
    runnable_reserve = commands.add_parser('reserve-runnable-v2', help='reserve one context-bound runnable-v2 trial after no-auth image preparation')
    runnable_reserve.add_argument('experiment', type=Path)
    runnable_reserve.add_argument('identity')
    runnable_reserve.add_argument('logical_id')
    runnable_reserve.add_argument('--expires-at', required=True)
    runnable_reserve.add_argument('--preparation-evidence', type=Path, required=True)
    runnable_reserve.add_argument('--retry-of')
    runnable_reserve.add_argument('--retry-reason')
    runnable_gate = commands.add_parser('check-runnable-v2', help='compare a runnable review gate without consuming or authorizing it')
    runnable_gate.add_argument('experiment', type=Path)
    runnable_gate.add_argument('identity')
    runnable_gate.add_argument('reservation_id')
    runnable_gate.add_argument('gate', type=Path)
    runnable_gate.add_argument('--preparation-evidence', type=Path, required=True)
    runnable_execute = commands.add_parser('execute-runnable-v2', help='execute one separately reviewed context-bound runnable-v2 reservation')
    runnable_execute.add_argument('experiment', type=Path)
    runnable_execute.add_argument('identity')
    runnable_execute.add_argument('reservation_id')
    runnable_execute.add_argument('--auth-file', type=Path, required=True)
    runnable_execute.add_argument('--gate', type=Path, required=True)
    runnable_execute.add_argument('--preparation-evidence', type=Path, required=True)
    runnable_reconcile = commands.add_parser('reconcile-runnable-v2', help='reconcile an existing runnable attempt without replaying auth or a native job')
    runnable_reconcile.add_argument('experiment', type=Path)
    runnable_reconcile.add_argument('identity')
    runnable_reconcile.add_argument('reservation_id')
    runnable_reconcile.add_argument('--gate', type=Path, required=True)
    v2resolve = commands.add_parser('resolve-account-stop-v2', help='v2 account disposition is fail-closed pending reviewed authority')
    v2resolve.add_argument('experiment', type=Path)
    v2resolve.add_argument('identity')
    v2resolve.add_argument('execution_id')
    v2resolve.add_argument('--reason', required=True)
    v2resolve.add_argument('--gate', type=Path, required=True)
    pretrial_release = commands.add_parser('resolve-pretrial-account-v2', help='publish a separately reviewed pre-native account-only disposition')
    pretrial_release.add_argument('--review', type=Path, required=True)
    v2gate = commands.add_parser('check-gate-v2', help='compare an existing gate without consuming or authorizing it')
    v2gate.add_argument('experiment', type=Path)
    v2gate.add_argument('identity')
    v2gate.add_argument('reservation_id')
    v2gate.add_argument('gate', type=Path)
    v2report = commands.add_parser('report-v2', help='inventory planned reservations and unreconciled execution records')
    v2report.add_argument('experiment', type=Path)
    v2report.add_argument('identity')
    v2report.add_argument('--output', type=Path)
    args = p.parse_args()
    if args.command == 'plan-v2':
        import schema2
        definition = json.loads(args.definition.read_text())
        if definition.pop('schema', None) != 2: raise ValueError('unsupported definition schema')
        print(json.dumps(schema2.initialize(args.output, **definition), indent=2))
    elif args.command == 'preflight-v2':
        import schema2
        print(json.dumps(schema2.preflight(args.experiment, args.identity, args.logical_id), indent=2))
    elif args.command == 'reserve-v2':
        import schema2
        print(json.dumps(schema2.reserve(args.experiment, args.identity, args.logical_id, expires_at=args.expires_at, predecessor_execution_id=args.retry_of, retry_reason=args.retry_reason), indent=2))
    elif args.command == 'prepare-runnable-v2':
        import schema2_runnable
        print(json.dumps(schema2_runnable.prepare_image(args.experiment,args.identity,args.logical_id,output=args.output),indent=2))
    elif args.command == 'reserve-runnable-v2':
        import schema2_runnable
        print(json.dumps(schema2_runnable.reserve(args.experiment,args.identity,args.logical_id,expires_at=args.expires_at,preparation_evidence=args.preparation_evidence,predecessor_execution_id=args.retry_of,retry_reason=args.retry_reason),indent=2))
    elif args.command == 'check-runnable-v2':
        import per_variant
        import schema2_runnable
        gate=per_variant.parse(args.gate.read_bytes(),1024*1024)
        print(json.dumps(schema2_runnable.check_gate(args.experiment,args.identity,args.reservation_id,gate,preparation_evidence=args.preparation_evidence),indent=2))
    elif args.command == 'execute-runnable-v2':
        import schema2_execution
        print(json.dumps(schema2_execution.execute_runnable(args.experiment,args.identity,args.reservation_id,args.auth_file,args.gate,preparation_evidence=args.preparation_evidence),indent=2))
    elif args.command == 'reconcile-runnable-v2':
        import schema2_execution
        result=schema2_execution.reconcile_runnable(args.experiment,args.identity,args.reservation_id,args.gate)
        print(json.dumps(result if result is not None else {'status':'blocked','reason':'insufficient or contradictory recovery evidence'},indent=2))
    elif args.command == 'resolve-account-stop-v2':
        import schema2_execution
        print(json.dumps(schema2_execution.resolve_account_stop(args.experiment,args.identity,args.execution_id,args.reason,gate_path=args.gate),indent=2))
    elif args.command == 'resolve-pretrial-account-v2':
        import schema2_pretrial_release
        print(json.dumps(schema2_pretrial_release.publish(args.review),indent=2))
    elif args.command == 'check-gate-v2':
        import schema2
        gate = json.loads(args.gate.read_text())
        schema2.validate_gate(args.experiment, args.identity, args.reservation_id, gate)
        print(json.dumps({'comparison':'matched', 'authorization':'not granted', 'gate_consumed':False}))
    elif args.command == 'report-v2':
        import schema2_reporting
        content = json.dumps(schema2_reporting.report(args.experiment, args.identity), indent=2) + '\n'
        if args.output:
            with args.output.open('x') as output: output.write(content)
        else: print(content, end='')
    elif args.command == 'plan':
        definition = json.loads(args.definition.read_text())
        if definition.pop('schema', None) != 1:
            raise ValueError('unsupported definition schema')
        result = controls.initialize(args.output, **definition)
        print(json.dumps(result, indent=2))
    elif args.command == 'run':
        print(json.dumps({'launched': controls.run_nop(args.experiment, args.identity, max_new=args.max_new, retry_of=args.retry_of, retry_reason=args.retry_reason)}))
    elif args.command == 'codex-plan':
        import codex_run
        print(json.dumps(codex_run.preflight_one(args.experiment, args.identity, args.logical_id), indent=2))
    elif args.command == 'codex-run':
        import codex_run
        print(json.dumps(codex_run.execute_one(args.experiment, args.identity, args.logical_id, args.auth_file, args.review_file, retry_of=args.retry_of, retry_reason=args.retry_reason)))
    elif args.command == 'codex-batch':
        import codex_run
        gates = json.loads(args.gates_file.read_text())
        print(json.dumps(codex_run.execute_batch(args.experiment, args.identity, args.auth_file, gates)))
    elif args.command == 'resolve-account-stop':
        manifest = controls.validate(args.experiment, args.identity)
        if manifest['kind'] == 'injected-control-only':
            controls.resolve_nop_stop(args.experiment, args.identity, args.execution_id, args.reason)
        else:
            import codex_run
            codex_run.resolve_account_stop(args.experiment, args.identity, args.execution_id, args.reason)
        print(json.dumps({'resolved': args.execution_id}))
    else:
        result = controls.report(args.experiment, args.identity)
        content = controls.markdown(result) if args.markdown else json.dumps(result, indent=2) + '\n'
        if args.output:
            with args.output.open('x') as output:
                output.write(content)
        else:
            print(content, end='')


if __name__ == '__main__':
    main()
