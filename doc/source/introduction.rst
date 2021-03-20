..
   vim: spell spelllang=en

.. highlight:: python

.. currentmodule:: lru_ng

.. toctree::
   :maxdepth: 3


Introduction
============

The module :mod:`lru_ng` provides a single Python type,
:class:`~lru_ng.LRUDict` that behaves like a normal Python dictionary
:class:`dict` by providing key-value mapping.

In addition, its capacity is bounded. When the size bound is reached, inserting
a new key will cause the least-recently used key (and its associated value) to
be removed (or "evicted") from the mapping. The :class:`LRUDict` can be
resized, and reducing its size will remove keys in the order from the least- to
the most-recent use.

Optionally, a *callback* can be registered for displaced items. The callback is
applied to the *(key, value)* pair, and can be used for a variety of purposes
such as freeing up resources, printing information, or queueing up the
discarded items for reuse, among others.


Installation
************

The latest version's source is available from the PyPI, and you can install it
using the ``pip`` command:

.. code-block:: sh

   pip install lru_ng

The ``pip`` utility will download the C source and try to build a binary
distribution. For this to work, you need CPython >= 3.6. The following are
required for compilation:

* A C compiler that supports C99 syntax
* Python development headers
* `setuptools <https://pypi.org/project/setuptools/>`_, although
  :mod:`distutils` included with Python should work, too.


First example
*************

An :class:`LRUDict` object is created by specifying its fixed size bound. In
this example we begin with a size of 4 items.

.. code-block:: python

   >>> from lru_ng import LRUDict
   >>> cache = LRUDict(3)

The object :code:`cache`, in many ways, will behave just like a Python
:class:`dict`. It can be indexed, and you can test for membership using the
:code:`in` keyword.

.. code-block:: python

   >>> cache["system"] = "programming"
   >>> cache["web"] = "development"
   >>> "technology" in cache
   False
   >>> cache["documentation"]
   Traceback (most recent call last):
     ...
   KeyError: 'documentation'

By incurring a :exc:`KeyError` exception, we have caused a *cache miss*, which
is recorded.

.. code-block:: python

   >>> # For Python >= 3.8
   >>> "hits: {0.hits}; misses: {0.misses}".format(cache.get_stats())
   'hits: 0; misses: 1'
   >>> # For Python < 3.8, use
   >>> "hits: {}; misses: {}".format(*cache.get_stats())
   'hits: 0; misses: 1'

Successful retrieval of a value by key increases the "hits" record by one:

.. code-block:: python

   >>> word = cache["system"]
   >>> hits, misses = cache.get_stats()
   >>> hits
   1

When more keys are inserted and the capacity reached, items are displaced by
starting from the least-recently used.

.. code-block:: python

   >>> for phrase in ("documentation writing", "software testing"):
   ...     topic, action = phrase.split(" ")
   ...     cache[topic] = action
   >>> print(cache)
   <LRUDict(3) object with dict {'system': 'programming', 'documentation': 'writing', 'software': 'testing'} at 0x...>
   >>> len(cache)
   3

The item for :code:`"web development"` has been removed from :code:`cache`. The
item is the first to be discarded because :code:`"web"` is the least recently
used key.


Using a callback
****************

A *callback* is a callable object with two arguments for :code:`(key, value)`.
If it is set, it will apply to displaced (or "evicted") key-value pairs. The
callback can be accessed or set via the :data:`LRUDict.callback` property.

.. code-block:: python

   >>> def func(key, value):
   ...     print(f"discarded: ({key}, {value})")
   ...     print("total length: ", len(key) + len(value))
   >>> cache.callback = func
   >>> cache["quality"] = "assurance"
   discarded: (system, programming)
   total length:  17

This is a rather trivial illustration. A more useful situation for the callback
would be, for example, where the values are objects that should be cleaned up
before reaching the end of lifecycle. If the values stored are file-like
objects, closing them would be a suitable task for a callback.

The callback can also be specified during the instance initialization as an
optional parameter:

.. code-block:: python

   >>> cache = LRUDict(size, callback=func)


Caveats with callbacks
**********************

Although the callback *may* execute arbitrary code, certain restrictions are
imposed on its action. Most notably:

.. warning::

   * The return value of the callback is discarded.
   * Any exception raised in the callback cannot be handled the usual way.

What the latter means is that, if the callback would have raised an exception,
the exception is passed to the "unraisable handler"
:obj:`sys.unraisablehook` which may be further customized. Python's default
unraisable handler will print a message with the traceback to
:obj:`sys.stderr`.

.. note::

   It is strongly suggested that a callback should only

   * do what is minimally necessary,
   * avoid lengthy computation or substantial interaction with an external
     system, and
   * especially avoid modifying the state of the :class:`LRUDict` object
     itself.

Because of the loss of return value and suppression of exceptions, it has
limited capability to interact with other parts of the programme.

The most significant reason why it works like this is that any method call that
"adds something" to the :class:`LRUDict` object may trigger the
eviction-handling callback. The caller may not be well-equipped to handle any
exception raised; indeed, we cannot assume that the caller ever knows or should
know what, if any, is being evicted.

Also, it is rather likely that the evicted object will go beyond the reach of
normal code if no other references to it are held. The callback in this way
shares some similarity with the :meth:`~object.__del__` method (or
"finalizer"). However, unlike :meth:`~object.__del__`, the callback is not
bound to the object about to be discarded.
