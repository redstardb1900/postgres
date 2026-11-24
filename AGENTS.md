# AGENTS.md — REL_16_11_zh 中文化分支工作规则（任何 agent 自动加载）

本分支是 PostgreSQL 16.11 的源码注释/PO 中文化分支。**总交接文档：`/home/redstar/pg/HANDOVER-FINAL-20261119.md`（仓库外）**；工具链详解：`src/tools/zh-tools/README.md`。

## 铁律（违者批次作废）

1. **只有注释变，代码一个字节不动**；行数守恒 1:1
2. 每行 zh 的 UTF-8 字节数 ≤ 对应 EN 行；注释块总和 ≤ EN 块总（预算权威=战役基线 worktree `/home/redstar/pg/postgres.worktrees/REL_16_11_base` 对应行，不是 HEAD）
3. 逐行预算不足时**等义压缩并披露，禁丢语法成分**（主语/宾语/限定语/极性）
4. 行首标点禁；跨行断句后整句拼读回读（防叠词与语义反转）
5. apply 底稿必须用 HEAD 工作树（含已译区），禁用 EN dump 当整文件底稿
6. 不采信任何 apply/自查打印——逐站 sed 回读复核
7. 同 EN 必同译；跨批先 grep 既译文件取同款术语
8. 批量脚本每轮 `git checkout HEAD -- <file>` 复位后只跑一次

## 门禁（每批必跑；python 一律 `-B`，`-I` 会忽略 PYTHONDONTWRITEBYTECODE）

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B -I src/tools/zh-tools/verify_c_comments.py --worktree HEAD . --paths <触达路径>
PYTHONDONTWRITEBYTECODE=1 python3 -B -I src/tools/zh-tools/term_lint.py --worktree HEAD . --paths <触达路径> --pgnls /home/redstar/pg/pgsty/pgnls
PYTHONDONTWRITEBYTECODE=1 python3 -B -I src/tools/zh-tools/check_scope.py --worktree . --base HEAD --incremental --manifest src/tools/zh-tools/manifests/batch-NNNN-*.json
PYTHONDONTWRITEBYTECODE=1 python3 -B -I src/tools/zh-tools/scan_halfen.py --worktree HEAD .
```

每批写 manifest（格式见 `src/tools/zh-tools/manifests/SCHEMA.md`，照抄最近批次三段结构：源文件 c_comment 条目 + 本 manifest + cumulative.json）；cumulative.json **纯追加**（触达文件已在册则只加 manifest 条目）；base_commit=`d61dd817be70749d14e982a369e97fdda9d5cba6`（战役基线）、parent_commit=当前 HEAD 全量 SHA、pgnls_commit=`cbaeef3eb8be500984ab3da9f3e251634f346d1c`。

## 提交规范

- 信息：简体中文，`zh: <目录> <内容> — batch NNNN（审查/清偿要点）`
- `git add` 目录级或逐一全列；提交前 `git status` 核对 staged；pre-commit 钩子自动跑 check_scope（约 75 秒）
- 只提交 manifest allowed_paths 覆盖的文件；游离 `__pycache__` 会触发 S2 拦截（删掉即可）

## 审查闭环

每批：翻译（不 commit）→ 主会话独立复跑门禁 → **只读审查 agent**（禁 stash/checkout/写仓库；独立复跑门禁贴 rc；全量配对走读；行号 sed 回读自证）→ FAIL 清偿 / PASS 提交。审查文案预算需现算（±1-3B 偏差高频），审查行号落地前必须现场重定位。

## 下一战役

**现行交接：** `/home/redstar/pg/HANDOVER-20261124-REL_16_11_zh.md`（仓库外）。  
FINAL 第七节 regex 未开工已过时（0436–0441 已完）；adt 至 0635、utils/mb 至 0636、backup 至 0637 收口。

**建议下一战役：** `src/backend/port/` → `jit` → 小件（foreign/bootstrap/archive）→ PL/客户端/contrib。  
暂缓：`ppport.h`、libstemmer、测试期望、`gram.y`/`scan.l`/flex·bison（详见交接 §4）。
