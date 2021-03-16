try:
    from setuptools import Extension, setup
except ModuleNotFoundError:
    from distutils.core import Extension, setup


modextension = Extension("lru_ng",
                         sources=["src/lrudict.c",
                                  "src/lrudict_pq.c"],
                         depends=["src/lrudict.h",
                                  "src/tinyset.c",
                                  "src/lrudict_exctype.h",
                                  "src/lrudict_statstype.h",
                                  "src/lrudict_pq.h"])


setup(name="lru_ng",
      version="2.0.0",
      description=("Fixed-size dict with least-recently used (LRU)"
                   " replacement policy and optional callback."),
      long_description=("The C-extension module lru_ng provides a class"
                        " LRUDict that is compatible with a large subset of"
                        " methods in Python's dict. It maintains recent-access"
                        " order and enforces a fixed size, discarding the"
                        " least-recently used items when the capacity is"
                        " filled.\n"
                        "This extension class is largely compatible with the"
                        " lru.LRU type."),
      platforms="OS Independent",
      author="Cong Ma",
      author_email="m.cong@protonmail.ch",
      url="https://github.com/congma/lru_ng",
      license="GNU GPL 3",
      keywords=["mapping", "container", "dict", "cache", "lru"],
      ext_modules=[modextension],
      classifiers=[
          "Development Status :: 4 - Beta",
          "Intended Audience :: Developers",
          "License :: OSI Approved :: GNU General Public License v3 (GPLv3)",
          "Operating System :: OS Independent",
          "Programming Language :: C",
          "Programming Language :: Python :: 3",
          "Programming Language :: Python :: 3 :: Only",
          "Programming Language :: Python :: 3.6",
          "Programming Language :: Python :: 3.7",
          "Programming Language :: Python :: 3.8",
          "Programming Language :: Python :: 3.9",
          "Programming Language :: Python :: Implementation :: CPython",
          "Topic :: Software Development :: Libraries :: Python Modules"])
