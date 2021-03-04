import gc
import random
import sys
import time
import threading
import unittest
from lru_ng import LRUDict

SIZES = [1, 2, 10, 1000]

# Only available on debug python builds.
gettotalrefcount = getattr(sys, 'gettotalrefcount', lambda: 0)


class TestLRU(unittest.TestCase):

    def setUp(self):
        gc.collect()
        self._before_count = gettotalrefcount()

    def tearDown(self):
        after_count = gettotalrefcount()
        self.assertEqual(self._before_count, after_count)

    def _check_kvi(self, valid_keys, l):
        valid_keys = list(valid_keys)
        valid_vals = list(map(str, valid_keys))
        self.assertEqual(valid_keys, l.keys())
        self.assertEqual(valid_vals, l.values())
        self.assertEqual(list(zip(valid_keys, valid_vals)), l.items())

    def test_invalid_size(self):
        self.assertRaises(ValueError, LRUDict, -1)
        self.assertRaises(ValueError, LRUDict, 0)

    def test_empty(self):
        l = LRUDict(1)
        self.assertEqual([], l.keys())
        self.assertEqual([], l.values())

    def test_add_within_size(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)
            self._check_kvi(range(size - 1, -1, -1), l)

    def test_delete_multiple_within_size(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)
            for i in range(0, size, 2):
                del l[i]
            self._check_kvi(range(size - 1, 0, -2), l)
            for i in range(0, size, 2):
                with self.assertRaises(KeyError):
                    l[i]

    def test_delete_multiple(self):
        for size in SIZES:
            l = LRUDict(size)
            n = size * 2
            for i in range(n):
                l[i] = str(i)
            for i in range(size, n, 2):
                del l[i]
            self._check_kvi(range(n - 1, size, -2), l)
            for i in range(0, size):
                with self.assertRaises(KeyError):
                    l[i]
            for i in range(size, n, 2):
                with self.assertRaises(KeyError):
                    l[i]

    def test_add_multiple(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)
            l[size] = str(size)
            self._check_kvi(range(size, 0, -1), l)

    def test_access_within_size(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)
            for i in range(size):
                self.assertEqual(l[i], str(i))
                self.assertEqual(l.get(i, None), str(i))

    def test_contains(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)
            for i in range(size):
                self.assertTrue(i in l)

    def test_access(self):
        for size in SIZES:
            l = LRUDict(size)
            n = size * 2
            for i in range(n):
                l[i] = str(i)
            self._check_kvi(range(n - 1, size - 1, -1), l)
            for i in range(size, n):
                self.assertEqual(l[i], str(i))
                self.assertEqual(l.get(i, None), str(i))

    def test_update(self):
        l = LRUDict(2)
        l['a'] = 1
        self.assertEqual(l['a'], 1)
        l.update(a=2)
        self.assertEqual(l['a'], 2)
        l['b'] = 2
        self.assertEqual(l['b'], 2)
        l.update(b=3)
        self.assertEqual(('b', 3), l.peek_first_item())
        self.assertEqual(l['a'], 2)
        self.assertEqual(l['b'], 3)
        l.update({'a': 1, 'b': 2})
        self.assertEqual(('b', 2), l.peek_first_item())
        self.assertEqual(l['a'], 1)
        self.assertEqual(l['b'], 2)
        l.update()
        self.assertEqual(('b', 2), l.peek_first_item())
        l.update(a=2)
        self.assertEqual(('a', 2), l.peek_first_item())

    def test_peek_first_item(self):
        l = LRUDict(2)
        with self.assertRaisesRegex(KeyError, "is empty"):
            l.peek_first_item()
        l[1] = '1'
        l[2] = '2'
        self.assertEqual((2, '2'), l.peek_first_item())

    def test_peek_last_item(self):
        l = LRUDict(2)
        with self.assertRaisesRegex(KeyError, "is empty"):
            l.peek_last_item()
        l[1] = '1'
        l[2] = '2'
        self.assertEqual((1, '1'), l.peek_last_item())

    def test_overwrite(self):
        l = LRUDict(1)
        l[1] = '2'
        l[1] = '1'
        self.assertEqual('1', l[1])
        self._check_kvi([1], l)

    def test_has_key(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(2 * size):
                l[i] = str(i)
                self.assertTrue(l.has_key(i))
            for i in range(size, 2 * size):
                self.assertTrue(l.has_key(i))
            for i in range(size):
                self.assertFalse(l.has_key(i))

    def test_capacity_get(self):
        for size in SIZES:
            l = LRUDict(size)
            self.assertTrue(size == l.get_size())

    def test_capacity_set(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size + 5):
                l[i] = str(i)
            l.set_size(size + 10)
            self.assertTrue(size + 10 == l.get_size())
            self.assertTrue(len(l) == size)
            for i in range(size + 20):
                l[i] = str(i)
            self.assertTrue(len(l) == size + 10)
            l.set_size(size + 10 - 1)
            self.assertTrue(len(l) == size + 10 - 1)

    def test_unhashable(self):
        l = LRUDict(1)
        self.assertRaises(TypeError, lambda: l[{'a': 'b'}])
        with self.assertRaises(TypeError):
            l[['1']] = '2'
        with self.assertRaises(TypeError):
            del l[{'1': '1'}]

    def test_clear(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size + 5):
                l[i] = str(i)
            l.clear()
            for i in range(size):
                l[i] = str(i)
            for i in range(size):
                _ = l[random.randint(0, size - 1)]
            l.clear()
            self.assertTrue(len(l) == 0)

    def test_get_and_del(self):
        l = LRUDict(2)
        l[1] = '1'
        self.assertEqual('1', l.get(1))
        self.assertEqual('1', l.get(2, '1'))
        self.assertIsNone(l.get(2))
        self.assertEqual('1', l[1])
        self.assertRaises(KeyError, lambda: l['2'])
        with self.assertRaises(KeyError):
            del l['2']

    def test_setdefault(self):
        l = LRUDict(2)
        l[1] = '1'
        val = l.setdefault(1)
        self.assertEqual('1', val)
        self.assertEqual((1, 0), l.get_stats())
        val = l.setdefault(2, '2')
        self.assertEqual('2', val)
        self.assertEqual((1, 0), l.get_stats())
        self.assertEqual(val, l[2])
        l.clear()
        val = 'long string' * 512
        l.setdefault(1, val)
        l[2] = '2'
        l[3] = '3'
        self.assertTrue(val)

    def test_pop(self):
        l = LRUDict(2)
        v = '2' * 4096
        l[1] = '1'
        l[2] = v
        val = l.pop(1)
        self.assertEqual('1', val)
        self.assertEqual((1, 0), l.get_stats())
        val = l.pop(2, 'not used')
        self.assertEqual(v, val)
        del val
        self.assertTrue(v)
        self.assertEqual((2, 0), l.get_stats())
        val = l.pop(3, '3' * 4096)
        self.assertEqual('3' * 4096, val)
        self.assertEqual((2, 1), l.get_stats())
        self.assertEqual(0, len(l))
        with self.assertRaises(KeyError) as ke:
            l.pop(4)
            self.assertEqual(4, ke.args[0])
        self.assertEqual((2, 2), l.get_stats())
        self.assertEqual(0, len(l))
        with self.assertRaises(TypeError):
            l.pop()

    def test_popitem(self):
        l = LRUDict(3)
        l[1] = '1'
        l[2] = '2'
        l[3] = '3'
        k, v = l.popitem()
        self.assertEqual((3, '3'), (k, v))
        self.assertEqual(len(l), 2)
        k, v = l.popitem(True)  # pop least recent
        self.assertEqual((1, '1'), (k, v))
        self.assertEqual((2, '2'), l.popitem(False))
        with self.assertRaises(KeyError) as ke:
            l.popitem()
            self.assertEqual('popitem(): LRUDict is empty', ke.args[0])
        self.assertEqual((0, 0), l.get_stats())

    def test_popitem_refcount(self):
        l = LRUDict(1)
        value = "A Pythong string"
        rc_before = sys.getrefcount(value)
        l[0] = value
        rc_after_insertion = sys.getrefcount(value)
        l.popitem()
        rc_after_popitem = sys.getrefcount(value)
        self.assertGreater(rc_after_insertion, rc_before)
        self.assertEqual(rc_after_popitem, rc_before)

    def test_stats(self):
        for size in SIZES:
            l = LRUDict(size)
            for i in range(size):
                l[i] = str(i)

            self.assertTrue(l.get_stats() == (0, 0))

            val = l[0]
            self.assertTrue(l.get_stats() == (1, 0))

            val = l.get(0, None)
            self.assertTrue(l.get_stats() == (2, 0))

            val = l.get(-1, None)
            self.assertTrue(l.get_stats() == (2, 1))

            try:
                val = l[-1]
            except:
                pass

            self.assertTrue(l.get_stats() == (2, 2))

            l.clear()
            self.assertTrue(len(l) == 0)
            self.assertTrue(l.get_stats() == (0, 0))

    def test_lru(self):
        l = LRUDict(1)
        l['a'] = 1
        l['a']
        self.assertEqual(l.keys(), ['a'])
        l['b'] = 2
        self.assertEqual(l.keys(), ['b'])

        l = LRUDict(2)
        l['a'] = 1
        l['b'] = 2
        self.assertEqual(len(l), 2)
        l['a']                  # Testing the first one
        l['c'] = 3
        self.assertEqual(sorted(l.keys()), ['a', 'c'])
        l['c']
        self.assertEqual(sorted(l.keys()), ['a', 'c'])

        l = LRUDict(3)
        l['a'] = 1
        l['b'] = 2
        l['c'] = 3
        self.assertEqual(len(l), 3)
        l['b']                  # Testing the middle one
        l['d'] = 4
        self.assertEqual(sorted(l.keys()), ['b', 'c', 'd'])
        l['d']                  # Testing the last one
        self.assertEqual(sorted(l.keys()), ['b', 'c', 'd'])
        l['e'] = 5
        self.assertEqual(sorted(l.keys()), ['b', 'd', 'e'])

    def test_callback(self):

        counter = [0]

        first_key = 'a'
        first_value = 1

        def callback(key, value):
            self.assertEqual(key, first_key)
            self.assertEqual(value, first_value)
            counter[0] += 1

        l = LRUDict(1, callback=callback)
        l[first_key] = first_value
        l['b'] = 1              # test calling the callback

        self.assertEqual(counter[0], 1)
        self.assertEqual(l.keys(), ['b'])

        l['b'] = 2              # doesn't call callback
        self.assertEqual(counter[0], 1)
        self.assertEqual(l.keys(), ['b'])
        self.assertEqual(l.values(), [2])

        l = LRUDict(1, callback=callback)
        l[first_key] = first_value

        l.set_callback(None)
        l['c'] = 1              # doesn't call callback
        self.assertEqual(counter[0], 1)
        self.assertEqual(l.keys(), ['c'])

        l.set_callback(callback)
        del l['c']              # doesn't call callback
        self.assertEqual(counter[0], 1)
        self.assertEqual(l.keys(), [])

        l = LRUDict(2, callback=callback)
        l['a'] = 1              # test calling the callback
        l['b'] = 2              # test calling the callback

        self.assertEqual(counter[0], 1)
        self.assertEqual(l.keys(), ['b', 'a'])
        l.set_size(1)
        self.assertEqual(counter[0], 2)  # callback invoked
        self.assertEqual(l.keys(), ['b'])

    def test_threads_callback_lock(self):
        # see: https://github.com/rotki/rotki/pull/2132#issuecomment-765408001

        def callback(key, value):
            with value:
                pass

        l = LRUDict(5, callback)

        def run():
            for i in range(10000):
                l[i] = threading.Lock()
                self.assertLessEqual(len(l), 5)

        threads_coll = [threading.Thread(target=run) for i in range(50)]
        for th in threads_coll:
            th.start()
        for th in threads_coll:
            th.join()
        self.assertEqual(len(l), 5)
        l.clear()

    def test_threads_callback_all_evictions(self):
        """Check that the callback is called on each evicted item once and only
        once.
        """
        counter = dict()

        def callback(key, value):
            nonlocal counter
            with open("/dev/urandom", "rb") as f:
                f.read(1024)
            try:
                counter[key] += 1
            except KeyError:
                counter[key] = 1

        l = LRUDict(100, callback)
        # Reduce GIL contention in I/O-bound threads.
        l._max_pending_callbacks = 2

        def runner(*args):
            nonlocal l
            batch_id, stride = args
            bt_low = batch_id * stride
            bt_high = bt_low + stride
            for i in range(bt_low, bt_high):
                l[i] = i

        m, n = 10, 200
        th_coll = [threading.Thread(target=runner, args=(i, n))
                   for i in range(m)]
        for th in th_coll:
            th.start()
        for th in th_coll:
            th.join()
        l.size = 1
        l["a"] = "a"
        n_evic = n * m
        for i in range(n_evic):
            self.assertIn(i, counter)
            times_visited = counter[i]
            self.assertEqual(times_visited, 1)


if __name__ == '__main__':
    unittest.main()
