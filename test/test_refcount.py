"""Testing refcount correctness of methods on LRUDict."""
from sys import getrefcount, exc_info
from contextlib import AbstractContextManager
from unittest import TestCase
from typing import Sequence, List, ContextManager
import pytest
from trackrefcount import TrackRCFor
from lru_ng import LRUDict


# NOTE:
#   - Avoid testing the refcount on the None, True, False, Ellipsis, or other
#     built-in objects that are not supposed to be deallocated. This will be
#     unreliable.
#   - Avoid tracking an object by name but then re-binding that name to some
#     other object in the suite of the "with" statement.
#   - Avoid playing tricks with names (i.e. Python variables) and
#     inspecting/eval'ling "names of names" because such tricks may not play
#     well with pytest's tricks.
#   - Avoid re-binding the name for the context manager to something else
#   - After exiting, it cannot be entered again.
#   - Contexts are not scopes.


class TestRefCount(TestCase):
    """Testing the reference count of objects being accessed by LRUDict during
    the latter's normal course of operation.

    This only tests a portion of memory behaviour. The tests are meant to catch
    the most egregious, Python-breaking errors, but not underlying memory
    access invisible to "normal" Python code.
    """
    def test_indexing_expr(self):
        ldobj = LRUDict(3)
        k = "lorem" * 5
        v = "ipsum" * 5
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj[k]  # this expression should not leak refcount
        t.assertEqualRC()

    def test_indexing_assignment(self):
        ldobj = LRUDict(1)
        k = "lorem"
        v = "ipsum"
        with TrackRCFor(v) as outer_v:
            with TrackRCFor(k, v) as inner_kv:
                # insertion increfs k by 2 and v by 1 due to internal node
                # keeping a ref to the key
                ldobj[k] = v
            inner_kv.assertDelta(2, 1)
            ldobj[k] = None
        # replacing the value associated with existing key should not leak
        # the value
        outer_v.assertEqualRC()

    def test_indexing_del(self):
        k = object()
        v = object()
        with TrackRCFor(k, v) as t:
            ldobj = LRUDict(3)
            ldobj[k] = v
            del ldobj[k]
        # del-statement by key should revert refcount of both key and value
        t.assertEqualRC()

    def test_eviction(self):
        ldobj = LRUDict(1)
        k = "spam"
        v = 10000000000
        with TrackRCFor(k, v) as t:
            ldobj[k] = v
            ldobj["eggs"] = 0  # evict the (k, v) pair as per LRU
        t.assertEqualRC()

    def test_eviction_due_to_resizing(self):
        ldobj = LRUDict(100)
        k = "spam2"
        v = 10000000000
        with TrackRCFor(k, v) as t:
            ldobj[k] = v
            ldobj["eggs"] = 0
            ldobj.size = 1  # reduce size causes eviction of (k, v)
        t.assertEqualRC()

    def test_deleting_object_also_frees_kvs(self):
        k = "something"
        v = "value"
        with TrackRCFor(k, v) as t:
            ldobj = LRUDict(1000)
            ldobj[k] = v
            del ldobj  # deleting the LRUDict should free up key-value pairs
        t.assertEqualRC()

    def test_eviction_callback_keeps_ref(self):
        def f(key, value):
            pass
        ldobj = LRUDict(1, f)
        k = "some key"
        v = "some value"
        with TrackRCFor(k, v) as t:
            ldobj[k] = v
            ldobj[0] = 0
        t.assertEqualRC()

    def test_calling_callback_for_refcount_of_callback(self):
        import sys
        # switch the unraisablehook to one that does nothing; the default one
        # writes to stderr and makes pytest believe this were somehow
        # significant.
        sys_unrh_save = sys.unraisablehook
        def do_nothing(*args):
            pass
        sys.unraisablehook = do_nothing

        def f(key, value):
            raise ValueError((key, value))

        ldobj = LRUDict(1, f)
        k = "some key"
        v = "some value"
        ldobj[k] = v
        # merely having the callback "f" called should not affect the refcount
        # of the callback.
        with TrackRCFor(f) as t:
            ldobj[0] = 0
        # restore old unraisablehook
        sys.unraisablehook = sys_unrh_save
        t.assertEqualRC()

    def test_disable_callback(self):
        def f(k, v):
            pass
        with TrackRCFor(f) as t:
            ldobj = LRUDict(10, f)
            ldobj.callback = None
        t.assertEqualRC()

    def test_enable_callback(self):
        def f(k, v):
            pass
        with TrackRCFor(f) as t:
            ldobj = LRUDict(10, f)
        t.assertDelta(1)

    def test_switch_callback(self):
        def f(k, v):
            pass
        with TrackRCFor(f) as t:
            ldobj = LRUDict(10, f)
            ldobj.callback = lambda x, y: (x, y)
        t.assertEqualRC()

    def test_queued_eviction(self):
        # forcibly disable eviction WITH a callback set
        ldobj = LRUDict(1, lambda x, y: (x, y))
        ldobj._suspend_purge = True
        k, v = "spam", "eggs"
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj[0] = 0  # this puts k, v on the staging list but not purged
        # k loses one ref because it is removed as a dict key, but v is not
        # touched.
        t.assertDelta(-1, 0)

    def test_rewriting(self):
        k, v = "once", "upon a time"
        ldobj = LRUDict(2)
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj[k] = v
        t.assertEqualRC()

    def test_method_get_hitting(self):
        k, v = "something", "something else"
        ldobj = LRUDict(2)
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj.get(k)
        t.assertEqualRC()

    def test_method_get_missing(self):
        k, default = "something", "something else"
        ldobj = LRUDict(2)
        with TrackRCFor(k, default) as t:
            ldobj.get(k, default)
        t.assertEqualRC()

    def test_method_setdefault_hitting(self):
        k, v = "method", "caller"
        default = "dispatcher"
        ldobj = LRUDict(2)
        ldobj[k] = v
        with TrackRCFor(k, v, default) as t:
            ldobj.setdefault(k, default)
        t.assertEqualRC()

    def test_method_setdefault_inserting(self):
        k = "method"
        default = "dispatcher"
        ldobj = LRUDict(2)
        with TrackRCFor(k, default) as t:
            ldobj.setdefault(k, default)
        t.assertDelta(2, 1)

    def test_method_pop_hitting_withdefault(self):
        k = "poppend"
        v = "popped by association"
        default = "not touched"
        ldobj = LRUDict(300)
        ldobj[k] = v
        with TrackRCFor(k, v, default) as t:
            ldobj.pop("poppend", default)
        t.assertDelta(-2, -1, 0)

    def test_method_pop_hitting_withoutdefault(self):
        k = "poppend"
        v = "popped by association"
        ldobj = LRUDict(2)
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj.pop("poppend")
        t.assertDelta(-2, -1)

    def test_method_pop_missing_withdefault(self):
        k = "poppend"
        default = "vicarious poppend"
        ldobj = LRUDict(2)
        ldobj[0] = 0
        with TrackRCFor(k, default) as t:
            ldobj.pop(k, default)
        t.assertEqualRC()

    def test_method_pop_missing_withoutdefault(self):
        k = "poppend"
        ldobj = LRUDict(2)
        ldobj[0] = 0
        with TrackRCFor(k) as t:
            with self.assertRaises(KeyError):
                ldobj.pop(k)
        t.assertEqualRC()

    def test_method_popitem(self):
        k = "item_key"
        v = "item_value"
        ldobj = LRUDict(4)
        with TrackRCFor(k, v) as t:
            ldobj[k] = v
            with t() as t_inner:
                tup = ldobj.popitem()
            t_inner.assertDelta(-1, 0)
            del tup
        t.assertEqualRC()

    def test_method_clear(self):
        k = "item_key"
        v = "item_value"
        ldobj = LRUDict(4)
        ldobj[k] = v
        with TrackRCFor(k, v) as t:
            ldobj.clear()
        t.assertDelta(-2, -1)

    def test_set_size(self):
        ldobj = LRUDict(100)
        big = 4096
        small = 1
        with TrackRCFor(big, small) as t:
            ldobj.size = big
            ldobj.size = small
        t.assertEqualRC()


class ManualRefCountContextMixin:
    """Mixin class that boilerplates the setUp/tearDown sequences to ensure
    that an LRUDict object is created and accessible as attribute, that certain
    key/value are inserted, and should not be touched by other tests that are
    constructed (by the tester) to not touch them.
    """
    def setUp(self):
        self.orig_size = 4
        self.lobj = LRUDict(self.orig_size)
        self.k_special, self.v_special = "ks", "vs"
        self.lobj[self.k_special] = self.v_special
        # Manually enter the refcount tracking context manager
        self.special_tracker = TrackRCFor(self.k_special, self.v_special)
        self.special_tracker.__enter__()

    def tearDown(self):
        # Manually exiting the refcount tracking context manager
        self.special_tracker.__exit__(*exc_info())
        self.special_tracker.assertEqualRC()


# The strict=True parameter is vital: This should unconditionally (x)fail.
# The output of pytest for this test is XFAIL (lower-case "x" mark). If the
# fault-injecting tests fails to end up with AssertionError, the real tests are
# not going to work.
@pytest.mark.xfail(True, reason=("Fault-injection test to make sure that a"
                                 " construct used in actual tests is able to"
                                 " catch injected faults."), run=True,
                   raises=AssertionError, strict=True)
class TestManualRefCountContext(ManualRefCountContextMixin, TestCase):
    """Test that the way we do the real test on LRUDict in the testcase class
    "TestRefCountOtherItems" actually stands a chance of catching an error.
    """
    def test_injected_del(self):
        del self.lobj[self.k_special]

    def test_injected_alias(self):
        self.anotherattr = self.v_special


class TestRefCountOtherItems(ManualRefCountContextMixin, TestCase):
    """Test manipulating some item in the LRUDict does not alter the refcount
    of other items in unusual ways."""

    def test_access(self):
        self.lobj["another key"] = self.v_special
        self.lobj.pop("another key")
        self.lobj["another key"] = self.v_special
        self.lobj["another key"] = None
        self.lobj[self.k_special]
        self.lobj["spam"] = "eggs"
        del self.lobj["spam"]

    def test_evict_things_but_not_the_special_one(self):
        capacity = self.lobj.size - 1
        def f(k, v):
            return None
        self.lobj.callback = f
        for i in range(capacity):
            self.lobj[i] = i
            self.lobj[self.k_special]  # "refresh" the special key
        self.lobj[i + 1] = "overflow"  # causes eviction but "special" is safe
        self.lobj[self.k_special]
        self.lobj.size = 1  # evict all but special
        self.lobj.size = self.orig_size
        self.callback = None
