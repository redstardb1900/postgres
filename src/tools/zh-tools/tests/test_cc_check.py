"""End-to-end tests for zhtools.cc_check on a tiny real GCC build: evidence
validation (cache key, hashes, bulk files, tool versions), coverage selection,
verified / violation / unverified verdicts, reviewed platform exceptions."""
import json
import os
import shutil
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import cc_check, ccreplay, makelog, evidence_run as ev

CC = "/usr/bin/gcc"
CXX = "/usr/bin/g++"


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


@unittest.skipUnless(os.access(CC, os.X_OK), "frozen compiler /usr/bin/gcc not available")
class CcCheckTest(unittest.TestCase):
    CODE = ('#include "postgres.h"\n#include "gen.h"\n/* %s */\n#ifdef FRONTEND\nint frontend = 1;\n#endif\n'
            'const char *f(void) { return __FILE__ " " STR(GEN); }\n')
    HDR = '#define STR_(x) #x\n#define STR(x) STR_(x)\n/* %s */\nextern const char *f(void);\n'

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.src = os.path.join(self.d, "base")
        self.zh = os.path.join(self.d, "zh")
        self.bld = os.path.join(self.d, "build")
        self.cc_dir = os.path.join(self.d, "cc_evidence")
        self.bulk = os.path.join(self.d, "bulk")
        self.side = os.path.join(self.d, "side")
        for root, c, h in ((self.src, "english", "header"), (self.zh, "中文", "头文件")):
            _write(os.path.join(root, "src/a/f.c"), self.CODE % c)
            _write(os.path.join(root, "src/include/postgres.h"), self.HDR % h)
            _write(os.path.join(root, "src/port/win32only.c"), "#ifdef WIN32\nint w;\n#endif\n")
        _write(os.path.join(self.bld, "src/include/gen.h"), "#define GEN generated\n")
        os.makedirs(os.path.join(self.bld, "src/a/.deps"))
        env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}
        self.cfg = {"cc": CC, "cxx": CXX, "srcdir": self.src, "builddir": self.bld, "evidence_out": self.side,
                    "env": env, "base_commit": "0" * 40, "configure_args": ["--enable-depend"]}
        argv = [CC, "-O2", "-I../include", "-I" + self.src + "/src/include", "-c", "-o", "f.o",
                self.src + "/src/a/f.c", "-MMD", "-MP", "-MF", ".deps/f.Po"]
        argv2 = argv[:1] + ["-DFRONTEND"] + argv[1:]
        argv2[argv2.index("f.o")] = "f_shlib.o"
        argv2[-1] = ".deps/f_shlib.Po"
        log = os.path.join(self.d, "world.log")
        _write(log, "make: Entering directory '%s'\nmake[1]: Entering directory '%s/src/a'\n%s\n%s\n"
               "make[1]: Leaving directory '%s/src/a'\nmake: Leaving directory '%s'\n" % (
                   self.bld, self.bld, " ".join(argv), " ".join(argv2), self.bld, self.bld))
        for a in (argv, argv2):
            rc, out = ev.capture(a, cwd=os.path.join(self.bld, "src/a"), env=env)
            self.assertEqual(rc, 0, out)
        res = makelog.collect([("world_bin", log, "sha")], self.cfg)
        self.assertTrue(res["closure"]["closed"])
        makelog.write_collect_outputs(res, self.cc_dir, self.bulk)
        base = ccreplay.replay_all(self.cfg, self.cc_dir, self.src, "base", jobs=1,
                                   side_dir=os.path.join(self.side, "replay-base"))
        shutil.copy2(base["tokens_path"], os.path.join(self.cc_dir, "tokens.jsonl"))
        closure = ev.read_json(os.path.join(self.cc_dir, "closure.json"))
        # the real freeze() reads the full evidence set; here we build the same
        # baseline.json shape by hand with real hashes and real tool versions
        self.material = {
            "base_commit": self.cfg["base_commit"], "configure_args": self.cfg["configure_args"], "env": env,
            "cc_version": ev.first_line(ev.capture([CC, "--version"], env=env)[1]),
            "cxx_version": ev.first_line(ev.capture([CXX, "--version"], env=env)[1]),
            "make_version": ev.first_line(ev.capture(["make", "--version"], env=env)[1]),
            "logs": {"world_bin": "sha"},
            "closure_sha256": ev.sha256_file(os.path.join(self.cc_dir, "closure.json")),
            "commands_sha256": ev.sha256_file(os.path.join(self.cc_dir, "commands.jsonl")),
            "flagsets_sha256": ev.sha256_file(os.path.join(self.cc_dir, "flagsets.json")),
            "po_headers_sha256": closure["bulk"]["po_headers"]["sha256"],
            "tokens_sha256": ev.sha256_file(os.path.join(self.cc_dir, "tokens.jsonl")),
            "token_cache_sha256": base["token_cache_sha256"],
        }
        self.baseline = {
            "schema_version": 2, "status": "frozen", "base_commit": self.cfg["base_commit"],
            "srcdir": self.src, "builddir": self.bld, "cc": CC, "cxx": CXX,
            "cc_version": self.material["cc_version"], "cxx_version": self.material["cxx_version"],
            "make_version": self.material["make_version"], "configure_args": self.cfg["configure_args"],
            "env": env, "coverage_closed": True, "cache_key_material": self.material,
            "cache_key_sha256": ccreplay.compute_cache_key(self.material),
            "bulk_files": {"po_headers": closure["bulk"]["po_headers"],
                           "headers_base": {"path": base["headers_path"], "sha256": base["headers_sha256"],
                                            "size": base["headers_size"]}},
        }
        self.write_baseline()

    def write_baseline(self):
        ev.write_json(os.path.join(self.cc_dir, "baseline.json"), self.baseline)

    def tearDown(self):
        shutil.rmtree(self.d)

    def run_check(self, paths, root=None, **kw):
        # the fake builddir has no pg_config.h and the srcdir is not a git repo,
        # so the tool/srcdir checks are exercised separately
        return cc_check.run(self.cc_dir, paths, root or self.zh, jobs=1,
                            side_dir=os.path.join(self.side, "cc"), check_tools=False, **kw)

    def test_verified_for_comment_only_change(self):
        r = self.run_check(["src/a/f.c", "src/include/postgres.h"])
        self.assertEqual(r["status"], "verified", r)
        self.assertEqual(r["files_verified"], 2)
        self.assertEqual(r["variants_selected"], 2)
        self.assertEqual(r["files"]["src/include/postgres.h"]["passing_variants"], 2)

    def test_violation_for_code_change(self):
        _write(os.path.join(self.zh, "src/include/postgres.h"), (self.HDR % "x").replace("extern", "static"))
        r = self.run_check(["src/include/postgres.h"])
        self.assertEqual(r["status"], "violation", r)
        self.assertEqual(len(r["variants_different"]), 2)
        self.assertEqual(r["variants_different"][0]["kind"], "tokens")

    def test_violation_for_header_set_change(self):
        _write(os.path.join(self.zh, "src/include/extra.h"), "\n")
        _write(os.path.join(self.zh, "src/include/postgres.h"), '#include "extra.h"\n' + self.HDR % "x")
        r = self.run_check(["src/include/postgres.h"])
        self.assertEqual(r["status"], "violation")
        self.assertIn("headers", r["variants_different"][0]["kind"])

    def test_uncovered_is_unverified(self):
        r = self.run_check(["src/port/win32only.c"])
        self.assertEqual(r["status"], "unverified")
        self.assertEqual(r["uncovered"], ["src/port/win32only.c"])
        # a candidate entry does not help; only a reviewed, approved one does
        ev.write_json(os.path.join(self.cc_dir, "platform_exceptions.json"), {"exceptions": [
            {"path": "src/port/win32only.c", "status": "candidate", "platform_guard": "#ifdef WIN32",
             "reason": "x", "reviewed_by": "m", "review_commit": "c"}]})
        self.assertEqual(self.run_check(["src/port/win32only.c"])["status"], "unverified")
        ev.write_json(os.path.join(self.cc_dir, "platform_exceptions.json"), {"exceptions": [
            {"path": "src/port/win32only.c", "status": "approved", "platform_guard": "#ifdef WIN32",
             "reason": "Windows-only TU", "reviewed_by": "GPT-5.6 Sol", "review_commit": "deadbeef"}]})
        r = self.run_check(["src/port/win32only.c", "src/a/f.c"])
        self.assertEqual(r["status"], "verified", r)
        self.assertEqual(r["platform_excepted"], ["src/port/win32only.c"])

    def test_preprocess_failure_is_unverified(self):
        _write(os.path.join(self.zh, "src/include/postgres.h"), '#include "missing.h"\n')
        r = self.run_check(["src/include/postgres.h"])
        self.assertEqual(r["status"], "unverified")
        self.assertEqual(len(r["variants_failed"]), 2)

    def test_missing_root_is_unverified(self):
        r = self.run_check(["src/a/f.c"], root=os.path.join(self.d, "nope"))
        self.assertEqual(r["status"], "unverified")
        self.assertIn("root", r["reason"])

    def test_tampered_in_repo_file_is_unverified(self):
        with open(os.path.join(self.cc_dir, "tokens.jsonl"), "a") as f:
            f.write("\n")
        r = self.run_check(["src/a/f.c"])
        self.assertEqual(r["status"], "unverified")
        self.assertTrue(any("tokens.jsonl" in p for p in r["evidence_problems"]), r["evidence_problems"])

    def test_tampered_bulk_file_is_unverified(self):
        with open(self.baseline["bulk_files"]["headers_base"]["path"], "a") as f:
            f.write("\n")
        r = self.run_check(["src/a/f.c"])
        self.assertEqual(r["status"], "unverified")
        self.assertTrue(any("bulk file" in p for p in r["evidence_problems"]))

    def test_bad_cache_key_is_unverified(self):
        self.baseline["cache_key_sha256"] = "0" * 64
        self.write_baseline()
        r = self.run_check(["src/a/f.c"])
        self.assertEqual(r["status"], "unverified")
        self.assertTrue(any("cache_key" in p for p in r["evidence_problems"]))

    def test_draft_or_unclosed_is_unverified(self):
        self.baseline["coverage_closed"] = False
        self.write_baseline()
        self.assertEqual(self.run_check(["src/a/f.c"])["status"], "unverified")
        self.baseline["coverage_closed"] = True
        self.baseline["status"] = "draft"
        self.write_baseline()
        self.assertEqual(self.run_check(["src/a/f.c"])["status"], "unverified")

    def test_tool_version_check(self):
        # with check_tools the fake srcdir (no git) / builddir (no pg_config.h) are rejected ...
        ok, info, problems, _ = cc_check.load_evidence(self.cc_dir, check_tools=True)
        self.assertFalse(ok)
        self.assertTrue(any("HEAD" in p for p in problems), problems)
        self.assertTrue(any("builddir" in p for p in problems), problems)
        # ... and a frozen compiler version that no longer matches is reported
        self.baseline["cc_version"] = "gcc (GCC) 0.0.0"
        self.write_baseline()
        ok, info, problems, _ = cc_check.load_evidence(self.cc_dir, check_tools=True)
        self.assertTrue(any("reports" in p and "frozen" in p for p in problems), problems)


if __name__ == "__main__":
    unittest.main()
