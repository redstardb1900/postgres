# zh-tools -- REL_16_11_zh 翻译校验工具

本目录是分支 `REL_16_11_zh`（PostgreSQL 16.11 源码注释 / README / `zh_CN.po`
中文化）的校验工具集。工具只证明可机械判定的事实：注释之外的字节是否相同、
注释结构与行数是否相同、改动是否越出批次范围、术语规则是否明确命中。它们不
重写编译器、bison/flex、gettext 或构建系统的语义。

全部代码为 Python 3.9+ 标准库，无 PyPI 依赖，不生成 `__pycache__`
（每个模块设置 `sys.dont_write_bytecode = True`；命令行调用时请同时设置
`PYTHONDONTWRITEBYTECODE=1`）。

## 职责

| 文件 | 职责 |
|---|---|
| `verify_c_comments.py` | `.c/.h/.cpp` 的"只有注释变了"证明：字节级 C 词法状态机（续行、字符串/字符、C++ raw string、`#include <>`、`#error/#warning`），骨架逐字节比较，注释类型/LF/CR 规则，UTF-8/BOM，受保护注释（`translator:`、fall-through、`pg_attribute_*`；文件头仅冻结 Copyright/IDENTIFICATION/License 尾段，模块说明可译；`/*-` alone 不再整段保护），trigraph 基线保留/新增拒绝 |
| `check_scope.py` | 批次范围门禁：改动必须全部落在 manifest 的 `allowed_paths` 内并逐文件登记；覆盖 tracked、untracked、ignored；`doc/` 按递归快照和 tree OID 零变化；Perl 保护集按 blob OID 与 mode 零变化；源码树全部 `*.txt` 相对 REL_16_11 零变化（改/增/删均失败，S5；`src/tools/zh-tools/` 下的工具夹具除外）。新增 Perl 文件（扩展名或 shebang）以及源码树 `*.txt` 的扫描始终相对冻结基线 REL_16_11 覆盖全部新增/修改路径及所有 untracked/ignored 文件，与 `--incremental` 无关 |
| `term_lint.py` | 保守术语检查：读取 pgnls `ref/phrasebook.tsv`（`status=decided` 且有 `zh_forbidden`）、`ref/glossary.tsv` 中"译文=原文"的保留词，以及 `term_rules.json`；只报告"英文命中且中文命中禁用译法"或"保留词丢失"的明确命中；规则分 `error`/`warning` 两级，只有 `error` 硬失败；支持带理由的 allowlist；不做任何自动替换 |
| `gen_baseline.py` | 从只读基线 worktree 冻结保护数据：`baseline/baseline.json`、`baseline/doc_snapshot.json`、`baseline/perl_protected.json`；`--verify` 可重新生成并与已提交数据比对。快照与 Perl 比对的文件读取均经 `os.open(O_NOFOLLOW)` + `fstat`，哈希前后复核 dev/ino/size/mode/mtime；普通文件的 perm/mode/size/sha256 全部取自同一个哈希 fd 的 fstat，不混用打开前的 lstat；symlink 只记录 link target 不跟随 |
| `make_evidence.py` | GNU Make 基线证据链 CLI：`environment`（机器/工具/locale/文件系统探测）、`configure`（全新源码树外 VPATH builddir）、`make NAME`（按 `frozen_config.json` 的 capture target 以 `-j1 -w` 捕获日志）、`check-world`（固定 `-j8 -k -Orecurse -w`，结构化结果）、`resummarize`（日志 SHA 不变时重新派生摘要）、`collect`（`.o/.Po/argv` 三方闭合 + flagset 模板）、`replay ROOT [--compare]`（`-E -P -MMD` 回放，token 哈希）、`freeze`（校验全部证据一致——含仓库外 make 捕获日志、`configure.log`、`config.log`、check-world 日志的 SHA-256——后写 `cc_evidence/baseline.json`）、`freeze --verify`（只读：从磁盘证据重算 baseline 与平台候选表并与已提交文件比对，不写任何文件）。平台例外候选在两种模式下都只做分类报告，从不审批 |
| `po_sync.py` | `zh_CN.po` 门禁（计划第 3 步）：`probe`（msgattrib `--only-fuzzy/--untranslated` 无匹配时 stdout 零字节的正反探针）、`archive`（把一次性 in-tree worktree 中 26 个 `make -C <dir> init-po` 生成的 POT 复制到仓库外，记录原始 SHA-256、去 `POT-Creation-Date` 的规范 hash、active-key hash、条目数、命令和 gettext/bison/flex/perl 版本，并硬检查 POT 引用与生成源输入）、`merge`（规定管线 `msgmerge --quiet --no-wrap --previous --sort-by-file --lang=zh_CN [-C 补译] <pgnls.po> <pot>` → `msgattrib --no-obsolete --no-wrap`；先无补译跑一遍记录 gettext 自身留下的 fuzzy/未译，再以 `po_supplement/<catalog>.po` 为精确匹配 compendium 跑第二遍；仅当补译键在 pgnls 中只以 `#~` 过时条目存在时才做第三步覆盖并由 msgattrib 重新序列化）、`check`（`msgfmt --check --check-format --statistics`、`msgcmp`、零字节证明 + 独立解析的 0 fuzzy/0 未译/无 obsolete、无重复 active key、复数完整、header `Language: zh_CN`/UTF-8/`Plural-Forms`、active key 集合与 POT 相同、补译条目逐条被使用且带 `# zh-reviewed:` 标记、目录与补译均不得残留 `# zh-review-pending`；`--pgnls` 时对补译跑 `term_lint.py --po` 作门禁、对整文件只做信息报告）、`report`（pgnls↔POT↔结果的新增/obsolete/pass1 fuzzy/未译/补译/与 REL_16_11 基线差异；`review_pending` 必须为空，`review_reviewed` 对应全部补译） |
| `po_supplement/` | 手工补译（compendium）：仅含 REL_16_11 POT 中 pgnls `zh_CN/REL_16_STABLE` 没有精确译文的消息；每条带 `# zh-reviewed: <复核者> <复核编号> (<日期>)` 译者注释，说明来源与依据 |
| `zhtools/` | 共享库：`c_lexer`（词法）、`c_verify`（规则）、`gitutil`、`protection`（快照/Perl 集）、`manifest`（schema v1 与 glob）、`termlint`、`evidence_run`（冻结环境进程运行/日志 SHA/目录权限探针）、`makelog`（`-w` 日志解析、危险 shell 拒绝、闭合）、`ccreplay`（argv 改写、路径分类、token/depfile 规范化、cache key、freeze）、`checkworld`（check-world 结构化解析）、`cc_check`（`--cc-check` 判定）、`potools`（机械 PO 读取：键集合、重复、复数、header、规范/active-key hash、覆盖拼接；不模拟 gettext） |
| `manifests/` | `SCHEMA.md`（manifest schema v1）、`scope_template.json`（模板）、`batch-NNNN-*.json`（每批一份）、`cumulative.json`（自 REL_16_11 以来全部改动的累计 manifest，供最终验收） |
| `evidence/` | 冻结输入 `frozen_config.json`、人读说明 `environment.md`，以及 `make_evidence.py` 写出的 `environment.json`、`configure.json`、`make_*.json`、`check_world.json`、`replay_compare_*.json`（大日志留在仓库外，只记路径与 SHA-256）；`evidence/po/` 为 `po_sync.py` 的 `msgattrib_probe.json`、`pot_manifest.json`、`po_merge.json`、`po_check.json`、`po_sync_report.json`（POT 本体与 init-po 日志留在仓库外 `/home/redstar/pg-builds/REL_16_11_pot-evidence`） |
| `cc_evidence/` | `--cc-check` 证据：`baseline.json`（schema v2，含不可分割 cache key 与基线失败集）、`closure.json`、`commands.jsonl`、`flagsets.json`、`tokens.jsonl`、`replay_base.json`、`platform_exceptions_candidates.json`；规则见其 `README.md`、字段见 `schema.json` |
| `tests/` | `unittest` 套件与固定正例/负例（`tests/fixtures/`） |
| `run_tests.py` | 运行全部测试 |

## 唯一证据链：GNU Make VPATH

构建语义只由一套冻结配置证明：以 `REL_16_11_base` 为唯一 srcdir、在 worktree
之外建立的 GNU Make VPATH build（`--enable-depend --enable-nls` 及冻结后的完整
configure 参数，`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++`）。compile command、
条件宏、生成头、POT、Bison/Flex 产物和 `make check-world` 全部来自这一配置。
不创建 Meson builddir、不运行 Meson/Ninja、不维护第二套配置映射。

`verify_c_comments.py --cc-check` 依赖该 build 以 `make -j1 -w` 捕获的
`world-bin` / `checkprep` / 测试程序日志、`.o/.Po/argv` 三方闭合和规范 argv
（见 `cc_evidence/README.md`）。证据由 `make_evidence.py` 生成并 `freeze`；
`--cc-check` 在证据缺失、`status != frozen`、任一 SHA-256 或工具版本不一致、
回放失败、或改动文件不被任何变体覆盖且无已评审平台例外时一律返回
`UNVERIFIED`（退出码 2），token 哈希不同返回违规（退出码 1）。

```sh
# 一次证据周期（在 zh worktree 的 src/tools/zh-tools 下；全部参数来自 evidence/frozen_config.json）
python3 make_evidence.py environment
python3 make_evidence.py configure
for t in world_bin checkprep check_prereqs check_prereqs_ecpg check_prereqs_libpq check_prereqs_pg_bsd_indent; do
  python3 make_evidence.py make $t; done
python3 make_evidence.py collect
python3 make_evidence.py replay /Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base   # label base
python3 make_evidence.py replay . --label zh --compare                                            # 当前 zh 树
python3 make_evidence.py check-world
python3 make_evidence.py freeze
# 之后任何时候复核已提交基线是否仍由磁盘证据逐字段重现（只读，不写文件）
python3 make_evidence.py freeze --verify --json /tmp/freeze_verify.json
```

已知限制（本版本未实现，不会伪装通过）：翻译后的 `.y`/`.l` 文件没有 token 级
证明（生成 TU 只按 `G` 规则哈希，不与源文件对应）；平台例外只有候选表，没有任何
已批准条目；`--cc-check --git` 需要检出的工作树。

## 基线

| 项 | 值 |
|---|---|
| 基线 tag / commit | `REL_16_11` = `d61dd817be70749d14e982a369e97fdda9d5cba6` |
| `REL_16_11:doc` tree | `5225740e19cc8738649577b6eb799b087519b7ad` |
| 基线 worktree（只读 detached） | `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base` |
| 目标 worktree / 分支 | `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_zh` / `REL_16_11_zh` |
| pgnls 术语库 | `/Users/niejinbo/work/code/pg/pgsty/pgnls` @ `cbaeef3eb8be500984ab3da9f3e251634f346d1c` |
| Make builddir / prefix / 证据日志 | `/home/redstar/pg-builds/REL_16_11_base-{make,install,evidence}`（原生 btrfs；**不得**放在 virtiofs 的 `/Users` 下，原因见 `evidence/environment.md`） |
| `doc/` 快照 | 441 项（435 个 tracked 文件 + 目录），字段 path/type/mode/perm/size/sha256/target |
| Perl 保护集 | 284 个 blob：277 个 `*.pl/*.pm/*.t` + 7 个 shebang 识别文件（`src/backend/utils/Gen_dummy_probes.pl.prolog`、`src/include/catalog/{duplicate_oids,unused_oids}`、`src/test/recovery/t/cp_history_files`、`src/tools/{git_changelog,pginclude/pgcheckdefines,pgindent/pgindent}`） |

基线数据可用 `gen_baseline.py --verify` 复现核对。

## 退出码

所有工具统一：

| 码 | 含义 |
|---|---|
| 0 | 全部通过 |
| 1 | 存在违规 / 命中 |
| 2 | 未验证：解析错误、输入或 git 错误、pgnls SHA 不匹配、`--cc-check` 证据不可用 |

退出码 2 永远阻止提交，不得当作通过。

## 常用命令

```sh
export PYTHONDONTWRITEBYTECODE=1
cd /Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_zh
T=src/tools/zh-tools

# 单元测试
python3 $T/run_tests.py

# 解析器自检：基线全部 .c/.h/.cpp 与自身比较，要求 0 违规 0 解析错误
python3 $T/verify_c_comments.py --self-check ../REL_16_11_base --json /tmp/selfcheck.json

# 每批提交前：工作树 vs 基线（提交与验收禁用 --no-strict-lines）
python3 $T/verify_c_comments.py --worktree REL_16_11 . --paths src/backend/access --json /tmp/vcc.json
python3 $T/verify_c_comments.py --worktree REL_16_11 . --cc-check   # 回放改动文件涉及的全部变体；无 C 改动时 verified

# 最终验收：两个 ref
python3 $T/verify_c_comments.py --git REL_16_11 REL_16_11_zh --json /tmp/vcc-final.json

# 范围 / doc / Perl / *.txt 门禁
#   每批（增量：manifest 对照 parent_commit 的 diff；doc/、Perl 保护集、新增 Perl 与全部 *.txt 始终对冻结基线绝对比较）
python3 $T/check_scope.py --incremental --worktree . --manifest $T/manifests/batch-NNNN-name.json --json /tmp/scope.json
python3 $T/check_scope.py --incremental --ref HEAD --repo . --manifest $T/manifests/batch-NNNN-name.json
#   最终验收（累计，diff 基于 REL_16_11，manifest 必须覆盖自基线以来全部改动）
python3 $T/check_scope.py --ref REL_16_11_zh --repo . --manifest $T/manifests/cumulative.json

# 术语检查
python3 $T/term_lint.py --worktree REL_16_11 . --pgnls /Users/niejinbo/work/code/pg/pgsty/pgnls --json /tmp/term.json
python3 $T/term_lint.py --po src/backend/po/zh_CN.po --pgnls /Users/niejinbo/work/code/pg/pgsty/pgnls

# 重新核对基线数据
python3 $T/gen_baseline.py --base-worktree ../REL_16_11_base --pgnls /Users/niejinbo/work/code/pg/pgsty/pgnls --verify
```

### PO 同步（计划第 3 步）

```sh
PG=/Users/niejinbo/work/code/pg/pgsty/pgnls
POT_WT=/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_pot
POT_EV=/home/redstar/pg-builds/REL_16_11_pot-evidence

# 1. 一次性 detached worktree + in-tree configure（参数与 evidence/frozen_config.json 一致，prefix 放仓库外）
git -C /Users/niejinbo/work/code/pg/postgres worktree add --detach $POT_WT REL_16_11
(cd $POT_WT && env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 HOME=$HOME \
   ./configure --prefix=/home/redstar/pg-builds/REL_16_11_pot-install --enable-depend --enable-nls ... )
# 2. 逐目录 init-po（禁止顶层 distprep/init-po，禁止在 zh worktree 生成 POT）
(cd $POT_WT && for d in $(git ls-files | grep 'nls\.mk$' | xargs -n1 dirname); do make -j1 -w -C $d init-po; done)
# 3. 探针、归档、合并、门禁、报告（均在 zh worktree 内运行）
python3 $T/po_sync.py probe
python3 $T/po_sync.py archive --pot-worktree $POT_WT --out $POT_EV
python3 $T/po_sync.py merge  --pgnls $PG
python3 $T/po_sync.py check  --pgnls $PG
python3 $T/po_sync.py report --pgnls $PG
# 4. 归档 hash 确认后删除临时 worktree
git -C /Users/niejinbo/work/code/pg/postgres worktree remove --force $POT_WT && git worktree prune
```

`merge` 是确定性的：输入（pgnls SHA、归档 POT、`po_supplement/`、gettext 版本）不变则 26 个
`zh_CN.po` 逐字节相同。出现新的 fuzzy/未译时，不得保留英文或模糊译文：在
`po_supplement/<catalog>.po` 增加带 `# zh-reviewed: <复核者> <编号> (<日期>)` 注释的精确条目，重新
`merge`/`check`。残留 `# zh-review-pending` 视为未复核，门禁失败。pgnls 本身的译文是术语权威，
`check --pgnls` 对它们只报告 term_lint 命中数，对补译条目则要求 0 error。

`verify_c_comments.py` 规则 ID（R1 骨架、R2 类型、R3 注释体安全、R4 编码/CR、
R5 受保护注释、R6 LF 数、R8 trigraph、R9 解析/文件；W6/W7/W9 为告警）见
`zhtools/c_verify.py` 开头说明；`check_scope.py` 的 S1–S4 见其 docstring。

文件头注释策略：以 `/*-` 开头的 PostgreSQL 文件横幅**可以**翻译模块说明（文件名下一行的短标题、以及 Copyright/IDENTIFICATION 之前的 NOTES/实现说明）。从首个匹配 `Copyright` / `(c) YYYY` / `IDENTIFICATION` / license 许可语句的行起，直到注释结束的尾段必须逐字节保留；`translator:`、fall-through、`pg_attribute_*` 仍整段不可改。`/*-` 本身不再构成整段保护。


### 词法器的保守约定

- **偏移**：token 的原始结束偏移是"最后一个字节的原始偏移 + 1"，不是"下一个
  字节的原始偏移"；两者在 `*/` 后紧跟反斜杠换行时不同（`/* c */\` 续行），续行
  属于代码。行注释以终止换行符的原始偏移结束，注释内部的续行属于注释。
- **指令判定**：块注释跨行后紧随的 `#` 按预处理指令处理。GCC/Clang 只在注释本身
  位于行首时才这样做，但标准措辞有歧义；指令模式是保守方向（`#include <a//b.h>`
  成为 header-name 即代码，不会被当成可翻译的 `//` 注释）。
- **trigraph**：只做扫描，不执行 C 翻译阶段 1 的替换（PostgreSQL 以 GNU 模式编译，
  trigraph 关闭）。规则是"新版本不得新增任何 trigraph，基线含 trigraph 的注释整段
  原文保留"。这只保证翻译不改变 trigraph 集合；最终安全性由冻结的 GNU 编译命令
  （`-std=gnu*`、无 `-trigraphs`）与 `--cc-check` 的预处理 token 交叉证明。

### term_lint 严重级别

每条规则是 `error` 或 `warning`。匹配以整段（整条注释 / 整条消息）为单位，禁用
译法可能与同段中另一个词的正确译法合法共现（`requires`→需要 与 `requirement`→要求），
因此只有在任何 PostgreSQL 语境下都错误的译法才是 `error`（硬失败，退出 1）；
其余为 `warning`（报告但退出 0）。`term_rules.json` 每条必须显式声明 `severity`。
pgnls phrasebook 为单条 NLS 消息设计：`--po` 模式下为 `error`，注释/文本模式下
为 `warning`（`--phrasebook-severity` 可覆盖）。

## 工作流约定

- 工具的新增和每次修改必须与首次使用该版本的翻译批次同一提交；仓库外不保留
  未版本化副本作为实际执行版本。
- 每个翻译批次提交一份 `manifests/batch-NNNN-*.json`（含 `parent_commit`），并通过
  `check_scope.py --incremental`、`verify_c_comments.py`（含 `--cc-check` 建立后）和
  `term_lint.py` 后才提交。提交信息记录父 SHA、pgnls SHA、工具 hash、schema
  版本、命令和报告摘要。最终验收用累计 manifest 做不带 `--incremental` 的全量检查。
- 分支上不运行 pgindent。

## 首提交启动例外

首个工具提交 `zh: add translation validation tools` 只包含 `src/tools/zh-tools/`。
此时工具尚不存在于任何已提交版本，无法"用已提交工具检查自己"，因此该提交不要求
先通过历史工具，但必须：

1. 通过本目录全部 `unittest`；
2. 在基线 2319 个 `.c/.h/.cpp` 上 `--self-check` 得到 0 违规、0 解析错误；
3. 用本提交的 `check_scope.py` 对工作树通过范围检查；
4. 提交前用原生 git 命令独立确认：diff 仅涉及 `src/tools/zh-tools/**`，
   `git diff --quiet REL_16_11 -- doc`，`HEAD:doc` tree OID 等于基线，Perl
   保护集逐项 blob OID 不变。

之后的每个提交都必须先通过当时已提交版本的工具。
