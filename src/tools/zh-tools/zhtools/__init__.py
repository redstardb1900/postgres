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

# 0.2.0: GNU Make evidence chain (make_evidence.py, makelog, ccreplay,
# checkworld, evidence_run) and an implemented --cc-check.
# 0.2.1: freeze hashes the configure / check-world / config.log evidence too
# and gains a read-only `freeze --verify`; platform candidates are reported,
# never approved.
# 0.2.2: freeze fails if either config.log is missing (not only if the
# evidence_out copy is missing); PO supplement entries must carry
# `# zh-reviewed:` and leftover `# zh-review-pending` is a hard fail.
TOOLS_VERSION = "0.2.2"
