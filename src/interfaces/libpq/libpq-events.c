/*-------------------------------------------------------------------------
 *
 * libpq-events.c
 *	  functions for supporting the libpq "events" API
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/libpq-events.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "libpq-fe.h"
#include "libpq-int.h"


/*
 * 为指定的 PGconn 注册事件处理器。  
 *
 * 同一个处理器不能在 PGconn 中注册多次。              
 * 此限制是必需的，因为我们使用处理器地址来标识 
 * 事件，以用于 PQinstanceData() 等目的。
 *
 * name 参数用于错误消息中以辅助调试。                  
 * 必须提供名称，但不必唯一。该字符串会被        
 * 复制，因此传入的值不必长期存在。  
 *
 * passThrough 参数是应用程序特定的指针，如果不需要，     
 * 可以设为 NULL。每当事件处理器被调用时，它会被传     
 * 递给事件处理器，libpq 不会以其他方式使用它。 
 *
 * 成功时函数返回非零值。如果失败，                      
 * 返回零。     
 */
int
PQregisterEventProc(PGconn *conn, PGEventProc proc,
					const char *name, void *passThrough)
{
	int			i;
	PGEventRegister regevt;

	if (!proc || !conn || !name || !*name)
		return false;			/* 参数错误  */

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
			return false;		/* 此前已注册    */
	}

	if (conn->nEvents >= conn->eventArraySize)
	{
		PGEvent    *e;
		int			newSize;

		newSize = conn->eventArraySize ? conn->eventArraySize * 2 : 8;
		if (conn->events)
			e = (PGEvent *) realloc(conn->events, newSize * sizeof(PGEvent));
		else
			e = (PGEvent *) malloc(newSize * sizeof(PGEvent));

		if (!e)
			return false;

		conn->eventArraySize = newSize;
		conn->events = e;
	}

	conn->events[conn->nEvents].proc = proc;
	conn->events[conn->nEvents].name = strdup(name);
	if (!conn->events[conn->nEvents].name)
		return false;
	conn->events[conn->nEvents].passThrough = passThrough;
	conn->events[conn->nEvents].data = NULL;
	conn->events[conn->nEvents].resultInitialized = false;
	conn->nEvents++;

	regevt.conn = conn;
	if (!proc(PGEVT_REGISTER, &regevt, passThrough))
	{
		conn->nEvents--;
		free(conn->events[conn->nEvents].name);
		return false;
	}

	return true;
}

/*
 * 在 PGconn 中为事件设置"实例数据"。        
 * 成功返回非零，失败返回零。     
 */
int
PQsetInstanceData(PGconn *conn, PGEventProc proc, void *data)
{
	int			i;

	if (!conn || !proc)
		return false;

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
		{
			conn->events[i].data = data;
			return true;
		}
	}

	return false;
}

/*
 * 获取事件的"实例数据"（如果有）。   
 */
void *
PQinstanceData(const PGconn *conn, PGEventProc proc)
{
	int			i;

	if (!conn || !proc)
		return NULL;

	for (i = 0; i < conn->nEvents; i++)
	{
		if (conn->events[i].proc == proc)
			return conn->events[i].data;
	}

	return NULL;
}

/*
 * 在 PGresult 中为事件设置"实例数据"。        
 * 成功返回非零，失败返回零。     
 */
int
PQresultSetInstanceData(PGresult *result, PGEventProc proc, void *data)
{
	int			i;

	if (!result || !proc)
		return false;

	for (i = 0; i < result->nEvents; i++)
	{
		if (result->events[i].proc == proc)
		{
			result->events[i].data = data;
			return true;
		}
	}

	return false;
}

/*
 * 获取事件的"实例数据"（如果有）。   
 */
void *
PQresultInstanceData(const PGresult *result, PGEventProc proc)
{
	int			i;

	if (!result || !proc)
		return NULL;

	for (i = 0; i < result->nEvents; i++)
		if (result->events[i].proc == proc)
			return result->events[i].data;

	return NULL;
}

/*
 * 触发应用创建的 PGresult 的 RESULTCREATE 事件。    
 *
 * 若事件过程不用 conn，则 conn 参数可为 NULL。     
 */
int
PQfireResultCreateEvents(PGconn *conn, PGresult *res)
{
	int			result = true;
	int			i;

	if (!res)
		return false;

	for (i = 0; i < res->nEvents; i++)
	{
		/* 事件可能已经触发，若是这样则不要重复触发 */
		if (!res->events[i].resultInitialized)
		{
			PGEventResultCreate evt;

			evt.conn = conn;
			evt.result = res;
			if (res->events[i].proc(PGEVT_RESULTCREATE, &evt,
									res->events[i].passThrough))
				res->events[i].resultInitialized = true;
			else
				result = false;
		}
	}

	return result;
}
