/*-------------------------------------------------------------------------
 *
 * libpq-be-fe-helpers.h
 *	  在扩展中使用 libpq 的辅助函数
 *
 * 直接构建进后端的代码不允许直接链接到 libpq。
 * 扩展代码则允许使用 libpq。不过，扩展中使用的 libpq
 * 在 libpq 内部阻塞时必须小心，否则
 * 中断将得不到处理，从而造成无法解除的
 * 死锁之类的问题。后端代码还须注意为该连接
 * 获取并释放一个外部 fd，否则 fd.c 对 fd 的记账
 * 就会出错。
 *
 * 本文件提供辅助函数，以便更容易遵守这些
 * 规则。它是一个仅含头文件的库，因为需要链接进每一个
 * 使用 libpq 的扩展，而且它看起来太小，不值得为此
 * 再增加一个专用的静态库。
 *
 * TODO：出于历史原因，这里建立的连接没有被置为
 * 非阻塞模式。即便只用异步的 libpq 函数，
 * 这也可能导致阻塞。这一点应当修正。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-be-fe-helpers.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_FE_HELPERS_H
#define LIBPQ_BE_FE_HELPERS_H

/*
 * 尽管名字如此，BUILDING_DLL 只在编译直接属于后端的代码时
 * 设置。那里也不允许使用 libpq。这显然挡不住 libpq-fe.h
 * 在其他情况下被包含，但也许仍能防止一些失误...
 *
 */
#ifdef BUILDING_DLL
#error "libpq may not be used code directly built into the backend"
#endif

#include "libpq-fe.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/wait_event.h"


static inline void libpqsrv_connect_prepare(void);
static inline void libpqsrv_connect_internal(PGconn *conn, uint32 wait_event_info);
static inline PGresult *libpqsrv_get_result_last(PGconn *conn, uint32 wait_event_info);
static inline PGresult *libpqsrv_get_result(PGconn *conn, uint32 wait_event_info);


/*
 * PQconnectdb() 的包装，在建立连接期间保留一个文件描述符
 * 并处理中断。
 *
 * 若 AcquireExternalFD() 失败则抛出错误，但连接建立本身
 * 失败时不抛出。调用者需要用 PQstatus() 检查连接是否
 * 建立成功。
 */
static inline PGconn *
libpqsrv_connect(const char *conninfo, uint32 wait_event_info)
{
	PGconn	   *conn = NULL;

	libpqsrv_connect_prepare();

	conn = PQconnectStart(conninfo);

	libpqsrv_connect_internal(conn, wait_event_info);

	return conn;
}

/*
 * 同 libpqsrv_connect()，但这是 PQconnectdbParams() 的包装。
 *
 */
static inline PGconn *
libpqsrv_connect_params(const char *const *keywords,
						const char *const *values,
						int expand_dbname,
						uint32 wait_event_info)
{
	PGconn	   *conn = NULL;

	libpqsrv_connect_prepare();

	conn = PQconnectStartParams(keywords, values, expand_dbname);

	libpqsrv_connect_internal(conn, wait_event_info);

	return conn;
}

/*
 * PQfinish() 的包装，另外会释放保留的文件描述符。
 *
 * 若 libpqsrv_connect* 返回了 NULL，则允许用 NULL 的 pgconn
 * 调用本函数。
 */
static inline void
libpqsrv_disconnect(PGconn *conn)
{
	/*
 * 若没有建立连接，我们就没有为其保留文件描述符（或已经
 * 释放了）。这条规则让本设施的使用者更容易编写 PG_CATCH()
 * 处理函数。
 *
 * 另见 libpqsrv_connect_internal()。
 */
	if (conn == NULL)
		return;

	ReleaseExternalFD();
	PQfinish(conn);
}


/* 以下是内部辅助函数 */


/*
 * 所有建立连接函数的辅助函数。
 */
static inline void
libpqsrv_connect_prepare(void)
{
	/*
 * 我们必须遵守 fd.c 对非虚拟文件描述符的限制。假定一个
 * PGconn 代表一个长期存在的文件描述符。（在这里做这件事
 * 也能确保必要时关闭 VFDs 以腾出位置。）
 */
	if (!AcquireExternalFD())
	{
#ifndef WIN32					/* 无法在 ereport() 宏内部写 #if */
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("There are too many open files on the local server."),
				 errhint("Raise the server's max_files_per_process and/or \"ulimit -n\" limits.")));
#else
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("There are too many open files on the local server."),
				 errhint("Raise the server's max_files_per_process setting.")));
#endif
	}
}

/*
 * 所有建立连接函数的辅助函数。
 */
static inline void
libpqsrv_connect_internal(PGconn *conn, uint32 wait_event_info)
{
	/*
 * conn == NULL 时 libpqsrv_disconnect() 不会释放文件描述符。
 * 因此在这里释放。
 */
	if (conn == NULL)
	{
		ReleaseExternalFD();
		return;
	}

	/*
 * 没有套接字就无法等待。注意我们还不想关闭 libpq 连接，
 * 以便调用者能发出有用的错误。
 */
	if (PQstatus(conn) == CONNECTION_BAD)
		return;

	/*
 * WaitLatchOrSocket() 有可能失败，在这里处理这种情况，
 * 而不是要求所有调用者都去处理。
 */
	PG_TRY();
	{
		PostgresPollingStatusType status;

		/*
 * 轮询连接，直到状态为 OK 或 FAILED。
 *
 * 按 PQconnectPoll 的约定，首先等到套接字可写。
 */
		status = PGRES_POLLING_WRITING;
		while (status != PGRES_POLLING_OK && status != PGRES_POLLING_FAILED)
		{
			int			io_flag;
			int			rc;

			if (status == PGRES_POLLING_READING)
				io_flag = WL_SOCKET_READABLE;
#ifdef WIN32

			/*
 * 等待连接建立时，Windows 需要不同的测试。
 *
 */
			else if (PQstatus(conn) == CONNECTION_STARTED)
				io_flag = WL_SOCKET_CONNECTED;
#endif
			else
				io_flag = WL_SOCKET_WRITEABLE;

			rc = WaitLatchOrSocket(MyLatch,
								   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET | io_flag,
								   PQsocket(conn),
								   0,
								   wait_event_info);

			/* 被中断了？ */
			if (rc & WL_LATCH_SET)
			{
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
			}

			/* 若套接字就绪，推进 libpq 状态机 */
			if (rc & io_flag)
				status = PQconnectPoll(conn);
		}
	}
	PG_CATCH();
	{
		/*
 * 若这里抛出错误，调用者不会带着 conn 调用
 * libpqsrv_disconnect()，因此立即释放资源。
 *
 */
		ReleaseExternalFD();
		PQfinish(conn);

		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * 处理中断的 PQexec() 包装。
 *
 * 除非 PQsetnonblocking(conn, 1) 生效，否则在把查询文本
 * 推向服务器时无法处理中断。若查询串相对 TCP 缓冲区
 * 可能很长，请考虑该设置。
 *
 * 它具有 PQsendQuery() 的前提条件，而不是 PQexec() 的。
 * 尤其是，PQexec() 会默默丢弃任何先前的查询结果。
 */
static inline PGresult *
libpqsrv_exec(PGconn *conn, const char *query, uint32 wait_event_info)
{
	if (!PQsendQuery(conn, query))
		return NULL;
	return libpqsrv_get_result_last(conn, wait_event_info);
}

/*
 * 处理中断的 PQexecParams() 包装。
 *
 * 见 libpqsrv_exec() 处的说明。
 */
static inline PGresult *
libpqsrv_exec_params(PGconn *conn,
					 const char *command,
					 int nParams,
					 const Oid *paramTypes,
					 const char *const *paramValues,
					 const int *paramLengths,
					 const int *paramFormats,
					 int resultFormat,
					 uint32 wait_event_info)
{
	if (!PQsendQueryParams(conn, command, nParams, paramTypes, paramValues,
						   paramLengths, paramFormats, resultFormat))
		return NULL;
	return libpqsrv_get_result_last(conn, wait_event_info);
}

/*
 * 同 PQexec()，循环调用 PQgetResult()，直到它返回 NULL 或
 * 另一终态。返回最后一个非 NULL 结果或该终态。
 */
static inline PGresult *
libpqsrv_get_result_last(PGconn *conn, uint32 wait_event_info)
{
	PGresult   *volatile lastResult = NULL;

	/* 以下过程中，出错时不要泄漏任何 PGresults。 */
	PG_TRY();
	{
		for (;;)
		{
			/* 等待并收集下一个 PGresult。 */
			PGresult   *result;

			result = libpqsrv_get_result(conn, wait_event_info);
			if (result == NULL)
				break;			/* 查询完成，或失败 */

			/*
 * 仿照 PQexec() 的行为：有多个结果时返回最后一个。
 *
 */
			PQclear(lastResult);
			lastResult = result;

			if (PQresultStatus(lastResult) == PGRES_COPY_IN ||
				PQresultStatus(lastResult) == PGRES_COPY_OUT ||
				PQresultStatus(lastResult) == PGRES_COPY_BOTH ||
				PQstatus(conn) == CONNECTION_BAD)
				break;
		}
	}
	PG_CATCH();
	{
		PQclear(lastResult);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return lastResult;
}

/*
 * 做与 PQgetResult() 等价的事，但留意中断。
 */
static inline PGresult *
libpqsrv_get_result(PGconn *conn, uint32 wait_event_info)
{
	/*
 * 收集数据，直到 PQgetResult 可以不阻塞地取得结果。
 *
 */
	while (PQisBusy(conn))
	{
		int			rc;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_EXIT_ON_PM_DEATH | WL_LATCH_SET |
							   WL_SOCKET_READABLE,
							   PQsocket(conn),
							   0,
							   wait_event_info);

		/* 被中断了？ */
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		/* 消费套接字上已有的任何数据 */
		if (PQconsumeInput(conn) == 0)
		{
			/* 出了问题；预期 PQgetResult() 返回 NULL */
			break;
		}
	}

	/* 现在可以收集并返回下一个 PGresult */
	return PQgetResult(conn);
}

#endif							/* LIBPQ_BE_FE_HELPERS_H */
