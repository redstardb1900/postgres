"""
Baseline protection data: recursive directory snapshots and the frozen
Perl protected set.

Snapshot entry fields (all compared, nothing else recorded):
  path     relative POSIX path below the snapshot root
  type     "file" | "dir" | "symlink" | "other"
  mode     git-style mode: 100644 / 100755 / 120000 / 040000
  perm     on-disk permission bits (octal string, st_mode & 0o7777)
  size     byte size (files only)
  sha256   hex digest of content (files only)
  target   symlink target (symlinks only)

Deliberately NOT recorded: mtime/ctime/atime, inode, uid/gid, device.
"""
import hashlib
import os
import re
import stat
import subprocess
import sys

sys.dont_write_bytecode = True

from . import gitutil  # noqa: E402

SNAPSHOT_SCHEMA_VERSION = 1
PERL_EXTS = (".pl", ".pm", ".t")
_PERL_SHEBANG = re.compile(rb"\A#![^\n]*\bperl\b")


def is_perl_name(path):
    return path.endswith(PERL_EXTS)


def is_perl_shebang(head):
    """True if the first line is a shebang that invokes perl."""
    if not head.startswith(b"#!"):
        return False
    first = head.split(b"\n", 1)[0]
    return _PERL_SHEBANG.match(first) is not None


class UnstableFile(RuntimeError):
    """The file changed identity or content while it was being read."""


_O_NOFOLLOW = getattr(os, "O_NOFOLLOW", 0)
_O_CLOEXEC = getattr(os, "O_CLOEXEC", 0)
_O_BINARY = getattr(os, "O_BINARY", 0)


def _same_identity(a, b):
    return (a.st_dev, a.st_ino, stat.S_IFMT(a.st_mode)) == (b.st_dev, b.st_ino, stat.S_IFMT(b.st_mode))


def open_regular_nofollow(path, expect_st=None):
    """Open *path* as a regular file without following a final symlink.

    Returns (fd, st) where st is the fstat of the opened descriptor.  Raises
    UnstableFile when the path is not a regular file, when O_NOFOLLOW is
    unavailable and the path is a symlink, or when the opened file is not
    the one *expect_st* (an earlier lstat) described.
    """
    flags = os.O_RDONLY | _O_NOFOLLOW | _O_CLOEXEC | _O_BINARY
    try:
        fd = os.open(path, flags)
    except OSError as e:
        raise UnstableFile("%s: cannot open: %s" % (path, e))
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise UnstableFile("%s: not a regular file after open" % path)
        if not _O_NOFOLLOW and os.path.islink(path):
            raise UnstableFile("%s: symlink (O_NOFOLLOW unavailable)" % path)
        if expect_st is not None and not _same_identity(expect_st, st):
            raise UnstableFile("%s: file identity changed between lstat and open" % path)
    except BaseException:
        os.close(fd)
        raise
    return fd, st


def read_regular_file(path, expect_st=None):
    """Read a regular file via a non-following descriptor.

    Metadata (dev/ino/type/size/mode/mtime) is captured with fstat before
    and after reading; any difference raises UnstableFile.  Returns
    (data, st_before).
    """
    fd, st1 = open_regular_nofollow(path, expect_st)
    try:
        chunks = []
        while True:
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                break
            chunks.append(chunk)
        st2 = os.fstat(fd)
    finally:
        os.close(fd)
    _check_stable(path, st1, st2)
    data = b"".join(chunks)
    if len(data) != st1.st_size:
        raise UnstableFile("%s: read %d bytes, size was %d" % (path, len(data), st1.st_size))
    return data, st1


def _check_stable(path, st1, st2):
    if not _same_identity(st1, st2) or (st1.st_size, st1.st_mode, st1.st_mtime_ns) != \
            (st2.st_size, st2.st_mode, st2.st_mtime_ns):
        raise UnstableFile("%s: metadata changed while reading" % path)


def sha256_fd_stable(path, expect_st=None):
    """SHA-256 of a regular file read via open_regular_nofollow.

    Returns (hexdigest, st) with the same before/after metadata check as
    read_regular_file, without holding the whole file in memory.
    """
    fd, st1 = open_regular_nofollow(path, expect_st)
    try:
        h = hashlib.sha256()
        total = 0
        while True:
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                break
            h.update(chunk)
            total += len(chunk)
        st2 = os.fstat(fd)
    finally:
        os.close(fd)
    _check_stable(path, st1, st2)
    if total != st1.st_size:
        raise UnstableFile("%s: read %d bytes, size was %d" % (path, total, st1.st_size))
    return h.hexdigest(), st1



def snapshot_tree(root, subdir):
    """Recursive snapshot of root/subdir (tracked, untracked and ignored alike)."""
    base = os.path.join(root, subdir)
    entries = []
    if not os.path.lexists(base):
        return entries
    st = os.lstat(base)
    if not stat.S_ISDIR(st.st_mode):
        entries.append(_entry(subdir, base, st))
        return entries
    entries.append(_entry(subdir, base, st))
    for dp, dns, fns in os.walk(base, followlinks=False):
        rel_dp = os.path.relpath(dp, root)
        dns.sort()
        for d in dns:
            full = os.path.join(dp, d)
            entries.append(_entry(os.path.join(rel_dp, d), full, os.lstat(full)))
        for fn in sorted(fns):
            full = os.path.join(dp, fn)
            entries.append(_entry(os.path.join(rel_dp, fn), full, os.lstat(full)))
    entries.sort(key=lambda e: e["path"])
    return entries


def _entry(rel, full, st):
    rel = rel.replace(os.sep, "/")
    e = {"path": rel, "perm": "%04o" % (st.st_mode & 0o7777)}
    if stat.S_ISLNK(st.st_mode):
        e["type"] = "symlink"
        e["mode"] = "120000"
        e["target"] = os.readlink(full)
    elif stat.S_ISDIR(st.st_mode):
        e["type"] = "dir"
        e["mode"] = "040000"
    elif stat.S_ISREG(st.st_mode):
        # hash through a non-following descriptor and require that the
        # opened file is the one lstat() described (no TOCTOU swap)
        digest, st_fd = sha256_fd_stable(full, expect_st=st)
        e["type"] = "file"
        e["mode"] = "100755" if st_fd.st_mode & 0o111 else "100644"
        e["size"] = st_fd.st_size
        e["sha256"] = digest
    else:
        e["type"] = "other"
        e["mode"] = "000000"
    return e


def compare_snapshots(expected, actual):
    """Return list of human-readable differences (empty == identical)."""
    exp = {e["path"]: e for e in expected}
    act = {e["path"]: e for e in actual}
    diffs = []
    for p in sorted(set(exp) - set(act)):
        diffs.append("missing: %s" % p)
    for p in sorted(set(act) - set(exp)):
        diffs.append("extra: %s (%s)" % (p, act[p]["type"]))
    for p in sorted(set(exp) & set(act)):
        a, b = exp[p], act[p]
        for k in ("type", "mode", "perm", "size", "sha256", "target"):
            if a.get(k) != b.get(k):
                diffs.append("changed: %s %s %r -> %r" % (p, k, a.get(k), b.get(k)))
    return diffs


def _batch_blob_heads(oids, cwd, nbytes=256):
    """Return dict oid -> first nbytes of blob, via one git cat-file --batch."""
    if not oids:
        return {}
    p = subprocess.Popen(["git", "cat-file", "--batch"], cwd=cwd,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out, _ = p.communicate(("\n".join(oids) + "\n").encode())
    heads = {}
    pos = 0
    for _ in oids:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].decode()
        parts = header.split(" ")
        if len(parts) != 3:
            raise gitutil.GitError("unexpected cat-file header: %r" % header)
        oid, _typ, size = parts
        size = int(size)
        body = out[nl + 1:nl + 1 + size]
        heads[oid] = body[:nbytes]
        pos = nl + 1 + size + 1  # trailing LF
    return heads


def perl_protected_set(ref, cwd):
    """Frozen Perl protected set at *ref*: list of dicts with path/mode/oid/reason."""
    tree = gitutil.ls_tree(ref, cwd=cwd)
    by_ext = []
    candidates = []
    for path, (mode, typ, oid) in tree.items():
        if typ != "blob":
            continue
        if is_perl_name(path):
            by_ext.append((path, mode, oid))
        elif mode != "120000":
            candidates.append((path, mode, oid))
    heads = _batch_blob_heads(sorted({oid for _, _, oid in candidates}), cwd)
    result = []
    for path, mode, oid in by_ext:
        result.append({"path": path, "mode": mode, "oid": oid, "reason": "extension"})
    for path, mode, oid in candidates:
        if is_perl_shebang(heads.get(oid, b"")):
            result.append({"path": path, "mode": mode, "oid": oid, "reason": "shebang"})
    result.sort(key=lambda e: e["path"])
    return result


def is_perl_file_on_disk(path):
    if is_perl_name(path):
        return True
    try:
        fd, _st = open_regular_nofollow(path)
    except UnstableFile:
        return False
    try:
        return is_perl_shebang(os.read(fd, 256))
    finally:
        os.close(fd)


def git_mode_of_stat(st):
    return "100755" if st.st_mode & 0o111 else "100644"
