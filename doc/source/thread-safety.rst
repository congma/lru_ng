..
   vim: spell spelllang=en

.. currentmodule:: lru_ng

.. highlight:: python

.. toctree::
   :maxdepth: 3


Thread safety
=============

The module relies on the :term:`global interpreter lock` (GIL) to maintain the
consistency of internal data structures. Internally, the building blocks are
mostly normal Python objects, although they're accessed and modified via the C
API. The GIL is expected to be held by the method caller, so that accesses are
ordered as a result of the GIL's functioning. When a thread is using a method,
it can be certain that no other thread is modifying the data at the same time.

However, there are four places where the protection is not perfect. These are
the :ref:`callback <introduction:using a callback>`, the key's
:meth:`~object.__hash__`/:meth:`~object.__eq__` methods (used by the internal
:class:`dict`), and the :meth:`~object.__del__` method on the key or value. In
the notes on :ref:`compatibility <difflru>` we have described the precaution
about these code paths that can in principle do anything.

Here, we should mention the qualities of these callable objects that makes the
programme overall safer. We will refer to them as "external code".

First, avoid releasing the GIL in the external code. The GIL is released, for
example, when input/output (I/O) is performed. Some code may drop the GIL when
computing the hash by a C extension on an internal :term:`buffer <bytes-like
object>` for speed. Even if :meth:`~object.__hash__` may be made to execute
before entering the critical section (relying on not-so-public Python C API),
:meth:`~object.__eq__` currently cannot be. When a thread-switch happens in the
middle of a method call, another thread may try to call into a method, too,
causing contention.  There's limited built-in ability to detect this at
run-time, but currently,

.. warning::

   No additional locking is performed,

because locking tend to degrade single-thread performance and necessarily make
the code more complex. Decisions about contention-handling (such as blocked
waiting, timed waiting, or other tactics) is best made by client code.

Second, avoid unwanted side effects in the external code. Modifying the
:class:`LRUDict` object itself is one such effect. Again, this can be detected
at runtime, but it is not preferable to rely on this.

Overall, callbacks and :meth:`~object.__del__` are mostly safe. However, the
timing and order of the callback execution cannot be guaranteed in general in a
threaded environment.


Summary
*******

.. note::

   1. As long as the key's :meth:`~object.__hash__` and :meth:`~object.__eq__`
      do not drop the GIL or modify the :class:`LRUDict`, usage in a
      threaded environment is generally safe.

   2. The callback and the :meth:`~object.__del__` method of keys or values are
      generally safe, but it is not preferable to modify the :class:`LRUDict`
      in there or clog up the purge queue by taking long computations.

   3. Limited ability to detect contention at runtime is provided (see
      :exc:`LRUDictBusyError`), and the user can implement their own
      synchronization.

   4. For single-thread programme it's "life as usual", and the GIL provides
      adequate protection.


Example: contention
*******************

In the following Python script, we simulate a (highly contrived) situation
where individual threads simply try to insert keys into a cache without
synchronization. The GIL is dropped in the :code:`__hash__` and :code:`__eq__`
methods of the key by taking some time to read from a file.

.. code-block:: python
   :linenos:
   :emphasize-lines: 15, 26, 36
   :caption: run_insertion.py -- simulate contention with key insertions

   import threading
   from lru_ng import LRUDict


   class Key:
       def __init__(self, value, read_blocks=128):
           self._v = value
           self.nbytes = read_blocks * 512

       def __hash__(self):
           h = hash(self._v)
           remaining = self.nbytes
           with open("/dev/urandom", "rb") as f:
               while remaining > 0:
                   remaining -= len(f.read(remaining))  # Release GIL
           return h

       def __eq__(self, other):
           try:
               v = other._v
           except AttributeError:
               v = other
           remaining = self.nbytes
           with open("/dev/urandom", "rb") as f:
               while remaining > 0:
                   remaining -= len(f.read(remaining))  # Release GIL
           return self._v == v


   cache = LRUDict(5)


   def runner(n):
       for i in range(n):
           k = Key(i)
           cache[k] = i  # Insertion without synchronization


   num_threads = 10
   num_keys = 1000
   threads = [threading.Thread(target=runner, args=(num_keys,))
              for i in range(num_threads)]
   for th in threads:
       th.start()
   for th in threads:
       th.join()

The program will output error messages like this::

    Traceback (most recent call last):
    ...
    File "run_insertion.py", line 36, in runner
      cache[k] = i  # Insertion without synchronization
    lru_ng.LRUDictBusyError: attempted entry into LRUDict critical section while busy

(The actual error messages themselves from all the threads may be out of order
too!) The threads are failing because of the unhandled error.

If the :code:`__hash__` or :code:`__eq__` methods had not released the GIL,
these errors would have been prevented.

By adding a lock around the insertion operation, we can make it "work" in the
sense of ensuring sequential access to the global cache. The following snippet
replacing the definition of :code:`runner()` should make the error messages
disappear:

.. code-block:: python
   :linenos:
   :lineno-start: 32
   :emphasize-lines: 1, 5, 7

   cond = threading.Condition()
   def runner(n):
       for i in range(n):
           k = Key(i)
           with cond:         # Wait on the underlying lock
               cache[k] = i
               cond.notify()  # Wake up one thread waiting on the lock
