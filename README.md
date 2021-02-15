<!--
vim: spell spelllang=en
-->
[![GitHub CI status](https://github.com/congma/lru_ng/workflows/GitHub%20CI/badge.svg)](https://github.com/congma/lru_ng/actions?query=workflow%3A%22GitHub+CI%22)
[![Travis-CI.com status](https://travis-ci.com/congma/lru_ng.svg?branch=devel)](https://travis-ci.com/congma/lru_ng)


Synopsis
========

`lru_ng` provides a Python type for fixed-size, ``dict`` like mapping object
`lru_ng.LRUDict` that performs cache replacement in the least-recently used
(LRU) order.

This is a fork of the original module
[`lru`](https://github.com/amitdev/lru-dict). The new iteration of the module
is intended for better thread-safety, more robust performance, and greater
compatibility with Python 3.

Limited compatibility with `lru` can be achieved by the Python import

```python
from lru_ng import LRUDict as LRU
```

For more information, please consult the documentation and `pydoc lru_ng`.


Documentation
=============

The documentation written in reStructuredText is contained in the `doc/source`
directory. Although readable as such, you can build the HTML pages using
[Sphinx](https://www.sphinx-doc.org/en/master/) and read them in a Web browser
locally. The following commands install `sphinx` and supporting packages, then
build the pages:

```shell
pip install sphinx sphinx_rtd_theme
cd doc && make html
```

The pages are now under the `doc/build/html` path, and the home page is
`index.html`.


Installation
============

Requirements:

- CPython >= 3.6.
- A C compiler that supports C99 syntax.
- Python development headers.
- [`setuptools`](https://github.com/pypa/setuptools)

In the repository directory, running the command

```shell
pip install .
```

will build and install the package.