/*------------------------------------------------------------------------
 * PostgreSQL 手工配置设置
 *
 * 本文件含各类配置符号与限制。在
 * 所有情况下，改动它们仅在极罕见情形或
 * 对开发者有用。若编辑任一项，务必做*完整*
 * 重建（及 initdb）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/pg_config_manual.h
 *------------------------------------------------------------------------
 */

/*
 * 这是运行 initdb 且未带 --wal-segsize 时所用的
 * wal_segment_size 默认值。必须是合法段大小。
 */
#define DEFAULT_XLOG_SEG_SIZE	(16*1024*1024)

/*
 * 标识符最大长度（如表名、列名、
 * 函数名）。实际名称比此少一字节，
 * 因长度须含结尾零字节。
 *
 * 更改此项需要 initdb。
 */
#define NAMEDATALEN 64

/*
 * 函数参数的最大个数。
 *
 * 最小值为 8（GIN 索引用 8 参数支持函数）。
 * 最大可能约 600（受 pg_proc 索引中索引元组大小限制；
 * BLCKSZ 大于 8K 可允许更多）。大于所需的值会
 * 浪费内存与处理时间，但并不直接
 * 耗磁盘。
 *
 * 更改此项不需 initdb，但需完整
 * 后端重编译（含任何用户定义 C 函数）。
 */
#define FUNC_MAX_ARGS		100

/*
 * 当基于 PostgreSQL 派生产品并做会导致
 * 可加载模块不兼容的改动时，建议更改此
 * 串，以便 dfmgr.c 能以清晰错误消息拒绝加载
 * 不兼容模块。典型不兼容例子是任何
 * 节点标签或节点结构的改动。（注意 dfmgr.c 已能
 * 检测主版本差异及某些编译期常量变化等
 * 常见不兼容来源。此设置用于捕获
 * 无法用直截了当方式检测的情况。）
 *
 * 该串无规定格式。建议包含
 * 产品或公司名，以及可选的内部相关 ABI
 * 版本。例："ACME Postgres/1.2"。注意若检出
 * ABI 不匹配时会出现在用户错误消息中。
 */
#define FMGR_ABI_EXTRA		"PostgreSQL"

/*
 * 索引中列的最大个数。做成非 32 倍数几乎无意义，
 * 因主要代价与索引元组头大小相关
 * （见 access/itup.h）。
 *
 * 更改此项需要 initdb。
 */
#define INDEX_MAX_KEYS		32

/*
 * 分区键中列的最大个数
 */
#define PARTITION_MAX_KEYS	32

/*
 * 决定内建 8 字节类型（含 float8、int8 与
 * timestamp）是否按值传递。若 sizeof(Datum) >=
 * 8（64 位平台）则默认开。若 sizeof(Datum)<8（32 位），
 * 必须关闭。保留为选项是为便于在
 * 64 位平台上测试按引用传递的代码路径。
 *
 * 更改此项需要 initdb。
 */
#if SIZEOF_VOID_P >= 8
#define USE_FLOAT8_BYVAL 1
#endif

/*
 * 无原生自旋锁时，用信号量模拟。
 * 减小此值降低 OS 资源消耗；增大可能
 * 改善性能，但提供真正自旋锁实现
 * 恐怕好得多。
 */
#define NUM_SPINLOCK_SEMAPHORES		128

/*
 * 既无自旋锁也无原子操作支持时，我们在
 * 信号量上的自旋锁之上实现原子操作。为
 * 在持有自旋锁时对原子操作安全，须使用
 * 独立的信号量。
 */
#define NUM_ATOMICS_SEMAPHORES		64

/*
 * MAXPGPATH：PostgreSQL 中路径名缓冲的标准大小（故
 * 可用路径名最大长度少一）。
 *
 * 若非有太多符号可选，我们本会用标准系统头
 * 符号：MAXPATHLEN、MAX_PATH、PATH_MAX 均由
 * 不同"标准"定义，且在同一平台上常有
 * 不同值！故我们干脆采用一个合理
 * 宽裕的设置。
 */
#define MAXPGPATH		1024

/*
 * 若机器字节大小不同可尝试更改，
 * 但不保证……
 */
#define BITS_PER_BYTE		8

/*
 * 磁盘 I/O 缓冲的首选对齐。在某些 CPUs 上，
 * 内核空间之间的拷贝若用户缓冲对齐在
 * 大于 MAXALIGN 的边界上会显著更快。理想上应为
 * 平台相关值，但目前先硬编码。
 */
#define ALIGNOF_BUFFER	32

/*
 * 若定义 EXEC_BACKEND，postmaster 用另一种方法
 * 启动子进程：不简单用 Unix 平台标准的 fork()，
 * 而在 Windows 上用 fork()+exec() 或等价物，
 * 并有大量额外代码把所需全局状态带到那些
 * 新进程。Windows 上必须启用（因无
 * fork()）。其他平台上仅用于验证那些
 * 本属 Windows 特有路径。
 */
#if defined(WIN32) && !defined(__CYGWIN__)
#define EXEC_BACKEND
#endif

/*
 * USE_POSIX_FADVISE 控制 Postgres 是否尝试使用
 * posix_fadvise() 内核调用。通常自动配置测试
 * 已足够，但某些较旧 Linux 发行版的
 * posix_fadvise()。必要时可在此去掉 #define。
 */
#if HAVE_DECL_POSIX_FADVISE && defined(HAVE_POSIX_FADVISE)
#define USE_POSIX_FADVISE
#endif

/*
 * 仅当我们有办法实现预取时才应编译
 * USE_PREFETCH 代码。（与 USE_POSIX_FADVISE 解耦，因将来
 * 可能支持其他底层预取 APIs。
 * 若更改此项，大概还需调整
 * check_effective_io_concurrency.)
 */
#ifdef USE_POSIX_FADVISE
#define USE_PREFETCH
#endif

/*
 * backend_flush_after、bgwriter_flush_after 与
 * checkpoint_flush_after 的默认及最大值；以块计。当前若
 * 存在 sync_file_range()（即仅 Linux）则默认启用。或许
 * 有 mmap 与 msync(MS_ASYNC) 时也可默认启用？
 */
#ifdef HAVE_SYNC_FILE_RANGE
#define DEFAULT_BACKEND_FLUSH_AFTER 0	/*  从不默认启用  */
#define DEFAULT_BGWRITER_FLUSH_AFTER 64
#define DEFAULT_CHECKPOINT_FLUSH_AFTER 32
#else
#define DEFAULT_BACKEND_FLUSH_AFTER 0
#define DEFAULT_BGWRITER_FLUSH_AFTER 0
#define DEFAULT_CHECKPOINT_FLUSH_AFTER 0
#endif
/*  上述三变量的上限  */
#define WRITEBACK_MAX_PENDING_FLUSHES 256

/*
 * USE_SSL 代码仅在与 SSL 实现同编时才应编译
 * 实现。
 */
#ifdef USE_OPENSSL
#define USE_SSL
#endif

/*
 * 这是放置 AF_UNIX 套接字文件的默认目录。
 * 注意：更改有破坏现有客户端应用之险，
 * 它们很可能仍在旧目录查找。
 * 但若你就是讨厌 /tmp 里的套接字，
 * 在此拧即可。亦可在运行时用
 * postmaster 的 -k 覆盖。
 *
 * 若设为空串，则默认不用 AF_UNIX 套接字：服务器
 * 除非改运行时配置否则不创建 AF_UNIX 套接字，
 * 客户端默认经 TCP/IP 连接，仅在显式指定时
 * 才用 AF_UNIX 套接字。
 *
 * Windows 上默认如此，因无良好标准位置放
 * AF_UNIX 套接字，且许多 Windows 安装
 * 尚不支持。
 */
#ifndef WIN32
#define DEFAULT_PGSOCKET_DIR  "/tmp"
#else
#define DEFAULT_PGSOCKET_DIR ""
#endif

/*
 * 这是 Windows 事件日志的默认事件源。
 */
#define DEFAULT_EVENT_SOURCE  "PostgreSQL"

/*
 * 假定的缓存行大小。不影响正确性，但可用于
 * 底层优化。当前用于在 xlog.c 中填充某些
 * 数据结构，确保高争用字段落在
 * 不同缓存行。过小会因伪共享损害性能，
 * 过大的唯一缺点是浪费几字节
 * 内存。默认 128，对所有受支持平台应够大。
 * 受支持平台。
 */
#define PG_CACHE_LINE_SIZE		128

/*
 * 假定的直接 I/O 对齐要求。4K 对应常见
 * 扇区与内存页大小。
 */
#define PG_IO_ALIGN_SIZE		4096

/*
 *------------------------------------------------------------------------
 * 下列符号用于启用调试代码，而非
 * 控制用户可见特性或资源限制。
 *------------------------------------------------------------------------
 */

/*
 * 含 Valgrind "客户端请求"（多在分配器），使
 * Valgrind 理解 PostgreSQL 内存上下文。可检出
 * 在普通构建上 Valgrind 发现不了的内存错误。亦
 * 亦可检出未持缓冲钉住时发生的缓冲访问
 * （或对在通用 bufmgr.c 请求上叠加自有客户端请求的
 * 索引访问方法，未持缓冲锁时）。
 * bufmgr.c 请求）。
 *
 * 回归套件在 Valgrind 下明显更慢。客户端
 * 请求落在热路径，故即使未在 Valgrind 下运行，
 * USE_VALGRIND 也会使执行慢几个百分点。
 *
 * 勿在未用 USE_VALGRIND 构建服务器的情况下用
 * Valgrind 测服务器；否则会从 sinval 消息得到
 * 误报（见 AddCatcacheInvalidationMessage 注释）。亦
 * 须使用抑制文件 src/tools/valgrind.supp
 * 排除其他已知误报。
 *
 * 通常应把 MEMORY_CONTEXT_CHECKING 与 USE_VALGRIND 同用；
 * 无前者时 repalloc() 的插桩较差。
 */
/* #define USE_VALGRIND */

/*
 * 定义此项使 pfree() 的内存立即清零，以便
 * 抓住引用已释放值的缺陷。
 * 目前若 --enable-cassert 则自动定义。
 */
#ifdef USE_ASSERT_CHECKING
#define CLOBBER_FREED_MEMORY
#endif

/*
 * 定义此项以检查内存分配错误（写越
 * 已分配字节）。目前若
 * cassert 或 USE_VALGRIND 则自动定义。
 */
#if defined(USE_ASSERT_CHECKING) || defined(USE_VALGRIND)
#define MEMORY_CONTEXT_CHECKING
#endif

/*
 * 定义此项使 palloc() 的内存填随机数据，以便
 * 抓住依赖未初始化内存内容的代码。
 * 注意：代价极其高昂。
 */
/* #define RANDOMIZE_ALLOCATED_MEMORY */

/*
 * 为缓存失效调试，定义 DISCARD_CACHES_ENABLED 以启用
 * debug_discard_caches GUC，在可投递失效时积极冲刷
 * syscache/relcache 项。详见
 * AcceptInvalidationMessages()（见 inval.c）
 *
 *
 * USE_ASSERT_CHECKING 构建默认启用此项。也可在无
 * cassert 构建及所隐含的 CLOBBER_FREED_MEMORY 与
 * MEMORY_CONTEXT_CHECKING 选项时使用 DISCARD_CACHES_ENABLED，
 * 但识别问题的效果恐怕较差。
 */
/* #define DISCARD_CACHES_ENABLED */

#if defined(USE_ASSERT_CHECKING) && !defined(DISCARD_CACHES_ENABLED)
#define DISCARD_CACHES_ENABLED
#endif

/*
 * 较旧仅编译期 clobber-cache 宏的
 * 宏。
 */
#if !defined(DISCARD_CACHES_ENABLED) && (defined(CLOBBER_CACHE_ALWAYS) || defined(CLOBBER_CACHE_RECURSIVELY))
#define DISCARD_CACHES_ENABLED
#endif

/*
 * 失效时回收 relcache 项所用内存。见
 * RelationBuildDesc()（relcache.c）。
 *
 * 对 clobber-cache 构建在 clobber 激活时自动生效，
 * 但可通过在此显式定义
 * RECOVER_RELATION_BUILD_MEMORY。定义为1则即使
 * 释放关系缓存内存；为 0 则即使 clobber 开也
 * 从不释放。
 */
 /* #define RECOVER_RELATION_BUILD_MEMORY 0 */	/* Force disable */
 /* #define RECOVER_RELATION_BUILD_MEMORY 1 */	/* Force enable */

/*
 * 定义此项强制所有解析与计划树经
 * copyObject()，以便抓住 copyObject() 中的错误与
 * 遗漏。
 */
/* #define COPY_PARSE_PLAN_TREES */

/*
 * 定义此项强制所有解析与计划树经
 * outfuncs.c/readfuncs.c，以便抓住那些模块中的
 * 遗漏。
 */
/* #define WRITE_READ_PARSE_PLAN_TREES */

/*
 * 定义此项强制对 DML 语句的所有原始解析树做
 * raw_expression_tree_walker() 扫描，以便抓住该函数中的
 * 错误与遗漏。
 */
/* #define RAW_EXPRESSION_COVERAGE_TEST */

/*
 * 启用锁相关操作的调试打印。
 */
/* #define LOCK_DEBUG */

/*
 * 启用 WAL 相关操作的调试打印；另见
 * wal_debug GUC 变量。
 */
/* #define WAL_DEBUG */

/*
 * 启用排序操作期间资源消耗的跟踪；
 * 另见 trace_sort GUC。8.1 默认启用。
 */
#define TRACE_SORT 1

/*
 * 启用 syncscan 操作跟踪（另见 trace_syncscan GUC）。
 */
/* #define TRACE_SYNCSCAN */
