..
   vim: spell spelllang=en

.. highlight:: python

.. currentmodule:: lru_ng

.. toctree::
   :maxdepth: 3


Compatibility
=============

Being a fork of the :code:`lru` `module
<https://github.com/amitdev/lru-dict>`_, this module is designed with
compatibility in mind.  Most of the time, drop-in replacement can be achieved
by the import statement

.. code-block:: python

   from lru_ng import LRUDict as LRU

However, this module is also intended for newer CPython applications following
newer conventions and standards, which can diverge from the inherited code
(hence the "ng" in the module name, for "new generation"). I attempt to provide
a list of differences from :code:`lru == 1.1.7` here.


Differences from :code:`lru.LRU` 1.1.7
**************************************

* Name of the class has changed to :code:`LRUDict` (because the acronym "LRU"
  is better reserved as the name of an *algorithm*).
* Python 2.7 support is removed. Instead, CPython 3.6+ is now required.
* The :meth:`~LRUDict.get_size`, :meth:`~LRUDict.set_size`, and
  :meth:`~LRUDict.set_callback` methods are obsoleted. To resize or change the
  callback, use the properties :attr:`~LRUDict.size` and
  :attr:`~LRUDict.callback` respectively.
* The :meth:`~LRUDict.peek_first_item` and :meth:`~LRUDict.peek_last_item`
  methods raise :exc:`KeyError` for empty :class:`~LRUDict`, rather than return
  :data:`None`.
* The :meth:`~LRUDict.popitem` method by default pops in the order *from the
  most recent to the least recent*. This is the inverse of the old default
  behaviour.
* The string returned by :func:`repr` can now be distinguished from that of a
  plain :class:`dict`.

In addition, there are also some significant internal changes that affect
behaviours in more subtle ways.

.. _difflru:

* The methods that takes a "key" parameter evaluate the hash only once, like
  the Python :class:`dict`. This results in a performance gain.
* Python :term:`garbage collection` (reclaiming objects forming reference
  cycles that are otherwise unreachable) is supported (see also
  :ref:`performance:memory usage`).
* The :class:`LRUDict` instance is unhashable.
* The callback is now executed outside the critical section of the code (where
  internal changes to the data structures are taking place). Newly evicted
  items are buffered, and a :meth:`~LRUDict.purge` method can optionally ensure
  that the buffer is cleared, although the code takes care of purging normally.
  This change reduces the likelihood that a misbehaving callback may crash the
  Python process.
* Similarly, the :meth:`~object.__del__` of the key or value cannot be normally
  triggered inside the critical section.
* The callback's exception is suppressed (more details in
  :ref:`introduction:caveats with callbacks`).
* Assuming the protection of the :term:`global interpreter lock`, the methods
  now has limited protection of the critical section. If for some exotic reason
  the :meth:`~object.__hash__` or :meth:`~object.__eq__` special-methods of the
  key attempt to modify the :class:`LRUDict` object inside a method call, the
  code will attempt to detect this and raise an :exc:`LRUDictBusyError`
  exception.
* Overall behaviour in Python threads is now more predictable and safer. Even
  if a callback releases the GIL, the internal state remains consistent.  There
  is limited ability to detect conflicting access at runtime; see
  :ref:`thread-safety:thread safety` for more.


Comparison with Python :class:`dict`
************************************

:class:`LRUDict` attempts to emulate Python built-in :class:`dict` in
:ref:`API <dict-emulation>` and behaviour (when sensible). However, currently
returning an iterable proxy by :meth:`~dict.keys`, :meth:`~dict.values`, and
:meth:`~dict.items` are among the unsupported methods.

A :class:`dict` maintains *key-insertion order* (since CPython 3.6+), which is
not the same as key-use order in the LRU sense: a :ref:`hit
<hits-and-misses:hits and misses>` promotes the key to the highest priority but
does not necessarily change the insertion order (unless it is removed and
inserted again).


Comparison with Python :func:`functools.lru_cache`
**************************************************

Python provides a native and optimized :func:`functools.lru_cache`
implementation, which is primarily used as a :term:`decorator` for memoization
(remembering return values for inputs that has been evaluated before).

This module, however, provides a container/mapping class, and a memoizing LRU
decorator can be built on top of it (although not as fast as the native one,
because the latter implements more optimizations and also avoids a lot of
overhead of handling Python calls or exceptions).  Instead, this module focuses
on :class:`dict`-compatibility and the ability to
set a callback.

Also, unlike :func:`functools.lru_cache`, there is no default size limit at
initialization: a value must be provided by the user. Unbound size is not
supported, either: there must be a size bound.
