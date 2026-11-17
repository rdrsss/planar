"""No-fault receipt primitives; host owns creation and B verifies it."""
import hashlib

PRODUCTS = ('repository/RESULT.txt', 'repository/STATE.txt')
EXPECTED = (b'2\n', b'correct\n')


def receipt(pre, post):
    if pre != post:
        raise ValueError('no-fault snapshots differ')
    if any(pre.get(path) != expected for path, expected in zip(PRODUCTS, EXPECTED)):
        raise ValueError('worker products do not match the task')
    digests = {path: hashlib.sha256(pre[path]).hexdigest() for path in PRODUCTS}
    return {'mode': 'none', 'pre_products': digests, 'post_products': dict(digests)}


def verify(value, pre, post):
    if value != receipt(pre, post):
        raise ValueError('invalid no-fault receipt')
