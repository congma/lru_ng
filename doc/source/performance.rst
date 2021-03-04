..
   vim: spell spelllang=en

.. highlight:: python

.. currentmodule:: lru_ng

.. toctree::
   :maxdepth: 3


Performance
===========


Speed
*****

In general, the code should perform as fast as :code:`lru.LRU` for the
following operations:

* Lookup by key (either a hit or miss), and
* Replace a key's associated value.

Internally there is more overhead because of some extra checking and loading in
order to avoid executing foreign code in a critical section. Such overhead
are insignificant though as shown by benchmarks, and the benefit outweighs the
cost.

The following operations are faster *and* overall safer (where applicable):

* Inserting a new item,
* Deleting an existing key-value pair,
* Evicting items under insertion load, with or without callback set, and
* Dict-like :ref:`methods <dict-emulation>`.

The one scenario that could incur significant slowdown is when

* a callback is not set, *and*
* the evicted key or value's :meth:`~object.__del__` method would have
  presented a risk of executing arbitrary code.

As special cases, for a handful of Python built-in types such as :class:`str` or
:class:`bytearray` (but not for subclasses), we can be certain that their
finalization/deallocation wouldn't interfere with our normal operation, and
they will not cause slowdown.

However, in general, we take extra care to defer potential deallocation despite
the overhead, because the safety far outweighs the extra speed. The "slow" code
can be observed in benchmarks where the evictions are triggered by resizing a
very large :class:`LRUDict` object to a small size, without the callback. The
cause is the overhead of extra moves to ensure that no :meth:`~object.__del__`
code may be triggered while doing internal sensitive operations, and that
normal method calls may not fail spuriously.


Memory usage
************

The memory usage is larger because of internal memoization of key hash values.
The overhead *per key* is the same as the platform's pointer size (4/8 bytes on
32/64-bit systems). That is, the overhead is O(n) where n is the number of keys.

Some additional memory allocations are made to keep a queue of items facing
imminent destruction, but these are usual small and allocated per
:class:`LRUDict` instance, or O(1).

The :class:`LRUDict` object participates effectively in Python's :term:`garbage
collection`. Reference cycles are detected by Python's cyclic garbage collector
and broken up when all external references are dropped. For example, the
following code

.. code-block:: python

   from lru_ng import LRUDict
   r = LRUDict(4)
   r[0] = r
   del r

will not leave a self-referencing, unusable, but "live" object permanently in
memory. It will eventually be collected by Python's garbage collector.
