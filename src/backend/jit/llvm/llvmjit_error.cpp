/*-------------------------------------------------------------------------
 *
 * llvmjit_error.cpp
 *	  需与 C++ 对接的 LLVM 错误处理
 *
 * 不幸的是，(重新)设置 C++ new 处理函数与 LLVM 的 OOM
 * 处理函数均未向 C 暴露。故本文件包装所需代码。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_error.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include <llvm/Support/ErrorHandling.h>

#include "jit/llvmjit.h"

#include <new>

static int fatal_new_handler_depth = 0;
static std::new_handler old_new_handler = NULL;

static void fatal_system_new_handler(void);
#if LLVM_VERSION_MAJOR > 4
static void fatal_llvm_new_handler(void *user_data, const char *reason, bool gen_crash_diag);
#if LLVM_VERSION_MAJOR < 14
static void fatal_llvm_new_handler(void *user_data, const std::string& reason, bool gen_crash_diag);
#endif
#endif
static void fatal_llvm_error_handler(void *user_data, const char *reason, bool gen_crash_diag);
#if LLVM_VERSION_MAJOR < 14
static void fatal_llvm_error_handler(void *user_data, const std::string& reason, bool gen_crash_diag);
#endif


/*
 * 进入一段区间：C++ 与 LLVM 错误均按 FATAL 处理。
 *
 * 对 LLVM 有必要，因其对此类情况的错误处理
 * 即 exit()、在启用异常时抛 std::bad_alloc()、abort()
 * 与 postgres 错误处理不兼容。故在执行 LLVM 代码
 * 非 LLVM 生成的函数时，把标准 new、LLVM 的 OOM
 * 及 LLVM 致命错误（有些 OOM 会伪装成这类）改送到
 * 自有错误处理。
 *
 * 这些处理函数用 FATAL，因为从 LLVM 内部没有可靠办法
 * 抛出一个保证不破坏 LLVM 状态的错误。
 *
 * 为免打扰使用 C++ 和/或 LLVM 的扩展，这些处理函数在
 * 未执行 LLVM 代码时卸下。ERROR 退出时不必调用
 * llvm_leave_fatal_on_oom()，错误恢复那时会重置
 * 这些处理函数。
 */
void
llvm_enter_fatal_on_oom(void)
{
	if (fatal_new_handler_depth == 0)
	{
		old_new_handler = std::set_new_handler(fatal_system_new_handler);
#if LLVM_VERSION_MAJOR > 4
		llvm::install_bad_alloc_error_handler(fatal_llvm_new_handler);
#endif
		llvm::install_fatal_error_handler(fatal_llvm_error_handler);
	}
	fatal_new_handler_depth++;
}

/*
 * 离开 llvm_enter_fatal_on_oom() 开始的致命错误区间。
 */
void
llvm_leave_fatal_on_oom(void)
{
	fatal_new_handler_depth--;
	if (fatal_new_handler_depth == 0)
	{
		std::set_new_handler(old_new_handler);
#if LLVM_VERSION_MAJOR > 4
		llvm::remove_bad_alloc_error_handler();
#endif
		llvm::remove_fatal_error_handler();
	}
}

/*
 * 当前是否处于 fatal-on-oom 区间？出错时可跳过清理。
 *
 */
bool
llvm_in_fatal_on_oom(void)
{
	return fatal_new_handler_depth > 0;
}

/*
 * 重置致命错误处理。只应在错误恢复循环中调用，
 * 如 PostgresMain()。
 */
void
llvm_reset_after_error(void)
{
	if (fatal_new_handler_depth != 0)
	{
		std::set_new_handler(old_new_handler);
#if LLVM_VERSION_MAJOR > 4
		llvm::remove_bad_alloc_error_handler();
#endif
		llvm::remove_fatal_error_handler();
	}
	fatal_new_handler_depth = 0;
}

void
llvm_assert_in_fatal_section(void)
{
	Assert(fatal_new_handler_depth > 0);
}

static void
fatal_system_new_handler(void)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory"),
			 errdetail("while in LLVM")));
}

#if LLVM_VERSION_MAJOR > 4
static void
fatal_llvm_new_handler(void *user_data,
					   const char *reason,
					   bool gen_crash_diag)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory"),
			 errdetail("While in LLVM: %s", reason)));
}
#if LLVM_VERSION_MAJOR < 14
static void
fatal_llvm_new_handler(void *user_data,
					   const std::string& reason,
					   bool gen_crash_diag)
{
	fatal_llvm_new_handler(user_data, reason.c_str(), gen_crash_diag);
}
#endif
#endif

static void
fatal_llvm_error_handler(void *user_data,
						 const char *reason,
						 bool gen_crash_diag)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("fatal llvm error: %s", reason)));
}

#if LLVM_VERSION_MAJOR < 14
static void
fatal_llvm_error_handler(void *user_data,
						 const std::string& reason,
						 bool gen_crash_diag)
{
	fatal_llvm_error_handler(user_data, reason.c_str(), gen_crash_diag);
}
#endif
