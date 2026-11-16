// Handoff and loop rules: references/loop.md, "Workflow handoff".
export const meta = {
  name: 'run',
  description: 'Drive the Planar execution loop through its agents.'
};

export default async function run(args) {
  const driverOptions = { ...args.driverOptions, agentType: 'planar:orchestrator' };
  const reply = await agent(
    JSON.stringify({ ...args, step: 'workflow bootstrap' }),
    driverOptions
  );
  if (!reply) return { status: 'incomplete', step: 'bootstrap' };
  const bootstrap = typeof reply === 'string' ? JSON.parse(reply) : reply;
  if (bootstrap.status !== 'ready') return bootstrap;
  let previous = [];
  for (;;) {
    const reply = await agent(
      JSON.stringify({ ...args, step: 'next workflow step', bootstrap, previous }),
      driverOptions
    );
    if (!reply) return { status: 'incomplete', step: 'orchestrator' };
    const next = typeof reply === 'string' ? JSON.parse(reply) : reply;
    if (next.status === 'bookkeeping') {
      const result = await agent(
        JSON.stringify({ ...args, bootstrap, step: next.step, task: next.task, previous }),
        driverOptions
      );
      if (!result) return { status: 'incomplete', task: next.task, step: next.step };
      const outcome = typeof result === 'string' ? JSON.parse(result) : result;
      if (outcome.status !== 'complete') return outcome;
      previous = outcome.previous;
      continue;
    }
    if (next.status !== 'dispatch') return next;
    previous = await Promise.all(next.dispatches.map(async dispatch => ({
      task: dispatch.task,
      step: dispatch.step,
      result: await agent(dispatch.prompt, dispatch.options)
    })));
  }
}
