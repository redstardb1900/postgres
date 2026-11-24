/*-------------------------------------------------------------------------
 *
 * crashdump.c
 *		   Windows 上 PostgreSQL 的自动崩溃转储
 *
 * 崩溃转储功能捕获 backend 产生的未处理 win32 异常，
 * 并尝试生成 Windows MiniDump 崩溃
 * 转储供日后调试分析。执行转储的机器
 * 无需特殊调试工具；用户只需把转储发给
 * 持有同版本 PostgreSQL 且具备调试工具的人。
 *
 *
 * crashdump module originally by Craig Ringer <ringerc@ringerc.id.au>
 *
 * LIMITATIONS
 * ===========
 * 硬性 OOM 或栈溢出时*不能*工作。
 *
 * 那种情况须用更复杂做法：处理函数（若可能）切到
 * 新栈并 fork 辅助进程来调试自身。
 *
 *
 * POSSIBLE FUTURE WORK
 * ====================
 * 额外加分项：崩溃转储格式允许嵌入用户提供的
 * 数据。若还有总应随转储附带的内容
 * 如 postgresql.conf 或日志末几行，或可加入，
 * 但代价是转储失败的几率更大。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/crashdump.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * 某些版本的 MS SDK 含 "typedef enum { ... } ;"，MS
 * 编译器对此会合理抱怨。干得好，Microsoft。
 * 本 pragma 仅在包含该头时禁用该警告。
 * 据撰写时所知，该 pragma 适用于全部受支持的 MSVC 版本
 *
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4091)
#endif
#include <dbghelp.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/*
 * 以下代码多基于 CodeProject 与 MSDN 示例，
 * 特别是
 * http://www.codeproject.com/KB/debug/postmortemdebug_standalone1.aspx
 *
 * 有用 MSDN 文章：
 *
 * http://msdn.microsoft.com/en-us/library/ff805116(v=VS.85).aspx
 * http://msdn.microsoft.com/en-us/library/ms679294(VS.85).aspx
 *
 * 其他关于 minidump 的有用文章：
 * http://www.debuginfo.com/articles/effminidumps.html
 */

typedef BOOL (WINAPI * MINIDUMPWRITEDUMP) (HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
										   CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
										   CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
										   CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
);


/*
 * 本函数是传给 SetUnhandledExceptionFilter 的异常处理函数。
 * 仅在有未处理异常时调用。处理函数将用
 * dbghelp.dll 生成崩溃转储，然后恢复正常的未处理
 * 异常流程，通常以运行时错误消息退出。
 *
 *
 * 本函数在未处理异常处理函数下运行，实质处于
 * 崩溃语境，故应谨慎用内存并避免调用任何
 * PostgreSQL 函数。
 */
static LONG WINAPI
crashDumpHandler(struct _EXCEPTION_POINTERS *pExceptionInfo)
{
	/*
	 * 仅当 postgres 数据目录内存在 "crashdumps"
	 * 目录时才写崩溃转储。
	 */
	DWORD		attribs = GetFileAttributesA("crashdumps");

	if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY))
	{
		/* crashdumps 存在且为目录。尝试写转储 */
		HMODULE		hDll = NULL;
		MINIDUMPWRITEDUMP pDump = NULL;
		MINIDUMP_TYPE dumpType;
		char		dumpPath[_MAX_PATH];
		HANDLE		selfProcHandle = GetCurrentProcess();
		DWORD		selfPid = GetProcessId(selfProcHandle);
		HANDLE		dumpFile;
		DWORD		systemTicks;
		struct _MINIDUMP_EXCEPTION_INFORMATION ExInfo;

		ExInfo.ThreadId = GetCurrentThreadId();
		ExInfo.ExceptionPointers = pExceptionInfo;
		ExInfo.ClientPointers = FALSE;

		/* 加载 dbghelp.dll 库及函数 */
		hDll = LoadLibrary("dbghelp.dll");
		if (hDll == NULL)
		{
			write_stderr("could not load dbghelp.dll, cannot write crash dump\n");
			return EXCEPTION_CONTINUE_SEARCH;
		}

		pDump = (MINIDUMPWRITEDUMP) (pg_funcptr_t) GetProcAddress(hDll, "MiniDumpWriteDump");

		if (pDump == NULL)
		{
			write_stderr("could not load required functions in dbghelp.dll, cannot write crash dump\n");
			return EXCEPTION_CONTINUE_SEARCH;
		}

		/*
		 * 尽量多转储，但排除共享内存、代码段与
		 * 内存映射文件。具体能转储什么取决于
		 * dbghelp.dll 版本，见：
		 * http://msdn.microsoft.com/en-us/library/ms680519(v=VS.85).aspx
		 */
		dumpType = MiniDumpNormal | MiniDumpWithHandleData |
			MiniDumpWithDataSegs;

		if (GetProcAddress(hDll, "EnumDirTree") != NULL)
		{
			/* 若此函数存在，则版本为 5.2 或更新 */
			dumpType |= MiniDumpWithIndirectlyReferencedMemory |
				MiniDumpWithPrivateReadWriteMemory;
		}

		systemTicks = GetTickCount();
		snprintf(dumpPath, _MAX_PATH,
				 "crashdumps\\postgres-pid%0i-%0i.mdmp",
				 (int) selfPid, (int) systemTicks);
		dumpPath[_MAX_PATH - 1] = '\0';

		dumpFile = CreateFile(dumpPath, GENERIC_WRITE, FILE_SHARE_WRITE,
							  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
							  NULL);
		if (dumpFile == INVALID_HANDLE_VALUE)
		{
			write_stderr("could not open crash dump file \"%s\" for writing: error code %lu\n",
						 dumpPath, GetLastError());
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if ((*pDump) (selfProcHandle, selfPid, dumpFile, dumpType, &ExInfo,
					  NULL, NULL))
			write_stderr("wrote crash dump to file \"%s\"\n", dumpPath);
		else
			write_stderr("could not write crash dump to file \"%s\": error code %lu\n",
						 dumpPath, GetLastError());

		CloseHandle(dumpFile);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}


void
pgwin32_install_crashdump_handler(void)
{
	SetUnhandledExceptionFilter(crashDumpHandler);
}
