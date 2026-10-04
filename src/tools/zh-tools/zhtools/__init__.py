"""
zhtools -- shared library for the REL_16_11_zh translation validation tools.

Everything in here is Python 3.9+ standard library only.  The modules are
deliberately small and mechanical: they prove facts about bytes (what is a
comment, what changed, what is protected) and never try to emulate a
compiler, a build system or gettext.
"""
import sys

# Never leave __pycache__ behind inside the source tree.
sys.dont_write_bytecode = True

TOOLS_VERSION = "0.1.0"
