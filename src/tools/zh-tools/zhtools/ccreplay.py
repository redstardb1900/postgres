"""
Compile-command replay for ``--cc-check`` (preprocessor token cross-check).

For every translation-unit variant recorded by :mod:`makelog` the frozen
argv is rewritten -- never by blind string substitution -- and executed
with ``-E -P`` against a *source root* (the baseline srcdir or a
translated tree):

1. every path operand (``-I/-iquote/-isystem/-idirafter/-include/
   -imacros/--sysroot/-isysroot`` and the source file) is resolved
   against the recorded cwd to an absolute path and classified:
   * under the Make builddir  -> kept verbatim (generated headers/TUs),
   * under the frozen srcdir  -> re-rooted to the target root,
   * anything else (system)   -> kept verbatim;
   include order is preserved;
2. only ``-c``, ``-o X``, ``-MMD``, ``-MD``, ``-MP``, ``-MF X``, ``-MT X``,
   ``-MQ X`` are removed;
3. ``-E -P -MMD -MF <side.d> -MT <variant_key> -o -`` is appended together
   with ``-fmacro-prefix-map``/``-ffile-prefix-map`` for *both* the frozen
   srcdir and the target root (generated TUs in the builddir carry
   ``#line`` directives with the frozen srcdir path), so ``__FILE__``
   expands identically on both sides;
4. the preprocessed text is canonicalised (runs of whitespace -> one
   space; GCC's own spacing already encodes token boundaries) and hashed;
   the side depfile is parsed and its project headers are normalised to
   ``SRC/<rel>`` / ``BUILD/<rel>``.

The *canonical argv* (target root replaced by ``{SRCROOT}``) is hashed too;
it must be identical between the two sides.
"""
import hashlib
import json
import multiprocessing
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True

from . import makelog, evidence_run as ev  # noqa: E402

PATH_OPTS_SEP = ("-I", "-iquote", "-isystem", "-idirafter", "-include", "-imacros", "--sysroot", "-isysroot",
                 "-iprefix", "-iwithprefix", "-iwithprefixbefore")
PATH_OPTS_JOINED = ("-I", "-isystem", "-iquote", "-idirafter", "--sysroot=", "-isysroot")
REMOVE_FLAGS = ("-c", "-MMD", "-MD", "-MP")
REMOVE_WITH_ARG = ("-o", "-MF", "-MT", "-MQ")
WS_RX = re.compile(r"\s+")


class ReplayError(Exception):
    pass


def _map_path(abs_path, srcdir, builddir, target_root):
    kind, rel = makelog.classify_path(abs_path, srcdir, builddir)
    if kind == "build":
        return abs_path, "BUILD/" + rel, "build"
    if kind == "src":
        return os.path.join(target_root, rel) if rel else target_root, "{SRCROOT}/" + rel, "src"
    return abs_path, abs_path, "sys"


def rewrite_argv(argv, cwd_abs, srcdir, builddir, target_root, variant_key, side_d):
    """Return (real_argv, canonical_argv, info).

    *real_argv* is what gets executed; *canonical_argv* has the target
    root replaced by ``{SRCROOT}`` and the side depfile path by
    ``{SIDE_D}`` so both sides can be compared token by token.
    """
    real = [argv[0]]
    canon = [argv[0]]
    info = {"n_src_paths": 0, "n_build_paths": 0, "n_sys_paths": 0, "source": None, "source_kind": None}
    saw_c = False
    i = 1
    n = len(argv)

    def add_path(opt, p):
        a = makelog.norm_join(cwd_abs, p)
        r, c, kind = _map_path(a, srcdir, builddir, target_root)
        info["n_%s_paths" % kind] += 1
        if opt:
            real.extend([opt, r])
            canon.extend([opt, c])
        else:
            real.append(r)
            canon.append(c)

    while i < n:
        t = argv[i]
        if t in REMOVE_FLAGS:
            if t == "-c":
                saw_c = True
            i += 1
            continue
        if t in REMOVE_WITH_ARG:
            if i + 1 >= n:
                raise ReplayError("dangling %s" % t)
            i += 2
            continue
        if t in ("-E", "-S", "-x", "-M", "-MM", "-MG", "-save-temps", "-pipe"):
            raise ReplayError("unexpected flag %s in frozen argv" % t)
        if t in PATH_OPTS_SEP:
            if i + 1 >= n:
                raise ReplayError("dangling %s" % t)
            add_path(t, argv[i + 1])
            i += 2
            continue
        joined = None
        for pre in PATH_OPTS_JOINED:
            if t.startswith(pre) and len(t) > len(pre) and t != pre:
                joined = pre
                break
        if joined:
            p = t[len(joined):]
            a = makelog.norm_join(cwd_abs, p)
            r, c, kind = _map_path(a, srcdir, builddir, target_root)
            info["n_%s_paths" % kind] += 1
            real.append(joined + r)
            canon.append(joined + c)
            i += 1
            continue
        if t.startswith("@") and len(t) > 1:
            raise ReplayError("response file %s" % t)
        if not t.startswith("-") and makelog.SOURCE_RX.search(t):
            if info["source"] is not None:
                raise ReplayError("two source operands: %s, %s" % (info["source"], t))
            a = makelog.norm_join(cwd_abs, t)
            r, c, kind = _map_path(a, srcdir, builddir, target_root)
            info["source"] = r
            info["source_kind"] = kind
            info["n_%s_paths" % kind] += 1
            real.append(r)
            canon.append(c)
            i += 1
            continue
        if t.startswith("-f") and ("prefix-map" in t):
            raise ReplayError("frozen argv already has %s" % t)
        real.append(t)
        canon.append(t)
        i += 1
    if not saw_c:
        raise ReplayError("frozen argv has no -c")
    if info["source"] is None:
        raise ReplayError("no source operand")
    tail_real = ["-E", "-P", "-MMD", "-MF", side_d, "-MT", variant_key, "-o", "-",
                 "-fmacro-prefix-map=%s=SRC" % srcdir, "-ffile-prefix-map=%s=SRC" % srcdir]
    tail_canon = ["-E", "-P", "-MMD", "-MF", "{SIDE_D}", "-MT", variant_key, "-o", "-",
                  "-fmacro-prefix-map={BASEROOT}=SRC", "-ffile-prefix-map={BASEROOT}=SRC"]
    if os.path.normpath(target_root) != os.path.normpath(srcdir):
        tail_real += ["-fmacro-prefix-map=%s=SRC" % target_root, "-ffile-prefix-map=%s=SRC" % target_root]
        tail_canon += ["-fmacro-prefix-map={SRCROOT}=SRC", "-ffile-prefix-map={SRCROOT}=SRC"]
    else:
        # keep the canonical shape identical on both sides
        tail_canon += ["-fmacro-prefix-map={SRCROOT}=SRC", "-ffile-prefix-map={SRCROOT}=SRC"]
    real += tail_real
    canon += tail_canon
    return real, canon, info


def canonical_tokens(text):
    """Collapse whitespace; return (canonical_text, n_chunks)."""
    canon = WS_RX.sub(" ", text).strip()
    return canon, (canon.count(" ") + 1 if canon else 0)


def normalize_depfile(text, srcdir, builddir, target_root, cwd_abs):
    rules = makelog.parse_depfile(text)
    hdrs = set()
    sys_hdrs = set()
    for tgt, prereqs in rules:
        for p in prereqs:
            a = makelog.norm_join(cwd_abs, p)
            kind, rel = makelog.classify_path(a, target_root, builddir)
            if kind == "src":
                hdrs.add("SRC/" + rel)
                continue
            kind2, rel2 = makelog.classify_path(a, srcdir, builddir)
            if kind2 == "src":
                hdrs.add("SRC/" + rel2)
            elif kind2 == "build":
                hdrs.add("BUILD/" + rel2)
            else:
                sys_hdrs.add(a)
    return sorted(hdrs), sorted(sys_hdrs)


def _run_one(job):
    (key, real_argv, canon_argv, cwd_abs, side_d, srcdir, builddir, target_root, info, lang, generated) = job
    try:
        p = subprocess.run(real_argv, cwd=cwd_abs, env=job_env(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as e:
        return {"variant_key": key, "status": "failed", "error": str(e)}
    # NOTE: nothing time- or machine-dependent goes into this record; tokens.jsonl
    # must be byte-identical when the replay is repeated on the same evidence.
    rec = {
        "variant_key": key,
        "lang": lang,
        "generated": generated,
        "source": _map_path(info["source"], target_root, builddir, target_root)[1].replace("{SRCROOT}/", "SRC/"),
        "source_kind": info["source_kind"],
        "canonical_argv_sha256": hashlib.sha256(json.dumps(canon_argv).encode()).hexdigest(),
        "n_src_paths": info["n_src_paths"],
        "n_build_paths": info["n_build_paths"],
        "n_sys_paths": info["n_sys_paths"],
    }
    if p.returncode != 0:
        rec["status"] = "failed"
        rec["rc"] = p.returncode
        rec["stderr"] = p.stderr.decode("utf-8", "replace")[-4000:]
        return rec
    text = p.stdout.decode("utf-8", "replace")
    canon, n_chunks = canonical_tokens(text)
    rec["token_sha256"] = hashlib.sha256(canon.encode("utf-8")).hexdigest()
    rec["n_chunks"] = n_chunks
    rec["n_bytes_pp"] = len(p.stdout)
    rec["stderr_lines"] = len(p.stderr.splitlines())
    try:
        with open(side_d, "rb") as f:
            dtext = f.read().decode("utf-8", "replace")
    except OSError as e:
        rec["status"] = "failed"
        rec["error"] = "side depfile missing: %s" % e
        return rec
    hdrs, sys_hdrs = normalize_depfile(dtext, srcdir, builddir, target_root, cwd_abs)
    # the source itself appears as the first prerequisite; drop it from headers
    hdrs = [h for h in hdrs if h != rec["source"]]
    rec["headers"] = hdrs
    rec["n_headers"] = len(hdrs)
    rec["n_sys_headers_in_depfile"] = len(sys_hdrs)
    rec["headers_sha256"] = hashlib.sha256("\n".join(hdrs).encode()).hexdigest()
    rec["status"] = "ok"
    return rec


_JOB_ENV = {}


def job_env():
    return _JOB_ENV


def _init_worker(env):
    global _JOB_ENV
    _JOB_ENV = env


def replay_variants(cfg, flagsets, recs, target_root, side_dir, jobs=4):
    """Replay the given command records against *target_root*.

    Returns (results sorted by variant_key, prep_errors).  Used both by the
    full baseline replay and by ``--cc-check`` for the subset of variants
    that cover the translated files.
    """
    srcdir = os.path.realpath(cfg["srcdir"])
    builddir = os.path.realpath(cfg["builddir"])
    target_root = os.path.realpath(target_root)
    os.makedirs(side_dir, exist_ok=True)
    env = ev.frozen_env(cfg)
    jobs_list = []
    prep_errors = []
    for rec in recs:
        key = rec["variant_key"]
        argv = makelog.expand_argv(flagsets, rec)
        cwd_abs = os.path.join(builddir, rec["cwd"]) if rec["cwd"] else builddir
        side_d = os.path.join(side_dir, key.replace("/", "__") + ".d")
        try:
            real, canon, info = rewrite_argv(argv, cwd_abs, srcdir, builddir, target_root, key, side_d)
        except ReplayError as e:
            prep_errors.append("%s: %s" % (key, e))
            continue
        jobs_list.append((key, real, canon, cwd_abs, side_d, srcdir, builddir, target_root, info, rec["lang"],
                          rec["generated"]))
    if jobs > 1 and len(jobs_list) > 1:
        with multiprocessing.Pool(jobs, initializer=_init_worker, initargs=(env,)) as pool:
            results = pool.map(_run_one, jobs_list, chunksize=4)
    else:
        _init_worker(env)
        results = [_run_one(j) for j in jobs_list]
    results.sort(key=lambda r: r["variant_key"])
    return results, prep_errors


def replay_all(cfg, cc_dir, target_root, label, jobs=4, side_dir=None):
    srcdir = os.path.realpath(cfg["srcdir"])
    builddir = os.path.realpath(cfg["builddir"])
    target_root = os.path.realpath(target_root)
    if not os.path.isdir(target_root):
        raise ev.EvidenceError("target root %s does not exist" % target_root)
    for comp in (cfg["cc"], cfg["cxx"]):
        if not os.access(comp, os.X_OK):
            raise ev.EvidenceError("frozen compiler %s is not executable" % comp)
    flagsets, recs = makelog.read_commands(cc_dir)
    side_dir = side_dir or os.path.join(cfg["evidence_out"], "replay-" + label)
    os.makedirs(side_dir, exist_ok=True)
    t0 = time.time()
    env = ev.frozen_env(cfg)
    results, prep_errors = replay_variants(cfg, flagsets, recs, target_root, side_dir, jobs)
    # tokens.jsonl (small, goes into the repo): hashes only.
    # headers.jsonl (bulky, stays outside the repo): the normalised header
    # list per variant; its per-variant sha256 is in tokens.jsonl.
    tokens_path = os.path.join(side_dir, "tokens.jsonl")
    headers_path = os.path.join(side_dir, "headers.jsonl")
    with open(tokens_path, "w", encoding="utf-8") as f, open(headers_path, "w", encoding="utf-8") as g:
        for r in results:
            hdrs = r.pop("headers", None)
            if hdrs is not None:
                g.write(json.dumps({"variant_key": r["variant_key"], "headers": hdrs}) + "\n")
            f.write(json.dumps(r, sort_keys=True) + "\n")
    n_ok = sum(1 for r in results if r["status"] == "ok")
    n_failed = len(results) - n_ok + len(prep_errors)
    cache = hashlib.sha256()
    for r in results:
        if r["status"] == "ok":
            cache.update(("%s %s %s\n" % (r["variant_key"], r["token_sha256"], r["headers_sha256"])).encode())
    out = {
        "label": label,
        "target_root": target_root,
        "n_variants": len(recs),
        "n_ok": n_ok,
        "n_failed": n_failed,
        "prep_errors": prep_errors,
        "failed": [{"variant_key": r["variant_key"], "error": r.get("error") or r.get("stderr", "")[-500:]}
                   for r in results if r["status"] != "ok"],
        "token_cache_sha256": cache.hexdigest(),
        "tokens_path": tokens_path,
        "tokens_sha256": ev.sha256_file(tokens_path),
        "headers_path": headers_path,
        "headers_sha256": ev.sha256_file(headers_path),
        "headers_size": os.path.getsize(headers_path),
        "elapsed_sec": round(time.time() - t0, 3),
        "jobs": jobs,
        "cc_version": ev.first_line(ev.capture([cfg["cc"], "--version"], env=env)[1]),
        "cxx_version": ev.first_line(ev.capture([cfg["cxx"], "--version"], env=env)[1]),
    }
    ev.write_json(os.path.join(side_dir, "replay.json"), out)
    return out


def read_tokens(path):
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                r = json.loads(line)
                out[r["variant_key"]] = r
    return out


def read_headers(path, tokens=None):
    """Read the bulky headers.jsonl; when *tokens* is given every list is
    checked against the per-variant headers_sha256 recorded in tokens.jsonl
    (a mismatch raises EvidenceError: the bulk file is not the one the
    in-repo evidence was frozen with)."""
    out = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            key = r["variant_key"]
            hdrs = r["headers"]
            if tokens is not None:
                t = tokens.get(key)
                if t is None or t.get("status") != "ok":
                    continue
                if hashlib.sha256("\n".join(hdrs).encode()).hexdigest() != t["headers_sha256"]:
                    raise ev.EvidenceError("headers.jsonl entry for %s does not match tokens.jsonl" % key)
            out[key] = hdrs
    return out


def compare_token_files(base_path, other_path):
    base = read_tokens(base_path)
    other = read_tokens(other_path)
    same = different = missing = 0
    diffs = []
    for key, b in sorted(base.items()):
        o = other.get(key)
        if o is None or o.get("status") != "ok":
            missing += 1
            diffs.append({"variant_key": key, "kind": "missing_or_failed"})
            continue
        if b.get("status") != "ok":
            missing += 1
            diffs.append({"variant_key": key, "kind": "baseline_failed"})
            continue
        problems = []
        if b["token_sha256"] != o["token_sha256"]:
            problems.append("tokens")
        if b["headers_sha256"] != o["headers_sha256"]:
            problems.append("headers")
        if b["canonical_argv_sha256"] != o["canonical_argv_sha256"]:
            problems.append("canonical_argv")
        if problems:
            different += 1
            diffs.append({"variant_key": key, "kind": "+".join(problems)})
        else:
            same += 1
    return {"base": base_path, "other": other_path, "same": same, "different": different, "missing": missing,
            "extra_in_other": sorted(set(other) - set(base)), "diffs": diffs}


# ---------------------------------------------------------------------------
# freeze: validate the whole evidence set and write baseline.json
# ---------------------------------------------------------------------------

CACHE_KEY_FIELDS = ("base_commit", "configure_args", "env", "cc_version", "cxx_version", "make_version", "logs",
                    "closure_sha256", "commands_sha256", "flagsets_sha256", "po_headers_sha256", "tokens_sha256",
                    "token_cache_sha256")


def compute_cache_key(material):
    """The indivisible cache key: logs, .o/.Po closure, canonical argv, tool versions and token hashes."""
    missing = [k for k in CACHE_KEY_FIELDS if k not in material]
    if missing:
        raise ev.EvidenceError("cache key material missing %s" % ", ".join(missing))
    return hashlib.sha256(json.dumps({k: material[k] for k in CACHE_KEY_FIELDS}, sort_keys=True).encode()).hexdigest()

def _tracked_c_files(srcdir):
    env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
    rc, out = ev.capture(["git", "ls-files", "-z", "--", "*.c", "*.h", "*.cpp"], cwd=srcdir, env=env, timeout=120)
    if rc != 0:
        raise ev.EvidenceError("git ls-files failed in %s" % srcdir)
    return sorted(p for p in out.split("\0") if p)


def _first_pp_conditional(path):
    """Return the first preprocessor conditional that appears before any code, or None."""
    try:
        with open(path, "rb") as f:
            data = f.read(65536).decode("utf-8", "replace")
    except OSError:
        return None
    # strip comments crudely for this heuristic only (not used for proofs)
    data = re.sub(r"/\*.*?\*/", "", data, flags=re.S)
    data = re.sub(r"//[^\n]*", "", data)
    for line in data.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("#"):
            if re.match(r"#\s*(if|ifdef|ifndef)\b", s):
                return s[:200]
            if re.match(r"#\s*(include|define|undef|pragma)\b", s):
                continue
            return None
        return None
    return None


PLATFORM_HINTS = (
    ("win32", r"(^|/)(win32|windows)|win32[a-z_]*\.(c|h)$|(^|/)msvc/"),
    ("darwin", r"darwin|(^|/)macos"),
    ("other_unix", r"(^|/)(aix|solaris|freebsd|netbsd|openbsd|cygwin|hpux|hurd|sysv_sema|sysv_shmem)"),
    ("llvm", r"(^|/)jit/llvm/|llvmjit"),
    ("tcl", r"(^|/)pl/tcl/"),
    ("zstd", r"zstd"),
    ("systemd", r"systemd"),
    ("arch", r"(^|/)atomics/arch-|(^|/)pg_crc32c_(sse42|armv8|sb8)|_x86|x86_64|ppc|sparc|arm"),
    ("ecpg_expected", r"^src/interfaces/ecpg/test/expected/"),
    ("test_only_sources", r"^src/test/|^src/tools/"),
)


def platform_candidates(srcdir, covered):
    """Uncovered tracked .c/.h/.cpp files with mechanical hints (never auto-approved)."""
    out = []
    for rel in _tracked_c_files(srcdir):
        if "SRC/" + rel in covered:
            continue
        hints = [name for name, rx in PLATFORM_HINTS if re.search(rx, rel)]
        out.append({
            "path": rel,
            "path_hints": hints,
            "first_pp_conditional": _first_pp_conditional(os.path.join(srcdir, rel)),
            "status": "candidate",
            "platform_guard": None,
            "reason": None,
            "reviewed_by": None,
            "review_commit": None,
        })
    return out


def _check_external_log(problems, label, rec, key_log="log", key_sha="log_sha256"):
    """An out-of-repo log recorded as (path, sha256) must still exist and hash the same."""
    path = rec.get(key_log)
    sha = rec.get(key_sha)
    if not path or not sha:
        problems.append("%s record has no %s/%s" % (label, key_log, key_sha))
        return
    if not os.path.exists(path):
        problems.append("%s log missing on disk: %s" % (label, path))
    elif ev.sha256_file(path) != sha:
        problems.append("%s log changed on disk: %s" % (label, path))


# baseline.json fields that are allowed to differ between a freeze and a later
# `freeze --verify` of the same evidence (timestamps only; everything else is
# derived from the evidence set and must reproduce byte for byte)
_FREEZE_VOLATILE = ("frozen_utc",)
_CANDIDATES_VOLATILE = ("generated_utc",)


def _stable(obj, volatile):
    return {k: v for k, v in obj.items() if k not in volatile}


def freeze(cfg, evidence_dir, cc_dir, verify=False):
    """Validate the whole evidence set.

    ``verify=False`` writes ``cc_evidence/baseline.json`` (status ``frozen``
    or ``draft``) and ``platform_exceptions_candidates.json``.
    ``verify=True`` is read-only: it recomputes both from the evidence on
    disk and compares them with the committed files; nothing is written.
    Platform candidates are only ever reported -- neither mode approves any.
    """
    problems = []
    report = {"tool": "make_evidence.py freeze" + (" --verify" if verify else ""), "generated_utc": ev.utc_now(),
              "mode": "verify" if verify else "freeze"}
    required = ["environment.json", "configure.json", "check_world.json"] + \
               ["make_%s.json" % t["name"] for t in cfg["capture_targets"]]
    for name in required:
        p = os.path.join(evidence_dir, name)
        if not os.path.exists(p):
            problems.append("missing %s" % name)
    for name in ("flagsets.json", "commands.jsonl", "closure.json", "tokens.jsonl", "replay_base.json"):
        if not os.path.exists(os.path.join(cc_dir, name)):
            problems.append("missing cc_evidence/%s" % name)
    if verify:
        for name in ("baseline.json", "platform_exceptions_candidates.json"):
            if not os.path.exists(os.path.join(cc_dir, name)):
                problems.append("missing cc_evidence/%s (nothing to verify against)" % name)
    if problems:
        report["problems"] = problems
        report["reason"] = "incomplete evidence"
        return 2, report

    closure = ev.read_json(os.path.join(cc_dir, "closure.json"))
    replay = ev.read_json(os.path.join(cc_dir, "replay_base.json"))
    # bulk files outside the repo must still be exactly what was recorded
    bulk = {"po_headers": closure["bulk"]["po_headers"],
            "headers_base": {"path": replay["headers_path"], "sha256": replay["headers_sha256"],
                             "size": replay["headers_size"]}}
    for name, b in bulk.items():
        if not os.path.exists(b["path"]):
            problems.append("bulk file %s missing: %s" % (name, b["path"]))
        elif ev.sha256_file(b["path"]) != b["sha256"]:
            problems.append("bulk file %s changed on disk: %s" % (name, b["path"]))
    if problems:
        report["problems"] = problems
        report["reason"] = "bulk evidence missing or changed"
        return 2, report
    if not closure["closed"]:
        problems.append("closure is not closed")
    if closure["unverified"]:
        problems.append("%d unverified findings in closure" % len(closure["unverified"]))
    # the logs must still match what collect() saw
    for t in cfg["capture_targets"]:
        rec = ev.read_json(os.path.join(evidence_dir, "make_%s.json" % t["name"]))
        if rec["exit_code"] != 0:
            problems.append("capture target %s exit %d" % (t["name"], rec["exit_code"]))
        logged = closure["logs"].get(t["name"], {}).get("sha256")
        if logged != rec["log_sha256"]:
            problems.append("log sha for %s differs between make record and closure" % t["name"])
        _check_external_log(problems, "make %s" % t["name"], rec)
    # configure and check-world keep their logs outside the repo too; the
    # baseline is only as good as those files, so they are hashed on every
    # freeze/verify, not just when they were captured
    cw = ev.read_json(os.path.join(evidence_dir, "check_world.json"))
    conf = ev.read_json(os.path.join(evidence_dir, "configure.json"))
    _check_external_log(problems, "configure", conf)
    if conf.get("exit_code") != 0:
        problems.append("configure exit %r" % conf.get("exit_code"))
    if not conf.get("config_log_sha256"):
        problems.append("configure record has no config_log_sha256")
    else:
        for cl in (os.path.join(cfg["builddir"], "config.log"), os.path.join(cfg["evidence_out"], "config.log")):
            if not os.path.exists(cl):
                problems.append("config.log missing: %s" % cl)
            elif ev.sha256_file(cl) != conf["config_log_sha256"]:
                problems.append("config.log changed on disk: %s" % cl)
    _check_external_log(problems, "check-world", cw)
    tokens = read_tokens(os.path.join(cc_dir, "tokens.jsonl"))
    flagsets, recs = makelog.read_commands(cc_dir)
    keys = set(r["variant_key"] for r in recs)
    bad = [k for k, r in tokens.items() if r.get("status") != "ok"]
    if bad:
        problems.append("%d variants failed in the baseline replay (first: %s)" % (len(bad), bad[0]))
    if set(tokens) != keys:
        problems.append("tokens.jsonl keys != commands.jsonl keys (%d vs %d)" % (len(tokens), len(keys)))
    headers = read_headers(bulk["headers_base"]["path"], tokens)
    if set(headers) != set(k for k, r in tokens.items() if r.get("status") == "ok"):
        problems.append("headers.jsonl variants != tokens.jsonl ok variants")

    # .Po superset audit: side depfile headers must be a subset of Make's .Po
    # headers (the variant's own .Po, or the inherited base .Po)
    po_hdrs = {}
    with open(bulk["po_headers"]["path"], "r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                po_hdrs[r["variant_key"]] = set(r["po_headers"])
    # A variant with its own .Po must stay within it.  A variant whose .Po is
    # inherited (``%_shlib.o: %.c %.o`` / ``%_srv.o`` rules: Make declares the
    # base object's .Po as the only dependency record) may legitimately see
    # more headers than the base object did -- ``_srv.o`` is compiled without
    # -DFRONTEND and pulls in postgres.h/elog.h.  That is a property of
    # PostgreSQL's Makefiles, not of the replay, so it is recorded (every
    # extra header, per variant) instead of blocking the freeze; the side
    # depfile written by the compiler for exactly that argv is the header
    # record that cc-check indexes.
    inherited = {r["variant_key"]: r["po_inherited_from"] for r in recs if r.get("po_inherited_from")}
    superset_violations = []
    inherited_not_superset = []
    for k, hdrs in sorted(headers.items()):
        extra = set(hdrs) - po_hdrs.get(k, set())
        if not extra:
            continue
        if k in inherited:
            inherited_not_superset.append({"variant_key": k, "po_inherited_from": inherited[k],
                                           "not_in_po": sorted(extra)})
        else:
            superset_violations.append({"variant_key": k, "not_in_po": sorted(extra)[:10]})
    if superset_violations:
        problems.append("%d variants have side-depfile headers missing from their own Make .Po" %
                        len(superset_violations))
    report["po_superset"] = {
        "checked": len(headers),
        "own_po_violations": superset_violations[:50],
        "inherited_checked": len([k for k in headers if k in inherited]),
        "inherited_not_superset": inherited_not_superset,
    }

    # coverage
    covered = set()
    n_src_tus = 0
    for k, r in tokens.items():
        if r.get("status") != "ok":
            continue
        covered.update(headers.get(k, []))
        if r["source_kind"] == "src":
            # tokens.jsonl stores the source already normalised ("SRC/<rel>")
            if not r["source"].startswith("SRC/"):
                raise ev.EvidenceError("variant %s: src source not normalised: %r" % (k, r["source"]))
            covered.add(r["source"])
            n_src_tus += 1
    covered_src = sorted(h for h in covered if h.startswith("SRC/"))
    tracked = _tracked_c_files(os.path.realpath(cfg["srcdir"]))
    candidates = platform_candidates(os.path.realpath(cfg["srcdir"]), covered)
    report["coverage"] = {
        "tracked_c_h_cpp": len(tracked),
        "covered_tracked": len([t for t in tracked if "SRC/" + t in covered]),
        "uncovered_tracked": len(candidates),
        "covered_src_paths_total": len(covered_src),
        "covered_build_paths_total": len([h for h in covered if h.startswith("BUILD/")]),
        "variants": len(tokens),
        "variants_src_source": n_src_tus,
        "variants_generated_source": sum(1 for r in tokens.values() if r.get("generated")),
    }
    candidates_doc = {
        "schema_version": 2,
        "generated_utc": report["generated_utc"],
        "note": "Tracked .c/.h/.cpp files not covered by any passing baseline variant (as source or via side "
                "depfile). Every entry is a CANDIDATE only: it may enter the platform exception table solely "
                "after review records platform_guard/reason/reviewed_by/review_commit. Nothing here is "
                "approved by this tool.",
        "n_candidates": len(candidates),
        "candidates": candidates,
    }
    report["platform_candidates"] = {
        "n_candidates": len(candidates),
        "by_hint": _count_hints(candidates),
        "approved": 0,
        "note": "report only; approval is a separate reviewed step, never performed by freeze/verify",
    }
    if not verify:
        ev.write_json(os.path.join(cc_dir, "platform_exceptions_candidates.json"), candidates_doc)

    envj = ev.read_json(os.path.join(evidence_dir, "environment.json"))
    status = "frozen" if not problems else "draft"
    key_material = {
        "base_commit": cfg["base_commit"],
        "configure_args": cfg["configure_args"],
        "env": cfg["env"],
        "cc_version": envj["versions"]["cc"]["first_line"],
        "cxx_version": envj["versions"]["cxx"]["first_line"],
        "make_version": envj["versions"]["make"]["first_line"],
        "logs": {t["name"]: closure["logs"][t["name"]]["sha256"] for t in cfg["capture_targets"]},
        "closure_sha256": ev.sha256_file(os.path.join(cc_dir, "closure.json")),
        "commands_sha256": ev.sha256_file(os.path.join(cc_dir, "commands.jsonl")),
        "flagsets_sha256": ev.sha256_file(os.path.join(cc_dir, "flagsets.json")),
        "po_headers_sha256": bulk["po_headers"]["sha256"],
        "tokens_sha256": ev.sha256_file(os.path.join(cc_dir, "tokens.jsonl")),
        "token_cache_sha256": replay.get("token_cache_sha256"),
    }
    baseline = {
        "schema_version": 2,
        "status": status,
        "base_commit": cfg["base_commit"],
        "srcdir": cfg["srcdir"],
        "builddir": cfg["builddir"],
        "cc": cfg["cc"],
        "cxx": cfg["cxx"],
        "cc_version": key_material["cc_version"],
        "cxx_version": key_material["cxx_version"],
        "make_version": key_material["make_version"],
        "configure_args": cfg["configure_args"],
        "env": cfg["env"],
        "logs": key_material["logs"],
        "n_variants": len(tokens),
        "n_objects": closure["n_objects"],
        "n_po_files": closure["n_po_files"],
        "coverage_closed": closure["closed"],
        "bulk_files": bulk,
        "token_cache_sha256": key_material["token_cache_sha256"],
        "cache_key_sha256": compute_cache_key(key_material),
        "cache_key_material": key_material,
        "check_world_exit_code": cw["exit_code"],
        "check_world_log_sha256": cw["log_sha256"],
        "check_world_args": cw["argv"],
        # the baseline failure set: final acceptance must reproduce exactly this (see environment.md)
        "check_world_failures": sorted(
            ({"dir": s["dir"], "kind": s["kind"], "test": t["name"], "status": t["status"],
              "detail": t.get("detail", "")}
             for s in cw["results"]["sections"] for t in s["tests"]
             if t["status"] in ("failed", "failed_ignored") or t["status"].startswith("unknown")),
            key=lambda f: (f["dir"], f["test"])),
        "check_world_summary": {k: cw["results"]["summary"][k] for k in (
            "sections", "regress_suites", "regress_tests", "regress_failed_tests", "regress_skipped_tests",
            "tap_suites", "tap_files", "tap_failed_files", "tap_skipped_files", "tap_results",
            "compiler_warnings", "make_errors", "regression_diffs_files")},
        "configure_exit_code": conf["exit_code"],
        "coverage": report["coverage"],
        "po_superset": report["po_superset"],
        "problems": problems,
        "frozen_utc": report["generated_utc"],
    }
    report["cache_key_sha256"] = baseline["cache_key_sha256"]
    if not verify:
        ev.write_json(os.path.join(cc_dir, "baseline.json"), baseline)
        report["problems"] = problems
        report["reason"] = "all evidence consistent" if not problems else "see problems"
        return (0 if not problems else 1), report

    # --verify: nothing written; the committed files must be exactly what the
    # evidence on disk reproduces (timestamps excepted)
    committed = ev.read_json(os.path.join(cc_dir, "baseline.json"))
    committed_cands = ev.read_json(os.path.join(cc_dir, "platform_exceptions_candidates.json"))
    mismatches = []
    if committed.get("status") != "frozen":
        mismatches.append("committed baseline.json status is %r, not 'frozen'" % committed.get("status"))
    want = _stable(baseline, _FREEZE_VOLATILE)
    have = _stable(committed, _FREEZE_VOLATILE)
    for k in sorted(set(want) | set(have)):
        if k == "problems":
            continue
        if want.get(k) != have.get(k):
            mismatches.append("baseline.json field %r differs from the evidence on disk" % k)
    try:
        self_key = compute_cache_key(committed.get("cache_key_material") or {})
    except ev.EvidenceError:
        self_key = None
    if committed.get("cache_key_sha256") != self_key:
        mismatches.append("committed cache_key_sha256 does not match its own cache_key_material")
    want_c = _stable(candidates_doc, _CANDIDATES_VOLATILE)
    have_c = _stable(committed_cands, _CANDIDATES_VOLATILE)
    if want_c != have_c:
        wp = [c["path"] for c in want_c.get("candidates", [])]
        hp = [c["path"] for c in have_c.get("candidates", [])]
        mismatches.append("platform_exceptions_candidates.json differs from the evidence on disk "
                          "(%d vs %d candidates; +%d/-%d paths)" % (len(wp), len(hp), len(set(wp) - set(hp)),
                                                                   len(set(hp) - set(wp))))
    not_candidate = [c["path"] for c in committed_cands.get("candidates", []) if c.get("status") != "candidate"
                     or c.get("reviewed_by") or c.get("review_commit")]
    if not_candidate:
        mismatches.append("%d committed candidates carry approval fields; approvals are out of scope for "
                          "freeze/verify (first: %s)" % (len(not_candidate), not_candidate[0]))
    report["problems"] = problems
    report["mismatches"] = mismatches
    report["committed_cache_key_sha256"] = committed.get("cache_key_sha256")
    if problems or mismatches:
        report["reason"] = "evidence does not reproduce the committed baseline"
        return 1, report
    report["reason"] = "committed baseline reproduced from the evidence on disk; nothing written"
    return 0, report


def _count_hints(candidates):
    out = {}
    for c in candidates:
        key = "+".join(c["path_hints"]) if c["path_hints"] else "unhinted"
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items()))
