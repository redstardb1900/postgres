#!/usr/bin/env python3
"""
make_evidence.py -- drive the frozen GNU Make VPATH evidence cycle.

All parameters come from evidence/frozen_config.json (srcdir = REL_16_11_base,
builddir outside every worktree, CC/CXX, configure arguments, env).  Large
logs are written to the out-of-repo directory cfg["evidence_out"]; every
in-repo report records their path and SHA-256.

Subcommands:
  environment          probe OS / locale / tool versions -> evidence/environment.json
  configure            fresh builddir + configure              -> evidence/configure.json
  make NAME            run a capture target from cfg["capture_targets"] with
                       `make -j1 -w ...` and record the log       -> evidence/make_<NAME>.json
  check-world          run `make -j<N> -Orecurse -w check-world`, parse the
                       log and on-disk results                  -> evidence/check_world.json
  collect              parse the -j1 -w logs, build .o/.Po/argv closure
                       -> cc_evidence/{commands.jsonl,flagsets.json,closure.json}
  replay ROOT          replay every variant with -E -P -MMD against ROOT
                       (the base srcdir or a translated tree); write token
                       hashes and side depfiles             -> cc_evidence/tokens.jsonl
  freeze               validate everything (including the SHA-256 of every
                       out-of-repo log: make captures, configure, check-world,
                       and both the builddir and evidence_out config.log;
                       a missing config.log is a hard fail),
                       write cc_evidence/baseline.json with status "frozen"
                       and the cache key
  freeze --verify      read-only: recompute from the evidence on disk and
                       compare with the committed baseline.json and platform
                       candidates; writes nothing.  Platform candidates are
                       reported only, never approved by either mode.

Exit codes: 0 ok, 1 the step produced a FAIL verdict, 2 unverified / error.
"""
import argparse
import glob
import json
import os
import platform
import re
import shutil
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zhtools import TOOLS_VERSION, evidence_run as ev  # noqa: E402
from zhtools import makelog, ccreplay, checkworld  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
EVIDENCE_DIR = os.path.join(HERE, "evidence")
CC_EVIDENCE_DIR = os.path.join(HERE, "cc_evidence")
DEFAULT_CONFIG = os.path.join(EVIDENCE_DIR, "frozen_config.json")


def _probe_versions(cfg, env):
    probes = {
        "cc": [cfg["cc"], "--version"],
        "cxx": [cfg["cxx"], "--version"],
        "cc_dumpmachine": [cfg["cc"], "-dumpmachine"],
        "make": ["make", "--version"],
        "bison": ["bison", "--version"],
        "flex": ["flex", "--version"],
        "perl": ["perl", "-e", "print $^V"],
        "python3": ["python3", "--version"],
        "xgettext": ["xgettext", "--version"],
        "msgfmt": ["msgfmt", "--version"],
        "git": ["git", "--version"],
        "pkg-config": ["pkg-config", "--version"],
        "dtrace": ["rpm", "-q", "systemtap-sdt-devel"],
        "ld": ["ld", "--version"],
        "ar": ["ar", "--version"],
        "openssl": ["openssl", "version"],
        "prove": ["prove", "--version"],
    }
    out = {}
    for name, argv in probes.items():
        rc, text = ev.capture(argv, env=env)
        out[name] = {"argv": argv, "rc": rc, "first_line": ev.first_line(text)}
    pkgs = ["icu-uc", "icu-i18n", "libxml-2.0", "libxslt", "liblz4", "libzstd", "openssl", "krb5-gssapi",
            "libselinux", "libsystemd", "uuid", "ossp-uuid", "zlib", "readline"]
    out["pkg_config_modversion"] = {}
    for p in pkgs:
        rc, text = ev.capture(["pkg-config", "--modversion", p], env=env)
        out["pkg_config_modversion"][p] = ev.first_line(text) if rc == 0 else None
    headers = ["readline/readline.h", "zlib.h", "openssl/ssl.h", "libxml2/libxml/parser.h", "libxslt/xslt.h",
               "lz4.h", "zstd.h", "gssapi/gssapi.h", "ldap.h", "security/pam_appl.h", "uuid/uuid.h",
               "unicode/ucol.h", "selinux/selinux.h", "systemd/sd-daemon.h", "tcl.h", "python3.9/Python.h",
               "sys/sdt.h"]
    out["usr_include"] = {h: os.path.exists(os.path.join("/usr/include", h)) for h in headers}
    out["programs"] = {}
    for prog in ("tclsh", "llvm-config", "clang", "krb5-config", "slapd", "krb5kdc", "ccache", "distcc", "sccache"):
        out["programs"][prog] = shutil.which(prog, path=env["PATH"])
    return out


def cmd_environment(cfg, args):
    env = ev.frozen_env(cfg)
    rc, locale_out = ev.capture(["locale"], env=env)
    rc2, locale_a = ev.capture(["locale", "-a"], env=env)
    osrel = {}
    try:
        with open("/etc/os-release", "r", encoding="utf-8") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    osrel[k] = v.strip('"')
    except OSError:
        pass
    cc_real = os.path.realpath(cfg["cc"])
    cxx_real = os.path.realpath(cfg["cxx"])
    report = {
        "tool": "make_evidence.py environment",
        "tools_version": TOOLS_VERSION,
        "generated_utc": ev.utc_now(),
        "uname": dict(zip(("sysname", "nodename", "release", "version", "machine"), os.uname())),
        "os_release": osrel,
        "python_platform": platform.platform(),
        "cpu_count": os.cpu_count(),
        "frozen_env": env,
        "caller_env_note": cfg.get("env_unset_note"),
        "locale": locale_out.strip().splitlines(),
        "locale_a_count": len(locale_a.split()),
        "locale_a_has": {l: (l in locale_a.split()) for l in ("C", "C.utf8", "POSIX", "en_US.utf8", "zh_CN.utf8")},
        "cc_realpath": cc_real,
        "cxx_realpath": cxx_real,
        "cc_is_wrapper": not (cc_real.startswith("/usr/") and os.path.basename(cc_real) in ("gcc", "g++", "cc", "c++")),
        "versions": _probe_versions(cfg, env),
        "filesystems": {},
    }
    for key in ("srcdir", "builddir", "evidence_out"):
        p = cfg[key]
        while not os.path.exists(p) and p != "/":
            p = os.path.dirname(p)
        rc, fstype = ev.capture(["stat", "-f", "-c", "%T", p], env=env)
        report["filesystems"][key] = {"probe_path": p, "fstype": fstype.strip()}
    # How a mode-0400 directory behaves on the build filesystem versus /tmp;
    # explains pg_verifybackup/t/003_corruption.pl results (see environment.md).
    report["dirperm_probe"] = {
        "builddir_fs": ev.dirperm_probe(report["filesystems"]["builddir"]["probe_path"]),
        "tmp": ev.dirperm_probe("/tmp"),
    }
    ev.write_json(os.path.join(EVIDENCE_DIR, "environment.json"), report)
    print("environment: wrote evidence/environment.json (%s, %s %s)" % (
        report["versions"]["cc"]["first_line"], osrel.get("NAME"), osrel.get("VERSION")))
    return 0


def _config_status_summary(builddir):
    """Extract the interesting @VAR@ substitutions from config.status."""
    path = os.path.join(builddir, "config.status")
    want = ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBS", "PERL", "PYTHON", "BISON", "FLEX",
            "DTRACE", "MSGFMT", "XGETTEXT", "MSGMERGE", "with_perl", "with_python", "with_tcl", "with_llvm",
            "with_icu", "with_ssl", "with_gssapi", "with_ldap", "with_pam", "with_uuid", "with_libxml",
            "with_libxslt", "with_lz4", "with_zstd", "with_selinux", "with_systemd", "with_readline",
            "with_zlib", "enable_nls", "enable_debug", "enable_cassert", "enable_tap_tests", "enable_dtrace",
            "autodepend", "PG_VERSION", "host_tuple", "TCL_CONFIG_SH", "LLVM_CONFIG", "CLANG",
            "WANTED_LANGUAGES", "GCC", "PG_SYSROOT", "DLSUFFIX", "python_includespec", "perl_includespec")
    out = {}
    rx = re.compile(r'^S\["([^"]+)"\]="(.*)"$')
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
    except OSError as e:
        return {"_error": str(e)}
    i = 0
    while i < len(lines):
        line = lines[i]
        # config.status splits long values as  S["X"]="part"\  /  "part"\  /  "end"
        while line.endswith('"\\') and i + 1 < len(lines):
            i += 1
            nxt = lines[i]
            line = line[:-2] + (nxt[1:] if nxt.startswith('"') else nxt)
        m = rx.match(line)
        if m and m.group(1) in want:
            out[m.group(1)] = m.group(2)
        i += 1
    return out


def _pg_config_h_summary(builddir):
    """Return the defined/undefined state of the feature macros in pg_config.h."""
    path = os.path.join(builddir, "src", "include", "pg_config.h")
    macros = ("USE_ASSERT_CHECKING", "ENABLE_NLS", "USE_OPENSSL", "USE_ICU", "USE_LZ4", "USE_ZSTD",
              "USE_LIBXML", "USE_LIBXSLT", "USE_LDAP", "USE_PAM", "USE_SYSTEMD", "USE_LLVM", "ENABLE_GSS",
              "HAVE_UUID_E2FS", "HAVE_UUID_OSSP", "HAVE_LIBREADLINE", "HAVE_LIBZ", "ENABLE_THREAD_SAFETY",
              "HAVE_LIBSELINUX", "PG_VERSION", "PG_VERSION_STR", "DEF_PGPORT", "BLCKSZ", "XLOG_BLCKSZ",
              "RELSEG_SIZE", "USE_SLICING_BY_8_CRC32C", "USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK", "USE_ARMV8_CRC32C",
              "HAVE_SPINLOCKS", "HAVE_ATOMICS", "PROFILE_PID_DIR", "HAVE_LOCALE_T", "LOCALE_T_IN_XLOCALE")
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        return {"_error": str(e)}
    for m in macros:
        d = re.search(r"^#define %s(?:\s+(.*))?$" % re.escape(m), text, re.M)
        if d:
            out[m] = d.group(1).strip() if d.group(1) else ""
        else:
            out[m] = None
    return out


def cmd_configure(cfg, args):
    builddir = cfg["builddir"]
    srcdir = cfg["srcdir"]
    if os.path.exists(builddir):
        if not args.force:
            print("configure: builddir %s exists; use --force to remove it" % builddir, file=sys.stderr)
            return 2
        shutil.rmtree(builddir)
    os.makedirs(builddir)
    os.makedirs(cfg["evidence_out"], exist_ok=True)
    env = ev.frozen_env(cfg)
    head = ev.capture(["git", "rev-parse", "HEAD"], cwd=srcdir, env=env)[1].strip()
    if head != cfg["base_commit"]:
        print("configure: srcdir HEAD %s != frozen base_commit %s" % (head, cfg["base_commit"]), file=sys.stderr)
        return 2
    dirty = ev.capture(["git", "status", "--porcelain", "--ignored"], cwd=srcdir, env=env)[1].strip()
    if dirty:
        print("configure: srcdir is not clean (VPATH requires a pristine srcdir):\n%s" % dirty, file=sys.stderr)
        return 2
    argv = [os.path.join(srcdir, "configure")] + list(cfg["configure_args"])
    log = os.path.join(cfg["evidence_out"], "configure.log")
    rec = ev.run_logged(argv, builddir, env, log)
    rec["srcdir_head"] = head
    rec["config_log_sha256"] = ev.sha256_file(os.path.join(builddir, "config.log")) if os.path.exists(
        os.path.join(builddir, "config.log")) else None
    rec["config_status_summary"] = _config_status_summary(builddir)
    rec["pg_config_h"] = _pg_config_h_summary(builddir)
    rec["disabled_features"] = cfg.get("disabled_features", {})
    rec["tools_version"] = TOOLS_VERSION
    rec["tool"] = "make_evidence.py configure"
    # Keep a copy of config.log out of the repo but next to the other logs.
    try:
        shutil.copy2(os.path.join(builddir, "config.log"), os.path.join(cfg["evidence_out"], "config.log"))
    except OSError:
        pass
    ev.write_json(os.path.join(EVIDENCE_DIR, "configure.json"), rec)
    print("configure: exit %d in %.1fs; log %s sha256 %s" % (rec["exit_code"], rec["elapsed_sec"], log,
                                                           rec["log_sha256"]))
    return 0 if rec["exit_code"] == 0 else 1


def _target(cfg, name):
    for t in cfg["capture_targets"]:
        if t["name"] == name:
            return t
    raise ev.EvidenceError("unknown capture target %r" % name)


def cmd_make(cfg, args):
    t = _target(cfg, args.name)
    env = ev.frozen_env(cfg)
    argv = ["make"] + list(t["args"])
    log = os.path.join(cfg["evidence_out"], "make_%s.log" % t["name"])
    rec = ev.run_logged(argv, cfg["builddir"], env, log)
    rec.update({"tool": "make_evidence.py make", "tools_version": TOOLS_VERSION, "target": t})
    # quick structural summary of the log
    rec["log_summary"] = makelog.summarize_log(log, (cfg["cc"], cfg["cxx"]))
    ev.write_json(os.path.join(EVIDENCE_DIR, "make_%s.json" % t["name"]), rec)
    print("make %s: exit %d in %.1fs; %d lines, %d compile commands; sha256 %s" % (
        t["name"], rec["exit_code"], rec["elapsed_sec"], rec["log_summary"]["lines"],
        rec["log_summary"]["compile_commands"], rec["log_sha256"]))
    return 0 if rec["exit_code"] == 0 else 1


def cmd_resummarize(cfg, args):
    """Re-derive log_summary (and check-world results) from the captured logs.

    Used when the log analysers gain fields after a capture; the logs are never
    re-run, and a record is only rewritten when its log still has the recorded
    SHA-256, so the summary stays tied to the original run.
    """
    rc = 0
    for t in cfg["capture_targets"]:
        rec_path = os.path.join(EVIDENCE_DIR, "make_%s.json" % t["name"])
        if not os.path.exists(rec_path):
            continue
        rec = ev.read_json(rec_path)
        if ev.sha256_file(rec["log"]) != rec["log_sha256"]:
            print("resummarize: log %s changed since it was recorded; not touching %s" % (rec["log"], rec_path),
                  file=sys.stderr)
            rc = 1
            continue
        rec["log_summary"] = makelog.summarize_log(rec["log"], (cfg["cc"], cfg["cxx"]))
        rec["tools_version"] = TOOLS_VERSION
        ev.write_json(rec_path, rec)
        s = rec["log_summary"]
        print("resummarize %s: %d compile, compiler warnings %d, make warnings %d" % (
            t["name"], s["compile_commands"], s["compiler_warnings"], s["make_warnings"]))
    cw_path = os.path.join(EVIDENCE_DIR, "check_world.json")
    if os.path.exists(cw_path):
        rec = ev.read_json(cw_path)
        if ev.sha256_file(rec["log"]) != rec["log_sha256"]:
            print("resummarize: check-world log changed since it was recorded; not touching it", file=sys.stderr)
            rc = 1
        else:
            rec["results"] = checkworld.parse(rec["log"], cfg["builddir"], cfg["srcdir"])
            rec["tools_version"] = TOOLS_VERSION
            ev.write_json(cw_path, rec)
            print("resummarize check-world: re-parsed %d lines" % rec["results"]["summary"]["log_lines"])
    return rc


def cmd_check_world(cfg, args):
    env = ev.frozen_env(cfg)
    cw = cfg["check_world"]
    argv = ["make"] + list(cw["args"])
    log = os.path.join(cfg["evidence_out"], "make_check_world.log")
    rec = ev.run_logged(argv, cfg["builddir"], env, log)
    rec.update({"tool": "make_evidence.py check-world", "tools_version": TOOLS_VERSION, "jobs": cw["jobs"]})
    results = checkworld.parse(log, cfg["builddir"], cfg["srcdir"])
    rec["results"] = results
    ev.write_json(os.path.join(EVIDENCE_DIR, "check_world.json"), rec)
    s = results["summary"]
    print("check-world: exit %d in %.1fs; regress suites %d (failed tests %d, ignored %d), tap files %d "
          "(fail %d, skipped %d), warnings %d; sha256 %s" % (
              rec["exit_code"], rec["elapsed_sec"], s["regress_suites"], s["regress_failed_tests"],
              s["regress_ignored_failures"], s["tap_files"], s["tap_failed_files"], s["tap_skipped_files"],
              s["compiler_warnings"], rec["log_sha256"]))
    return 0 if rec["exit_code"] == 0 else 1


def cmd_collect(cfg, args):
    logs = []
    for t in cfg["capture_targets"]:
        rec_path = os.path.join(EVIDENCE_DIR, "make_%s.json" % t["name"])
        if not os.path.exists(rec_path):
            print("collect: missing %s (run `make %s` first)" % (rec_path, t["name"]), file=sys.stderr)
            return 2
        rec = ev.read_json(rec_path)
        if rec["exit_code"] != 0:
            print("collect: capture target %s exited %d; refusing" % (t["name"], rec["exit_code"]), file=sys.stderr)
            return 2
        if ev.sha256_file(rec["log"]) != rec["log_sha256"]:
            print("collect: log %s changed since it was recorded" % rec["log"], file=sys.stderr)
            return 2
        logs.append((t["name"], rec["log"], rec["log_sha256"]))
    result = makelog.collect(logs, cfg)
    os.makedirs(CC_EVIDENCE_DIR, exist_ok=True)
    makelog.write_collect_outputs(result, CC_EVIDENCE_DIR, os.path.join(cfg["evidence_out"], "cc_bulk"))
    c = result["closure"]
    print("collect: %d compile commands -> %d variants (%d sources, %d generated); objects %d (+%d symlinks, "
          "%d foreign), Po %d (+%d inherited); closed=%s; unverified=%d" % (
              result["n_compile_commands"], result["n_variants"], result["n_sources"],
              result["n_generated_variants"], c["n_objects"], c["n_object_symlinks"], len(c["foreign_objects"]),
              c["n_po_files"], c["n_po_inherited"], c["closed"], len(result["unverified"])))
    for u in result["unverified"][:20]:
        print("  UNVERIFIED: %s" % u)
    for k in ("objects_without_argv", "argv_without_object", "objects_without_po", "po_without_object"):
        if c[k]:
            print("  %s: %d (first: %s)" % (k, len(c[k]), c[k][0]))
    return 0 if (c["closed"] and not result["unverified"]) else 1


def cmd_replay(cfg, args):
    root = os.path.abspath(args.root)
    label = args.label or ("base" if root == cfg["srcdir"] else os.path.basename(root))
    out = ccreplay.replay_all(cfg, CC_EVIDENCE_DIR, root, label, jobs=args.jobs or cfg.get("replay_jobs", 4),
                              side_dir=os.path.join(cfg["evidence_out"], "replay-" + label))
    print("replay %s: %d variants, %d ok, %d failed, token-cache sha256 %s; %.1fs" % (
        label, out["n_variants"], out["n_ok"], out["n_failed"], out["token_cache_sha256"], out["elapsed_sec"]))
    for f in out["failed"][:10]:
        print("  FAILED %s: %s" % (f["variant_key"], f["error"].strip().splitlines()[-1] if f["error"].strip() else ""))
    if label == "base":
        # the baseline token cache goes into the repo; the bulky header lists stay outside
        shutil.copy2(out["tokens_path"], os.path.join(CC_EVIDENCE_DIR, "tokens.jsonl"))
        ev.write_json(os.path.join(CC_EVIDENCE_DIR, "replay_base.json"), out)
        print("replay base: wrote cc_evidence/tokens.jsonl and replay_base.json (headers stay in %s)" %
              out["headers_path"])
    if args.compare:
        cmp_ = ccreplay.compare_token_files(os.path.join(CC_EVIDENCE_DIR, "tokens.jsonl"), out["tokens_path"])
        print("compare vs baseline tokens: %d same, %d different, %d missing" % (
            cmp_["same"], cmp_["different"], cmp_["missing"]))
        ev.write_json(os.path.join(EVIDENCE_DIR, "replay_compare_%s.json" % label), cmp_)
        return 0 if (out["n_failed"] == 0 and cmp_["different"] == 0 and cmp_["missing"] == 0) else 1
    return 0 if out["n_failed"] == 0 else 1


def cmd_freeze(cfg, args):
    rc, report = ccreplay.freeze(cfg, EVIDENCE_DIR, CC_EVIDENCE_DIR, verify=args.verify)
    if args.verify:
        verdict = "VERIFIED" if rc == 0 else ("UNVERIFIED" if rc == 2 else "MISMATCH")
    else:
        verdict = "FROZEN" if rc == 0 else ("UNVERIFIED" if rc == 2 else "NOT FROZEN")
    print("freeze%s: %s -- %s" % (" --verify" if args.verify else "", verdict, report.get("reason", "")))
    for p in report.get("problems", []):
        print("  problem: %s" % p)
    for m in report.get("mismatches", []):
        print("  mismatch: %s" % m)
    pc = report.get("platform_candidates")
    if pc:
        print("  platform candidates (report only, %d approved): %d -- %s" % (
            pc["approved"], pc["n_candidates"], ", ".join("%s %d" % kv for kv in pc["by_hint"].items())))
    if args.json:
        ev.write_json(args.json, report)
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("environment")
    p = sub.add_parser("configure")
    p.add_argument("--force", action="store_true")
    p = sub.add_parser("make")
    p.add_argument("name")
    sub.add_parser("check-world")
    sub.add_parser("resummarize", help="re-derive log_summary/results from the already captured logs")
    sub.add_parser("collect")
    p = sub.add_parser("replay")
    p.add_argument("root")
    p.add_argument("--label", default=None)
    p.add_argument("--jobs", type=int, default=None)
    p.add_argument("--compare", action="store_true", help="compare against cc_evidence/tokens.jsonl")
    p = sub.add_parser("freeze")
    p.add_argument("--verify", action="store_true",
                   help="read-only: recompute baseline.json and the platform candidates from the evidence on "
                        "disk and compare with the committed files; write nothing")
    p.add_argument("--json", default=None, help="write the freeze/verify report here")
    args = ap.parse_args(argv)
    try:
        cfg = ev.load_config(args.config)
        fn = {
            "environment": cmd_environment, "configure": cmd_configure, "make": cmd_make,
            "check-world": cmd_check_world, "resummarize": cmd_resummarize, "collect": cmd_collect,
            "replay": cmd_replay, "freeze": cmd_freeze,
        }[args.cmd]
        return fn(cfg, args)
    except ev.EvidenceError as e:
        print("make_evidence: UNVERIFIED: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
