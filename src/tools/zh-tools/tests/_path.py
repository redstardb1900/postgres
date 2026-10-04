"""Make the zh-tools directory importable from the tests package."""
import os
import sys

sys.dont_write_bytecode = True

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

FIXTURES = os.path.join(TOOLS_DIR, "tests", "fixtures")
