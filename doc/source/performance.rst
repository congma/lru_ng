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

The one operation that could be significantly slower is eviction without a
callback when the evicted key or value's :meth:`~object.__del__` method would
have presented a risk of being executed. For a handful of Python built-in types
such as :class:`str` or :class:`bytearray` (but not for subclasses), extra
checks are bypassed and the overhead is minimal. We choose to implement this
despite the cost because the safety far outweighs the extra speed. The "slow"
code is can be observed in benchmarks where the evictions are triggered by
resizing a very large :class:`LRUDict` object to a small size, without the
callback. The cause is the overhead of extra moves to ensure no
:meth:`~object.__del__` code can be triggered while doing internal sensitive
operations.


Memory usage
************

The memory usage is a bit larger because of internal memoization of key hash
values. The overhead *per key* is the same as the platform's pointer size (4/8
bytes on 32/64-bit systems). Some additional memory allocations are made to
keep a list of items facing imminent destruction, but these are usual small and
allocated per :class:`LRUDict` instance.

The :class:`LRUDict` object participates effectively in Python's :term:`garbage
collection`. Reference cycles are detected by Python's tracing garbage
collector and broken up when all external references are dropped. For example,
the following code

.. code-block:: python

   from lru_ng import LRUDict
   r = LRUDict(4)
   r[0] = r
   del r

will not leave an "unfreeable" self-referencing object in the memory.
