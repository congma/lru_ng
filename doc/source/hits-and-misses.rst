..
   vim: spell spelllang=en

.. highlight:: python

.. currentmodule:: lru_ng


Hits and misses
===============

This is a summary of what constitutes a "hit" or a "miss", for which the
information is scattered around in the :ref:`api-reference:api reference`.

.. note::
   In general,

   * a *hit* refers to the successful retrieval of a value associated
     with a given key from the :class:`LRUDict` object itself, and

   * a *miss* refers to one such attempt that fails.

An "insert"-like method typically neither hits nor misses, and neither does
deleting a key-value pair (no matter whether the key exists). For instance, the
index-assignment statement

.. code-block:: python

   L["watermelon"] = "green"

neither hits nor misses no matter what. The :code:`del`-index statement

.. code-block:: python

   del L["lemon"]

does not affect hit/miss stats either, even if it may fail and raise
:exc:`KeyError`.

Somewhat special is the :meth:`~LRUDict.setdefault` method that may either
"retrieve" from the :class:`LRUDict` -- in the case that the given key is "in";
or "insert" in it -- in the case that there is not yet a key in it. The former
counts as a hit, because *the* value associated with the given key is indeed
obtained from the :class:`LRUDict`.  However, the latter is not a miss, because
it is an insert-only operation: no attempt has been made to retrieve the value
associated with the given key.

Merely testing for membership using the :code:`in`/:code:`not in` operator
neither hits nor misses.

A miss is not always associated with a :exc:`KeyError`. An example is the
:meth:`~LRUDict.get` method. The statement

.. code-block:: python

   colour = L.get("apple", "red")

incurs a miss if the key :code:`"apple"` is :code:`not in L`, and it will not
raise :exc:`KeyError`. The substitute value :code:`"red"`, though assigned to
the variable :code:`colour`, is *missed* by :code:`L`.

The methods :meth:`~LRUDict.popitem`, :meth:`~LRUDict.peek_first_item`, and
:meth:`~LRUDict.peek_last_item` neither hit nor miss, because they do not
accept a key.

The tallying of hits/misses is orthogonal to other side effects. For example,
the :meth:`~LRUDict.pop` is a "destructive" way to retrieve a value. It can hit
or miss, and in the case of a hit the item is removed from the
:class:`LRUDict`. The fact that a hit is scored does not imply that the item is
"still there".

It is safe to say that a key, if hit and extant, will always be promoted to the
most-recently used one, if it is not so already. The converse is not true:
assigning a value to an existing key will promote the key to the most recent,
but it is not considered a hit.
