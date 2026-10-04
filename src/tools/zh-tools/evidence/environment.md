# 冻结的 GNU Make 证据环境（REL_16_11 基线）

本文件记录计划"阶段 1：冻结范围和证据环境"中 Make VPATH 基线的全部可复现参数。
机器可读的对应物是同目录的 `frozen_config.json`（输入）与 `environment.json`、
`configure.json`、`make_*.json`、`check_world.json`（输出）。任何一项变化都意味着
一次新的证据周期：必须重新 configure、重新捕获日志、重新生成 token 缓存。

## 机器与工具

| 项 | 值 |
|---|---|
| OS | openEuler 22.03 (LTS-SP4)，Linux 7.0.14-orbstack（OrbStack 虚拟机，aarch64） |
| 文件系统 | srcdir 在 `/Users`（OrbStack virtiofs 挂载的 macOS 文件系统，`stat -f` 报告 `fuseblk`，只读使用）；builddir / prefix / 日志目录在虚拟机原生根文件系统 `/home/redstar/pg-builds`（btrfs）；`/tmp` 为 tmpfs |
| CC / CXX | `/usr/bin/gcc`、`/usr/bin/g++` = gcc (GCC) 10.3.1，aarch64-linux-gnu；是真实二进制（非 ccache/distcc/sccache 包装） |
| GNU Make | 4.3 |
| bison / flex | 3.8.2 / 2.6.4 |
| perl / python3 | 5.34.0（含 IPC::Run、Test::More，prove = TAP::Harness 3.44）/ 3.9.9（python3-devel） |
| gettext | 0.21.1 |
| git | 2.33.0 |
| dtrace | systemtap-sdt-devel 4.5 的 `/usr/bin/dtrace`（仅用于 `probes.h` 生成与 `probes.o`） |
| locale | 进程环境 `LANG=C.UTF-8`，无 `LC_ALL`；系统有 `C`、`C.utf8`、`POSIX`、`en_*`、`zh_CN.utf8` |

## 冻结的 configure 命令

srcdir = `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base`（detached，
HEAD 必须等于 `d61dd817be70749d14e982a369e97fdda9d5cba6`，且 `git status --ignored` 为空）。
builddir = `/home/redstar/pg-builds/REL_16_11_base-make`（源码树外，全新创建）。

```sh
env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 \
    HOME=/home/redstar SHELL=/bin/bash USER=redstar LOGNAME=redstar \
  /Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base/configure \
    --prefix=/home/redstar/pg-builds/REL_16_11_base-install \
    --enable-depend --enable-nls --enable-tap-tests --enable-cassert --enable-debug --enable-dtrace \
    --with-perl --with-python --with-openssl --with-gssapi --with-ldap --with-pam --with-uuid=e2fs \
    --with-libxml --with-libxslt --with-lz4 --with-icu --with-selinux \
    CC=/usr/bin/gcc CXX=/usr/bin/g++
```

环境以 `env -i` 构造，只含上表 6 个变量；`LC_ALL`、`LANGUAGE`、`TZ`、`TMPDIR`、`PGPORT`、
`PGHOST`、`PGDATA`、`PG_TEST_EXTRA`、`PROVE_FLAGS`、`MAKEFLAGS`、`CFLAGS`、`CPPFLAGS`、
`LDFLAGS`、`PKG_CONFIG_PATH` 一律不设置。

### 启用项及探测依据

| 选项 | 依据 |
|---|---|
| `--enable-depend` | 计划要求；产生 `.deps/*.Po` |
| `--enable-nls` | 计划要求；gettext 0.21.1 |
| `--enable-tap-tests` | 计划要求；perl IPC::Run 可用 |
| `--enable-cassert --enable-debug` | 让 `USE_ASSERT_CHECKING` 分支进入活跃预处理区域，扩大 token 覆盖；`-g` 不影响预处理 |
| `--enable-dtrace` | `/usr/bin/dtrace`（systemtap）可生成 `probes.h`，configure 探测通过 |
| `--with-perl` / `--with-python` | perl-devel 5.34、python3-devel 3.9.9 |
| `--with-openssl` | openssl-devel 1.1.1wa |
| `--with-gssapi` | krb5-devel 1.19.2 |
| `--with-ldap` | openldap-devel 2.6.0 |
| `--with-pam` | pam-devel 1.5.2 |
| `--with-uuid=e2fs` | util-linux-devel 2.37.2（`uuid/uuid.h`）；ossp 也存在但二选一 |
| `--with-libxml` / `--with-libxslt` | libxml2-devel 2.9.14 / libxslt-devel 1.1.37 |
| `--with-lz4` | lz4-devel 1.9.3 |
| `--with-icu` | libicu-devel 72.1 |
| `--with-selinux` | libselinux-devel 3.3（contrib/sepgsql 可编译；其测试需要启用 SELinux，check-world 不运行） |

### 禁用项及原因（不为追求覆盖安装新包）

| 选项 | 原因 |
|---|---|
| `--with-tcl` | 无 tcl-devel：`/usr/include/tcl.h`、`tclConfig.sh` 不存在（仅 runtime tcl-8.6.12） |
| `--with-zstd` | 无 zstd-devel：`/usr/include/zstd.h` 不存在（仅 runtime libzstd.so.1.5.0） |
| `--with-systemd` | 无 systemd-devel：`/usr/include/systemd/sd-daemon.h` 不存在 |
| `--with-llvm` | 无 llvm-devel：`llvm-config` 不存在（仅 llvm-libs-12.0.1 与 clang-12）；因此没有 `.bc` bitcode 产物 |
| `--with-bonjour` / `--with-bsd-auth` | 非 Linux API |
| `--with-system-tzdata` | 保持内置 `src/timezone` 构建进入基线 |
| `PG_TEST_EXTRA` | 不设置：无 `slapd`、`krb5kdc`；ssl/ldap/kerberos 套件在基线中为 skip |

configure 结果摘要见 `configure.json`（`config_status_summary`、`pg_config_h`），其中
`USE_LLVM`、`USE_ZSTD`、`USE_SYSTEMD`、`HAVE_UUID_OSSP` 为未定义，其余特性宏均已定义。

## 捕获命令（全部 `-j1 -w`，不交错）

| 记录 | 命令（在 builddir 中） | 用途 |
|---|---|---|
| `make_world_bin.json` | `make -j1 -w world-bin` | 主体编译命令 |
| `make_checkprep.json` | `make -j1 -w checkprep` | `EXTRA_INSTALL` 测试模块（src/test/modules、contrib 测试扩展） |
| `make_check_prereqs.json` | `make -j1 -w -C src/test all` | check-world 会以并行方式编译的测试程序（regress/isolation/modules） |
| `make_check_prereqs_ecpg.json` | `make -j1 -w -C src/interfaces/ecpg/test all` | ecpg 测试程序（`.pgc` → builddir `.c` 的生成 TU） |
| `make_check_prereqs_libpq.json` | `make -j1 -w -C src/interfaces/libpq test-build` | libpq 测试程序 |
| `make_check_prereqs_pg_bsd_indent.json` | `make -j1 -w -C src/tools/pg_bsd_indent all` | 只由其自身 TAP check 构建的 7 个对象 |

后四项是计划中 `world-bin` + `checkprep` 之外的补充：没有它们，`make check-world` 会在
`-j8` 下交错地编译这些对象，`.o/.Po/argv` 三方闭合无法成立（`pg_bsd_indent` 的 7 个无 argv
对象正是在一次 check-world 之后重新 `collect` 时发现的）。

`make check-world` 以固定并行度 `make -j8 -k -Orecurse -w check-world` 运行：`-Orecurse`
让每个递归 make 的输出整体成组，`# +++ ... check in <dir> +++` 锚点因此可按目录归属；
`-k` 保证失败集完整（没有 `-k` 时 make 在第一个失败后不再启动新套件，失败集取决于调度）。
ecpg 的 pg_regress 不打印锚点，解析器按 make cwd 单独成段（`sections_without_anchor` = 1）。

## 为什么 builddir 不在 virtiofs 上（迁移记录）

第一次证据周期把 builddir / prefix / 日志放在 `/Users/niejinbo/work/code/pg/postgres.builds/`
（virtiofs）。configure、全部编译与 token 回放都正常，但 `make check-world` 两次都退出 2，
失败只发生在 `tmp_check/` 的文件系统语义上：

| 失败 | 轮次 | 根因 |
|---|---|---|
| `src/bin/pg_verifybackup/t/003_corruption.pl` 子测试 44 `search_directory_fails` | 两轮（确定性） | 测试把 `backup/base` `chmod 0400` 后期待 `could not stat file or directory`。POSIX 文件系统上 `readdir()` 成功、子项 `stat()` 返回 EACCES；virtiofs 上 `opendir()` 成功但 `readdir()` 直接返回 EACCES，`pg_verifybackup.c` `verify_backup_directory()` 的 `readdir` 循环结束后不检查 `errno`，于是目录被当作空目录，只剩 "present in the manifest but not on disk"。 |
| `src/test/recovery/t/032_relfilenode_reuse.pl` | 仅第 1 轮（第 2 轮通过） | standby 的 walreceiver 已把 WAL 写到 `0/37F4AE0` 之后，startup 进程在同一文件 `000000010000000000000003` 偏移 7749632 处读到全零（`invalid magic number 0000`），重新流式拉取后在同一偏移再次读到零，循环直到 221 s 超时：同一文件由两个进程写/读时在 virtiofs 上读到过期页。 |

两者都不是 REL_16_11 源码问题，也与依赖、locale（统一 `C.UTF-8`）或 Make 并行无关。
`zhtools.evidence_run.dirperm_probe` 把 0400 目录语义写进 `environment.json["dirperm_probe"]`：
virtiofs 为 `readdir_fails`，`/tmp`（tmpfs）与 `/home/redstar`（btrfs）为 `posix`。

因此 builddir、prefix、evidence_out 全部迁移到原生 btrfs `/home/redstar/pg-builds/`，
srcdir 仍是 virtiofs 上的只读 worktree。旧目录保留为诊断归档，不属于基线；其关键哈希记录在
`frozen_config.json["migration"]`：

| 旧证据 | SHA-256 |
|---|---|
| 第 1 轮 check-world 日志（无 `-k`，exit 2，003 + 032 失败）`…/postgres.builds/REL_16_11_base-evidence/make_check_world.run1.log` | `d7bfcdc2033bf0981616e91b3f77a9ad1ce8eebede9fe43d82c89b846a75be48` |
| 第 2 轮 check-world 日志（`-k`，exit 2，仅 003 失败）`…/make_check_world.log` | `7bdc2ca87c8800411bd674e0b7f5933db67d2a2a0747b9a16eb88be19b3e3aff` |
| 旧 `make -j1 -w world-bin` 日志 | `c0b30e49f998ba9bf8c73ff934d98ab0cf693a4268b462ba5310c2c9518354d8` |
| 旧 token 缓存（1504 变体，base 与 zh 相同） | `258b7cb94146ced12d8d994e57dc4213317d06f77776b75413a581e1133917bd` |

## 本周期（btrfs）的已知环境差异

* **PG_TEST_EXTRA 未设置**导致的 skip：`src/test/{kerberos,ldap,ssl}` 三个套件 `NOTESTS`
  （6 个 `.pl` skipped），`src/interfaces/libpq/t/004_load_balance_dns.pl`、
  `src/test/authentication/t/005_sspi.pl`（Windows）、`src/test/recovery/t/031_recovery_conflict.pl`
  （上游标记 disabled due to instability）skipped；合计 9 个 skipped TAP 文件。
* check-world 的实际退出码、失败集与告警见 `check_world.json` 和
  `cc_evidence/baseline.json["check_world_*"]`（本文件不重复数字，以机器记录为准）。
