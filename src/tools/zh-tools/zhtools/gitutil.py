"""Thin wrappers around the git command line (no third-party libraries)."""
import hashlib
import os
import subprocess
import sys

sys.dont_write_bytecode = True


class GitError(RuntimeError):
    pass


def run(args, cwd=None, check=True, binary=False):
    """Run git with *args* in *cwd*; return stdout (str, or bytes if binary)."""
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    env["GIT_OPTIONAL_LOCKS"] = "0"
    p = subprocess.run(["git"] + list(args), cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode != 0:
        raise GitError("git %s failed (%d): %s" % (" ".join(args), p.returncode,
                                                   p.stderr.decode("utf-8", "replace").strip()))
    if binary:
        return p.stdout
    return p.stdout.decode("utf-8", "replace")


def version():
    return run(["--version"]).strip()


def rev_parse(rev, cwd=None):
    return run(["rev-parse", "--verify", rev + "^{object}"], cwd=cwd).strip()


def toplevel(cwd):
    return run(["rev-parse", "--show-toplevel"], cwd=cwd).strip()


def show_blob(ref, path, cwd=None):
    """Return bytes of ref:path, or None if it does not exist."""
    p = subprocess.run(["git", "cat-file", "blob", "%s:%s" % (ref, path)], cwd=cwd,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        return None
    return p.stdout


def ls_tree(ref, cwd=None, path=None):
    """Return dict path -> (mode, type, oid) for all entries under ref."""
    args = ["ls-tree", "-r", "-z", "--full-tree", ref]
    if path:
        args += ["--", path]
    out = run(args, cwd=cwd, binary=True)
    result = {}
    for rec in out.split(b"\0"):
        if not rec:
            continue
        meta, p = rec.split(b"\t", 1)
        mode, typ, oid = meta.split(b" ")
        result[p.decode("utf-8", "surrogateescape")] = (mode.decode(), typ.decode(), oid.decode())
    return result


def ls_files(cwd, extra=()):
    out = run(["ls-files", "-z"] + list(extra), cwd=cwd, binary=True)
    return [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]


def diff_names(old, new=None, cwd=None, paths=(), filters=None):
    """``git diff --name-status`` between refs (or ref vs. worktree if new is None)."""
    args = ["diff", "--name-status", "-z", "--no-renames", "--no-color"]
    if filters:
        args.append("--diff-filter=" + filters)
    args.append(old)
    if new is not None:
        args.append(new)
    if paths:
        args += ["--"] + list(paths)
    out = run(args, cwd=cwd, binary=True)
    parts = out.split(b"\0")
    res = []
    i = 0
    while i + 1 < len(parts):
        status = parts[i].decode()
        path = parts[i + 1].decode("utf-8", "surrogateescape")
        res.append((status, path))
        i += 2
    return res


def blob_oid(data):
    """SHA-1 object id git would assign to *data* as a blob."""
    h = hashlib.sha1()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()
