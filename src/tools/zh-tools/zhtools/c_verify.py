"""
Comment-only change verification for one (old, new) pair of C/C++ files.

``compare_files(old_bytes, new_bytes, strict_lines=True)`` returns a
``FileResult`` with a list of violations (fatal), warnings (advisory) and
some statistics.  Every rule has a stable identifier so that tests and
reports can refer to it:

  R1_SKELETON          non-comment bytes or comment positions changed
  R2_KIND              a block comment became a line comment or vice versa
  R3_EARLY_CLOSE       new block comment body contains ``*/``
  R3_NESTED_OPEN       new block comment body adds a nested ``/*``
  R3_LINE_BACKSLASH    new ``//`` comment ends with a backslash
  R4_UTF8              new file is not valid UTF-8
  R4_BOM               new file starts with a UTF-8 BOM
  R4_CR                carriage returns inside a comment changed
  R5_PROTECTED         a protected comment (translator:, fall through,
                       pg_attribute_*) was modified, OR a Copyright /
                       IDENTIFICATION / licence *line* in a file-header
                       comment was modified (module description and NOTES
                       may change, even between Copyright and IDENTIFICATION)
  R6_LF_COUNT          number of LF bytes in a comment changed
                       (advisory W6_LF_COUNT with --no-strict-lines)
  R8_TRIGRAPH_NEW      new comment introduces a trigraph
  R8_TRIGRAPH_BASE     comment that contained a trigraph in the baseline
                       was modified
  R9_PARSE             lexer reported a hard problem (exit code 2)
  R9_FILE              file added / deleted / unreadable (exit code 1)

  W7_UNTRANSLATED      comment unchanged and still plain ASCII prose
  W7_IDENT_LOST        code identifier present in old comment is missing
  W9_LEX               lexer warning (unterminated literal etc.)
"""
import re
import sys

sys.dont_write_bytecode = True

from . import c_lexer  # noqa: E402

# --- fully protected comments (any edit is R5; checked on OLD text) -------
_PROTECT_RULES = (
    ("translator", re.compile(rb"translator\s*:", re.I)),
    # GCC -Wimplicit-fallthrough=3 accepts many spellings; be generous.
    ("fallthrough", re.compile(rb"fall[\s\-_]*s?[\s\-_]*thr(ough|u)", re.I)),
    ("pg_attribute", re.compile(rb"pg_attribute_")),
)

# Legal/meta *lines* inside a comment must remain byte-identical substrings
# of the new comment.  Module description and NOTES — including text that
# sits between Copyright and IDENTIFICATION — are translatable.
# ``/*-`` alone is NOT a protect reason (pgindent "do not reformat" marker).
_LEGAL_LINE_RE = re.compile(
    rb"copyright|\(c\)\s*\d{4}|permission (is hereby granted|to use)"
    rb"|redistribution and use"
    rb"|licen[cs]e|this software is provided|as is and any express"
    rb"|in no event shall|procur(?:e|ement) of substitute"
    rb"|^[ \t]*\*[ \t]*IDENTIFICATION[ \t]*$",
    re.I | re.M,
)

_IDENT_RE = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*")
_HAS_LETTERS = re.compile(rb"[A-Za-z]{3,}")


class Finding(object):
    __slots__ = ("rule", "message", "line", "comment_index", "context")

    def __init__(self, rule, message, line=None, comment_index=None, context=None):
        self.rule = rule
        self.message = message
        self.line = line
        self.comment_index = comment_index
        self.context = context

    def to_dict(self):
        d = {"rule": self.rule, "message": self.message}
        if self.line is not None:
            d["line"] = self.line
        if self.comment_index is not None:
            d["comment"] = self.comment_index
        if self.context:
            d["context"] = self.context
        return d


class FileResult(object):
    def __init__(self, path):
        self.path = path
        self.violations = []
        self.warnings = []
        self.parse_error = False
        self.stats = {}

    @property
    def ok(self):
        return not self.violations and not self.parse_error

    def status(self):
        if self.parse_error:
            return "parse_error"
        if self.violations:
            return "violation"
        return "ok"

    def to_dict(self, max_findings=None):
        v = self.violations
        w = self.warnings
        if max_findings is not None:
            v = v[:max_findings]
            w = w[:max_findings]
        return {
            "path": self.path,
            "status": self.status(),
            "violations": [f.to_dict() for f in v],
            "warnings": [f.to_dict() for f in w],
            "n_violations": len(self.violations),
            "n_warnings": len(self.warnings),
            "stats": self.stats,
        }


# WaitIO "will fall through" is prose: the waiter returns immediately because
# StartBufferIO has not begun.  It is not a switch annotation, and GCC
# -Wimplicit-fallthrough does not consult it.  Real fall-through comments stay
# protected.
_WAITIO_FALLTHROUGH_PROSE = re.compile(rb"WaitIO will fall through")

# fsm_readbuf "possible to fall through and return the buffer" is prose: the
# unlocked PageIsNew check can skip PageInit and hand back a buffer another
# backend is still initializing.  Not a switch annotation.
_FSM_FALLTHROUGH_PROSE = re.compile(
    rb"possible to fall through and return the buffer")

# FileFallocate "fall through to the FileZero() backed implementation" is
# prose: a real posix_fallocate failure returns immediately, and EINVAL /
# EOPNOTSUPP continues into FileZero().  Not a switch annotation, and GCC
# -Wimplicit-fallthrough does not consult it.
_FILEZERO_FALLTHROUGH_PROSE = re.compile(
    rb"fall through to the FileZero\(\) backed implementation")

# toast_internals "fall through without writing the data again" is prose
# about skipping a second write when reusing a toast OID during rewrite,
# not a switch fall-through annotation.
_TOAST_REWRITE_FALLTHROUGH_PROSE = re.compile(
    rb"fall through without writing the data again")


def legal_protected_segments(old_text):
    """Return Copyright/IDENTIFICATION/licence line blobs that must survive.

    Each segment is one or more consecutive lines that must appear as a
    contiguous substring of the new comment.  NOTES and other module prose
    between Copyright and IDENTIFICATION are intentionally *not* included.
    """
    if not _LEGAL_LINE_RE.search(old_text):
        return []
    lines = old_text.splitlines(True)
    segs = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not _LEGAL_LINE_RE.search(line):
            i += 1
            continue
        chunk = [line]
        if re.search(rb"IDENTIFICATION", line, re.I):
            j = i + 1
            while j < n:
                body = lines[j].lstrip().lstrip(b"*").strip()
                if not body or body.startswith(b"-"):
                    break
                if re.match(rb"NOTES\b", body, re.I):
                    break
                chunk.append(lines[j])
                j += 1
                break  # path line only
            segs.append(b"".join(chunk))
            i = j
            continue
        # Multi-line quoted licence terms (e.g. rbtree.c Thomas Niemann).
        j = i + 1
        while j < n and b"".join(chunk).count(b'"') % 2 == 1:
            chunk.append(lines[j])
            j += 1
        segs.append(b"".join(chunk))
        i = j
    return segs


def legal_protected_tail(old_text):
    """Compatibility helper: join of ``legal_protected_segments``, or None."""
    segs = legal_protected_segments(old_text)
    if not segs:
        return None
    return b"".join(segs)


def protected_reason(old_text):
    """Return a full-protect reason, or None if the comment may be edited.

    Copyright/IDENTIFICATION lines are *not* reported here; use
    ``legal_protected_segments`` and require each segment to survive.
    File-banner comments (``/*-``) may mention fall-through in prose; that
    is not a GCC -Wimplicit-fallthrough annotation, so it is not full-protect.
    """
    for name, rx in _PROTECT_RULES:
        if rx.search(old_text):
            if name == "fallthrough" and (
                    old_text.startswith(b"/*-")
                    or _WAITIO_FALLTHROUGH_PROSE.search(old_text)
                    or _FSM_FALLTHROUGH_PROSE.search(old_text)
                    or _FILEZERO_FALLTHROUGH_PROSE.search(old_text)
                    or _TOAST_REWRITE_FALLTHROUGH_PROSE.search(old_text)):
                continue
            return name
    return None


def _code_identifiers(text):
    """Identifiers in a comment that look like code rather than prose."""
    out = set()
    for m in _IDENT_RE.finditer(text):
        w = m.group(0)
        end = m.end()
        if b"_" in w and len(w) > 2:
            out.add(w)
        elif end < len(text) and text[end:end + 1] == b"(":
            out.add(w)
        elif len(w) > 3 and w[0:1].isupper() and any(ch in b"abcdefghijklmnopqrstuvwxyz" for ch in w) \
                and any(ch in b"ABCDEFGHIJKLMNOPQRSTUVWXYZ" for ch in w[1:]):
            out.add(w)  # CamelCase like XLogRecPtr
        elif len(w) > 4 and w.isupper():
            out.add(w)  # ALLCAPS macro-ish, e.g. ERROR, MAXALIGN
    return out


def _skeleton_diff_context(old_skel, new_skel, old_data):
    """Locate the first differing byte and return (line, context string)."""
    n = min(len(old_skel), len(new_skel))
    i = 0
    # fast path using chunks
    step = 4096
    while i < n and old_skel[i:i + step] == new_skel[i:i + step]:
        i += step
    while i < n and old_skel[i] == new_skel[i]:
        i += 1
    # line number in old skeleton ~ old file: placeholders have no LF, so
    # count LFs before i in the skeleton.
    line = old_skel.count(b"\n", 0, i) + 1
    ls = old_skel.rfind(b"\n", 0, i) + 1
    le = old_skel.find(b"\n", i)
    if le < 0:
        le = len(old_skel)
    ls2 = new_skel.rfind(b"\n", 0, i) + 1
    le2 = new_skel.find(b"\n", i)
    if le2 < 0:
        le2 = len(new_skel)
    ctx = "old: %r\nnew: %r" % (
        old_skel[ls:le].replace(b"\x00", b"@")[:200],
        new_skel[ls2:le2].replace(b"\x00", b"@")[:200],
    )
    return line, ctx


def compare_files(old_data, new_data, path="<mem>", strict_lines=True, warn=True):
    res = FileResult(path)
    try:
        old = c_lexer.lex(old_data)
        new = c_lexer.lex(new_data)
    except Exception as e:  # pragma: no cover - lexer is designed not to raise
        res.parse_error = True
        res.violations.append(Finding("R9_PARSE", "lexer exception: %r" % (e,)))
        return res

    res.stats = {
        "old_bytes": len(old_data),
        "new_bytes": len(new_data),
        "comments": len(old.comments),
        "block_comments": sum(1 for c in old.comments if c.kind == c_lexer.BLOCK),
        "line_comments": sum(1 for c in old.comments if c.kind == c_lexer.LINE),
        "splices": old.splices,
        "changed_comments": 0,
        "protected_comments": 0,
        "trigraphs_old": 0,
        "trigraphs_new": 0,
    }

    for w in old.warnings + new.warnings:
        if w[0] in ("unterminated_block_comment", "unterminated_raw_string"):
            res.parse_error = True
            res.violations.append(Finding("R9_PARSE", "%s at byte %d" % w,
                                          line=_byte_line(old_data if w in old.warnings else new_data, w[1])))
        else:
            res.warnings.append(Finding("W9_LEX", "%s at byte %d" % w))

    # round-trip sanity (lexer self-consistency)
    if c_lexer.reassemble(old.skeleton, old.comments) != old_data or \
            c_lexer.reassemble(new.skeleton, new.comments) != new_data:
        res.parse_error = True
        res.violations.append(Finding("R9_PARSE", "skeleton/comment round-trip mismatch"))
        return res

    # Rule 4: encoding of the new file
    if new_data.startswith(b"\xef\xbb\xbf"):
        res.violations.append(Finding("R4_BOM", "new file starts with UTF-8 BOM", line=1))
    try:
        new_data.decode("utf-8")
    except UnicodeDecodeError as e:
        res.violations.append(Finding("R4_UTF8", "new file is not valid UTF-8: %s" % e,
                                      line=_byte_line(new_data, e.start)))

    # Rule 1: skeleton byte-identical
    if old.skeleton != new.skeleton:
        line, ctx = _skeleton_diff_context(old.skeleton, new.skeleton, old_data)
        if len(old.comments) != len(new.comments):
            msg = "non-comment bytes differ (comment count %d -> %d)" % (
                len(old.comments), len(new.comments))
        else:
            msg = "non-comment bytes differ"
        res.violations.append(Finding("R1_SKELETON", msg, line=line, context=ctx))
        # Without an identical skeleton the comments cannot be paired
        # reliably; stop here (the remaining rules presuppose pairing).
        return res

    # Trigraph scan on whole files
    tri_old = c_lexer.find_trigraphs(old_data)
    tri_new = c_lexer.find_trigraphs(new_data)
    res.stats["trigraphs_old"] = len(tri_old)
    res.stats["trigraphs_new"] = len(tri_new)
    if len(tri_new) > len(tri_old):
        res.violations.append(Finding("R8_TRIGRAPH_NEW",
                                      "trigraph count increased %d -> %d" % (len(tri_old), len(tri_new))))

    # Per-comment rules; skeleton equality guarantees a 1:1 pairing.
    for oc, nc in zip(old.comments, new.comments):
        if oc.text == nc.text:
            if warn and oc.kind == c_lexer.BLOCK and protected_reason(oc.text) is None \
                    and _HAS_LETTERS.search(oc.text) and _is_ascii(oc.text) and len(oc.text) > 24 \
                    and not _is_legal_only_comment(oc.text):
                res.warnings.append(Finding("W7_UNTRANSLATED", "comment unchanged (still ASCII prose)",
                                            line=oc.line, comment_index=oc.index))
            continue
        res.stats["changed_comments"] += 1

        if oc.kind != nc.kind:
            res.violations.append(Finding("R2_KIND", "comment kind changed %s -> %s" % (oc.kind, nc.kind),
                                          line=oc.line, comment_index=oc.index))
            continue

        reason = protected_reason(oc.text)
        if reason is not None:
            res.stats["protected_comments"] += 1
            res.violations.append(Finding("R5_PROTECTED", "protected comment (%s) was modified" % reason,
                                          line=oc.line, comment_index=oc.index,
                                          context=_snippet(oc.text)))
        else:
            missing = [seg for seg in legal_protected_segments(oc.text)
                       if seg not in nc.text]
            if missing:
                res.stats["protected_comments"] += 1
                res.violations.append(Finding(
                    "R5_PROTECTED",
                    "copyright/IDENTIFICATION region was modified",
                    line=oc.line, comment_index=oc.index,
                    context=_snippet(missing[0])))

        if c_lexer.find_trigraphs(oc.text):
            res.violations.append(Finding("R8_TRIGRAPH_BASE",
                                          "comment containing a baseline trigraph was modified",
                                          line=oc.line, comment_index=oc.index))
        elif c_lexer.find_trigraphs(nc.text):
            res.violations.append(Finding("R8_TRIGRAPH_NEW", "new comment introduces a trigraph",
                                          line=oc.line, comment_index=oc.index))

        if nc.kind == c_lexer.BLOCK:
            body = nc.body()
            if b"*/" in body:
                res.violations.append(Finding("R3_EARLY_CLOSE", "new block comment body contains '*/'",
                                              line=oc.line, comment_index=oc.index))
            if body.count(b"/*") > oc.body().count(b"/*"):
                res.violations.append(Finding("R3_NESTED_OPEN", "new block comment adds a nested '/*'",
                                              line=oc.line, comment_index=oc.index))
        else:
            if nc.text.rstrip(b"\r").endswith(b"\\"):
                res.violations.append(Finding("R3_LINE_BACKSLASH", "new // comment ends with a backslash",
                                              line=oc.line, comment_index=oc.index))

        olf = oc.text.count(b"\n")
        nlf = nc.text.count(b"\n")
        if olf != nlf:
            f = Finding("R6_LF_COUNT" if strict_lines else "W6_LF_COUNT",
                        "LF count in comment changed %d -> %d" % (olf, nlf),
                        line=oc.line, comment_index=oc.index)
            (res.violations if strict_lines else res.warnings).append(f)

        if oc.text.count(b"\r") != nc.text.count(b"\r"):
            res.violations.append(Finding("R4_CR", "carriage-return count in comment changed",
                                          line=oc.line, comment_index=oc.index))

        if warn:
            lost = sorted(_code_identifiers(oc.text) - set(_IDENT_RE.findall(nc.text)))
            if lost:
                res.warnings.append(Finding("W7_IDENT_LOST",
                                            "identifiers missing from new comment: %s" %
                                            ", ".join(x.decode("ascii", "replace") for x in lost[:8]),
                                            line=oc.line, comment_index=oc.index))
    return res


def _is_legal_only_comment(text):
    """True when every non-chrome line is a protected legal/IDENTIFICATION line."""
    segs = legal_protected_segments(text)
    if not segs:
        return False
    remain = text
    for seg in segs:
        remain = remain.replace(seg, b"", 1)
    for line in remain.splitlines():
        body = line.lstrip()
        if body.startswith(b"/*"):
            body = body[2:]
        if body.endswith(b"*/"):
            body = body[:-2]
        body = body.lstrip(b"*").strip()
        if not body or body.startswith(b"-"):
            continue
        return False
    return True


def _is_ascii(b):
    try:
        b.decode("ascii")
        return True
    except UnicodeDecodeError:
        return False


def _byte_line(data, off):
    return data.count(b"\n", 0, max(0, min(off, len(data)))) + 1


def _snippet(text, n=120):
    s = text.decode("utf-8", "replace")
    s = s.replace("\n", "\\n")
    return s[:n] + ("..." if len(s) > n else "")
