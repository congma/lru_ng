..
   vim: spell spelllang=en

.. highlight:: python

.. toctree::
   :maxdepth: 3


API Reference
=============

Module
******

.. py:module:: lru_ng

The module :mod:`lru_ng` exposes the following class, :class:`LRUDict`. It can
be imported as 

.. code-block:: python

   from lru_ng import LRUDict


Exception
*********

.. py:exception:: LRUDictBusyError

   Raised by any :class:`LRUDict` method indicating the method call cannot
   begin because another method has not finished processing. This is a subclass
   of :exc:`RuntimeError`.

   This is typically a result of unsynchronized access in threaded code (see
   the section :ref:`thread-safety:thread safety`). In single-thread code, it
   may arise because a key's :meth:`~object.__hash__` or :meth:`~object.__eq__`
   methods cause a jump while in the middle of a method call.

   This exception will not be raised if :attr:`LRUDict._detect_conflict` is set
   to :data:`False`.


The :class:`LRUDict` object
***************************

.. py:class:: LRUDict(size : int, callback : Optional[Callable] = None)

   Initialize a :class:`LRUDict` object.

   :param int size: Size bound or "capacity" of the :class:`LRUDict` object.
                    Must be a positive integer that is no greater than
                    :data:`sys.maxsize`.
   :param callback: Callback object to be applied to displaced or "evicted"
                    key-value pairs.
   :type callback:  callable or :data:`None`
   :raises TypeError: if argument types do not match the intended ones.
   :raises ValueError: if :code:`size` is negative or zero.
   :raises OverflowError: if :code:`size` is greater than :data:`sys.maxsize`.


Properties
----------

.. py:method:: LRUDict.size
   :property:

   Get or set the size bound. This property can be set (i.e. assigned to) in
   order to resize the bound after initialization. If the size is reduced and
   is less than the current length (i.e. number of items stored), evictions
   will be triggred.

   :raises TypeError: if setting the size with a value that cannot be converted
                      to integer.
   :raises ValueError: if setting the size to a negative value or zero.
   :raises OverflowError: if setting the size to a value greater than
                          :data:`sys.maxsize`.
   :raises AttributeError: if attempting to delete the property.

.. py:method:: LRUDict.callback
   :property:

   Get or set the callback. This property can be set (i.e. assigned to) in
   order to change the callback after initialization. The value :data:`None`
   indicates that no callback is set, and setting it to :data:`None` disables
   callback upon eviction.

   :raises TypeError: if setting the callback to a non-callable object.
   :raises AttributeError: if attempting to delete the property.


Special methods for the mapping protocol
----------------------------------------

.. py:method:: LRUDict.__getitem__(self, key, /) -> Any

   Access the value associated with the key. Implement the indexing syntax
   :code:`L[key]`. On success, it also increases the hits counter and increases
   the misses counter on failure.

   :param hashable key: Hashable object.
   :return:             The value associated with :code:`key`.
   :raises KeyError:    if :code:`key` is not found.

.. py:method:: LRUDict.__setitem__(self, key, value, /) -> None

   Assign the value associated with the key. Implement the index-assignment
   syntax :code:`L[key] = value`. Do not modify the hit/miss counter.
 
   :param hashable key: Hashable object.
   :param value:        Object as value to be associated with :code:`key`.
   :return:             :data:`None`.

.. py:method:: LRUDict.__delitem__(self, key, /) -> None

  Delete the key and its associated value. Implement the :code:`del L[key]`
  syntax. Do not modify the hit/miss counter.

  :param hashable key: Hashable object.
  :return:             :data:`None`.
  :raises KeyError:    if :code:`key` is not found.

.. py:method:: LRUDict.__contains__(self, key, /) -> Bool

   Test key membership. Implement the :code:`key in L` or :code:`key not in L`
   syntax.

   :param hashable key: Hashable object.
   :return:             :data:`True` or :data:`False`.

.. py:method:: LRUDict.__len__(self, /) -> int

   Support for the builtin :func:`len` function.
 
   :return: The length of the :class:`LRUDict` object (number of items stored).


.. _dict-emulation:

Methods emulating :class:`dict`
-------------------------------

.. py:method:: LRUDict.clear(self, /) -> None

   Remove all items from the :class:`LRUDict` object and reset the hits/misses
   counter.

   :return: :data:`None`.

.. py:method:: LRUDict.get(self, key, default=None, /) -> Any

   If :code:`key` is in the :class:`LRUDict`, return the value associated with
   :code:`key` and increment the "hits" counter, if :code:`key` is in the
   object.  Otherwise, return the value of :code:`default` and increment the
   "missing" counter.

.. py:method:: LRUDict.setdefault(self, key, default=None, /) -> Any

   If :code:`key` is in the :class:`LRUDict`, return the value associated with
   :code:`key` and increment the "hits" counter (just like the :meth:`get`
   method).  Otherwise, return :code:`default`, *insert* the :code:`key`
   with the value :code:`default`, and return :code:`default`.

   .. note:: Like Python's :meth:`dict.setdefault`, the hash function for
             :code:`key` is evaluated only once.

.. py:method:: LRUDict.pop(self, key[, default], /) -> Any

   If :code:`key` is in the :class:`LRUDict`, return its associated value,
   remove this *(key, value)* pair, and increment the "hits" counter. If
   :code:`key` is not in the object, return :code:`default` if it is given as
   an argument; however, if :code:`default` is not given, raises
   :exc:`KeyError`. No matter whether :code:`default` is given, the "missing"
   counter is incremented as long as :code:`key` is not in the
   :class:`LRUDict`.

   :return: *(key, value)* pair.
   :raises KeyError: if :code:`key` is not stored but :code:`default` is not
                     given.

.. py:method:: LRUDict.popitem(self, least_recent : Bool = False, /) -> Tuple[Object, Object]

   Return a pair (two-element tuple) *key, value* and remove it from the
   storage. If the :class:`LRUDict` object is empty (hence no item to pop),
   raise :exc:`KeyError`.

   By default, The item popped is the  *most-recently* used (MRU) one. If the
   optional paramter :code:`least_recent` is :data:`True`, the *least-recently*
   used (LRU) one is returned instead.

   This method does not modify the hits/misses counters.

   :param bool least_recent: Boolean flag indicating the order of popping.
                             *Default:* :data:`False` (popping the MRU)
   :return:                  *(key, value)* pair.

   .. note:: The optional argument :code:`least_recent` is specific to
             :class:`LRUDict` and not present for :class:`dict`. It bears some
             similarity to Python's :meth:`collections.OrderedDict.popitem`,
             and the default order (LIFO) is the same despite different
             terminologies.

.. py:method:: LRUDict.update(self[, other,] /, *, **kwargs) -> None

   Update self with the key-value pairs from :code:`other`, a Python
   :class:`dict`, if the argument is present.  If keyword arguments are also
   given, further update self using them. This method may cause eviction if the
   update would have grown the length of self beyond the size limit. The update
   is performed in the iteration order of :code:`other` (which is the same as
   key-insertion order), and then the order of keyword arguments as they are
   specified in the method call if any.

   .. warning:: This method may make multiples passes into the critical section
                in order to consume the sources, and while :code:`self` is
                being updated the intermediate list of evicted items may grow
                (bound by the diffrence of source size and self size). It is
                preferrable to not having another thread modifying
                :code:`other` or :code:`self` while :code:`self` is being
                updated.

.. py:method:: LRUDict.has_key(self, key, /) -> Bool

   **Deprecated**. Use :code:`key in L` aka. :meth:`__contains__` instead.

The following methods are modelled with their counterparts of :class:`dict`,
but instead of returning a *dynamic view* object, they return a list,
which contains shallow copies, that reflects the state of the :class:`LRUDict`
object at the time of call. The items returned are in MRU-to-LRU order.
Accessing the items in the returned sequences will not modify the ordering of
items in the original :class:`LRUDict` object.

.. py:method:: LRUDict.keys(self, /) -> List

.. py:method:: LRUDict.values(self, /) -> List

.. py:method:: LRUDict.items(self, /) -> List[Tuple[Object, Object]]


Methods specific to :class:`LRUDict`
------------------------------------

.. py:method:: LRUDict.to_dict(self, /) -> Dict

   Return a new dictionary, :code:`other`, whose keys and values are shallow
   copies of self's. :code:`other`'s iteration order (i.e.  key-insertion
   order) is the same as :code:`self`'s recent-use (i.e. LRU-to-MRU) order.

   The method can be loosely considered an inverse of :meth:`update` when
   evictions are ignored. In other words, if :code:`L` in the following snippet
   is a :class:`LRUDict` object, :code:`L_dup` will be it's duplicate with the
   same recent-use order:

   .. code-block:: python

      d = L.to_dict()
      L_dup = LRUDict(len(d))
      L_dup.update(d)

   This can be used to dump a picklable copy if the keys and values are
   picklable. The pickle can be loaded into a dictionary and be used to update
   an empty :class:`LRUDict` object with sufficient size to restore the
   original content and order.

   :return: New dictionary.

.. py:method:: LRUDict.peek_first_item(self, /) -> Tuple[Object, Object]

   Return a tuple *(key, value)* of the *most-recently* used ("first") item, or
   raise :exc:`KeyError` if the :class:`LRUDict` is empty. The hits/misses
   counters are not modified.

   :return: *(key, value)* pair.
   :raises KeyError: if the :class:`LRUDict` is empty.

.. py:method:: LRUDict.peek_last_item(self, /) -> Tuple[Object, Object]

   Return a tuple *(key, value)* of the *least-recently* used ("last") item, or
   raise :exc:`KeyError` if the :class:`LRUDict` is empty. The hits/misses
   counters are not modified.

   :return: *(key, value)* pair.
   :raises KeyError: if the :class:`LRUDict` is empty.

.. py:method:: LRUDict.get_stats(self, /) -> Tuple[int, int]

   Return a tuple of integers, *(hits, misses)*, that provides feedback on the
   :class:`LRUDict` :ref:`hit/miss information <hits-and-misses:hits and
   misses>`.

   .. note:: On CPython >= 3.8, The specific type of the return value (named
             :class:`LRUDictStats`) is a built-in "struct sequence" (a subclass
             of :class:`tuple`), similar to named-tuple classes created by
             Python :func:`collections.namedtuple`. As such, it also supports
             accessing the fields by the attributes :code:`.hits` and
             :code:`.misses` respectively.

   .. warning:: The numerical values are stored as C :code:`unsigned long`
                internally and may wrap around to zero if overflown, although
                this seems unlikely.


Other special methods
---------------------

.. py:method:: LRUDict.__repr__(self, /) -> str

   Return a textual representation of the :class:`LRUDict` object. The output
   includes a preview of the stored key and values if the overall line is not
   too long, but the order of keys should not be presumed to be relevant.


Less-common and experimental methods
------------------------------------

.. py:method:: LRUDict.purge(self, /) -> int

   Purge internally-buffered evicted items manually.

   As an implementation detail, evicted items are temporarily buffered in the
   case that a callback is set when the items are evicted. Purging refers to
   clearing the buffer (with callback application if set). Normally, this
   happens without intervention, but in threaded environments there is no
   absolute guarantee that the buffer will always be cleared all the time. This
   method can be used to purge manually. As noted in
   :ref:`introduction:caveats with callbacks`, the purge will ignore the return
   value of the callback and suppress exceptions raised in there.

   :return: Number of items purged, or -1 in the case of error.

.. py:method:: LRUDict._suspend_purge
   :property:

   Set to :data:`True` to disable purging altogether. Set to :data:`False` to
   re-enable (but do not immediately trigger purging).

.. py:method:: LRUDict._purge_queue_size
   :property:

   Peek the length of the purge buffer. The value is indicative only and may
   not be accurate in a threaded environment.

.. py:method:: LRUDict._max_pending_callbacks
   :property:

   The maximum number of callbacks allowed to be pending on the purge queue.
   Must be an integer between 1 and C :code:`USHRT_MAX` (typically 65535).

.. py:method:: LRUDict._detect_conflict
   :property:

   Set to :data:`False` to disable runtime conflict detection. By doing so, no
   method will raise :exc:`LRUDictBusyError`.


Obsolete methods
----------------

.. py:method:: LRUDict.get_size(self, /) -> int
.. py:method:: LRUDict.set_size(self, size : int, /) -> None

   **Deprecated**. Use the property :data:`size` instead.

.. py:method:: LRUDict.set_callback(self, func : Callable, /) -> None

   **Deprecated**. Use the property :data:`callback` instead.
