"""Tests for zhtools.makelog: -j1 -w log parsing, cwd tracking, variants, shell rejection, closure."""
import json
import os
import shutil
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import makelog

CC = "/usr/bin/gcc"
CXX = "/usr/bin/g++"
COMPILERS = (CC, CXX)


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class ClassifyLineTest(unittest.TestCase):
    def test_simple_compile(self):
        kind, info = makelog.classify_line(
            CC + ' -Wall -I. -I../../src/include -DVAL_CC="\\"/usr/bin/gcc\\"" -c -o foo.o /src/a/foo.c -MMD -MP -MF .deps/foo.Po',
            COMPILERS)
        self.assertEqual(kind, "compile")
        self.assertEqual(info["source"], "/src/a/foo.c")
        self.assertEqual(info["object"], "foo.o")
        self.assertEqual(info["lang"], "c")
        self.assertIn('-DVAL_CC="/usr/bin/gcc"', info["tokens"])

    def test_cxx_compile(self):
        kind, info = makelog.classify_line(CXX + " -std=c++14 -c -o x.o /src/x.cpp", COMPILERS)
        self.assertEqual(kind, "compile")
        self.assertEqual(info["lang"], "c++")

    def test_link_ignored(self):
        kind, _ = makelog.classify_line(CC + " -o postgres a.o b.o -lm", COMPILERS)
        self.assertEqual(kind, "link")

    def test_other_lines(self):
        for line in ("rm -f foo.o", "'/usr/bin/perl' gen.pl", "touch stamp", "", "\tcontinued",
                     "make -C src all", "make[1]: Nothing to be done for 'all'."):
            self.assertEqual(makelog.classify_line(line, COMPILERS)[0], "other", line)

    def test_diagnostic_line_is_not_a_command(self):
        kind, _ = makelog.classify_line(CC + ": error: unrecognized command-line option", COMPILERS)
        self.assertEqual(kind, "other")

    def test_reject_shell_operators(self):
        bad = [
            CC + " -c -o a.o a.c && echo done",
            CC + " -c -o a.o a.c; true",
            CC + " -c -o a.o a.c | tee log",
            CC + " -c -o a.o a.c > out.txt",
            CC + " -c -o a.o a.c 2>/dev/null",
            CC + " -c -o a.o a.c < in",
            CC + " -c -o a.o a.c || exit 1",
            CC + " -c -o a.o `cat flags` a.c",
            CC + " -c -o a.o $(cat flags) a.c",
            CC + " @response.rsp -c -o a.o a.c",
            CC + " -x c -c -o a.o a.c",
            CC + " -c -c -o a.o a.c",
            CC + " -c -oa.o a.c",
            CC + " -c -o a.o a.c b.c",
            CC + " -c a.c",
            CC + " -E -o a.i a.c",
        ]
        for line in bad:
            with self.assertRaises(makelog.LogParseError, msg=line):
                makelog.classify_line(line, COMPILERS)

    def test_reject_wrapper_and_compound(self):
        for line in ("ccache " + CC + " -c -o a.o a.c",
                     "cd sub && " + CC + " -c -o a.o a.c",
                     "env FOO=1 " + CC + " -c -o a.o a.c",
                     "/usr/lib64/ccache/gcc -c -o a.o a.c && " + CC + " -c -o b.o b.c",
                     CC + " -c -o a.o a.c -B/usr/lib64/ccache -wrapper distcc"):
            with self.assertRaises(makelog.LogParseError, msg=line):
                makelog.classify_line(line, COMPILERS)

    def test_quoted_compiler_mention_inside_other_command_is_ok(self):
        # the compiler path only appears inside quotes of a non-compiler command
        kind, _ = makelog.classify_line('echo "built with /usr/bin/gcc" >> notes', COMPILERS)
        self.assertEqual(kind, "other")

    def test_unquoted_compiler_mention_inside_other_command_is_rejected(self):
        with self.assertRaises(makelog.LogParseError):
            makelog.classify_line("sh -c " + CC + " -c -o a.o a.c", COMPILERS)


class IterLogTest(unittest.TestCase):
    def test_continuation_lines_joined(self):
        d = tempfile.mkdtemp()
        try:
            p = os.path.join(d, "log")
            _write(p, "prereqdir=`cd x` && \\\n\tcd y && \\\n\tfor f in a; do echo $f; done\nnext\n")
            lines = list(makelog.iter_log(p))
            self.assertEqual(len(lines), 2)
            self.assertEqual(lines[0][0], 1)
            self.assertIn("for f in a", lines[0][1])
            self.assertEqual(lines[1], (4, "next"))
        finally:
            shutil.rmtree(d)

    def test_continuation_hides_nothing(self):
        d = tempfile.mkdtemp()
        try:
            p = os.path.join(d, "log")
            _write(p, "make: Entering directory '/b'\ncd sub && \\\n" + CC + " -c -o a.o a.c\nmake: Leaving directory '/b'\n")
            cmds, problems, _ = makelog.parse_log(p, COMPILERS, "/b")
            self.assertEqual(cmds, [])
            self.assertTrue(any("non-simple" in x for x in problems), problems)
        finally:
            shutil.rmtree(d)


class ParseLogTest(unittest.TestCase):
    LOG = (
        "make: Entering directory '/b'\n"
        "make -C src/port all\n"
        "make[1]: Entering directory '/b/src/port'\n"
        + CC + " -O2 -I../../src/include -c -o path.o /s/src/port/path.c -MMD -MP -MF .deps/path.Po\n"
        + CC + " -O2 -fPIC -I../../src/include -c -o path_shlib.o /s/src/port/path.c -MMD -MP -MF .deps/path_shlib.Po\n"
        + CC + " -O2 -DFRONTEND -I../../src/include -c -o path_srv.o /s/src/port/path.c -MMD -MP -MF .deps/path_srv.Po\n"
        "rm -f libpgport.a\n"
        "ar crs libpgport.a path.o\n"
        "make[1]: Leaving directory '/b/src/port'\n"
        "make -C src/backend all\n"
        "make[1]: Entering directory '/b/src/backend'\n"
        "make -C parser all\n"
        "make[2]: Entering directory '/b/src/backend/parser'\n"
        "bison -o gram.c /s/src/backend/parser/gram.y\n"
        + CC + " -O2 -I. -I../../../src/include -c -o gram.o gram.c -MMD -MP -MF .deps/gram.Po\n"
        "make[2]: Leaving directory '/b/src/backend/parser'\n"
        + CC + " -O2 -o postgres parser/gram.o -lm\n"
        "make[1]: Leaving directory '/b/src/backend'\n"
        "make: Leaving directory '/b'\n"
    )

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.log = os.path.join(self.d, "world.log")
        _write(self.log, self.LOG)

    def tearDown(self):
        shutil.rmtree(self.d)

    def test_cwd_tracking_and_variants(self):
        cmds, problems, stats = makelog.parse_log(self.log, COMPILERS, "/b")
        self.assertEqual(problems, [])
        self.assertEqual(stats["compile"], 4)
        self.assertEqual(stats["link"], 1)
        self.assertEqual([c["cwd"] for c in cmds], ["/b/src/port"] * 3 + ["/b/src/backend/parser"])
        self.assertEqual([c["object"] for c in cmds], ["path.o", "path_shlib.o", "path_srv.o", "gram.o"])
        # three variants of the same source are all kept
        self.assertEqual(len(set(c["source"] for c in cmds[:3])), 1)

    def test_leaving_mismatch_is_reported(self):
        _write(self.log, "make: Entering directory '/b'\nmake: Leaving directory '/other'\n")
        _, problems, _ = makelog.parse_log(self.log, COMPILERS, "/b")
        self.assertTrue(any("Leaving directory" in p for p in problems))

    def test_missing_w_is_reported(self):
        _write(self.log, CC + " -c -o a.o a.c\n")
        _, problems, _ = makelog.parse_log(self.log, COMPILERS, "/b")
        self.assertTrue(any("-w" in p for p in problems))

    def test_summarize(self):
        s = makelog.summarize_log(self.log, COMPILERS)
        self.assertEqual(s["compile_commands"], 4)
        self.assertEqual(s["link_commands"], 1)
        self.assertEqual(s["enter"], 4)
        self.assertEqual(s["problems"], 0)


class DepfileTest(unittest.TestCase):
    def test_parse_with_continuations_and_phony(self):
        text = ("path.o: /s/src/port/path.c ../../src/include/pg_config.h \\\n"
                "  /s/src/include/c.h \\\n"
                "  /usr/include/stdio.h\n"
                "../../src/include/pg_config.h:\n"
                "/s/src/include/c.h:\n")
        rules = makelog.parse_depfile(text)
        self.assertEqual(rules[0][0], "path.o")
        self.assertEqual(rules[0][1][0], "/s/src/port/path.c")
        self.assertEqual(len(rules), 3)
        tgt, first = makelog.po_first_source(rules)
        self.assertEqual((tgt, first), ("path.o", "/s/src/port/path.c"))
        self.assertEqual(makelog.po_headers(rules), ["../../src/include/pg_config.h", "/s/src/include/c.h",
                                                     "/usr/include/stdio.h"])

    def test_classify_path(self):
        self.assertEqual(makelog.classify_path("/b/src/include/pg_config.h", "/s", "/b"), ("build", "src/include/pg_config.h"))
        self.assertEqual(makelog.classify_path("/s/src/include/c.h", "/s", "/b"), ("src", "src/include/c.h"))
        self.assertEqual(makelog.classify_path("/usr/include/stdio.h", "/s", "/b"), ("sys", "/usr/include/stdio.h"))
        # a sibling directory with a common prefix is NOT under the root
        self.assertEqual(makelog.classify_path("/s2/x.h", "/s", "/b")[0], "sys")

    def test_classify_path_through_symlink(self):
        # Make echoes whichever spelling it was invoked with; the roots handed
        # to collect()/replay are realpath'd.  A path reached through a symlink
        # (macOS /var -> /private/var) must still classify against the other
        # spelling of the same directory.
        d = tempfile.mkdtemp()
        try:
            real_b = os.path.join(d, "realdir", "b")
            os.makedirs(os.path.join(real_b, "src"))
            link = os.path.join(d, "linkb")
            os.symlink(real_b, link)
            self.assertEqual(makelog.classify_path(os.path.join(link, "src", "x.o"), "/s", real_b),
                             ("build", "src/x.o"))
            self.assertEqual(makelog.classify_path(os.path.join(real_b, "src", "x.o"), "/s", link),
                             ("build", "src/x.o"))
        finally:
            shutil.rmtree(d)

    def test_canonical_path_resolves_existing_prefix(self):
        d = tempfile.mkdtemp()
        try:
            real = os.path.join(d, "real")
            os.makedirs(real)
            link = os.path.join(d, "link")
            os.symlink(real, link)
            # a non-existent suffix under a symlinked directory still resolves
            # to the canonical directory plus that suffix
            self.assertEqual(makelog.canonical_path(os.path.join(link, "missing.c")),
                             makelog.canonical_path(os.path.join(real, "missing.c")))
            self.assertTrue(makelog.canonical_path(os.path.join(link, "missing.c")).endswith("missing.c"))
        finally:
            shutil.rmtree(d)

    def test_norm_join(self):
        self.assertEqual(makelog.norm_join("/b/src/port", "../../src/include"), "/b/src/include")
        self.assertEqual(makelog.norm_join("/b/src/port", "/abs/x.c"), "/abs/x.c")


class ClosureTest(unittest.TestCase):
    """Fake builddir + log -> three-way closure."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.src = os.path.join(self.d, "src")
        self.bld = os.path.join(self.d, "bld")
        os.makedirs(self.src)
        os.makedirs(self.bld)
        _write(os.path.join(self.src, "src/port/path.c"), "int x;\n")
        _write(os.path.join(self.src, "src/include/c.h"), "\n")
        _write(os.path.join(self.bld, "src/include/pg_config.h"), "\n")
        self.cfg = {"cc": CC, "cxx": CXX, "srcdir": self.src, "builddir": self.bld}
        self.log = os.path.join(self.d, "world.log")
        _write(self.log, (
            "make: Entering directory '%(b)s'\n"
            "make[1]: Entering directory '%(b)s/src/port'\n"
            + CC + " -O2 -I../../src/include -I%(s)s/src/include -c -o path.o %(s)s/src/port/path.c -MMD -MP -MF .deps/path.Po\n"
            + CC + " -O2 -fPIC -I../../src/include -I%(s)s/src/include -c -o path_shlib.o %(s)s/src/port/path.c -MMD -MP -MF .deps/path_shlib.Po\n"
            "make[1]: Leaving directory '%(b)s/src/port'\n"
            "make: Leaving directory '%(b)s'\n") % {"b": self.bld, "s": self.src})
        for obj in ("path", "path_shlib"):
            _write(os.path.join(self.bld, "src/port/%s.o" % obj), "")
            _write(os.path.join(self.bld, "src/port/.deps/%s.Po" % obj),
                   "%s.o: %s/src/port/path.c ../../src/include/pg_config.h %s/src/include/c.h\n"
                   "../../src/include/pg_config.h:\n%s/src/include/c.h:\n" % (obj, self.src, self.src, self.src))

    def tearDown(self):
        shutil.rmtree(self.d)

    def collect(self):
        return makelog.collect([("world_bin", self.log, "sha")], self.cfg)

    def test_closed(self):
        r = self.collect()
        self.assertEqual(r["unverified"], [])
        self.assertTrue(r["closure"]["closed"])
        self.assertEqual(r["n_variants"], 2)
        self.assertEqual(r["n_sources"], 1)
        v = r["variants"]["src/port/path_shlib.o"]
        self.assertEqual(v["cwd"], "src/port")
        self.assertEqual(v["source_kind"], "src")
        self.assertEqual(v["source"], "src/port/path.c")
        self.assertEqual(v["po_headers"], ["BUILD/src/include/pg_config.h", "SRC/src/include/c.h"])
        # one flagset per distinct option set; both reconstruct exactly
        self.assertEqual(len(r["flagsets"]), 2)
        for c in r["compact"]:
            self.assertEqual(makelog.expand_argv(r["flagsets"], c), r["variants"][c["variant_key"]]["argv"])

    def test_object_without_argv(self):
        _write(os.path.join(self.bld, "src/port/extra.o"), "")
        _write(os.path.join(self.bld, "src/port/.deps/extra.Po"), "extra.o: x.c\n")
        r = self.collect()
        self.assertFalse(r["closure"]["closed"])
        self.assertEqual(r["closure"]["objects_without_argv"], ["src/port/extra.o"])

    def test_missing_po(self):
        # the _shlib variant may inherit the base .Po (Make rule %_shlib.o: %.c %.o) ...
        os.remove(os.path.join(self.bld, "src/port/.deps/path_shlib.Po"))
        r = self.collect()
        self.assertTrue(r["closure"]["closed"])
        self.assertEqual(r["closure"]["po_inherited"], {"src/port/path_shlib.o": "src/port/path.o"})
        # ... but when the base .Po is missing too, nothing can be inherited
        os.remove(os.path.join(self.bld, "src/port/.deps/path.Po"))
        r = self.collect()
        self.assertFalse(r["closure"]["closed"])
        self.assertEqual(r["closure"]["objects_without_po"], ["src/port/path.o", "src/port/path_shlib.o"])

    def test_missing_object(self):
        os.remove(os.path.join(self.bld, "src/port/path.o"))
        r = self.collect()
        self.assertFalse(r["closure"]["closed"])
        self.assertEqual(r["closure"]["argv_without_object"], ["src/port/path.o"])
        self.assertEqual(r["closure"]["po_without_object"], ["src/port/.deps/path.Po"])

    def test_po_first_prereq_mismatch(self):
        _write(os.path.join(self.bld, "src/port/.deps/path.Po"), "path.o: %s/src/port/other.c\n" % self.src)
        r = self.collect()
        self.assertTrue(any("first prerequisite" in u for u in r["unverified"]))

    def test_duplicate_object_different_argv(self):
        with open(self.log, "a") as f:
            f.write("make: Entering directory '%s'\nmake[1]: Entering directory '%s/src/port'\n" % (self.bld, self.bld))
            f.write(CC + " -O3 -c -o path.o %s/src/port/path.c\n" % self.src)
            f.write("make[1]: Leaving directory '%s/src/port'\nmake: Leaving directory '%s'\n" % (self.bld, self.bld))
        r = self.collect()
        self.assertTrue(any("compiled twice" in u for u in r["unverified"]))

    def test_duplicate_object_identical_is_fine(self):
        with open(self.log) as f:
            text = f.read()
        _write(self.log, text + text)
        r = self.collect()
        self.assertEqual(r["unverified"], [])
        self.assertEqual(r["n_duplicate_identical"], 2)

    def test_tmp_install_ignored(self):
        _write(os.path.join(self.bld, "tmp_install/usr/lib/foo.o"), "")
        _write(os.path.join(self.bld, "src/port/tmp_check/x.o"), "")
        r = self.collect()
        self.assertTrue(r["closure"]["closed"])

    def test_object_symlink_alias(self):
        os.makedirs(os.path.join(self.bld, "src/iso"))
        os.symlink("../port/path.o", os.path.join(self.bld, "src/iso/path.o"))
        r = self.collect()
        self.assertTrue(r["closure"]["closed"], r["closure"])
        self.assertEqual(r["closure"]["object_symlinks"], {"src/iso/path.o": "src/port/path.o"})
        os.remove(os.path.join(self.bld, "src/iso/path.o"))
        os.symlink("../port/nonexistent.o", os.path.join(self.bld, "src/iso/path.o"))
        r = self.collect()
        self.assertFalse(r["closure"]["closed"])
        self.assertTrue(any("not a known variant" in u for u in r["unverified"]))

    def test_inherited_po_only_for_shlib_srv_shape(self):
        # a _srv variant of the same source without its own .Po inherits the base .Po
        with open(self.log, "a") as f:
            f.write("make: Entering directory '%s'\nmake[1]: Entering directory '%s/src/port'\n" % (self.bld, self.bld))
            f.write(CC + " -O2 -I../../src/include -c -o path_srv.o %s/src/port/path.c\n" % self.src)
            f.write(CC + " -O2 -I../../src/include -c -o other_srv.o %s/src/port/path.c\n" % self.src)
            f.write("make[1]: Leaving directory '%s/src/port'\nmake: Leaving directory '%s'\n" % (self.bld, self.bld))
        _write(os.path.join(self.bld, "src/port/path_srv.o"), "")
        _write(os.path.join(self.bld, "src/port/other_srv.o"), "")
        r = self.collect()
        self.assertEqual(r["closure"]["po_inherited"], {"src/port/path_srv.o": "src/port/path.o"})
        self.assertEqual(r["variants"]["src/port/path_srv.o"]["po_file"], "src/port/.deps/path.Po")
        # other_srv.o has no base object "other.o": not closed
        self.assertFalse(r["closure"]["closed"])
        self.assertEqual(r["closure"]["objects_without_po"], ["src/port/other_srv.o"])

    def test_foreign_dtrace_object(self):
        with open(self.log, "a") as f:
            f.write("make: Entering directory '%s'\nmake[1]: Entering directory '%s/src/port'\n" % (self.bld, self.bld))
            f.write("/usr/bin/dtrace  -C -G -s %s/probes.d path.o -o probes.o\n" % self.src)
            f.write("make[1]: Leaving directory '%s/src/port'\nmake: Leaving directory '%s'\n" % (self.bld, self.bld))
        _write(os.path.join(self.bld, "src/port/probes.o"), "")
        r = self.collect()
        self.assertTrue(r["closure"]["closed"], r["closure"])
        self.assertEqual(list(r["closure"]["foreign_objects"]), ["src/port/probes.o"])
        self.assertEqual(r["closure"]["foreign_objects"]["src/port/probes.o"]["tool"], "dtrace")
        # a dtrace -h (header) invocation is not an object producer
        self.assertEqual(makelog.classify_line("/usr/bin/dtrace -C -h -s p.d -o probes.h.tmp", COMPILERS)[0], "other")

    def test_write_and_read_outputs(self):
        r = self.collect()
        out = os.path.join(self.d, "cc")
        bulk = os.path.join(self.d, "bulk")
        makelog.write_collect_outputs(r, out, bulk)
        self.assertTrue(os.path.exists(os.path.join(bulk, "po_headers.jsonl")))
        self.assertFalse(os.path.exists(os.path.join(out, "po_headers.jsonl")))
        flagsets, recs = makelog.read_commands(out)
        self.assertEqual(len(recs), 2)
        with open(os.path.join(out, "closure.json")) as f:
            closure = json.load(f)
        self.assertTrue(closure["closed"])
        self.assertEqual(closure["n_flagsets"], 2)
        self.assertEqual(closure["bulk"]["po_headers"]["path"], os.path.join(bulk, "po_headers.jsonl"))


if __name__ == "__main__":
    unittest.main()
