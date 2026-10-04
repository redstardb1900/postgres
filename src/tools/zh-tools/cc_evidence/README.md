# `--cc-check` evidence (schema version 2)

`verify_c_comments.py --cc-check` is the compiler-backed proof that a
translated `.c/.h/.cpp` file preprocesses to the same token stream as the
baseline, for **every** translation-unit variant that includes it.  The
evidence in this directory is produced only by `make_evidence.py` from the
frozen GNU Make VPATH build described in `../evidence/environment.md`; the
order is

```
environment -> configure -> make <capture targets> -> collect -> replay <base> -> check-world -> freeze
```

and `verify_c_comments.py --cc-check` replays the affected variants against
the current tree and compares token hashes with `tokens.jsonl`.

## Layout

| file | in repo | contents |
|---|---|---|
| `baseline.json` | yes | see `schema.json`; `status` must be `frozen`; carries the indivisible cache key |
| `closure.json` | yes | the `.o` / `.Po` / argv three-way closure and everything that needed a rule to close it |
| `commands.jsonl` | yes | one record per variant: `variant_key` (builddir-relative object path), `cwd`, `source`, `source_kind`, `generated`, `lang`, `flagset`, `log`, `object_tok`, `dep_tok`, `source_tok`, `po_file`, `po_inherited_from` |
| `flagsets.json` | yes | deduplicated argv templates (`{SRC}`, `{OBJ}`, `{DEP}` placeholders) |
| `tokens.jsonl` | yes | per-variant hashes of the base replay (`token_sha256`, `canonical_argv_sha256`, `headers_sha256`, counters); byte-identical across replays |
| `replay_base.json` | yes | base replay summary |
| `platform_exceptions_candidates.json` | yes | uncovered tracked files, status always `candidate` |
| `platform_exceptions.json` | optional | reviewed exceptions only (`status: approved` with `path`, `platform_guard`, `reason`, `reviewed_by`, `review_commit`); absent in the baseline commit |
| `<evidence_out>/cc_bulk/po_headers.jsonl` | no (sha in `closure.json`) | header list of every Make `.Po` |
| `<evidence_out>/replay-base/headers.jsonl` | no (sha in `baseline.json`) | header list of every variant's side depfile |
| `<evidence_out>/*.log` | no (sha in `../evidence/*.json`) | the raw configure / make / check-world logs |

## How a variant is accepted (`zhtools/makelog.py`)

* Logs come from `make -j1 -w <target>`; `-w` Entering/Leaving lines maintain
  the cwd stack, recipe lines ending in `\` are joined.
* A compile command is accepted only if, after POSIX `shlex` splitting, it is
  **one** command whose argv[0] is the frozen `/usr/bin/gcc` or `/usr/bin/g++`
  and contains `-c`.  `&&`, `;`, `|`, redirections, `@response` files,
  `ccache`/`distcc`/`sccache`/`env`/`sh` wrappers, `-x`, joined `-o` and
  unknown shapes make the finding `UNVERIFIED` (collection refuses to proceed).
* The variant key is the builddir-relative path of the `-o` object.  The same
  source compiled with different flags (`foo.o`, `foo_shlib.o`, `foo_srv.o`)
  is a different variant; nothing is de-duplicated.
* Closure: every active `.o` under the builddir (minus `tmp_install`) must
  have exactly one argv and a `.Po`; every argv its `.o`; every `.Po` its
  `.o`.  Three shapes needed an explicit rule and are listed in
  `closure.json`:
  * `*_shlib.o` / `*_srv.o` built by the `%_shlib.o: %.c %.o` style rules
    have no `.Po` of their own; they inherit the `.Po` of the base object
    **only** if the base `.Po` exists and names the same source
    (`po_inherited`);
  * `src/test/isolation/pg_regress.o` is a Make-created symlink to
    `src/test/regress/pg_regress.o` (`object_symlinks`);
  * `src/backend/utils/probes.o` is produced by `dtrace -G`, not a compiler
    (`foreign_objects`); it has no TU.
* Sources that live in the builddir (bison/flex output, `fmgrtab.c`,
  `sql_help.c`, ecpg's `.pgc`-generated `.c`, ...) are variants with
  `source_kind: build` / `generated: true`.  Their tokens are hashed like
  every other variant, but no `src` file maps to them, so a translated
  `.y`/`.l` is **not** proven by this check (G rule); only headers they
  include are.

## How a variant is replayed (`zhtools/ccreplay.py`)

* Every path-valued argument (`-I`, `-iquote`, `-isystem`, `-idirafter`,
  `-include`, `-imacros`, `--sysroot`, the source operand) is classified
  against the recorded cwd as `src` (under srcdir), `build` (under builddir)
  or `sys`.  `src` paths are re-rooted to the target tree, `build` and `sys`
  paths are kept; nothing is string-substituted blindly.
* `-c`, `-o`, `-MMD`, `-MP`, `-MF`, `-MT`, `-MQ` are removed and
  `-E -P -MMD -MF <side.d> -MT <variant_key> -o -` appended, plus
  `-fmacro-prefix-map`/`-ffile-prefix-map` of both srcdir and target root to
  `SRC` so `__FILE__` and `#line` strings agree.
* The preprocessed output is canonicalised by collapsing whitespace runs to a
  single space (GCC spacing encodes token boundaries) and hashed with
  SHA-256; the side depfile is normalised to `SRC/`, `BUILD/`, system paths
  and hashed separately (`headers_sha256`).
* `freeze` checks that every side-depfile header set is a subset of the
  variant's **own** Make `.Po` (no variant sees a header Make did not
  record); a violation there blocks the freeze.  Variants whose `.Po` is
  inherited (`_shlib.o`, `_srv.o`) are compared against the base object's
  `.Po` and any extra headers are *recorded* in `baseline.json["po_superset"]
  ["inherited_not_superset"]` instead: `_srv.o` is compiled without
  `-DFRONTEND` and pulls in `postgres.h`/`utils/elog.h`/`lib/stringinfo.h`
  that the frontend object never included.  In the REL_16_11 baseline this
  concerns 35 of the 131 inherited variants, all `_srv.o`; the side depfile
  the compiler wrote for exactly that argv is the header record cc-check
  indexes, so a translated `elog.h` still selects those variants.
* Cache key (`CACHE_KEY_FIELDS`): base commit, configure args, env, cc/cxx/make
  versions, every log SHA, the closure/commands/flagsets/po_headers/tokens
  SHAs and the token-cache SHA.  Any change produces a new key and discards
  every cached verdict.

## Verdicts (`zhtools/cc_check.py`)

| result | exit | meaning |
|---|---|---|
| `verified` | 0 | every changed `.c/.h/.cpp` is covered by at least one variant and every selected variant replays to the baseline token hash |
| `violation` | 1 | at least one variant's token hash differs |
| `unverified` | 2 | evidence missing/draft/hash mismatch, tool versions differ, a replay failed, or a changed file is covered by no variant and has no approved platform exception |

`platform_exceptions_candidates.json` is advisory only.  A file is excepted
only through a reviewed `platform_exceptions.json` entry with status
`approved`; the baseline commit ships none, so every uncovered translated
file is `unverified` until a reviewer decides otherwise.

Not implemented in this version: a token-level proof for translated `.y`/`.l`
files (would need bison/flex re-generation under the frozen configuration,
then comparison of the generated TUs).
