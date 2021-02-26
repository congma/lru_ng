from unittest import TestCase
import pytest
from lru_ng import LRUDict


class Prepared:
    def __init__(self, N):
        self.N = N
        self.d = {str(x): str(x + 1) for x in range(self.N)}
        self.r = LRUDict(self.N)
        self.r.update(self.d)
    def clear(self):
        self.r.clear()


@pytest.fixture(params=(1, 10, 100, 1000, 10000))
def res(request):
    obj = Prepared(request.param)
    yield obj
    obj.clear()


class TestConversion:
    """Test type-conversion like operations."""
    def test_to_dict_invariant(self, res):
        assert res.d == res.r.to_dict()

    def test_order(self, res):
        r_to_dict = res.r.to_dict()
        new_dict = dict(reversed(res.r.items()))
        assert r_to_dict == new_dict

    def test_new_inst_via_dict_invariant(self, res):
        tmp = res.r.to_dict()
        new_inst = LRUDict(len(res.r))
        new_inst.update(tmp)
        new_inst_dump = new_inst.items()
        old_dump = res.r.items()
        assert new_inst_dump == old_dump
