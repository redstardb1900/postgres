# `--cc-check` evidence input (schema version 1)

`verify_c_comments.py --cc-check` is the second, compiler-backed proof that
a translated `.c/.h/.cpp` file preprocesses to the same token stream as the
baseline.  It can only run against evidence captured from the frozen GNU
Make VPATH build of `REL_16_11_base` (plan step 1):

* `make -j1 -w world-bin` and `make -j1 -w checkprep` logs (non-interleaved),
* the `.o` / `.deps/*.Po` coverage set,
* one canonical argv per translation-unit **variant** (the same source
  compiled with different flags is a different variant; never de-duplicated),
* `CC`/`CXX` absolute paths and versions, `make --version`, the complete
  configure arguments,
* the baseline token hashes.

**Status of this tool version:** the evidence does not exist yet, so
`--cc-check` always reports `UNVERIFIED` and the exit code is 2.  It never
reports success without evidence.  The replay (argv rewriting of
`-I/-iquote/-isystem/-idirafter/-include/-imacros/--sysroot` relative to the
recorded cwd, removal of `-c/-o/-MMD/-MP/-MF/-MT/-MQ`, appending
`-E -P -MMD -MF <side.d> -MT <variant-key> -o <side.i>`, prefix maps and token
comparison) will be added together with the baseline batch that produces the
evidence.

## Layout

```
cc_evidence/
  baseline.json          see schema.json; "status" must be "frozen"
  commands.jsonl         one JSON object per TU variant
  tokens/<variant_key>.json
```

`commands.jsonl` record:

```json
{"variant_key": "src/backend/access/heap/heapam.c#0",
 "source": "src/backend/access/heap/heapam.c",
 "cwd": "/abs/builddir/src/backend/access/heap",
 "argv": ["/usr/bin/gcc", "-Wall", "...", "-c", "-o", "heapam.o", "/abs/srcdir/src/backend/access/heap/heapam.c"],
 "object": "heapam.o",
 "po_file": ".deps/heapam.Po",
 "headers": ["src/include/postgres.h", "..."]}
```

`tokens/<variant_key>.json`:

```json
{"variant_key": "...", "token_sha256": "...", "n_tokens": 123456,
 "depfile_headers": ["src/include/postgres.h", "..."]}
```

Rules the future replay must enforce (from the plan):

* accept only a single command starting with the frozen compiler absolute
  path; `&&`, `;`, pipes, redirections, response files, ccache/distcc/sccache
  or unknown wrappers → `UNVERIFIED`;
* every active C/C++ object must have `.o`, `.Po` and a complete argv, else
  the whole check is `UNVERIFIED`;
* generated TUs whose source lives in the builddir (`gram.c`, `scan.c`,
  `pl_gram.c`, `preproc.c`, `pgc.c`, `sql_help.c`, `fmgrtab.c`, ...) are not
  token-compared here;
* every translated header must be covered by at least one passing variant.
