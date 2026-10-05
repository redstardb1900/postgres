import json
import os
import random
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

from tests._path import FIXTURES, TOOLS_DIR  # noqa: E402
from zhtools import c_lexer, c_verify  # noqa: E402
import verify_c_comments  # noqa: E402


def rules_of(res):
    return {f.rule for f in res.violations}


def warn_rules_of(res):
    return {f.rule for f in res.warnings}


def read(name):
    with open(os.path.join(FIXTURES, name), "rb") as f:
        return f.read()


class FixedCases(unittest.TestCase):
    """Fixed positive and negative cases from tests/fixtures/negative_cases.json."""

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(FIXTURES, "negative_cases.json"), encoding="utf-8") as f:
            cls.cases = json.load(f)["cases"]

    def test_all_cases(self):
        self.assertGreaterEqual(len(self.cases), 40)
        seen = set()
        for case in self.cases:
            self.assertNotIn(case["id"], seen, "duplicate case id")
            seen.add(case["id"])
            res = c_verify.compare_files(case["old"].encode("utf-8"), case["new"].encode("utf-8"),
                                         path=case["id"])
            with self.subTest(case=case["id"]):
                if case["expect"] == "ok":
                    self.assertEqual(res.violations, [],
                                     "%s: unexpected %s" % (case["id"], [f.to_dict() for f in res.violations]))
                else:
                    self.assertIn(case["expect"], rules_of(res),
                                  "%s: got %s" % (case["id"], [f.to_dict() for f in res.violations]))
                if case.get("expect_warning"):
                    self.assertIn(case["expect_warning"], warn_rules_of(res))

    def test_sample_fixture_passes(self):
        res = c_verify.compare_files(read("sample_old.c.txt"), read("sample_zh.c.txt"), path="sample")
        self.assertEqual([f.to_dict() for f in res.violations], [])
        self.assertEqual(res.stats["changed_comments"], 7)
        self.assertNotIn("W7_IDENT_LOST", warn_rules_of(res))

    def test_sample_fixture_protected_comments_present(self):
        res = c_lexer.lex(read("sample_old.c.txt"))
        reasons = [c_verify.protected_reason(c.text) for c in res.comments]
        self.assertNotIn("pgindent_block", reasons)
        self.assertIn("translator", reasons)
        self.assertIn("fallthrough", reasons)
        self.assertIn("pg_attribute", reasons)
        header = res.comments[0].text
        self.assertTrue(header.startswith(b"/*-"))
        legal = c_verify.legal_protected_tail(header)
        self.assertIsNotNone(legal)
        self.assertIn(b"Copyright", legal)
        self.assertIsNone(c_verify.protected_reason(header))

    def test_header_module_description_translatable(self):
        old = read("sample_old.c.txt")
        # Translate only the short description line; keep Copyright tail.
        new = old.replace(
            b"Fixture for verify_c_comments.py tests (not real PostgreSQL code).",
            "verify_c_comments.py 测试夹具（非真实 PostgreSQL 代码）。".encode("utf-8"),
        )
        res = c_verify.compare_files(old, new, path="header-module")
        self.assertEqual(res.violations, [], [f.to_dict() for f in res.violations])
        # Touching Copyright must fail.
        bad = old.replace(b"Portions Copyright (c) 1996-2023", "Portions 版权 (c) 1996-2023".encode("utf-8"))
        res2 = c_verify.compare_files(old, bad, path="header-legal")
        self.assertIn("R5_PROTECTED", {f.rule for f in res2.violations})

    def test_waitio_fallthrough_prose_is_translatable(self):
        old = (
            "/*\n"
            " * Note that if the other guy has pinned the buffer but not\n"
            " * yet done StartBufferIO, WaitIO will fall through and we'll\n"
            " * be busy-looping here.\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        new = (
            "/*\n"
            " * 注意，若对方已经钉住该缓冲区，但尚未\n"
            " * 调用 StartBufferIO，WaitIO 会直接返回，我们\n"
            " * 会在这里忙等。\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        comment = c_lexer.lex(old).comments[0].text
        self.assertIsNone(c_verify.protected_reason(comment))
        res = c_verify.compare_files(old, new, path="waitio-prose")
        self.assertEqual(res.violations, [],
                         [f.to_dict() for f in res.violations])
        marker = b"/* FALLTHROUGH */\n"
        self.assertEqual(c_verify.protected_reason(marker), "fallthrough")

    def test_fsm_fallthrough_prose_is_translatable(self):
        old = (
            "/*\n"
            " * lock, it's possible to fall through and return the buffer while someone\n"
            " * else is still initializing the page. PageGetContents() does not require that.\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        new = (
            "/*\n"
            " * 没有锁，因此可能跳过初始化并返回该缓冲区，而别人\n"
            " * 仍在初始化该页。PageGetContents() 并不要求页头内容正确。\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        comment = c_lexer.lex(old).comments[0].text
        self.assertIsNone(c_verify.protected_reason(comment))
        res = c_verify.compare_files(old, new, path="fsm-prose")
        self.assertEqual(res.violations, [],
                         [f.to_dict() for f in res.violations])
        marker = b"/* FALLTHROUGH */\n"
        self.assertEqual(c_verify.protected_reason(marker), "fallthrough")

    def test_filezero_fallthrough_prose_is_translatable(self):
        old = (
            "/*\n"
            " * Return in cases of a \"real\" failure, if fallocate is not supported,\n"
            " * fall through to the FileZero() backed implementation.\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        new = (
            "/*\n"
            " * 若是 \"真正\" 的失败则返回；若 fallocate 不受支持，\n"
            " * 则落到由 FileZero() 支撑的实现。\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        comment = c_lexer.lex(old).comments[0].text
        self.assertIsNone(c_verify.protected_reason(comment))
        res = c_verify.compare_files(old, new, path="filezero-prose")
        self.assertEqual(res.violations, [],
                         [f.to_dict() for f in res.violations])
        marker = b"/* NB: need to fall through to munmap()! */\n"
        self.assertEqual(c_verify.protected_reason(marker), "fallthrough")

    def test_toast_rewrite_fallthrough_prose_is_translatable(self):
        old = (
            b"/*\n"
            b" * Check for that, and if so, just\n"
            b" * fall through without writing the data again.\n"
            b" */\n"
        )
        self.assertIsNone(c_verify.protected_reason(old))

    def test_dirty_xmax_fallthrough_prose_is_translatable(self):
        old = b"/* XXX shouldn't we fall through to look at xmax? */"
        self.assertIsNone(c_verify.protected_reason(old))
        # bare switch annotations remain protected
        self.assertEqual(
            c_verify.protected_reason(b"/* fall through */"), "fallthrough")

    def test_heapam_fallthrough_prose_is_translatable(self):
        samples = [
            (
                b"/*\n"
                b" * Reset these bits and restart; otherwise fall through to\n"
                b" * create a new multi below.\n"
                b" */\n"
            ),
            (
                b"/*\n"
                b" * otherwise we can fall through to lock this tuple\n"
                b" * version without waiting.\n"
                b" */\n"
            ),
            (
                b"/*\n"
                b" * we can fall through to allow the current\n"
                b" * transaction to lock the tuple.\n"
                b" */\n"
            ),
        ]
        for old in samples:
            self.assertIsNone(c_verify.protected_reason(old), old)
        self.assertEqual(
            c_verify.protected_reason(b"/* fall through */"), "fallthrough")

    def test_banner_fallthrough_prose_is_translatable(self):
        """File headers may document switch fall-through; not a GCC marker."""
        old = (
            "/*-------------------------------------------------------------------------\n"
            " *\n"
            " * barrier.c\n"
            " *\t  Barriers.\n"
            " *\n"
            " * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group\n"
            " *\n"
            " * Example cases that fall-through, as follows:\n"
            " *\n"
            " * IDENTIFICATION\n"
            " *\t  src/backend/storage/ipc/barrier.c\n"
            " *-------------------------------------------------------------------------\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        new = (
            "/*-------------------------------------------------------------------------\n"
            " *\n"
            " * barrier.c\n"
            " *\t  屏障。\n"
            " *\n"
            " * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group\n"
            " *\n"
            " * 示例分支会贯穿执行，如下：\n"
            " *\n"
            " * IDENTIFICATION\n"
            " *\t  src/backend/storage/ipc/barrier.c\n"
            " *-------------------------------------------------------------------------\n"
            " */\n"
            "int x;\n"
        ).encode("utf-8")
        comment = c_lexer.lex(old).comments[0].text
        self.assertIsNone(c_verify.protected_reason(comment))
        self.assertTrue(c_verify.legal_protected_segments(comment))
        res = c_verify.compare_files(old, new, path="banner-fallthrough")
        self.assertEqual(res.violations, [],
                         [f.to_dict() for f in res.violations])

    def test_no_strict_lines_demotes_rule6(self):
        old = b"/* one two */\nint a;\n"
        new = b"/* \xe4\xb8\x80\n * \xe4\xba\x8c */\nint a;\n"
        strict = c_verify.compare_files(old, new)
        self.assertIn("R6_LF_COUNT", rules_of(strict))
        loose = c_verify.compare_files(old, new, strict_lines=False)
        self.assertNotIn("R6_LF_COUNT", rules_of(loose))
        self.assertIn("W6_LF_COUNT", warn_rules_of(loose))

    def test_invalid_utf8_new(self):
        res = c_verify.compare_files(b"/* a */\n", b"/* \xff */\n")
        self.assertIn("R4_UTF8", rules_of(res))

    def test_file_level_rules_reported_even_when_skeleton_differs(self):
        res = c_verify.compare_files(b"int a;\n", b"\xef\xbb\xbfint b;\n")
        self.assertIn("R4_BOM", rules_of(res))
        self.assertIn("R1_SKELETON", rules_of(res))

    def test_skeleton_diff_context_has_line(self):
        res = c_verify.compare_files(b"int a;\nint b; /* x */\nint c;\n", b"int a;\nint B; /* x */\nint c;\n")
        v = [f for f in res.violations if f.rule == "R1_SKELETON"][0]
        self.assertEqual(v.line, 2)
        self.assertIn("old:", v.context)


class MutationTests(unittest.TestCase):
    """Randomised mutation testing with a fixed seed.

    Mutations outside comments must always be rejected (R1); mutations
    inside non-protected comment bodies that keep LF/CR counts, avoid
    comment delimiters and trigraphs must always be accepted.
    """

    SEED = 20231611
    ROUNDS = 400

    def setUp(self):
        self.rng = random.Random(self.SEED)
        self.base = read("sample_old.c.txt")
        self.lex = c_lexer.lex(self.base)
        self.comment_ranges = [(c.start, c.end) for c in self.lex.comments]

    def _in_comment(self, off):
        for s, e in self.comment_ranges:
            if s <= off < e:
                return True
        return False

    def _random_code_offset(self):
        while True:
            off = self.rng.randrange(len(self.base))
            if not self._in_comment(off):
                return off

    def test_code_mutations_are_rejected(self):
        rejected = 0
        for _ in range(self.ROUNDS):
            off = self._random_code_offset()
            kind = self.rng.choice(("replace", "insert", "delete"))
            b = bytearray(self.base)
            if kind == "replace":
                nb = self.rng.choice(b"ABCxyz01_ \t;{}()")
                if nb == b[off]:
                    nb = (nb + 1) & 0x7F or 0x41
                b[off] = nb
            elif kind == "insert":
                b[off:off] = bytes([self.rng.choice(b" \t\nabc;/*")])
            else:
                del b[off]
            mutated = bytes(b)
            if mutated == self.base:
                continue
            res = c_verify.compare_files(self.base, mutated)
            self.assertTrue(res.violations, "mutation at %d (%s) was not rejected" % (off, kind))
            rejected += 1
        self.assertGreater(rejected, self.ROUNDS // 2)

    def test_comment_body_mutations_are_accepted(self):
        accepted = 0
        safe = [c for c in self.lex.comments if c_verify.protected_reason(c.text) is None
                and c_verify.legal_protected_tail(c.text) is None
                and len(c.body()) > 4]
        self.assertTrue(safe)
        han = ["中", "文", "注", "释", "元组", "缓冲区"]
        for _ in range(self.ROUNDS):
            c = self.rng.choice(safe)
            body = c.body()
            if c.kind == c_lexer.BLOCK:
                inner_start = c.start + 2
                inner_end = c.end - 2
            else:
                inner_start = c.start + 2
                inner_end = c.end
            # choose a byte position in the body that is not a LF/CR and
            # not adjacent to delimiters, replace with Chinese text
            cand = [inner_start + i for i, ch in enumerate(body)
                    if ch not in (0x0A, 0x0D, 0x2A, 0x2F, 0x3F, 0x5C)]
            if not cand:
                continue
            off = self.rng.choice(cand)
            b = bytearray(self.base)
            b[off:off + 1] = self.rng.choice(han).encode("utf-8")
            mutated = bytes(b)
            res = c_verify.compare_files(self.base, mutated)
            self.assertEqual(res.violations, [],
                             "comment mutation at %d rejected: %s" % (off, [f.to_dict() for f in res.violations]))
            accepted += 1
            self.assertEqual(inner_end >= off, True)
        self.assertGreater(accepted, self.ROUNDS // 2)

    def test_lf_mutations_inside_comment_are_rejected(self):
        safe = [c for c in self.lex.comments if c_verify.protected_reason(c.text) is None
                and c_verify.legal_protected_tail(c.text) is None
                and c.kind == c_lexer.BLOCK and len(c.body()) > 4]
        for c in safe:
            b = bytearray(self.base)
            b[c.start + 2:c.start + 2] = b"\n"
            res = c_verify.compare_files(self.base, bytes(b))
            self.assertIn("R6_LF_COUNT", rules_of(res))

    def test_every_byte_of_header_name_after_multiline_comment_is_code(self):
        """Review finding #1: exhaustive single-byte mutation of the header name."""
        base = b"int x; /* spans\n */ #include <path//header.h>\nint q;\n"
        start = base.index(b"<path")
        end = base.index(b">", start) + 1
        for off in range(start, end):
            b = bytearray(base)
            b[off] = ord("!") if b[off] != ord("!") else ord("?")
            res = c_verify.compare_files(base, bytes(b))
            self.assertIn("R1_SKELETON", rules_of(res), "offset %d" % off)
        # translating the comment itself still passes
        ok = base.replace(b"/* spans\n */", "/* 跨行\n */".encode("utf-8"))
        self.assertEqual(c_verify.compare_files(base, ok).violations, [])

    def test_splice_after_block_comment_mutations(self):
        """Review finding #2: code after "*/\\\\\\n" is code, comment is translatable."""
        base = b"#define X /* English */\\\n  1\n#define Y /* two */\\\n  2\n"
        # comment translations pass (incl. the second one)
        new = base.replace(b"English", "英文".encode("utf-8")).replace(b"two", "二".encode("utf-8"))
        self.assertEqual([f.to_dict() for f in c_verify.compare_files(base, new).violations], [])
        # every byte outside the comments is protected
        lex = c_lexer.lex(base)
        ranges = [(c.start, c.end) for c in lex.comments]
        self.assertEqual([base[s:e] for s, e in ranges], [b"/* English */", b"/* two */"])
        for off in range(len(base)):
            if any(s <= off < e for s, e in ranges):
                continue
            b = bytearray(base)
            b[off] = ord("Z") if b[off] != ord("Z") else ord("Q")
            res = c_verify.compare_files(base, bytes(b))
            self.assertTrue(res.violations, "offset %d (%r) not rejected" % (off, base[off:off + 1]))

    def test_round_trip_random_bytes(self):
        """Lexer round-trip must hold for arbitrary byte soup."""
        alphabet = b"/*\n\\\"'#<>()RLuU8abc \t;"
        for _ in range(500):
            n = self.rng.randrange(0, 60)
            data = bytes(self.rng.choice(alphabet) for _ in range(n))
            r = c_lexer.lex(data)
            self.assertEqual(c_lexer.reassemble(r.skeleton, r.comments), data, repr(data))
            # the self comparison must never report a rule violation; an
            # unterminated comment is a parse error (R9), which is allowed
            res = c_verify.compare_files(data, data)
            self.assertTrue(all(f.rule == "R9_PARSE" for f in res.violations), repr(data))
            if res.violations:
                self.assertTrue(res.parse_error)


def _git(cwd, *args, **kw):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@x", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@x", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
    return subprocess.run(["git"] + list(args), cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw).stdout.decode()


def _write(root, rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)


class CliModes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="zhtools-vcc-")
        self.root = self.tmp.name
        self.addCleanup(self.tmp.cleanup)

    def run_cli(self, *argv):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "verify_c_comments.py")] + list(argv),
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def test_single_file_pass_and_fail(self):
        old = os.path.join(FIXTURES, "sample_old.c.txt")
        new = os.path.join(FIXTURES, "sample_zh.c.txt")
        rc, out, _ = self.run_cli(old, new, "-j", "1")
        self.assertEqual(rc, 0, out)
        bad = os.path.join(self.root, "bad.c")
        _write(self.root, "bad.c", read("sample_zh.c.txt").replace(b"result += 2", b"result += 3"))
        rc, out, _ = self.run_cli(old, bad, "-j", "1")
        self.assertEqual(rc, 1)
        self.assertIn("R1_SKELETON", out)

    def test_dir_mode_and_json(self):
        _write(self.root, "a/x.c", read("sample_old.c.txt"))
        _write(self.root, "a/y.h", b"/* h */\n")
        _write(self.root, "b/x.c", read("sample_zh.c.txt"))
        _write(self.root, "b/y.h", "/* 头 */\n".encode("utf-8"))
        rep = os.path.join(self.root, "r.json")
        rc, out, err = self.run_cli("--dir", os.path.join(self.root, "a"), os.path.join(self.root, "b"),
                                    "--json", rep, "-j", "2")
        self.assertEqual(rc, 0, out + err)
        with open(rep, encoding="utf-8") as f:
            r = json.load(f)
        self.assertEqual(r["files_checked"], 2)
        self.assertEqual(r["exit_code"], 0)
        self.assertEqual({x["path"] for x in r["files"]}, {"x.c", "y.h"})
        # a file only on one side is a violation
        _write(self.root, "b/z.cpp", b"// new\n")
        rc, out, _ = self.run_cli("--dir", os.path.join(self.root, "a"), os.path.join(self.root, "b"), "-j", "1")
        self.assertEqual(rc, 1)
        self.assertIn("R9_FILE", out)

    def test_git_and_worktree_modes(self):
        _git(self.root, "init", "-q", "-b", "main")
        _write(self.root, "src/x.c", read("sample_old.c.txt"))
        _write(self.root, "src/k.h", b"/* keep */\n")
        _write(self.root, "README", b"not c\n")
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-q", "-m", "base")
        _git(self.root, "tag", "BASE")
        # worktree: translated x.c
        _write(self.root, "src/x.c", read("sample_zh.c.txt"))
        rc, out, err = self.run_cli("--worktree", "BASE", self.root, "-j", "1")
        self.assertEqual(rc, 0, out + err)
        # untracked new .c is a violation
        _write(self.root, "src/new.c", b"/* x */\n")
        rc, out, _ = self.run_cli("--worktree", "BASE", self.root, "-j", "1")
        self.assertEqual(rc, 1)
        self.assertIn("file added", out)
        os.unlink(os.path.join(self.root, "src/new.c"))
        # --paths restricts
        _write(self.root, "src/k.h", b"/* keep */ int broken;\n")
        rc, out, _ = self.run_cli("--worktree", "BASE", self.root, "--paths", "src/x.c", "-j", "1")
        self.assertEqual(rc, 0, out)
        rc, out, _ = self.run_cli("--worktree", "BASE", self.root, "-j", "1")
        self.assertEqual(rc, 1)
        _write(self.root, "src/k.h", b"/* keep */\n")
        _git(self.root, "commit", "-q", "-am", "zh")
        _git(self.root, "tag", "ZH")
        rc, out, err = self.run_cli("--git", "BASE", "ZH", "--repo", self.root, "-j", "1")
        self.assertEqual(rc, 0, out + err)
        # deleting a tracked C file between refs is a violation
        _git(self.root, "rm", "-q", "src/k.h")
        _git(self.root, "commit", "-q", "-m", "rm")
        rc, out, _ = self.run_cli("--git", "BASE", "HEAD", "--repo", self.root, "-j", "1")
        self.assertEqual(rc, 1)
        self.assertIn("deleted", out)

    def test_self_check_mode(self):
        _git(self.root, "init", "-q", "-b", "main")
        _write(self.root, "a.c", read("sample_old.c.txt"))
        _write(self.root, "b.h", b"#error don't\n/* ok */\n")
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-q", "-m", "x")
        rep = os.path.join(self.root, "sc.json")
        rc, out, err = self.run_cli("--self-check", self.root, "--json", rep, "-j", "1")
        self.assertEqual(rc, 0, out + err)
        with open(rep, encoding="utf-8") as f:
            r = json.load(f)
        self.assertEqual(r["files_checked"], 2)
        self.assertEqual(r["violations"], 0)
        self.assertEqual(r["rule_counts"], {})

    def test_cc_check_without_usable_evidence_is_unverified_exit_2(self):
        old = os.path.join(FIXTURES, "sample_old.c.txt")
        new = os.path.join(FIXTURES, "sample_zh.c.txt")
        rep = os.path.join(self.root, "cc.json")
        # single-file mode has no tree root and an empty evidence dir -> unverified
        ev = os.path.join(self.root, "ev")
        os.makedirs(ev)
        rc, out, _ = self.run_cli(old, new, "--cc-check", "--cc-evidence", ev, "--json", rep, "-j", "1")
        self.assertEqual(rc, 2)
        self.assertIn("UNVERIFIED", out)
        with open(rep, encoding="utf-8") as f:
            r = json.load(f)
        self.assertEqual(r["cc_check"]["status"], "unverified")
        self.assertEqual(r["exit_code"], 2)
        # an old-schema or draft baseline must stay unverified
        with open(os.path.join(ev, "baseline.json"), "w") as f:
            json.dump({"schema_version": 1, "status": "frozen", "base_commit": "x", "cc": "/usr/bin/gcc",
                       "cxx": "/usr/bin/g++", "cc_version": "v", "make_version": "m", "configure_args": []}, f)
        open(os.path.join(ev, "commands.jsonl"), "w").close()
        rc, out, _ = self.run_cli(old, new, "--cc-check", "--cc-evidence", ev, "-j", "1")
        self.assertEqual(rc, 2)
        self.assertIn("schema_version", out)
        with open(os.path.join(ev, "baseline.json"), "w") as f:
            json.dump({"schema_version": 2, "status": "draft", "coverage_closed": True}, f)
        rc, out, _ = self.run_cli(old, new, "--cc-check", "--cc-evidence", ev, "-j", "1")
        self.assertEqual(rc, 2)
        self.assertIn("not 'frozen'", out)

    def test_parse_error_exit_2(self):
        _write(self.root, "o.c", b"int a; /* open\n")
        _write(self.root, "n.c", b"int a; /* open\n")
        rc, out, _ = self.run_cli(os.path.join(self.root, "o.c"), os.path.join(self.root, "n.c"), "-j", "1")
        self.assertEqual(rc, 2)
        self.assertIn("R9_PARSE", out)

    def test_no_pycache_created(self):
        old = os.path.join(FIXTURES, "sample_old.c.txt")
        self.run_cli(old, old, "-j", "1")
        for dp, dns, _ in os.walk(TOOLS_DIR):
            self.assertNotIn("__pycache__", dns, dp)


if __name__ == "__main__":
    unittest.main()
