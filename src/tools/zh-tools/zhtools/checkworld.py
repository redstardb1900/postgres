"""
Structured results of ``make -jN -Orecurse -w check-world``.

The log is grouped per recursive make by ``--output-sync=recurse``; the
anchors printed by PostgreSQL's own test recipes
(``# +++ regress check in <dir> +++``, ``# +++ isolation check in ...``,
``# +++ tap check in ...``, ``# +++ regress install-check ...``) delimit
the test sections.  Within a section we record:

* pg_regress (PG16 TAP-style output): ``ok N - name  ms`` /
  ``not ok N - name`` / ``ok N - name # SKIP ...`` and the final
  ``# All N tests passed.`` / ``# M of N tests failed[, K of these
  failures ignored].``
* prove: ``t/NNN_x.pl .. ok|skipped: ...|Dubious``, ``Failed N/M
  subtests``, ``Files=, Tests=``, ``Result: PASS|FAIL|NOTESTS``.

On-disk artefacts are listed too: every ``regression.diffs`` (there must
be none), every ``regression.out``, and the ``tmp_check/log`` directories.
Compiler warnings (``: warning:``) anywhere in the log are counted after
normalising srcdir/builddir prefixes.
"""
import os
import re
import sys

sys.dont_write_bytecode = True

from . import makelog  # noqa: E402

ANCHOR_RX = re.compile(r"^# \+\+\+ (regress|isolation|tap|ecpg|regress install|isolation install|tap install)"
                       r"[ -]?(check|install-check)? in (\S+) \+\+\+$")
REGRESS_RESULT_RX = re.compile(r"^(ok|not ok)\s+(\d+)\s+[-+]\s+(\S+)(.*)$")
REGRESS_SUMMARY_RX = re.compile(r"^# (All (\d+) tests passed\.|(\d+) of (\d+) tests failed\.?"
                                r"(?:, (\d+) of these failures ignored\.)?)$")
PROVE_FILE_RX = re.compile(r"^(t/\S+\.pl)\s*\.+\s*(.*)$")
PROVE_SUMMARY_RX = re.compile(r"^Files=(\d+), Tests=(\d+),")
PROVE_RESULT_RX = re.compile(r"^Result: (PASS|FAIL|NOTESTS)$")
PROVE_FAILED_RX = re.compile(r"^Failed (\d+)/(\d+) subtests")
# prove prints these on their own line when the test's stderr interleaved
# with the "t/NNN.pl ...." progress line, leaving the latter without a status
PROVE_DUBIOUS_RX = re.compile(r"^(Dubious, test returned \d+|No subtests run)\b")


def _norm_warning(line, srcdir, builddir):
    # directory-boundary aware: "/b/" must not match "/backend/"
    for root, tag in sorted(((builddir, "BUILD"), (srcdir, "SRC")), key=lambda x: -len(x[0])):
        root = root.rstrip("/")
        line = line.replace(root + "/", tag + "/")
    return line


def parse(log_path, builddir, srcdir):
    sections = []
    cur = None
    warnings = {}
    make_warnings = {}
    make_errors = []
    cwd = builddir
    stack = []
    n_lines = 0
    for lineno, line in makelog.iter_log(log_path):
        n_lines += 1
        m = makelog.ENTER_RX.match(line)
        if m:
            stack.append(cwd)
            cwd = m.group(2)
            continue
        m = makelog.LEAVE_RX.match(line)
        if m:
            cwd = stack.pop() if stack else builddir
            continue
        if makelog.COMPILER_WARNING_RX.match(line):
            k = _norm_warning(line, srcdir, builddir)
            warnings[k] = warnings.get(k, 0) + 1
        if makelog.MAKE_WARNING_RX.match(line):
            k = makelog.MAKE_WARNING_RX.sub("", line).strip()[:80]
            make_warnings[k] = make_warnings.get(k, 0) + 1
        if makelog.MAKE_ERROR_RX.match(line):
            make_errors.append({"lineno": lineno, "line": line[:300]})
        relcwd = os.path.relpath(cwd, builddir) if cwd.startswith(builddir) else cwd
        m = ANCHOR_RX.match(line)
        if m:
            cur = {"kind": m.group(1), "mode": m.group(2) or "check", "dir": m.group(3), "lineno": lineno,
                   "cwd": relcwd, "anchor": True, "tests": [], "summary": None, "prove": None}
            sections.append(cur)
            continue
        is_regress = REGRESS_RESULT_RX.match(line) is not None
        is_prove = PROVE_FILE_RX.match(line) is not None
        if (is_regress or is_prove) and (cur is None or cur["cwd"] != relcwd):
            # A test run whose recipe prints no "# +++ ... +++" anchor (ecpg's
            # pg_regress in src/interfaces/ecpg/test is the one in PG16).  Never
            # fold it into whatever anchored section happened to come before it.
            cur = {"kind": "regress" if is_regress else "tap", "mode": "check", "dir": relcwd, "lineno": lineno,
                   "cwd": relcwd, "anchor": False, "tests": [], "summary": None, "prove": None}
            sections.append(cur)
        if cur is None:
            continue
        m = REGRESS_RESULT_RX.match(line)
        if m:
            tail = m.group(4)
            status = "ok" if m.group(1) == "ok" else "failed"
            if "# SKIP" in tail:
                status = "skipped"
            elif status == "failed" and "ignored" in tail:
                status = "failed_ignored"
            cur["tests"].append({"n": int(m.group(2)), "name": m.group(3), "status": status})
            continue
        m = REGRESS_SUMMARY_RX.match(line)
        if m:
            if m.group(2):
                cur["summary"] = {"total": int(m.group(2)), "failed": 0, "ignored": 0}
            else:
                cur["summary"] = {"total": int(m.group(4)), "failed": int(m.group(3)),
                                  "ignored": int(m.group(5) or 0)}
            continue
        m = PROVE_FILE_RX.match(line)
        if m:
            rest = m.group(2).strip()
            if rest.startswith("ok"):
                status = "ok"
            elif rest.startswith("skipped"):
                status = "skipped"
            elif rest.startswith("Dubious") or rest.startswith("Failed") or "not ok" in rest:
                status = "failed"
            else:
                status = "unknown:" + rest[:40]
            cur["tests"].append({"name": m.group(1), "status": status, "detail": rest[:200]})
            continue
        m = PROVE_FAILED_RX.match(line) or PROVE_DUBIOUS_RX.match(line)
        if m and cur["tests"]:
            last = cur["tests"][-1]
            if last["status"] != "failed" or not last["detail"]:
                last["status"] = "failed"
                last["detail"] = line[:200]
            else:
                last["detail"] = (last["detail"] + "; " + line)[:200]
            continue
        m = PROVE_SUMMARY_RX.match(line)
        if m:
            cur["prove"] = dict(cur["prove"] or {}, files=int(m.group(1)), tests=int(m.group(2)))
            continue
        m = PROVE_RESULT_RX.match(line)
        if m:
            cur["prove"] = dict(cur["prove"] or {}, result=m.group(1))
            continue
        if line == "All tests successful.":
            cur["prove"] = dict(cur["prove"] or {}, all_successful=True)
            continue

    # on-disk artefacts
    diffs = []
    outs = []
    tap_logs = {}
    for dp, dns, fns in os.walk(builddir):
        dns[:] = [d for d in dns if d != "tmp_install"]
        rel = os.path.relpath(dp, builddir)
        if "regression.diffs" in fns:
            p = os.path.join(dp, "regression.diffs")
            from . import evidence_run as ev
            diffs.append({"path": os.path.relpath(p, builddir), "size": os.path.getsize(p),
                          "sha256": ev.sha256_file(p)})
        if "regression.out" in fns:
            outs.append(os.path.relpath(os.path.join(dp, "regression.out"), builddir))
        if rel.endswith("tmp_check/log") or rel.endswith("tmp_check" + os.sep + "log"):
            tap_logs[rel] = sorted(fns)

    regress = [s for s in sections if s["kind"] in ("regress", "isolation", "ecpg", "regress install",
                                                     "isolation install")]
    tap = [s for s in sections if s["kind"].startswith("tap")]
    summary = {
        "sections": len(sections),
        "sections_without_anchor": sum(1 for s in sections if not s["anchor"]),
        "regress_suites": len(regress),
        "regress_suites_without_summary": sum(1 for s in regress if s["summary"] is None),
        "regress_tests": sum(len(s["tests"]) for s in regress),
        "regress_failed_tests": sum(1 for s in regress for t in s["tests"] if t["status"] == "failed"),
        "regress_ignored_failures": sum(1 for s in regress for t in s["tests"] if t["status"] == "failed_ignored"),
        "regress_skipped_tests": sum(1 for s in regress for t in s["tests"] if t["status"] == "skipped"),
        "regress_summary_failed": sum((s["summary"] or {}).get("failed", 0) for s in regress),
        "tap_suites": len(tap),
        "tap_files": sum(len(s["tests"]) for s in tap),
        "tap_failed_files": sum(1 for s in tap for t in s["tests"] if t["status"] == "failed"),
        "tap_skipped_files": sum(1 for s in tap for t in s["tests"] if t["status"] == "skipped"),
        "tap_unknown_files": sum(1 for s in tap for t in s["tests"] if t["status"].startswith("unknown")),
        "tap_results": {r: sum(1 for s in tap if (s["prove"] or {}).get("result") == r)
                        for r in ("PASS", "FAIL", "NOTESTS")},
        "tap_suites_without_result": sum(1 for s in tap if not (s["prove"] or {}).get("result")),
        "compiler_warnings": sum(warnings.values()),
        "compiler_warning_kinds": len(warnings),
        "make_warnings": sum(make_warnings.values()),
        "make_errors": len(make_errors),
        "regression_diffs_files": len(diffs),
        "regression_out_files": len(outs),
        "tap_log_dirs": len(tap_logs),
        "log_lines": n_lines,
    }
    return {
        "summary": summary,
        "sections": sections,
        "warnings": dict(sorted(warnings.items())),
        "make_warnings": dict(sorted(make_warnings.items())),
        "make_errors": make_errors,
        "regression_diffs": diffs,
        "regression_out": sorted(outs),
        "tap_log_dirs": tap_logs,
    }
