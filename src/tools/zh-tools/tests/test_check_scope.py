import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

from tests._path import TOOLS_DIR  # noqa: E402
from zhtools import manifest as manifest_mod, protection  # noqa: E402
import gen_baseline  # noqa: E402

BASE_SHA = "d61dd817be70749d14e982a369e97fdda9d5cba6"
PGNLS_SHA = "cbaeef3eb8be500984ab3da9f3e251634f346d1c"

_ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@x", GIT_COMMITTER_NAME="t",
            GIT_COMMITTER_EMAIL="t@x", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1",
            PYTHONDONTWRITEBYTECODE="1")


def git(cwd, *args):
    return subprocess.run(["git"] + list(args), cwd=cwd, env=_ENV, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode().strip()


def write(root, rel, data, mode=None):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    if mode is not None:
        os.chmod(p, mode)
    return p


class GlobTests(unittest.TestCase):
    def test_glob(self):
        rx = manifest_mod.glob_to_regex
        self.assertTrue(rx("src/tools/zh-tools/**").match("src/tools/zh-tools/a/b.py"))
        self.assertTrue(rx("src/tools/zh-tools/**").match("src/tools/zh-tools/x"))
        self.assertFalse(rx("src/tools/zh-tools/**").match("src/tools/zh-toolsx/x"))
        self.assertTrue(rx("src/backend/**/*.c").match("src/backend/a.c"))
        self.assertTrue(rx("src/backend/**/*.c").match("src/backend/x/y/a.c"))
        self.assertFalse(rx("src/backend/*.c").match("src/backend/x/a.c"))
        self.assertTrue(rx("README").match("README"))
        self.assertFalse(rx("README").match("doc/README"))
        self.assertTrue(rx("src/?.c").match("src/a.c"))
        self.assertFalse(rx("src/?.c").match("src/ab.c"))

    def test_manifest_validation(self):
        good = {"schema_version": 1, "batch_id": "x", "title": "t", "base_commit": BASE_SHA,
                "pgnls_commit": PGNLS_SHA,
                "allowed_paths": [{"pattern": "src/**", "kind": "c_comment"}],
                "files": [{"path": "src/a.c", "kind": "c_comment", "action": "modify",
                           "segments_total": 3, "translated": 2, "preserved": 1, "preserved_reasons": ["copyright"]}]}
        manifest_mod.Manifest(good)
        bad_cases = [
            dict(good, schema_version=2),
            dict(good, base_commit="abc"),
            dict(good, allowed_paths=[]),
            dict(good, allowed_paths=[{"pattern": "doc/**", "kind": "readme"}]),
            dict(good, allowed_paths=[{"pattern": "**", "kind": "readme"}]),
            dict(good, allowed_paths=[{"pattern": "src/notes.txt", "kind": "readme"}]),
            dict(good, files=[dict(good["files"][0], path="src/notes.txt")]),
            dict(good, allowed_paths=[{"pattern": "src/**", "kind": "nope"}]),
            dict(good, files=[dict(good["files"][0], action="delete")]),
            dict(good, files=[dict(good["files"][0], translated=5)]),
            dict(good, files=[dict(good["files"][0], preserved_reasons=[])]),
            dict(good, files=[dict(good["files"][0], path="other/a.c")]),
            dict(good, files=[good["files"][0], good["files"][0]]),
        ]
        for bc in bad_cases:
            with self.assertRaises(manifest_mod.ManifestError):
                manifest_mod.Manifest(bc)


class ProtectionUnit(unittest.TestCase):
    def test_perl_shebang(self):
        self.assertTrue(protection.is_perl_shebang(b"#!/usr/bin/perl\nprint 1;\n"))
        self.assertTrue(protection.is_perl_shebang(b"#!/usr/bin/env perl\n"))
        self.assertTrue(protection.is_perl_shebang(b"#! /usr/bin/perl -w\n"))
        self.assertFalse(protection.is_perl_shebang(b"#!/bin/sh\n# perl is mentioned here\n"))
        self.assertFalse(protection.is_perl_shebang(b"# perl\n"))
        self.assertFalse(protection.is_perl_shebang(b"#!/usr/bin/perlfoo\n"))
        self.assertTrue(protection.is_perl_name("a/b.pl"))
        self.assertTrue(protection.is_perl_name("x.t"))
        self.assertFalse(protection.is_perl_name("x.txt"))
        self.assertTrue(protection.is_txt_name("src/notes.txt"))
        self.assertTrue(protection.is_txt_name("meson_options.txt"))
        self.assertFalse(protection.is_txt_name("x.TXT"))
        self.assertFalse(protection.is_txt_name("notes.txt.bak"))
        self.assertFalse(protection.is_txt_name("src/tools/zh-tools/tests/fixtures/sample_old.c.txt"))

    def test_read_regular_file_does_not_follow_symlinks(self):
        with tempfile.TemporaryDirectory() as d:
            real = write(d, "real.pl", "#!/usr/bin/perl\n1;\n")
            link = os.path.join(d, "link")
            os.symlink("real.pl", link)
            data, st = protection.read_regular_file(real)
            self.assertEqual(data, b"#!/usr/bin/perl\n1;\n")
            self.assertEqual(protection.git_mode_of_stat(st), "100644")
            with self.assertRaises(protection.UnstableFile):
                protection.read_regular_file(link)
            with self.assertRaises(protection.UnstableFile):
                protection.sha256_fd_stable(link)
            # a shebang-perl file reached only through a symlink is not
            # classified by following the link
            self.assertFalse(protection.is_perl_file_on_disk(link))
            self.assertTrue(protection.is_perl_file_on_disk(real))
            # directories are rejected
            with self.assertRaises(protection.UnstableFile):
                protection.read_regular_file(d)

    def test_read_regular_file_identity_check(self):
        with tempfile.TemporaryDirectory() as d:
            a = write(d, "a", "A")
            b = write(d, "b", "B")
            st_b = os.lstat(b)
            # pretending the earlier lstat() saw another inode -> rejected
            with self.assertRaises(protection.UnstableFile):
                protection.read_regular_file(a, expect_st=st_b)
            data, _ = protection.read_regular_file(a, expect_st=os.lstat(a))
            self.assertEqual(data, b"A")
            digest, st = protection.sha256_fd_stable(a, expect_st=os.lstat(a))
            self.assertEqual(digest, hashlib.sha256(b"A").hexdigest())
            self.assertEqual(st.st_size, 1)

    def test_snapshot_file_fields_come_from_hashing_fd_stat(self):
        """perm/mode/size must be taken from the fstat of the hashed descriptor,
        never from the pre-open lstat (TOCTOU)."""
        with tempfile.TemporaryDirectory() as d:
            write(d, "doc/f.txt", "abc", mode=0o644)
            real = protection.sha256_fd_stable
            calls = []

            class FakeSt(object):
                st_mode = 0o100755
                st_size = 99
                st_dev = 1
                st_ino = 2

            def fake(path, expect_st=None):
                calls.append((path, expect_st))
                return "f" * 64, FakeSt()

            protection.sha256_fd_stable = fake
            try:
                snap = protection.snapshot_tree(d, "doc")
            finally:
                protection.sha256_fd_stable = real
            f = [e for e in snap if e["path"] == "doc/f.txt"][0]
            self.assertEqual((f["mode"], f["perm"], f["size"], f["sha256"]), ("100755", "0755", 99, "f" * 64))
            self.assertEqual(len(calls), 1)
            self.assertIsNotNone(calls[0][1])  # lstat passed for identity check
            # directories and symlinks keep lstat-derived fields
            dd = [e for e in snap if e["path"] == "doc"][0]
            self.assertEqual(dd["type"], "dir")
            self.assertNotIn("size", dd)

    def test_snapshot_file_unstable_raises(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "doc/f.txt", "abc")
            real = protection.sha256_fd_stable

            def fake(path, expect_st=None):
                raise protection.UnstableFile("swapped")

            protection.sha256_fd_stable = fake
            try:
                with self.assertRaises(protection.UnstableFile):
                    protection.snapshot_tree(d, "doc")
            finally:
                protection.sha256_fd_stable = real

    def test_snapshot_and_compare(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "doc/a.txt", "a")
            write(d, "doc/sub/b.txt", "bb", mode=0o755)
            os.symlink("a.txt", os.path.join(d, "doc/link"))
            snap = protection.snapshot_tree(d, "doc")
            self.assertEqual([e["path"] for e in snap], ["doc", "doc/a.txt", "doc/link", "doc/sub", "doc/sub/b.txt"])
            self.assertEqual(snap[2]["type"], "symlink")
            self.assertEqual(snap[2]["target"], "a.txt")
            self.assertEqual(snap[4]["mode"], "100755")
            self.assertEqual(protection.compare_snapshots(snap, protection.snapshot_tree(d, "doc")), [])
            write(d, "doc/a.txt", "A")
            write(d, "doc/new", "n")
            os.chmod(os.path.join(d, "doc/sub/b.txt"), 0o644)
            diffs = protection.compare_snapshots(snap, protection.snapshot_tree(d, "doc"))
            self.assertTrue(any(x.startswith("extra: doc/new") for x in diffs))
            self.assertTrue(any(x.startswith("changed: doc/a.txt sha256") for x in diffs))
            self.assertTrue(any(x.startswith("changed: doc/sub/b.txt mode") for x in diffs))


class ScopeEndToEnd(unittest.TestCase):
    """Build a miniature repository that mimics the protected layout."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="zhtools-scope-")
        self.addCleanup(self.tmp.cleanup)
        self.base = os.path.join(self.tmp.name, "base")
        os.makedirs(self.base)
        git(self.base, "init", "-q", "-b", "main")
        write(self.base, ".gitignore", "*.o\nbuild/\n")
        write(self.base, "doc/src/sgml/ref.sgml", "<para>doc</para>\n")
        write(self.base, "doc/src/sgml/mk.pl", "#!/usr/bin/perl\n", mode=0o755)
        write(self.base, "src/a.c", "int a; /* x */\n")
        write(self.base, "src/b.h", "/* h */\n")
        write(self.base, "src/t/001.t", "use Test;\n")
        write(self.base, "src/tools/pgindent/pgindent", "#!/usr/bin/perl\nprint 1;\n", mode=0o755)
        write(self.base, "src/tools/Mod.pm", "package Mod;\n")
        write(self.base, "src/bin/script", "#!/bin/sh\necho hi\n", mode=0o755)
        write(self.base, "README", "readme\n")
        write(self.base, "src/data.txt", "machine\n")
        git(self.base, "add", "-A")
        git(self.base, "commit", "-q", "-m", "base")
        self.base_sha = git(self.base, "rev-parse", "HEAD")
        # work clone (acts as the translation worktree)
        self.work = os.path.join(self.tmp.name, "work")
        git(self.tmp.name, "clone", "-q", self.base, self.work)
        # baseline from the base repo
        self.bl = os.path.join(self.tmp.name, "baseline")
        files = gen_baseline.build(self.base, None)
        os.makedirs(self.bl)
        for name, obj in files.items():
            with open(os.path.join(self.bl, name), "w", encoding="utf-8") as f:
                json.dump(obj, f)
        self.assertEqual(files["baseline.json"]["perl_protected_files"], 4)
        self.assertEqual({e["path"] for e in files["perl_protected.json"]["entries"]},
                         {"doc/src/sgml/mk.pl", "src/t/001.t", "src/tools/pgindent/pgindent", "src/tools/Mod.pm"})

    def manifest(self, allowed, files=None, require=True):
        m = {"schema_version": 1, "batch_id": "t", "title": "t", "base_commit": self.base_sha,
             "pgnls_commit": PGNLS_SHA, "require_file_entries": require,
             "allowed_paths": [{"pattern": a, "kind": "c_comment"} for a in allowed],
             "files": files or []}
        p = os.path.join(self.tmp.name, "m.json")
        with open(p, "w") as f:
            json.dump(m, f)
        return p

    def run_scope(self, *argv):
        rep = os.path.join(self.tmp.name, "scope.json")
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "check_scope.py"),
                            "--baseline-dir", self.bl, "--json", rep] + list(argv),
                           env=_ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = p.stdout.decode() + p.stderr.decode()
        data = None
        if os.path.exists(rep):
            with open(rep) as f:
                data = json.load(f)
        return p.returncode, out, data

    def fe(self, path, action="modify"):
        return {"path": path, "kind": "c_comment", "action": action, "segments_total": 1,
                "translated": 1, "preserved": 0, "preserved_reasons": []}

    def rules(self, data):
        return {v["rule"] for v in data["violations"]}

    def test_clean_worktree_passes(self):
        rc, out, data = self.run_scope("--worktree", self.work, "--manifest", self.manifest(["src/**"]))
        self.assertEqual(rc, 0, out)
        self.assertEqual(data["n_changes"], 0)

    def test_in_scope_change_passes(self):
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        m = self.manifest(["src/**"], [self.fe("src/a.c")])
        rc, out, _ = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 0, out)

    def test_missing_file_entry_and_wrong_action(self):
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", self.manifest(["src/**"]))
        self.assertEqual(rc, 1)
        self.assertIn("S2_NO_FILE_ENTRY", self.rules(data))
        m = self.manifest(["src/**"], [self.fe("src/a.c", action="add")])
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S2_NO_FILE_ENTRY", self.rules(data))
        # without require_file_entries the pattern alone suffices
        rc, out, _ = self.run_scope("--worktree", self.work, "--manifest", self.manifest(["src/**"], require=False))
        self.assertEqual(rc, 0, out)

    def test_outside_manifest_tracked_untracked_ignored(self):
        write(self.work, "README", "改\n")                      # tracked, outside
        write(self.work, "src/new.txt", "u\n")                 # untracked, inside pattern but no entry
        write(self.work, "other/x.o", b"\0")                   # ignored, outside
        write(self.work, "build/out.log", "ignored dir\n")     # ignored, outside
        m = self.manifest(["src/**"], [self.fe("src/a.c")])
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        paths = {(v["rule"], v["path"]) for v in data["violations"]}
        self.assertIn(("S2_OUTSIDE_MANIFEST", "README"), paths)
        self.assertIn(("S2_OUTSIDE_MANIFEST", "other/x.o"), paths)
        self.assertIn(("S2_OUTSIDE_MANIFEST", "build/out.log"), paths)
        self.assertIn(("S2_NO_FILE_ENTRY", "src/new.txt"), paths)
        self.assertIn(("S5_TXT_NEW", "src/new.txt"), paths)
        origins = {c["path"]: c["origin"] for c in data["changes"]}
        self.assertEqual(origins["other/x.o"], "ignored")
        self.assertEqual(origins["src/new.txt"], "untracked")
        self.assertEqual(origins["README"], "tracked")

    def test_doc_changes_detected_in_all_forms(self):
        m = self.manifest(["src/**"])
        # tracked modification
        write(self.work, "doc/src/sgml/ref.sgml", "<para>译</para>\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        r = self.rules(data)
        self.assertIn("S3_DOC_SNAPSHOT", r)
        self.assertIn("S3_DOC_DIFF", r)
        self.assertIn("S2_OUTSIDE_MANIFEST", r)
        git(self.work, "checkout", "--", "doc")
        # untracked file under doc
        write(self.work, "doc/notes.txt", "n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S3_DOC_SNAPSHOT", self.rules(data))
        os.unlink(os.path.join(self.work, "doc/notes.txt"))
        # ignored file under doc
        write(self.work, "doc/x.o", b"o")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S3_DOC_SNAPSHOT", self.rules(data))
        self.assertIn("S2_OUTSIDE_MANIFEST", self.rules(data))
        os.unlink(os.path.join(self.work, "doc/x.o"))
        # permission-only change
        os.chmod(os.path.join(self.work, "doc/src/sgml/ref.sgml"), 0o600)
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S3_DOC_SNAPSHOT", self.rules(data))
        os.chmod(os.path.join(self.work, "doc/src/sgml/ref.sgml"), 0o644)
        # back to clean
        rc, out, _ = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 0, out)
        # committed doc change -> HEAD:doc tree differs
        write(self.work, "doc/src/sgml/ref.sgml", "<para>译</para>\n")
        git(self.work, "commit", "-q", "-am", "doc")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S3_DOC_TREE", self.rules(data))
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertIn("S3_DOC_TREE", self.rules(data))
        self.assertIn("S3_DOC_DIFF", self.rules(data))

    def test_perl_protection(self):
        m = self.manifest(["src/**"], [self.fe("src/tools/pgindent/pgindent"), self.fe("src/t/001.t"),
                                       self.fe("src/tools/Mod.pm"), self.fe("src/bin/script"),
                                       self.fe("src/new.pl", "add"), self.fe("src/newperl", "add")])
        # content change in shebang-detected file (listed in manifest, still forbidden)
        write(self.work, "src/tools/pgindent/pgindent", "#!/usr/bin/perl\nprint 2;\n", mode=0o755)
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertIn(("S4_PERL_CHANGED", "src/tools/pgindent/pgindent"),
                      {(v["rule"], v["path"]) for v in data["violations"]})
        git(self.work, "checkout", "--", "src")
        # mode-only change
        os.chmod(os.path.join(self.work, "src/t/001.t"), 0o755)
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S4_PERL_CHANGED", self.rules(data))
        os.chmod(os.path.join(self.work, "src/t/001.t"), 0o644)
        # deletion
        os.unlink(os.path.join(self.work, "src/tools/Mod.pm"))
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn("S4_PERL_MISSING", self.rules(data))
        git(self.work, "checkout", "--", "src")
        # protected file replaced by a symlink to identical content: must
        # be reported without following the link
        os.unlink(os.path.join(self.work, "src/tools/Mod.pm"))
        write(self.work, "src/tools/Mod.pm.real", "package Mod;\n")
        os.symlink("Mod.pm.real", os.path.join(self.work, "src/tools/Mod.pm"))
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn(("S4_PERL_CHANGED", "src/tools/Mod.pm"), {(v["rule"], v["path"]) for v in data["violations"]})
        os.unlink(os.path.join(self.work, "src/tools/Mod.pm"))
        os.unlink(os.path.join(self.work, "src/tools/Mod.pm.real"))
        git(self.work, "checkout", "--", "src")
        # new perl files by extension and by shebang (untracked)
        write(self.work, "src/new.pl", "1;\n")
        write(self.work, "src/newperl", "#!/usr/bin/env perl\n1;\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        paths = {(v["rule"], v["path"]) for v in data["violations"]}
        self.assertIn(("S4_PERL_NEW", "src/new.pl"), paths)
        self.assertIn(("S4_PERL_NEW", "src/newperl"), paths)
        os.unlink(os.path.join(self.work, "src/new.pl"))
        os.unlink(os.path.join(self.work, "src/newperl"))
        # a shell script with shebang is NOT perl -> allowed to change
        write(self.work, "src/bin/script", "#!/bin/sh\necho 译\n", mode=0o755)
        rc, out, _ = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 0, out)
        # committed perl change in --ref mode
        write(self.work, "src/tools/Mod.pm", "package Mod; # 译\n")
        git(self.work, "commit", "-q", "-am", "perl")
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertIn("S4_PERL_CHANGED", self.rules(data))

    def test_txt_is_hard_exclude(self):
        m = self.manifest(["src/**"], [self.fe("src/a.c")])
        # content change of a baseline *.txt
        write(self.work, "src/data.txt", "译\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertIn(("S5_TXT_CHANGED", "src/data.txt"), {(v["rule"], v["path"]) for v in data["violations"]})
        git(self.work, "checkout", "--", "src/data.txt")
        # deletion
        os.unlink(os.path.join(self.work, "src/data.txt"))
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn(("S5_TXT_DELETED", "src/data.txt"), {(v["rule"], v["path"]) for v in data["violations"]})
        git(self.work, "checkout", "--", "src/data.txt")
        # new untracked *.txt, even listed in the manifest, is forbidden
        write(self.work, "src/extra.txt", "n\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn(("S5_TXT_NEW", "src/extra.txt"), {(v["rule"], v["path"]) for v in data["violations"]})
        os.unlink(os.path.join(self.work, "src/extra.txt"))
        # ignored *.txt
        write(self.work, "build/out.txt", "n\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m)
        self.assertIn(("S5_TXT_NEW", "build/out.txt"), {(v["rule"], v["path"]) for v in data["violations"]})
        os.unlink(os.path.join(self.work, "build/out.txt"))
        # committed change in --ref mode
        write(self.work, "src/data.txt", "译\n")
        git(self.work, "commit", "-q", "-am", "txt")
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertIn("S5_TXT_CHANGED", self.rules(data))

    def test_base_mismatch_is_fatal(self):
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        git(self.work, "commit", "-q", "-am", "x")
        m = self.manifest(["src/**"], [self.fe("src/a.c")])
        rc, _, data = self.run_scope("--worktree", self.work, "--base", "HEAD", "--manifest", m)
        self.assertEqual(rc, 1)
        self.assertEqual(self.rules(data), {"S1_BASE_MISMATCH"})
        # manifest with a different base commit
        with open(m) as f:
            mm = json.load(f)
        mm["base_commit"] = BASE_SHA
        with open(m, "w") as f:
            json.dump(mm, f)
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m)
        self.assertIn("S1_BASE_MISMATCH", self.rules(data))

    def test_incremental_mode(self):
        # batch 1 committed on top of base
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        git(self.work, "commit", "-q", "-am", "batch1")
        parent = git(self.work, "rev-parse", "HEAD")
        # batch 2 in the working tree
        write(self.work, "src/b.h", "/* 头 */\n")
        m2 = os.path.join(self.tmp.name, "m2.json")
        with open(m2, "w") as f:
            json.dump({"schema_version": 1, "batch_id": "b2", "title": "t", "base_commit": self.base_sha,
                       "parent_commit": parent, "pgnls_commit": PGNLS_SHA,
                       "allowed_paths": [{"pattern": "src/**", "kind": "c_comment"}],
                       "files": [self.fe("src/b.h")]}, f)
        # cumulative mode: a.c is changed since base but not in m2 -> fail
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m2)
        self.assertEqual(rc, 1)
        self.assertIn(("S2_NO_FILE_ENTRY", "src/a.c"), {(v["rule"], v["path"]) for v in data["violations"]})
        # incremental mode: only b.h is in the diff -> pass
        rc, out, data = self.run_scope("--worktree", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 0, out)
        self.assertEqual(data["incremental"], True)
        self.assertEqual([c["path"] for c in data["changes"]], ["src/b.h"])
        self.assertEqual(data["base_resolved"], parent)
        # incremental on a ref
        git(self.work, "commit", "-q", "-am", "batch2")
        rc, out, _ = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 0, out)
        # doc/, Perl and *.txt protection stay absolute in incremental mode
        write(self.work, "doc/src/sgml/ref.sgml", "<para>译</para>\n")
        write(self.work, "src/tools/Mod.pm", "package Mod; # 译\n")
        write(self.work, "src/data.txt", "译\n")
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 1)
        self.assertIn("S3_DOC_SNAPSHOT", self.rules(data))
        self.assertIn("S4_PERL_CHANGED", self.rules(data))
        self.assertIn("S5_TXT_CHANGED", self.rules(data))
        git(self.work, "checkout", "--", ".")
        # parent_commit missing -> S1
        with open(m2) as f:
            mm = json.load(f)
        del mm["parent_commit"]
        with open(m2, "w") as f:
            json.dump(mm, f)
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(self.rules(data), {"S1_BASE_MISMATCH"})
        # parent_commit that does not descend from the frozen base -> S1
        other = os.path.join(self.tmp.name, "other")
        os.makedirs(other)
        git(other, "init", "-q", "-b", "main")
        write(other, "x", "x")
        git(other, "add", "-A")
        git(other, "commit", "-q", "-m", "unrelated")
        unrelated = git(other, "rev-parse", "HEAD")
        git(self.work, "fetch", "-q", other, "main")
        mm["parent_commit"] = unrelated
        with open(m2, "w") as f:
            json.dump(mm, f)
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(self.rules(data), {"S1_BASE_MISMATCH"})

    def test_incremental_detects_perl_added_by_earlier_commit(self):
        """Review finding: Perl added relative to REL_16_11 by a previous
        batch and untouched by this one must still fail, in both modes."""
        # batch 1 (already committed): adds a .pl, a shebang-perl file and
        # turns a shell script into perl
        write(self.work, "src/new.pl", "1;\n")
        write(self.work, "src/newperl", "#!/usr/bin/env perl\n1;\n")
        write(self.work, "src/bin/script", "#!/usr/bin/perl\nprint 1;\n", mode=0o755)
        git(self.work, "add", "-A")
        git(self.work, "commit", "-q", "-m", "batch1 sneaks perl in")
        parent = git(self.work, "rev-parse", "HEAD")
        # batch 2 only touches src/a.c
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        m2 = os.path.join(self.tmp.name, "m2.json")
        with open(m2, "w") as f:
            json.dump({"schema_version": 1, "batch_id": "b2", "title": "t", "base_commit": self.base_sha,
                       "parent_commit": parent, "pgnls_commit": PGNLS_SHA,
                       "allowed_paths": [{"pattern": "src/**", "kind": "c_comment"}],
                       "files": [self.fe("src/a.c")]}, f)
        expected = {("S4_PERL_NEW", "src/new.pl"), ("S4_PERL_NEW", "src/newperl"),
                    ("S4_PERL_NEW", "src/bin/script")}
        # incremental worktree
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 1)
        got = {(v["rule"], v["path"]) for v in data["violations"]}
        self.assertTrue(expected <= got, got)
        # the incremental manifest check itself is satisfied (only S4 fires)
        self.assertEqual(self.rules(data), {"S4_PERL_NEW"})
        self.assertEqual([c["path"] for c in data["changes"]], ["src/a.c"])
        # incremental ref
        git(self.work, "commit", "-q", "-am", "batch2")
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 1)
        got = {(v["rule"], v["path"]) for v in data["violations"]}
        self.assertTrue(expected <= got, got)
        self.assertEqual(self.rules(data), {"S4_PERL_NEW"})
        # cumulative ref also fails (and additionally flags the manifest gap)
        rc, _, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m2)
        self.assertEqual(rc, 1)
        got = {(v["rule"], v["path"]) for v in data["violations"]}
        self.assertTrue(expected <= got, got)
        # removing the offending files again makes incremental pass
        git(self.work, "rm", "-q", "src/new.pl", "src/newperl")
        write(self.work, "src/bin/script", "#!/bin/sh\necho hi\n", mode=0o755)
        git(self.work, "commit", "-q", "-am", "undo perl")
        m3 = os.path.join(self.tmp.name, "m3.json")
        with open(m2) as f:
            mm = json.load(f)
        mm["parent_commit"] = git(self.work, "rev-parse", "HEAD~1")
        mm["files"] = [self.fe("src/bin/script")]
        with open(m3, "w") as f:
            json.dump(mm, f)
        rc, out, data = self.run_scope("--ref", "HEAD", "--repo", self.work, "--manifest", m3, "--incremental")
        # deletions of new.pl/newperl are 'D' in the incremental diff and
        # have no manifest entry -> S2, but no S4_PERL_NEW remains
        self.assertNotIn("S4_PERL_NEW", self.rules(data))

    def test_incremental_detects_untracked_and_ignored_perl(self):
        write(self.work, "src/a.c", "int a; /* 甲 */\n")
        git(self.work, "commit", "-q", "-am", "batch1")
        parent = git(self.work, "rev-parse", "HEAD")
        write(self.work, "src/b.h", "/* 头 */\n")
        write(self.work, "tools/helper.pl", "1;\n")                 # untracked, outside pattern
        write(self.work, "build/gen.pm", "package gen;\n")          # ignored (build/)
        write(self.work, "build/run", "#!/usr/bin/perl\n")          # ignored, shebang
        m2 = os.path.join(self.tmp.name, "m2.json")
        with open(m2, "w") as f:
            json.dump({"schema_version": 1, "batch_id": "b2", "title": "t", "base_commit": self.base_sha,
                       "parent_commit": parent, "pgnls_commit": PGNLS_SHA,
                       "allowed_paths": [{"pattern": "src/**", "kind": "c_comment"}],
                       "files": [self.fe("src/b.h")]}, f)
        rc, _, data = self.run_scope("--worktree", self.work, "--manifest", m2, "--incremental")
        self.assertEqual(rc, 1)
        got = {(v["rule"], v["path"]) for v in data["violations"]}
        for p in ("tools/helper.pl", "build/gen.pm", "build/run"):
            self.assertIn(("S4_PERL_NEW", p), got)

    def test_bad_inputs_exit_2(self):
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "check_scope.py"), "--worktree", self.work,
                            "--manifest", "/nonexistent.json", "--baseline-dir", self.bl], env=_ENV,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 2)
        self.assertIn("UNVERIFIED", p.stderr.decode())

    def test_gen_baseline_verify_roundtrip(self):
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "gen_baseline.py"), "--base-worktree",
                            self.base, "--out", self.bl, "--verify"], env=_ENV,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 0, p.stdout.decode() + p.stderr.decode())
        # tamper with the committed snapshot -> verify fails
        sp = os.path.join(self.bl, "doc_snapshot.json")
        with open(sp) as f:
            d = json.load(f)
        d["entries"][-1]["sha256"] = "0" * 64
        with open(sp, "w") as f:
            json.dump(d, f)
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "gen_baseline.py"), "--base-worktree",
                            self.base, "--out", self.bl, "--verify"], env=_ENV,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(p.returncode, 1)
        self.assertIn("DIFFERS", p.stdout.decode())


if __name__ == "__main__":
    unittest.main()
