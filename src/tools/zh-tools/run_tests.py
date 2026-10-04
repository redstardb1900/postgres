#!/usr/bin/env python3
"""Run the zh-tools unittest suite without leaving __pycache__ behind.

Usage:  python3 run_tests.py [-v] [pattern]
"""
import os
import sys

sys.dont_write_bytecode = True
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")

import unittest  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main(argv):
    verbosity = 1
    pattern = "test_*.py"
    for a in argv:
        if a == "-v":
            verbosity = 2
        else:
            pattern = a
    suite = unittest.defaultTestLoader.discover(os.path.join(HERE, "tests"), pattern=pattern, top_level_dir=HERE)
    result = unittest.TextTestRunner(verbosity=verbosity).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
