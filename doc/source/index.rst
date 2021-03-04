..
   vim: spell spelllang=en
..
   lru_ng documentation master file, created by
   sphinx-quickstart on Mon Feb  8 13:05:34 2021.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

.. highlight:: python

:mod:`lru_ng`: Mapping with LRU replacement and callback
========================================================

:mod:`lru_ng` is a Python module based on the original :code:`lru` `module by
Amit Dev <https://github.com/amitdev/lru-dict>`_. It features better support
for a threaded environment, greater control over callback execution, and other
internal bug-fixes and improvements.

In most situations, drop-in compatibility with :code:`lru.LRU` can be achieved
by the following :code:`import` statement in Python code:

.. code-block:: python

   from lru_ng import LRUDict as LRU

Differences from :code:`lru.LRU` are described in the
:ref:`compatibility:compatibility` section.

The code is optimized for fast insertion, deletion, and eviction. The extension
class participates in Python's cycle garbage collection.


.. toctree::
   :numbered:
   :maxdepth: 2
   :caption: Contents:

   introduction
   api-reference
   hits-and-misses
   compatibility
   thread-safety
   reentrancy
   performance


Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
