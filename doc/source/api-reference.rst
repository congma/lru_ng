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
   may arise because a key's :meth:`~object.__hash__` or :meth:`__eq__` methods 
   cause a jump while in the middle of a method call.

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
   :code:`L[key]`. On success, also increases the hits counter and increases the
   misses counter on failure.

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

.. py:method:: LRUDict.get(self, key, default=None, /)

   If :code:`key` is in the :class:`LRUDict`, return the value associated with
   :code:`key` and increment the "hits" counter, if :code:`key` is in the
   object.  Otherwise, return the value of :code:`default` and increment the
   "missing" counter.

.. py:method:: LRUDict.setdefault(self, key, default=None, /)

   If :code:`key` is in the :class:`LRUDict`, return the value associated with
   :code:`key` and increment the "hits" counter (just like the :meth:`get`
   method).  Otherwise, return :code:`default` and *insert* the :code:`key`
   with the value :code:`default`.

   .. note:: Like Python's :meth:`dict.setdefault`, the hash function for
             :code:`key` is evaluated only once.

.. py:method:: LRUDict.pop(self, key[, default], /)

   If :code:`key` is in the :class:`LRUDict`, return its associated value,
   remove this *(key, value)* pair, and increment the "hits" counter. If
   :code:`key` is not in the object, return :code:`default` if it is given as
   an argument; however, if :code:`default` is not given, raises
   :exc:`KeyError`. No matter whether :code:`default` is given, the "missing"
   counter is incremented as long as :code:`key` is not in the
   :class:`LRUDict`.

   :raises KeyError: if :code:`key` is not stored but :code:`default` is not
                     given.

.. py:method:: LRUDict.popitem(self, least_recent : Bool = False, /) -> Tuple[Object, Object]

   Return a pair (two-element tuple) *key, value* and remove it from the
   storage. If the :class:`LRUDict` object is empty (hence no item to pop),
   raise :code:`KeyError`.

   The item removed is the least-recently used one if the optional paramter
   :code:`least_recent` is :data:`True`. Otherwise, the *most-recently* used
   one is returned (which is the default behaviour).

   This method does not modify the hits/misses counters.

   :param bool least_recent: Boolean flag indicating the order of popping.
                             *Default:* :data:`False`
   :return:                  *(key, value)* pair.

   .. note:: The optional argument :code:`least_recent` is specific to
             :class:`LRUDict` and not present for :class:`dict`.

.. py:method:: LRUDict.update(self, other={}, /, *, **kwargs) -> None

   Update self with the key-value pairs from :code:`other`, a Python
   :class:`dict`, if the argument is present.  If keyword arguments are also
   given, further update self using them. This method may cause eviction if the
   update would have grown the length of self beyond the size limit. The update
   is performed in the iteration order of :code:`other`, and then the order of
   keyword arguments as they are given in the method call if any.

.. py:method:: LRUDict.has_key(self, key, /) -> Bool

   **Deprecated**. Use :code:`key in L` aka. :meth:`__contains__` instead.


Methods specific to :class:`LRUDict`
------------------------------------

.. py:method:: LRUDict.peek_first_item(self, /) -> Tuple[Object, Object]

   Return a tuple *(key, value)* of the *most-recently* used ("first") item, or
   raise :exc:`KeyError` if the :class:`LRUDict` is empty. The hits/misses
   counters are not modified.

   :raises KeyError: if the :class:`LRUDict` is empty.

.. py:method:: LRUDict.peek_last_item(self, /) -> Tuple[Object, Object]

   Return a tuple *(key, value)* of the *least-recently* used ("last") item, or
   raise :exc:`KeyError` if the :class:`LRUDict` is empty. The hits/misses
   counters are not modified.

   :raises KeyError: if the :class:`LRUDict` is empty.

.. py:method:: LRUDict.get_stats(self, /) -> Tuple[int, int]

   Return a tuple of integers, *(hits, misses)*, that provides feedback on the
   :class:`LRUDict` :ref:`hit/miss information <hits-and-misses:hits and
   misses>`. The specific type of the return value (named
   :class:`LRUDictStats`) is a built-in "struct sequence", similar to
   named-tuple classes created by Python :func:`collections.namedtuple`. As
   such, it also supports accessing the fields by the attributes :code:`.hits`
   and :code:`.misses` respectively.

   .. warning:: The numerical values are stored as C :code:`unsigned long`
                internally and may wrap around to zero if overflown.


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

   :return: Number of items purged.

.. py:method:: LRUDict._suspend_purge
   :property:

   Set to :data:`True` to disable purging altogether. Set to :data:`False` to
   re-enable (but do not immediately trigger purging).

.. py:method:: LRUDict._purge_queue_size
   :property:

   Peek the length of the purge buffer. The value is indicative only and may
   not be accurate in a threaded environment.

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
