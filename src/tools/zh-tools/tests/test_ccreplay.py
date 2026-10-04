"""Tests for zhtools.ccreplay: argv rewriting / path mapping, depfile normalisation,
canonical tokens, cache key, and -- when /usr/bin/gcc exists -- a real two-root replay."""
import json
import os
import shutil
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import ccreplay, makelog, evidence_run as ev

CC = "/usr/bin/gcc"
CXX = "/usr/bin/g++"
SRC = "/roots/base"
ZH = "/roots/zh"
BLD = "/roots/build"
CWD = BLD + "/src/backend/access/heap"


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def _read(path):
    with open(path, "rb") as f:
        return f.read()


class RewriteArgvTest(unittest.TestCase):
    ARGV = [CC, "-Wall", "-O2", "-I../../../../src/include", "-I" + SRC + "/src/include", "-I", SRC + "/src/pl/x",
            "-iquote", ".", "-isystem", "/usr/include/libxml2", "-D_GNU_SOURCE", '-DVAL_CC="/usr/bin/gcc"',
            "-c", "-o", "heapam.o", SRC + "/src/backend/access/heap/heapam.c", "-MMD", "-MP", "-MF", ".deps/heapam.Po"]

    def rewrite(self, root):
        return ccreplay.rewrite_argv(self.ARGV, CWD, SRC, BLD, root, "src/backend/access/heap/heapam.o", "/side/heapam.d")

    def test_mapping_and_order(self):
        real, canon, info = self.rewrite(ZH)
        # builddir paths kept (made absolute), srcdir paths re-rooted, system kept, order preserved
        self.assertEqual(real[1:10], ["-Wall", "-O2", "-I" + BLD + "/src/include", "-I" + ZH + "/src/include",
                                      "-I", ZH + "/src/pl/x", "-iquote", CWD, "-isystem"])
        self.assertEqual(real[10], "/usr/include/libxml2")
        self.assertIn(ZH + "/src/backend/access/heap/heapam.c", real)
        self.assertEqual(info["source_kind"], "src")
        self.assertEqual((info["n_src_paths"], info["n_build_paths"], info["n_sys_paths"]), (3, 2, 1))
        # removed
        for t in ("-c", "-MP", ".deps/heapam.Po", "heapam.o"):
            self.assertNotIn(t, real)
        self.assertEqual(real.count("-o"), 1)
        self.assertEqual(real.count("-MF"), 1)
        # appended tail
        self.assertEqual(real[real.index("-E"):real.index("-E") + 9],
                         ["-E", "-P", "-MMD", "-MF", "/side/heapam.d", "-MT", "src/backend/access/heap/heapam.o", "-o", "-"])
        self.assertIn("-fmacro-prefix-map=%s=SRC" % SRC, real)
        self.assertIn("-fmacro-prefix-map=%s=SRC" % ZH, real)
        self.assertIn("-ffile-prefix-map=%s=SRC" % ZH, real)

    def test_canonical_identical_across_roots(self):
        _, canon_base, _ = self.rewrite(SRC)
        _, canon_zh, _ = self.rewrite(ZH)
        self.assertEqual(canon_base, canon_zh)
        self.assertIn("-I{SRCROOT}/src/include", canon_zh)
        self.assertIn("-IBUILD/src/include", canon_zh)
        self.assertIn("{SIDE_D}", canon_zh)
        self.assertNotIn(ZH, json.dumps(canon_zh))
        self.assertNotIn(SRC, json.dumps(canon_zh))

    def test_no_blind_substitution(self):
        # a -D value that merely *contains* the srcdir string must not be rewritten
        argv = [CC, '-DPATH="%s/x"' % SRC, "-c", "-o", "a.o", SRC + "/a.c"]
        real, _, _ = ccreplay.rewrite_argv(argv, BLD, SRC, BLD, ZH, "a.o", "/side/a.d")
        self.assertIn('-DPATH="%s/x"' % SRC, real)
        self.assertIn(ZH + "/a.c", real)

    def test_generated_source_kept_in_builddir(self):
        argv = [CC, "-I.", "-c", "-o", "gram.o", "gram.c", "-MMD", "-MF", ".deps/gram.Po"]
        real, canon, info = ccreplay.rewrite_argv(argv, BLD + "/src/backend/parser", SRC, BLD, ZH, "k", "/side/g.d")
        self.assertEqual(info["source"], BLD + "/src/backend/parser/gram.c")
        self.assertEqual(info["source_kind"], "build")
        self.assertIn("BUILD/src/backend/parser/gram.c", canon)

    def test_rejections(self):
        base = [CC, "-c", "-o", "a.o", SRC + "/a.c"]
        for extra in (["-x", "c"], ["@rsp"], ["-E"], ["-M"], ["-fmacro-prefix-map=/x=/y"], ["-include"]):
            with self.assertRaises(ccreplay.ReplayError, msg=extra):
                ccreplay.rewrite_argv(base[:1] + extra + base[1:], BLD, SRC, BLD, ZH, "k", "/s.d")
        with self.assertRaises(ccreplay.ReplayError):
            ccreplay.rewrite_argv([CC, "-o", "a.o", SRC + "/a.c"], BLD, SRC, BLD, ZH, "k", "/s.d")  # no -c
        with self.assertRaises(ccreplay.ReplayError):
            ccreplay.rewrite_argv([CC, "-c", "-o", "a.o", SRC + "/a.c", SRC + "/b.c"], BLD, SRC, BLD, ZH, "k", "/s.d")

    def test_map_path_sibling_prefix(self):
        # /roots/base2 must not be treated as under /roots/base
        real, canon, kind = ccreplay._map_path("/roots/base2/x.h", SRC, BLD, ZH)
        self.assertEqual(kind, "sys")
        self.assertEqual(real, "/roots/base2/x.h")


class TokensAndDepfileTest(unittest.TestCase):
    def test_canonical_tokens(self):
        a, n = ccreplay.canonical_tokens("int  x ;\n\n\nint\ty;\n")
        self.assertEqual(a, "int x ; int y;")
        self.assertEqual(n, 5)
        self.assertEqual(ccreplay.canonical_tokens("   ")[1], 0)
        self.assertNotEqual(ccreplay.canonical_tokens("a b")[0], ccreplay.canonical_tokens("ab")[0])

    def test_normalize_depfile(self):
        text = ("key: %s/src/x.c ../../src/include/pg_config.h \\\n %s/src/include/c.h /usr/include/stdio.h\n"
                "../../src/include/pg_config.h:\n%s/src/include/c.h:\n") % (ZH, ZH, ZH)
        hdrs, sys_h = ccreplay.normalize_depfile(text, SRC, BLD, ZH, BLD + "/src/port")
        self.assertEqual(hdrs, ["BUILD/src/include/pg_config.h", "SRC/src/include/c.h", "SRC/src/x.c"])
        self.assertEqual(sys_h, ["/usr/include/stdio.h"])
        # the same depfile produced on the base side normalises identically
        hdrs_b, _ = ccreplay.normalize_depfile(text.replace(ZH, SRC), SRC, BLD, SRC, BLD + "/src/port")
        self.assertEqual(hdrs, hdrs_b)


class CacheKeyTest(unittest.TestCase):
    def material(self):
        return {k: "v-" + k for k in ccreplay.CACHE_KEY_FIELDS}

    def test_every_field_changes_key(self):
        base = ccreplay.compute_cache_key(self.material())
        for k in ccreplay.CACHE_KEY_FIELDS:
            m = self.material()
            m[k] = "changed"
            self.assertNotEqual(ccreplay.compute_cache_key(m), base, k)

    def test_missing_field_rejected(self):
        m = self.material()
        del m["tokens_sha256"]
        with self.assertRaises(ev.EvidenceError):
            ccreplay.compute_cache_key(m)

    def test_extra_fields_ignored_and_stable(self):
        m = self.material()
        k1 = ccreplay.compute_cache_key(m)
        m["unrelated"] = 1
        self.assertEqual(ccreplay.compute_cache_key(m), k1)


class CompareTokensTest(unittest.TestCase):
    def test_compare(self):
        d = tempfile.mkdtemp()
        try:
            a = os.path.join(d, "a.jsonl")
            b = os.path.join(d, "b.jsonl")
            rows = [{"variant_key": "x.o", "status": "ok", "token_sha256": "1", "headers_sha256": "h", "canonical_argv_sha256": "c"},
                    {"variant_key": "y.o", "status": "ok", "token_sha256": "2", "headers_sha256": "h", "canonical_argv_sha256": "c"},
                    {"variant_key": "z.o", "status": "ok", "token_sha256": "3", "headers_sha256": "h", "canonical_argv_sha256": "c"}]
            _write(a, "".join(json.dumps(r) + "\n" for r in rows))
            rows2 = [dict(rows[0]), dict(rows[1], token_sha256="9"), ]
            _write(b, "".join(json.dumps(r) + "\n" for r in rows2))
            c = ccreplay.compare_token_files(a, b)
            self.assertEqual((c["same"], c["different"], c["missing"]), (1, 1, 1))
            self.assertEqual(c["diffs"][0], {"variant_key": "y.o", "kind": "tokens"})
        finally:
            shutil.rmtree(d)


@unittest.skipUnless(os.access(CC, os.X_OK), "frozen compiler /usr/bin/gcc not available")
class RealReplayTest(unittest.TestCase):
    """Two roots that differ only in comments must hash identically; a code change must not."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.src = os.path.join(self.d, "base")
        self.zh = os.path.join(self.d, "zh")
        self.bad = os.path.join(self.d, "bad")
        self.bld = os.path.join(self.d, "build")
        self.cc_dir = os.path.join(self.d, "cc")
        self.side = os.path.join(self.d, "side")
        # like PostgreSQL's _srv.o variants, the second variant sees an extra
        # header that the base object's .Po (which it inherits) never recorded
        code = ('#include "postgres.h"\n#include "gen.h"\n/* %s */\n#ifdef FRONTEND\n#include "fe_only.h"\n'
                'int frontend = FE;\n#endif\n'
                'const char *f(void) { return __FILE__ " " STR(GEN); }\n')
        hdr = '#define STR_(x) #x\n#define STR(x) STR_(x)\n/* %s */\nextern const char *f(void);\n'
        for root, c, h in ((self.src, "english comment", "header comment"),
                           (self.zh, "中文注释，长度不同", "头文件注释"),
                           (self.bad, "english comment", "header comment")):
            _write(os.path.join(root, "src/a/f.c"), code % c)
            _write(os.path.join(root, "src/include/postgres.h"), hdr % h)
            _write(os.path.join(root, "src/include/fe_only.h"), "#define FE 1\n")
        # the "bad" root changes code, not comments
        _write(os.path.join(self.bad, "src/include/postgres.h"), (hdr % "header comment").replace("extern", "static"))
        _write(os.path.join(self.bld, "src/include/gen.h"), "#define GEN generated\n")
        os.makedirs(os.path.join(self.bld, "src/a/.deps"))
        self.cfg = {"cc": CC, "cxx": CXX, "srcdir": self.src, "builddir": self.bld, "evidence_out": self.side,
                    "env": {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8"}, "base_commit": "0" * 40,
                    "configure_args": []}
        argv = [CC, "-O2", "-I../include", "-I" + self.src + "/src/include", "-c", "-o", "f.o",
                self.src + "/src/a/f.c", "-MMD", "-MP", "-MF", ".deps/f.Po"]
        # the %_shlib.o: %.c %.o style rule: no depfile flags, the .Po is inherited
        argv2 = argv[:1] + ["-DFRONTEND"] + argv[1:argv.index("-MMD")]
        argv2[argv2.index("f.o")] = "f_shlib.o"
        log = os.path.join(self.d, "world.log")
        _write(log, "make: Entering directory '%s'\nmake[1]: Entering directory '%s/src/a'\n%s\n%s\n"
               "make[1]: Leaving directory '%s/src/a'\nmake: Leaving directory '%s'\n" % (
                   self.bld, self.bld, " ".join(argv), " ".join(argv2), self.bld, self.bld))
        # really compile so that .o and .Po exist
        env = dict(self.cfg["env"])
        for a in (argv, argv2):
            rc, out = ev.capture(a, cwd=os.path.join(self.bld, "src/a"), env=env)
            self.assertEqual(rc, 0, out)
        res = makelog.collect([("world_bin", log, "sha")], self.cfg)
        self.assertEqual(res["unverified"], [])
        self.assertTrue(res["closure"]["closed"], res["closure"])
        self.assertEqual(res["closure"]["po_inherited"], {"src/a/f_shlib.o": "src/a/f.o"})
        makelog.write_collect_outputs(res, self.cc_dir, os.path.join(self.d, "bulk"))

    def tearDown(self):
        shutil.rmtree(self.d)

    def test_replay_roots(self):
        base = ccreplay.replay_all(self.cfg, self.cc_dir, self.src, "base", jobs=1)
        self.assertEqual(base["n_failed"], 0, base)
        self.assertEqual(base["n_ok"], 2)
        zh = ccreplay.replay_all(self.cfg, self.cc_dir, self.zh, "zh", jobs=1)
        self.assertEqual(zh["n_failed"], 0, zh)
        cmp_ = ccreplay.compare_token_files(base["tokens_path"], zh["tokens_path"])
        self.assertEqual((cmp_["same"], cmp_["different"], cmp_["missing"]), (2, 0, 0), cmp_)
        toks = ccreplay.read_tokens(base["tokens_path"])
        t = toks["src/a/f.o"]
        self.assertNotIn("headers", t)
        hdrs = ccreplay.read_headers(base["headers_path"], toks)
        self.assertEqual(hdrs["src/a/f.o"], ["BUILD/src/include/gen.h", "SRC/src/include/postgres.h"])
        self.assertEqual(hdrs["src/a/f_shlib.o"],
                         ["BUILD/src/include/gen.h", "SRC/src/include/fe_only.h", "SRC/src/include/postgres.h"])
        # a tampered bulk file is rejected
        with open(base["headers_path"], "a") as f:
            f.write(json.dumps({"variant_key": "src/a/f.o", "headers": ["SRC/other.h"]}) + "\n")
        with self.assertRaises(ev.EvidenceError):
            ccreplay.read_headers(base["headers_path"], toks)
        self.assertEqual(t["source_kind"], "src")
        self.assertNotEqual(toks["src/a/f.o"]["token_sha256"], toks["src/a/f_shlib.o"]["token_sha256"])
        # the token cache differs between the two variants but is stable across roots
        self.assertEqual(base["token_cache_sha256"], zh["token_cache_sha256"])
        bad = ccreplay.replay_all(self.cfg, self.cc_dir, self.bad, "bad", jobs=1)
        cmp_bad = ccreplay.compare_token_files(base["tokens_path"], bad["tokens_path"])
        self.assertEqual(cmp_bad["different"], 2)
        self.assertEqual(cmp_bad["diffs"][0]["kind"], "tokens")

    def test_missing_root_rejected(self):
        with self.assertRaises(ev.EvidenceError):
            ccreplay.replay_all(self.cfg, self.cc_dir, os.path.join(self.d, "nope"), "nope", jobs=1)

    def _git(self, *args):
        env = {"PATH": "/usr/bin:/bin", "HOME": self.d, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@x",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@x"}
        rc, out = ev.capture(["git"] + list(args), cwd=self.src, env=env)
        self.assertEqual(rc, 0, out)
        return out

    def _full_evidence(self):
        """Everything freeze() reads, produced the way make_evidence.py produces it."""
        # the srcdir must be a git repo (coverage is measured against tracked .c/.h/.cpp)
        _write(os.path.join(self.src, "src/port/win32only.c"), "#ifdef WIN32\nint w;\n#endif\n")
        self._git("init", "-q")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "base")
        ev_dir = os.path.join(self.d, "evidence")
        os.makedirs(ev_dir)
        self.cfg["capture_targets"] = [{"name": "world_bin", "args": ["-j1", "-w", "world-bin"]}]
        log = os.path.join(self.d, "world.log")
        log_sha = ev.sha256_file(log)
        # re-collect with the real log sha (setUp used a placeholder)
        res = makelog.collect([("world_bin", log, log_sha)], self.cfg)
        makelog.write_collect_outputs(res, self.cc_dir, os.path.join(self.d, "bulk"))
        base = ccreplay.replay_all(self.cfg, self.cc_dir, self.src, "base", jobs=1,
                                   side_dir=os.path.join(self.side, "replay-base"))
        shutil.copy2(base["tokens_path"], os.path.join(self.cc_dir, "tokens.jsonl"))
        ev.write_json(os.path.join(self.cc_dir, "replay_base.json"), base)
        env = self.cfg["env"]
        ev.write_json(os.path.join(ev_dir, "environment.json"), {"versions": {
            k: {"first_line": ev.first_line(ev.capture([c, "--version"], env=env)[1])}
            for k, c in (("cc", CC), ("cxx", CXX), ("make", "make"))}})
        conf_log = os.path.join(self.side, "configure.log")
        _write(conf_log, "checking build system type... ok\n")
        # configure.json records the configure output and the config.log the
        # builddir got; the evidence_out copy must hash the same
        _write(os.path.join(self.bld, "config.log"), "This file contains any messages\n")
        shutil.copy2(os.path.join(self.bld, "config.log"), os.path.join(self.side, "config.log"))
        ev.write_json(os.path.join(ev_dir, "configure.json"), {
            "exit_code": 0, "log": conf_log, "log_sha256": ev.sha256_file(conf_log),
            "config_log_sha256": ev.sha256_file(os.path.join(self.bld, "config.log"))})
        ev.write_json(os.path.join(ev_dir, "make_world_bin.json"),
                      {"exit_code": 0, "log": log, "log_sha256": log_sha})
        cw_log = os.path.join(self.side, "make_check_world.log")
        _write(cw_log, "x\n")
        ev.write_json(os.path.join(ev_dir, "check_world.json"), {
            "exit_code": 2, "log": cw_log, "log_sha256": ev.sha256_file(cw_log),
            "argv": ["make", "-k", "check-world"],
            "results": {"summary": {k: 0 for k in (
                "sections", "regress_suites", "regress_tests", "regress_failed_tests", "regress_skipped_tests",
                "tap_suites", "tap_files", "tap_failed_files", "tap_skipped_files", "compiler_warnings",
                "make_errors", "regression_diffs_files")} | {"tap_results": {"PASS": 1, "FAIL": 1}},
                "sections": [
                    {"dir": "src/bin/x", "kind": "tap", "tests": [
                        {"name": "t/001.pl", "status": "ok"},
                        {"name": "t/003.pl", "status": "failed", "detail": "Failed 1/44 subtests"}]},
                    {"dir": "src/test/regress", "kind": "regress", "tests": [{"name": "boolean", "status": "ok"}]},
                ]}})
        return ev_dir

    def test_freeze_end_to_end(self):
        ev_dir = self._full_evidence()
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 0, report)
        b = ev.read_json(os.path.join(self.cc_dir, "baseline.json"))
        self.assertEqual(b["status"], "frozen")
        self.assertEqual(b["schema_version"], 2)
        self.assertTrue(b["coverage_closed"])
        self.assertEqual(b["n_variants"], 2)
        self.assertEqual(b["cache_key_sha256"], ccreplay.compute_cache_key(b["cache_key_material"]))
        self.assertEqual(sorted(b["cache_key_material"]), sorted(ccreplay.CACHE_KEY_FIELDS))
        # coverage: f.c, postgres.h and fe_only.h covered, win32only.c is the only candidate
        self.assertEqual(b["coverage"]["tracked_c_h_cpp"], 4)
        self.assertEqual(b["coverage"]["covered_tracked"], 3)
        self.assertEqual(b["coverage"]["uncovered_tracked"], 1)
        # f_shlib.o inherits f.o's .Po, which never saw fe_only.h: recorded, not a freeze problem;
        # a variant with its own .Po must stay within it
        ps = b["po_superset"]
        self.assertEqual(ps["checked"], 2)
        self.assertEqual(ps["inherited_checked"], 1)
        self.assertEqual(ps["own_po_violations"], [])
        self.assertEqual(ps["inherited_not_superset"],
                         [{"variant_key": "src/a/f_shlib.o", "po_inherited_from": "src/a/f.o",
                           "not_in_po": ["SRC/src/include/fe_only.h"]}])
        cands = ev.read_json(os.path.join(self.cc_dir, "platform_exceptions_candidates.json"))
        self.assertEqual([c["path"] for c in cands["candidates"]], ["src/port/win32only.c"])
        self.assertEqual(cands["candidates"][0]["status"], "candidate")
        self.assertEqual(cands["candidates"][0]["first_pp_conditional"], "#ifdef WIN32")
        self.assertIsNone(cands["candidates"][0]["reviewed_by"])
        # the baseline failure set is recorded verbatim
        self.assertEqual(b["check_world_exit_code"], 2)
        self.assertEqual(b["check_world_failures"],
                         [{"dir": "src/bin/x", "kind": "tap", "test": "t/003.pl", "status": "failed",
                           "detail": "Failed 1/44 subtests"}])
        # the report classifies the candidates but approves none
        self.assertEqual(report["platform_candidates"], {
            "n_candidates": 1, "by_hint": {"win32": 1}, "approved": 0,
            "note": "report only; approval is a separate reviewed step, never performed by freeze/verify"})
        # a log that changes after the capture makes the baseline a draft, not frozen
        with open(os.path.join(self.d, "world.log"), "a") as f:
            f.write("\n")
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertEqual(ev.read_json(os.path.join(self.cc_dir, "baseline.json"))["status"], "draft")
        self.assertTrue(any("changed on disk" in p for p in report["problems"]), report["problems"])

    def _freeze_ok(self):
        ev_dir = self._full_evidence()
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 0, report)
        return ev_dir

    def test_freeze_hashes_configure_and_check_world_logs(self):
        ev_dir = self._freeze_ok()
        # configure output log tampered
        with open(os.path.join(self.side, "configure.log"), "a") as f:
            f.write("tampered\n")
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertTrue(any(p.startswith("configure log changed on disk") for p in report["problems"]),
                        report["problems"])
        self.assertEqual(ev.read_json(os.path.join(self.cc_dir, "baseline.json"))["status"], "draft")
        # repair, then tamper with the builddir config.log
        _write(os.path.join(self.side, "configure.log"), "checking build system type... ok\n")
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 0, report)
        with open(os.path.join(self.bld, "config.log"), "a") as f:
            f.write("x\n")
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertTrue(any(p.startswith("config.log changed on disk") for p in report["problems"]), report)
        shutil.copy2(os.path.join(self.side, "config.log"), os.path.join(self.bld, "config.log"))
        # a missing builddir config.log is a problem even when the evidence_out copy is intact
        os.remove(os.path.join(self.bld, "config.log"))
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertTrue(any(p.startswith("config.log missing:") and self.bld in p for p in report["problems"]),
                        report["problems"])
        shutil.copy2(os.path.join(self.side, "config.log"), os.path.join(self.bld, "config.log"))
        # a configure record without the hash is a problem, not silently accepted
        conf = ev.read_json(os.path.join(ev_dir, "configure.json"))
        saved_hash = conf["config_log_sha256"]
        del conf["config_log_sha256"]
        ev.write_json(os.path.join(ev_dir, "configure.json"), conf)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertIn("configure record has no config_log_sha256", report["problems"])
        conf["config_log_sha256"] = saved_hash
        ev.write_json(os.path.join(ev_dir, "configure.json"), conf)
        # check-world log removed
        os.remove(os.path.join(self.side, "make_check_world.log"))
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertTrue(any(p.startswith("check-world log missing on disk") for p in report["problems"]), report)
        # a record without any log reference is a problem, not silently accepted
        _write(os.path.join(self.side, "make_check_world.log"), "x\n")
        cw = ev.read_json(os.path.join(ev_dir, "check_world.json"))
        del cw["log"]
        ev.write_json(os.path.join(ev_dir, "check_world.json"), cw)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir)
        self.assertEqual(rc, 1)
        self.assertIn("check-world record has no log/log_sha256", report["problems"])

    def test_freeze_verify_is_read_only_and_detects_drift(self):
        ev_dir = self._freeze_ok()
        base_path = os.path.join(self.cc_dir, "baseline.json")
        cand_path = os.path.join(self.cc_dir, "platform_exceptions_candidates.json")
        before = (_read(base_path), _read(cand_path))
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 0, report)
        self.assertEqual(report["mode"], "verify")
        self.assertEqual(report["mismatches"], [])
        self.assertEqual(report["cache_key_sha256"], report["committed_cache_key_sha256"])
        # nothing written
        self.assertEqual(before, (_read(base_path), _read(cand_path)))
        # a committed baseline that the evidence no longer reproduces: edit a derived field
        b = ev.read_json(base_path)
        b["coverage"]["covered_tracked"] += 1
        ev.write_json(base_path, b)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 1)
        self.assertIn("baseline.json field 'coverage' differs from the evidence on disk", report["mismatches"])
        # verify never rewrites the committed file, even when it is wrong
        self.assertEqual(ev.read_json(base_path)["coverage"]["covered_tracked"], b["coverage"]["covered_tracked"])
        # a tampered cache key is caught against its own material
        b["cache_key_sha256"] = "0" * 64
        ev.write_json(base_path, b)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 1)
        self.assertIn("committed cache_key_sha256 does not match its own cache_key_material", report["mismatches"])
        # restore; a candidate that someone marked approved by hand is rejected
        _write(base_path, before[0].decode("utf-8"))
        c = ev.read_json(cand_path)
        c["candidates"][0]["status"] = "approved"
        c["candidates"][0]["reviewed_by"] = "someone"
        ev.write_json(cand_path, c)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 1)
        self.assertTrue(any("carry approval fields" in m for m in report["mismatches"]), report["mismatches"])
        self.assertTrue(any(m.startswith("platform_exceptions_candidates.json differs") for m in
                            report["mismatches"]), report["mismatches"])
        # a draft baseline can never verify
        _write(cand_path, before[1].decode("utf-8"))
        b = ev.read_json(base_path)
        b["status"] = "draft"
        ev.write_json(base_path, b)
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 1)
        self.assertIn("committed baseline.json status is 'draft', not 'frozen'", report["mismatches"])
        # external evidence drift is a problem in verify mode too, and still read-only
        _write(base_path, before[0].decode("utf-8"))
        with open(os.path.join(self.side, "make_check_world.log"), "a") as f:
            f.write("late\n")
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 1)
        self.assertTrue(any(p.startswith("check-world log changed on disk") for p in report["problems"]), report)
        self.assertEqual(before[0], _read(base_path))

    def test_freeze_verify_without_committed_baseline_is_unverified(self):
        ev_dir = self._full_evidence()
        rc, report = ccreplay.freeze(self.cfg, ev_dir, self.cc_dir, verify=True)
        self.assertEqual(rc, 2)
        self.assertIn("missing cc_evidence/baseline.json (nothing to verify against)", report["problems"])
        self.assertFalse(os.path.exists(os.path.join(self.cc_dir, "baseline.json")))


class DirpermProbeTest(unittest.TestCase):
    def test_probe_reports_known_semantics_and_cleans_up(self):
        d = tempfile.mkdtemp()
        try:
            r = ev.dirperm_probe(d)
            self.assertEqual(r["parent"], d)
            self.assertIn(r.get("semantics"), ("posix", "unenforced", "readdir_fails"))
            if os.geteuid() == 0:
                self.assertEqual(r["semantics"], "unenforced")
            self.assertEqual(os.listdir(d), [], "probe must remove its scratch directory")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_probe_on_missing_parent_is_an_error_not_an_exception(self):
        r = ev.dirperm_probe("/nonexistent/zh-tools-probe")
        self.assertTrue(r["error"].startswith("mkdtemp:"))
        self.assertNotIn("semantics", r)


if __name__ == "__main__":
    unittest.main()
