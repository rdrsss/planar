Contributing
============

This is a small internal text-utility library used for automated testing.
It intentionally does not follow this workspace's usual conventions: there
is no Makefile, no AGENTS.md, and no docs/ tree, and its tests run through
``./run-tests`` rather than ``make test``.

Guidelines
----------

* Run ``./run-tests`` before submitting a change. It must exit 0.
* Keep ``textkit.py`` dependency-free (standard library only).
* Do not edit ``STATUS.txt`` by hand; it is a build marker written by the
  review process.
