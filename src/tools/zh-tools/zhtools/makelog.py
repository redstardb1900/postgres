"""
GNU Make ``-j1 -w`` log parser and .o/.Po/argv closure (plan: --cc-check evidence).

Input: the complete, non-interleaved logs of ``make -j1 -w world-bin``,
``make -j1 -w checkprep`` and the other capture targets listed in
``frozen_config.json``.  Output: one record per *translation-unit variant*
(``variant_key`` = builddir-relative object path, never de-duplicated),
with the original cwd and the complete argv, plus the three-way closure
against the ``.o`` files and ``.deps/*.Po`` files that actually exist in
the builddir.

Rules (all violations -> "unverified", never relaxed):

* cwd is tracked from ``make[N]: Entering directory '...'`` /
  ``Leaving directory`` lines (a stack; ``-w`` is mandatory);
* a line is a compiler invocation when its first shlex token is exactly
  the frozen ``cc`` or ``cxx`` absolute path;
* a compiler invocation must be a single simple command: no ``&&``,
  ``||``, ``;``, ``|``, ``<``, ``>``, ``>>``, ``2>``, backticks, ``$(``,
  response files (``@file``), ``ccache``/``distcc``/``sccache`` or any
  other wrapper in front of the compiler.  Any such line that mentions
  the compiler path anywhere is an UNVERIFIED finding, not a skip;
* a compile command has exactly one ``-c`` and exactly one source
  operand (``.c``/``.cpp``/``.cc``/``.cxx``/``.C``); ``-o`` is required
  (PostgreSQL always passes it);  link lines (no ``-c``) are ignored;
* the same object path produced twice in the logs is UNVERIFIED unless
  the argv is identical (re-running a capture target legitimately
  repeats nothing because make is up to date).
"""
import json
import os
import re
import shlex
import sys

sys.dont_write_bytecode = True

ENTER_RX = re.compile(r"^make(?:\[(\d+)\])?: Entering directory '(.*)'$")
LEAVE_RX = re.compile(r"^make(?:\[(\d+)\])?: Leaving directory '(.*)'$")
SHELL_META_TOKENS = ("&&", "||", ";", "|", "<", ">", ">>", "2>", "2>&1", "1>", "&")
WRAPPERS = ("ccache", "distcc", "sccache", "icecc", "env", "nice", "time", "sh", "bash", "/bin/sh", "/bin/bash")
SOURCE_EXTS = (".c", ".cpp", ".cc", ".cxx", ".C")
SOURCE_RX = re.compile(r"\.(c|cpp|cc|cxx|C)$")
# compiler diagnostics look like  file.c:12:3: warning: ...  (column optional)
COMPILER_WARNING_RX = re.compile(r"^\S+:\d+(?::\d+)?: warning: ")
COMPILER_ERROR_RX = re.compile(r"^\S+:\d+(?::\d+)?: (?:fatal )?error: |^\S+: error: ")
MAKE_WARNING_RX = re.compile(r"^make(?:\[\d+\])?: [Ww]arning: ")
MAKE_ERROR_RX = re.compile(r"^make(?:\[\d+\])?: \*\*\* ")


class LogParseError(Exception):
    pass


def _has_shell_meta(line, tokens):
    """Detect shell control operators in the raw line / parsed tokens."""
    for t in tokens:
        if t in SHELL_META_TOKENS:
            return "shell operator %r" % t
        if t.startswith("@") and len(t) > 1 and not t.startswith("@@"):
            return "response file %r" % t
    # Operators glued to other tokens (e.g. "foo;bar", "2>/dev/null") are
    # visible in the raw line outside quotes; a conservative raw scan:
    stripped = _strip_quotes(line)
    for op in ("&&", "||", ";", "|", "`", "$(", ">", "<"):
        if op in stripped:
            return "shell operator %r in raw line" % op
    return None


def _strip_quotes(line):
    """Remove single- and double-quoted segments (POSIX rules, approx.)."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == "'":
            j = line.find("'", i + 1)
            if j < 0:
                break
            i = j + 1
            continue
        if c == '"':
            j = i + 1
            while j < n:
                if line[j] == "\\":
                    j += 2
                    continue
                if line[j] == '"':
                    break
                j += 1
            i = j + 1
            continue
        if c == "\\" and i + 1 < n:
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


FOREIGN_OBJECT_TOOLS = ("dtrace",)


def classify_line(line, compilers):
    """Return ("compile", info) | ("link", None) | ("foreign", info) | ("other", None)
    or raise LogParseError.

    *compilers* is the tuple of frozen absolute compiler paths.  "foreign" is
    an object file produced by a non-compiler tool we know about (dtrace -G);
    it is recorded so that the closure can account for the .o without
    pretending it is a C translation unit.
    """
    if not line or line[0] in " \t":
        return "other", None
    first = line.split(None, 1)[0]
    mentions = any(c in line for c in compilers)
    if first not in compilers and os.path.basename(first) in FOREIGN_OBJECT_TOOLS and not mentions:
        try:
            tokens = shlex.split(line, posix=True)
        except ValueError as e:
            raise LogParseError("shlex failed (%s): %s" % (e, line[:200]))
        if _has_shell_meta(line, tokens):
            return "other", None
        if "-G" in tokens and "-o" in tokens and tokens.index("-o") + 1 < len(tokens):
            return "foreign", {"tool": os.path.basename(first), "tokens": tokens,
                               "object": tokens[tokens.index("-o") + 1]}
        return "other", None
    if first not in compilers:
        if mentions:
            # the compiler path appears somewhere else: maybe a wrapper, a
            # `cd x && gcc`, or just a diagnostic like "/usr/bin/gcc: error".
            if first.rstrip(":") in compilers:
                return "other", None  # a diagnostic line, not a command
            stripped = _strip_quotes(line)
            if any(c in stripped for c in compilers):
                raise LogParseError("compiler path inside a non-simple command: %s" % line[:200])
        return "other", None
    try:
        tokens = shlex.split(line, posix=True)
    except ValueError as e:
        raise LogParseError("shlex failed (%s): %s" % (e, line[:200]))
    if not tokens or tokens[0] not in compilers:
        return "other", None
    meta = _has_shell_meta(line, tokens)
    if meta:
        raise LogParseError("%s: %s" % (meta, line[:200]))
    for t in tokens[1:]:
        if os.path.basename(t) in ("ccache", "distcc", "sccache", "icecc"):
            raise LogParseError("compiler wrapper %r: %s" % (t, line[:200]))
    if "-c" not in tokens:
        if "-E" in tokens or "-S" in tokens:
            raise LogParseError("unexpected -E/-S compiler invocation in build log: %s" % line[:200])
        return "link", None
    if tokens.count("-c") != 1:
        raise LogParseError("multiple -c: %s" % line[:200])
    # output and source
    obj = None
    sources = []
    i = 1
    while i < len(tokens):
        t = tokens[i]
        if t == "-o":
            if i + 1 >= len(tokens):
                raise LogParseError("dangling -o: %s" % line[:200])
            if obj is not None:
                raise LogParseError("multiple -o: %s" % line[:200])
            obj = tokens[i + 1]
            i += 2
            continue
        if t.startswith("-o") and len(t) > 2 and not t.startswith("-objc"):
            raise LogParseError("joined -o form not accepted: %s" % line[:200])
        if t == "-x":
            raise LogParseError("-x language override not accepted: %s" % line[:200])
        if t in ("-include", "-imacros", "-I", "-iquote", "-isystem", "-idirafter", "-MF", "-MT", "-MQ",
                 "-D", "-U", "-isysroot", "--sysroot", "-L", "-l", "-specs"):
            i += 2
            continue
        if not t.startswith("-") and SOURCE_RX.search(t):
            sources.append(t)
        i += 1
    if len(sources) != 1:
        raise LogParseError("expected exactly one source operand, got %r: %s" % (sources, line[:200]))
    if obj is None:
        raise LogParseError("no -o in compile command: %s" % line[:200])
    return "compile", {"tokens": tokens, "source": sources[0], "object": obj,
                       "lang": "c++" if tokens[0] == compilers[1] else "c"}


def iter_log(path):
    """Yield (first_physical_lineno, logical_line).

    Make echoes recipes verbatim, so a recipe line that the shell would
    continue (trailing backslash-newline) appears as several physical
    lines; they are joined here so that ``cd x && \\`` + ``gcc ...`` is
    seen as one (non-simple) command and cannot masquerade as a compile.
    """
    with open(path, "rb") as f:
        pending = None
        pending_no = 0
        for lineno, raw in enumerate(f, 1):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if pending is not None:
                line = pending + "\n" + line
            else:
                pending_no = lineno
            if line.endswith("\\") and not line.endswith("\\\\"):
                pending = line[:-1]
                continue
            pending = None
            yield pending_no, line
        if pending is not None:
            yield pending_no, pending


def parse_log(path, compilers, builddir):
    """Parse one -j1 -w log. Returns (commands, problems, stats).

    commands: list of dicts {lineno, cwd, tokens, source, object, lang}
    problems: list of strings (every one is UNVERIFIED)
    """
    commands = []
    foreign = []
    problems = []
    stack = []
    cwd = builddir
    stats = {"lines": 0, "enter": 0, "leave": 0, "link": 0, "compile": 0, "foreign": 0}
    for lineno, line in iter_log(path):
        stats["lines"] += 1
        m = ENTER_RX.match(line)
        if m:
            stack.append(cwd)
            cwd = m.group(2)
            stats["enter"] += 1
            continue
        m = LEAVE_RX.match(line)
        if m:
            if m.group(2) != cwd:
                problems.append("%s:%d: Leaving directory %r but current cwd is %r" % (path, lineno, m.group(2), cwd))
            cwd = stack.pop() if stack else builddir
            stats["leave"] += 1
            continue
        try:
            kind, info = classify_line(line, compilers)
        except LogParseError as e:
            problems.append("%s:%d: %s" % (path, lineno, e))
            continue
        if kind == "link":
            stats["link"] += 1
        elif kind == "compile":
            stats["compile"] += 1
            info["lineno"] = lineno
            info["cwd"] = cwd
            commands.append(info)
        elif kind == "foreign":
            stats["foreign"] += 1
            info["lineno"] = lineno
            info["cwd"] = cwd
            foreign.append(info)
    if stack:
        problems.append("%s: %d unmatched 'Entering directory' at EOF" % (path, len(stack)))
    if stats["enter"] == 0:
        problems.append("%s: no 'Entering directory' lines (was make run with -w?)" % path)
    stats["foreign_objects"] = foreign
    return commands, problems, stats


def summarize_log(path, compilers):
    """Quick structural summary used right after a capture run."""
    stats = {"lines": 0, "enter": 0, "leave": 0, "compile_commands": 0, "link_commands": 0, "problems": 0,
             "compiler_warnings": 0, "compiler_errors": 0, "make_warnings": 0, "make_errors": 0,
             "compiler_warning_lines": [], "make_warning_kinds": {}}
    for lineno, line in iter_log(path):
        stats["lines"] += 1
        if ENTER_RX.match(line):
            stats["enter"] += 1
            continue
        if LEAVE_RX.match(line):
            stats["leave"] += 1
            continue
        if COMPILER_WARNING_RX.match(line):
            stats["compiler_warnings"] += 1
            if len(stats["compiler_warning_lines"]) < 200:
                stats["compiler_warning_lines"].append({"lineno": lineno, "line": line[:300]})
        if COMPILER_ERROR_RX.match(line):
            stats["compiler_errors"] += 1
        if MAKE_WARNING_RX.match(line):
            stats["make_warnings"] += 1
            k = MAKE_WARNING_RX.sub("", line).strip()[:80]
            stats["make_warning_kinds"][k] = stats["make_warning_kinds"].get(k, 0) + 1
        if MAKE_ERROR_RX.match(line):
            stats["make_errors"] += 1
        try:
            kind, _ = classify_line(line, compilers)
        except LogParseError:
            stats["problems"] += 1
            continue
        if kind == "compile":
            stats["compile_commands"] += 1
        elif kind == "link":
            stats["link_commands"] += 1
    return stats


# ---------------------------------------------------------------------------
# .Po depfiles
# ---------------------------------------------------------------------------

def parse_depfile(text):
    """Parse a Make dependency file; return list of (target, [prereqs])."""
    # join continuation lines
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    rules = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            continue
        # a target may contain a drive-letter-free colon only as separator
        tgt, _, rest = line.partition(":")
        tgt = tgt.strip()
        prereqs = rest.split()
        rules.append((tgt, prereqs))
    return rules


def read_po(path):
    with open(path, "rb") as f:
        return parse_depfile(f.read().decode("utf-8", "replace"))


def po_first_source(rules):
    """Return (object_target, first_source_prereq) of the main rule, or (None, None)."""
    for tgt, prereqs in rules:
        if prereqs and tgt.endswith(".o"):
            return tgt, prereqs[0]
    return None, None


def po_headers(rules):
    """All prerequisites of the main .o rule that are not the first source."""
    out = []
    for tgt, prereqs in rules:
        if tgt.endswith(".o") and prereqs:
            out.extend(prereqs[1:])
            break
    return out


# ---------------------------------------------------------------------------
# path helpers
# ---------------------------------------------------------------------------

def norm_join(cwd, p):
    if os.path.isabs(p):
        return os.path.normpath(p)
    return os.path.normpath(os.path.join(cwd, p))


def rel_under(path, root):
    """Return path relative to root if path is under root, else None."""
    root = root.rstrip("/") + "/"
    if path == root.rstrip("/"):
        return ""
    if path.startswith(root):
        return path[len(root):]
    return None


def classify_path(abs_path, srcdir, builddir):
    """('src', rel) | ('build', rel) | ('sys', abs)."""
    r = rel_under(abs_path, builddir)
    if r is not None:
        return "build", r
    r = rel_under(abs_path, srcdir)
    if r is not None:
        return "src", r
    return "sys", abs_path


# ---------------------------------------------------------------------------
# collect: logs -> variants + closure
# ---------------------------------------------------------------------------

def find_objects(builddir):
    """All .o files under builddir except tmp_install/ and tmp_check/.

    Returns (regular_objects, symlinks) where symlinks maps the link's
    builddir-relative path to its resolved builddir-relative target.
    """
    out = []
    links = {}
    for dp, dns, fns in os.walk(builddir):
        dns[:] = [d for d in dns if d not in ("tmp_install", "tmp_check") and not d.startswith(".git")]
        for fn in fns:
            if fn.endswith(".o"):
                full = os.path.join(dp, fn)
                rel = os.path.relpath(full, builddir)
                if os.path.islink(full):
                    target = os.path.normpath(os.path.join(dp, os.readlink(full)))
                    links[rel] = os.path.relpath(target, builddir)
                else:
                    out.append(rel)
    return sorted(out), links


def find_po_files(builddir):
    out = []
    for dp, dns, fns in os.walk(builddir):
        dns[:] = [d for d in dns if d not in ("tmp_install", "tmp_check")]
        if os.path.basename(dp) == ".deps":
            for fn in fns:
                if fn.endswith(".Po"):
                    out.append(os.path.relpath(os.path.join(dp, fn), builddir))
    return sorted(out)


def po_path_for_object(obj_rel):
    d, fn = os.path.split(obj_rel)
    return os.path.join(d, ".deps", fn[:-2] + ".Po")


GENERATED_NAMES = ("gram.c", "scan.c", "pl_gram.c", "preproc.c", "pgc.c", "sql_help.c", "fmgrtab.c",
                   "bootparse.c", "bootscanner.c", "repl_gram.c", "repl_scanner.c", "syncrep_gram.c",
                   "syncrep_scanner.c", "jsonpath_gram.c", "jsonpath_scan.c", "guc-file.c", "cubeparse.c",
                   "cubescan.c", "segparse.c", "segscan.c", "exprparse.c", "exprscan.c", "psqlscan.c",
                   "psqlscanslash.c", "specparse.c", "specscan.c")


def collect(logs, cfg):
    """logs: list of (name, path, sha256). Returns the full collect result."""
    compilers = (cfg["cc"], cfg["cxx"])
    srcdir = os.path.realpath(cfg["srcdir"])
    builddir = os.path.realpath(cfg["builddir"])
    unverified = []
    all_cmds = []
    foreign_objects = {}
    log_stats = {}
    for name, path, sha in logs:
        cmds, problems, stats = parse_log(path, compilers, builddir)
        unverified.extend(problems)
        for c in cmds:
            c["log"] = name
        all_cmds.extend(cmds)
        for fo in stats.pop("foreign_objects"):
            obj_abs = norm_join(fo["cwd"], fo["object"])
            kind, rel = classify_path(obj_abs, srcdir, builddir)
            if kind == "build":
                foreign_objects[rel] = {"tool": fo["tool"], "argv": fo["tokens"], "log": name, "lineno": fo["lineno"],
                                        "cwd": rel_under(os.path.normpath(fo["cwd"]), builddir)}
        log_stats[name] = dict(stats, sha256=sha, path=path)

    # variants keyed by builddir-relative object path
    variants = {}
    dup_identical = 0
    for c in all_cmds:
        obj_abs = norm_join(c["cwd"], c["object"])
        kind, rel = classify_path(obj_abs, srcdir, builddir)
        if kind != "build":
            unverified.append("%s:%d: object %s is not under builddir" % (c["log"], c["lineno"], obj_abs))
            continue
        src_abs = norm_join(c["cwd"], c["source"])
        skind, srel = classify_path(src_abs, srcdir, builddir)
        cwd_kind, cwd_rel = classify_path(os.path.normpath(c["cwd"]), srcdir, builddir)
        if cwd_kind != "build":
            unverified.append("%s:%d: cwd %s is not under builddir" % (c["log"], c["lineno"], c["cwd"]))
            continue
        rec = {
            "variant_key": rel,
            "object": rel,
            "cwd": cwd_rel,
            "argv": c["tokens"],
            "lang": c["lang"],
            "source_kind": skind,            # src | build | sys
            "source": srel if skind != "sys" else src_abs,
            "generated": skind == "build",
            "log": c["log"],
            "lineno": c["lineno"],
        }
        if rel in variants:
            if variants[rel]["argv"] == c["tokens"] and variants[rel]["cwd"] == cwd_rel:
                dup_identical += 1
                continue
            unverified.append("object %s compiled twice with different argv/cwd (%s:%d vs %s:%d)" % (
                rel, variants[rel]["log"], variants[rel]["lineno"], c["log"], c["lineno"]))
            continue
        variants[rel] = rec

    # closure against the builddir
    objects, aliases = find_objects(builddir)
    po_files = find_po_files(builddir)
    po_set = set(po_files)
    obj_set = set(objects) | set(aliases)
    # symlinked objects (src/test/isolation/pg_regress.o -> ../regress/pg_regress.o)
    # are accounted for by their target variant
    alias_ok = {}
    alias_bad = []
    for a, target in sorted(aliases.items()):
        if target in variants:
            alias_ok[a] = target
        else:
            alias_bad.append("%s -> %s (target is not a known variant)" % (a, target))
    unverified.extend(alias_bad)
    # objects produced by a known non-compiler tool (dtrace -G)
    foreign_ok = {o: f for o, f in foreign_objects.items() if o in obj_set and o not in variants}
    objects_without_argv = sorted(o for o in objects if o not in variants and o not in foreign_ok)
    argv_without_object = sorted(v for v in variants if v not in obj_set)

    # .Po assignment.  Make writes .deps/X.Po only from the generic %.o: %.c
    # rule.  The explicit rules  %_shlib.o: %.c %.o  and  %_srv.o: %.c %.o
    # (src/common, src/port) compile the same source again without -MMD and
    # instead depend on the base object, whose .Po therefore is the
    # Make-declared dependency file of the variant.  Inheriting is allowed
    # only in exactly that shape (same directory, same source, base .Po
    # present); anything else stays unclosed.
    po_inherited = {}
    for key, v in variants.items():
        po_rel = po_path_for_object(key)
        if po_rel in po_set:
            v["po_file"] = po_rel
            continue
        d, fn = os.path.split(key)
        base = None
        for suffix in ("_shlib.o", "_srv.o"):
            if fn.endswith(suffix):
                base = os.path.join(d, fn[:-len(suffix)] + ".o")
        if base and base in variants and variants[base]["source_kind"] == v["source_kind"] and \
                variants[base]["source"] == v["source"] and po_path_for_object(base) in po_set:
            v["po_file"] = po_path_for_object(base)
            v["po_inherited_from"] = base
            po_inherited[key] = base
        else:
            v["po_file"] = None
    objects_without_po = sorted(o for o in objects if po_path_for_object(o) not in po_set
                                and o not in po_inherited and o not in foreign_ok)
    po_without_object = sorted(p for p in po_files if _object_for_po(p) not in obj_set)

    # .Po consistency: first prerequisite must be the compiled source
    po_mismatch = []
    for key, v in variants.items():
        po_rel = v.get("po_file")
        if not po_rel:
            continue
        rules = read_po(os.path.join(builddir, po_rel))
        tgt, first = po_first_source(rules)
        if first is None:
            po_mismatch.append("%s: no .o rule with prerequisites" % po_rel)
            continue
        first_abs = norm_join(os.path.join(builddir, v["cwd"]), first)
        src_abs = os.path.join(srcdir if v["source_kind"] == "src" else builddir, v["source"]) \
            if v["source_kind"] != "sys" else v["source"]
        if os.path.realpath(first_abs) != os.path.realpath(src_abs):
            po_mismatch.append("%s: first prerequisite %s != compiled source %s" % (po_rel, first_abs, src_abs))
        hdrs = []
        for h in po_headers(rules):
            h_abs = norm_join(os.path.join(builddir, v["cwd"]), h)
            k, r = classify_path(h_abs, srcdir, builddir)
            if k == "src":
                hdrs.append("SRC/" + r)
            elif k == "build":
                hdrs.append("BUILD/" + r)
            else:
                hdrs.append(h_abs)
        v["po_headers"] = sorted(set(hdrs))
    unverified.extend(po_mismatch)

    closed = not (objects_without_argv or argv_without_object or objects_without_po or po_without_object
                  or alias_bad)
    sources = set((v["source_kind"], v["source"]) for v in variants.values())
    flagsets, compact = _compact_variants(variants)
    return {
        "schema_version": 2,
        "logs": log_stats,
        "n_compile_commands": len(all_cmds),
        "n_duplicate_identical": dup_identical,
        "n_variants": len(variants),
        "n_sources": len(sources),
        "n_generated_variants": sum(1 for v in variants.values() if v["generated"]),
        "variants": variants,
        "flagsets": flagsets,
        "compact": compact,
        "closure": {
            "closed": closed,
            "n_objects": len(objects),
            "n_object_symlinks": len(aliases),
            "n_po_files": len(po_files),
            "n_po_inherited": len(po_inherited),
            "objects_without_argv": objects_without_argv,
            "argv_without_object": argv_without_object,
            "objects_without_po": objects_without_po,
            "po_without_object": po_without_object,
            "po_mismatch": po_mismatch,
            "po_inherited": po_inherited,
            "object_symlinks": alias_ok,
            "object_symlinks_bad": alias_bad,
            "foreign_objects": foreign_ok,
        },
        "unverified": unverified,
    }


def _object_for_po(po_rel):
    d = os.path.dirname(os.path.dirname(po_rel))
    fn = os.path.basename(po_rel)[:-3] + ".o"
    return os.path.join(d, fn)


def _compact_variants(variants):
    """Factor argv into (flagset, source, object) so commands.jsonl stays small.

    The flagset is the argv with the source operand replaced by "{SRC}",
    the -o operand by "{OBJ}" and the -MF operand (if any) by "{DEP}".
    Reconstruction is exact.
    """
    flagsets = {}
    compact = []
    for key in sorted(variants):
        v = variants[key]
        argv = v["argv"]
        tmpl = list(argv)
        # source operand is the single token equal to the recorded source spelling
        src_tok = [i for i, t in enumerate(argv) if i > 0 and not t.startswith("-") and SOURCE_RX.search(t)
                   and argv[i - 1] not in ("-o", "-MF", "-MT", "-MQ", "-include", "-imacros")]
        oi = argv.index("-o") + 1
        assert len(src_tok) == 1, (key, src_tok)
        tmpl[src_tok[0]] = "{SRC}"
        tmpl[oi] = "{OBJ}"
        dep_tok = None
        if "-MF" in argv:
            di = argv.index("-MF") + 1
            dep_tok = argv[di]
            tmpl[di] = "{DEP}"
        tkey = json.dumps(tmpl)
        fid = flagsets.setdefault(tkey, len(flagsets))
        rec = {
            "variant_key": key, "cwd": v["cwd"], "flagset": fid,
            "source_tok": argv[src_tok[0]], "object_tok": argv[oi], "dep_tok": dep_tok,
            "source_kind": v["source_kind"], "source": v["source"], "lang": v["lang"],
            "generated": v["generated"], "log": v["log"], "po_file": v.get("po_file"),
        }
        if v.get("po_inherited_from"):
            rec["po_inherited_from"] = v["po_inherited_from"]
        compact.append(rec)
    fs = [json.loads(k) for k, _ in sorted(flagsets.items(), key=lambda kv: kv[1])]
    return fs, compact


def expand_argv(flagsets, rec):
    out = []
    for t in flagsets[rec["flagset"]]:
        if t == "{SRC}":
            out.append(rec["source_tok"])
        elif t == "{OBJ}":
            out.append(rec["object_tok"])
        elif t == "{DEP}":
            out.append(rec["dep_tok"])
        else:
            out.append(t)
    return out


def write_collect_outputs(result, cc_dir, bulk_dir):
    """Write the in-repo evidence (cc_dir) and the bulky per-variant header
    lists (bulk_dir, outside the repo; SHA-256 recorded in closure.json)."""
    from . import evidence_run as ev
    os.makedirs(cc_dir, exist_ok=True)
    os.makedirs(bulk_dir, exist_ok=True)
    with open(os.path.join(cc_dir, "flagsets.json"), "w", encoding="utf-8") as f:
        json.dump({"schema_version": 2, "flagsets": result["flagsets"]}, f, indent=0)
        f.write("\n")
    with open(os.path.join(cc_dir, "commands.jsonl"), "w", encoding="utf-8") as f:
        for rec in result["compact"]:
            f.write(json.dumps(rec, sort_keys=True) + "\n")
    # per-variant project header sets from the Make .Po files (audit superset)
    po_path = os.path.join(bulk_dir, "po_headers.jsonl")
    with open(po_path, "w", encoding="utf-8") as f:
        for key in sorted(result["variants"]):
            v = result["variants"][key]
            f.write(json.dumps({"variant_key": key, "po_headers": v.get("po_headers", [])}) + "\n")
    closure = dict(result["closure"])
    # foreign-object argv can be huge (dtrace -G lists every backend .o); keep the head only
    for fo in closure.get("foreign_objects", {}).values():
        argv = fo.pop("argv")
        fo["argv_head"] = argv[:6]
        fo["n_argv"] = len(argv)
    closure.update({
        "bulk": {"po_headers": {"path": po_path, "sha256": ev.sha256_file(po_path),
                                "size": os.path.getsize(po_path)}},
        "schema_version": 2,
        "logs": result["logs"],
        "n_compile_commands": result["n_compile_commands"],
        "n_duplicate_identical": result["n_duplicate_identical"],
        "n_variants": result["n_variants"],
        "n_sources": result["n_sources"],
        "n_generated_variants": result["n_generated_variants"],
        "n_flagsets": len(result["flagsets"]),
        "unverified": result["unverified"],
    })
    with open(os.path.join(cc_dir, "closure.json"), "w", encoding="utf-8") as f:
        json.dump(closure, f, indent=1, sort_keys=True)
        f.write("\n")


def read_commands(cc_dir):
    with open(os.path.join(cc_dir, "flagsets.json"), "r", encoding="utf-8") as f:
        flagsets = json.load(f)["flagsets"]
    recs = []
    with open(os.path.join(cc_dir, "commands.jsonl"), "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return flagsets, recs
