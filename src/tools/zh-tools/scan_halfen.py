#!/usr/bin/env python3
"""
scan_halfen.py -- report half-English debt in C comments (halfen3).

Scanning modes (exactly one):
  --git REF              scan comments of every tracked .c/.h/.cpp at REF
  --git-diff OLD NEW     restrict to files changed between OLD and NEW
  --worktree BASE ROOT   scan files modified in ROOT vs BASE
                         (ROOT may be the working tree)

Options:
  --paths P [P ...]      restrict to these path prefixes
  --pureen               also report PUREEN comments (untranslated English
                         prose; use only on files that should be fully
                         translated -- otherwise this duplicates the
                         W7_UNTRANSLATED advisory of verify_c_comments.py)
  --exemptions FILE      curated site ledger (default: halfen_exemptions.json
                         next to this script; entries {"path": glob,
                         "pattern": body regex, "reason": str})
  --no-exemptions        skip the site ledger (raw findings)
  --json FILE            machine-readable report

Exit codes: 0 no findings, 1 findings, 2 usage / git / IO error.
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, gitutil, halfen  # noqa: E402

C_EXTS = (".c", ".h", ".cpp")


def is_c_path(p):
    return p.endswith(C_EXTS)


def under_paths(p, paths):
    if not paths:
        return True
    return any(p == x or p.startswith(x.rstrip("/") + "/") for x in paths)


def main(argv=None):
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--git", metavar="REF")
    g.add_argument("--git-diff", nargs=2, metavar=("OLD", "NEW"))
    g.add_argument("--worktree", nargs=2, metavar=("BASE", "ROOT"))
    ap.add_argument("--paths", nargs="+", metavar="P")
    ap.add_argument("--pureen", action="store_true")
    ap.add_argument("--exemptions", metavar="FILE",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "halfen_exemptions.json"))
    ap.add_argument("--no-exemptions", action="store_true")
    ap.add_argument("--json", metavar="FILE")
    args = ap.parse_args(argv)

    try:
        if args.no_exemptions:
            site = []
        elif os.path.exists(args.exemptions):
            site = halfen.load_site_exemptions(args.exemptions)
        else:
            site = []

        blobs = []
        if args.git is not None:
            tree = gitutil.ls_tree(args.git)
            for path in sorted(tree):
                if is_c_path(path) and under_paths(path, args.paths):
                    data = gitutil.show_blob(args.git, path)
                    if data is not None:
                        blobs.append((path, data))
        elif args.git_diff is not None:
            old, new = args.git_diff
            for _status, path in sorted(gitutil.diff_names(old, new),
                                        key=lambda sp: sp[1]):
                if is_c_path(path) and under_paths(path, args.paths):
                    data = gitutil.show_blob(new, path)
                    if data is not None:
                        blobs.append((path, data))
        else:
            base, root = args.worktree
            for _status, path in sorted(gitutil.diff_names(base),
                                        key=lambda sp: sp[1]):
                if not (is_c_path(path) and under_paths(path, args.paths)):
                    continue
                full = os.path.join(root, path)
                if os.path.exists(full):
                    with open(full, "rb") as fh:
                        blobs.append((path, fh.read()))
                else:
                    data = gitutil.show_blob("HEAD", path)
                    if data is not None:
                        blobs.append((path, data))
    except (OSError, gitutil.GitError, ValueError) as e:
        print("scan_halfen: %s" % e, file=sys.stderr)
        return 2

    findings = []
    lex_errors = 0
    for path, data in blobs:
        for f in halfen.scan_bytes(data, path, include_pureen=args.pureen,
                                   site_exemptions=site):
            if f.excerpt.startswith("<lex error"):
                lex_errors += 1
            findings.append(f)

    if args.git is not None:
        mode = "git %s" % args.git
    elif args.git_diff is not None:
        mode = "git-diff %s..%s" % (args.git_diff[0], args.git_diff[1])
    else:
        mode = "worktree %s vs %s" % (args.worktree[1], args.worktree[0])
    report = {
        "tools_version": TOOLS_VERSION,
        "mode": mode,
        "files": len(blobs),
        "findings": [f.to_dict() for f in findings],
        "n_findings": len(findings),
        "lex_errors": lex_errors,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
    for f in findings:
        tag = "MIXED" if f.cls == halfen.MIXED else "PUREEN"
        print("%s:%d: %s run=%d: %s" % (f.path, f.line, tag, f.run, f.excerpt))
    print("scan_halfen: %d files, %d findings (%d lex errors)%s"
          % (len(blobs), len(findings), lex_errors,
             "; pureen on" if args.pureen else ""))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
