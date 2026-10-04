"""Tests for zhtools.potools: the mechanical PO reader behind the PO gate."""
import os
import shutil
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import potools

HEADER = ('msgid ""\nmsgstr ""\n"Project-Id-Version: x\\n"\n"POT-Creation-Date: 2026-10-04 21:32+0800\\n"\n'
          '"Language: zh_CN\\n"\n"Content-Type: text/plain; charset=UTF-8\\n"\n'
          '"Plural-Forms: nplurals=1; plural=0;\\n"\n\n')

SAMPLE = HEADER + '''#. extracted
#: a.c:1 b.c:2
#, c-format
msgid "hello \\"%s\\"\\n"
msgstr "你好 \\"%s\\"\\n"

msgctxt "ctx"
msgid "hello \\"%s\\"\\n"
msgstr "有上下文"

#, fuzzy
#| msgid "old one"
msgid "new one"
msgstr "旧的"

msgid "one %d"
msgid_plural "many %d"
msgstr[0] "%d 个"

msgid ""
"multi\\n"
"line"
msgstr ""
"多\\n"
"行"

msgid "untranslated"
msgstr ""

#~ msgid "gone"
#~ msgstr "没了"
'''


class UnescapeEscapeTest(unittest.TestCase):
    def test_roundtrip(self):
        for s in ("", "a", 'q"q', "back\\slash", "nl\nx", "tab\t", "\x01ctl", "中文"):
            self.assertEqual(potools.unescape(potools.escape(s)), s)

    def test_octal_and_hex(self):
        self.assertEqual(potools.unescape("\\101\\x42"), "AB")
        with self.assertRaises(potools.PoError):
            potools.unescape("bad\\q")
        with self.assertRaises(potools.PoError):
            potools.unescape("dangling\\")


class ParseTest(unittest.TestCase):
    def setUp(self):
        self.entries = potools.parse(SAMPLE)

    def test_structure(self):
        e = self.entries
        self.assertEqual(len(e), 8)
        self.assertTrue(e[0].is_header)
        self.assertEqual(e[1].msgid, 'hello "%s"\n')
        self.assertEqual(e[1].msgstr, '你好 "%s"\n')
        self.assertEqual(e[1].references, ["a.c:1", "b.c:2"])
        self.assertEqual(e[1].flags, ["c-format"])
        self.assertEqual(e[1].comments, ["#. extracted"])
        self.assertEqual(e[2].key, ("ctx", 'hello "%s"\n'))
        self.assertTrue(e[3].fuzzy)
        self.assertEqual(e[3].previous, ['msgid "old one"'])
        self.assertEqual(e[4].msgid_plural, "many %d")
        self.assertEqual(e[4].msgstr_plural, {0: "%d 个"})
        self.assertEqual(e[5].msgid, "multi\nline")
        self.assertEqual(e[5].msgstr, "多\n行")
        self.assertFalse(e[6].translated)
        self.assertTrue(e[7].obsolete)
        self.assertEqual(e[7].msgstr, "没了")

    def test_summary_and_keys(self):
        s = potools.summarize(self.entries)
        self.assertEqual(s, {"entries_total": 8, "active": 6, "obsolete": 1, "fuzzy": 1, "untranslated": 1,
                             "translated": 4, "plural_entries": 1, "with_msgctxt": 1})
        self.assertEqual(potools.duplicate_keys(self.entries), [])
        dup = potools.parse(HEADER + 'msgid "a"\nmsgstr "1"\n\nmsgid "a"\nmsgstr "2"\n')
        self.assertEqual(potools.duplicate_keys(dup), [(None, "a")])
        # same msgid under different contexts is not a duplicate
        self.assertEqual(len(set(potools.active_keys(self.entries))), 6)

    def test_header(self):
        h = potools.header_fields(self.entries)
        self.assertEqual(h["Language"], "zh_CN")
        self.assertEqual(potools.plural_forms_nplurals(h), 1)
        self.assertIsNone(potools.plural_forms_nplurals({"Plural-Forms": "garbage"}))

    def test_plurals(self):
        self.assertEqual(potools.check_plurals(self.entries, 1), [])
        bad = potools.check_plurals(self.entries, 2)
        self.assertEqual(len(bad), 1)
        self.assertEqual(bad[0]["have"], [0])
        empty = potools.parse(HEADER + 'msgid "x"\nmsgid_plural "xs"\nmsgstr[0] ""\n')
        self.assertEqual(potools.check_plurals(empty, 1)[0]["empty_forms"], [0])

    def test_errors(self):
        with self.assertRaises(potools.PoError):
            potools.parse('msgid "a"\nmsgstr "b"\n\nmsgstr "orphan"\n')
        with self.assertRaises(potools.PoError):
            potools.parse('garbage line\n')
        with self.assertRaises(potools.PoError):
            potools.parse('msgid "unterminated\n')

    def test_active_key_hash_is_order_and_translation_independent(self):
        a = potools.parse(HEADER + 'msgid "a"\nmsgstr "1"\n\nmsgid "b"\nmsgstr "2"\n')
        b = potools.parse(HEADER + '#: x.c:1\nmsgid "b"\nmsgstr "other"\n\nmsgid "a"\nmsgstr ""\n')
        c = potools.parse(HEADER + 'msgid "a"\nmsgstr "1"\n\nmsgid "c"\nmsgstr "2"\n')
        d = potools.parse(HEADER + 'msgid "a"\nmsgstr "1"\n\nmsgctxt "k"\nmsgid "b"\nmsgstr "2"\n')
        p = potools.parse(HEADER + 'msgid "a"\nmsgstr "1"\n\nmsgid "b"\nmsgid_plural "bs"\nmsgstr[0] "2"\n')
        self.assertEqual(potools.active_key_hash(a), potools.active_key_hash(b))
        self.assertNotEqual(potools.active_key_hash(a), potools.active_key_hash(c))
        self.assertNotEqual(potools.active_key_hash(a), potools.active_key_hash(d))
        self.assertNotEqual(potools.active_key_hash(a), potools.active_key_hash(p))


class FileTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d)

    def _w(self, name, text, mode="w"):
        p = os.path.join(self.d, name)
        with open(p, mode, encoding=None if "b" in mode else "utf-8") as f:
            f.write(text)
        return p

    def test_canonical_pot_hash_ignores_creation_date_only(self):
        a = self._w("a.pot", SAMPLE)
        b = self._w("b.pot", SAMPLE.replace("2026-10-04 21:32+0800", "2027-01-01 00:00+0000"))
        c = self._w("c.pot", SAMPLE.replace('msgid "new one"', 'msgid "new one!"'))
        d = self._w("d.pot", SAMPLE.replace("#: a.c:1 b.c:2", "#: a.c:9 b.c:2"))
        self.assertEqual(potools.canonical_pot_hash(a), potools.canonical_pot_hash(b))
        self.assertNotEqual(potools.canonical_pot_hash(a), potools.canonical_pot_hash(c))
        self.assertNotEqual(potools.canonical_pot_hash(a), potools.canonical_pot_hash(d))
        # the raw file hashes differ even when only the date differs
        with open(a, "rb") as fa, open(b, "rb") as fb:
            self.assertNotEqual(fa.read(), fb.read())

    def test_read_rejects_bom_and_non_utf8(self):
        bom = self._w("bom.po", b"\xef\xbb\xbf" + SAMPLE.encode("utf-8"), "wb")
        with self.assertRaises(potools.PoError):
            potools.read(bom)
        bad = self._w("bad.po", SAMPLE.encode("utf-8") + b'\nmsgid "\xff"\nmsgstr ""\n', "wb")
        with self.assertRaises(potools.PoError):
            potools.read(bad)
        ok = self._w("ok.po", SAMPLE)
        self.assertEqual(len(potools.read(ok)), 8)


class SerializeTest(unittest.TestCase):
    def test_roundtrip_through_parse(self):
        for e in potools.parse(SAMPLE):
            if e.obsolete:
                continue
            text = potools.serialize_entry(e)
            back = potools.parse(text)
            self.assertEqual(len(back), 1, text)
            b = back[0]
            self.assertEqual(b.key, e.key)
            self.assertEqual(b.msgid_plural, e.msgid_plural)
            self.assertEqual(b.translations(), e.translations())
            self.assertEqual(b.flags, e.flags)
            self.assertEqual(b.references, e.references)
            self.assertEqual(b.comments, e.comments)
            self.assertEqual(b.previous, e.previous)

    def test_multiline_layout_matches_gettext_no_wrap(self):
        e = potools.Entry(1)
        e.msgid = "multi\nline"
        e.msgstr = "多\n行"
        self.assertEqual(potools.serialize_entry(e), 'msgid ""\n"multi\\n"\n"line"\nmsgstr ""\n"多\\n"\n"行"')
        e.msgid = "trail\n"
        e.msgstr = "尾\n"
        self.assertEqual(potools.serialize_entry(e), 'msgid "trail\\n"\nmsgstr "尾\\n"')

    def test_split_blocks(self):
        blocks = potools.split_blocks(SAMPLE)
        self.assertEqual(len(blocks), 8)
        self.assertTrue(blocks[0].startswith('msgid ""'))
        self.assertTrue(blocks[-1].startswith("#~"))


if __name__ == "__main__":
    unittest.main()
