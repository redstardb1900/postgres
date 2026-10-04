"""
Frozen-environment process runner for the GNU Make evidence cycle.

Everything that touches the build (configure, make, the preprocessor
replay) goes through :func:`run_logged` so that the exact environment,
command line, exit code, duration and the SHA-256 of the captured output
are recorded the same way every time.  The environment is always built
with ``env -i`` semantics from ``frozen_config.json["env"]``; the caller's
environment never leaks into the build.
"""
import hashlib
import json
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True


class EvidenceError(RuntimeError):
    pass


def load_config(path):
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    for key in ("base_commit", "srcdir", "builddir", "evidence_out", "cc", "cxx", "configure_args", "env"):
        if key not in cfg:
            raise EvidenceError("frozen_config.json missing %r" % key)
    for key in ("srcdir", "builddir", "evidence_out", "cc", "cxx"):
        if not os.path.isabs(cfg[key]):
            raise EvidenceError("%s must be absolute: %r" % (key, cfg[key]))
    return cfg


def frozen_env(cfg, extra=None):
    """Return the frozen environment dict (nothing inherited)."""
    env = dict(cfg["env"])
    if extra:
        env.update(extra)
    return env


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def dirperm_probe(parent):
    """Record how the filesystem under *parent* treats a mode-0400 directory.

    POSIX filesystems let ``readdir()`` succeed on a directory without the
    search (x) bit and fail the subsequent ``stat()`` of its entries with
    EACCES.  Some network/virtual filesystems (OrbStack virtiofs) instead fail
    ``readdir()`` itself.  The difference changes which error message a
    program such as ``pg_verifybackup`` emits, so the baseline records it
    next to the test results instead of leaving it to folklore.  Running as
    root (which bypasses permission checks) is reported as well.
    """
    import shutil
    import tempfile

    result = {"parent": parent, "euid": os.geteuid()}
    try:
        tmp = tempfile.mkdtemp(prefix=".zh-dirperm.", dir=parent)
    except OSError as e:
        result["error"] = "mkdtemp: %s" % e.strerror
        return result
    sub = os.path.join(tmp, "d")
    try:
        os.mkdir(sub)
        with open(os.path.join(sub, "f"), "w"):
            pass
        os.chmod(sub, 0o400)
        try:
            entries = os.listdir(sub)
            result["readdir"] = "ok"
            try:
                os.stat(os.path.join(sub, entries[0]) if entries else os.path.join(sub, "f"))
                result["stat_entry"] = "ok"
            except OSError as e:
                result["stat_entry"] = e.strerror
        except OSError as e:
            result["readdir"] = e.strerror
            result["stat_entry"] = None
    finally:
        try:
            os.chmod(sub, 0o700)
        except OSError:
            pass
        shutil.rmtree(tmp, ignore_errors=True)
    if result.get("readdir") == "ok" and result.get("stat_entry") not in (None, "ok"):
        result["semantics"] = "posix"
    elif result.get("readdir") == "ok" and result.get("stat_entry") == "ok":
        result["semantics"] = "unenforced"
    elif result.get("readdir") is not None:
        result["semantics"] = "readdir_fails"
    return result


def run_logged(argv, cwd, env, log_path, stdin=None):
    """Run *argv* in *cwd* with *env*; stdout+stderr go (in order) to *log_path*.

    Returns a dict with argv, cwd, exit code, timing, log path, size and
    SHA-256.  Never raises on a non-zero exit code; the caller decides.
    """
    os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
    t0 = time.time()
    started = utc_now()
    with open(log_path, "wb") as log:
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=stdin or subprocess.DEVNULL,
                             stdout=log, stderr=subprocess.STDOUT)
        rc = p.wait()
    t1 = time.time()
    return {
        "argv": list(argv),
        "cwd": cwd,
        "env": dict(env),
        "started_utc": started,
        "finished_utc": utc_now(),
        "elapsed_sec": round(t1 - t0, 3),
        "exit_code": rc,
        "log": log_path,
        "log_size": os.path.getsize(log_path),
        "log_sha256": sha256_file(log_path),
    }


def capture(argv, cwd=None, env=None, timeout=60):
    """Run a short probe command and return (rc, stdout_text)."""
    try:
        p = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def first_line(text):
    return text.strip().splitlines()[0] if text.strip() else ""


def write_json(path, obj):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")


def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)
