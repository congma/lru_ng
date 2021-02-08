try:
    from setuptools import Extension, setup
except ModuleNotFoundError:
    from distutils.core import Extension, setup


modextension = Extension("lru_ng",
                         sources=["src/lrudict.c"],
                         depends=["src/lrudict_exctype.h",
                                  "src/lrudict_statstype.h"])


setup(name="lru_ng",
      version="2.0.0",
      description=("Fixed-size dict with least-recently used (LRU)"
                   " replacement policy and optional callback."),
      author="Cong Ma",
      author_email="m.cong@protonmail.ch",
      url="https://github.com/congma/lru_ng",
      license="MIT",
      keywords=["mapping", "container", "dict", "cache", "lru"],
      ext_modules=[modextension],
      classifiers=[
          "Development Status :: 4 - Beta",
          "Intended Audience :: Developers",
          "License :: OSI Approved :: MIT License",
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
