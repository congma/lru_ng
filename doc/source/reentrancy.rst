..
   vim: spell spelllang=en

.. currentmodule:: lru_ng

.. highlight:: python

.. toctree::
   :maxdepth: 3


Re-entrancy
===========

"Re-entrancy" is a concept related to but distinct from :ref:`"thread-safety"
<thread-safety:thread safety>`. In our context it means the ability to call
into the code for :class:`LRUDict` while another such call has not returned.
The pending call's caller is not necessarily another thread: it can be a
different part of the same thread's code, for example a :term:`coroutine` that
has yielded at some point.

.. note::
   In summary,

   * the callback's action is "mostly" re-entrant but no more reentrant than
     the callback itself, while
   * for the methods, re-entering is an exception.


Actions of the callback
***********************

The :attr:`~LRUDict.callback` is an optional callable that is :ref:`called
automatically <introduction:using a callback>` on each evicted item (the
key-value pair removed because the capacity is reached). Although integrated
into the workings of the :class:`LRUDict` object, the callback is beyond its
control and subject to :ref:`caveats <introduction:caveats with callbacks>`.
It is generally not up to the inner workings of :class:`LRUDict` to handle
every situation where the callback may misbehave; this is a consequence of
Rice's Theorem.

Nevertheless, a degree of re-entrancy pertaining to the action of callbacks is
supported, provided that

1. every entrance into the body of :class:`LRUDict` methods is eventually
   paired with an exit, and that
2. the callback is otherwise re-entrant in and by itself.

It is up to the programmer to ensure that the callback should behave thus.

Rule 1 means that the callback is *allowed* to redirect the control flow towards
re-entering the :class:`LRUDict` object while it is being called by such an
object upon item eviction. This is obviously an error-prone situation. It is
partially mitigated by hitting Python's recursion (or more precisely,
call-stack) limit, which is understood by :class:`LRUDict`'s internal routines
and propagated whenever possible. For coroutines, if too many of them yield
inside the callback before returning, there's still the hard limit of
:code:`USHRT_MAX` currently-pending entrances (typically 65535). If the
current-pending counter is about to be saturated, the purge will simply be
skipped.

As an illustration, it is very easy to write an "amplification attack" version
of the callback, where for each evicted key it inserts more unique keys back
in. The rate of amplification can go exponential and beyond:

.. _amp-attack:

.. code-block:: python

   from lru_ng import LRUDict


   def cb(key, value):
       for i in range(cb.version):
           cb.version += 1
           key = "callback-%d" % i
           cb.victim[key] = i


   r = LRUDict(1, callback=cb)
   cb.version = 1
   cb.victim = r
   r[0] = 0
   r[1] = 1

This is not considered a valid way to use the module.


Trade-off with multiple pending callbacks
-----------------------------------------

(WIP)

The :class:`LRUDict` instance has a private property,
:attr:`~LRUDict._max_pending_callbacks`, that can be fine-tuned to allow for
greater control over callback execution. This is normally not required, but in
certain cases the default (a fairly large value, 8192) may not work well, and a
much lower limit may be desirable.

One of the possible situations is the case when, in a multi-threaded
environment, the callback performs GIL release-acquire cycles (typically by
doing I/O). If there's a large number of them pending at the same time, each
failure to acquire the GIL causes blocking for the thread. Since only one
thread can acquire the GIL, most threads already executing in the callback will
still spend time waiting in the callback, and little progress can be made
overall.

In this particular scenario, lowering :attr:`~LRUDict._max_pending_callbacks`
helps. If the pending-callback count has already saturated, any new entrance
into the "purge" section will not touch the evicted items in the queue, but
instead returns almost immediately (i.e. progress is made). The queue will
eventually be cleared, if not by calling :meth:`~LRUDict.purge` explicitly.

However, there are two major downsides of lowering the
:attr:`~LRUDict._max_pending_callbacks`:

1. The queue will not be purged as aggressively, so sometimes it may be
   worthwhile to check if there are stuck items.
2. If the callback behaves like the ":ref:`amplification attack <amp-attack>`"
   example above, it will likely evade the recursion limit, because the calls
   that could have been indirect recursions are "consumed" when the
   pending-callback counter saturates.

No. 2 may sound counter-intuitive. The :ref:`reason <why-circumvent>` is given
at the end of this page, for it is not very relevant to "normal", well-behaving
callbacks. Notice that this doesn't mean a smaller max-callback limit always
serves to curb runaway callback at runtime. It's not difficult to contrive a
counterexample.

In summary, these are the pros and cons of using large/small max-callback
bounds:

+--------------------------+------------------------+-----------------------+
|  Callback behaviour      |      Small bound       |      Large bound      |
+==========================+========================+=======================+
| Single-thread, plain     |                 ✅ (No impact)                 |
+--------------------------+------------------------+-----------------------+
| Single-thread, coroutine | ❌ May miss some purges|  ✅ No extra issue    |
+--------------------------+------------------------+-----------------------+
| Multi-thread, I/O bound  | ✅ Better concurrency  | ❌ High GIL contention|
+--------------------------+------------------------+-----------------------+
| Runaway callback         | ❌ All bets are off; situation-dependent       |
+--------------------------+------------------------+-----------------------+


Normal method access
********************

Normally, the maintenance of the element's use-order means that full
re-entrancy can be costly, since every operation may cause a reordering. If,
for example, the mere insertion of a key

.. code-block:: python

   L[key] = value

causes another call into :code:`L`'s methods while the insertion is incomplete,
then that new call must be able to work on sensible data that may have been
partially modified by the previous (but pending) call, and that the first call
(key insertion) must understand the possible mutations to the state in the wake
of the latter one, *and* also decide on a meaningful way to proceed.

The complexity to support this kind of operation means that currently, we
detect this sort of re-entry and raise the :exc:`LRUDictBusyError` exception.
It is up to the caller to decide what to do -- maybe to try again after some
timeout, or abandon the operation, or perform some other action instead.

In practice, the kind of "re-entering" prevented this way is almost always
caused by a key's :meth:`~object.__hash__` or :meth:`~object.__eq__`
*redirecting the control flow* in such a way that makes the :class:`LRUDict`
instance accessed again while already in the process of interacting with the
key. This is typically the result of programming error, but possible reasons
include GIL-dropping in the implementation of these methods, which may arise
from complicated interaction with C-extensions.

The benefit to be gained from supporting full re-entrancy (beside raising
exceptions) seems minimal. If you know how to achieve this in a cost-efficient
manner, please help!


.. _why-circumvent:

Appendix: why max-callback limit may circumvent stack limit
***********************************************************

(Implementation details inside)

The reason is that the call tree looks like this in the "amplification attack"
example::

    insert
      evict
      purge
        callback
          insert
            .
             .
              .

If the bound on pending callbacks is not hit, the call stack goes ever deeper
and eventually hits the stack (recursion) limit.

However, if a callback may be bypassed because too many have been pending, the
call tree will get flattened, turning the unbound recursion into infinite
loop::

    insert
      evict
      purge
        callback    [1]
          insert
            evict
            purge   [callback skipped, too many pending]
          insert
          .
          .
          .
          [... inescapable loop without growing deeper ...]

Even if the callback marked as :code:`[1]` eventually returns by running out of
insertion operations in its body, the next call into the callback may cause an
even larger number of insertions because there is an even larger number of
enqueued items to call on. Worse, there can always be written a sufficiently
bad callback that circumvents restrictions. The only solution is to not use
such a callback.
