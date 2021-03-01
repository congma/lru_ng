from collections import defaultdict
import itertools
import gevent
from gevent.event import Event
import pytest
from lru_ng import LRUDict


@pytest.mark.parametrize("stride", (1, 2, 10, 20))
@pytest.mark.parametrize("cache_size", (1, 2, 10, 11, 100))
@pytest.mark.parametrize("n_pusher", range(0, 5))
def test_callback_unique_keys(n_pusher, cache_size, stride):
    """The necessary conditions being tested are, assuming that the collection
    of keys pushed by each pusher do not overlap with each other:

    a) that during the course of pushing by multiple greenlets, the callback
    can never visit duplicate keys; and

    b) the keys pulled by the puller are distinct; and

    c) that the set of keys visited by the callback and the keys pulled from
    the cache are disjoint; and

    d) that union of the two sets in c) is equal to the set of all pushed keys.

    This sounds trivial but is apparently lru.LRU and pylru.lrucache as of now
    don't maintain condition a).
    """

    pending = n_pusher
    cb_histogram = defaultdict(int)

    def cb(*args):
        nonlocal cb_histogram
        cb_histogram[args[0]] += 1
        # The following line is the test: yielding explicitly in the callback.
        # What this does is to clog up the purge queue -- don't do this in
        # production code!
        gevent.sleep(0)

    cache = LRUDict(cache_size, callback=cb)

    def push(ev, cache, iterable):
        nonlocal pending
        for i in iterable:
            cache[i] = i
            gevent.sleep(0)
        pending -= 1

    def pull(ev, cahce):
        nonlocal pending
        pulled_keys = defaultdict(int)
        ev.wait()
        # Keep on popping items until all producers (pushers) have definitely
        # ceased production.
        while True:
            try:
                s = cache.popitem()
            except KeyError:
                if pending:
                    continue
                else:
                    break
            else:
                pulled_keys[s[0]] += 1
            finally:
                gevent.sleep(0)
        # As a consequence of the test some keys may remain in the queue.
        cache.purge()
        return pulled_keys

    chunks = [range(i * stride, (i + 1) * stride) for i in range(n_pusher)]
    union_of_all_keys = frozenset(sum(map(list, chunks), []))

    starter = Event()

    # Create greenlets that wait for the starter.
    jobs = [gevent.spawn(push, starter, cache, c) for c in chunks]
    jobs.append(gevent.spawn(pull, starter, cache))

    # Let the greenlets run.
    starter.set()
    gevent.joinall(jobs)

    # "cache" should be empty after puller pulled everything.
    assert not cache

    # a) Each key is subjected to the callback only once if ever.
    for key, count in cb_histogram.items():
        assert count == 1
    visited_keys = frozenset(cb_histogram.keys())

    pulled_histogram = jobs[-1].get()
    # b) No key is pulled more than once
    for key, count in pulled_histogram.items():
        assert count == 1
    pulled_keys = frozenset(pulled_histogram.keys())

    # c) Pulled and visited are disjoint
    assert not (pulled_keys & visited_keys)

    # d) Union of pulled and visited is the entire set of keys
    assert (pulled_keys | visited_keys) == union_of_all_keys
