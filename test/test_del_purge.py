from lru_ng import LRUDict


def test_del_purge():
    callback_called = False

    def cb(*args):
        nonlocal callback_called
        callback_called = True

    r = LRUDict(1, cb)
    r._suspend_purge = True
    r[0] = 0
    r[1] = 1
    del r
    assert callback_called
