import gc
from unittest import TestCase
import pytest
import yagot
from lru_ng import LRUDict


class K:
    __slots__ = ("other")
    def __init__(self, other=None):
        self.other = other


class TestYagotWorks(TestCase):
    @yagot.garbage_checked()
    def test_nogarbage(self):
        r = LRUDict(1)
        del r

    @yagot.garbage_checked()
    @pytest.mark.xfail(True, reason=("Injection test to test Yagot can detect"
                                     " garbage"),
                       raises=AssertionError, run=True, strict=True)
    def test_garbage(self):
        k = K()
        k.other = k


class TestRefCycle(TestCase):
    """Test cycle-breaking via participating in Python's garbage collection."""
    def test_gc_tracking(self):
        r = LRUDict(1)
        self.assertTrue(gc.is_tracked(r))

    @yagot.garbage_checked(leaks_only=True)
    def test_link_value(self):
        r = LRUDict(5)
        r[0] = r
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_link_tuple(self):
        r = LRUDict(5)
        r[0] = (0, r)
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_link_dict(self):
        r = LRUDict(5)
        r[0] = {0: r}
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_link_mutual(self):
        r = LRUDict(5)
        s = LRUDict(1)
        r[0] = s
        s[0] = r
        del r, s

    @yagot.garbage_checked(leaks_only=True)
    def test_list_link(self):
        r = LRUDict(1)
        l = [r]
        r[0] = l
        del l, r

    @yagot.garbage_checked(leaks_only=True)
    def test_transient(self):
        r = LRUDict(1, callback=lambda x, y: (x, y))
        r[0] = r
        r._suspend_purge = True
        r[1] = 0
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_link_callback(self):
        class CB:
            def __init__(self, other=None):
                self.other = other
            def __call__(self, key, value):
                pass
        r = LRUDict(1)
        f = CB(r)
        r.callback = f
        r[0] = r
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_large_number_of_links(self):
        N = 10000
        r = LRUDict(N, callback=lambda k, v: (k, v))
        for i in range(N):
            r[i] = r
        for i in range(N, 2 * N):
            r[i] = r
        del r

    @yagot.garbage_checked(leaks_only=True)
    def test_to_dict_cycle(self):
        r = LRUDict(10)
        r[0] = r
        d = r.to_dict()
        del r
        d[0]
        del d[0]
