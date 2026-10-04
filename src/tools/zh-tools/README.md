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
| `verify_c_comments.py` | `.c/.h/.cpp` 的"只有注释变了"证明：字节级 C 词法状态机（续行、字符串/字符、C++ raw string、`#include <>`、`#error/#warning`），骨架逐字节比较，注释类型/LF/CR 规则，UTF-8/BOM，受保护注释（`translator:`、fall-through、`pg_attribute_*`、`/*-` pgindent 块、Copyright/License），trigraph 基线保留/新增拒绝 |
| `check_scope.py` | 批次范围门禁：改动必须全部落在 manifest 的 `allowed_paths` 内并逐文件登记；覆盖 tracked、untracked、ignored；`doc/` 按递归快照和 tree OID 零变化；Perl 保护集按 blob OID 与 mode 零变化。新增 Perl 文件（扩展名或 shebang）的扫描始终相对冻结基线 REL_16_11 覆盖全部新增/修改路径及所有 untracked/ignored 文件，与 `--incremental` 无关 |
| `term_lint.py` | 保守术语检查：读取 pgnls `ref/phrasebook.tsv`（`status=decided` 且有 `zh_forbidden`）、`ref/glossary.tsv` 中"译文=原文"的保留词，以及 `term_rules.json`；只报告"英文命中且中文命中禁用译法"或"保留词丢失"的明确命中；规则分 `error`/`warning` 两级，只有 `error` 硬失败；支持带理由的 allowlist；不做任何自动替换 |
| `gen_baseline.py` | 从只读基线 worktree 冻结保护数据：`baseline/baseline.json`、`baseline/doc_snapshot.json`、`baseline/perl_protected.json`；`--verify` 可重新生成并与已提交数据比对。快照与 Perl 比对的文件读取均经 `os.open(O_NOFOLLOW)` + `fstat`，哈希前后复核 dev/ino/size/mode/mtime；普通文件的 perm/mode/size/sha256 全部取自同一个哈希 fd 的 fstat，不混用打开前的 lstat；symlink 只记录 link target 不跟随 |
| `zhtools/` | 共享库：`c_lexer`（词法）、`c_verify`（规则）、`gitutil`、`protection`（快照/Perl 集）、`manifest`（schema v1 与 glob）、`termlint`、`cc_check`（接口占位） |
| `manifests/` | `SCHEMA.md`（manifest schema v1）、`scope_template.json`（模板）、`batch-NNNN-*.json`（每批一份） |
| `cc_evidence/` | `--cc-check` 证据输入的 schema 与说明（本版本仅接口） |
| `tests/` | `unittest` 套件与固定正例/负例（`tests/fixtures/`） |
| `run_tests.py` | 运行全部测试 |

## 唯一证据链：GNU Make VPATH

构建语义只由一套冻结配置证明：以 `REL_16_11_base` 为唯一 srcdir、在 worktree
之外建立的 GNU Make VPATH build（`--enable-depend --enable-nls` 及冻结后的完整
configure 参数，`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++`）。compile command、
条件宏、生成头、POT、Bison/Flex 产物和 `make check-world` 全部来自这一配置。
不创建 Meson builddir、不运行 Meson/Ninja、不维护第二套配置映射。

`verify_c_comments.py --cc-check` 依赖该 build 捕获的 `make -j1 -w world-bin` /
`make -j1 -w checkprep` 日志、`.o/.Po` 覆盖集和规范 argv（见
`cc_evidence/README.md`）。**本版本尚未建立该基线，`--cc-check` 一律返回
`UNVERIFIED`、退出码 2，不会伪装成功。**

## 基线

| 项 | 值 |
|---|---|
| 基线 tag / commit | `REL_16_11` = `d61dd817be70749d14e982a369e97fdda9d5cba6` |
| `REL_16_11:doc` tree | `5225740e19cc8738649577b6eb799b087519b7ad` |
| 基线 worktree（只读 detached） | `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base` |
| 目标 worktree / 分支 | `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_zh` / `REL_16_11_zh` |
| pgnls 术语库 | `/Users/niejinbo/work/code/pg/pgsty/pgnls` @ `cbaeef3eb8be500984ab3da9f3e251634f346d1c` |
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
python3 $T/verify_c_comments.py --worktree REL_16_11 . --cc-check   # 目前必然退出 2

# 最终验收：两个 ref
python3 $T/verify_c_comments.py --git REL_16_11 REL_16_11_zh --json /tmp/vcc-final.json

# 范围 / doc / Perl 门禁
#   每批（增量：manifest 对照 parent_commit 的 diff；doc/、Perl 保护集与新增 Perl 扫描始终对冻结基线绝对比较）
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

`verify_c_comments.py` 规则 ID（R1 骨架、R2 类型、R3 注释体安全、R4 编码/CR、
R5 受保护注释、R6 LF 数、R8 trigraph、R9 解析/文件；W6/W7/W9 为告警）见
`zhtools/c_verify.py` 开头说明；`check_scope.py` 的 S1–S4 见其 docstring。

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
