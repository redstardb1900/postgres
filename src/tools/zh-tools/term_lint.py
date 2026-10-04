#!/usr/bin/env python3
"""
term_lint.py -- conservative terminology check for translated text.

Inputs (one of):
  --c-pair OLD.c NEW.c           lint every changed comment of one C file pair
  --worktree BASEREF ROOT        lint changed comments of all modified
                                 .c/.h/.cpp files in ROOT vs BASEREF
  --po FILE.po                   lint msgid/msgstr pairs
  --text-pair OLD NEW            lint one whole-file text pair (README etc.)

Rule sources:
  --pgnls DIR                    pgnls checkout (ref/phrasebook.tsv,
                                 ref/glossary.tsv); its HEAD must equal the
                                 frozen pgnls_commit in baseline/baseline.json
                                 unless --no-check-pgnls-sha is given
  --rules FILE                   term_rules.json (default: next to this script)
  --allowlist FILE               JSON list of {"rule","path","pattern","reason"}
                                 entries; every entry needs a reason

Only explicit hits are reported (English pattern matched the source AND the
forbidden Chinese pattern matched the translation, or a preserved term
vanished).  Nothing is rewritten.

Severity: every rule is "error" or "warning".  Matching is per segment, so
a forbidden rendering can legitimately co-occur with a correct one inside a
multi-sentence comment; only renderings that are wrong in every context are
errors (term_rules.json sets this per rule).  Phrasebook rules were written
for single NLS messages: they are errors for --po and warnings for comment /
text input (override with --phrasebook-severity).

Exit codes: 0 no error hits (warnings may be present), 1 error hits or
unpairable files, 2 unverified (bad inputs / pgnls mismatch).
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, c_lexer, gitutil, termlint  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
C_EXTS = (".c", ".h", ".cpp")


def load_rules(pgnls, rules_path, check_sha, baseline_dir, phrasebook_severity):
    info = {"pgnls": None, "rules": rules_path, "n_rules": 0, "phrasebook_severity": phrasebook_severity}
    rules = []
    preserved = []
    if rules_path:
        with open(rules_path, "r", encoding="utf-8") as f:
            tr, pt = termlint.load_term_rules(json.load(f))
        rules += tr
        preserved += pt
    if pgnls:
        head = gitutil.rev_parse("HEAD", cwd=pgnls)
        info["pgnls"] = {"path": os.path.abspath(pgnls), "commit": head}
        if check_sha:
            with open(os.path.join(baseline_dir, "baseline.json"), "r", encoding="utf-8") as f:
                frozen = json.load(f).get("pgnls_commit")
            if frozen != head:
                raise ValueError("pgnls HEAD %s != frozen pgnls_commit %s" % (head, frozen))
            dirty = gitutil.run(["status", "--porcelain", "--", "ref"], cwd=pgnls).strip()
            if dirty:
                raise ValueError("pgnls ref/ has local modifications")
        with open(os.path.join(pgnls, "ref", "phrasebook.tsv"), "r", encoding="utf-8") as f:
            rules += termlint.load_phrasebook(f.read(), severity=phrasebook_severity)
        with open(os.path.join(pgnls, "ref", "glossary.tsv"), "r", encoding="utf-8") as f:
            preserved += termlint.load_glossary_preserved(f.read())
    rules += termlint.preserved_term_rules(preserved)
    info["n_rules"] = len(rules)
    info["n_error_rules"] = sum(1 for r in rules if r.severity == termlint.ERROR)
    info["n_warning_rules"] = sum(1 for r in rules if r.severity == termlint.WARNING)
    info["preserved_terms"] = sorted(set(preserved))
    return rules, info


def lint_c_pair(rules, old, new, path):
    lo = c_lexer.lex(old)
    ln = c_lexer.lex(new)
    hits = []
    if lo.skeleton != ln.skeleton:
        # cannot pair comments; verify_c_comments.py will reject this anyway
        return hits, False
    for oc, nc in zip(lo.comments, ln.comments):
        if oc.text == nc.text:
            continue
        hits += termlint.lint_pair(rules, oc.text.decode("utf-8", "replace"),
                                   nc.text.decode("utf-8", "replace"), path=path, line=oc.line)
    return hits, True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--c-pair", nargs=2, metavar=("OLD", "NEW"))
    src.add_argument("--worktree", nargs=2, metavar=("BASEREF", "ROOT"))
    src.add_argument("--po", metavar="FILE")
    src.add_argument("--text-pair", nargs=2, metavar=("OLD", "NEW"))
    ap.add_argument("--paths", nargs="+", default=[])
    ap.add_argument("--pgnls", default=None)
    ap.add_argument("--rules", default=os.path.join(HERE, "term_rules.json"))
    ap.add_argument("--no-rules", action="store_true", help="do not load term_rules.json")
    ap.add_argument("--allowlist", default=None)
    ap.add_argument("--baseline-dir", default=os.path.join(HERE, "baseline"))
    ap.add_argument("--no-check-pgnls-sha", action="store_true")
    ap.add_argument("--phrasebook-severity", choices=("error", "warning"), default=None,
                    help="default: error for --po, warning otherwise")
    ap.add_argument("--json", dest="json_out", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    pb_sev = args.phrasebook_severity or (termlint.ERROR if args.po else termlint.WARNING)
    try:
        rules, info = load_rules(args.pgnls, None if args.no_rules else args.rules,
                                 not args.no_check_pgnls_sha, args.baseline_dir, pb_sev)
        allow = termlint.Allowlist([])
        if args.allowlist:
            with open(args.allowlist, "r", encoding="utf-8") as f:
                allow = termlint.Allowlist(json.load(f))
    except (OSError, ValueError, gitutil.GitError) as e:
        print("term_lint: UNVERIFIED: %s" % e, file=sys.stderr)
        return 2
    if not rules:
        print("term_lint: UNVERIFIED: no rules loaded", file=sys.stderr)
        return 2

    hits = []
    unpaired = []
    n_pairs = 0
    try:
        if args.c_pair:
            with open(args.c_pair[0], "rb") as f:
                old = f.read()
            with open(args.c_pair[1], "rb") as f:
                new = f.read()
            h, ok = lint_c_pair(rules, old, new, args.c_pair[1])
            hits += h
            n_pairs += 1
            if not ok:
                unpaired.append(args.c_pair[1])
        elif args.worktree:
            base, root = args.worktree
            root = os.path.abspath(root)
            for status, p in gitutil.diff_names(base, None, cwd=root, paths=args.paths):
                if not p.endswith(C_EXTS) or status[0] != "M":
                    continue
                old = gitutil.show_blob(base, p, cwd=root)
                with open(os.path.join(root, p), "rb") as f:
                    new = f.read()
                h, ok = lint_c_pair(rules, old, new, p)
                hits += h
                n_pairs += 1
                if not ok:
                    unpaired.append(p)
        elif args.po:
            with open(args.po, "r", encoding="utf-8") as f:
                entries = termlint.parse_po(f.read())
            for msgid, msgstr, line in entries:
                if not msgstr:
                    continue
                hits += termlint.lint_pair(rules, msgid, msgstr, path=args.po, line=line)
                n_pairs += 1
        else:
            with open(args.text_pair[0], "r", encoding="utf-8") as f:
                old = f.read()
            with open(args.text_pair[1], "r", encoding="utf-8") as f:
                new = f.read()
            hits += termlint.lint_pair(rules, old, new, path=args.text_pair[1], line=1)
            n_pairs += 1
    except (OSError, UnicodeDecodeError, gitutil.GitError) as e:
        print("term_lint: UNVERIFIED: %s" % e, file=sys.stderr)
        return 2

    kept = [h for h in hits if not allow.allows(h)]
    suppressed = len(hits) - len(kept)
    errors = [h for h in kept if h.severity == termlint.ERROR]
    warnings = [h for h in kept if h.severity == termlint.WARNING]
    exit_code = 1 if errors or unpaired else 0
    if not args.quiet:
        for h in kept:
            print("%s:%s: %s %s: en=%s | zh=%s  (%s)" % (h.path, h.line if h.line is not None else "-",
                                                      h.severity.upper(), h.rule, _snip(h.en), _snip(h.zh),
                                                      h.note))
        for p in unpaired:
            print("%s: comments could not be paired (non-comment bytes differ)" % p)
    print("term_lint: %d rules (%d error/%d warning), %d pairs, %d error hits, %d warnings "
          "(%d allowlisted), %d unpaired files" % (
              info["n_rules"], info["n_error_rules"], info["n_warning_rules"], n_pairs, len(errors),
              len(warnings), suppressed, len(unpaired)))
    if args.json_out:
        rep = {"tool": "term_lint.py", "tools_version": TOOLS_VERSION, "rules": info,
               "pairs": n_pairs, "hits": [h.to_dict() for h in kept], "n_errors": len(errors),
               "n_warnings": len(warnings), "suppressed": suppressed,
               "unpaired": unpaired, "exit_code": exit_code}
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(rep, f, ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
    return exit_code


def _snip(s, n=80):
    s = s.replace("\n", "\\n")
    return s[:n] + ("..." if len(s) > n else "")


if __name__ == "__main__":
    sys.exit(main())
