#!/usr/bin/env python3
"""
gen_baseline.py -- freeze the protection baseline from the read-only base
worktree (REL_16_11_base).

Writes into --out (default: baseline/ next to this script):
  baseline.json        base commit, tree OIDs, pgnls commit, counts
  doc_snapshot.json    recursive on-disk snapshot of doc/ (tracked +
                       untracked + ignored; path/type/mode/perm/size/sha256)
  perl_protected.json  every *.pl/*.pm/*.t blob and every blob whose
                       shebang invokes perl, with mode and blob OID

With --verify the files are regenerated in memory and compared with the
ones on disk (exit 1 if they differ) -- this is how reviewers can prove
the committed baseline really describes the base commit.
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, gitutil, protection  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "baseline")


def build(base_worktree, pgnls_dir, expect_commit=None):
    root = os.path.abspath(base_worktree)
    head = gitutil.rev_parse("HEAD", cwd=root)
    if expect_commit and head != expect_commit:
        raise SystemExit("base worktree HEAD %s != expected %s" % (head, expect_commit))
    if gitutil.run(["status", "--porcelain", "--ignored"], cwd=root).strip():
        raise SystemExit("base worktree is not clean (git status --porcelain --ignored)")
    doc_tree = gitutil.run(["rev-parse", "HEAD:doc"], cwd=root).strip()
    root_tree = gitutil.run(["rev-parse", "HEAD^{tree}"], cwd=root).strip()
    pgnls_commit = gitutil.rev_parse("HEAD", cwd=pgnls_dir) if pgnls_dir else None

    doc_snapshot = protection.snapshot_tree(root, "doc")
    perl = protection.perl_protected_set("HEAD", root)
    tracked_doc = gitutil.ls_files(root, ["--", "doc"])

    baseline = {
        "schema_version": protection.SNAPSHOT_SCHEMA_VERSION,
        "tools_version": TOOLS_VERSION,
        "base_ref_name": "REL_16_11",
        "base_commit": head,
        "base_tree": root_tree,
        "doc_tree": doc_tree,
        "pgnls_commit": pgnls_commit,
        "pgnls_path": os.path.abspath(pgnls_dir) if pgnls_dir else None,
        "doc_tracked_files": len(tracked_doc),
        "doc_snapshot_entries": len(doc_snapshot),
        "perl_protected_files": len(perl),
        "perl_by_extension": sum(1 for e in perl if e["reason"] == "extension"),
        "perl_by_shebang": sum(1 for e in perl if e["reason"] == "shebang"),
        "notes": [
            "doc_snapshot.json covers everything on disk below doc/ (tracked, untracked, ignored).",
            "perl_protected.json is frozen from the base commit's blobs; compare by blob OID and mode.",
            "Timestamps, inodes and uid/gid are intentionally not recorded.",
        ],
    }
    return {
        "baseline.json": baseline,
        "doc_snapshot.json": {"schema_version": protection.SNAPSHOT_SCHEMA_VERSION, "root": "doc",
                              "base_commit": head, "doc_tree": doc_tree, "entries": doc_snapshot},
        "perl_protected.json": {"schema_version": protection.SNAPSHOT_SCHEMA_VERSION, "base_commit": head,
                                "extensions": list(protection.PERL_EXTS), "entries": perl},
    }


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, indent=1, sort_keys=True) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-worktree", required=True)
    ap.add_argument("--pgnls", default=None)
    ap.add_argument("--expect-commit", default=None)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--verify", action="store_true", help="compare with existing files instead of writing")
    args = ap.parse_args(argv)

    files = build(args.base_worktree, args.pgnls, args.expect_commit)
    if args.verify:
        rc = 0
        for name, obj in files.items():
            p = os.path.join(args.out, name)
            try:
                with open(p, "r", encoding="utf-8") as f:
                    existing = json.load(f)
            except (OSError, ValueError) as e:
                print("%s: cannot read: %s" % (p, e))
                rc = 1
                continue
            if name == "baseline.json":
                # pgnls_path and tools_version are informational
                for k in ("pgnls_path", "tools_version"):
                    existing.pop(k, None)
                    obj = dict(obj)
                    obj.pop(k, None)
            if existing != obj:
                print("%s: DIFFERS from regenerated data" % p)
                if name == "doc_snapshot.json":
                    for d in protection.compare_snapshots(existing.get("entries", []), obj["entries"])[:20]:
                        print("   " + d)
                rc = 1
            else:
                print("%s: matches" % p)
        return rc

    os.makedirs(args.out, exist_ok=True)
    for name, obj in files.items():
        p = os.path.join(args.out, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(dump(obj))
        print("wrote %s" % p)
    b = files["baseline.json"]
    print("base_commit=%s doc_tree=%s doc_entries=%d perl_protected=%d (ext=%d shebang=%d)" % (
        b["base_commit"], b["doc_tree"], b["doc_snapshot_entries"], b["perl_protected_files"],
        b["perl_by_extension"], b["perl_by_shebang"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
