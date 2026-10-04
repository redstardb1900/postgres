#!/usr/bin/env python3
"""
po_sync.py -- the zh_CN.po gate for REL_16_11_zh (plan step 3).

Exact POTs come from a throw-away in-tree worktree of REL_16_11 in which
`make -C <nls dir> init-po` was run per directory (never a top-level
distprep/init-po, never inside the zh worktree).  This tool

  probe      proves that the installed msgattrib prints zero bytes for
             --only-fuzzy / --untranslated when nothing matches (and
             something when one entry matches); every later check relies on it
  archive    copies the 26 POTs out of the repo, records raw SHA-256,
             canonical hash (dynamic header dropped), active-key hash, entry
             counts, the init-po commands/logs and gettext/bison/flex/perl
             versions, and checks the POT source references the plan
             requires (gram/scan, boot, repl, syncrep, jsonpath, guc-file;
             pl_gram; preproc/pgc; sql_help)      -> evidence/po/pot_manifest.json
  merge      per component:
               msgmerge --quiet --no-wrap --previous --sort-by-file
                        --lang=zh_CN [-C supplement] <pgnls.po> <pot> -o tmp.po
               msgattrib --no-obsolete --no-wrap -o <zh_CN.po> tmp.po
             A first pass without the supplement records what msgmerge left
             fuzzy/untranslated; the supplement (hand translations kept in
             po_supplement/<catalog>.po, every one of them flagged for
             100% review) is then used as an exact-match compendium
  check      msgfmt --check --check-format --statistics, msgcmp against the
             POT, 0 fuzzy / 0 untranslated (msgattrib zero-byte proof plus an
             independent parse), no duplicate active key, complete plurals,
             header Language/charset/Plural-Forms, every supplement entry used
                                                   -> evidence/po/po_check.json
  report     pgnls vs POT vs result: added / obsolete / fuzzy-resolved /
             filled-by-supplement lists per component -> evidence/po/po_sync_report.json

All gettext tools run with LC_ALL=C.  Exit codes: 0 pass, 1 violations,
2 unverified (missing tool, probe failure, missing input).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, potools  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
EVIDENCE_DIR = os.path.join(HERE, "evidence", "po")
SUPPLEMENT_DIR = os.path.join(HERE, "po_supplement")
LANG = "zh_CN"
EXPECTED_COMPONENTS = 26
GETTEXT_TOOLS = ("xgettext", "msgmerge", "msgattrib", "msgfmt", "msgcmp")

# the plan's hard requirement: these generated sources must have been fed to
# xgettext.  A pattern maps to the basenames that may appear in "#:"
# references (bison/flex #line directives make xgettext report the grammar
# file, not the generated .c).  Sources that contain no translatable string
# at all (REL_16_11: bootparse.y/bootscanner.l, syncrep_gram.y/
# syncrep_scanner.l -- they only use errmsg_internal) cannot show up in any
# POT; for those the proof is that the generated .c exists in the POT
# worktree and is listed in the xgettext input (gettext-files), which
# archive() checks under "generated_inputs".
REQUIRED_REFERENCES = {
    "postgres": {
        "gram": ["gram.y"], "scan": ["scan.l"],
        "repl": ["repl_gram.y", "repl_scanner.l"],
        "jsonpath": ["jsonpath_gram.y", "jsonpath_scan.l"],
        "guc-file": ["guc-file.l"],
    },
    "plpgsql": {"pl_gram": ["pl_gram.y"]},
    "ecpg": {"preproc": ["preproc.y"], "pgc": ["pgc.l"]},
    "psql": {"sql_help": ["sql_help.c", "sql_help.h"]},
}
REQUIRED_GENERATED_INPUTS = {
    "postgres": {
        "gram": ["parser/gram.c"], "scan": ["parser/scan.c"],
        "boot": ["bootstrap/bootparse.c", "bootstrap/bootscanner.c"],
        "repl": ["replication/repl_gram.c", "replication/repl_scanner.c"],
        "syncrep": ["replication/syncrep_gram.c", "replication/syncrep_scanner.c"],
        "jsonpath": ["utils/adt/jsonpath_gram.c", "utils/adt/jsonpath_scan.c"],
        "guc-file": ["utils/misc/guc-file.c"],
    },
    "plpgsql": {"pl_gram": ["pl_gram.c"]},
    "ecpg": {"preproc": ["preproc.c"], "pgc": ["pgc.c"]},
    "psql": {"sql_help": ["sql_help.c", "sql_help.h"]},
}
EXPECTED_DIRS = {"ecpg": "src/interfaces/ecpg/preproc", "pgscripts": "src/bin/scripts"}
# every hand-translated supplement entry carries this translator comment;
# msgmerge copies compendium translator comments into the merged catalog, so
# the mark is visible in the committed zh_CN.po on exactly the supplement
# entries (check_one verifies that) and in the report's review_pending list
REVIEW_MARK = "# zh-review-pending"


class Unverified(RuntimeError):
    pass


def env_c():
    return {"PATH": "/usr/local/bin:/usr/bin:/bin", "LC_ALL": "C", "LANG": "C"}


def run(argv, cwd=None, stdin=None):
    p = subprocess.run(argv, cwd=cwd, env=env_c(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=stdin)
    return p.returncode, p.stdout, p.stderr


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")


def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def tool_versions():
    out = {}
    for t in GETTEXT_TOOLS + ("bison", "flex", "perl", "make"):
        if shutil.which(t, path=env_c()["PATH"]) is None:
            raise Unverified("required tool %r not found" % t)
        argv = [t, "--version"] if t != "perl" else [t, "-e", "print $^V"]
        rc, so, se = run(argv)
        out[t] = (so or se).decode("utf-8", "replace").strip().splitlines()[0] if (so or se) else ""
    return out


def components(repo):
    """The NLS directories of the tree: {catalog: nls_dir_relative}. Exactly 26, fixed mapping checks."""
    rc, so, se = run(["git", "ls-files", "-z", "--", "*/nls.mk"], cwd=repo)
    if rc != 0:
        raise Unverified("git ls-files failed: %s" % se.decode("utf-8", "replace"))
    out = {}
    for p in sorted(x for x in so.decode("utf-8").split("\0") if x):
        d = os.path.dirname(p)
        with open(os.path.join(repo, p), "r", encoding="utf-8") as f:
            text = f.read()
        m = re.search(r"^CATALOG_NAME\s*=\s*(\S+)", text, re.M)
        if not m:
            raise Unverified("%s has no CATALOG_NAME" % p)
        name = m.group(1)
        if name in out:
            raise Unverified("catalog %s defined twice (%s, %s)" % (name, out[name], d))
        out[name] = d
    if len(out) != EXPECTED_COMPONENTS:
        raise Unverified("expected %d nls.mk directories, found %d" % (EXPECTED_COMPONENTS, len(out)))
    for name, d in EXPECTED_DIRS.items():
        if out.get(name) != d:
            raise Unverified("catalog %s maps to %r, expected %r" % (name, out.get(name), d))
    return out


def supplement_path(name):
    return os.path.join(SUPPLEMENT_DIR, name + ".po")


# ---------------------------------------------------------------------------
# probe
# ---------------------------------------------------------------------------

_PROBE_HEADER = ('msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
                 '"Language: zh_CN\\n"\n"Plural-Forms: nplurals=1; plural=0;\\n"\n\n')
_PROBE_CLEAN = _PROBE_HEADER + 'msgid "a"\nmsgstr "甲"\n\nmsgid "b"\nmsgstr "乙"\n'
_PROBE_FUZZY = _PROBE_HEADER + 'msgid "a"\nmsgstr "甲"\n\n#, fuzzy\nmsgid "b"\nmsgstr "乙"\n'
_PROBE_UNTRANSLATED = _PROBE_HEADER + 'msgid "a"\nmsgstr "甲"\n\nmsgid "b"\nmsgstr ""\n'


def do_probe(report_path=None):
    """Prove the msgattrib zero-byte contract on which `check` relies."""
    versions = tool_versions()
    d = tempfile.mkdtemp(prefix="zh-po-probe.")
    results = []
    ok = True
    try:
        cases = [
            ("clean", _PROBE_CLEAN, "--only-fuzzy", 0),
            ("clean", _PROBE_CLEAN, "--untranslated", 0),
            ("one_fuzzy", _PROBE_FUZZY, "--only-fuzzy", 1),
            ("one_fuzzy", _PROBE_FUZZY, "--untranslated", 0),
            ("one_untranslated", _PROBE_UNTRANSLATED, "--untranslated", 1),
            ("one_untranslated", _PROBE_UNTRANSLATED, "--only-fuzzy", 0),
        ]
        for label, text, flag, want in cases:
            p = os.path.join(d, label + ".po")
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
            rc, so, se = run(["msgattrib", flag, "--no-wrap", p])
            n_entries = sum(1 for line in so.decode("utf-8").split("\n") if line.startswith("msgid ") and
                            line != 'msgid ""')
            # with no match msgattrib must print nothing at all (not even the header)
            passed = rc == 0 and ((len(so) == 0) if want == 0 else (n_entries == want))
            ok = ok and passed
            results.append({"case": label, "flag": flag, "rc": rc, "stdout_bytes": len(so),
                            "entries_in_stdout": n_entries, "expected_entries": want, "pass": passed,
                            "stderr": se.decode("utf-8", "replace")[-300:]})
    finally:
        shutil.rmtree(d, ignore_errors=True)
    report = {"tool": "po_sync.py probe", "tools_version": TOOLS_VERSION, "generated_utc": utc_now(),
              "versions": versions, "cases": results, "pass": ok}
    if report_path:
        write_json(report_path, report)
    return ok, report


def require_probe():
    ok, rep = do_probe()
    if not ok:
        raise Unverified("msgattrib zero-byte probe failed: %s" % json.dumps(
            [c for c in rep["cases"] if not c["pass"]], ensure_ascii=False))
    return rep


# ---------------------------------------------------------------------------
# archive
# ---------------------------------------------------------------------------

def _pot_reference_basenames(entries):
    names = set()
    for e in entries:
        for r in e.references:
            f = r.rsplit(":", 1)[0]
            names.add(os.path.basename(f))
    return names


def do_archive(pot_worktree, out_dir, repo, base_commit):
    """Copy POTs out of the throw-away worktree and record everything about them."""
    probe = require_probe()
    comps = components(repo)
    pot_worktree = os.path.abspath(pot_worktree)
    out_dir = os.path.abspath(out_dir)
    if os.path.commonpath([out_dir, repo]) == repo:
        raise Unverified("archive directory %s must be outside the repo" % out_dir)
    rc, so, se = run(["git", "rev-parse", "HEAD"], cwd=pot_worktree)
    if rc != 0:
        raise Unverified("POT worktree is not a git checkout: %s" % pot_worktree)
    head = so.decode().strip()
    if head != base_commit:
        raise Unverified("POT worktree HEAD %s != base commit %s" % (head, base_commit))
    # the POT worktree's components must be the same 26 directories
    if components(pot_worktree) != comps:
        raise Unverified("POT worktree nls.mk set differs from the zh tree")
    config_status = os.path.join(pot_worktree, "config.status")
    if not os.path.exists(config_status):
        raise Unverified("POT worktree was not configured in-tree (no config.status)")
    with open(config_status, "r", encoding="utf-8", errors="replace") as f:
        cs = f.read()
    m = re.search(r'^ac_cs_config="(.*)"$', cs, re.M)
    configure_args = m.group(1) if m else None
    if configure_args is None:
        raise Unverified("config.status has no ac_cs_config line")
    m = re.search(r'^S\["enable_nls"\]="(.*)"$', cs, re.M)
    enable_nls = m.group(1) if m else None
    if enable_nls != "yes":
        raise Unverified("POT worktree configured without --enable-nls (enable_nls=%r)" % enable_nls)
    for want in ("'--enable-nls'", "'--enable-depend'"):
        if want not in configure_args:
            raise Unverified("POT worktree configure args lack %s: %s" % (want, configure_args))
    os.makedirs(os.path.join(out_dir, "pot"), exist_ok=True)
    pots = {}
    problems = []
    for name, d in sorted(comps.items()):
        src = os.path.join(pot_worktree, d, "po", name + ".pot")
        if not os.path.exists(src):
            problems.append("%s: POT missing at %s" % (name, src))
            continue
        dst = os.path.join(out_dir, "pot", name + ".pot")
        shutil.copy2(src, dst)
        if sha256_file(src) != sha256_file(dst):
            raise Unverified("copy of %s differs" % src)
        try:
            entries = potools.read(dst)
        except potools.PoError as e:
            problems.append("%s: unparsable POT: %s" % (name, e))
            continue
        summary = potools.summarize(entries)
        dups = potools.duplicate_keys(entries)
        if dups:
            problems.append("%s: %d duplicate keys in POT" % (name, len(dups)))
        rec = {
            "catalog": name,
            "nls_dir": d,
            "po_dir": d + "/po",
            "source": src,
            "archived": dst,
            "size": os.path.getsize(dst),
            "sha256": sha256_file(dst),
            "canonical_sha256": potools.canonical_pot_hash(dst),
            "active_key_sha256": potools.active_key_hash(entries),
            "entries": summary["active"],
            "plural_entries": summary["plural_entries"],
            "with_msgctxt": summary["with_msgctxt"],
            "header": potools.header_fields(entries),
            "init_po_command": "make -j1 -w -C %s init-po" % d,
        }
        # reference / generated-input checks
        if name in REQUIRED_REFERENCES:
            names = _pot_reference_basenames(entries)
            rec["required_references"] = {}
            for label, cands in REQUIRED_REFERENCES[name].items():
                hit = sorted(c for c in cands if c in names)
                rec["required_references"][label] = {"candidates": cands, "found": hit, "pass": bool(hit)}
                if not hit:
                    problems.append("%s: POT has no reference to %s (%s)" % (name, label, "/".join(cands)))
        if name in REQUIRED_GENERATED_INPUTS:
            rec["generated_inputs"] = {}
            gettext_files = None
            gf = os.path.join(pot_worktree, d, "gettext-files")
            if os.path.exists(gf):
                with open(gf, "r", encoding="utf-8") as f:
                    gettext_files = set(os.path.normpath(x.strip()) for x in f if x.strip())
            for label, files in REQUIRED_GENERATED_INPUTS[name].items():
                rows = []
                for rel in files:
                    p = os.path.join(pot_worktree, d, rel)
                    exists = os.path.exists(p)
                    listed = (os.path.normpath(rel) in gettext_files) if gettext_files is not None else None
                    rows.append({"file": rel, "exists": exists, "listed_in_gettext_files": listed,
                                 "sha256": sha256_file(p) if exists else None})
                    if not exists:
                        problems.append("%s: generated input %s/%s missing" % (name, d, rel))
                    if listed is False:
                        problems.append("%s: %s not in gettext-files" % (name, rel))
                rec["generated_inputs"][label] = rows
        pots[name] = rec
    # init-po logs, if the caller kept them next to the archive
    logs_dir = os.path.join(out_dir, "logs")
    logs = {}
    if os.path.isdir(logs_dir):
        for fn in sorted(os.listdir(logs_dir)):
            p = os.path.join(logs_dir, fn)
            if os.path.isfile(p):
                logs[fn] = {"sha256": sha256_file(p), "size": os.path.getsize(p)}
    manifest = {
        "tool": "po_sync.py archive",
        "tools_version": TOOLS_VERSION,
        "generated_utc": utc_now(),
        "base_commit": base_commit,
        "pot_worktree": pot_worktree,
        "pot_worktree_head": head,
        "configure_args": configure_args,
        "archive_dir": out_dir,
        "versions": probe["versions"],
        "probe": {"pass": probe["pass"], "cases": len(probe["cases"])},
        "components": pots,
        "n_components": len(pots),
        "logs": logs,
        "problems": problems,
        "pass": not problems and len(pots) == EXPECTED_COMPONENTS,
    }
    write_json(os.path.join(EVIDENCE_DIR, "pot_manifest.json"), manifest)
    write_json(os.path.join(out_dir, "pot_manifest.json"), manifest)
    return manifest


def load_pot_manifest():
    p = os.path.join(EVIDENCE_DIR, "pot_manifest.json")
    if not os.path.exists(p):
        raise Unverified("evidence/po/pot_manifest.json missing; run archive first")
    return read_json(p)


def verify_pots(manifest):
    """Every archived POT must still hash as recorded."""
    for name, rec in manifest["components"].items():
        if not os.path.exists(rec["archived"]):
            raise Unverified("archived POT missing: %s" % rec["archived"])
        if sha256_file(rec["archived"]) != rec["sha256"]:
            raise Unverified("archived POT changed on disk: %s" % rec["archived"])
    return manifest


# ---------------------------------------------------------------------------
# merge
# ---------------------------------------------------------------------------

def _msgmerge(def_po, pot, out, compendium=None):
    argv = ["msgmerge", "--quiet", "--no-wrap", "--previous", "--sort-by-file", "--lang=" + LANG]
    if compendium:
        argv += ["-C", compendium]
    argv += [def_po, pot, "-o", out]
    rc, so, se = run(argv)
    return argv, rc, se.decode("utf-8", "replace")


def _msgattrib_active(src, out):
    argv = ["msgattrib", "--no-obsolete", "--no-wrap", "-o", out, src]
    rc, so, se = run(argv)
    return argv, rc, se.decode("utf-8", "replace")


def _keys_json(entries):
    return [potools.key_to_json(e.key) for e in entries]


def _apply_overrides(target, supp, def_po):
    """Splice supplement translations over entries that msgmerge revived from
    pgnls *obsolete* entries.  Returns (applied list, problems).

    A supplement key that pgnls still translates actively is a conflict
    (pgnls wins; the supplement entry must be dropped) and is reported as a
    problem rather than applied.
    """
    sup_entries = {e.key: e for e in potools.active_entries(potools.read(supp))}
    def_entries = potools.read(def_po)
    def_active = {e.key for e in potools.active_entries(def_entries)}
    def_obsolete = {e.key: e for e in def_entries if e.obsolete and not e.is_header}
    with open(target, "r", encoding="utf-8") as f:
        text = f.read()
    blocks = potools.split_blocks(text)
    applied = []
    problems = []
    for i, block in enumerate(blocks):
        try:
            parsed = potools.parse(block, target)
        except potools.PoError:
            continue
        if len(parsed) != 1 or parsed[0].is_header:
            continue
        cur = parsed[0]
        s = sup_entries.get(cur.key)
        if s is None or cur.translations() == s.translations():
            continue
        if cur.key in def_active:
            problems.append("supplement entry %r conflicts with an active pgnls translation" % (cur.msgid[:60],))
            continue
        if cur.key not in def_obsolete:
            problems.append("supplement entry %r differs from the merged result for an unknown reason" %
                            (cur.msgid[:60],))
            continue
        new = potools.Entry(cur.lineno)
        new.msgctxt, new.msgid, new.msgid_plural = cur.msgctxt, cur.msgid, cur.msgid_plural
        new.references = list(cur.references)
        new.flags = [f for f in cur.flags if f != "fuzzy"]
        new.comments = [c for c in s.comments if not c.startswith("#.")] + [c for c in cur.comments]
        new.msgstr = s.msgstr
        new.msgstr_plural = dict(s.msgstr_plural)
        blocks[i] = potools.serialize_entry(new)
        applied.append({"key": potools.key_to_json(cur.key), "pgnls_obsolete_msgstr": def_obsolete[cur.key].translations(),
                        "msgstr": s.translations()})
    if applied:
        with open(target, "w", encoding="utf-8") as f:
            f.write("\n\n".join(blocks) + "\n")
    return applied, problems


def do_merge(pgnls, repo, only=None):
    manifest = verify_pots(load_pot_manifest())
    require_probe()
    pgnls = os.path.abspath(pgnls)
    rc, so, se = run(["git", "rev-parse", "HEAD"], cwd=pgnls)
    if rc != 0:
        raise Unverified("pgnls is not a git checkout: %s" % pgnls)
    pgnls_head = so.decode().strip()
    base = read_json(os.path.join(HERE, "baseline", "baseline.json"))
    if pgnls_head != base["pgnls_commit"]:
        raise Unverified("pgnls HEAD %s != frozen %s" % (pgnls_head, base["pgnls_commit"]))
    po_src_dir = os.path.join(pgnls, LANG, "REL_16_STABLE")
    tmp = tempfile.mkdtemp(prefix="zh-po-merge.")
    results = {}
    problems = []
    try:
        for name, rec in sorted(manifest["components"].items()):
            if only and name not in only:
                continue
            def_po = os.path.join(po_src_dir, name + ".po")
            if not os.path.exists(def_po):
                problems.append("%s: pgnls PO missing: %s" % (name, def_po))
                continue
            pot = rec["archived"]
            target = os.path.join(repo, rec["po_dir"], LANG + ".po")
            supp = supplement_path(name)
            has_supp = os.path.exists(supp)
            r = {"catalog": name, "pgnls_po": def_po, "pgnls_po_sha256": sha256_file(def_po), "pot": pot,
                 "pot_sha256": rec["sha256"], "target": os.path.relpath(target, repo),
                 "supplement": os.path.relpath(supp, repo) if has_supp else None,
                 "supplement_sha256": sha256_file(supp) if has_supp else None, "commands": []}
            # pass 1: plain merge, to record what gettext alone would leave open
            tmp1 = os.path.join(tmp, name + ".pass1.po")
            argv, rc1, err1 = _msgmerge(def_po, pot, tmp1)
            r["commands"].append({"argv": argv, "rc": rc1, "stderr": err1[-500:]})
            if rc1 != 0:
                problems.append("%s: msgmerge (pass 1) failed: %s" % (name, err1.strip()[-200:]))
                results[name] = r
                continue
            p1 = potools.read(tmp1)
            p1_active = potools.active_entries(p1)
            r["pass1"] = potools.summarize(p1)
            r["pass1_fuzzy_keys"] = _keys_json([e for e in p1_active if e.fuzzy])
            r["pass1_untranslated_keys"] = _keys_json([e for e in p1_active if not e.fuzzy and
                                                        any(t == "" for t in e.translations())])
            # pass 2: with the hand-translation compendium when there is one
            tmp2 = os.path.join(tmp, name + ".pass2.po")
            argv, rc2, err2 = _msgmerge(def_po, pot, tmp2, compendium=supp if has_supp else None)
            r["commands"].append({"argv": argv, "rc": rc2, "stderr": err2[-500:]})
            if rc2 != 0:
                problems.append("%s: msgmerge (pass 2) failed: %s" % (name, err2.strip()[-200:]))
                results[name] = r
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            argv, rc3, err3 = _msgattrib_active(tmp2, target)
            r["commands"].append({"argv": argv, "rc": rc3, "stderr": err3[-500:]})
            if rc3 != 0:
                problems.append("%s: msgattrib failed: %s" % (name, err3.strip()[-200:]))
                results[name] = r
                continue
            # pass 3 (only when needed): a supplement entry whose key pgnls
            # carries solely as an obsolete ("#~") entry.  msgmerge revives
            # obsolete def.po translations ahead of any compendium, so the
            # retired wording would win; the supplement translation is
            # spliced in and the file re-serialized by msgattrib.
            if has_supp:
                over, perr = _apply_overrides(target, supp, def_po)
                r["overrides"] = over
                problems.extend("%s: %s" % (name, m) for m in perr)
                if over:
                    argv, rc4, err4 = _msgattrib_active(target, target)
                    r["commands"].append({"argv": argv, "rc": rc4, "stderr": err4[-500:]})
                    if rc4 != 0:
                        problems.append("%s: msgattrib after override failed: %s" % (name, err4.strip()[-200:]))
                        results[name] = r
                        continue
            r["target_sha256"] = sha256_file(target)
            r["result"] = potools.summarize(potools.read(target))
            results[name] = r
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    report = {"tool": "po_sync.py merge", "tools_version": TOOLS_VERSION, "generated_utc": utc_now(),
              "pgnls": pgnls, "pgnls_commit": pgnls_head, "pgnls_branch_dir": po_src_dir,
              "versions": manifest["versions"], "components": results, "problems": problems,
              "pass": not problems}
    write_json(os.path.join(EVIDENCE_DIR, "po_merge.json"), report)
    return report


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def _zero_byte(flag, path):
    rc, so, se = run(["msgattrib", flag, "--no-wrap", path])
    return rc == 0 and len(so) == 0, len(so), se.decode("utf-8", "replace")


def check_one(name, target, pot, supplement=None):
    """All gates for one zh_CN.po; returns (ok, record)."""
    rec = {"catalog": name, "target": target, "pot": pot, "problems": []}
    pr = rec["problems"]
    if not os.path.exists(target):
        pr.append("target missing")
        return False, rec
    rec["target_sha256"] = sha256_file(target)
    # 1. msgfmt
    argv = ["msgfmt", "--check", "--check-format", "--statistics", "-o", "/dev/null", target]
    rc, so, se = run(argv)
    rec["msgfmt"] = {"argv": argv, "rc": rc, "stderr": se.decode("utf-8", "replace").strip()}
    if rc != 0:
        pr.append("msgfmt --check failed")
    m = re.search(r"(\d+) translated", rec["msgfmt"]["stderr"])
    rec["msgfmt_translated"] = int(m.group(1)) if m else None
    rec["msgfmt_fuzzy"] = int(re.search(r"(\d+) fuzzy", rec["msgfmt"]["stderr"]).group(1)) \
        if re.search(r"(\d+) fuzzy", rec["msgfmt"]["stderr"]) else 0
    rec["msgfmt_untranslated"] = int(re.search(r"(\d+) untranslated", rec["msgfmt"]["stderr"]).group(1)) \
        if re.search(r"(\d+) untranslated", rec["msgfmt"]["stderr"]) else 0
    # 2. msgcmp against the POT (every POT msgid present and translated)
    argv = ["msgcmp", target, pot]
    rc, so, se = run(argv)
    rec["msgcmp"] = {"argv": argv, "rc": rc, "stderr": se.decode("utf-8", "replace").strip()[-800:]}
    if rc != 0:
        pr.append("msgcmp against POT failed")
    # 3. zero-byte proofs
    for flag in ("--only-fuzzy", "--untranslated"):
        ok, n, err = _zero_byte(flag, target)
        rec["msgattrib_" + flag.strip("-").replace("-", "_")] = {"stdout_bytes": n, "pass": ok}
        if not ok:
            pr.append("msgattrib %s printed %d bytes" % (flag, n))
    # 4. independent parse: duplicates, plurals, header, obsolete, fuzzy/untranslated counts
    try:
        entries = potools.read(target)
    except potools.PoError as e:
        pr.append("unparsable: %s" % e)
        return False, rec
    s = potools.summarize(entries)
    rec["summary"] = s
    if s["fuzzy"]:
        pr.append("%d fuzzy entries (parser)" % s["fuzzy"])
    if s["untranslated"]:
        pr.append("%d untranslated entries (parser)" % s["untranslated"])
    if s["obsolete"]:
        pr.append("%d obsolete entries left" % s["obsolete"])
    dups = potools.duplicate_keys(entries)
    rec["duplicate_keys"] = [potools.key_to_json(k) for k in dups]
    if dups:
        pr.append("%d duplicate active keys" % len(dups))
    hdr = potools.header_fields(entries)
    rec["header"] = {k: hdr.get(k) for k in ("Project-Id-Version", "POT-Creation-Date", "PO-Revision-Date",
                                             "Language", "Content-Type", "Plural-Forms", "Last-Translator")}
    if hdr.get("Language") != LANG:
        pr.append("header Language is %r" % hdr.get("Language"))
    if "charset=UTF-8" not in hdr.get("Content-Type", ""):
        pr.append("header Content-Type is %r" % hdr.get("Content-Type"))
    npl = potools.plural_forms_nplurals(hdr)
    if npl is None:
        pr.append("header Plural-Forms missing or without nplurals")
    else:
        bad = potools.check_plurals(entries, npl)
        rec["plural_problems"] = bad
        if bad:
            pr.append("%d incomplete plural entries" % len(bad))
    # 5. key set identical to the POT (msgcmp allows extra entries in the PO; we do not)
    pot_entries = potools.read(pot)
    pot_keys = set(potools.active_keys(pot_entries))
    po_keys = set(potools.active_keys(entries))
    rec["keys"] = {"pot": len(pot_keys), "po": len(po_keys), "missing_in_po": len(pot_keys - po_keys),
                   "extra_in_po": len(po_keys - pot_keys)}
    if pot_keys != po_keys:
        pr.append("active key set differs from POT (missing %d, extra %d)" % (
            len(pot_keys - po_keys), len(po_keys - pot_keys)))
    rec["active_key_sha256"] = potools.active_key_hash(entries)
    rec["pot_active_key_sha256"] = potools.active_key_hash(pot_entries)
    # 6. supplement: every entry used verbatim, none unused, every one flagged for review
    if supplement and os.path.exists(supplement):
        sup = potools.read(supplement)
        by_key = {e.key: e for e in potools.active_entries(entries)}
        used, unused, mismatched, unflagged = [], [], [], []
        for e in potools.active_entries(sup):
            if not any(c.startswith(REVIEW_MARK) for c in e.comments):
                unflagged.append(potools.key_to_json(e.key))
            t = by_key.get(e.key)
            if t is None:
                unused.append(potools.key_to_json(e.key))
            elif t.translations() != e.translations():
                mismatched.append(potools.key_to_json(e.key))
            else:
                used.append(potools.key_to_json(e.key))
        # the review mark must appear in the result on exactly the supplement keys
        marked_in_target = sorted((potools.key_to_json(e.key) for e in potools.active_entries(entries)
                                   if any(c.startswith(REVIEW_MARK) for c in e.comments)), key=str)
        want_marked = sorted(used + mismatched, key=str)
        rec["supplement"] = {"path": supplement, "sha256": sha256_file(supplement), "entries": len(used) +
                             len(unused) + len(mismatched), "used": used, "unused": unused,
                             "mismatched": mismatched, "unflagged": unflagged,
                             "review_marks_in_target": len(marked_in_target)}
        if marked_in_target != want_marked:
            pr.append("review marks in target (%d) do not match the supplement keys (%d)" % (
                len(marked_in_target), len(want_marked)))
        if unused:
            pr.append("%d supplement entries not in the POT (stale)" % len(unused))
        if mismatched:
            pr.append("%d supplement entries overridden by pgnls (drop them from the supplement)" % len(mismatched))
        if unflagged:
            pr.append("%d supplement entries lack the '%s' translator comment" % (len(unflagged), REVIEW_MARK))
    return not pr, rec


def _term_lint(po_path, pgnls):
    """Run term_lint.py --po; returns (rc, n_errors, n_warnings)."""
    d = tempfile.mkdtemp(prefix="zh-po-term.")
    try:
        out = os.path.join(d, "term.json")
        env = dict(os.environ)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        p = subprocess.run([sys.executable, os.path.join(HERE, "term_lint.py"), "--po", po_path, "--pgnls", pgnls,
                            "--json", out], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if os.path.exists(out):
            r = read_json(out)
            return p.returncode, r.get("n_errors"), r.get("n_warnings")
        return p.returncode, None, None
    finally:
        shutil.rmtree(d, ignore_errors=True)


def do_check(repo, only=None, pgnls=None):
    manifest = verify_pots(load_pot_manifest())
    require_probe()
    results = {}
    all_ok = True
    for name, rec in sorted(manifest["components"].items()):
        if only and name not in only:
            continue
        target = os.path.join(repo, rec["po_dir"], LANG + ".po")
        supp = supplement_path(name)
        ok, r = check_one(name, target, rec["archived"], supp)
        r["target"] = os.path.relpath(target, repo)
        if pgnls:
            # the hand translations must be term-clean; the pgnls-sourced
            # bulk is the terminology authority itself and is only reported
            if os.path.exists(supp):
                rc, ne, nw = _term_lint(supp, pgnls)
                r["term_lint_supplement"] = {"rc": rc, "errors": ne, "warnings": nw}
                if rc != 0:
                    ok = False
                    r["problems"].append("term_lint on the supplement exited %d (%s errors)" % (rc, ne))
            rc, ne, nw = _term_lint(target, pgnls)
            r["term_lint_target_info"] = {"rc": rc, "errors": ne, "warnings": nw,
                                          "note": "informational: pgnls-sourced translations, not a gate here"}
        results[name] = r
        all_ok = all_ok and ok
    totals = {k: sum(r.get("summary", {}).get(k, 0) for r in results.values())
              for k in ("active", "translated", "fuzzy", "untranslated", "plural_entries")}
    report = {"tool": "po_sync.py check", "tools_version": TOOLS_VERSION, "generated_utc": utc_now(),
              "versions": manifest["versions"], "pot_manifest_generated_utc": manifest["generated_utc"],
              "components": results, "totals": totals, "n_components": len(results), "pass": all_ok}
    write_json(os.path.join(EVIDENCE_DIR, "po_check.json"), report)
    return report


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

def do_report(pgnls, repo, base_ref):
    manifest = verify_pots(load_pot_manifest())
    merge = read_json(os.path.join(EVIDENCE_DIR, "po_merge.json")) if os.path.exists(
        os.path.join(EVIDENCE_DIR, "po_merge.json")) else None
    pgnls = os.path.abspath(pgnls)
    po_src_dir = os.path.join(pgnls, LANG, "REL_16_STABLE")
    comps = {}
    tot = {"pot_entries": 0, "pgnls_entries": 0, "added_vs_pgnls": 0, "obsolete_vs_pgnls": 0,
           "pass1_fuzzy": 0, "pass1_untranslated": 0, "supplement_entries": 0, "base_entries": 0,
           "changed_vs_base": 0}
    for name, rec in sorted(manifest["components"].items()):
        pot = potools.read(rec["archived"])
        pot_keys = set(potools.active_keys(pot))
        src = os.path.join(po_src_dir, name + ".po")
        pg = potools.read(src)
        pg_keys = set(potools.active_keys(pg))
        target = os.path.join(repo, rec["po_dir"], LANG + ".po")
        tgt = potools.read(target)
        tgt_by_key = {e.key: e for e in potools.active_entries(tgt)}
        pg_by_key = {e.key: e for e in potools.active_entries(pg)}
        # translations that differ from pgnls for the same key (should only be supplement-filled)
        changed_vs_pgnls = [potools.key_to_json(k) for k in sorted(pot_keys & pg_keys, key=str)
                            if tgt_by_key[k].translations() != pg_by_key[k].translations()]
        supp = supplement_path(name)
        supp_keys = []
        if os.path.exists(supp):
            supp_keys = [potools.key_to_json(e.key) for e in potools.active_entries(potools.read(supp))]
        # against the REL_16_11 baseline zh_CN.po
        rc, so, se = run(["git", "show", "%s:%s/%s.po" % (base_ref, rec["po_dir"], LANG)], cwd=repo)
        base_summary = None
        changed_vs_base = None
        if rc == 0:
            base_entries = potools.parse(so.decode("utf-8"), "base")
            base_summary = potools.summarize(base_entries)
            base_by_key = {e.key: e for e in potools.active_entries(base_entries)}
            changed_vs_base = sum(1 for k, e in tgt_by_key.items()
                                  if k not in base_by_key or base_by_key[k].translations() != e.translations()
                                  or base_by_key[k].fuzzy)
        m = (merge or {}).get("components", {}).get(name, {})
        c = {
            "catalog": name,
            "target": os.path.relpath(target, repo),
            "pot_entries": len(pot_keys),
            "pgnls_entries": len(pg_keys),
            "added_vs_pgnls": [potools.key_to_json(k) for k in sorted(pot_keys - pg_keys, key=str)],
            "obsolete_vs_pgnls": [potools.key_to_json(k) for k in sorted(pg_keys - pot_keys, key=str)],
            "pass1_fuzzy": m.get("pass1_fuzzy_keys"),
            "pass1_untranslated": m.get("pass1_untranslated_keys"),
            "supplement_entries": supp_keys,
            "changed_translation_vs_pgnls": changed_vs_pgnls,
            "result": potools.summarize(tgt),
            "base_rel_16_11": base_summary,
            "changed_vs_base_rel_16_11": changed_vs_base,
        }
        comps[name] = c
        tot["pot_entries"] += c["pot_entries"]
        tot["pgnls_entries"] += c["pgnls_entries"]
        tot["added_vs_pgnls"] += len(c["added_vs_pgnls"])
        tot["obsolete_vs_pgnls"] += len(c["obsolete_vs_pgnls"])
        tot["pass1_fuzzy"] += len(c["pass1_fuzzy"] or [])
        tot["pass1_untranslated"] += len(c["pass1_untranslated"] or [])
        tot["supplement_entries"] += len(supp_keys)
        tot["base_entries"] += (base_summary or {}).get("active", 0)
        tot["changed_vs_base"] += changed_vs_base or 0
    review = []
    for name, c in comps.items():
        if not c["supplement_entries"]:
            continue
        sup = potools.read(supplement_path(name))
        for e in potools.active_entries(sup):
            review.append({"catalog": name, "msgctxt": e.msgctxt, "msgid": e.msgid, "msgid_plural": e.msgid_plural,
                           "msgstr": e.translations(), "flags": e.flags, "comments": e.comments,
                           "review_status": "pending GPT 100% review"})
    report = {"tool": "po_sync.py report", "tools_version": TOOLS_VERSION, "generated_utc": utc_now(),
              "pgnls": pgnls, "pgnls_commit": read_json(os.path.join(HERE, "baseline", "baseline.json"))["pgnls_commit"],
              "base_ref": base_ref, "components": comps, "totals": tot, "review_pending": review}
    write_json(os.path.join(EVIDENCE_DIR, "po_sync_report.json"), report)
    return report


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=REPO, help="zh worktree root (default: this tree)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe")
    p.add_argument("--json", default=os.path.join(EVIDENCE_DIR, "msgattrib_probe.json"))
    p = sub.add_parser("archive")
    p.add_argument("--pot-worktree", required=True)
    p.add_argument("--out", required=True, help="out-of-repo archive directory")
    p.add_argument("--base-commit", default=None, help="default: baseline/baseline.json base_commit")
    p = sub.add_parser("merge")
    p.add_argument("--pgnls", required=True)
    p.add_argument("--only", nargs="*", default=None)
    p = sub.add_parser("check")
    p.add_argument("--only", nargs="*", default=None)
    p.add_argument("--pgnls", default=None, help="also run term_lint.py: gate on the supplement, report on the target")
    p = sub.add_parser("report")
    p.add_argument("--pgnls", required=True)
    p.add_argument("--base-ref", default="REL_16_11")
    args = ap.parse_args(argv)
    repo = os.path.abspath(args.repo)
    try:
        if args.cmd == "probe":
            ok, rep = do_probe(args.json)
            for c in rep["cases"]:
                print("probe %-16s %-15s rc=%d stdout=%d bytes entries=%d/%d %s" % (
                    c["case"], c["flag"], c["rc"], c["stdout_bytes"], c["entries_in_stdout"], c["expected_entries"],
                    "ok" if c["pass"] else "FAIL"))
            print("probe: %s (%s)" % ("PASS" if ok else "FAIL", rep["versions"]["msgattrib"]))
            return 0 if ok else 2
        if args.cmd == "archive":
            base_commit = args.base_commit or read_json(os.path.join(HERE, "baseline", "baseline.json"))["base_commit"]
            m = do_archive(args.pot_worktree, args.out, repo, base_commit)
            for name, r in sorted(m["components"].items()):
                print("archive %-18s %5d entries  sha256 %s  canonical %s  keys %s" % (
                    name, r["entries"], r["sha256"][:12], r["canonical_sha256"][:12], r["active_key_sha256"][:12]))
            for pr in m["problems"]:
                print("  problem: %s" % pr)
            print("archive: %s; %d components -> %s" % ("PASS" if m["pass"] else "FAIL", m["n_components"],
                                                        m["archive_dir"]))
            return 0 if m["pass"] else 1
        if args.cmd == "merge":
            rep = do_merge(args.pgnls, repo, set(args.only) if args.only else None)
            for name, r in sorted(rep["components"].items()):
                p1 = r.get("pass1", {})
                res = r.get("result", {})
                print("merge %-18s pass1: %d fuzzy %d untranslated%s -> result: %d active %d translated %d fuzzy "
                      "%d untranslated" % (name, p1.get("fuzzy", -1), p1.get("untranslated", -1),
                                           " (+supplement)" if r.get("supplement") else "",
                                           res.get("active", -1), res.get("translated", -1), res.get("fuzzy", -1),
                                           res.get("untranslated", -1)))
            for pr in rep["problems"]:
                print("  problem: %s" % pr)
            print("merge: %s" % ("PASS" if rep["pass"] else "FAIL"))
            return 0 if rep["pass"] else 1
        if args.cmd == "check":
            rep = do_check(repo, set(args.only) if args.only else None, args.pgnls)
            for name, r in sorted(rep["components"].items()):
                s = r.get("summary", {})
                tl = r.get("term_lint_target_info")
                print("check %-18s %5d active %5d translated %d fuzzy %d untranslated  msgfmt=%s msgcmp=%s%s %s" % (
                    name, s.get("active", -1), s.get("translated", -1), s.get("fuzzy", -1),
                    s.get("untranslated", -1), r.get("msgfmt", {}).get("rc"), r.get("msgcmp", {}).get("rc"),
                    (" term(pgnls-info)=%s/supp=%s" % (tl["errors"], r.get("term_lint_supplement", {}).get("errors", "-")))
                    if tl else "",
                    "ok" if not r["problems"] else "FAIL: " + "; ".join(r["problems"])))
            t = rep["totals"]
            print("check: %s; %d components, %d active, %d translated, %d fuzzy, %d untranslated" % (
                "PASS" if rep["pass"] else "FAIL", rep["n_components"], t["active"], t["translated"], t["fuzzy"],
                t["untranslated"]))
            return 0 if rep["pass"] else 1
        if args.cmd == "report":
            rep = do_report(args.pgnls, repo, args.base_ref)
            t = rep["totals"]
            print("report: POT %d entries; pgnls %d; added %d, obsolete %d; pass1 fuzzy %d untranslated %d; "
                  "supplement %d; REL_16_11 base %d entries, %d changed/added vs base; %d review-pending" % (
                      t["pot_entries"], t["pgnls_entries"], t["added_vs_pgnls"], t["obsolete_vs_pgnls"],
                      t["pass1_fuzzy"], t["pass1_untranslated"], t["supplement_entries"], t["base_entries"],
                      t["changed_vs_base"], len(rep["review_pending"])))
            return 0
    except Unverified as e:
        print("po_sync: UNVERIFIED: %s" % e, file=sys.stderr)
        return 2
    except potools.PoError as e:
        print("po_sync: UNVERIFIED: PO parse error: %s" % e, file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
