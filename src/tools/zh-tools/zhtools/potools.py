"""
Minimal, mechanical PO/POT reader for the PO gate.

This module proves facts about catalog *files*: which active keys they
contain, whether a key is duplicated, whether plural entries are complete,
what the header says, and stable hashes over them.  It never emulates
gettext -- merging, fuzzy matching, format checking and compilation are done
by the real msgmerge/msgattrib/msgfmt/msgcmp (see po_sync.py); this reader
only exists so that the gate can look inside their inputs and outputs and
so that the POT archive can be hashed independently of its timestamp.

A "key" is ``(msgctxt, msgid)`` -- exactly what gettext uses for lookup.
"""
import hashlib
import os
import re
import sys

sys.dont_write_bytecode = True

_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v",
            "\\": "\\", '"': '"', "'": "'", "?": "?"}
_STR_RX = re.compile(r'^"(.*)"$')
_KEYWORD_RX = re.compile(r'^(msgctxt|msgid_plural|msgid|msgstr\[(\d+)\]|msgstr)\s+(".*")$')


class PoError(ValueError):
    pass


def unescape(s):
    """Decode the contents of one PO string literal (without the quotes)."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            raise PoError("dangling backslash")
        e = s[i]
        if e in _ESCAPES:
            out.append(_ESCAPES[e])
            i += 1
        elif e in "01234567":
            j = i
            while j < n and j - i < 3 and s[j] in "01234567":
                j += 1
            out.append(chr(int(s[i:j], 8)))
            i = j
        elif e == "x":
            j = i + 1
            while j < n and s[j] in "0123456789abcdefABCDEF":
                j += 1
            if j == i + 1:
                raise PoError("bad \\x escape")
            out.append(chr(int(s[i + 1:j], 16)))
            i = j
        else:
            raise PoError("unknown escape \\%s" % e)
    return "".join(out)


class Entry(object):
    __slots__ = ("msgctxt", "msgid", "msgid_plural", "msgstr", "msgstr_plural", "flags", "references",
                 "obsolete", "previous", "comments", "lineno")

    def __init__(self, lineno):
        self.msgctxt = None
        self.msgid = None
        self.msgid_plural = None
        self.msgstr = None
        self.msgstr_plural = {}
        self.flags = []
        self.references = []
        self.obsolete = False
        self.previous = []
        self.comments = []
        self.lineno = lineno

    @property
    def key(self):
        return (self.msgctxt, self.msgid)

    @property
    def is_header(self):
        return self.msgid == "" and self.msgctxt is None

    @property
    def fuzzy(self):
        return "fuzzy" in self.flags

    def translations(self):
        if self.msgid_plural is not None:
            return [self.msgstr_plural[k] for k in sorted(self.msgstr_plural)]
        return [self.msgstr if self.msgstr is not None else ""]

    @property
    def translated(self):
        """gettext's notion: all forms non-empty and not fuzzy."""
        if self.fuzzy:
            return False
        return all(t != "" for t in self.translations())


def parse(text, path="<po>"):
    """Parse PO text into a list of Entry.  Raises PoError on malformed input."""
    entries = []
    cur = None
    field = None  # which string field continuation lines append to
    lines = text.split("\n")
    for idx, raw in enumerate(lines, 1):
        line = raw.rstrip("\r")
        if line.strip() == "":
            if cur is not None:
                entries.append(cur)
                cur = None
                field = None
            continue
        obsolete = False
        if line.startswith("#~"):
            obsolete = True
            line = line[2:].lstrip()
            if line.startswith("|"):
                # obsolete previous-msgid line (#~| msgid ...): keep as previous
                if cur is None:
                    cur = Entry(idx)
                cur.obsolete = True
                cur.previous.append(line[1:].strip())
                field = None
                continue
        if line.startswith("#"):
            if cur is None:
                cur = Entry(idx)
            if line.startswith("#,"):
                cur.flags.extend(f.strip() for f in line[2:].split(",") if f.strip())
            elif line.startswith("#:"):
                cur.references.extend(line[2:].split())
            elif line.startswith("#|"):
                cur.previous.append(line[2:].strip())
            else:
                cur.comments.append(line)
            field = None
            continue
        if cur is None:
            cur = Entry(idx)
        if obsolete:
            cur.obsolete = True
        m = _KEYWORD_RX.match(line)
        if m:
            kw, plural_idx, lit = m.group(1), m.group(2), m.group(3)
            sm = _STR_RX.match(lit)
            if not sm:
                raise PoError("%s:%d: bad string literal" % (path, idx))
            val = unescape(sm.group(1))
            if kw == "msgctxt":
                cur.msgctxt = val
                field = "msgctxt"
            elif kw == "msgid":
                cur.msgid = val
                field = "msgid"
            elif kw == "msgid_plural":
                cur.msgid_plural = val
                field = "msgid_plural"
            elif kw == "msgstr":
                cur.msgstr = val
                field = "msgstr"
            else:
                k = int(plural_idx)
                cur.msgstr_plural[k] = val
                field = ("msgstr_plural", k)
            continue
        sm = _STR_RX.match(line.strip())
        if sm and field is not None:
            val = unescape(sm.group(1))
            if isinstance(field, tuple):
                cur.msgstr_plural[field[1]] += val
            else:
                setattr(cur, field, getattr(cur, field) + val)
            continue
        raise PoError("%s:%d: unexpected line: %r" % (path, idx, raw[:80]))
    if cur is not None:
        entries.append(cur)
    for e in entries:
        if e.msgid is None:
            raise PoError("%s:%d: entry without msgid" % (path, e.lineno))
    return entries


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    if data.startswith(b"\xef\xbb\xbf"):
        raise PoError("%s: UTF-8 BOM" % path)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as e:
        raise PoError("%s: not UTF-8: %s" % (path, e))
    return parse(text, path)


def header_fields(entries):
    """Return the header entry's fields as an ordered dict (first entry with msgid "")."""
    for e in entries:
        if e.is_header and not e.obsolete:
            out = {}
            for line in (e.msgstr or "").split("\n"):
                if ":" in line:
                    k, v = line.split(":", 1)
                    out[k.strip()] = v.strip()
            return out
    return {}


def active_entries(entries):
    return [e for e in entries if not e.obsolete and not e.is_header]


def active_keys(entries):
    return [e.key for e in active_entries(entries)]


def duplicate_keys(entries):
    seen = set()
    dups = []
    for k in active_keys(entries):
        if k in seen:
            dups.append(k)
        seen.add(k)
    return dups


def _key_repr(key):
    ctx, mid = key
    return ("%s\x04%s" % (ctx, mid)) if ctx is not None else mid


def active_key_hash(entries):
    """SHA-256 over the sorted set of active (msgctxt, msgid[, msgid_plural]) -- independent of order,
    comments, references and translations."""
    items = sorted(_key_repr(e.key) + ("\x00" + e.msgid_plural if e.msgid_plural is not None else "")
                   for e in active_entries(entries))
    h = hashlib.sha256()
    for it in items:
        h.update(it.encode("utf-8"))
        h.update(b"\n")
    return h.hexdigest()


# header lines whose value is a timestamp or otherwise changes between two
# otherwise identical xgettext runs
POT_DYNAMIC_HEADER_FIELDS = ("POT-Creation-Date",)


def canonical_pot_hash(path):
    """SHA-256 of the POT text with the dynamic header fields removed.

    Two POTs produced from the same sources by the same xgettext with the
    same options hash the same here even though their creation dates
    differ; any difference in messages, references, comments or flags
    changes the hash.
    """
    with open(path, "rb") as f:
        text = f.read().decode("utf-8")
    out = []
    for line in text.split("\n"):
        skip = False
        for fld in POT_DYNAMIC_HEADER_FIELDS:
            if line.startswith('"%s:' % fld):
                skip = True
        if not skip:
            out.append(line)
    return hashlib.sha256("\n".join(out).encode("utf-8")).hexdigest()


def plural_forms_nplurals(header):
    pf = header.get("Plural-Forms", "")
    m = re.search(r"nplurals\s*=\s*(\d+)", pf)
    return int(m.group(1)) if m else None


def check_plurals(entries, nplurals):
    """Entries with msgid_plural must have msgstr[0..nplurals-1], all non-empty."""
    problems = []
    for e in active_entries(entries):
        if e.msgid_plural is None:
            continue
        want = list(range(nplurals))
        have = sorted(e.msgstr_plural)
        if have != want:
            problems.append({"key": list(e.key), "line": e.lineno, "have": have, "want": want})
        elif any(e.msgstr_plural[i] == "" for i in want):
            problems.append({"key": list(e.key), "line": e.lineno, "empty_forms": [i for i in want if e.msgstr_plural[i] == ""]})
    return problems


def summarize(entries):
    act = active_entries(entries)
    return {
        "entries_total": len(entries),
        "active": len(act),
        "obsolete": sum(1 for e in entries if e.obsolete),
        "fuzzy": sum(1 for e in act if e.fuzzy),
        "untranslated": sum(1 for e in act if not e.fuzzy and any(t == "" for t in e.translations())),
        "translated": sum(1 for e in act if e.translated),
        "plural_entries": sum(1 for e in act if e.msgid_plural is not None),
        "with_msgctxt": sum(1 for e in act if e.msgctxt is not None),
    }


def key_to_json(key):
    return {"msgctxt": key[0], "msgid": key[1]}


def escape(s):
    """Encode a Python string as the inside of a PO string literal."""
    out = []
    for c in s:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif ord(c) < 0x20:
            out.append("\\%03o" % ord(c))
        else:
            out.append(c)
    return "".join(out)


def _literal(keyword, s):
    """One PO keyword + string, in gettext's no-wrap layout (newline-split)."""
    # gettext keeps a string on one line unless it contains a newline that is
    # not its single final character
    if "\n" in s[:-1]:
        parts = s.split("\n")
        lines = [keyword + ' ""']
        for i, p in enumerate(parts):
            if i < len(parts) - 1:
                lines.append('"%s\\n"' % escape(p))
            elif p != "":
                lines.append('"%s"' % escape(p))
        return "\n".join(lines)
    return '%s "%s"' % (keyword, escape(s))


def serialize_entry(e):
    """Serialize one Entry as a PO block (no trailing blank line).

    Only used to splice an override into a merged catalog before the result
    is re-serialized by msgattrib; gettext, not this function, defines the
    final layout.
    """
    lines = list(e.comments)
    if e.previous:
        lines.extend("#| " + p for p in e.previous)
    if e.references:
        lines.append("#: " + " ".join(e.references))
    if e.flags:
        lines.append("#, " + ", ".join(e.flags))
    if e.msgctxt is not None:
        lines.append(_literal("msgctxt", e.msgctxt))
    lines.append(_literal("msgid", e.msgid))
    if e.msgid_plural is not None:
        lines.append(_literal("msgid_plural", e.msgid_plural))
        for k in sorted(e.msgstr_plural):
            lines.append(_literal("msgstr[%d]" % k, e.msgstr_plural[k]))
    else:
        lines.append(_literal("msgstr", e.msgstr or ""))
    return "\n".join(lines)


def split_blocks(text):
    """Split PO text into blocks separated by blank lines (strings never contain a bare blank line)."""
    return re.split(r"\n\s*\n", text.strip("\n"))
