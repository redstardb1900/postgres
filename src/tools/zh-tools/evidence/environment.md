# 冻结的 GNU Make 证据环境（REL_16_11 基线，macOS 原生）

本文件记录计划"阶段 1：冻结范围和证据环境"中 Make VPATH 基线的全部可复现参数。
机器可读的对应物是同目录的 `frozen_config.json`（输入）与 `environment.json`、
`configure.json`、`make_*.json`、`check_world.json`（输出）。任何一项变化都意味着
一次新的证据周期：必须重新 configure、重新捕获日志、重新生成 token 缓存。

> 本周期是**原生 macOS 重新冻结**。此前的证据基线建立在 OrbStack Linux 虚拟机上
> （srcdir 经 virtiofs、builddir 在 btrfs），该主机已不可用；`frozen_config.json`
> 中 `migration`、下文"迁移记录"记录了这次切换。旧 Linux 归档仅作诊断保留，不属于本基线。

## 机器与工具

| 项 | 值 |
|---|---|
| OS | macOS 26.4.1（Tahoe，`BuildVersion 25E253`），Darwin，arm64 |
| 文件系统 | srcdir、builddir、prefix、evidence_out **全部**在同一个本地 APFS 数据卷（挂载点 `/`）上；`/tmp`、`$TMPDIR` 亦为 APFS。没有 virtiofs / 网络挂载 |
| CC / CXX | `/usr/bin/gcc`、`/usr/bin/g++` = Apple clang 21.0.0（`clang-2100.0.123.102`）；是真实二进制（非 ccache/distcc/sccache 包装） |
| SDK | `/Library/Developer/CommandLineTools/SDKs/MacOSX26.4.sdk`（clang 自动 `-isysroot`；`--with-libxml`/`--with-libxslt` 经 SDK 的 `.tbd` 与 brew `pkg-config` 找到） |
| GNU Make | 3.81（Apple 随系统提供；`-w` 的目录用 `` `dir' `` 反引号-单引号引用，见下"迁移记录"） |
| bison / flex | 2.3 / 2.6.4 Apple(flex-35)（都满足 PG16 configure 的最低要求） |
| perl / python3 | 5.34.1（**无 IPC::Run**，故 TAP 套件不可用）/ 3.9.6（CommandLineTools，无嵌入用 libpython） |
| gettext | 1.0（Homebrew，keg-only：二进制在 `/opt/homebrew/opt/gettext/bin`，头文件/库在 `/opt/homebrew/opt/gettext/{include,lib}`） |
| openssl@3 / krb5 | 3.5.9 / 1.22.2（Homebrew，均 keg-only）：`/opt/homebrew/opt/openssl@3/{include,lib}`、`/opt/homebrew/opt/krb5/{include,lib}`；为 `--with-openssl` / `--with-gssapi` 提供头文件与库。Homebrew 另链接了一个 openssl@4（仅 krb5 依赖），故 PATH 把 keg-only 的 openssl@3/bin 放在最前，使 `openssl` 与编译链接的 3.5 树一致 |
| pkg-config | 3.0.8（brew `pkgconf`，`/opt/homebrew/bin/pkg-config`） |
| git | 2.50.1 (Apple Git-155) |
| locale | 进程环境 `LANG=C`，无 `LC_ALL`；`locale -a` 含 `C`、`C.UTF-8`、`POSIX`、`en_US.UTF-8`、`zh_CN.UTF-8`（Linux 上同名 locale 为 `en_US.utf8`/`zh_CN.utf8`） |

## 冻结的 configure 命令

srcdir = `/Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base`（detached，
HEAD 必须等于 `d61dd817be70749d14e982a369e97fdda9d5cba6`，且 `git status --ignored` 为空）。
builddir = `/Users/niejinbo/pg-builds/REL_16_11_base-make`（源码树外，全新创建）。

```sh
env -i PATH=/opt/homebrew/opt/openssl@3/bin:/opt/homebrew/opt/krb5/bin:/opt/homebrew/opt/gettext/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
    LANG=C HOME=/Users/niejinbo SHELL=/bin/sh USER=niejinbo LOGNAME=niejinbo \
    LDFLAGS=-L/opt/homebrew/opt/gettext/lib -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/opt/krb5/lib \
    CPPFLAGS=-I/opt/homebrew/opt/gettext/include -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/opt/krb5/include \
  /Users/niejinbo/work/code/pg/postgres.worktrees/REL_16_11_base/configure \
    --prefix=/Users/niejinbo/pg-builds/REL_16_11_base-install \
    --enable-depend --enable-nls --enable-cassert --enable-debug \
    --with-perl --without-python --with-openssl --with-gssapi \
    --with-libxml --with-libxslt --without-icu --disable-tap-tests \
    CC=/usr/bin/gcc CXX=/usr/bin/g++
```

环境以 `env -i` 构造，只含上表变量；`LC_ALL`、`LANGUAGE`、`TZ`、`TMPDIR`、`PGPORT`、
`PGHOST`、`PGDATA`、`PG_TEST_EXTRA`、`PROVE_FLAGS`、`MAKEFLAGS`、`PKG_CONFIG_PATH`
一律不设置。`LDFLAGS`/`CPPFLAGS` 指向三个 keg-only 的 brew 包：gettext（`libintl` 头与库，
`--enable-nls`）、openssl@3（OpenSSL 3.5 头与库，`--with-openssl`）、krb5
（`gssapi/gssapi.h` 与 `libgssapi_krb5`，`--with-gssapi`）；与翻译树无关。

### 启用项及探测依据

| 选项 | 依据 |
|---|---|
| `--enable-depend` | 计划要求；产生 `.deps/*.Po` |
| `--enable-nls` | 计划要求；brew gettext 1.0，`msgfmt`/`msgmerge`/`xgettext` 均在冻结 PATH 上。13 个受 `#ifdef ENABLE_NLS` 保护的 C/H 文件因此进入活跃预处理区域 |
| `--enable-cassert --enable-debug` | 让 `USE_ASSERT_CHECKING` 分支进入活跃预处理区域，扩大 token 覆盖；`-g` 不影响预处理 |
| `--with-perl` | 系统 perl 5.34.1（含 ExtUtils::Embed / libperl；TAP 不可用与此无关） |
| `--with-libxml` / `--with-libxslt` | SDK 提供 libxml2/libxslt 头与 `.tbd`，brew `pkg-config` 探测通过 |
| `--with-openssl` / `--with-gssapi` | brew openssl@3 3.5.9 与 krb5 1.22.2（keg-only，头/库路径固定写进 `LDFLAGS`/`CPPFLAGS`）。这两个选项让 `be-secure-openssl.c`、`be-secure-gssapi.c`、`be-gssapi-common.c` 三个**已翻译**文件重新获得编译变体；关闭它们时这三个文件没有变体，`--cc-check` 只能把它们列为未验证 |

### 禁用项及原因

| 选项 | 原因 |
|---|---|
| `--enable-tap-tests` | 系统 perl 5.34.1 **无 IPC::Run**（`Can't locate IPC/Run.pm`），prove 无法运行 TAP 套件。本基线 check-world 只跑 pg_regress 套件 |
| `--with-python` | CommandLineTools 的 Python 3.9 框架没有可链接的 libpython 供嵌入 |
| `--with-icu` | SDK 只有 `libicucore`，没有 `unicode/ucol.h`，也没有 icu 的 pkg-config 文件 |
| `--with-ldap` / `--with-pam` | 不属于冻结最小构建 |
| `--with-lz4` / `--with-zstd` | SDK 无头/库，也未装对应 brew 包 |
| `--with-uuid=e2fs` | 保持 configure 默认；macOS 的 `uuid_generate` 由 libSystem 提供 |
| `--with-tcl` | 无 Tcl 头 |
| `--enable-dtrace` | 无可用的 DTrace/sdt 探针支持 |
| `--with-selinux` / `--with-systemd` | 非 macOS API |
| `--with-llvm` / `--with-bonjour` / `--with-bsd-auth` | 不属于冻结最小构建 |
| `PG_TEST_EXTRA` | 不设置：无 `slapd`、`krb5kdc` 等 |
| `--with-system-tzdata` | 保持内置 `src/timezone` 构建进入基线 |

configure 结果摘要见 `configure.json`（`config_status_summary`、`pg_config_h`）。
禁用项会让更多 `#ifdef` 分支不活跃，相关 C/H 文件经 `collect` 后会出现在
`cc_evidence/platform_exceptions_candidates.json` 中，需要一个一个人工复核后才可批准为平台例外。
本周期唯一"为覆盖而装包"的例外是 openssl@3 / krb5（见启用项）：macOS SDK 已不带 OpenSSL 头、
也没有 `libgssapi`，不装这两个 brew 包，三个已翻译文件（`be-secure-openssl.c`、
`be-secure-gssapi.c`、`be-gssapi-common.c`）就永远没有编译变体、`--cc-check` 永远无法验证它们；
其余禁用项不涉及已翻译文件，故不为它们装包。

## 捕获命令（全部 `-j1 -w`，不交错）

| 记录 | 命令（在 builddir 中） | 用途 |
|---|---|---|
| `make_world_bin.json` | `make -j1 -w world-bin` | 主体编译命令 |
| `make_checkprep.json` | `make -j1 -w checkprep` | `EXTRA_INSTALL` 测试模块（src/test/modules、contrib 测试扩展） |
| `make_check_prereqs.json` | `make -j1 -w -C src/test all` | check-world 期间会编译的测试程序（regress/isolation/modules），须提前以 `-j1 -w` 捕获 argv |
| `make_check_prereqs_ecpg.json` | `make -j1 -w -C src/interfaces/ecpg/test all` | ecpg 测试程序（`.pgc` → builddir `.c` 的生成 TU） |
| `make_check_prereqs_libpq.json` | `make -j1 -w -C src/interfaces/libpq test-build` | libpq 测试程序 |
| `make_check_prereqs_pg_bsd_indent.json` | `make -j1 -w -C src/tools/pg_bsd_indent all` | 只由其自身 TAP check 构建的 7 个对象 |
| `make_install.json` | `make -j1 -w install` | 安装到冻结 prefix；macOS 上 check-world 的前置条件，见下（不编译任何对象） |

后四项是计划中 `world-bin` + `checkprep` 之外的补充：没有它们，这些对象会在
`make check-world` 过程中才被编译，其 argv 不在任何一份 `-j1 -w` 捕获日志里，
`.o/.Po/argv` 三方闭合无法成立（`pg_bsd_indent` 的 7 个无 argv 对象正是在一次
check-world 之后重新 `collect` 时发现的；旧 Linux 基线当时为 `-j8`，交错编译同样不可闭合）。

`install` 是 macOS 特有的必要补充：`make check` 会把二进制以 `DESTDIR=tmp_install` 装进临时树，
而这些二进制内嵌的是**最终 prefix 的绝对 install_name**（`<prefix>/lib/libpq.5.dylib`）。
Linux 上 `LD_LIBRARY_PATH` 能让它们加载临时树里的库；macOS 的 `/bin/sh` 会**剥掉传给子进程的
`DYLD_*` 变量**（bash 启动时即删除，连 `env` 输出里都看不到），于是 `DYLD_LIBRARY_PATH`
失效，`initdb` 以 `dyld: Library not loaded: <prefix>/lib/libpq.5.dylib` 触发 SIGABRT
（`Abort trap: 6`），check-world 里每个套件都在 0.4 秒内失败。先把 prefix 真正装上，
那个绝对路径就存在了。`install` 不编译任何对象，不影响 `.o/.Po/argv` 闭合。

`make check-world` 以 `make -j1 -k -w check-world` 运行，**必须 `-j1`**：macOS 系统自带的
GNU Make 3.81 没有 `--output-sync`/`-O`（3.82 才引入），而 `checkworld.py` 依赖"每个递归
make 的输出整体成组"——若在 `-j8` 下去掉 `-Orecurse`，并发子 make 的输出会逐行交错，
`# +++ ... check in <dir> +++` 锚点归属与 `Entering/Leaving directory` 的 cwd 栈都会串线；
串行输出天然成组，解析质量与 `-Orecurse` 相同。`-k` 保证失败集完整（没有 `-k` 时 make
在第一个失败后不再启动新套件，失败集取决于调度）。ecpg 的 pg_regress 不打印锚点，
解析器按 make cwd 单独成段（`sections_without_anchor` = 1）。

## 迁移记录：从 OrbStack Linux 到原生 macOS

第一次证据周期把 srcdir 放在 virtiofs 的 `/Users`、builddir/prefix/日志放在虚拟机原生
btrfs 的 `/home/redstar/pg-builds`。该 Linux 主机已不可用，本周期在 macOS 上原生重建。
因此旧周期针对 virtiofs 的两点 POSIX 差异**在本基线中不存在**（四个根目录同处一个本地
APFS 卷）：

| 旧 virtiofs 失败 | 现象 | 根因 |
|---|---|---|
| `pg_verifybackup/t/003_corruption.pl` 子测试 44 `search_directory_fails` | 确定性 | 测试把 `backup/base` `chmod 0400` 后期待 `could not stat file or directory`；POSIX 文件系统上 `readdir()` 成功、子项 `stat()` 返回 EACCES，virtiofs 上 `readdir()` 直接返回 EACCES，目录被当作空目录 |
| `src/test/recovery/t/032_relfilenode_reuse.pl` | 偶发 | 同一 WAL 文件由两进程写/读时在 virtiofs 上读到过期零页，超时 |

原生 macOS 下 `zhtools.evidence_run.dirperm_probe` 应报告 `posix`（`environment.json`）。
旧 Linux 归档保存在 `/Users/niejinbo/work/code/pg/postgres.builds/REL_16_11_base-evidence`，
仅作诊断，不属于本基线。

### 工具缺陷修复：GNU Make 3.81 的目录引用

`make -w` 的 "Entering/Leaving directory" 行在 GNU Make 4.x（旧 Linux 基线）是
`'dir'`，在 GNU Make 3.81（macOS 系统 make）是 `` `dir' ``（反引号-单引号）。
`zhtools.makelog` 原来的 `ENTER_RX`/`LEAVE_RX` 只认 4.x 形式，导致 macOS 下**所有**日志
都被判为"没有 Entering directory 行"、每条编译命令丢失 `cwd`、同名对象互相冲突
（`collect` 报 `closed=False`、`unverified=78`）。修复后两种引用都接受，
`collect` 恢复 `closed=True`、`unverified=0`。回归测试：
`tests/test_makelog.py::ParseLogTest::test_make381_backtick_directory_quotes`。

## 本周期（macOS）的已知环境差异

* **TAP 关闭**：`--disable-tap-tests`（无 IPC::Run）。`make check-world` 只跑 pg_regress
  套件；`check_world.json` 的 `tap_files` 为 0，这是配置使然，不是测试失败。
* **check-world 之前必须先 `make install` 到 prefix**：macOS `/bin/sh` 剥离 `DYLD_*`，
  `make check` 的 `DESTDIR=tmp_install` 暂存树无法靠 `DYLD_LIBRARY_PATH` 找到那些按绝对
  install_name 引用的库；`make_install.json` 是本周期相对旧 Linux 基线新增的捕获目标，
  不编译任何对象。
* **locale 固定为 `C`**：`frozen_config.json["env"]` 取 `LANG=C`（无 `LC_ALL`）。
  macOS 的 `locale -a` 同时提供 `C.UTF-8`、`en_US.UTF-8`、`zh_CN.UTF-8`，旧基线取
  `C.UTF-8`。本周期选 `C` 只是为了让 configure 与 pg_regress 在最朴素的 locale 下运行；
  若改为 UTF-8 locale，属于一次新的证据周期。`environment.json["locale_a_has"]`
  同时记录两种拼写（Linux `*.utf8` 与 macOS `*.UTF-8`）的探测结果。
* **`--enable-nls` / `--with-openssl` / `--with-gssapi` 依赖 keg-only brew 包**：gettext、
  openssl@3、krb5 的头/库路径因此固定写进 `frozen_config.json["env"]` 的 `LDFLAGS`/`CPPFLAGS`，
  `/opt/homebrew/opt/openssl@3/bin` 亦置于 `PATH` 之首；换机器时这些路径必须一起复核。
* **相对旧 Linux 基线仍关闭的选项**：`--with-ldap`、`--with-pam`、`--with-lz4`、
  `--with-python`、`--enable-dtrace`、`--with-selinux`、ICU 与 TAP 套件仍禁用；其中
  `--with-openssl`、`--with-gssapi` 已在本周期重新启用（理由见"启用项"），其余不涉及
  任何已翻译文件。
* check-world 的实际退出码、失败集与告警见 `check_world.json` 和
  `cc_evidence/baseline.json["check_world_*"]`（本文件不重复数字，以机器记录为准）。
