/*-------------------------------------------------------------------------
 *
 * main.c
 *	  postgres 可执行文件的 main() 桩例程。
 *
 * 它为 postgres 的任何化身完成一些必要的启动工作
 * （postmaster、独立后端、独立 bootstrap 进程，或者
 * 由 postmaster 另行 exec 出来的子进程），然后分派到
 * 该化身对应的 FooMain() 例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "common/username.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/postmaster.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"


const char *progname;
static bool reached_main = false;


static void startup_hacks(const char *progname);
static void init_locale(const char *categoryname, int category, const char *locale);
static void help(const char *progname);
static void check_root(const char *progname);


/*
 * 任何 Postgres 服务器进程都从这里开始执行。
 */
int
main(int argc, char *argv[])
{
	bool		do_check_root = true;

	reached_main = true;

	/*
	 * 若当前平台支持，就设置一个处理程序，在
	 * 后端或 postmaster 因致命信号或异常崩溃时调用。
	 */
#if defined(WIN32)
	pgwin32_install_crashdump_handler();
#endif

	progname = get_progname(argv[0]);

	/*
	 * 平台特定的启动处理
	 */
	startup_hacks(progname);

	/*
	 * 记住最初给出的 argv[] 数组的物理位置，供
	 * ps 显示可能使用。在某些平台上，为了给 ps 设置进程标题，必须
	 * 覆盖 argv[] 的存储。在这种情况下
	 * save_ps_display_args 会制作并返回 argv[] 数组的一份新副本。
	 *
	 * save_ps_display_args 也可能挪动环境字符串以腾出
	 * 额外空间。因此这应当在启动期间尽早完成，
	 * 以免和可能保存 getenv()
	 * 结果指针的代码缠在一起。
	 */
	argv = save_ps_display_args(argc, argv);

	/*
	 * 启动必要的子系统：错误处理和内存管理
	 *
	 * 此后的代码可以使用 elog/ereport，不过
	 * 消息本地化也许不会立刻生效，而且在 GUC 设置加载之前，
	 * 消息只会送到 stderr。
	 */
	MyProcPid = getpid();
	MemoryContextInit();

	/*
	 * 设置区域设置
	 */
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("postgres"));

	/*
	 * 在 postmaster 中吸收环境里的 LC_COLLATE 和
	 * LC_CTYPE 值。各个后端稍后会把它们改成取自
	 * pg_database 的设置，但 postmaster 无法那样做。若把
	 * 它们留在 "C"，postmaster 里的消息本地化
	 * 可能效果不好。
	 */
	init_locale("LC_COLLATE", LC_COLLATE, "");
	init_locale("LC_CTYPE", LC_CTYPE, "");

	/*
	 * LC_MESSAGES 会在稍后的 GUC 选项处理中设置，但我们在这里
	 * 先设置它，以便启动错误消息可以本地化。
	 */
#ifdef LC_MESSAGES
	init_locale("LC_MESSAGES", LC_MESSAGES, "");
#endif

	/*
	 * 我们始终把这些保持为 "C"，只在 pg_locale.c 里短暂例外；解释
	 * 见那个文件。
	 */
	init_locale("LC_MONETARY", LC_MONETARY, "C");
	init_locale("LC_NUMERIC", LC_NUMERIC, "C");
	init_locale("LC_TIME", LC_TIME, "C");

	/*
	 * 既然已经从区域设置环境中吸收了我们想要的部分，就去掉任何
	 * LC_ALL 设置，这样 pg_perm_setlocale 安装的环境
	 * 变量才会生效。
	 */
	unsetenv("LC_ALL");

	/*
	 * 在做其他事情之前先接住标准选项，尤其是在我们
	 * 坚持不以 root 运行之前。
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fputs(PG_BACKEND_VERSIONSTR, stdout);
			exit(0);
		}

		/*
		 * 除此之外，我们允许 root 调用 "--describe-config" 和 "-C var"。
		 * 这相当安全，因为它们是
		 * 只读操作。-C 这种情况很重要，因为 pg_ctl 可能
		 * 在 Windows 上仍持有管理员权限时尝试调用它。
		 * 注意，虽然 -C 通常可以出现在 argv 的任何位置，
		 * 若想绕过 root 检查，就必须把它放在最前面。这
		 * 降低了我们把其他模式的 -C
		 * 开关误当成 postmaster/postgres 那个开关的风险。
		 */
		if (strcmp(argv[1], "--describe-config") == 0)
			do_check_root = false;
		else if (argc > 2 && strcmp(argv[1], "-C") == 0)
			do_check_root = false;
	}

	/*
	 * 确保我们不是以 root 运行，除非对所选
	 * 选项来说这样是安全的。
	 */
	if (do_check_root)
		check_root(progname);

	/*
	 * 根据第一个参数分派到各个子程序之一。
	 */

	if (argc > 1 && strcmp(argv[1], "--check") == 0)
		BootstrapModeMain(argc, argv, true);
	else if (argc > 1 && strcmp(argv[1], "--boot") == 0)
		BootstrapModeMain(argc, argv, false);
#ifdef EXEC_BACKEND
	else if (argc > 1 && strncmp(argv[1], "--fork", 6) == 0)
		SubPostmasterMain(argc, argv);
#endif
	else if (argc > 1 && strcmp(argv[1], "--describe-config") == 0)
		GucInfoMain();
	else if (argc > 1 && strcmp(argv[1], "--single") == 0)
		PostgresSingleUserMain(argc, argv,
							   strdup(get_user_name_or_exit(progname)));
	else
		PostmasterMain(argc, argv);
	/* 上面的函数不应返回 */
	abort();
}



/*
 * 把平台特定的启动处理放在这里。这是放置
 * 任何新服务器进程启动早期必须执行的代码的正确位置。
 * 注意，当后端或子 bootstrap 进程被 fork 出来时，这段代码不会
 * 执行，除非我们处于 fork/exec
 * 环境（即定义了 EXEC_BACKEND）。
 *
 * XXX 这里需要代码，证明该平台
 * 过于简陋，不帮忙就无法提供标准 C 执行环境。
 * 若做得到，避免再往这里加东西。
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Windows 特定的执行环境处理。
	 */
#ifdef WIN32
	{
		WSADATA		wsaData;
		int			err;

		/* 默认让输出流不缓冲 */
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		/* 准备 Winsock */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			write_stderr("%s: WSAStartup failed: %d\n",
						 progname, err);
			exit(1);
		}

		/*
		 * 默认情况下，abort() 只在非调试
		 * 构建中生成崩溃转储。由于我们的 Assert() / ExceptionalCondition() 使用 abort()，
		 * 保留默认设置会让调试更难。
		 *
		 * MINGW 自己的 C 运行时没有 _set_abort_behavior()。当
		 * 用 mingw 以微软的 UCRT 为目标时，它从不链接到库的调试
		 * 版本，因此也不需要调用
		 * _set_abort_behavior()。
		 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
		_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
							_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

		/*
		 * SEM_FAILCRITICALERRORS 会使更多错误报告给
		 * 调用方。
		 *
		 * 我们以前还指定 SEM_NOGPFAULTERRORBOX，但那会阻止
		 * Windows 崩溃报告工作。其中包括已注册的
		 * 即时调试器，使在 Windows 上调试问题变得不必要地困难。
		 * 现在我们尝试在下面分别禁用弹出框的来源
		 * （注意 SEM_NOGPFAULTERRORBOX 实际上并没有
		 * 阻止所有这些弹出框的来源）。
		 */
		SetErrorMode(SEM_FAILCRITICALERRORS);

		/*
		 * 把错误显示到 stderr，而不是弹出框（注意这不影响
		 * 源自 C 运行时的错误，见下文）。
		 */
		_set_error_mode(_OUT_TO_STDERR);

		/*
		 * 在 DEBUG 构建中，错误（包括断言）和 C 运行时错误通过
		 * _CrtDbgReport 报告。默认情况下这类错误会以
		 * 弹出框显示（即使有 NOGPFAULTERRORBOX），妨碍继续
		 * 进行。改为把这类错误报告到 stderr（以及调试器）。
		 * 这是 C 运行时特有的，因此上面的办法并不足以
		 * 抑制这些弹出框。
		 */
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	}
#endif							/* WIN32 */

	/*
	 * 初始化 dummy_spinlock，以防我们所在的平台必须
	 * 使用 pg_memory_barrier() 的回退实现。
	 */
	SpinLockInit(&dummy_spinlock);
}


/*
 * 为某个区域设置类别做最初的永久设置。若失败，
 * 也许是因为环境里 LC_foo=invalid，就使用 C 区域设置。若连
 * 这也失败，也许是因为内存不足，整个启动都会因此失败。
 * 当本函数返回时，保证已经为给定
 * 类别的环境变量做了设置。
 */
static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}



/*
 * 帮助显示应当与 PostmasterMain() 和
 * PostgresMain() 所接受的选项一致。
 *
 * XXX 在 Windows 上，这些消息的非 ASCII 本地化只有在
 * 控制台输出代码页覆盖所需字符时才能正确显示。
 * write_console() 发出的消息没有这个问题。
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections (deprecated)\n"));
	printf(_("  -k DIRECTORY       Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l                 enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -s                 show statistics after each query\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -t pa|pl|ex        show timings after each query\n"));
	printf(_("  -T                 send SIGABRT to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  --check            selects check mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
check_root(const char *progname)
{
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromise.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}

	/*
	 * 还要确保实际 uid 和有效 uid 相同。从 root shell 把程序
	 * 作为 setuid 程序执行是一个安全漏洞，因为在许多
	 * 平台上，若实际 uid 是 root，恶意子程序可以 setuid 回到 root。
	 * （既然实际上没人把 postgres 当作 setuid 程序使用，
	 * 试图主动修复这种情况似乎比它的价值更麻烦；
	 * 我们只花力气检查它。）
	 */
	if (getuid() != geteuid())
	{
		write_stderr("%s: real and effective user IDs must match\n",
					 progname);
		exit(1);
	}
#else							/* WIN32 */
	if (pgwin32_is_admin())
	{
		write_stderr("Execution of PostgreSQL by a user with administrative permissions is not\n"
					 "permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromises.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}
#endif							/* WIN32 */
}

/*
 * 至少在 linux 上，set_ps_display() 会破坏 /proc/$pid/environ。
 * sanitizer 库使用 /proc/$pid/environ 来实现 getenv()，因为它希望
 * 独立于 libc 工作。取决于启用了哪些检测器，
 * sanitizer 库可能直到我们调用了
 * set_ps_display() 之后才初始化，从而使检测器看不到由环境提供的
 * 选项。
 *
 * 我们可以定义 __ubsan_default_options 来绕过，这是一个弱符号，
 * libsanitizer 用它从应用程序取得默认值，并返回
 * getenv("UBSAN_OPTIONS")。但仅当已经到达 main 时才这样做，以免
 * 最终依赖一个尚未能工作的 getenv()。
 *
 * 另一方面，启用不同的检测器时，libsanitizer 可能
 * 调用得如此之早，以至于它自己也没有完全初始化，从而导致
 * 递归，并在 libsanitizer 内部做核心转储。为防止这种情况，确保
 * 本函数在构建时不含任何检测器回调。
 *
 * 由于不运行检测器时不会调用本函数，似乎没有必要
 * 只在有条件时才编译它。
 */
const char *__ubsan_default_options(void);

#if __has_attribute(disable_sanitizer_instrumentation)
__attribute__((disable_sanitizer_instrumentation))
#endif
const char *
__ubsan_default_options(void)
{
	/* 在保证 libc 已初始化之前不要调用它 */
	if (!reached_main)
		return "";

	return getenv("UBSAN_OPTIONS");
}
