"""Tests for the halfen3 half-English debt scanner (zhtools/halfen.py,
scan_halfen.py CLI)."""
import json
import os
import re
import subprocess
import sys
import unittest

from tests._path import TOOLS_DIR

sys.dont_write_bytecode = True

from zhtools import halfen  # noqa: E402


def zh(text):
    return text.encode("utf-8")


class TokenRules(unittest.TestCase):
    """halfen2 token rules, kept verbatim from the campaign scanner."""

    def test_prose_tokens(self):
        self.assertTrue(halfen.is_prose_token(b"the"))
        self.assertTrue(halfen.is_prose_token(b"result,"))
        self.assertTrue(halfen.is_prose_token(b"(indexkey"))  # parens stripped
        self.assertTrue(halfen.is_prose_token(b" kB"))        # unit word alone
        self.assertTrue(halfen.is_prose_token(b"Autovacuum"))

    def test_non_prose_tokens(self):
        for tok in (b"x",                             # lone letter
                    b"16",                            # number
                    b"ALTER",                         # all-caps keyword
                    b"ATRewriteCatalogs",             # CamelCase (>=2 uppers)
                    b"pg_attribute_c",                # identifier
                    b"ExecInitNode()",                # call
                    b"----",                          # separator line
                    ):
            self.assertFalse(halfen.is_prose_token(tok), tok)

    def test_cjk_bearing_token_never_counts(self):
        # Chinese wording around a kept term is not an English fragment.
        self.assertFalse(halfen.is_prose_token(zh("poll(2)，否则用")))
        self.assertFalse(halfen.is_prose_token(zh("Copy Data，传回调用者")))


class Classify(unittest.TestCase):
    def test_mixed(self):
        self.assertEqual(halfen.classify(zh("返回 the result set 行")), "mixed")
        self.assertEqual(halfen.classify(zh("这种 fetch the tuple 的做法")), "mixed")

    def test_mixed_requires_two_consecutive_tokens(self):
        # single kept term between Chinese words stays quiet
        self.assertIsNone(halfen.classify(zh("返回 tuple 行")))
        self.assertIsNone(halfen.classify(zh("可用时用 poll(2)，否则用 select(2)")))
        # "Copy Data，..." glues the second word to Chinese: run of 1
        self.assertIsNone(halfen.classify(zh("Copy Data，传回调用者")))
        # but a real two-word run still flags
        self.assertEqual(halfen.classify(zh("Copy Data 传回调用者")), "mixed")

    def test_pureen_needs_flag(self):
        self.assertIsNone(halfen.classify(b"this is a plain english comment"))
        self.assertEqual(halfen.classify(b"this is a plain english comment",
                                         include_pureen=True), "pureen")
        self.assertIsNone(halfen.classify(b"return -1;", include_pureen=True))
        self.assertIsNone(halfen.classify(zh("返回结果集"), include_pureen=True))


class ScanBytes(unittest.TestCase):
    def test_code_lines_never_examined(self):
        """The recorded ``*ptr = `` false-positive family: real code lines
        with a leading ``*`` must never be classified, only comments."""
        src = (zh("/* 返回 the result set 行 */\n")
               + b"*parameterDefaults = lappend(*parameterDefaults, def);\n"
               + b"*Irel = (Relation *) palloc(i * sizeof(Relation));\n"
               + b"*list = lappend(*list, member);\n"
               + b"return -1;\n")
        findings = halfen.scan_bytes(src, "t.c")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 1)
        self.assertEqual(findings[0].cls, halfen.MIXED)
        self.assertEqual(findings[0].run_text, "the result set")

    def test_fully_clean_file(self):
        src = (zh("/* 返回结果集的行数。*/\n")
               + b"*parameterTypes = buildoidvector(inTypes, inCount);\n")
        self.assertEqual(halfen.scan_bytes(src, "t.c"), [])

    def test_protected_and_copyright_exempt(self):
        src = (b"/* FALLTHROUGH */\n"
               b"/* translator: foo. */\n"
               b"/* pg_attribute_noreturn() */\n"
               b"/*\n * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group\n */\n")
        self.assertEqual(halfen.scan_bytes(src, "t.c"), [])

    def test_builtin_pattern_exempts_wal_types(self):
        src = zh("/* 处理 running xacts 记录。*/\n")
        self.assertEqual(halfen.scan_bytes(src, "t.c"), [])

    def test_site_exemptions(self):
        src = zh("/* 使用 Gather Merge 节点。*/\n")
        site = [(re.compile("t\\.c"), re.compile(b"Gather Merge"),
                 "node name kept verbatim")]
        self.assertEqual(halfen.scan_bytes(src, "t.c", site_exemptions=site), [])
        # wrong path -> still flagged
        site2 = [(re.compile("other\\.c"), re.compile(b"Gather Merge"), "x")]
        self.assertEqual(len(halfen.scan_bytes(src, "t.c", site_exemptions=site2)), 1)

    def test_unterminated_comment_still_scanned(self):
        # the lexer warns instead of raising; the comment is still scanned
        findings = halfen.scan_bytes(zh("/* 返回 the result set 行\nint a;"), "t.c")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].cls, halfen.MIXED)


class SiteLedger(unittest.TestCase):
    def test_load_site_exemptions(self):
        path = os.path.join(TOOLS_DIR, "halfen_exemptions.json")
        with open(path, encoding="utf-8") as f:
            entries = json.load(f)
        self.assertIsInstance(entries, list)
        loaded = halfen.load_site_exemptions(path)
        self.assertEqual(len(loaded), len(entries))

    def test_entry_without_reason_rejected(self):
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump([{"path": "a.c", "pattern": "x"}], f)
            bad = f.name
        try:
            with self.assertRaises(ValueError):
                halfen.load_site_exemptions(bad)
        finally:
            os.unlink(bad)


class Cli(unittest.TestCase):
    """End-to-end through git modes on the real repository."""

    @classmethod
    def setUpClass(cls):
        cls.repo = os.path.dirname(TOOLS_DIR.rstrip("/"))  # .../src/tools
        cls.repo = os.path.dirname(os.path.dirname(cls.repo))
        cls.cli = os.path.join(TOOLS_DIR, "scan_halfen.py")

    def run_cli(self, *args):
        env = dict(os.environ)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        p = subprocess.run([sys.executable, "-I", self.cli] + list(args),
                           capture_output=True, text=True, env=env, cwd=self.repo)
        return p.returncode, p.stdout, p.stderr

    def test_git_mode_single_file(self):
        rc, out, err = self.run_cli("--git", "HEAD", "--paths",
                                    "src/backend/commands/analyzecc.c")
        self.assertIn(err, ("", None))
        # whatever the debt state, the CLI must report and exit 0/1 only
        self.assertIn(rc, (0, 1))
        self.assertRegex(out, r"scan_halfen: \d+ files, \d+ findings")

    def test_diff_mode_and_json(self):
        rc, out, err = self.run_cli("--git-diff", "REL_16_11", "HEAD", "--paths",
                                    "src/backend/commands", "--no-exemptions")
        self.assertIn(rc, (0, 1))
        self.assertRegex(out, r"scan_halfen: \d+ files, \d+ findings")


if __name__ == "__main__":
    unittest.main()
