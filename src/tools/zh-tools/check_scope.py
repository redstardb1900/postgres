#!/usr/bin/env python3
"""
check_scope.py -- prove that a batch changed nothing outside its manifest
and nothing inside the protected areas (doc/ and the Perl protected set).

Targets (exactly one):
  --worktree ROOT      compare the base commit with the working tree in ROOT,
                       covering tracked changes, untracked files AND ignored
                       files (git ls-files --others [--ignored])
  --ref REF            compare the base commit with commit REF (use --repo)

Required inputs:
  --manifest FILE      scope manifest (schema v1, see manifests/SCHEMA.md)
  --baseline-dir DIR   baseline.json / doc_snapshot.json / perl_protected.json
                       (default: baseline/ next to this script)

Modes:
  (default)            cumulative: diff against the frozen base commit; the
                       manifest must describe everything changed since
                       REL_16_11 (used for the final acceptance)
  --incremental        per-batch: diff against the manifest's parent_commit
                       (must descend from the frozen base); doc/ and Perl
                       checks are still absolute against the frozen baseline

Checks (violation ids):
  S1_BASE_MISMATCH     base ref does not resolve to the frozen base commit
                       (or, with --incremental, to the manifest parent_commit)
  S2_OUTSIDE_MANIFEST  a changed/untracked/ignored path matches no
                       allowed_paths pattern
  S2_NO_FILE_ENTRY     changed path has no files[] entry (when the manifest
                       sets require_file_entries) or the action disagrees
  S3_DOC_TREE          HEAD/REF:doc tree OID differs from the baseline
  S3_DOC_SNAPSHOT      on-disk doc/ snapshot differs from the baseline
  S3_DOC_DIFF          git diff against the base commit touches doc/
  S4_PERL_CHANGED      a Perl protected file differs (blob OID or mode)
  S4_PERL_MISSING      a Perl protected file is missing
  S4_PERL_NEW          a new Perl file (by extension or shebang) appeared

Exit codes: 0 pass, 1 violations, 2 unverified (bad inputs / git error).
"""
import argparse
import json
import os
import stat
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, gitutil, manifest as manifest_mod, protection  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASELINE = os.path.join(HERE, "baseline")


class Scope(object):
    def __init__(self):
        self.violations = []
        self.info = {}

    def add(self, rule, path, message):
        self.violations.append({"rule": rule, "path": path, "message": message})


def load_baseline(d):
    out = {}
    for name in ("baseline.json", "doc_snapshot.json", "perl_protected.json"):
        p = os.path.join(d, name)
        with open(p, "r", encoding="utf-8") as f:
            out[name] = json.load(f)
    b = out["baseline.json"]
    for key in ("base_commit", "doc_tree"):
        if key not in b:
            raise ValueError("baseline.json missing %s" % key)
    if out["doc_snapshot.json"].get("base_commit") != b["base_commit"] or \
            out["perl_protected.json"].get("base_commit") != b["base_commit"]:
        raise ValueError("baseline files refer to different base commits")
    return out


def collect_changes_worktree(root, base):
    """Return list of (status, path, origin) for the working tree vs base.

    status: A/M/D/T (git), 'U' untracked, 'I' ignored.
    """
    changes = []
    for status, path in gitutil.diff_names(base, None, cwd=root):
        changes.append((status[0], path, "tracked"))
    for path in gitutil.ls_files(root, ["--others", "--exclude-standard"]):
        changes.append(("U", path, "untracked"))
    for path in gitutil.ls_files(root, ["--others", "--ignored", "--exclude-standard"]):
        changes.append(("I", path, "ignored"))
    return changes


def collect_changes_ref(repo, base, ref):
    return [(status[0], path, "tracked") for status, path in gitutil.diff_names(base, ref, cwd=repo)]


def check(args):
    scope = Scope()
    report = {
        "tool": "check_scope.py",
        "tools_version": TOOLS_VERSION,
        "argv": list(args.argv),
        "python": sys.version.split()[0],
        "git_version": gitutil.version(),
    }
    baseline = load_baseline(args.baseline_dir)
    base_commit = baseline["baseline.json"]["base_commit"]
    doc_tree = baseline["baseline.json"]["doc_tree"]
    perl_entries = baseline["perl_protected.json"]["entries"]
    perl_by_path = {e["path"]: e for e in perl_entries}
    report["base_commit"] = base_commit
    report["doc_tree"] = doc_tree

    mf = manifest_mod.load(args.manifest)
    report["manifest"] = {"path": args.manifest, "batch_id": mf.data["batch_id"],
                          "base_commit": mf.data["base_commit"], "pgnls_commit": mf.data["pgnls_commit"]}
    if mf.data["base_commit"] != base_commit:
        scope.add("S1_BASE_MISMATCH", args.manifest,
                  "manifest base_commit %s != baseline %s" % (mf.data["base_commit"], base_commit))
    bj_pgnls = baseline["baseline.json"].get("pgnls_commit")
    if bj_pgnls and mf.data["pgnls_commit"] != bj_pgnls:
        scope.add("S1_BASE_MISMATCH", args.manifest,
                  "manifest pgnls_commit %s != baseline %s" % (mf.data["pgnls_commit"], bj_pgnls))

    if args.worktree:
        repo = os.path.abspath(args.worktree)
        report["mode"] = "worktree"
        report["target"] = repo
    else:
        repo = os.path.abspath(args.repo or os.getcwd())
        report["mode"] = "ref"
        report["target"] = gitutil.rev_parse(args.ref, cwd=repo)

    # The diff base: the frozen base commit (cumulative check), or -- with
    # --incremental -- the manifest's parent_commit, which must descend from
    # the frozen base.  doc/ and Perl checks below always use the frozen
    # baseline regardless of mode.
    if args.incremental:
        report["incremental"] = True
        parent = mf.parent_commit
        if not parent:
            scope.add("S1_BASE_MISMATCH", args.manifest, "--incremental requires manifest parent_commit")
            return scope, report
        diff_base = args.base or parent
        resolved = gitutil.rev_parse(diff_base, cwd=repo)
        report["base_resolved"] = resolved
        if resolved != parent:
            scope.add("S1_BASE_MISMATCH", diff_base, "resolves to %s, manifest parent_commit is %s" % (resolved, parent))
            return scope, report
        if subprocess.run(["git", "merge-base", "--is-ancestor", base_commit, parent], cwd=repo,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode != 0:
            scope.add("S1_BASE_MISMATCH", parent, "parent_commit does not descend from frozen base %s" % base_commit)
            return scope, report
    else:
        report["incremental"] = False
        diff_base = args.base or base_commit
        resolved = gitutil.rev_parse(diff_base, cwd=repo)
        report["base_resolved"] = resolved
        if resolved != base_commit:
            scope.add("S1_BASE_MISMATCH", diff_base, "resolves to %s, baseline is %s" % (resolved, base_commit))
            return scope, report  # nothing below is meaningful

    # ---- S2: changed set vs manifest -----------------------------------
    if args.worktree:
        changes = collect_changes_worktree(repo, resolved)
    else:
        changes = collect_changes_ref(repo, resolved, args.ref)
    report["changes"] = [{"status": s, "path": p, "origin": o} for s, p, o in changes]
    report["n_changes"] = len(changes)
    for status, path, origin in changes:
        pat = mf.allows(path)
        if pat is None:
            scope.add("S2_OUTSIDE_MANIFEST", path, "%s (%s) matches no allowed_paths pattern" % (status, origin))
            continue
        if mf.require_file_entries and origin == "tracked":
            fe = mf.file_entry(path)
            if fe is None:
                scope.add("S2_NO_FILE_ENTRY", path, "no files[] entry in manifest")
            else:
                expected = "add" if status == "A" else "modify"
                if status == "D":
                    scope.add("S2_NO_FILE_ENTRY", path, "deleted file cannot be in scope")
                elif fe["action"] != expected:
                    scope.add("S2_NO_FILE_ENTRY", path, "manifest action %r but git status %s" % (fe["action"], status))
        if mf.require_file_entries and origin != "tracked":
            # untracked / ignored files inside allowed paths are not commits;
            # they must be listed too so nothing silently rides along.
            if mf.file_entry(path) is None:
                scope.add("S2_NO_FILE_ENTRY", path, "%s file inside allowed paths has no files[] entry" % origin)

    # ---- S3: doc/ ------------------------------------------------------
    if args.worktree:
        head_doc = gitutil.run(["rev-parse", "HEAD:doc"], cwd=repo).strip()
        report["head_doc_tree"] = head_doc
        if head_doc != doc_tree:
            scope.add("S3_DOC_TREE", "doc", "HEAD:doc %s != baseline %s" % (head_doc, doc_tree))
        snap = protection.snapshot_tree(repo, "doc")
        diffs = protection.compare_snapshots(baseline["doc_snapshot.json"]["entries"], snap)
        report["doc_snapshot_entries"] = len(snap)
        report["doc_snapshot_diffs"] = diffs
        for d in diffs:
            scope.add("S3_DOC_SNAPSHOT", "doc", d)
        doc_diff = gitutil.diff_names(base_commit, None, cwd=repo, paths=["doc"])
        for status, path in doc_diff:
            scope.add("S3_DOC_DIFF", path, "git diff vs base: %s" % status)
    else:
        ref_doc = gitutil.run(["rev-parse", "%s:doc" % args.ref], cwd=repo).strip()
        report["ref_doc_tree"] = ref_doc
        if ref_doc != doc_tree:
            scope.add("S3_DOC_TREE", "doc", "%s:doc %s != baseline %s" % (args.ref, ref_doc, doc_tree))
        for status, path in gitutil.diff_names(base_commit, args.ref, cwd=repo, paths=["doc"]):
            scope.add("S3_DOC_DIFF", path, "git diff vs base: %s" % status)

    # ---- S4: Perl protected set ---------------------------------------
    report["perl_protected_files"] = len(perl_entries)
    if args.worktree:
        for e in perl_entries:
            full = os.path.join(repo, e["path"])
            try:
                lst = os.lstat(full)
            except FileNotFoundError:
                scope.add("S4_PERL_MISSING", e["path"], "missing from worktree")
                continue
            if not stat.S_ISREG(lst.st_mode):
                scope.add("S4_PERL_CHANGED", e["path"], "not a regular file (symlink/dir/special)")
                continue
            try:
                # non-following open + identity/metadata check (no TOCTOU)
                data, st = protection.read_regular_file(full, expect_st=lst)
            except protection.UnstableFile as ex:
                scope.add("S4_PERL_CHANGED", e["path"], str(ex))
                continue
            mode = protection.git_mode_of_stat(st)
            oid = gitutil.blob_oid(data)
            if oid != e["oid"]:
                scope.add("S4_PERL_CHANGED", e["path"], "blob %s != baseline %s" % (oid, e["oid"]))
            if mode != e["mode"]:
                scope.add("S4_PERL_CHANGED", e["path"], "mode %s != baseline %s" % (mode, e["mode"]))
        for status, path, origin in changes:
            if status == "D" or path in perl_by_path:
                continue
            full = os.path.join(repo, path)
            if os.path.isfile(full) and protection.is_perl_file_on_disk(full):
                scope.add("S4_PERL_NEW", path, "new Perl file (%s)" % origin)
    else:
        tree = gitutil.ls_tree(args.ref, cwd=repo)
        for e in perl_entries:
            t = tree.get(e["path"])
            if t is None:
                scope.add("S4_PERL_MISSING", e["path"], "missing from %s" % args.ref)
                continue
            mode, typ, oid = t
            if oid != e["oid"]:
                scope.add("S4_PERL_CHANGED", e["path"], "blob %s != baseline %s" % (oid, e["oid"]))
            if mode != e["mode"]:
                scope.add("S4_PERL_CHANGED", e["path"], "mode %s != baseline %s" % (mode, e["mode"]))
        new_paths = [p for s, p, _ in changes if s != "D" and p not in perl_by_path]
        for path in new_paths:
            if protection.is_perl_name(path):
                scope.add("S4_PERL_NEW", path, "new Perl file (extension)")
                continue
            blob = gitutil.show_blob(args.ref, path, cwd=repo)
            if blob is not None and protection.is_perl_shebang(blob[:256]):
                scope.add("S4_PERL_NEW", path, "new Perl file (shebang)")
    return scope, report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    tgt = ap.add_mutually_exclusive_group(required=True)
    tgt.add_argument("--worktree", metavar="ROOT")
    tgt.add_argument("--ref", metavar="REF")
    ap.add_argument("--repo", default=None, help="repository for --ref (default: cwd)")
    ap.add_argument("--base", default=None,
                    help="diff base ref (default: baseline base_commit, or manifest parent_commit with --incremental)")
    ap.add_argument("--incremental", action="store_true",
                    help="per-batch mode: diff against the manifest's parent_commit instead of the frozen base; "
                         "doc/ and Perl checks still use the frozen baseline")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--baseline-dir", default=DEFAULT_BASELINE)
    ap.add_argument("--json", dest="json_out", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    args.argv = sys.argv[1:] if argv is None else list(argv)

    t0 = time.time()
    try:
        scope, report = check(args)
    except (gitutil.GitError, manifest_mod.ManifestError, protection.UnstableFile, OSError, ValueError) as e:
        print("check_scope: UNVERIFIED: %s" % e, file=sys.stderr)
        if args.json_out:
            with open(args.json_out, "w", encoding="utf-8") as f:
                json.dump({"tool": "check_scope.py", "exit_code": 2, "error": str(e)}, f, indent=1)
        return 2

    exit_code = 1 if scope.violations else 0
    report["violations"] = scope.violations
    report["n_violations"] = len(scope.violations)
    report["elapsed_sec"] = round(time.time() - t0, 3)
    report["exit_code"] = exit_code
    if not args.quiet:
        for v in scope.violations:
            print("VIOLATION %s %s: %s" % (v["rule"], v["path"], v["message"]))
    print("check_scope: %s; %d changed paths, %d violations; doc_tree=%s perl_protected=%d; %.2fs" % (
        "PASS" if exit_code == 0 else "FAIL", report.get("n_changes", 0), len(scope.violations),
        report["doc_tree"][:12], report.get("perl_protected_files", 0), report["elapsed_sec"]))
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
