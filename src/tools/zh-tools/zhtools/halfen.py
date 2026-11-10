"""
Half-English debt scanning for C comment text (third generation).

A "half-English" (halfen) comment is a translated comment that still
contains runs of English prose -- leftover fragments of the original
English wording inside otherwise Chinese text.  Two finding classes:

  MIXED    comment contains CJK text AND a run of >= 2 consecutive prose
           tokens (space-separated English words), the classic debt the
           2026-11 campaigns cleared in src/backend/commands.
  PUREEN   comment contains no CJK AND >= 4 prose tokens, i.e. it looks
           like an untranslated English comment.  Reported only with
           --pureen: on a tree that is only partially translated this is
           the common case and duplicates the advisory W7_UNTRANSLATED
           of verify_c_comments.py; it is useful when scanning files that
           are supposed to be fully translated.

Token rules (unchanged from the halfen2 campaign scanner): a prose token
has no underscore, is not all-uppercase, carries fewer than 2 uppercase
letters and at least 2 alphabetic characters after punctuation stripping.
CamelCase identifiers, snake_case names and ALL-CAPS keywords are exempt
automatically; numbers, units and lone letters never count.

Lineage: halfen and halfen2 (a /tmp campaign tool) classified whole *lines*
with a comment-continuation heuristic; lines of real code such as
``*parameterDefaults = lappend(...)`` carry a leading ``*`` that mimics
comment decoration and were misflagged (the ``*ptr = `` false-positive
family, 9+ sites recorded during the commands campaign).  halfen3 extracts
comments with the real lexer (c_lexer), so code lines are never examined.

Exemptions, in order:
  * comments that verify_c_comments protects (translator:, GCC fall-through
    annotations, pg_attribute_*) and copyright/IDENTIFICATION segments;
  * per-comment built-in exempt patterns (HALFEN_OK_PATTERNS below) for
    judged-legitimate classes recorded by the campaigns;
  * the curated site ledger in halfen_exemptions.json (entries
    {"path": glob, "pattern": body regex, "reason": str}).

Nothing is rewritten; the scanner only reports.
"""
import json
import os
import re
import sys

sys.dont_write_bytecode = True

from . import c_lexer, c_verify  # noqa: E402
from .manifest import glob_to_regex  # noqa: E402

MIXED = "mixed"
PUREEN = "pureen"

_CJK_RE = re.compile(
    "[" + "".join("%s-%s" % (chr(lo), chr(hi)) for lo, hi in
                  ((0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF)))
    + "]")
_LETTER_RE = re.compile(rb"[A-Za-z]")
_UPPER_RE = re.compile(rb"[A-Z]")
_STRIP_RE = b" \t\r\n.,;:!?()[]{}<>'\"`-=|/&*+~^%$#@"

# Judged-legitimate comment classes from the 2026-11 translation campaigns.
# Each entry is (compiled body regex, human reason); extend only with a
# recorded campaign judgment.
HALFEN_OK_PATTERNS = tuple(
    (re.compile(pat), reason) for pat, reason in (
        # WAL record type names kept verbatim (rmgr descriptors):
        (rb"running xacts|known assigned xid", "WAL record type name"),
    )
)


def is_prose_token(token):
    """True for a whitespace-separated token that reads as English prose.

    A token that itself contains CJK text (e.g. ``poll(2)，否则用``) is
    Chinese wording around a kept term, not an English prose fragment, and
    never counts; otherwise a run of pure-ASCII prose tokens flags real
    leftover English phrases such as ``the result set``.  Identifiers
    (underscores), CamelCase, ALL-CAPS keywords, numbers, units and lone
    letters never count either (halfen2 token rules).
    """
    if any(b > 0x7F for b in token):
        return False
    token = token.strip(_STRIP_RE)
    if len(token) < 2:
        return False
    if b"_" in token:
        return False
    if not _LETTER_RE.search(token):
        return False
    if len(_LETTER_RE.findall(token)) < 2:
        return False
    uppers = len(_UPPER_RE.findall(token))
    if uppers >= 2:
        return False
    if token.isupper():
        return False
    return True


def longest_prose_run(body):
    """Longest run of consecutive prose tokens in the body (token count)."""
    best = run = 0
    for token in body.split():
        if is_prose_token(token):
            run += 1
            best = max(best, run)
        else:
            run = 0
    return best


def prose_token_count(body):
    return sum(1 for token in body.split() if is_prose_token(token))


def has_cjk(text):
    return bool(_CJK_RE.search(text.decode("utf-8", "replace")))


class Finding(object):
    __slots__ = ("path", "line", "end_line", "cls", "run", "run_text",
                 "excerpt", "reason")

    def __init__(self, path, line, end_line, cls, run, excerpt, run_text=b"",
                 reason=None):
        self.path = path
        self.line = line
        self.end_line = end_line
        self.cls = cls
        self.run = run
        self.run_text = run_text
        self.excerpt = excerpt
        self.reason = reason

    def to_dict(self):
        d = {"path": self.path, "line": self.line, "end_line": self.end_line,
             "class": self.cls, "run": self.run, "run_text": self.run_text,
             "excerpt": self.excerpt}
        if self.reason:
            d["reason"] = self.reason
        return d


def longest_prose_run_tokens(body):
    """Tokens of the longest consecutive prose-token run (list of bytes)."""
    best = []
    run = []
    for token in body.split():
        if is_prose_token(token):
            run.append(token)
            if len(run) > len(best):
                best = list(run)
        else:
            run = []
    return best


def _single_line(body):
    return b" ".join(body.split())


def scan_bytes(data, path, include_pureen=False, site_exemptions=()):
    """Scan one file blob; return a list of Finding (comments only).

    ``site_exemptions`` is a sequence of (compiled path regex, compiled
    body regex, reason) triples; a comment matching both its file path and
    the body pattern is exempt with that reason.
    """
    findings = []
    try:
        parsed = c_lexer.lex(data)
    except Exception as e:  # lexer failure: one finding per file, not a crash
        return [Finding(path, 1, 1, MIXED, 0, "<lex error: %s>" % e)]

    for comment in parsed.comments:
        text = comment.text
        if c_verify.protected_reason(text) is not None:
            continue
        if c_verify.legal_protected_segments(text):
            continue  # copyright / IDENTIFICATION / licence header
        body = _comment_body(text)
        cls = classify(body, include_pureen=include_pureen)
        if cls is None:
            continue
        reason = None
        for pat, ok_reason in HALFEN_OK_PATTERNS:
            if pat.search(body):
                reason = ok_reason
                break
        if reason is None:
            for path_re, body_re, ok_reason in site_exemptions:
                if path_re.match(path) and body_re.search(body):
                    reason = ok_reason
                    break
        if reason is not None:
            continue
        run_tokens = longest_prose_run_tokens(body)
        findings.append(Finding(
            path, comment.line, comment.end_line, cls,
            len(run_tokens) if cls == MIXED else prose_token_count(body),
            _single_line(body)[:110].decode("utf-8", "replace"),
            run_text=b" ".join(run_tokens).decode("utf-8", "replace")))
    return findings


def classify(body, include_pureen=False):
    """MIXED / PUREEN / None for one comment body."""
    cjk = has_cjk(body)
    if cjk:
        if longest_prose_run(body) >= 2:
            return MIXED
        return None
    if include_pureen and prose_token_count(body) >= 4:
        return PUREEN
    return None


def _comment_body(text):
    """Text between the comment delimiters (best effort; body never
    contains ``*/``)."""
    if text.startswith(b"/*"):
        end = text.rfind(b"*/")
        return text[2:end if end != -1 else len(text)]
    if text.startswith(b"//"):
        return text[2:]
    return text


def load_site_exemptions(path):
    """Load halfen_exemptions.json -> [(path_re, body_re, reason)]."""
    with open(path, encoding="utf-8") as f:
        entries = json.load(f)
    out = []
    for entry in entries:
        reason = entry.get("reason")
        if not reason:
            raise ValueError("site exemption without reason: %r" % (entry,))
        out.append((re.compile(glob_to_regex(entry["path"])),
                    re.compile(entry["pattern"].encode("utf-8")),
                    reason))
    return out
