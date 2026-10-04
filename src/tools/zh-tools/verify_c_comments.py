#!/usr/bin/env python3
"""
verify_c_comments.py -- prove that only comments changed in .c/.h/.cpp files.

Modes (exactly one):
  --git OLDREF NEWREF            compare two git refs (uses git diff --name-only)
  --worktree BASEREF ROOT        compare BASEREF blobs with the working tree
  --dir OLDROOT NEWROOT          compare two directory trees
  --self-check ROOT              lex every tracked .c/.h/.cpp under ROOT and
                                 compare each file with itself (parser
                                 stability + round-trip check)
  OLD.c NEW.c                    compare a single pair of files

Options:
  --paths P [P ...]   restrict to these path prefixes (relative to the tree)
  --json FILE         write a machine-readable report
  -j N                parallel workers (default: os.cpu_count())
  --no-strict-lines   demote the per-comment LF-count rule to a warning
                      (debugging only; forbidden for commits and acceptance)
  --no-warnings       suppress advisory warnings in the output
  --cc-check          compiler-preprocessor token cross-check: replay the
                      frozen GNU Make compile commands (-E -P -MMD) of every
                      variant covering a changed file against the translated
                      tree and compare token / header-set / argv hashes with
                      the frozen baseline (see cc_evidence/README.md).
                      "verified" only when every changed file is covered by a
                      passing variant; a token difference is a violation
                      (exit 1); anything the check cannot establish is
                      "unverified" (exit 2).
  --cc-evidence DIR   frozen evidence directory (default: cc_evidence/ next
                      to this script)
  --cc-root DIR       translated tree to preprocess (default: the --worktree
                      ROOT; for --git the repo is used only when its HEAD is
                      NEWREF)
  --cc-jobs N         parallel preprocessor invocations

Exit codes: 0 all files pass, 1 at least one violation, 2 parse error,
unverified cc-check, or usage/IO error.
"""
import argparse
import json
import multiprocessing
import os
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, c_verify, cc_check, gitutil  # noqa: E402

C_EXTS = (".c", ".h", ".cpp")


def is_c_path(p):
    return p.endswith(C_EXTS)


def under_paths(p, paths):
    if not paths:
        return True
    for pre in paths:
        pre = pre.rstrip("/")
        if p == pre or p.startswith(pre + "/"):
            return True
    return False


# ---------------------------------------------------------------------------
# work items:  (path, old_spec, new_spec)   where spec is one of
#   ("git", cwd, ref)   ("file", abspath)   None (missing)
# ---------------------------------------------------------------------------

def _load(spec, path):
    if spec is None:
        return None
    if spec[0] == "git":
        return gitutil.show_blob(spec[2], path, cwd=spec[1])
    if spec[0] == "file":
        try:
            with open(spec[1], "rb") as f:
                return f.read()
        except OSError:
            return None
    raise ValueError(spec)


def _worker(item):
    path, old_spec, new_spec, strict_lines, warn = item
    old = _load(old_spec, path)
    new = _load(new_spec, path)
    if old is None or new is None:
        r = c_verify.FileResult(path)
        if old is None and new is None:
            r.violations.append(c_verify.Finding("R9_FILE", "file unreadable on both sides"))
        elif old is None:
            r.violations.append(c_verify.Finding("R9_FILE", "file added (no baseline version)"))
        else:
            r.violations.append(c_verify.Finding("R9_FILE", "file deleted or unreadable in new tree"))
        return r.to_dict()
    r = c_verify.compare_files(old, new, path=path, strict_lines=strict_lines, warn=warn)
    return r.to_dict(max_findings=50)


def build_items_git(oldref, newref, paths, cwd):
    items = []
    for status, p in gitutil.diff_names(oldref, newref, cwd=cwd, paths=paths):
        if not is_c_path(p):
            continue
        old_spec = ("git", cwd, oldref) if status != "A" else None
        new_spec = ("git", cwd, newref) if status != "D" else None
        items.append((p, old_spec, new_spec))
    return items


def build_items_worktree(baseref, root, paths):
    root = os.path.abspath(root)
    items = []
    seen = set()
    for status, p in gitutil.diff_names(baseref, None, cwd=root, paths=paths):
        if not is_c_path(p):
            continue
        seen.add(p)
        old_spec = ("git", root, baseref) if status != "A" else None
        new_spec = ("file", os.path.join(root, p)) if status != "D" else None
        if status == "D" and not os.path.exists(os.path.join(root, p)):
            new_spec = None
        items.append((p, old_spec, new_spec))
    # untracked (not ignored) new C files are also a violation
    for p in gitutil.ls_files(root, ["--others", "--exclude-standard"] + (["--"] + list(paths) if paths else [])):
        if is_c_path(p) and p not in seen:
            items.append((p, None, ("file", os.path.join(root, p))))
    return items


def _walk(root):
    out = []
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d != ".git"]
        for fn in fns:
            if is_c_path(fn):
                full = os.path.join(dp, fn)
                out.append(os.path.relpath(full, root))
    return out


def build_items_dir(oldroot, newroot, paths):
    oldroot = os.path.abspath(oldroot)
    newroot = os.path.abspath(newroot)
    olds = set(_walk(oldroot))
    news = set(_walk(newroot))
    items = []
    for p in sorted(olds | news):
        if not under_paths(p, paths):
            continue
        o = ("file", os.path.join(oldroot, p)) if p in olds else None
        n = ("file", os.path.join(newroot, p)) if p in news else None
        if p in olds and p in news:
            with open(o[1], "rb") as f1, open(n[1], "rb") as f2:
                if f1.read() == f2.read():
                    continue
        items.append((p, o, n))
    return items


def build_items_selfcheck(root, paths):
    root = os.path.abspath(root)
    try:
        files = gitutil.ls_files(root, ["--"] + list(paths) if paths else [])
    except gitutil.GitError:
        files = _walk(root)
    items = []
    for p in files:
        if is_c_path(p) and under_paths(p, paths):
            spec = ("file", os.path.join(root, p))
            items.append((p, spec, spec))
    return items


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--git", nargs=2, metavar=("OLDREF", "NEWREF"))
    mode.add_argument("--worktree", nargs=2, metavar=("BASEREF", "ROOT"))
    mode.add_argument("--dir", nargs=2, metavar=("OLDROOT", "NEWROOT"))
    mode.add_argument("--self-check", metavar="ROOT")
    ap.add_argument("files", nargs="*", help="OLD NEW (single-file mode)")
    ap.add_argument("--paths", nargs="+", default=[])
    ap.add_argument("--repo", default=None, help="repository to run git in for --git (default: cwd)")
    ap.add_argument("--json", dest="json_out", default=None)
    ap.add_argument("-j", type=int, default=None)
    ap.add_argument("--no-strict-lines", action="store_true")
    ap.add_argument("--no-warnings", action="store_true")
    ap.add_argument("--cc-check", action="store_true")
    ap.add_argument("--cc-evidence", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "cc_evidence"))
    ap.add_argument("--cc-root", default=None)
    ap.add_argument("--cc-jobs", type=int, default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    t0 = time.time()
    strict = not args.no_strict_lines
    warn = not args.no_warnings
    report = {
        "tool": "verify_c_comments.py",
        "tools_version": TOOLS_VERSION,
        "argv": sys.argv[1:] if argv is None else list(argv),
        "python": sys.version.split()[0],
        "strict_lines": strict,
    }
    try:
        report["git_version"] = gitutil.version()
    except Exception:
        report["git_version"] = None

    try:
        if args.git:
            cwd = args.repo or os.getcwd()
            report["mode"] = "git"
            report["old"] = gitutil.rev_parse(args.git[0], cwd=cwd)
            report["new"] = gitutil.rev_parse(args.git[1], cwd=cwd)
            items = build_items_git(args.git[0], args.git[1], args.paths, cwd)
        elif args.worktree:
            report["mode"] = "worktree"
            report["old"] = gitutil.rev_parse(args.worktree[0], cwd=args.worktree[1])
            report["new"] = os.path.abspath(args.worktree[1])
            items = build_items_worktree(args.worktree[0], args.worktree[1], args.paths)
        elif args.dir:
            report["mode"] = "dir"
            items = build_items_dir(args.dir[0], args.dir[1], args.paths)
        elif args.self_check:
            report["mode"] = "self-check"
            report["root"] = os.path.abspath(args.self_check)
            items = build_items_selfcheck(args.self_check, args.paths)
            warn = False  # W7 "untranslated" is meaningless when old == new
        elif len(args.files) == 2:
            report["mode"] = "files"
            items = [(args.files[1], ("file", os.path.abspath(args.files[0])),
                      ("file", os.path.abspath(args.files[1])))]
        else:
            ap.error("choose a mode: --git, --worktree, --dir, --self-check or OLD NEW")
            return 2
    except gitutil.GitError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    work = [(p, o, n, strict, warn) for (p, o, n) in items]
    jobs = args.j or os.cpu_count() or 1
    if jobs > 1 and len(work) > 1:
        with multiprocessing.Pool(jobs) as pool:
            results = pool.map(_worker, work, chunksize=8)
    else:
        results = [_worker(w) for w in work]

    results.sort(key=lambda r: r["path"])
    n_viol = sum(1 for r in results if r["status"] == "violation")
    n_parse = sum(1 for r in results if r["status"] == "parse_error")
    n_ok = sum(1 for r in results if r["status"] == "ok")
    agg = {}
    for r in results:
        for k, v in r.get("stats", {}).items():
            if isinstance(v, int):
                agg[k] = agg.get(k, 0) + v
    rule_counts = {}
    for r in results:
        for f in r["violations"] + r["warnings"]:
            rule_counts[f["rule"]] = rule_counts.get(f["rule"], 0) + 1

    exit_code = 0
    if n_viol:
        exit_code = 1
    if n_parse:
        exit_code = 2

    cc = None
    if args.cc_check:
        cc_root = args.cc_root
        if cc_root is None and args.worktree:
            cc_root = os.path.abspath(args.worktree[1])
        elif cc_root is None and args.git:
            repo = args.repo or os.getcwd()
            try:
                if gitutil.rev_parse("HEAD", cwd=repo) == report["new"]:
                    cc_root = os.path.abspath(gitutil.toplevel(repo))
            except gitutil.GitError:
                cc_root = None
        cc = cc_check.run(args.cc_evidence, [r["path"] for r in results], cc_root, jobs=args.cc_jobs)
        report["cc_check"] = cc
        if cc["status"] == "violation":
            exit_code = max(exit_code, 1)
        elif cc["status"] != "verified":
            exit_code = 2

    report.update({
        "files_checked": len(results),
        "ok": n_ok,
        "violations": n_viol,
        "parse_errors": n_parse,
        "rule_counts": dict(sorted(rule_counts.items())),
        "aggregate": agg,
        "elapsed_sec": round(time.time() - t0, 3),
        "exit_code": exit_code,
        "files": results,
    })

    if not args.quiet:
        for r in results:
            if r["status"] == "ok" and not r["warnings"]:
                continue
            for f in r["violations"]:
                print("%s:%s: VIOLATION %s: %s" % (r["path"], f.get("line", "-"), f["rule"], f["message"]))
                if f.get("context"):
                    for ln in f["context"].splitlines():
                        print("    " + ln)
            if warn:
                for f in r["warnings"]:
                    print("%s:%s: warning %s: %s" % (r["path"], f.get("line", "-"), f["rule"], f["message"]))
    print("verify_c_comments: %d files, %d ok, %d violations, %d parse errors; comments=%d "
          "changed=%d splices=%d trigraphs(old/new)=%d/%d; %.2fs" % (
              len(results), n_ok, n_viol, n_parse, agg.get("comments", 0), agg.get("changed_comments", 0),
              agg.get("splices", 0), agg.get("trigraphs_old", 0), agg.get("trigraphs_new", 0),
              report["elapsed_sec"]))
    if cc is not None:
        print("cc-check: %s -- %s" % (cc["status"].upper(), cc["reason"]))
        if not args.quiet:
            for p in cc.get("uncovered", []):
                print("  cc-check UNCOVERED %s" % p)
            for d in cc.get("variants_different", []):
                print("  cc-check DIFFERENT %s (%s)" % (d["variant_key"], d["kind"]))
            for f in cc.get("variants_failed", []):
                print("  cc-check FAILED %s: %s" % (f["variant_key"], (f["error"] or "").strip().splitlines()[-1:]))

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
