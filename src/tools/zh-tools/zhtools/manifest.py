"""Scope manifest loading, validation and glob matching (schema version 1)."""
import json
import re
import sys

sys.dont_write_bytecode = True

MANIFEST_SCHEMA_VERSION = 1

KINDS = ("tool", "c_comment", "yl_comment", "makefile_comment", "meson_comment",
         "shell_comment", "readme", "po", "manifest", "baseline")
ACTIONS = ("add", "modify")
_HEX40 = re.compile(r"\A[0-9a-f]{40}\Z")


class ManifestError(ValueError):
    pass


def glob_to_regex(pattern):
    """Translate a path glob with ``**`` support into a compiled regex.

    ``**/`` matches zero or more directory components, a trailing ``/**``
    matches everything below a directory, ``*`` and ``?`` never cross ``/``.
    """
    i = 0
    n = len(pattern)
    out = []
    while i < n:
        c = pattern[i]
        if c == "*":
            if pattern[i:i + 3] == "**/":
                out.append("(?:[^/]+/)*")
                i += 3
                continue
            if pattern[i:i + 2] == "**":
                out.append(".*")
                i += 2
                continue
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.compile("\\A" + "".join(out) + "\\Z")


class Manifest(object):
    def __init__(self, data, source="<manifest>"):
        self.source = source
        self.data = data
        self._validate()
        self.allowed = [(a["pattern"], glob_to_regex(a["pattern"])) for a in data["allowed_paths"]]
        self.files = {f["path"]: f for f in data.get("files", [])}

    def _validate(self):
        d = self.data
        if not isinstance(d, dict):
            raise ManifestError("manifest must be a JSON object")
        if d.get("schema_version") != MANIFEST_SCHEMA_VERSION:
            raise ManifestError("schema_version must be %d" % MANIFEST_SCHEMA_VERSION)
        for key in ("batch_id", "title", "base_commit", "pgnls_commit", "allowed_paths"):
            if key not in d:
                raise ManifestError("missing key %r" % key)
        for key in ("base_commit", "pgnls_commit"):
            if not _HEX40.match(str(d[key])):
                raise ManifestError("%s must be a 40-hex commit id" % key)
        if d.get("parent_commit") is not None and not _HEX40.match(str(d["parent_commit"])):
            raise ManifestError("parent_commit must be a 40-hex commit id")
        if not isinstance(d["allowed_paths"], list) or not d["allowed_paths"]:
            raise ManifestError("allowed_paths must be a non-empty list")
        for a in d["allowed_paths"]:
            if not isinstance(a, dict) or not a.get("pattern") or a.get("kind") not in KINDS:
                raise ManifestError("bad allowed_paths entry: %r" % (a,))
            if a["pattern"].startswith("/") or a["pattern"].startswith("doc/") or a["pattern"] == "**":
                raise ManifestError("allowed pattern not acceptable: %r" % a["pattern"])
        seen = set()
        for f in d.get("files", []):
            if not isinstance(f, dict) or not f.get("path"):
                raise ManifestError("bad files entry: %r" % (f,))
            if f["path"] in seen:
                raise ManifestError("duplicate files entry: %s" % f["path"])
            seen.add(f["path"])
            if f.get("kind") not in KINDS:
                raise ManifestError("files entry %s: bad kind %r" % (f["path"], f.get("kind")))
            if f.get("action") not in ACTIONS:
                raise ManifestError("files entry %s: bad action %r" % (f["path"], f.get("action")))
            for k in ("segments_total", "translated", "preserved"):
                if not isinstance(f.get(k), int) or f[k] < 0:
                    raise ManifestError("files entry %s: %s must be a non-negative int" % (f["path"], k))
            if f["translated"] + f["preserved"] > f["segments_total"]:
                raise ManifestError("files entry %s: translated+preserved exceeds segments_total" % f["path"])
            if f["preserved"] and not f.get("preserved_reasons"):
                raise ManifestError("files entry %s: preserved>0 requires preserved_reasons" % f["path"])
            if not self.data_allows(f["path"]):
                raise ManifestError("files entry %s is outside allowed_paths" % f["path"])

    def data_allows(self, path):
        for a in self.data["allowed_paths"]:
            if glob_to_regex(a["pattern"]).match(path):
                return True
        return False

    @property
    def require_file_entries(self):
        return bool(self.data.get("require_file_entries", True))

    @property
    def parent_commit(self):
        return self.data.get("parent_commit")

    def allows(self, path):
        """Return the matching pattern or None."""
        for pat, rx in self.allowed:
            if rx.match(path):
                return pat
        return None

    def file_entry(self, path):
        return self.files.get(path)


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        try:
            data = json.load(f)
        except ValueError as e:
            raise ManifestError("%s: invalid JSON: %s" % (path, e))
    return Manifest(data, source=path)
