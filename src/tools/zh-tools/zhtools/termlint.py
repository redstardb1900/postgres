"""
Conservative terminology linting for (English, Chinese) text pairs.

Only *explicit* hits are reported: a rule fires when its English pattern
matches the source text AND its forbidden Chinese pattern matches the
translation (or, for preserved terms, when the term is in the source and
absent from the translation).  Nothing is ever rewritten.

Rule sources:
  * pgnls ``ref/phrasebook.tsv`` rows with ``status=decided`` and a
    non-empty ``zh_forbidden`` column (ids P01..).
  * ``term_rules.json`` next to the tools: style-guide derived rules
    (ids TL..) and the list of English terms that must be preserved
    verbatim (ids PT_<term>).
  * pgnls ``ref/glossary.tsv`` rows whose Chinese equals the English are
    added to the preserved terms.
"""
import csv
import io
import json
import re
import sys

sys.dont_write_bytecode = True

from .manifest import glob_to_regex  # noqa: E402


class Rule(object):
    __slots__ = ("id", "en", "zh_forbidden", "note", "source", "kind", "term")

    def __init__(self, id_, en, zh_forbidden, note, source, kind="pair", term=None):
        self.id = id_
        self.en = en
        self.zh_forbidden = zh_forbidden
        self.note = note
        self.source = source
        self.kind = kind
        self.term = term


class Hit(object):
    __slots__ = ("rule", "path", "line", "en", "zh", "note")

    def __init__(self, rule, path, line, en, zh, note):
        self.rule = rule
        self.path = path
        self.line = line
        self.en = en
        self.zh = zh
        self.note = note

    def to_dict(self):
        return {"rule": self.rule, "path": self.path, "line": self.line,
                "en": _snip(self.en), "zh": _snip(self.zh), "note": self.note}


def _snip(s, n=160):
    s = s.replace("\n", "\\n")
    return s[:n] + ("..." if len(s) > n else "")


def load_phrasebook(text):
    rules = []
    rd = csv.DictReader(io.StringIO(text), delimiter="\t", quoting=csv.QUOTE_NONE)
    for row in rd:
        if row.get("status") != "decided":
            continue
        forb = (row.get("zh_forbidden") or "").strip()
        en = (row.get("en_pattern") or "").strip()
        if not forb or not en:
            continue
        try:
            rules.append(Rule(row["id"], re.compile(en, re.I), re.compile(forb),
                              row.get("note") or "", "phrasebook"))
        except re.error as e:
            raise ValueError("phrasebook %s: bad regex: %s" % (row.get("id"), e))
    return rules


def load_glossary_preserved(text):
    """Terms whose glossary translation is the term itself."""
    terms = []
    rd = csv.reader(io.StringIO(text), delimiter="\t", quoting=csv.QUOTE_NONE)
    header = next(rd, None)
    if not header or len(header) < 2:
        return terms
    for row in rd:
        if len(row) < 2:
            continue
        en, zh = row[0].strip(), row[1].strip()
        if en and en == zh and re.match(r"\A[A-Za-z][A-Za-z0-9_]*\Z", en):
            terms.append(en)
    return terms


def load_term_rules(obj):
    rules = []
    for r in obj.get("rules", []):
        rules.append(Rule(r["id"], re.compile(r["en"], re.I), re.compile(r["zh_forbidden"]),
                          r.get("note", ""), "term_rules"))
    return rules, list(obj.get("preserve_terms", []))


def preserved_term_rules(terms):
    rules = []
    for t in sorted(set(terms)):
        rules.append(Rule("PT_" + t, re.compile(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(t)),
                          None, "preserved term must stay verbatim", "preserve", kind="preserve", term=t))
    return rules


class Allowlist(object):
    def __init__(self, entries):
        self.entries = []
        for e in entries:
            if not e.get("rule") or not (e.get("reason") or "").strip():
                raise ValueError("allowlist entry needs 'rule' and non-empty 'reason': %r" % (e,))
            self.entries.append((e["rule"], glob_to_regex(e.get("path", "**")),
                                 re.compile(e["pattern"]) if e.get("pattern") else None, e["reason"]))

    def allows(self, hit):
        for rule, path_rx, pat, _reason in self.entries:
            if rule != "*" and rule != hit.rule:
                continue
            if not path_rx.match(hit.path or ""):
                continue
            if pat is not None and not pat.search(hit.en):
                continue
            return True
        return False


def lint_pair(rules, en, zh, path="", line=None):
    """Return hits for one (English, Chinese) pair."""
    hits = []
    if en == zh:
        return hits  # untouched text: nothing to judge
    for r in rules:
        if r.kind == "preserve":
            if r.en.search(en) and not r.en.search(zh):
                hits.append(Hit(r.id, path, line, en, zh, r.note))
        else:
            if r.en.search(en) and r.zh_forbidden.search(zh):
                hits.append(Hit(r.id, path, line, en, zh, r.note))
    return hits


# ---- PO parsing (minimal, standard library only) --------------------------

_PO_STR = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def parse_po(text):
    """Yield (msgid, msgstr, line) for translated, non-obsolete entries."""
    entries = []
    cur = {}
    key = None
    start_line = None

    def flush():
        if cur.get("msgid") is not None and "msgid" in cur:
            if cur["msgid"] == "":
                return  # header
            if "msgstr" in cur:
                entries.append((cur["msgid"], cur["msgstr"], start_line))
            for k in sorted(k for k in cur if k.startswith("msgstr[")):
                entries.append((cur.get("msgid_plural", cur["msgid"]), cur[k], start_line))

    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            if line.startswith("#~"):
                continue  # obsolete
            if not line and cur:
                flush()
                cur = {}
                key = None
            continue
        m = re.match(r"\A(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(.*)\Z", line)
        if m:
            key = m.group(1)
            if key == "msgid" and cur.get("msgid") is not None:
                flush()
                cur = {}
            if key == "msgid":
                start_line = ln
            rest = m.group(2)
            cur[key] = ""
            line = rest
        if key is None:
            continue
        for sm in _PO_STR.finditer(line):
            cur[key] += _unescape(sm.group(1))
    if cur:
        flush()
    return entries
