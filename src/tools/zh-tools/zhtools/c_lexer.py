"""
Byte-level C / C++ comment lexer.

The lexer follows the C translation phases that matter for comment
detection:

  1. (trigraphs are *not* replaced; PostgreSQL is built in GNU mode where
     trigraphs are disabled.  They are only *scanned* so that callers can
     forbid new ones -- see ``find_trigraphs``.)
  2. Backslash-newline line splicing is applied globally, while a mapping
     from every spliced byte back to its original offset is kept.
  3. The spliced byte stream is scanned with a small state machine that
     recognises block comments, line comments, string literals (including
     ``L``/``u8``/``u``/``U`` prefixes and escapes), character literals,
     C++11 raw string literals (``R"d(...)d"`` with the same prefixes),
     ``#include <header-name>`` and ``#error``/``#warning`` text lines.

Only the *original* bytes are ever reported: every comment carries its
original byte range, and the skeleton is the original file with each
comment replaced by ``\\x00C<n>\\x00``.  ``reassemble(skeleton, comments)``
must give back the original bytes; that round-trip is checked in the
self-check mode of verify_c_comments.py.

The lexer never raises on odd input (unterminated literals, missing final
newline, ...).  Such conditions are reported as ``LexResult.warnings`` so
that the verifier can decide what to do.  A literal that is still open at
a newline is closed at that newline; this makes stray apostrophes inside
``#error``/``#warning`` or ``#if 0`` text harmless and always errs on the
side of treating bytes as *code* (which the verifier then requires to be
unchanged).
"""
import sys

sys.dont_write_bytecode = True

from bisect import bisect_right  # noqa: E402

TRIGRAPHS = (b"??=", b"??/", b"??'", b"??(", b"??)", b"??!", b"??<", b"??>", b"??-")

BLOCK = "block"
LINE = "line"

_WS = b" \t\f\v"
_IDENT_START = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_")
_IDENT_CONT = _IDENT_START | frozenset(b"0123456789")
_RAW_DELIM_FORBIDDEN = frozenset(b' ()\\\t\v\f\n')


class Comment(object):
    """One comment, located in *original* byte offsets."""

    __slots__ = ("kind", "start", "end", "text", "line", "end_line", "index")

    def __init__(self, kind, start, end, text, line, end_line, index):
        self.kind = kind
        self.start = start
        self.end = end
        self.text = text
        self.line = line
        self.end_line = end_line
        self.index = index

    def body(self):
        if self.kind == BLOCK:
            # Opening may be spliced ("/\\\n*"); strip only when it is the
            # plain two-byte form so that the body is exact for the common
            # case and still safe (a superset) otherwise.
            t = self.text
            if t.startswith(b"/*"):
                t = t[2:]
            if t.endswith(b"*/"):
                t = t[:-2]
            return t
        t = self.text
        if t.startswith(b"//"):
            t = t[2:]
        return t

    def to_dict(self, with_text=True):
        d = {
            "index": self.index,
            "kind": self.kind,
            "start": self.start,
            "end": self.end,
            "line": self.line,
            "end_line": self.end_line,
        }
        if with_text:
            d["text"] = self.text.decode("utf-8", "replace")
        return d


class LexResult(object):
    __slots__ = ("data", "comments", "skeleton", "warnings", "splices")

    def __init__(self, data, comments, skeleton, warnings, splices):
        self.data = data
        self.comments = comments
        self.skeleton = skeleton
        self.warnings = warnings
        self.splices = splices


def placeholder(n):
    return b"\x00C%d\x00" % n


def splice_lines(data):
    """Remove backslash-newline (and backslash-CR-LF) sequences.

    Returns (spliced_bytes, omap) where omap is None when nothing was
    removed (identity mapping) or a list such that omap[i] is the original
    offset of spliced byte i, with omap[len(spliced)] == len(data).
    """
    if b"\\\n" not in data and b"\\\r\n" not in data:
        return data, None, 0
    out = bytearray()
    omap = []
    i = 0
    n = len(data)
    splices = 0
    while i < n:
        c = data[i]
        if c == 0x5C:  # backslash
            if i + 1 < n and data[i + 1] == 0x0A:
                i += 2
                splices += 1
                continue
            if i + 2 < n and data[i + 1] == 0x0D and data[i + 2] == 0x0A:
                i += 3
                splices += 1
                continue
        out.append(c)
        omap.append(i)
        i += 1
    omap.append(n)
    return bytes(out), omap, splices


def _line_of(data, offset, lf_positions):
    """1-based line number of byte offset in original data."""
    return bisect_right(lf_positions, offset - 1) + 1


def lex(data):
    """Lex *data* (bytes).  Returns LexResult."""
    if not isinstance(data, (bytes, bytearray)):
        raise TypeError("lex() expects bytes")
    data = bytes(data)
    spliced, omap, nsplices = splice_lines(data)
    if omap is None:
        def orig(p):
            return p
    else:
        def orig(p):
            return omap[p]

    n = len(spliced)
    comments = []
    warnings = []

    # directive tracking
    at_line_start = True          # only whitespace/comments so far on this logical line
    in_directive = False
    directive_name = None         # bytes or None
    directive_text_mode = False   # #error / #warning: literals end at newline silently
    expect_directive_name = False

    i = 0
    while i < n:
        c = spliced[i]

        # ---- newline handling (code state) ----
        if c == 0x0A:
            at_line_start = True
            in_directive = False
            directive_name = None
            directive_text_mode = False
            expect_directive_name = False
            i += 1
            continue

        if c in _WS:
            i += 1
            continue

        # ---- comments ----
        if c == 0x2F and i + 1 < n:  # '/'
            c2 = spliced[i + 1]
            if c2 == 0x2A:  # block comment
                start_s = i
                j = spliced.find(b"*/", i + 2)
                if j < 0:
                    warnings.append(("unterminated_block_comment", orig(start_s)))
                    end_s = n
                else:
                    end_s = j + 2
                comments.append((BLOCK, orig(start_s), orig(end_s) if end_s < n else len(data)))
                i = end_s
                # a block comment does not change at_line_start
                continue
            if c2 == 0x2F:  # line comment
                start_s = i
                j = spliced.find(b"\n", i + 2)
                if j < 0:
                    end_s = n
                else:
                    end_s = j
                comments.append((LINE, orig(start_s), orig(end_s) if end_s < n else len(data)))
                i = end_s
                continue

        # ---- preprocessor directive start ----
        if c == 0x23 and at_line_start:  # '#'
            in_directive = True
            expect_directive_name = True
            at_line_start = False
            i += 1
            continue

        if expect_directive_name:
            expect_directive_name = False
            if c in _IDENT_START:
                j = i + 1
                while j < n and spliced[j] in _IDENT_CONT:
                    j += 1
                directive_name = spliced[i:j]
                if directive_name in (b"error", b"warning"):
                    directive_text_mode = True
                i = j
                continue
            # '# 123 "file"' style or lone '#': fall through to normal lexing

        at_line_start = False

        # ---- #include <header-name> ----
        if c == 0x3C and in_directive and directive_name in (b"include", b"include_next", b"import"):  # '<'
            j = i + 1
            while j < n and spliced[j] not in (0x3E, 0x0A):
                j += 1
            if j < n and spliced[j] == 0x3E:
                i = j + 1
            else:
                i = j
            continue

        # ---- string / char / raw string literals ----
        if c == 0x22 or c == 0x27 or c in _IDENT_START:
            # Determine whether an encoding prefix / raw prefix applies.
            j = i
            if c in _IDENT_START:
                # scan identifier; check if it is a literal prefix
                k = i + 1
                while k < n and spliced[k] in _IDENT_CONT:
                    k += 1
                ident = spliced[i:k]
                if k < n and spliced[k] in (0x22, 0x27) and ident in (
                        b"L", b"u", b"U", b"u8",
                        b"R", b"LR", b"uR", b"UR", b"u8R"):
                    is_raw = ident.endswith(b"R") and spliced[k] == 0x22
                    quote = spliced[k]
                    if is_raw:
                        i = _lex_raw_string(data, spliced, omap, orig, k, warnings)
                        continue
                    i = _lex_quoted(spliced, k, quote, warnings, orig, directive_text_mode)
                    continue
                # ordinary identifier
                i = k
                continue
            i = _lex_quoted(spliced, i, c, warnings, orig, directive_text_mode)
            continue

        # digits and everything else: single byte advance (pp-numbers may
        # contain quotes only via digit separators in C++14, which
        # PostgreSQL does not use; if present they'd be lexed as char
        # literals -- conservative)
        i += 1

    # ---- materialise comments and skeleton on ORIGINAL bytes ----
    lf_positions = []
    p = data.find(b"\n")
    while p >= 0:
        lf_positions.append(p)
        p = data.find(b"\n", p + 1)

    out_comments = []
    skel = bytearray()
    last = 0
    for idx, (kind, s, e) in enumerate(comments):
        text = data[s:e]
        line = _line_of(data, s, lf_positions)
        end_line = _line_of(data, max(s, e - 1), lf_positions)
        out_comments.append(Comment(kind, s, e, text, line, end_line, idx))
        skel += data[last:s]
        skel += placeholder(idx)
        last = e
    skel += data[last:]

    return LexResult(data, out_comments, bytes(skel), warnings, nsplices)


def _lex_quoted(spliced, i, quote, warnings, orig, text_mode):
    """Lex a string ("), or char (') literal starting at spliced[i] == quote.

    Returns the spliced index just past the closing quote, or at the newline
    that terminated the literal prematurely.
    """
    n = len(spliced)
    j = i + 1
    while j < n:
        c = spliced[j]
        if c == 0x5C:  # backslash escape
            j += 2
            continue
        if c == quote:
            return j + 1
        if c == 0x0A:
            if not text_mode:
                warnings.append(("unterminated_literal", orig(i)))
            return j  # leave newline to main loop
        j += 1
    if not text_mode:
        warnings.append(("unterminated_literal", orig(i)))
    return n


def _lex_raw_string(data, spliced, omap, orig, k, warnings):
    """Lex a C++11 raw string whose opening quote is at spliced[k].

    Raw strings undo line splicing, so the body is scanned on the ORIGINAL
    bytes, and the end is mapped back to a spliced index.
    """
    o = orig(k)  # original offset of the opening quote
    n = len(data)
    j = o + 1
    # delimiter: up to 16 chars, none of the forbidden set
    while j < n and data[j] != 0x28 and (j - o - 1) < 17:  # '('
        if data[j] in _RAW_DELIM_FORBIDDEN:
            break
        j += 1
    if j >= n or data[j] != 0x28:
        warnings.append(("bad_raw_string_delimiter", o))
        # treat as ordinary string
        return _lex_quoted(spliced, k, 0x22, warnings, orig, False)
    delim = data[o + 1:j]
    closer = b")" + delim + b'"'
    e = data.find(closer, j + 1)
    if e < 0:
        warnings.append(("unterminated_raw_string", o))
        return len(spliced)
    end_orig = e + len(closer)
    # map original offset back to spliced index
    if omap is None:
        return end_orig
    return bisect_right(omap, end_orig - 1)


def reassemble(skeleton, comments):
    """Inverse of lex(): put comment texts back into the skeleton."""
    out = bytearray()
    pos = 0
    for c in comments:
        ph = placeholder(c.index)
        j = skeleton.find(ph, pos)
        if j < 0:
            raise ValueError("placeholder %d missing from skeleton" % c.index)
        out += skeleton[pos:j]
        out += c.text
        pos = j + len(ph)
    out += skeleton[pos:]
    return bytes(out)


def find_trigraphs(data):
    """Return sorted list of (offset, trigraph_bytes) in data."""
    hits = []
    for t in TRIGRAPHS:
        p = data.find(t)
        while p >= 0:
            hits.append((p, t))
            p = data.find(t, p + 1)
    hits.sort()
    return hits
