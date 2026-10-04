# Scope manifest schema (schema_version 1)

Every translation batch commits exactly one manifest under
`src/tools/zh-tools/manifests/batch-NNNN-<name>.json`.  `check_scope.py`
rejects any changed, untracked or ignored path that the manifest does not
explicitly allow, so the manifest *is* the closed scope of the batch.

```json
{
  "schema_version": 1,
  "batch_id": "0001-tools",
  "title": "short human title",
  "base_commit": "<40-hex frozen base commit (REL_16_11)>",
  "parent_commit": "<40-hex parent of this batch; optional, used by --incremental>",
  "pgnls_commit": "<40-hex pgnls commit used for terminology>",
  "require_file_entries": true,
  "allowed_paths": [
    {"pattern": "src/tools/zh-tools/**", "kind": "tool", "note": "why"}
  ],
  "files": [
    {
      "path": "src/backend/access/heap/heapam.c",
      "kind": "c_comment",
      "action": "modify",
      "segments_total": 412,
      "translated": 380,
      "preserved": 32,
      "preserved_reasons": ["copyright header", "translator: comments", "fall-through"],
      "note": "optional free text"
    }
  ],
  "notes": "optional free text"
}
```

Field rules (enforced by `zhtools/manifest.py`):

| field | rule |
|---|---|
| `schema_version` | must be `1` |
| `base_commit`, `pgnls_commit` | 40 hex chars; must equal `baseline/baseline.json` |
| `parent_commit` | optional, 40 hex chars; the commit this batch is diffed against in `check_scope.py --incremental` (must descend from `base_commit`). Without `--incremental` the diff base is always `base_commit` and the manifest must be cumulative |
| `allowed_paths[].pattern` | path glob; `**` crosses directories, `*`/`?` do not; must not start with `/` or `doc/`, must not be `**` |
| `allowed_paths[].kind`, `files[].kind` | one of `tool`, `c_comment`, `yl_comment`, `makefile_comment`, `meson_comment`, `shell_comment`, `readme`, `po`, `manifest`, `baseline` |
| `files[].action` | `add` or `modify`; must agree with the git status of the path |
| `files[].segments_total/translated/preserved` | non-negative ints, `translated + preserved <= segments_total` |
| `files[].preserved_reasons` | required when `preserved > 0` |
| `files[].path` | must match an `allowed_paths` pattern; no duplicates |
| `require_file_entries` | default `true`; when true every changed path (tracked, untracked **and** ignored) needs a `files[]` entry |

What a "segment" is depends on `kind`: a comment for `c_comment`,
`yl_comment`, `makefile_comment`, `meson_comment`, `shell_comment`; a
paragraph for `readme`; a message for `po`.  For `tool`, `manifest` and
`baseline` files use `0/0/0`.

Hard exclusions are not expressible in a manifest: `doc/**` and the Perl
protected set (`baseline/perl_protected.json`) are always checked
independently of `allowed_paths`.

`manifests/scope_template.json` is a copy-and-fill template.
