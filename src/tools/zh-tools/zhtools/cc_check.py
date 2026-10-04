"""
``--cc-check`` interface: compiler-preprocessor token cross-check.

STATUS IN THIS TOOL VERSION: interface and evidence schema only.

The real check (replaying the frozen Make VPATH compile commands with
``-E -P`` against the baseline and the translated tree and comparing
normalised tokens) needs evidence that can only be produced by the
baseline build batch:

  * ``make -j1 -w world-bin`` / ``make -j1 -w checkprep`` logs,
  * the ``.o`` / ``.deps/*.Po`` coverage set,
  * the canonical argv per translation-unit variant,
  * tool versions and the baseline token cache.

Until that evidence exists, ``run()`` MUST return ``status="unverified"``
so that the caller exits with code 2.  It never fabricates a pass.

Evidence directory layout (schema version 1, see also
``cc_evidence/README.md`` and ``cc_evidence/schema.json``):

  cc_evidence/
    baseline.json       {"schema_version": 1, "status": "frozen"|"draft",
                         "base_commit": ..., "cc": "/usr/bin/gcc",
                         "cxx": "/usr/bin/g++", "cc_version": ..., ...}
    commands.jsonl      one TU variant per line:
                         {"variant_key","source","cwd","argv","object",
                          "po_file","headers":[...]}
    tokens/<variant_key>.json   baseline token hash + depfile summary
"""
import json
import os
import sys

sys.dont_write_bytecode = True

SCHEMA_VERSION = 1
IMPLEMENTED = False


def load_evidence(evidence_dir):
    """Validate the evidence directory. Returns (ok, info_dict, problems)."""
    problems = []
    info = {"dir": evidence_dir}
    if not evidence_dir:
        return False, info, ["no --cc-evidence directory given"]
    bj = os.path.join(evidence_dir, "baseline.json")
    if not os.path.isfile(bj):
        return False, info, ["%s not found" % bj]
    try:
        with open(bj, "r", encoding="utf-8") as f:
            base = json.load(f)
    except (OSError, ValueError) as e:
        return False, info, ["cannot read %s: %s" % (bj, e)]
    info["baseline"] = {k: base.get(k) for k in ("schema_version", "status", "base_commit", "cc", "cxx")}
    if base.get("schema_version") != SCHEMA_VERSION:
        problems.append("schema_version %r != %d" % (base.get("schema_version"), SCHEMA_VERSION))
    if base.get("status") != "frozen":
        problems.append("baseline status is %r, not 'frozen'" % (base.get("status"),))
    for key in ("base_commit", "cc", "cxx", "cc_version", "make_version", "configure_args"):
        if key not in base:
            problems.append("baseline.json missing %r" % key)
    cj = os.path.join(evidence_dir, "commands.jsonl")
    if not os.path.isfile(cj):
        problems.append("commands.jsonl not found")
    if not os.path.isdir(os.path.join(evidence_dir, "tokens")):
        problems.append("tokens/ directory not found")
    return not problems, info, problems


def run(evidence_dir, paths):
    """Entry point used by verify_c_comments.py.

    Always returns a dict with ``status`` in {"verified", "unverified"},
    ``reason`` and ``evidence``.  In this version status is always
    "unverified".
    """
    ok, info, problems = load_evidence(evidence_dir)
    result = {
        "status": "unverified",
        "implemented": IMPLEMENTED,
        "schema_version": SCHEMA_VERSION,
        "evidence": info,
        "evidence_problems": problems,
        "files_requested": len(paths),
        "files_verified": 0,
    }
    if not IMPLEMENTED:
        result["reason"] = ("compile-command replay is not implemented in this tool version; "
                            "Make VPATH baseline evidence has not been established")
        return result
    # (future) replay goes here; must set status="verified" only when every
    # requested file is covered by at least one passing TU variant.
    result["reason"] = "evidence invalid: %s" % "; ".join(problems) if problems else "not run"
    return result
