"""
``--cc-check``: compiler-preprocessor token cross-check against the frozen
GNU Make VPATH evidence (schema version 2).

Inputs (in-repo, ``cc_evidence/``): ``baseline.json`` (status must be
``frozen``), ``flagsets.json`` + ``commands.jsonl`` (canonical argv per
variant), ``tokens.jsonl`` (baseline token / header-set / canonical-argv
hashes), ``platform_exceptions.json`` (optional, *reviewed* exceptions only).
Inputs (outside the repo, paths + SHA-256 recorded in ``baseline.json``):
the per-variant header lists ``headers.jsonl`` and the Make ``.Po`` header
superset ``po_headers.jsonl``.

For a set of translated ``.c/.h/.cpp`` paths the check

1. validates the evidence: frozen status, cache key recomputation, in-repo
   file hashes, bulk file hashes, frozen compiler/make versions, srcdir
   HEAD == base commit, builddir present;
2. selects every variant whose source *or* side-depfile header set contains
   the path (headers.jsonl index).  A path with no variant is **uncovered**
   -> UNVERIFIED unless it is a reviewed platform exception;
3. replays those variants with the frozen argv against the translated root
   (``ccreplay.replay_variants``) and compares token hash, header-set hash
   and canonical-argv hash with the baseline;
4. is ``verified`` only when every requested path is covered by at least one
   passing variant and no replayed variant differs or fails.

Generated TUs (source in the builddir) are replayed for their header
coverage but a generated *source* is never one of the requested paths (the
``.y/.l`` validator is separate and not implemented in this version).
Anything the check cannot establish mechanically is ``unverified``.
"""
import json
import os
import sys
import time

sys.dont_write_bytecode = True

from . import ccreplay, evidence_run as ev, makelog  # noqa: E402

SCHEMA_VERSION = 2
IMPLEMENTED = True


class CcCheckError(Exception):
    pass


def _git_head(path):
    rc, out = ev.capture(["git", "rev-parse", "HEAD"], cwd=path, env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
    return out.strip() if rc == 0 else None


def load_evidence(evidence_dir, check_tools=True):
    """Validate the evidence directory. Returns (ok, info, problems, data)."""
    problems = []
    info = {"dir": evidence_dir}
    data = {}
    if not evidence_dir or not os.path.isdir(evidence_dir):
        return False, info, ["evidence directory %r not found" % (evidence_dir,)], data
    bj = os.path.join(evidence_dir, "baseline.json")
    if not os.path.isfile(bj):
        return False, info, ["%s not found" % bj], data
    try:
        base = ev.read_json(bj)
    except (OSError, ValueError) as e:
        return False, info, ["cannot read %s: %s" % (bj, e)], data
    info["baseline"] = {k: base.get(k) for k in ("schema_version", "status", "base_commit", "cc", "cxx",
                                                 "cache_key_sha256", "n_variants", "coverage_closed")}
    if base.get("schema_version") != SCHEMA_VERSION:
        problems.append("schema_version %r != %d" % (base.get("schema_version"), SCHEMA_VERSION))
    if base.get("status") != "frozen":
        problems.append("baseline status is %r, not 'frozen'" % (base.get("status"),))
    if not base.get("coverage_closed"):
        problems.append("baseline .o/.Po/argv coverage is not closed")
    for key in ("base_commit", "srcdir", "builddir", "cc", "cxx", "cc_version", "cxx_version", "make_version",
                "configure_args", "env", "cache_key_sha256", "cache_key_material", "bulk_files"):
        if key not in base:
            problems.append("baseline.json missing %r" % key)
    if problems:
        return False, info, problems, data
    # cache key must recompute from its material
    try:
        if ccreplay.compute_cache_key(base["cache_key_material"]) != base["cache_key_sha256"]:
            problems.append("cache_key_sha256 does not recompute from cache_key_material")
    except ev.EvidenceError as e:
        problems.append(str(e))
    # in-repo files must match the material
    mat = base["cache_key_material"]
    for name, key in (("closure.json", "closure_sha256"), ("commands.jsonl", "commands_sha256"),
                      ("flagsets.json", "flagsets_sha256"), ("tokens.jsonl", "tokens_sha256")):
        p = os.path.join(evidence_dir, name)
        if not os.path.isfile(p):
            problems.append("%s not found" % name)
        elif ev.sha256_file(p) != mat.get(key):
            problems.append("%s does not match the frozen cache key material" % name)
    # bulk files outside the repo
    for name, b in base["bulk_files"].items():
        if not os.path.isfile(b["path"]):
            problems.append("bulk file %s missing: %s" % (name, b["path"]))
        elif ev.sha256_file(b["path"]) != b["sha256"]:
            problems.append("bulk file %s changed: %s" % (name, b["path"]))
    if check_tools:
        env = dict(base["env"])
        for comp, key in ((base["cc"], "cc_version"), (base["cxx"], "cxx_version")):
            rc, out = ev.capture([comp, "--version"], env=env)
            line = ev.first_line(out) if rc == 0 else None
            if line != base[key]:
                problems.append("%s reports %r, frozen %r" % (comp, line, base[key]))
        rc, out = ev.capture(["make", "--version"], env=env)
        if (ev.first_line(out) if rc == 0 else None) != base["make_version"]:
            problems.append("make version differs from frozen %r" % base["make_version"])
        head = _git_head(base["srcdir"])
        if head != base["base_commit"]:
            problems.append("srcdir %s HEAD %s != frozen base_commit %s" % (base["srcdir"], head, base["base_commit"]))
        if not os.path.isdir(base["builddir"]) or not os.path.isfile(
                os.path.join(base["builddir"], "src", "include", "pg_config.h")):
            problems.append("builddir %s missing or not configured" % base["builddir"])
    data["baseline"] = base
    return not problems, info, problems, data


def load_exceptions(evidence_dir):
    """Reviewed platform exceptions: path -> entry. Candidates are never used."""
    p = os.path.join(evidence_dir, "platform_exceptions.json")
    if not os.path.isfile(p):
        return {}
    doc = ev.read_json(p)
    out = {}
    for e in doc.get("exceptions", []):
        if e.get("status") != "approved":
            continue
        if not all(e.get(k) for k in ("path", "platform_guard", "reason", "reviewed_by", "review_commit")):
            continue
        out[e["path"]] = e
    return out


def build_index(evidence_dir, base):
    tokens = ccreplay.read_tokens(os.path.join(evidence_dir, "tokens.jsonl"))
    headers = ccreplay.read_headers(base["bulk_files"]["headers_base"]["path"], tokens)
    index = {}
    for key, t in tokens.items():
        if t.get("status") != "ok":
            continue
        if t["source_kind"] == "src":
            index.setdefault(t["source"], set()).add(key)
        for h in headers.get(key, []):
            index.setdefault(h, set()).add(key)
    return tokens, headers, index


def run(evidence_dir, paths, root, jobs=None, side_dir=None, check_tools=True):
    """Entry point used by verify_c_comments.py.

    *paths* are tree-relative ``.c/.h/.cpp`` paths that changed; *root* is
    the translated tree to preprocess.  Returns a dict with ``status`` in
    {"verified", "unverified", "violation"}.
    """
    t0 = time.time()
    result = {
        "status": "unverified",
        "implemented": IMPLEMENTED,
        "schema_version": SCHEMA_VERSION,
        "files_requested": len(paths),
        "files_verified": 0,
        "root": os.path.realpath(root) if root else None,
    }
    ok, info, problems, data = load_evidence(evidence_dir, check_tools=check_tools)
    result["evidence"] = info
    result["evidence_problems"] = problems
    if not ok:
        result["reason"] = "evidence invalid: " + "; ".join(problems)
        return result
    if not root or not os.path.isdir(root):
        result["reason"] = "no translated tree root (--cc-root) to preprocess"
        return result
    base = data["baseline"]
    try:
        tokens, headers, index = build_index(evidence_dir, base)
    except ev.EvidenceError as e:
        result["reason"] = "evidence invalid: %s" % e
        return result
    exceptions = load_exceptions(evidence_dir)

    selected = set()
    uncovered = []
    excepted = []
    per_file = {}
    for p in sorted(set(paths)):
        key = "SRC/" + p
        variants = index.get(key)
        if variants:
            selected |= variants
            per_file[p] = {"variants": sorted(variants)}
        elif p in exceptions:
            excepted.append(p)
            per_file[p] = {"variants": [], "platform_exception": exceptions[p]}
        else:
            uncovered.append(p)
            per_file[p] = {"variants": []}
    result["uncovered"] = uncovered
    result["platform_excepted"] = excepted
    result["variants_selected"] = len(selected)

    cfg = {"srcdir": base["srcdir"], "builddir": base["builddir"], "cc": base["cc"], "cxx": base["cxx"],
           "env": base["env"]}
    flagsets, recs = makelog.read_commands(evidence_dir)
    recs = [r for r in recs if r["variant_key"] in selected]
    side_dir = side_dir or os.path.join(os.environ.get("TMPDIR", "/tmp"), "zh-cc-check-%d" % os.getpid())
    results, prep_errors = ccreplay.replay_variants(cfg, flagsets, recs, root, side_dir,
                                                    jobs=jobs or os.cpu_count() or 1)
    diffs = []
    failed = [{"variant_key": r["variant_key"], "error": r.get("error") or r.get("stderr", "")[-800:]}
              for r in results if r["status"] != "ok"]
    passed = set()
    for r in results:
        if r["status"] != "ok":
            continue
        b = tokens[r["variant_key"]]
        kinds = []
        if r["token_sha256"] != b["token_sha256"]:
            kinds.append("tokens")
        if r["headers_sha256"] != b["headers_sha256"]:
            kinds.append("headers")
        if r["canonical_argv_sha256"] != b["canonical_argv_sha256"]:
            kinds.append("canonical_argv")
        if kinds:
            diffs.append({"variant_key": r["variant_key"], "kind": "+".join(kinds), "source": r["source"]})
        else:
            passed.add(r["variant_key"])
    for p, d in per_file.items():
        d["passing_variants"] = len([v for v in d["variants"] if v in passed])
    files_verified = sum(1 for p, d in per_file.items() if d["passing_variants"] > 0)
    result.update({
        "variants_replayed": len(results),
        "variants_passed": len(passed),
        "variants_failed": failed,
        "prep_errors": prep_errors,
        "variants_different": diffs,
        "files": per_file,
        "files_verified": files_verified,
        "elapsed_sec": round(time.time() - t0, 3),
        "cache_key_sha256": base["cache_key_sha256"],
    })
    if failed or prep_errors:
        result["status"] = "unverified"
        result["reason"] = "%d variant(s) failed to preprocess" % (len(failed) + len(prep_errors))
    elif uncovered:
        result["status"] = "unverified"
        result["reason"] = "%d file(s) not covered by any baseline variant and not a reviewed platform exception" % \
                           len(uncovered)
    elif diffs:
        result["status"] = "violation"
        result["reason"] = "%d variant(s) preprocess differently from the baseline" % len(diffs)
    else:
        result["status"] = "verified"
        result["reason"] = "%d file(s) covered by %d passing variant(s)%s" % (
            files_verified, len(passed), "; %d by reviewed platform exception" % len(excepted) if excepted else "")
    return result
