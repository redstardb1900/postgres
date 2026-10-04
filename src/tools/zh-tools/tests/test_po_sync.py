"""Tests for po_sync.py: the msgattrib probe, component discovery, the per-catalog
gate (check_one) and the obsolete-revival override.  Cases that need the real
gettext tools are skipped when they are not installed."""
import os
import shutil
import subprocess
import tempfile
import unittest

from . import _path  # noqa: F401
import po_sync  # noqa: E402
from zhtools import potools  # noqa: E402

HAVE_GETTEXT = all(shutil.which(t) for t in po_sync.GETTEXT_TOOLS)

HEADER = ('msgid ""\nmsgstr ""\n"Project-Id-Version: x\\n"\n"POT-Creation-Date: 2026-10-04 21:32+0800\\n"\n'
          '"PO-Revision-Date: 2026-10-04 21:40+0800\\n"\n"Last-Translator: t\\n"\n'
          '"Language: zh_CN\\n"\n"MIME-Version: 1.0\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
          '"Content-Transfer-Encoding: 8bit\\n"\n"Plural-Forms: nplurals=1; plural=0;\\n"\n\n')
POT = ('msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n'
       '#: a.c:1\n#, c-format\nmsgid "file \\"%s\\" not found"\nmsgstr ""\n\n'
       '#: a.c:2\nmsgid "one"\nmsgid_plural "many"\nmsgstr[0] ""\n\n'
       '#: a.c:3\nmsgctxt "verb"\nmsgid "close"\nmsgstr ""\n')
GOOD = (HEADER + '#: a.c:1\n#, c-format\nmsgid "file \\"%s\\" not found"\nmsgstr "找不到文件 \\"%s\\""\n\n'
        '#: a.c:2\nmsgid "one"\nmsgid_plural "many"\nmsgstr[0] "%d 个"\n\n'
        '#: a.c:3\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关闭"\n')


def _w(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


@unittest.skipUnless(HAVE_GETTEXT, "GNU gettext tools not installed")
class ProbeTest(unittest.TestCase):
    def test_probe_passes_and_records_versions(self):
        ok, rep = po_sync.do_probe()
        self.assertTrue(ok, rep)
        self.assertEqual(len(rep["cases"]), 6)
        self.assertTrue(all(c["pass"] for c in rep["cases"]))
        self.assertIn("msgattrib", rep["versions"])
        zero = [c for c in rep["cases"] if c["expected_entries"] == 0]
        self.assertTrue(all(c["stdout_bytes"] == 0 for c in zero))


class ComponentsTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.env = {"PATH": "/usr/bin:/bin", "HOME": self.d, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@x",
                    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@x"}

    def tearDown(self):
        shutil.rmtree(self.d)

    def _git(self, *a):
        subprocess.run(["git"] + list(a), cwd=self.d, env=self.env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    def _repo(self, dirs):
        for name, d in dirs.items():
            _w(os.path.join(self.d, d, "nls.mk"), "# nls\nCATALOG_NAME     = %s\nGETTEXT_FILES = x.c\n" % name)
        self._git("init", "-q")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "x")

    def test_exactly_26_with_fixed_mapping(self):
        dirs = {"c%02d" % i: "src/x/c%02d" % i for i in range(24)}
        dirs["ecpg"] = "src/interfaces/ecpg/preproc"
        dirs["pgscripts"] = "src/bin/scripts"
        self._repo(dirs)
        comps = po_sync.components(self.d)
        self.assertEqual(len(comps), 26)
        self.assertEqual(comps["ecpg"], "src/interfaces/ecpg/preproc")
        self.assertEqual(comps["pgscripts"], "src/bin/scripts")

    def test_wrong_count_or_mapping_is_unverified(self):
        dirs = {"c%02d" % i: "src/x/c%02d" % i for i in range(25)}
        dirs["ecpg"] = "src/interfaces/ecpg/preproc"
        self._repo(dirs)
        with self.assertRaises(po_sync.Unverified):
            po_sync.components(self.d)  # 26 dirs but pgscripts missing / mapped wrong
        shutil.rmtree(os.path.join(self.d, "src/x/c24"))
        _w(os.path.join(self.d, "src/bin/scripts/nls.mk"), "CATALOG_NAME = pgscripts\n")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "y")
        self.assertEqual(len(po_sync.components(self.d)), 26)
        _w(os.path.join(self.d, "src/x/extra/nls.mk"), "CATALOG_NAME = extra\n")
        self._git("add", "-A")
        with self.assertRaises(po_sync.Unverified):
            po_sync.components(self.d)  # 27


@unittest.skipUnless(HAVE_GETTEXT, "GNU gettext tools not installed")
class CheckOneTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.pot = _w(os.path.join(self.d, "x.pot"), POT)

    def tearDown(self):
        shutil.rmtree(self.d)

    def _check(self, text, supplement=None):
        target = _w(os.path.join(self.d, "zh_CN.po"), text)
        return po_sync.check_one("x", target, self.pot, supplement)

    def test_good_catalog_passes(self):
        ok, rec = self._check(GOOD)
        self.assertTrue(ok, rec["problems"])
        self.assertEqual(rec["msgfmt"]["rc"], 0)
        self.assertEqual(rec["msgcmp"]["rc"], 0)
        self.assertEqual(rec["summary"]["active"], 3)
        self.assertEqual(rec["keys"], {"pot": 3, "po": 3, "missing_in_po": 0, "extra_in_po": 0})
        self.assertEqual(rec["active_key_sha256"], rec["pot_active_key_sha256"])
        self.assertTrue(rec["msgattrib_only_fuzzy"]["pass"])
        self.assertTrue(rec["msgattrib_untranslated"]["pass"])

    def test_fuzzy_fails_both_ways(self):
        ok, rec = self._check(GOOD.replace('#: a.c:3\nmsgctxt', '#: a.c:3\n#, fuzzy\nmsgctxt'))
        self.assertFalse(ok)
        self.assertTrue(any("--only-fuzzy printed" in p for p in rec["problems"]), rec["problems"])
        self.assertIn("1 fuzzy entries (parser)", rec["problems"])
        self.assertNotEqual(rec["msgcmp"]["rc"], 0)

    def test_untranslated_fails(self):
        ok, rec = self._check(GOOD.replace('msgstr "关闭"', 'msgstr ""'))
        self.assertFalse(ok)
        self.assertTrue(any("--untranslated printed" in p for p in rec["problems"]), rec["problems"])
        self.assertIn("1 untranslated entries (parser)", rec["problems"])

    def test_missing_and_extra_keys(self):
        ok, rec = self._check(GOOD.replace('#: a.c:3\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关闭"\n', ""))
        self.assertFalse(ok)
        self.assertNotEqual(rec["msgcmp"]["rc"], 0)
        self.assertEqual(rec["keys"]["missing_in_po"], 1)
        ok, rec = self._check(GOOD + '\nmsgid "extra"\nmsgstr "多余"\n')
        self.assertFalse(ok)
        self.assertEqual(rec["keys"]["extra_in_po"], 1)
        self.assertTrue(any("active key set differs" in p for p in rec["problems"]))

    def test_duplicate_key_and_obsolete(self):
        ok, rec = self._check(GOOD + '\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关"\n')
        self.assertFalse(ok)
        self.assertNotEqual(rec["msgfmt"]["rc"], 0)  # msgfmt rejects duplicates itself
        ok, rec = self._check(GOOD + '\n#~ msgid "old"\n#~ msgstr "旧"\n')
        self.assertFalse(ok)
        self.assertIn("1 obsolete entries left", rec["problems"])

    def test_plural_incomplete_and_header(self):
        two = GOOD.replace("nplurals=1; plural=0;", "nplurals=2; plural=(n != 1);")
        ok, rec = self._check(two)
        self.assertFalse(ok)
        self.assertIn("1 incomplete plural entries", rec["problems"])
        ok, rec = self._check(GOOD.replace('"Language: zh_CN\\n"\n', '"Language: zh\\n"\n'))
        self.assertFalse(ok)
        self.assertIn("header Language is 'zh'", rec["problems"])
        ok, rec = self._check(GOOD.replace('"Plural-Forms: nplurals=1; plural=0;\\n"\n', ""))
        self.assertFalse(ok)
        self.assertIn("header Plural-Forms missing or without nplurals", rec["problems"])
        ok, rec = self._check(GOOD.replace("charset=UTF-8", "charset=GB2312"))
        self.assertFalse(ok)
        self.assertTrue(any(p.startswith("header Content-Type") for p in rec["problems"]))

    def test_format_mismatch_is_caught_by_msgfmt(self):
        ok, rec = self._check(GOOD.replace('msgstr "找不到文件 \\"%s\\""', 'msgstr "找不到文件 \\"%d\\""'))
        self.assertFalse(ok)
        self.assertNotEqual(rec["msgfmt"]["rc"], 0)

    def test_supplement_accounting(self):
        sup = _w(os.path.join(self.d, "sup.po"), HEADER +
                 '# zh-reviewed: test\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关闭"\n\n'
                 '# zh-reviewed:\nmsgid "stale"\nmsgstr "过期"\n\n'
                 'msgid "one"\nmsgid_plural "many"\nmsgstr[0] "%d 项"\n')
        marked = GOOD.replace('#: a.c:3\nmsgctxt', '# zh-reviewed: test\n#: a.c:3\nmsgctxt')
        ok, rec = self._check(marked, sup)
        self.assertFalse(ok)
        s = rec["supplement"]
        self.assertEqual([k["msgid"] for k in s["used"]], ["close"])
        self.assertEqual([k["msgid"] for k in s["unused"]], ["stale"])
        self.assertEqual([k["msgid"] for k in s["mismatched"]], ["one"])
        self.assertEqual([k["msgid"] for k in s["unflagged"]], ["one"])
        self.assertTrue(any("stale" in p for p in rec["problems"]))
        self.assertTrue(any("overridden by pgnls" in p for p in rec["problems"]))
        self.assertTrue(any("translator comment" in p for p in rec["problems"]))
        # a clean supplement whose single entry is marked in the target passes
        sup2 = _w(os.path.join(self.d, "sup2.po"), HEADER +
                  '# zh-reviewed: test\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关闭"\n')
        ok, rec = self._check(marked, sup2)
        self.assertTrue(ok, rec["problems"])
        self.assertEqual(rec["supplement"]["review_marks_in_target"], 1)
        self.assertEqual(rec["supplement"]["still_pending"], [])
        self.assertEqual(rec["supplement"]["pending_marks_in_target"], 0)
        # leftover pending marks fail even when a reviewed mark is also present
        pending = GOOD.replace('#: a.c:3\nmsgctxt',
                               '# zh-review-pending: leftover\n# zh-reviewed: test\n#: a.c:3\nmsgctxt')
        ok, rec = self._check(pending, sup2)
        self.assertFalse(ok)
        self.assertTrue(any("still carry" in p for p in rec["problems"]), rec["problems"])
        # the mark on a non-supplement entry is a mismatch
        ok, rec = self._check(marked.replace('#: a.c:2\n', '# zh-reviewed: stray\n#: a.c:2\n'), sup2)
        self.assertFalse(ok)
        self.assertTrue(any("review marks in target" in p for p in rec["problems"]), rec["problems"])


@unittest.skipUnless(HAVE_GETTEXT, "GNU gettext tools not installed")
class MergePipelineTest(unittest.TestCase):
    """The prescribed msgmerge/msgattrib pipeline plus the override stage on tiny catalogs."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.pot = _w(os.path.join(self.d, "x.pot"), POT + '\n#: a.c:4\nmsgid "revived"\nmsgstr ""\n\n'
                      '#: a.c:5\nmsgid "brand new"\nmsgstr ""\n')
        # pgnls: translates two keys actively, carries "revived" only as obsolete, lacks "brand new"
        self.defpo = _w(os.path.join(self.d, "pgnls.po"), HEADER +
                        '#: a.c:1\n#, c-format\nmsgid "file \\"%s\\" not found"\nmsgstr "找不到文件 \\"%s\\""\n\n'
                        '#: a.c:2\nmsgid "one"\nmsgid_plural "many"\nmsgstr[0] "%d 个"\n\n'
                        '#: a.c:3\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关闭"\n\n'
                        '#~ msgid "revived"\n#~ msgstr "旧译法"\n')
        self.sup = _w(os.path.join(self.d, "sup.po"), HEADER +
                      '# zh-reviewed: new\nmsgid "brand new"\nmsgstr "全新"\n\n'
                      '# zh-reviewed: OVERRIDE\nmsgid "revived"\nmsgstr "新译法"\n')

    def tearDown(self):
        shutil.rmtree(self.d)

    def _pipeline(self, compendium):
        tmp = os.path.join(self.d, "tmp.po")
        argv, rc, err = po_sync._msgmerge(self.defpo, self.pot, tmp, compendium)
        self.assertEqual(rc, 0, err)
        self.assertEqual(argv[:7], ["msgmerge", "--quiet", "--no-wrap", "--previous", "--sort-by-file",
                                    "--lang=zh_CN", "-C" if compendium else self.defpo])
        target = os.path.join(self.d, "zh_CN.po")
        argv, rc, err = po_sync._msgattrib_active(tmp, target)
        self.assertEqual(rc, 0, err)
        self.assertEqual(argv[:4], ["msgattrib", "--no-obsolete", "--no-wrap", "-o"])
        return target

    def test_plain_merge_leaves_new_untranslated_and_revives_obsolete(self):
        target = self._pipeline(None)
        s = potools.summarize(potools.read(target))
        self.assertEqual((s["active"], s["untranslated"], s["fuzzy"], s["obsolete"]), (5, 1, 0, 0))
        by = {e.key: e for e in potools.active_entries(potools.read(target))}
        self.assertEqual(by[(None, "revived")].msgstr, "旧译法")

    def test_compendium_fills_new_but_not_revived_then_override_does(self):
        target = self._pipeline(self.sup)
        by = {e.key: e for e in potools.active_entries(potools.read(target))}
        self.assertEqual(by[(None, "brand new")].msgstr, "全新")
        self.assertIn("# zh-reviewed: new", by[(None, "brand new")].comments)
        self.assertEqual(by[(None, "revived")].msgstr, "旧译法")  # def.po obsolete wins over -C
        applied, problems = po_sync._apply_overrides(target, self.sup, self.defpo)
        self.assertEqual(problems, [])
        self.assertEqual(len(applied), 1)
        self.assertEqual(applied[0]["key"]["msgid"], "revived")
        self.assertEqual(applied[0]["pgnls_obsolete_msgstr"], ["旧译法"])
        argv, rc, err = po_sync._msgattrib_active(target, target)
        self.assertEqual(rc, 0, err)
        entries = potools.read(target)
        by = {e.key: e for e in potools.active_entries(entries)}
        self.assertEqual(by[(None, "revived")].msgstr, "新译法")
        self.assertEqual(by[(None, "revived")].references, ["a.c:4"])
        self.assertTrue(any(c.startswith("# zh-reviewed: OVERRIDE") for c in by[(None, "revived")].comments))
        # idempotent and gate-clean
        applied2, _ = po_sync._apply_overrides(target, self.sup, self.defpo)
        self.assertEqual(applied2, [])
        ok, rec = po_sync.check_one("x", target, self.pot, self.sup)
        self.assertTrue(ok, rec["problems"])
        self.assertEqual(rec["supplement"]["review_marks_in_target"], 2)

    def test_override_refuses_to_beat_an_active_pgnls_translation(self):
        sup = _w(os.path.join(self.d, "sup_conflict.po"), HEADER +
                 '# zh-reviewed:\nmsgctxt "verb"\nmsgid "close"\nmsgstr "关上"\n')
        target = self._pipeline(sup)
        applied, problems = po_sync._apply_overrides(target, sup, self.defpo)
        self.assertEqual(applied, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("conflicts with an active pgnls translation", problems[0])
        by = {e.key: e for e in potools.active_entries(potools.read(target))}
        self.assertEqual(by[("verb", "close")].msgstr, "关闭")


if __name__ == "__main__":
    unittest.main()
