"""Historical recovery-control runner; retained evidence remains readable.

TS-19.10 / T-055: the old runner created evidence under an inferred project
before validating its inputs. New control creation is retired until it can use
an independently reviewed selected-project and output context.
"""


def run(variant):
    if variant not in ('positive', 'changed-check', 'recorded-live'):
        raise ValueError('unknown control variant')
    raise RuntimeError('new recovery controls are retired; inspect existing evidence at its recorded root')


if __name__ == '__main__':
    run('positive')
