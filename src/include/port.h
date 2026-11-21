/*-------------------------------------------------------------------------
 *
 * port.h
 *	  Header for src/port/ compatibility functions.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PORT_H
#define PG_PORT_H

#include <ctype.h>

/*
 * Windows 专用移植代码足够多，故把大部分
 * 另文件。
 * 注意：某些 CYGWIN 头或 #define WIN32。
 */
#if defined(WIN32) && !defined(__CYGWIN__)
#include "port/win32_port.h"
#endif

/* socket 在 WIN32 上有不同定义 */
#ifndef WIN32
typedef int pgsocket;

#define PGINVALID_SOCKET (-1)
#else
typedef SOCKET pgsocket;

#define PGINVALID_SOCKET INVALID_SOCKET
#endif

/* 若平台无 socklen_t，假定这样可用 */
#ifndef HAVE_SOCKLEN_T
typedef unsigned int socklen_t;
#endif

/* 非阻塞 */
extern bool pg_set_noblock(pgsocket sock);
extern bool pg_set_block(pgsocket sock);

/* Unix/Win32 可移植路径处理（path.c） */

extern bool has_drive_prefix(const char *path);
extern char *first_dir_separator(const char *filename);
extern char *last_dir_separator(const char *filename);
extern char *first_path_var_separator(const char *pathlist);
extern void join_path_components(char *ret_path,
								 const char *head, const char *tail);
extern void canonicalize_path(char *path);
extern void canonicalize_path_enc(char *path, int encoding);
extern void make_native_path(char *filename);
extern void cleanup_path(char *path);
extern bool path_contains_parent_reference(const char *path);
extern bool path_is_relative_and_below_cwd(const char *path);
extern bool path_is_prefix_of_path(const char *path1, const char *path2);
extern char *make_absolute_path(const char *path);
extern const char *get_progname(const char *argv0);
extern void get_share_path(const char *my_exec_path, char *ret_path);
extern void get_etc_path(const char *my_exec_path, char *ret_path);
extern void get_include_path(const char *my_exec_path, char *ret_path);
extern void get_pkginclude_path(const char *my_exec_path, char *ret_path);
extern void get_includeserver_path(const char *my_exec_path, char *ret_path);
extern void get_lib_path(const char *my_exec_path, char *ret_path);
extern void get_pkglib_path(const char *my_exec_path, char *ret_path);
extern void get_locale_path(const char *my_exec_path, char *ret_path);
extern void get_doc_path(const char *my_exec_path, char *ret_path);
extern void get_html_path(const char *my_exec_path, char *ret_path);
extern void get_man_path(const char *my_exec_path, char *ret_path);
extern bool get_home_path(char *ret_path);
extern void get_parent_directory(char *path);

/* common/pgfnames.c */
extern char **pgfnames(const char *path);
extern void pgfnames_cleanup(char **filenames);

#define IS_NONWINDOWS_DIR_SEP(ch)	((ch) == '/')
#define is_nonwindows_absolute_path(filename) \
( \
	IS_NONWINDOWS_DIR_SEP((filename)[0]) \
)

#define IS_WINDOWS_DIR_SEP(ch)	((ch) == '/' || (ch) == '\\')
/* 见 path_is_relative_and_below_cwd() 如何处理 'E:abc'。 */
#define is_windows_absolute_path(filename) \
( \
	IS_WINDOWS_DIR_SEP((filename)[0]) || \
	(isalpha((unsigned char) ((filename)[0])) && (filename)[1] == ':' && \
	 IS_WINDOWS_DIR_SEP((filename)[2])) \
)

/*
 *	is_absolute_path and IS_DIR_SEP
 *
 *	此处用宏可避免在 libpq 中编入 path.c。
 */
#ifndef WIN32
#define IS_DIR_SEP(ch) IS_NONWINDOWS_DIR_SEP(ch)
#define is_absolute_path(filename) is_nonwindows_absolute_path(filename)
#else
#define IS_DIR_SEP(ch) IS_WINDOWS_DIR_SEP(ch)
#define is_absolute_path(filename) is_windows_absolute_path(filename)
#endif

/*
 * 本宏集中列出标识"先前已建立的网络连接
 * 硬失败"的全部 errno。
 * 拟用于 switch，形式为
 * case ALL_CONNECTION_FAILURE_ERRNOS:。
 *
 * 注意：把 EPIPE 与 ECONNRESET 归为一组，视为
 * 服务器可能崩溃，连同其他表明网络
 * 连通性丢失、但对服务器状态证明不多的错误。实际
 * 报告错误处通常单独挑出 EPIPE 与 ECONNRESET，
 * 而让网络失败走通用报告。
 */
#define ALL_CONNECTION_FAILURE_ERRNOS \
	EPIPE: \
	case ECONNRESET: \
	case ECONNABORTED: \
	case EHOSTDOWN: \
	case EHOSTUNREACH: \
	case ENETDOWN: \
	case ENETRESET: \
	case ENETUNREACH: \
	case ETIMEDOUT

/* 可移植 locale 初始化（exec.c） */
extern void set_pglocale_pgservice(const char *argv0, const char *app);

/* 可移植查找并执行二进制（exec.c） */
extern int	validate_exec(const char *path);
extern int	find_my_exec(const char *argv0, char *retpath);
extern int	find_other_exec(const char *argv0, const char *target,
							const char *versionstr, char *retpath);
extern char *pipe_read_line(char *cmd, char *line, int maxsize);

/* 本不属此，但与 find_other_exec() 同用，故… */
#define PG_BACKEND_VERSIONSTR "postgres (PostgreSQL) " PG_VERSION "\n"

#ifdef EXEC_BACKEND
/* exec 前禁用 ASLR，仅开发者构建（exec.c） */
extern int	pg_disable_aslr(void);
#endif


#if defined(WIN32) || defined(__CYGWIN__)
#define EXE ".exe"
#else
#define EXE ""
#endif

#if defined(WIN32) && !defined(__CYGWIN__)
#define DEVNULL "nul"
#else
#define DEVNULL "/dev/null"
#endif

/* 可移植延时处理 */
extern void pg_usleep(long microsec);

/* 可移植 SQL 式大小写无关比较与转换 */
extern int	pg_strcasecmp(const char *s1, const char *s2);
extern int	pg_strncasecmp(const char *s1, const char *s2, size_t n);
extern unsigned char pg_toupper(unsigned char ch);
extern unsigned char pg_tolower(unsigned char ch);
extern unsigned char pg_ascii_toupper(unsigned char ch);
extern unsigned char pg_ascii_tolower(unsigned char ch);

/*
 * 自 v12 起总以自有实现替换 snprintf() 等。
 * 核心代码不再查阅此符号，
 * 但仍定义，以防扩展在查看。
 */
#define USE_REPL_SNPRINTF 1

/*
 * libintl >= 0.13 会尝试用宏替换 printf() 等
 * 为自有、理解 %$ 格式的版本。我们亦然，
 * 故若存在则禁用其宏。
 */
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsprintf
#undef vsprintf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef vfprintf
#undef vfprintf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef vprintf
#undef vprintf
#endif
#ifdef printf
#undef printf
#endif

extern int	pg_vsnprintf(char *str, size_t count, const char *fmt, va_list args) pg_attribute_printf(3, 0);
extern int	pg_snprintf(char *str, size_t count, const char *fmt,...) pg_attribute_printf(3, 4);
extern int	pg_vsprintf(char *str, const char *fmt, va_list args) pg_attribute_printf(2, 0);
extern int	pg_sprintf(char *str, const char *fmt,...) pg_attribute_printf(2, 3);
extern int	pg_vfprintf(FILE *stream, const char *fmt, va_list args) pg_attribute_printf(2, 0);
extern int	pg_fprintf(FILE *stream, const char *fmt,...) pg_attribute_printf(2, 3);
extern int	pg_vprintf(const char *fmt, va_list args) pg_attribute_printf(1, 0);
extern int	pg_printf(const char *fmt,...) pg_attribute_printf(1, 2);

#ifndef WIN32
/*
 * 加 pg_ 前缀以警示：Windows 实现有
 * 非标准副作用——改变当前文件位置。
 */
#define pg_pread pread
#define pg_pwrite pwrite
#endif

/*
 * 对 printf 用 __VA_ARGS__，以免替换
 * format() 属性声明中的 "printf" 格式原型。
 * 可惜这使取 printf 函数指针
 * 达不到期望。（若需要，须显式写
 * pg_printf。）对 printf 兄弟函数，用
 * 无参宏，使函数指针行为不意外。
 */
#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#define vsprintf		pg_vsprintf
#define sprintf			pg_sprintf
#define vfprintf		pg_vfprintf
#define fprintf			pg_fprintf
#define vprintf			pg_vprintf
#define printf(...)		pg_printf(__VA_ARGS__)

/* snprintf.c 亦提供 */
extern int	pg_strfromd(char *str, size_t count, int precision, double value);

/* 用更稳健的自有包装替换 strerror() */
extern char *pg_strerror(int errnum);
#define strerror pg_strerror

/* strerror_r() 同理；优先 GNU API */
extern char *pg_strerror_r(int errnum, char *buf, size_t buflen);
#define strerror_r pg_strerror_r
#define PG_STRERROR_R_BUFLEN 256	/* strerror_r 推荐缓冲大小 */

/* 包装 strsignal()，必要时提供自有版 */
extern const char *pg_strsignal(int signum);

extern int	pclose_check(FILE *stream);

/* 保存时区信息的全局变量。 */
#if defined(WIN32) || defined(__CYGWIN__)
#define TIMEZONE_GLOBAL _timezone
#define TZNAME_GLOBAL _tzname
#else
#define TIMEZONE_GLOBAL timezone
#define TZNAME_GLOBAL tzname
#endif

#if defined(WIN32) || defined(__CYGWIN__)
/*
 *	Win32 在并发访问时 rename/unlink 不可靠。
 */
extern int	pgrename(const char *from, const char *to);
extern int	pgunlink(const char *path);

/* 先含此，以免后续头看到这些 define */
#ifdef _MSC_VER
#include <io.h>
#endif

#define rename(from, to)		pgrename(from, to)
#define unlink(path)			pgunlink(path)
#endif							/* defined(WIN32) || defined(__CYGWIN__) */

/*
 *	Win32 亦无符号链接，但可用较新
 *	Win32 的联接点模拟。
 *
 *	Cygwin 自有符号链接可在 Win95/98/ME 上工作，
 *	而联接点不能，故改用那些。无法
 *	预知 Cygwin 二进制会跑在何种系统。
 *	注意：CYGWIN 头或会 #define WIN32。
 */
#if defined(WIN32) && !defined(__CYGWIN__)
extern int	pgsymlink(const char *oldpath, const char *newpath);
extern int	pgreadlink(const char *path, char *buf, size_t size);

#define symlink(oldpath, newpath)	pgsymlink(oldpath, newpath)
#define readlink(path, buf, size)	pgreadlink(path, buf, size)
#endif

extern bool rmtree(const char *path, bool rmtopdir);

#if defined(WIN32) && !defined(__CYGWIN__)

/*
 * 要 lseek() 的 64 位变体。
 *
 * 对 Visual Studio，须在 <io.h> 之后，以免搅乱其
 * lseek() 与 _lseeki64() 声明。
 *
 * MinGW 已有宏，故须先取消定义（视
 * _FILE_OFFSET_BITS 而定，可能指向自有 lseek64，但我们不
 * 依赖其已设置）。
 */
#undef lseek
#define lseek(a,b,c) _lseeki64((a),(b),(c))

/*
 * 要 chsize() 的 64 位变体。它设 errno 并返回之，
 * 故把非零结果转为 -1 以合 POSIX。
 *
 * 阻止 MinGW 声明函数，并在定义自有前
 * 取消其宏。
 */
#ifndef _MSC_VER
#define FTRUNCATE_DEFINED
#include <unistd.h>
#undef ftruncate
#endif
#define ftruncate(a,b) (_chsize_s((a),(b)) == 0 ? 0 : -1)

/*
 * open()/fopen() 替换，以允许删除打开文件及
 * 传入其他特殊选项。
 */
#define		O_DIRECT	0x80000000
extern HANDLE pgwin32_open_handle(const char *, int, bool);
extern int	pgwin32_open(const char *, int,...);
extern FILE *pgwin32_fopen(const char *, const char *);
#define		open(a,b,c) pgwin32_open(a,b,c)
#define		fopen(a,b) pgwin32_fopen(a,b)

/*
 * Mingw-w64 头将 popen、pclose 映到 _popen 与 _pclose。我们要
 * 用自有 popen 包装而非纯 _popen，故覆盖之。为
 * 一致，pclose 亦用自有版。
 */
#ifdef popen
#undef popen
#endif
#ifdef pclose
#undef pclose
#endif

/*
 * system()/popen() 替换，给命令再套一对
 * 引号。
 */
extern int	pgwin32_system(const char *command);
extern FILE *pgwin32_popen(const char *command, const char *type);

#define system(a) pgwin32_system(a)
#define popen(a,b) pgwin32_popen(a,b)
#define pclose(a) _pclose(a)

#else							/* !WIN32 */

/*
 *	Win32 对套接字与管道需特殊 close，Unix 上
 *	close() 全管。
 */
#define closesocket close
#endif							/* WIN32 */

/*
 * Windows 上 setvbuf() 不支持 _IOLBF，且把其解作
 * _IOFBF。雪上加霜，若开"参数校验"，
 * setvbuf(file, NULL, _IOFBF, 0) 会直接崩溃。故在
 * 想选行缓冲处，Windows 上回退为
 * 无缓冲。务必用 PG_IOLBF 而非直接
 * _IOLBF，以实现此行为。
 */
#ifndef WIN32
#define PG_IOLBF	_IOLBF
#else
#define PG_IOLBF	_IONBF
#endif

/*
 * 库例程的默认 extern 声明或宏替代。
 * 必要时由 src/port/ 文件提供。
 */

/* fseeko/ftello 所用类型 */
#ifndef WIN32					/* WIN32 在 port/win32_port.h 处理 */
#define pgoff_t off_t
#endif

#ifndef HAVE_GETPEEREID
/* Windows 上 Perl 可能有不兼容的 uid_t/gid_t 定义。 */
#ifndef PLPERL_HAVE_UID_GID
extern int	getpeereid(int sock, uid_t *uid, gid_t *gid);
#endif
#endif

/*
 * 因 *gcc* 某版 bug（比 clang 自称的 gcc 兼容
 * 更新），Glibc 对 clang 不用内建。这会导致
 * 大量多余函数调用，故在 clang 下还原。C++
 * 中 libc++（非 libstdc++）亦有问题，一并禁用。
 */
#if defined(__clang__) && !defined(__cplusplus)
/* 须分开，以免迷惑其他编译器 */
#if __has_builtin(__builtin_isinf)
/* 须先包含，以免被覆盖 */
#include <math.h>
#undef isinf
#define isinf __builtin_isinf
#endif							/* __has_builtin(isinf) */
#endif							/* __clang__ && !__cplusplus */

#ifndef HAVE_EXPLICIT_BZERO
extern void explicit_bzero(void *buf, size_t len);
#endif

#ifdef HAVE_BUGGY_STRTOF
extern float pg_strtof(const char *nptr, char **endptr);
#define strtof(a,b) (pg_strtof((a),(b)))
#endif

#ifdef WIN32
/* src/port/win32link.c */
extern int	link(const char *src, const char *dst);
#endif

#ifndef HAVE_MKDTEMP
extern char *mkdtemp(char *path);
#endif

#ifndef HAVE_INET_ATON
#include <netinet/in.h>
#include <arpa/inet.h>
extern int	inet_aton(const char *cp, struct in_addr *addr);
#endif

#if !HAVE_DECL_STRLCAT
extern size_t strlcat(char *dst, const char *src, size_t siz);
#endif

#if !HAVE_DECL_STRLCPY
extern size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

#if !HAVE_DECL_STRNLEN
extern size_t strnlen(const char *str, size_t maxlen);
#endif

/* thread.c */
#ifndef WIN32
extern bool pg_get_user_name(uid_t user_id, char *buffer, size_t buflen);
extern bool pg_get_user_home_dir(uid_t user_id, char *buffer, size_t buflen);
#endif

extern void pg_qsort(void *base, size_t nel, size_t elsize,
					 int (*cmp) (const void *, const void *));
extern int	pg_qsort_strcmp(const void *a, const void *b);

#define qsort(a,b,c,d) pg_qsort(a,b,c,d)

typedef int (*qsort_arg_comparator) (const void *a, const void *b, void *arg);

extern void qsort_arg(void *base, size_t nel, size_t elsize,
					  qsort_arg_comparator cmp, void *arg);

extern void qsort_interruptible(void *base, size_t nel, size_t elsize,
								qsort_arg_comparator cmp, void *arg);

extern void *bsearch_arg(const void *key, const void *base0,
						 size_t nmemb, size_t size,
						 int (*compar) (const void *, const void *, void *),
						 void *arg);

/* port/chklocale.c */
extern int	pg_get_encoding_from_locale(const char *ctype, bool write_message);

#if defined(WIN32) && !defined(FRONTEND)
extern int	pg_codepage_to_encoding(UINT cp);
#endif

/* port/inet_net_ntop.c */
extern char *pg_inet_net_ntop(int af, const void *src, int bits,
							  char *dst, size_t size);

/* port/pg_strong_random.c */
extern void pg_strong_random_init(void);
extern bool pg_strong_random(void *buf, size_t len);

/*
 * Postgres 12 前，后端曾用 pg_backend_random
 * 包装 pg_strong_random。
 */
#define pg_backend_random pg_strong_random

/* port/pgcheckdir.c */
extern int	pg_check_dir(const char *dir);

/* port/pgmkdirp.c */
extern int	pg_mkdir_p(char *path, int omode);

/* port/pqsignal.c (see also interfaces/libpq/legacy-pqsignal.c) */
#ifdef FRONTEND
#define pqsignal pqsignal_fe
#endif
typedef void (*pqsigfunc) (SIGNAL_ARGS);
extern pqsigfunc pqsignal(int signo, pqsigfunc func);

/* port/quotes.c */
extern char *escape_single_quotes_ascii(const char *src);

/* common/wait_error.c */
extern char *wait_result_to_str(int exitstatus);
extern bool wait_result_is_signal(int exit_status, int signum);
extern bool wait_result_is_any_signal(int exit_status, bool include_command_not_found);
extern int	wait_result_to_exit_code(int exit_status);

/*
 * 假定一切 Unix 皆有的接口。保留各宏
 * 以便更好文档化。
 *
 * 对符号链接相关函数，常无须测这些宏，
 * 因我们在 Windows 上提供了可处理绝对
 * 目录路径的基本支持。要测完整符号链接支持
 * （含相对路径与非目录）应条件于
 * HAVE_READLINK/HAVE_SYMLINK。
 */
#ifndef WIN32
#define HAVE_GETRLIMIT 1
#define HAVE_POLL 1
#define HAVE_POLL_H 1
#define HAVE_READLINK 1
#define HAVE_SETSID 1
#define HAVE_SHM_OPEN 1
#define HAVE_SYMLINK 1
#endif

#endif							/* PG_PORT_H */
