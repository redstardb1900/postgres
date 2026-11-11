/*-------------------------------------------------------------------------
 *
 * fe-exec.c
 *	  functions related to sending a query down to the backend
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-exec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"

/* 保持与 libpq-fe.h 中 ExecStatusType 相同顺序    */
char	   *const pgresStatus[] = {
	"PGRES_EMPTY_QUERY",
	"PGRES_COMMAND_OK",
	"PGRES_TUPLES_OK",
	"PGRES_COPY_OUT",
	"PGRES_COPY_IN",
	"PGRES_BAD_RESPONSE",
	"PGRES_NONFATAL_ERROR",
	"PGRES_FATAL_ERROR",
	"PGRES_COPY_BOTH",
	"PGRES_SINGLE_TUPLE",
	"PGRES_PIPELINE_SYNC",
	"PGRES_PIPELINE_ABORTED"
};

/* 如果完全无法创建 PGresult，则返回此结构   */
static const PGresult OOM_result = {
	.resultStatus = PGRES_FATAL_ERROR,
	.client_encoding = PG_SQL_ASCII,
	.errMsg = "out of memory\n",
};

/*
PQescapeString 和 PQescapeBytea 所需的静态状态；
 *
 * 初始化为产生向后兼容行为的值                       */
static int	static_client_encoding = PG_SQL_ASCII;
static bool static_std_strings = false;


static PGEvent *dupEvents(PGEvent *events, int count, size_t *memSize);
static bool pqAddTuple(PGresult *res, PGresAttValue *tup,
					   const char **errmsgp);
static int	PQsendQueryInternal(PGconn *conn, const char *query, bool newQuery);
static bool PQsendQueryStart(PGconn *conn, bool newQuery);
static int	PQsendQueryGuts(PGconn *conn,
							const char *command,
							const char *stmtName,
							int nParams,
							const Oid *paramTypes,
							const char *const *paramValues,
							const int *paramLengths,
							const int *paramFormats,
							int resultFormat);
static void parseInput(PGconn *conn);
static PGresult *getCopyResult(PGconn *conn, ExecStatusType copytype);
static bool PQexecStart(PGconn *conn);
static PGresult *PQexecFinish(PGconn *conn);
static int	PQsendDescribe(PGconn *conn, char desc_type,
						   const char *desc_target);
static int	check_field_number(const PGresult *res, int field_num);
static void pqPipelineProcessQueue(PGconn *conn);
static int	pqPipelineFlush(PGconn *conn);


/* ----------------
 * PGresult 的空间管理。   
 *
 * 以前，libpq 为查询返回的每个元组字段单独调用     
 * malloc()。这种做法极其昂贵 --- malloc/free 消耗了应用
 * 程序运行时间的相当大一部分。而且没有必要单独 
 * 跟踪各个字段，因为当 PGresult 被释放时                
 * 它们会一起释放。所以现在，我们从 malloc 获取       
 * 大块存储空间，并在其中用简单的分配器为查询数据
 * 分配空间。这大大减少了 malloc/free 调用的次数，      
 * 同时也避免了 malloc 存储区域出现                    
 * 碎片化。                              
 * PGresult 结构本身仍单独 malloc 分配。本可以把它与    
 * 第一个分配块合并，但对于常见的不需要额外存储的 
 * 情况来说这样做会浪费空间（即 SQL 命令未返回        
 * 元组）。                           
 *
 * 我们还单独分配顶层的元组指针数组，因为需要通过   
 * realloc 扩大它，简单空间分配器无法有效处理这种  
 * 情况。（遗憾的是 FE/BE 协议不会预先告诉我们将返回 
 * 多少元组。）                                         
 * PGresult 的所有其他辅助存储都保存在大小为                  
 * PGRESULT_DATA_BLOCKSIZE 的 PGresult_data 块中。每个块             
 * 开头的开销为指向下一块的链接。空闲空间管理信息 
 * 保存在所属 PGresult。 
 * 因此，返回少量数据的查询需要三次 malloc 调用：一次
 * 用于 PGresult，一次用于元组指针数组，一次用于       
 * PGresult_data 块。
 *
 * 只有最近分配的 PGresult_data 块才是可继续添加内容的 
 * 候选 --- 旧块中剩余的额外空间都会浪费掉。我们可以  
 * 做得更聪明并搜索整条链，但重点是简单快速。典型的 
 * 应用程序本来也不会长时间保留 PGresult，因此其中一个内
 * 部浪费一些空间并不是问题。                                   
 *
 * 空间分配器的调优常量如下：      
 * PGRESULT_DATA_BLOCKSIZE：标准分配块的大小（字节）        
 * PGRESULT_ALIGN_BOUNDARY：二进制数据的假定对齐要求        
 * PGRESULT_SEP_ALLOC_THRESHOLD：大于此值的对象将分配            
 *	 独立块，而不是挤入常规分配块中。                 
 * 正确工作的要求如下：        
 * PGRESULT_ALIGN_BOUNDARY 必须是所有机器数据类型对齐要      
 *		求的倍数。（当前由 configure 测试设置，应当自动
 *		正确。）                             
 * PGRESULT_SEP_ALLOC_THRESHOLD + PGRESULT_BLOCK_OVERHEAD <=
 *			PGRESULT_DATA_BLOCKSIZE
 *		pqResultAlloc 假设小于阈值的对象可以放进              
 *		新块中。   
 * 一个块末尾浪费的空间量可能多达                       
 * PGRESULT_SEP_ALLOC_THRESHOLD，所以把它设得太大并不划算。 
 * ----------------
 */

#define PGRESULT_DATA_BLOCKSIZE		2048
#define PGRESULT_ALIGN_BOUNDARY		MAXIMUM_ALIGNOF /*来自 configure*/
#define PGRESULT_BLOCK_OVERHEAD		Max(sizeof(PGresult_data), PGRESULT_ALIGN_BOUNDARY)
#define PGRESULT_SEP_ALLOC_THRESHOLD	(PGRESULT_DATA_BLOCKSIZE / 2)


/*
PQmakeEmptyPGresult
 *   返回新分配的已初始化 PGresult，给定状态。
 *   如果 conn 非 NULL 且状态指示错误，复制 conn 的 errorMessage。
 *   同时从 conn 复制所有 PGEvents。
 *
 * 注意：复制 errorMessage 的逻辑现已过时；没有内部调用者使用。
 *
 *
 * 但该行为已为外部调用者文档化，所以最好保留。                                                 */
PGresult *
PQmakeEmptyPGresult(PGconn *conn, ExecStatusType status)
{
	PGresult   *result;

	result = (PGresult *) malloc(sizeof(PGresult));
	if (!result)
		return NULL;

	result->ntups = 0;
	result->numAttributes = 0;
	result->attDescs = NULL;
	result->tuples = NULL;
	result->tupArrSize = 0;
	result->numParameters = 0;
	result->paramDescs = NULL;
	result->resultStatus = status;
	result->cmdStatus[0] = '\0';
	result->binary = 0;
	result->events = NULL;
	result->nEvents = 0;
	result->errMsg = NULL;
	result->errFields = NULL;
	result->errQuery = NULL;
	result->null_field[0] = '\0';
	result->curBlock = NULL;
	result->curOffset = 0;
	result->spaceLeft = 0;
	result->memorySize = sizeof(PGresult);

	if (conn)
	{
		/* 复制操作 PGresult 可能需要的连接数据             */
		result->noticeHooks = conn->noticeHooks;
		result->client_encoding = conn->client_encoding;

		/* 考虑复制 conn 的 errorMessage   */
		switch (status)
		{
			case PGRES_EMPTY_QUERY:
			case PGRES_COMMAND_OK:
			case PGRES_TUPLES_OK:
			case PGRES_COPY_OUT:
			case PGRES_COPY_IN:
			case PGRES_COPY_BOTH:
			case PGRES_SINGLE_TUPLE:
				/* 非错误情况 */
				break;
			default:
				/* 这里故意不使用也不修改 errorReported          */
				pqSetResultError(result, &conn->errorMessage, 0);
				break;
		}

		/* 最后复制事件；若需 PQclear，结果必须有效     */
		if (conn->nEvents > 0)
		{
			result->events = dupEvents(conn->events, conn->nEvents,
									   &result->memorySize);
			if (!result->events)
			{
				PQclear(result);
				return NULL;
			}
			result->nEvents = conn->nEvents;
		}
	}
	else
	{
		/* 默认...   */
		result->noticeHooks.noticeRec = NULL;
		result->noticeHooks.noticeRecArg = NULL;
		result->noticeHooks.noticeProc = NULL;
		result->noticeHooks.noticeProcArg = NULL;
		result->client_encoding = PG_SQL_ASCII;
	}

	return result;
}

/*
PQsetResultAttrs
 *
 * 为给定结果设置属性。如果提供的结果中已包含属性，
 * 则此函数失败。如果 numAttributes 为零或 attDescs 为 NULL，
 * 则忽略此调用。如果函数失败，返回零。如果函数成功，
 *
 *
 * 返回非零值。                                                   */
int
PQsetResultAttrs(PGresult *res, int numAttributes, PGresAttDesc *attDescs)
{
	int			i;

	/* 参数 NULL 或 OOM_result 时失败   */
	if (!res || (const PGresult *) res == &OOM_result)
		return false;

	/* 如果属性已存在，则不能被覆盖。       */
	if (res->numAttributes > 0)
		return false;

	/* 忽略无操作请求*/
	if (numAttributes <= 0 || !attDescs)
		return true;

	res->attDescs = (PGresAttDesc *)
		PQresultAlloc(res, numAttributes * sizeof(PGresAttDesc));

	if (!res->attDescs)
		return false;

	res->numAttributes = numAttributes;
	memcpy(res->attDescs, attDescs, numAttributes * sizeof(PGresAttDesc));

	/* 深拷贝属性名称，并确定格式             */
	res->binary = 1;
	for (i = 0; i < res->numAttributes; i++)
	{
		if (res->attDescs[i].name)
			res->attDescs[i].name = pqResultStrdup(res, res->attDescs[i].name);
		else
			res->attDescs[i].name = res->null_field;

		if (!res->attDescs[i].name)
			return false;

		if (res->attDescs[i].format == 0)
			res->binary = 0;
	}

	return true;
}

/*
PQcopyResult
 *
 * 返回提供的 'src' PGresult 的深拷贝，src 不能为 NULL。
 * 'flags' 参数控制结果的哪些部分会被复制，哪些不会被复制。
 * 创建的结果始终设置为 PGRES_TUPLES_OK 状态。
 * 源结果的错误消息不会被复制，但 cmdStatus 会被复制。
 *
 * 要设置自定义属性，请使用 PQsetResultAttrs。该函数要求
 * 结果中不包含任何属性，因此要使用该函数，
 * 不能在此函数中使用 PG_COPYRES_ATTRS 或 PG_COPYRES_TUPLES
 * 选项。
 *
 * 选项：
 *   PG_COPYRES_ATTRS - 复制源结果的属性
 *
 *   PG_COPYRES_TUPLES - 复制源结果的元组。这隐含了
 *   复制属性，因为元组需要属性。
 *
 *   PG_COPYRES_EVENTS - 复制源结果的事件。
 *
 *
 *
 *   PG_COPYRES_NOTICEHOOKS - 复制源结果的通知钩子。                                                                                        */
PGresult *
PQcopyResult(const PGresult *src, int flags)
{
	PGresult   *dest;
	int			i;

	if (!src)
		return NULL;

	dest = PQmakeEmptyPGresult(NULL, PGRES_TUPLES_OK);
	if (!dest)
		return NULL;

	/* 始终复制这些。cmdStatus 真的有用吗？         */
	dest->client_encoding = src->client_encoding;
	strcpy(dest->cmdStatus, src->cmdStatus);

	/* 要属性？ */
	if (flags & (PG_COPYRES_ATTRS | PG_COPYRES_TUPLES))
	{
		if (!PQsetResultAttrs(dest, src->numAttributes, src->attDescs))
		{
			PQclear(dest);
			return NULL;
		}
	}

	/* 需要复制元组？ */
	if (flags & PG_COPYRES_TUPLES)
	{
		int			tup,
					field;

		for (tup = 0; tup < src->ntups; tup++)
		{
			for (field = 0; field < src->numAttributes; field++)
			{
				if (!PQsetvalue(dest, tup, field,
								src->tuples[tup][field].value,
								src->tuples[tup][field].len))
				{
					PQclear(dest);
					return NULL;
				}
			}
		}
	}

	/* 需要复制通知钩子？ */
	if (flags & PG_COPYRES_NOTICEHOOKS)
		dest->noticeHooks = src->noticeHooks;

	/* 要复制 PGEvents？   */
	if ((flags & PG_COPYRES_EVENTS) && src->nEvents > 0)
	{
		dest->events = dupEvents(src->events, src->nEvents,
								 &dest->memorySize);
		if (!dest->events)
		{
			PQclear(dest);
			return NULL;
		}
		dest->nEvents = src->nEvents;
	}

	/* 触发 PGEVT_RESULTCOPY 事件       */
	for (i = 0; i < dest->nEvents; i++)
	{
		/* 不触发之前有失败的事件                   */
		if (src->events[i].resultInitialized)
		{
			PGEventResultCopy evt;

			evt.src = src;
			evt.dest = dest;
			if (dest->events[i].proc(PGEVT_RESULTCOPY, &evt,
									 dest->events[i].passThrough))
				dest->events[i].resultInitialized = true;
		}
	}

	return dest;
}

/*
复制 PGEvents 数组（不预留额外空间）。
 * 不复制事件实例数据，将其设置为 NULL。
 * 同时，所有 resultInitialized 标志都被清除。
 *
 * 分配的总空间计入 *memSize。                      */
static PGEvent *
dupEvents(PGEvent *events, int count, size_t *memSize)
{
	PGEvent    *newEvents;
	size_t		msize;
	int			i;

	if (!events || count <= 0)
		return NULL;

	msize = count * sizeof(PGEvent);
	newEvents = (PGEvent *) malloc(msize);
	if (!newEvents)
		return NULL;

	for (i = 0; i < count; i++)
	{
		newEvents[i].proc = events[i].proc;
		newEvents[i].passThrough = events[i].passThrough;
		newEvents[i].data = NULL;
		newEvents[i].resultInitialized = false;
		newEvents[i].name = strdup(events[i].name);
		if (!newEvents[i].name)
		{
			while (--i >= 0)
				free(newEvents[i].name);
			free(newEvents);
			return NULL;
		}
		msize += strlen(events[i].name) + 1;
	}

	*memSize += msize;
	return newEvents;
}


/*
设置元组字段的值。tup_num 必须小于或等于
 * PQntuples(res)。如果相等，则创建一个新元组并
 * 添加到结果中。
 * 成功返回非零值，失败返回零。
 *
 * （失败时，我们通过 pqInternalNotice 报告具体问题。）                                  */
int
PQsetvalue(PGresult *res, int tup_num, int field_num, char *value, int len)
{
	PGresAttValue *attval;
	const char *errmsg = NULL;

	/* 参数 NULL 或 OOM_result 时失败   */
	if (!res || (const PGresult *) res == &OOM_result)
		return false;

	/* 无效 field_num？*/
	if (!check_field_number(res, field_num))
		return false;

	/* tup_num 无效，须 <= ntups     */
	if (tup_num < 0 || tup_num > res->ntups)
	{
		pqInternalNotice(&res->noticeHooks,
						 "row number %d is out of range 0..%d",
						 tup_num, res->ntups);
		return false;
	}

	/* 需要分配新元组？      */
	if (tup_num == res->ntups)
	{
		PGresAttValue *tup;
		int			i;

		tup = (PGresAttValue *)
			pqResultAlloc(res, res->numAttributes * sizeof(PGresAttValue),
						  true);

		if (!tup)
			goto fail;

		/* 将每列初始化为 NULL     */
		for (i = 0; i < res->numAttributes; i++)
		{
			tup[i].len = NULL_LEN;
			tup[i].value = res->null_field;
		}

		/* 加入数组        */
		if (!pqAddTuple(res, tup, &errmsg))
			goto fail;
	}

	attval = &res->tuples[tup_num][field_num];

	/* 将 NULL_LEN 或 NULL 值指针视为 NULL 字段           */
	if (len == NULL_LEN || value == NULL)
	{
		attval->len = NULL_LEN;
		attval->value = res->null_field;
	}
	else if (len <= 0)
	{
		attval->len = 0;
		attval->value = res->null_field;
	}
	else
	{
		attval->value = (char *) pqResultAlloc(res, (size_t) len + 1, true);
		if (!attval->value)
			goto fail;
		attval->len = len;
		memcpy(attval->value, value, len);
		attval->value[len] = '\0';
	}

	return true;

	/*
	通过 pqInternalNotice 报告失败。如果前面的代码没有提供
	 *
	 * 错误消息，则假定指的是"内存不足"。 */
fail:
	if (!errmsg)
		errmsg = libpq_gettext("out of memory");
	pqInternalNotice(&res->noticeHooks, "%s", errmsg);

	return false;
}

/*
 * pqResultAlloc - 在 PGresult 中分配本地存储的导出例程。     
 *
 * 我们强制这类分配全部最大对齐，因为不知道值    
 * 是否可能是二进制的。    
 */
void *
PQresultAlloc(PGresult *res, size_t nBytes)
{
	/* 参数 NULL 或 OOM_result 时失败   */
	if (!res || (const PGresult *) res == &OOM_result)
		return NULL;

	return pqResultAlloc(res, nBytes, true);
}

/*
pqResultAlloc -
 *   为 PGresult 分配辅助存储。
 *
 * nBytes 是对象所需的空间量。
 * 如果 isBinary 为 true，我们假设需要将对象对齐到
 * 机器分配边界上。
 * 如果 isBinary 为 false，我们假设对象是字符串，可以
 *
 * 分配在任意字节边界上。                                   */
void *
pqResultAlloc(PGresult *res, size_t nBytes, bool isBinary)
{
	char	   *space;
	PGresult_data *block;

	if (!res)
		return NULL;

	if (nBytes <= 0)
		return res->null_field;

	/*
	如果需要对齐，将当前位置向上舍入到对齐
	 *
	 * 边界。              */
	if (isBinary)
	{
		int			offset = res->curOffset % PGRESULT_ALIGN_BOUNDARY;

		if (offset)
		{
			res->curOffset += PGRESULT_ALIGN_BOUNDARY - offset;
			res->spaceLeft -= PGRESULT_ALIGN_BOUNDARY - offset;
		}
	}

	/* 如果当前块中有足够空间，没问题。          */
	if (nBytes <= (size_t) res->spaceLeft)
	{
		space = res->curBlock->space + res->curOffset;
		res->curOffset += nBytes;
		res->spaceLeft -= nBytes;
		return space;
	}

	/*
	如果请求的对象非常大，给它自己的块；这样可以
	 * 避免浪费当前块的大部分空间来启动新块。
	 * （无论如何，我们必须特殊处理大于块大小的请求。）
	 *
	 * 在这种情况下，对象始终获得二进制对齐。                      */
	if (nBytes >= PGRESULT_SEP_ALLOC_THRESHOLD)
	{
		size_t		alloc_size;

		/* 对于过大的请求，不要导致回绕。 */
		if (nBytes > SIZE_MAX - PGRESULT_BLOCK_OVERHEAD)
			return NULL;

		alloc_size = nBytes + PGRESULT_BLOCK_OVERHEAD;
		block = (PGresult_data *) malloc(alloc_size);
		if (!block)
			return NULL;
		res->memorySize += alloc_size;
		space = block->space + PGRESULT_BLOCK_OVERHEAD;
		if (res->curBlock)
		{
			/*
			 * 将特殊块放在活动块下方，这样我们就不会  
			 * 浪费活动块中的空闲空间。             
			 */
			block->next = res->curBlock->next;
			res->curBlock->next = block;
		}
		else
		{
			/* 必须将新块设置为第一个活动块。        */
			block->next = NULL;
			res->curBlock = block;
			res->spaceLeft = 0; /* 确保标记为已满    */
		}
		return space;
	}

	/* 否则启动新块。         */
	block = (PGresult_data *) malloc(PGRESULT_DATA_BLOCKSIZE);
	if (!block)
		return NULL;
	res->memorySize += PGRESULT_DATA_BLOCKSIZE;
	block->next = res->curBlock;
	res->curBlock = block;
	if (isBinary)
	{
		/* 对象需要完全对齐    */
		res->curOffset = PGRESULT_BLOCK_OVERHEAD;
		res->spaceLeft = PGRESULT_DATA_BLOCKSIZE - PGRESULT_BLOCK_OVERHEAD;
	}
	else
	{
		/* 我们可以把它紧接在开销指针后面   */
		res->curOffset = sizeof(PGresult_data);
		res->spaceLeft = PGRESULT_DATA_BLOCKSIZE - sizeof(PGresult_data);
	}

	space = block->space + res->curOffset;
	res->curOffset += nBytes;
	res->spaceLeft -= nBytes;
	return space;
}

/*
PQresultMemorySize -
 *
 *   返回为 PGresult 分配的总空间。        */
size_t
PQresultMemorySize(const PGresult *res)
{
	if (!res)
		return 0;
	return res->memorySize;
}

/*
pqResultStrdup -
 *
 *   类似 strdup，但空间是 PGresult 的辅助空间。 */
char *
pqResultStrdup(PGresult *res, const char *str)
{
	char	   *space = (char *) pqResultAlloc(res, strlen(str) + 1, false);

	if (space)
		strcpy(space, str);
	return space;
}

/*
pqSetResultError -
 *   为 PGresult 分配新的错误消息
 *
 * 从 errorMessage 缓冲区的给定偏移处开始复制文本
 *
 * （调用者负责确保偏移量有效）           */
void
pqSetResultError(PGresult *res, PQExpBuffer errorMessage, int offset)
{
	char	   *msg;

	if (!res)
		return;

	/*
	我们在这里处理两种 OOM 场景。errorMessage 缓冲区可能
	 * 被标记为"已损坏"（由于之前分配消息内存失败），
	 * 或者它可能正常但 pqResultStrdup 失败并返回 NULL。
	 * 无论哪种情况，只需让 res->errMsg 直接指向
	 *
	 * 常量"out of memory"字符串。                       */
	if (!PQExpBufferBroken(errorMessage))
		msg = pqResultStrdup(res, errorMessage->data + offset);
	else
		msg = NULL;
	if (msg)
		res->errMsg = msg;
	else
		res->errMsg = libpq_gettext("out of memory\n");
}

/*
PQclear -
 *
 *   释放与 PGresult 关联的内存            */
void
PQclear(PGresult *res)
{
	PGresult_data *block;
	int			i;

	/* 方便起见，NULL 指针无操作             */
	if (!res)
		return;
	/* 参数为 OOM_result 也无操作              */
	if ((const PGresult *) res == &OOM_result)
		return;

	/* 关闭可能的事件             */
	for (i = 0; i < res->nEvents; i++)
	{
		/* 只向成功初始化的事件处理器发送 DESTROY     */
		if (res->events[i].resultInitialized)
		{
			PGEventResultDestroy evt;

			evt.result = res;
			(void) res->events[i].proc(PGEVT_RESULTDESTROY, &evt,
									   res->events[i].passThrough);
		}
		free(res->events[i].name);
	}

	free(res->events);

	/* 释放所有辅助块          */
	while ((block = res->curBlock) != NULL)
	{
		res->curBlock = block->next;
		free(block);
	}

	/* 释放顶层的元组指针数组      */
	free(res->tuples);

	/* 将指针字段清零以捕获编程错误              */
	res->attDescs = NULL;
	res->tuples = NULL;
	res->paramDescs = NULL;
	res->errFields = NULL;
	res->events = NULL;
	res->nEvents = 0;
	/* res->curBlock 已在前面清零     */

	/* 释放 PGresult 结构本身       */
	free(res);
}

/*
 * 用于释放任何部分构建的异步结果的便捷子程序。    
 *
 * "next" 结果也会被清除。    
 */
void
pqClearAsyncResult(PGconn *conn)
{
	PQclear(conn->result);
	conn->result = NULL;
	conn->error_result = false;
	PQclear(conn->next_result);
	conn->next_result = NULL;
}

/*
pqSaveErrorResult -
 *   记住我们有一个错误条件
 *
 * 在 libpq 的大部分地方，报告错误只需将文本追加到
 * conn->errorMessage 并向调用者返回失败代码。
 * 在返回失败代码不切实际的地方，改为调用此函数
 * 来记住需要报告错误。
 *
 * （看起来将文本追加到 conn->errorMessage 可能就足够了，
 * 但我们不能依赖这在内存不足条件下工作。OOM 危险也是
 *
 *
 * 我们不在这里创建新 PGresult 的原因。）                                                                  */
void
pqSaveErrorResult(PGconn *conn)
{
	/* 丢弃挂起的结果...    */
	pqClearAsyncResult(conn);
	/* ... 并设置标志以记住稍后创建错误结果       */
	conn->error_result = true;
}

/*
 * pqSaveWriteError -
 *	  报告写入失败。 
 *
 * 同上，但在把 conn->write_err_msg 追加到我们已有的         
 * 其他错误之后调用。这用于我们已检测到写入失败且     
 * 已耗尽报告其他内容的机会的情形。
 */
static void
pqSaveWriteError(PGconn *conn)
{
	/*
	如果 write_err_msg 因为之前的 strdup 失败而为 null，
	 * 则尽力而为。（我们的这些操作很可能也会遇到 OOM 失败，
	 *
	 * 但不妨一试。）     */
	if (conn->write_err_msg)
	{
		appendPQExpBufferStr(&conn->errorMessage, conn->write_err_msg);
		/* 避免可能将同一条消息追加两次      */
		conn->write_err_msg[0] = '\0';
	}
	else
		libpq_append_conn_error(conn, "write to server failed");

	pqSaveErrorResult(conn);
}

/*
pqPrepareAsyncResult -
 *   准备当前的异步结果对象以返回给调用者
 *
 * 如果还没有异步结果对象，则使用 conn->errorMessage 中的内容
 * 构建一个错误对象。在任何情况下，清除异步结果存储，
 * 并更新我们对已返回给应用程序的错误文本量的记录。
 *
 *
 *
 * 注意：在任何情况下（即使是 OOM）我们都不返回 NULL。       */
PGresult *
pqPrepareAsyncResult(PGconn *conn)
{
	PGresult   *res;

	res = conn->result;
	if (res)
	{
		/*
		如果已存在的结果是 ERROR（可能是从服务器接收到的），
		 * 则假定它表示 conn->errorMessage 中的内容，
		 *
		 * 并推进 errorReported。                     */
		if (res->resultStatus == PGRES_FATAL_ERROR)
			conn->errorReported = conn->errorMessage.len;
	}
	else
	{
		/*
		 * libpq 内部错误后会到达这里。通常应总是设置   
		 *  error_result = true，若非如此则编造错误文本。        
		 *      
		 */
		if (!conn->error_result)
			libpq_append_conn_error(conn, "no error text available");

		/* 偏执：确保 errorReported 偏移合理     */
		if (conn->errorReported < 0 ||
			conn->errorReported >= conn->errorMessage.len)
			conn->errorReported = 0;

		/*
		 * 为该错误创建 PGresult 结构。我们暂时谎报结果      
		 * 状态，使 PQmakeEmptyPGresult 不会无谓地复制整        
		 * 个 conn->errorMessage。 
		 */
		res = PQmakeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
		if (res)
		{
			/*
		报告我们拥有的任何新错误文本，并推进
 *
			 * errorReported。 */
			res->resultStatus = PGRES_FATAL_ERROR;
			pqSetResultError(res, &conn->errorMessage, conn->errorReported);
			conn->errorReported = conn->errorMessage.len;
		}
		else
		{
			/*
			 * 糟糕，PGresult 内存不足。所幸我们手中还有      
			 * 一张王牌：可以使用静态的 OOM_result。在这里   
			 * 去掉 const 有点丑，但把 OOM_result 声明为       
			 * const，希望它会被分配到只读存储中。             
			 *         
			 */
			res = unconstify(PGresult *, &OOM_result);

			/*
		不推进 errorReported。也许我们稍后能够
 *
			 * 报告该文本。              */
		}
	}

	/*
	用 next_result（如果有的话）替换 conn->result。
	 * 正常情况下没有下一个结果，我们只是放弃当前结果的
	 * 所有权。在单行模式下，这将恢复状态到创建当前
	 *
	 * 单行结果之前的状态。                                  */
	conn->result = conn->next_result;
	conn->error_result = false; /* next_result 永不为错误   */
	conn->next_result = NULL;

	return res;
}

/*
pqInternalNotice - 生成内部生成的通知消息
 *
 * 可以传入格式字符串和可选参数。注意我们在这里进行
 * libpq_gettext()，所以调用者不需要。
 *
 * 提供的文本作为主消息（即不应包含尾随换行符，
 *
 * 且不应超过一行）。                                                */
void
pqInternalNotice(const PGNoticeHooks *hooks, const char *fmt,...)
{
	char		msgBuf[1024];
	va_list		args;
	PGresult   *res;

	if (hooks->noticeRec == NULL)
		return;					/* 没有人接收通知？       */

	/* 格式化消息    */
	va_start(args, fmt);
	vsnprintf(msgBuf, sizeof(msgBuf), libpq_gettext(fmt), args);
	va_end(args);
	msgBuf[sizeof(msgBuf) - 1] = '\0';	/* 确保它确实被终止       */

	/* 创建一个 PGresult 传递给通知接收器 */
	res = PQmakeEmptyPGresult(NULL, PGRES_NONFATAL_ERROR);
	if (!res)
		return;
	res->noticeHooks = *hooks;

	/*
	设置通知的字段。
 *   */
	pqSaveMessageField(res, PG_DIAG_MESSAGE_PRIMARY, msgBuf);
	pqSaveMessageField(res, PG_DIAG_SEVERITY, libpq_gettext("NOTICE"));
	pqSaveMessageField(res, PG_DIAG_SEVERITY_NONLOCALIZED, "NOTICE");
	/* XXX 也应提供 SQLSTATE？       */

	/*
	 * 结果文本始终只是主消息 + 换行符。如果无法分配， 
	 * 则用 "out of memory" 代替，同 pqSetResultError 一样。  
	 */
	res->errMsg = (char *) pqResultAlloc(res, strlen(msgBuf) + 2, false);
	if (res->errMsg)
		sprintf(res->errMsg, "%s\n", msgBuf);
	else
		res->errMsg = libpq_gettext("out of memory\n");

	/*
	传给接收器后释放。
 *       */
	res->noticeHooks.noticeRec(res->noticeHooks.noticeRecArg, res);
	PQclear(res);
}

/*
pqAddTuple
 *   向 PGresult 结构添加行指针，必要时扩大它
 *   如果成功返回 true，如果错误阻止添加行则返回 false
 *
 * 出错时，*errmsgp 可设置为要返回的错误字符串。
 *
 * 如果保持为 NULL，则假定错误为"内存不足"。                */
static bool
pqAddTuple(PGresult *res, PGresAttValue *tup, const char **errmsgp)
{
	if (res->ntups >= res->tupArrSize)
	{
		/*
		尝试扩大数组。
		 *
		 * 我们可以使用 realloc，因为结构的浅拷贝是可以的。
		 * 注意：第一次时 res->tuples 为 NULL。虽然 ANSI 规定
		 * 在这种情况下 realloc() 应该像 malloc() 一样工作，
		 * 但一些旧的 C 库（如 SunOS 4.1.x）会核心转储。
		 * 失败时 realloc 应该返回 NULL 而不破坏
		 * 现有分配。注意：res->ntups 之后的位置是垃圾数据，
		 *
		 * 不一定是 NULL。                       */
		int			newSize;
		PGresAttValue **newTuples;

		/*
		 * 由于我们使用整数表示行号，因此无法支持超过  
		 * INT_MAX 行。但要确保允许这么多行。     
		 */
		if (res->tupArrSize <= INT_MAX / 2)
			newSize = (res->tupArrSize > 0) ? res->tupArrSize * 2 : 128;
		else if (res->tupArrSize < INT_MAX)
			newSize = INT_MAX;
		else
		{
			*errmsgp = libpq_gettext("PGresult cannot support more than INT_MAX tuples");
			return false;
		}

		/*
		此外在 32 位平台上，理论上 newSize
		 * 达到 INT_MAX 前就可溢出 size_t。
		 *
		 * （实际在那之前无疑已 OOM，但检查一下。）                             */
#if INT_MAX >= (SIZE_MAX / 2)
		if (newSize > SIZE_MAX / sizeof(PGresAttValue *))
		{
			*errmsgp = libpq_gettext("size_t overflow");
			return false;
		}
#endif

		if (res->tuples == NULL)
			newTuples = (PGresAttValue **)
				malloc(newSize * sizeof(PGresAttValue *));
		else
			newTuples = (PGresAttValue **)
				realloc(res->tuples, newSize * sizeof(PGresAttValue *));
		if (!newTuples)
			return false;		/* malloc/realloc 失败    */
		res->memorySize +=
			(newSize - res->tupArrSize) * sizeof(PGresAttValue *);
		res->tupArrSize = newSize;
		res->tuples = newTuples;
	}
	res->tuples[res->ntups] = tup;
	res->ntups++;
	return true;
}

/*
pqSaveMessageField - 保存错误或通知消息的一个字段
 *    */
void
pqSaveMessageField(PGresult *res, char code, const char *value)
{
	PGMessageField *pfield;

	pfield = (PGMessageField *)
		pqResultAlloc(res,
					  offsetof(PGMessageField, contents) +
					  strlen(value) + 1,
					  true);
	if (!pfield)
		return;					/*内存不足？ */
	pfield->code = code;
	strcpy(pfield->contents, value);
	pfield->next = res->errFields;
	res->errFields = pfield;
}

/*
pqSaveParameterStatus - 记住后端发送的参数状态
 *          */
void
pqSaveParameterStatus(PGconn *conn, const char *name, const char *value)
{
	pgParameterStatus *pstatus;
	pgParameterStatus *prev;

	/*
	忘记关于该参数的任何旧信息
 *          */
	for (pstatus = conn->pstatus, prev = NULL;
		 pstatus != NULL;
		 prev = pstatus, pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, name) == 0)
		{
			if (prev)
				prev->next = pstatus->next;
			else
				conn->pstatus = pstatus->next;
			free(pstatus);		/* 同时释放名值字符串      */
			break;
		}
	}

	/*
	将新信息存储为单个 malloc 块
 *    */
	pstatus = (pgParameterStatus *) malloc(sizeof(pgParameterStatus) +
										   strlen(name) + strlen(value) + 2);
	if (pstatus)
	{
		char	   *ptr;

		ptr = ((char *) pstatus) + sizeof(pgParameterStatus);
		pstatus->name = ptr;
		strcpy(ptr, name);
		ptr += strlen(name) + 1;
		pstatus->value = ptr;
		strcpy(ptr, value);
		pstatus->next = conn->pstatus;
		conn->pstatus = pstatus;
	}

	/*
	 * 把 libpq 感兴趣的设置值保存到 PGconn 对象的字段中。 
	 * 我们还将 client_encoding 和 standard_conforming_strings           
	 * 也保存在静态变量中，使 PQescapeString 与 PQescapeBytea   
	 * 表现得比较合理（至少在单连接程序中）。      
	 *           
	 */
	if (strcmp(name, "client_encoding") == 0)
	{
		conn->client_encoding = pg_char_to_encoding(value);
		/* 如果不识别编码名称，回退到 SQL_ASCII               */
		if (conn->client_encoding < 0)
			conn->client_encoding = PG_SQL_ASCII;
		static_client_encoding = conn->client_encoding;
	}
	else if (strcmp(name, "standard_conforming_strings") == 0)
	{
		conn->std_strings = (strcmp(value, "on") == 0);
		static_std_strings = conn->std_strings;
	}
	else if (strcmp(name, "server_version") == 0)
	{
		/* 将服务器版本转数字形式。           */
		int			cnt;
		int			vmaj,
					vmin,
					vrev;

		cnt = sscanf(value, "%d.%d.%d", &vmaj, &vmin, &vrev);

		if (cnt == 3)
		{
			/* 旧式，如 9.6.1    */
			conn->sversion = (100 * vmaj + vmin) * 100 + vrev;
		}
		else if (cnt == 2)
		{
			if (vmaj >= 10)
			{
				/* 新式，如 10.1    */
				conn->sversion = 100 * 100 * vmaj + vmin;
			}
			else
			{
				/* 旧式无次版本，如 9.6devel              */
				conn->sversion = (100 * vmaj + vmin) * 100;
			}
		}
		else if (cnt == 1)
		{
			/* 新式无次版本，如 10devel              */
			conn->sversion = 100 * 100 * vmaj;
		}
		else
			conn->sversion = 0; /* 未知  */
	}
	else if (strcmp(name, "default_transaction_read_only") == 0)
	{
		conn->default_transaction_read_only =
			(strcmp(value, "on") == 0) ? PG_BOOL_YES : PG_BOOL_NO;
	}
	else if (strcmp(name, "in_hot_standby") == 0)
	{
		conn->in_hot_standby =
			(strcmp(value, "on") == 0) ? PG_BOOL_YES : PG_BOOL_NO;
	}
	else if (strcmp(name, "scram_iterations") == 0)
	{
		conn->scram_sha_256_iterations = atoi(value);
	}
}


/*
pqRowProcessor
 *   将接收到的行添加到当前异步结果 (conn->result)。
 *   成功返回 1，出错返回 0。
 *
 * 出错时，*errmsgp 可设置为要返回的错误字符串。
 * （此字符串应已通过 libpq_gettext() 翻译。）
 * 如果保持为 NULL，则假定错误为"内存不足"。
 *
 * 在单行模式下，我们创建仅包含当前行的新结果，
 * 将之前的结果保存到 conn->next_result 中，以便在
 * pqPrepareAsyncResult() 之后它再次变为活动状态。这允许结果
 *
 * 元数据（列描述）传递到每个结果行。                                                */
int
pqRowProcessor(PGconn *conn, const char **errmsgp)
{
	PGresult   *res = conn->result;
	int			nfields = res->numAttributes;
	const PGdataValue *columns = conn->rowBuf;
	PGresAttValue *tup;
	int			i;

	/*
	在单行模式下，创建一个新的 PGresult，仅包含这一行；
	 * 原始的 conn->result 保持不变，以便可以作为未来行的
	 *
	 * 模板再次使用。                  */
	if (conn->singleRowMode)
	{
		/* 复制此时应该在结果中的所有内容              */
		res = PQcopyResult(res,
						   PG_COPYRES_ATTRS | PG_COPYRES_EVENTS |
						   PG_COPYRES_NOTICEHOOKS);
		if (!res)
			return 0;
	}

	/*
	基本上，我们只是在 PGresult 中为每个字段分配空间并
	 * 复制数据。
	 *
	 * 注意：malloc 失败时，我们返回 0，保持 *errmsgp 仍为 NULL，
	 * 调用者将理解为"内存不足"。这比尝试在此处设置此类消息更好，
	 *
	 *
	 * 因为显然没有足够的内存让 gettext() 做任何事情。                       */
	tup = (PGresAttValue *)
		pqResultAlloc(res, nfields * sizeof(PGresAttValue), true);
	if (tup == NULL)
		goto fail;

	for (i = 0; i < nfields; i++)
	{
		int			clen = columns[i].len;

		if (clen < 0)
		{
			/* null 列   */
			tup[i].len = NULL_LEN;
			tup[i].value = res->null_field;
		}
		else
		{
			bool		isbinary = (res->attDescs[i].format != 0);
			char	   *val;

			val = (char *) pqResultAlloc(res, (size_t) clen + 1, isbinary);
			if (val == NULL)
				goto fail;

			/* 复制数据并以零终止（含二进制）          */
			memcpy(val, columns[i].value, clen);
			val[clen] = '\0';

			tup[i].len = clen;
			tup[i].value = val;
		}
	}

	/* 添加元组到结果数组                     */
	if (!pqAddTuple(res, tup, errmsgp))
		goto fail;

	/*
	成功。在单行模式下，立即使结果对客户端可用。
 *
 *                    */
	if (conn->singleRowMode)
	{
		/* 将结果状态更改为特殊的单行值       */
		res->resultStatus = PGRES_SINGLE_TUPLE;
		/* 保存旧结果供以后重用    */
		conn->next_result = conn->result;
		conn->result = res;
		/* 并标记结果准备返回         */
		conn->asyncStatus = PGASYNC_READY_MORE;
	}

	return 1;

fail:
	/* 释放本地 PGresult（若有）                  */
	if (res != conn->result)
		PQclear(res);
	return 0;
}


/*
pqAllocCmdQueueEntry
 *   获取命令队列条目供调用者填充。
 *
 * 如果回收队列中有空闲元素，则返回它；如果没有，
 * 则分配一个新的。调用者负责在结构体填充完成后
 * 将其添加到命令队列 (pqAppendCmdQueueEntry)，
 * 或者在发生错误时释放内存 (pqRecycleCmdQueueEntry)。
 *
 *
 * 如果分配失败，设置错误消息并返回 NULL。                       */
static PGcmdQueueEntry *
pqAllocCmdQueueEntry(PGconn *conn)
{
	PGcmdQueueEntry *entry;

	if (conn->cmd_queue_recycle == NULL)
	{
		entry = (PGcmdQueueEntry *) malloc(sizeof(PGcmdQueueEntry));
		if (entry == NULL)
		{
			libpq_append_conn_error(conn, "out of memory");
			return NULL;
		}
	}
	else
	{
		entry = conn->cmd_queue_recycle;
		conn->cmd_queue_recycle = entry->next;
	}
	entry->next = NULL;
	entry->query = NULL;

	return entry;
}

/*
 * pqAppendCmdQueueEntry
 *		把调用者分配的条目追加到命令队列，并更新    
 *		conn->asyncStatus 以反映之。   
 *
 * 查询本身必须已由调用者放入输出缓冲区。              
 *        
 */
static void
pqAppendCmdQueueEntry(PGconn *conn, PGcmdQueueEntry *entry)
{
	Assert(entry->next == NULL);

	if (conn->cmd_queue_head == NULL)
		conn->cmd_queue_head = entry;
	else
		conn->cmd_queue_tail->next = entry;

	conn->cmd_queue_tail = entry;

	switch (conn->pipelineStatus)
	{
		case PQ_PIPELINE_OFF:
		case PQ_PIPELINE_ON:

			/*
			 * 非管线中止态时，若有就绪待消费结果，就    
			 * 让它保持原状（即不离开 READY 或 READY_MORE）； 
			 * 否则将我们置为忙碌，等待服务器          
			 * 的数据到来。                  
			 */
			if (conn->asyncStatus == PGASYNC_IDLE)
				conn->asyncStatus = PGASYNC_BUSY;
			break;

		case PQ_PIPELINE_ABORTED:

			/*
			 * 管线中止状态下，我们不指望从服务器得      
			 * 到任何内容（因为不发送排队查询）。      
			 * 因此若处于 IDLE，就像 PQgetResult 那样让        
			 * 自己消费队列中的命令；若处于其他状态，    
			 * 我们无需做任何事。         
			 */
			if (conn->asyncStatus == PGASYNC_IDLE ||
				conn->asyncStatus == PGASYNC_PIPELINE_IDLE)
				pqPipelineProcessQueue(conn);
			break;
	}
}

/*
pqRecycleCmdQueueEntry
 *
 *   将命令队列条目推入空闲列表。    */
static void
pqRecycleCmdQueueEntry(PGconn *conn, PGcmdQueueEntry *entry)
{
	if (entry == NULL)
		return;

	/* 可回收的条目不应该有后续命令             */
	Assert(entry->next == NULL);

	if (entry->query)
	{
		free(entry->query);
		entry->query = NULL;
	}

	entry->next = conn->cmd_queue_recycle;
	conn->cmd_queue_recycle = entry;
}


/*
PQsendQuery
 *   提交查询，但不等待它完成
 *
 * 返回：1 如果成功提交
 *       0 如果出错（conn->errorMessage 已设置）
 *
 * PQsendQueryContinue 是非导出版本，行为完全相同，
 *
 * 只是不会重置 conn->errorMessage。                     */
int
PQsendQuery(PGconn *conn, const char *query)
{
	return PQsendQueryInternal(conn, query, true);
}

int
PQsendQueryContinue(PGconn *conn, const char *query)
{
	return PQsendQueryInternal(conn, query, false);
}

static int
PQsendQueryInternal(PGconn *conn, const char *query, bool newQuery)
{
	PGcmdQueueEntry *entry = NULL;

	if (!PQsendQueryStart(conn, newQuery))
		return 0;

	/* 检查参数       */
	if (!query)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "%s not allowed in pipeline mode",
								"PQsendQuery");
		return 0;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* 错误消息已设置 */

	/* 发送查询消息        */
	/* 构造外发的 Query 消息         */
	if (pqPutMsgStart('Q', conn) < 0 ||
		pqPuts(query, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		/* 错误消息应该已经设置好      */
		pqRecycleCmdQueueEntry(conn, entry);
		return 0;
	}

	/* 记住我们正在使用简单查询协议  */
	entry->queryclass = PGQUERY_SIMPLE;
	/* 记住查询文本（如可能）            */
	entry->query = strdup(query);

	/*
	 * 推一把数据。非阻塞模式下，如果无法全部发送不要  
	 * 抱怨；PQgetResult() 会完成所需的额外刷新。             
	 */
	if (pqFlush(conn) < 0)
		goto sendFailed;

	/* 已启动！       */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* 错误消息应该已经设置好      */
	return 0;
}

/*
 * PQsendQueryParams
 *		类似 PQsendQuery，但使用扩展查询协议以便传递参数         
 */
int
PQsendQueryParams(PGconn *conn,
				  const char *command,
				  int nParams,
				  const Oid *paramTypes,
				  const char *const *paramValues,
				  const int *paramLengths,
				  const int *paramFormats,
				  int resultFormat)
{
	if (!PQsendQueryStart(conn, true))
		return 0;

	/* 检查参数        */
	if (!command)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	return PQsendQueryGuts(conn,
						   command,
						   "",	/* 使用未命名语句 */
						   nParams,
						   paramTypes,
						   paramValues,
						   paramLengths,
						   paramFormats,
						   resultFormat);
}

/*
PQsendPrepare
 *   提交 Parse 消息，但不等待它完成
 *
 * 返回：1 如果成功提交
 *
 *       0 如果出错（conn->errorMessage 已设置）      */
int
PQsendPrepare(PGconn *conn,
			  const char *stmtName, const char *query,
			  int nParams, const Oid *paramTypes)
{
	PGcmdQueueEntry *entry = NULL;

	if (!PQsendQueryStart(conn, true))
		return 0;

	/* 检查参数        */
	if (!stmtName)
	{
		libpq_append_conn_error(conn, "statement name is a null pointer");
		return 0;
	}
	if (!query)
	{
		libpq_append_conn_error(conn, "command string is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* 错误消息已设置 */

	/* 构造 Parse 消息         */
	if (pqPutMsgStart('P', conn) < 0 ||
		pqPuts(stmtName, conn) < 0 ||
		pqPuts(query, conn) < 0)
		goto sendFailed;

	if (nParams > 0 && paramTypes)
	{
		int			i;

		if (pqPutInt(nParams, 2, conn) < 0)
			goto sendFailed;
		for (i = 0; i < nParams; i++)
		{
			if (pqPutInt(paramTypes[i], 4, conn) < 0)
				goto sendFailed;
		}
	}
	else
	{
		if (pqPutInt(0, 2, conn) < 0)
			goto sendFailed;
	}
	if (pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* 添加 Sync，除非管线模式。  */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart('S', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* 记住我们只执行一个 Parse  */
	entry->queryclass = PGQUERY_PREPARE;

	/* 记住查询文本（如可能）            */
	/* 如果内存不足，query 就只是 NULL        */
	entry->query = strdup(query);

	/*
	给数据一个推送（在管线模式下，仅当我们超过大小
	 * 阈值时）。在非阻塞模式下，如果无法全部发送，
	 *
	 * 不要抱怨；PQgetResult() 会进行所需的额外刷新。  */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* 已启动！       */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* 错误消息应该已经设置好      */
	return 0;
}

/*
PQsendQueryPrepared
 *   类似 PQsendQuery，但执行先前准备的语句，
 *
 *   使用扩展查询协议以便传递参数                     */
int
PQsendQueryPrepared(PGconn *conn,
					const char *stmtName,
					int nParams,
					const char *const *paramValues,
					const int *paramLengths,
					const int *paramFormats,
					int resultFormat)
{
	if (!PQsendQueryStart(conn, true))
		return 0;

	/* 检查参数        */
	if (!stmtName)
	{
		libpq_append_conn_error(conn, "statement name is a null pointer");
		return 0;
	}
	if (nParams < 0 || nParams > PQ_QUERY_PARAM_MAX_LIMIT)
	{
		libpq_append_conn_error(conn, "number of parameters must be between 0 and %d",
								PQ_QUERY_PARAM_MAX_LIMIT);
		return 0;
	}

	return PQsendQueryGuts(conn,
						   NULL,	/* 无命令需解析  */
						   stmtName,
						   nParams,
						   NULL,	/*无参数类型 */
						   paramValues,
						   paramLengths,
						   paramFormats,
						   resultFormat);
}

/*
PQsendQueryStart
 *
 *   PQsendQuery 及同类例程的公共启动代码        */
static bool
PQsendQueryStart(PGconn *conn, bool newQuery)
{
	if (!conn)
		return false;

	/*
	如果这是查询周期的开始，重置错误状态。
	 * 但是，在管线模式下且已有内容排队时，错误缓冲区
	 *
	 * 属于该命令，我们不应该清除它。                   */
	if (newQuery && conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/* 无活动连接则不发送。                           */
	if (conn->status != CONNECTION_OK)
	{
		libpq_append_conn_error(conn, "no connection to the server");
		return false;
	}

	/* 在忙碌时也不能发送，除非正在为稍后排队         */
	if (conn->asyncStatus != PGASYNC_IDLE &&
		conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "another command is already in progress");
		return false;
	}

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		/*
		排队命令时，我们不会太多地改变连接状态，
		 * 因为它已被当前命令使用。连接状态将在
		 * pqPipelineProcessQueue() 推进到开始处理排队的
		 * 消息时更新。
		 *
		 * 只需确保在给定当前连接状态的情况下我们可以安全地
		 * 排队。我们可以在另一个队列项后面排队，
		 * 或在非队列命令（发送自己 sync 的命令）后面排队，
		 *
		 * 但如果连接处于复制状态则不能排队。                              */
		switch (conn->asyncStatus)
		{
			case PGASYNC_IDLE:
			case PGASYNC_PIPELINE_IDLE:
			case PGASYNC_READY:
			case PGASYNC_READY_MORE:
			case PGASYNC_BUSY:
				/* 可排队   */
				break;

			case PGASYNC_COPY_IN:
			case PGASYNC_COPY_OUT:
			case PGASYNC_COPY_BOTH:
				libpq_append_conn_error(conn, "cannot queue commands during COPY");
				return false;
		}
	}
	else
	{
		/*
		 * 此命令的结果将立即到达。初始化异步              
		 * 结果累积状态       
		 */
		pqClearAsyncResult(conn);

		/* 重置单行处理模式         */
		conn->singleRowMode = false;
	}

	/* 准备发送命令消息      */
	return true;
}

/*
PQsendQueryGuts
 *   使用扩展查询协议发送查询的公共代码
 *   PQsendQueryStart 应该已经完成
 *
 *
 * command 可以为 NULL，表示我们使用已准备好的语句                 */
static int
PQsendQueryGuts(PGconn *conn,
				const char *command,
				const char *stmtName,
				int nParams,
				const Oid *paramTypes,
				const char *const *paramValues,
				const int *paramLengths,
				const int *paramFormats,
				int resultFormat)
{
	int			i;
	PGcmdQueueEntry *entry;

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* 错误消息已设置 */

	/*
	 * 我们将发送 Parse（如需）、Bind、Describe Portal、       
	 * Execute、Sync（非管线），用指定语句名与未命      
	 * 名门户。   
	 */

	if (command)
	{
		/* 构造 Parse 消息         */
		if (pqPutMsgStart('P', conn) < 0 ||
			pqPuts(stmtName, conn) < 0 ||
			pqPuts(command, conn) < 0)
			goto sendFailed;
		if (nParams > 0 && paramTypes)
		{
			if (pqPutInt(nParams, 2, conn) < 0)
				goto sendFailed;
			for (i = 0; i < nParams; i++)
			{
				if (pqPutInt(paramTypes[i], 4, conn) < 0)
					goto sendFailed;
			}
		}
		else
		{
			if (pqPutInt(0, 2, conn) < 0)
				goto sendFailed;
		}
		if (pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* 构造 Bind 消息         */
	if (pqPutMsgStart('B', conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPuts(stmtName, conn) < 0)
		goto sendFailed;

	/* 发送参数格式     */
	if (nParams > 0 && paramFormats)
	{
		if (pqPutInt(nParams, 2, conn) < 0)
			goto sendFailed;
		for (i = 0; i < nParams; i++)
		{
			if (pqPutInt(paramFormats[i], 2, conn) < 0)
				goto sendFailed;
		}
	}
	else
	{
		if (pqPutInt(0, 2, conn) < 0)
			goto sendFailed;
	}

	if (pqPutInt(nParams, 2, conn) < 0)
		goto sendFailed;

	/* 发送参数    */
	for (i = 0; i < nParams; i++)
	{
		if (paramValues && paramValues[i])
		{
			int			nbytes;

			if (paramFormats && paramFormats[i] != 0)
			{
				/* 二进制参数  */
				if (paramLengths)
					nbytes = paramLengths[i];
				else
				{
					libpq_append_conn_error(conn, "length must be given for binary parameter");
					goto sendFailed;
				}
			}
			else
			{
				/* 文本参数，不使用 paramLengths   */
				nbytes = strlen(paramValues[i]);
			}
			if (pqPutInt(nbytes, 4, conn) < 0 ||
				pqPutnchar(paramValues[i], nbytes, conn) < 0)
				goto sendFailed;
		}
		else
		{
			/* 将参数视为 NULL   */
			if (pqPutInt(-1, 4, conn) < 0)
				goto sendFailed;
		}
	}
	if (pqPutInt(1, 2, conn) < 0 ||
		pqPutInt(resultFormat, 2, conn))
		goto sendFailed;
	if (pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* 构造 Describe Portal 消息         */
	if (pqPutMsgStart('D', conn) < 0 ||
		pqPutc('P', conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* 构造 Execute 消息         */
	if (pqPutMsgStart('E', conn) < 0 ||
		pqPuts("", conn) < 0 ||
		pqPutInt(0, 4, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* 如果不在管线模式，构造 Sync 消息      */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart('S', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* 记住我们正在使用扩展查询协议    */
	entry->queryclass = PGQUERY_EXTENDED;

	/* 记住查询文本（如可能）            */
	/* 如果内存不足，query 就只是 NULL        */
	if (command)
		entry->query = strdup(command);

	/*
	给数据一个推送（在管线模式下，仅当我们超过大小
	 * 阈值时）。在非阻塞模式下，如果无法全部发送，
	 *
	 * 不要抱怨；PQgetResult() 会进行所需的额外刷新。  */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* 已启动！       */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* 错误消息应该已经设置好      */
	return 0;
}

/*
选择逐行处理模式
 *           */
int
PQsetSingleRowMode(PGconn *conn)
{
	/*
	只允许在已启动查询且尚未收到任何结果时
	 *
	 * 设置该标志。                 */
	if (!conn)
		return 0;
	if (conn->asyncStatus != PGASYNC_BUSY)
		return 0;
	if (!conn->cmd_queue_head ||
		(conn->cmd_queue_head->queryclass != PGQUERY_SIMPLE &&
		 conn->cmd_queue_head->queryclass != PGQUERY_EXTENDED))
		return 0;
	if (pgHavePendingResult(conn))
		return 0;

	/* 设置标志 */
	conn->singleRowMode = true;
	return 1;
}

/*
消费来自后端的任何可用输入
 * 0 返回：某种问题
 *
 * 1 返回：没有问题           */
int
PQconsumeInput(PGconn *conn)
{
	if (!conn)
		return 0;

	/*
	对于非阻塞连接，尝试刷新发送队列，
	 * 否则我们可能永远无法获得响应，因为某些内容可能尚未
	 *
	 * 实际发送，仍在我们的写缓冲区中！     */
	if (pqIsnonblocking(conn))
	{
		if (pqFlush(conn) < 0)
			return 0;
	}

	/*
	 * 如果还有数据则加载更多数据。无论处于何种状态  
	 * 我们都这样做，因为调用我们很可能是因为应用程    
	 * 序想要摆脱读选择条件。注意我们绝不会阻塞      
	 * 等待更多输入。  
	 */
	if (pqReadData(conn) < 0)
		return 0;

	/* 数据的解析稍后进行。        */
	return 1;
}


/*
parseInput：如果合适，解析来自后端的输入数据，
 * 直到输入耗尽或达到停止状态。
 *
 * 注意：此函数不会尝试从后端读取更多数据。                         */
static void
parseInput(PGconn *conn)
{
	pqParseInput3(conn);
}

/*
 * PQisBusy
 *	 若 PQgetResult 会阻塞等待输入则返回 true。   
 */

int
PQisBusy(PGconn *conn)
{
	if (!conn)
		return false;

	/* 状态允许时解析可用数据。            */
	parseInput(conn);

	/*
	 * 除 BUSY 外的所有状态下 PQgetResult 都会立即返回。     
	 * 此外，如果我们已检测到读 EOF 并丢弃了连接，      
	 * 可以预期 PQgetResult 会立即失败。注意我们在这       
	 * 里*不*检查 conn->write_failed —— 一旦它被设置，     
	 * 我们就知道遇上了麻烦，但仍需继续尝试读取，     
	 * 直到取得完整消息或 EOF。
	 */
	return conn->asyncStatus == PGASYNC_BUSY && conn->status != CONNECTION_BAD;
}

/*
PQgetResult
 *   获取查询生成的下一个 PGresult。如果没有剩余的查询工作
 *   或发生错误（例如内存不足），则返回 NULL。
 *
 *   在管线模式下，一旦某个查询的所有结果都已返回，
 *   PQgetResult 返回 NULL，让用户知道下一个查询正在处理中。
 *   在管线结束时，返回一个结果，
 *
 *
 *   PQresultStatus(result) == PGRES_PIPELINE_SYNC。             */
PGresult *
PQgetResult(PGconn *conn)
{
	PGresult   *res;

	if (!conn)
		return NULL;

	/* 状态允许时解析可用数据。            */
	parseInput(conn);

	/* 未就绪则阻塞等待。                           */
	while (conn->asyncStatus == PGASYNC_BUSY)
	{
		int			flushResult;

		/*
		 * 如果数据仍未发送，就发送它。否则我们可能在等
		 * 待后端甚至尚未收到的命令的结果。    
		 */
		while ((flushResult = pqFlush(conn)) > 0)
		{
			if (pqWait(false, true, conn))
			{
				flushResult = -1;
				break;
			}
		}

		/*
		等待更多数据，并加载它。（注意：如果连接已丢失，
		 * pqWait 应该立即返回，因为套接字应该是可读就绪的，
		 * 或者带有最后的服务器数据，或者带有 EOF 指示。
		 * 因此我们期望这不会导致在报告先前的写入失败时
		 *
		 * 有任何不当延迟。）             */
		if (flushResult ||
			pqWait(true, false, conn) ||
			pqReadData(conn) < 0)
		{
			/* 报告 pqWait 或 pqReadData 保存的错误   */
			pqSaveErrorResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			return pqPrepareAsyncResult(conn);
		}

		/* 解析。 */
		parseInput(conn);

		/*
		若有写错误但上面未获结果或检测读错，报告写错误。
 *
 *                                                  */
		if (conn->write_failed && conn->asyncStatus == PGASYNC_BUSY)
		{
			pqSaveWriteError(conn);
			conn->asyncStatus = PGASYNC_IDLE;
			return pqPrepareAsyncResult(conn);
		}
	}

	/* 返回适当的内容。      */
	switch (conn->asyncStatus)
	{
		case PGASYNC_IDLE:
			res = NULL;			/* 查询已完成   */
			break;
		case PGASYNC_PIPELINE_IDLE:
			Assert(conn->pipelineStatus != PQ_PIPELINE_OFF);

			/*
			 * 我们即将返回终止当前查询结果轮次的 NULL；
			 * 准备下次被调用时发送下一查询的结果(如有)   
			 * 若命令队列中没有下一元素，这会让我们进入  
			 * IDLE 状态。                                                
			 */
			pqPipelineProcessQueue(conn);
			res = NULL;			/* 查询已完成   */
			break;

		case PGASYNC_READY:
			res = pqPrepareAsyncResult(conn);

			/* 适当地推进队列            */
			pqCommandQueueAdvance(conn, false,
								  res->resultStatus == PGRES_PIPELINE_SYNC);

			if (conn->pipelineStatus != PQ_PIPELINE_OFF)
			{
				/*
		我们即将发送当前查询的结果。现在将我们设置为空闲，
 *
				 * 然后...  */
				conn->asyncStatus = PGASYNC_PIPELINE_IDLE;

				/*
				 * ……发送管线同步结果时，立即推进队列 
				 * 处理，让下次调用时备好返回服务器传     
				 * 来的下一个结果。其余情形把队列状态变更  
				 * 到下次处理，这样就会发送出终止性的     
				 * NULL 结果。                                         
				 *                     
				 *
				 * （换言之：管线同步之后我们不返回 NULL   
				 * 。）
				 */
				if (res->resultStatus == PGRES_PIPELINE_SYNC)
					pqPipelineProcessQueue(conn);
			}
			else
			{
				/* 将状态设置回 BUSY，允许解析继续进行。    */
				conn->asyncStatus = PGASYNC_BUSY;
			}
			break;
		case PGASYNC_READY_MORE:
			res = pqPrepareAsyncResult(conn);
			/* 将状态设置回 BUSY，允许解析继续进行。    */
			conn->asyncStatus = PGASYNC_BUSY;
			break;
		case PGASYNC_COPY_IN:
			res = getCopyResult(conn, PGRES_COPY_IN);
			break;
		case PGASYNC_COPY_OUT:
			res = getCopyResult(conn, PGRES_COPY_OUT);
			break;
		case PGASYNC_COPY_BOTH:
			res = getCopyResult(conn, PGRES_COPY_BOTH);
			break;
		default:
			libpq_append_conn_error(conn, "unexpected asyncStatus: %d", (int) conn->asyncStatus);
			pqSaveErrorResult(conn);
			conn->asyncStatus = PGASYNC_IDLE;	/* 尝试恢复有效状态   */
			res = pqPrepareAsyncResult(conn);
			break;
	}

	/* 触发 PGEVT_RESULTCREATE 事件（如有）             */
	if (res && res->nEvents > 0)
		(void) PQfireResultCreateEvents(conn, res);

	return res;
}

/*
 * getCopyResult
 *	  PQgetResult 的辅助函数：为进行中的 COPY 生成结果   
 */
static PGresult *
getCopyResult(PGconn *conn, ExecStatusType copytype)
{
	/*
	如果服务器连接已丢失，不要假装一切正常；
	 * 而是返回 PGRES_FATAL_ERROR 结果，并将 asyncStatus
	 * 重置为空闲（对应我们在 PQgetResult 的早期步骤中检测到
	 * I/O 错误时的做法）。结果中返回的文本是
	 * conn->errorMessage 中的内容；我们希望其中填充了
	 *
	 * 检测到连接丢失时相关的信息。                                                          */
	if (conn->status != CONNECTION_OK)
	{
		pqSaveErrorResult(conn);
		conn->asyncStatus = PGASYNC_IDLE;
		return pqPrepareAsyncResult(conn);
	}

	/* 如果我们有 COPY 的异步结果，返回它     */
	if (conn->result && conn->result->resultStatus == copytype)
		return pqPrepareAsyncResult(conn);

	/* 否则创建合适 PGresult           */
	return PQmakeEmptyPGresult(conn, copytype);
}


/*
PQexec
 *   向后端发送查询并将结果打包在 PGresult 中
 *
 * 如果查询甚至没有发送，返回 NULL；conn->errorMessage 设置为
 * 相关消息。
 * 如果查询已发送，返回一个新的 PGresult（可能指示
 * 成功或失败）。
 *
 *
 * 用户负责在完成后通过 PQclear() 释放 PGresult。                                                 */
PGresult *
PQexec(PGconn *conn, const char *query)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQuery(conn, query))
		return NULL;
	return PQexecFinish(conn);
}

/*
 * PQexecParams
 *		类似 PQexec，但使用扩展查询协议以便传递参数         
 */
PGresult *
PQexecParams(PGconn *conn,
			 const char *command,
			 int nParams,
			 const Oid *paramTypes,
			 const char *const *paramValues,
			 const int *paramLengths,
			 const int *paramFormats,
			 int resultFormat)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQueryParams(conn, command,
						   nParams, paramTypes, paramValues, paramLengths,
						   paramFormats, resultFormat))
		return NULL;
	return PQexecFinish(conn);
}

/*
PQprepare
 *   通过发出 Parse 消息创建预准备语句。
 *
 * 如果查询甚至没有发送，返回 NULL；conn->errorMessage 设置为
 * 相关消息。
 * 如果查询已发送，返回一个新的 PGresult（可能指示
 * 成功或失败）。
 *
 *
 * 用户负责在完成后通过 PQclear() 释放 PGresult。                                            */
PGresult *
PQprepare(PGconn *conn,
		  const char *stmtName, const char *query,
		  int nParams, const Oid *paramTypes)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendPrepare(conn, stmtName, query, nParams, paramTypes))
		return NULL;
	return PQexecFinish(conn);
}

/*
PQexecPrepared
 *   类似 PQexec，但执行先前准备的语句，
 *
 *   使用扩展查询协议以便传递参数                     */
PGresult *
PQexecPrepared(PGconn *conn,
			   const char *stmtName,
			   int nParams,
			   const char *const *paramValues,
			   const int *paramLengths,
			   const int *paramFormats,
			   int resultFormat)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendQueryPrepared(conn, stmtName,
							 nParams, paramValues, paramLengths,
							 paramFormats, resultFormat))
		return NULL;
	return PQexecFinish(conn);
}

/*
PQexec 及同类例程的公共代码：准备发送命令
 *            */
static bool
PQexecStart(PGconn *conn)
{
	PGresult   *result;

	if (!conn)
		return false;

	/*
	由于这是查询周期的开始，重置错误状态。
	 * 但是，在管线模式下且已有内容排队时，错误缓冲区
	 *
	 * 属于该命令，我们不应该清除它。                      */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "synchronous command execution functions are not allowed in pipeline mode");
		return false;
	}

	/*
	 * 静默丢弃应用程序未消费的任何先前查询结果。     
	 * 这可能是糟糕的设计，但它的存在是为了向后兼容。  
	 */
	while ((result = PQgetResult(conn)) != NULL)
	{
		ExecStatusType resultStatus = result->resultStatus;

		PQclear(result);		/* 只需要其状态   */
		if (resultStatus == PGRES_COPY_IN)
		{
			/* 退出 COPY IN 状态      */
			if (PQputCopyEnd(conn,
							 libpq_gettext("COPY terminated by new PQexec")) < 0)
				return false;
			/* 继续等待以接收 COPY 的失败消息         */
		}
		else if (resultStatus == PGRES_COPY_OUT)
		{
			/*
		退出 COPY OUT 状态：我们只需切换回 BUSY，
 *
			 * 并允许剩余的 COPY 数据被丢弃。                           */
			conn->asyncStatus = PGASYNC_BUSY;
			/* 继续等待以接收 COPY 的完成消息            */
		}
		else if (resultStatus == PGRES_COPY_BOTH)
		{
			/* COPY BOTH 期间禁用 PQexec          */
			libpq_append_conn_error(conn, "PQexec not allowed during COPY BOTH");
			return false;
		}
		/* 同时检查连接丢失          */
		if (conn->status == CONNECTION_BAD)
			return false;
	}

	/* 可以发送命令   */
	return true;
}

/*
PQexec 及同类例程的公共代码：等待命令结果
 *            */
static PGresult *
PQexecFinish(PGconn *conn)
{
	PGresult   *result;
	PGresult   *lastResult;

	/*
	为了向后兼容，如果有多个结果，返回最后一个。
	 * （我们曾经在这里有逻辑来连接连续的错误消息，
	 * 但现在这会自动发生，因为 conn->errorMessage 会在此循环中
	 * 持续累积错误。）
	 *
	 * 但是，如果看到 COPY IN/OUT/BOTH，我们必须停止。
	 * 我们将在应用程序执行数据传输后恢复解析。
	 *
	 *
	 * 如果连接丢失也要停止（否则会无限循环）。                                         */
	lastResult = NULL;
	while ((result = PQgetResult(conn)) != NULL)
	{
		PQclear(lastResult);
		lastResult = result;
		if (result->resultStatus == PGRES_COPY_IN ||
			result->resultStatus == PGRES_COPY_OUT ||
			result->resultStatus == PGRES_COPY_BOTH ||
			conn->status == CONNECTION_BAD)
			break;
	}

	return lastResult;
}

/*
PQdescribePrepared
 *   获取关于先前准备的语句的信息
 *
 * 如果查询甚至没有发送，返回 NULL；conn->errorMessage 设置为
 * 相关消息。
 * 如果查询已发送，返回一个新的 PGresult（可能指示
 * 成功或失败）。成功时，PGresult 包含状态
 * PGRES_COMMAND_OK，其参数和列标题字段分别描述
 * 语句的输入和输出。
 *
 *
 * 用户负责在完成后通过 PQclear() 释放 PGresult。                                                                                             */
PGresult *
PQdescribePrepared(PGconn *conn, const char *stmt)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendDescribe(conn, 'S', stmt))
		return NULL;
	return PQexecFinish(conn);
}

/*
PQdescribePortal
 *   获取关于先前创建的门户的信息
 *
 * 这与 PQdescribePrepared 非常相似，只是不返回参数信息。
 * 注意：目前 libpq 并没有真正向客户端暴露门户；
 *
 *
 * 但这可以与通过 SQL DECLARE CURSOR 命令创建的门户一起使用。                               */
PGresult *
PQdescribePortal(PGconn *conn, const char *portal)
{
	if (!PQexecStart(conn))
		return NULL;
	if (!PQsendDescribe(conn, 'P', portal))
		return NULL;
	return PQexecFinish(conn);
}

/*
PQsendDescribePrepared
 *   提交 Describe Statement 命令，但不等待它完成
 *
 * 返回：1 如果成功提交
 *
 *       0 如果出错（conn->errorMessage 已设置）      */
int
PQsendDescribePrepared(PGconn *conn, const char *stmt)
{
	return PQsendDescribe(conn, 'S', stmt);
}

/*
PQsendDescribePortal
 *   提交 Describe Portal 命令，但不等待它完成
 *
 * 返回：1 如果成功提交
 *
 *       0 如果出错（conn->errorMessage 已设置）      */
int
PQsendDescribePortal(PGconn *conn, const char *portal)
{
	return PQsendDescribe(conn, 'P', portal);
}

/*
PQsendDescribe
 *   发送 Describe 命令的公共代码
 *
 * desc_type 的可用选项为：
 *   'S' 描述预准备语句；或
 *   'P' 描述门户。
 *
 * 成功返回 1，失败返回 0。                       */
static int
PQsendDescribe(PGconn *conn, char desc_type, const char *desc_target)
{
	PGcmdQueueEntry *entry = NULL;

	/* null desc_target 作空串             */
	if (!desc_target)
		desc_target = "";

	if (!PQsendQueryStart(conn, true))
		return 0;

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* 错误消息已设置 */

	/* 构造 Describe 消息         */
	if (pqPutMsgStart('D', conn) < 0 ||
		pqPutc(desc_type, conn) < 0 ||
		pqPuts(desc_target, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/* 构造 Sync 消息         */
	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		if (pqPutMsgStart('S', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto sendFailed;
	}

	/* 记住正执行 Describe         */
	entry->queryclass = PGQUERY_DESCRIBE;

	/*
	给数据一个推送（在管线模式下，仅当我们超过大小
	 * 阈值时）。在非阻塞模式下，如果无法全部发送，
	 *
	 * 不要抱怨；PQgetResult() 会进行所需的额外刷新。  */
	if (pqPipelineFlush(conn) < 0)
		goto sendFailed;

	/* 已启动！       */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* 错误消息应该已经设置好      */
	return 0;
}

/*
 * PQnotifies
 *	  返回最新一条异步通知的 PGnotify* 结构，         
 * 其尚未被处理。        
 *
 * 若当前没有未处理的        
 * 来自后端的异步通知，则返回 NULL。 
 *
 * 调用者负责对返回的结构执行 FREE 释放。       
 *
 * 注意：此函数不会从套接字读取任何新数据；       
 * 因此通常调用者应先调用 PQconsumeInput()。 
 */
PGnotify *
PQnotifies(PGconn *conn)
{
	PGnotify   *event;

	if (!conn)
		return NULL;

	/* 解析任何可用数据，看看能否提取 NOTIFY 消息。     */
	parseInput(conn);

	event = conn->notifyHead;
	if (event)
	{
		conn->notifyHead = event->next;
		if (!conn->notifyHead)
			conn->notifyTail = NULL;
		event->next = NULL;		/* 勿让应用见内部状态          */
	}
	return event;
}

/*
PQputCopyData - 在 COPY IN 或 COPY BOTH 期间发送数据到后端
 *
 * 成功返回 1，数据无法发送返回 0（仅在非阻塞模式下可能），
 *
 * 发生错误返回 -1。           */
int
PQputCopyData(PGconn *conn, const char *buffer, int nbytes)
{
	if (!conn)
		return -1;
	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -1;
	}

	/*
	处理输入缓冲区中可能挂起的任何 NOTICE 或 NOTIFY 消息。
	 * 由于服务器可能在 COPY 期间生成大量通知，我们希望
	 * 及时清除它们，以防止输入缓冲区无限扩展。
	 * （注意：实际的输入数据读取发生在 pqSendSome 内部，
	 *
	 *
	 * 但它无权再次清除数据。）                                                                                 */
	parseInput(conn);

	if (nbytes > 0)
	{
		/*
		 * 尝试先刷新已发送的数据，而非扩大输出缓冲区。
		 * 若无法把缓冲区扩大到足以容纳数据，非阻塞时  
		 * 返回 0，否则为硬错误。（为简单起见，     
		 * 总是假设有 5 字节的开销。）        
		 */
		if ((conn->outBufSize - conn->outCount - 5) < nbytes)
		{
			if (pqFlush(conn) < 0)
				return -1;
			if (pqCheckOutBufferSpace(conn->outCount + 5 + (size_t) nbytes,
									  conn))
				return pqIsnonblocking(conn) ? 0 : -1;
		}
		/* 发数据（太简不委 fe-protocol）                     */
		if (pqPutMsgStart('d', conn) < 0 ||
			pqPutnchar(buffer, nbytes, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}
	return 1;
}

/*
PQputCopyEnd - 在 COPY IN 期间发送 EOF 指示给后端
 *
 * 调用此函数后，使用 PQgetResult() 检查命令完成状态。
 *
 * 成功返回 1，数据无法发送返回 0（仅在非阻塞模式下可能），
 *
 * 发生错误返回 -1。                */
int
PQputCopyEnd(PGconn *conn, const char *errormsg)
{
	if (!conn)
		return -1;
	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -1;
	}

	/*
	发送 COPY END 指示符。这很简单，不需要委托给
	 *
	 * fe-protocol 文件。                               */
	if (errormsg)
	{
		/* 发 COPY FAIL  */
		if (pqPutMsgStart('f', conn) < 0 ||
			pqPuts(errormsg, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}
	else
	{
		/* 发 COPY DONE  */
		if (pqPutMsgStart('c', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}

	/*
	 * 如果以扩展查询模式发送了 COPY 命令，必须同时发 
	 * 出 Sync。  
	 */
	if (conn->cmd_queue_head &&
		conn->cmd_queue_head->queryclass != PGQUERY_SIMPLE)
	{
		if (pqPutMsgStart('S', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return -1;
	}

	/* 返回活动状态    */
	if (conn->asyncStatus == PGASYNC_COPY_BOTH)
		conn->asyncStatus = PGASYNC_COPY_OUT;
	else
		conn->asyncStatus = PGASYNC_BUSY;

	/* 尝试刷新      */
	if (pqFlush(conn) < 0)
		return -1;

	return 1;
}

/*
 * PQgetCopyData - 在 COPY OUT 或 COPY BOTH 期间读取后端      
 * 一行数据
 *
 * 成功时将 *buffer 设为指向一行 malloc 的数据，并     
 * 返回该行长度（总是 > 0）。     
 * 若尚无可用行则返回 0（仅在 async 为真时可能）， 
 * -1 = COPY 结束 (查 PQgetResult), -2 = 错误 (查            
 * PQerrorMessage).
 */
int
PQgetCopyData(PGconn *conn, char **buffer, int async)
{
	*buffer = NULL;				/* 所有失败情况    */
	if (!conn)
		return -2;
	if (conn->asyncStatus != PGASYNC_COPY_OUT &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return -2;
	}
	return pqGetCopyData3(conn, buffer, async);
}

/*
PQgetline - 从后端获取换行符终止的字符串。
 *
 * 主要用于应用程序使用 "COPY <rel> to stdout" 并读取
 * 输出字符串。返回 buffer 中以 null 终止的字符串。
 *
 * XXX 此例程现已弃用，因为它无法处理二进制数据。
 * 如果在 COPY BINARY 期间调用，则返回 EOF。
 *
 * PQgetline 读取最多 length-1 个字符（类似 fgets(3)），
 * 但去除终止的 \n（类似 gets(3)）。
 *
 * 注意：调用者负责在使用此例程时检测 COPY 结束信号
 * （仅包含 "\." 的行）。
 *
 * 返回值：
 *   EOF 如果出错（例如给出了无效参数）
 *   0 如果到达 EOL（即已读取 \n）
 *     （这是为了向后兼容 --- 此例程过去总是返回 EOF 或 0，
 *     假定行在 length 字节内结束。）
 *
 *
 *   1 其他情况（即缓冲区在 \n 到达之前已填满）                                                                       */
int
PQgetline(PGconn *conn, char *buffer, int length)
{
	if (!buffer || length <= 0)
		return EOF;
	*buffer = '\0';
	/* length 必须至少为 3 以容纳 \. 终止符！   */
	if (length < 3)
		return EOF;

	if (!conn)
		return EOF;

	return pqGetline3(conn, buffer, length);
}

/*
PQgetlineAsync - 无阻塞地获取 COPY 数据行。
 *
 * 此例程适用于希望异步执行 "COPY <rel> to stdout"
 * 的应用程序，即不阻塞。在发出 COPY 命令并获得
 * PGRES_COPY_OUT 响应后，应用程序应调用 PQconsumeInput
 * 和此例程，直到检测到数据结束信号。与 PQgetline 不同，
 * 此例程负责检测数据结束。
 *
 * 每次调用时，如果 libpq 的输入缓冲区中有完整的数据行，
 * PQgetlineAsync 将返回数据。否则，直到该行的剩余部分
 * 到达之前不会返回数据。
 *
 * 如果返回 -1，则已识别出数据结束信号（并已从 libpq 的
 * 输入缓冲区中移除）。调用者*必须*接下来调用 PQendcopy，
 * 然后返回正常处理。
 *
 * 返回值：
 *   -1  如果已识别出 COPY 数据结束标记
 *   0   如果没有可用数据
 *   >0  返回的字节数。
 *
 * 返回的数据不会跨越数据行边界。如果可能，
 * 一次返回一整行。但如果调用者提供的缓冲区太小，
 * 无法容纳后端发送的一行，则返回部分数据行。
 * 在文本模式下，可以通过测试最后一个返回的字节
 * 是否为 '\n' 来检测这一点。
 *
 *
 * 返回的数据*不*以 null 终止。                                                                                                                     */

int
PQgetlineAsync(PGconn *conn, char *buffer, int bufsize)
{
	if (!conn)
		return -1;

	return pqGetlineAsync3(conn, buffer, bufsize);
}

/*
 * PQputline -- 在 COPY IN 期间向后端发送字符串。
 * OK 则返 0，否则 EOF。 
 *
 * 此函数已弃用，主要原因是其返回约定无法让调用      
 * 者区分硬错误与非阻塞模式下的发送失败。             
 *              
 */
int
PQputline(PGconn *conn, const char *string)
{
	return PQputnbytes(conn, string, strlen(string));
}

/*
PQputnbytes -- 类似 PQputline，缓冲区可不以 null 终止。
 *
 * 成功返 0，否则 EOF。      */
int
PQputnbytes(PGconn *conn, const char *buffer, int nbytes)
{
	if (PQputCopyData(conn, buffer, nbytes) > 0)
		return 0;
	else
		return EOF;
}

/*
PQendcopy
 *   完成 COPY IN/OUT 的数据传输部分后，
 *   应用程序必须调用此例程来完成命令协议。
 *
 * 这已弃用；使用 PQgetResult 获取传输状态更简洁。
 *
 * 返回值：
 *   0 成功
 *
 *
 *   1 失败                                            */
int
PQendcopy(PGconn *conn)
{
	if (!conn)
		return 0;

	return pqEndcopy3(conn);
}


/* ----------------
 *		PQfn - 向 POSTGRES 后端发送函数调用。     
 *
 *		conn			: 后端连接      
 *		fnid			: 要调用的函数的 OID   
 *		result_buf		: 结果缓冲区的指针
 *		result_len		: 结果的实际长度在此返回       
 *		result_is_int	: 若结果为整数必须为 1，            
 *						  否则应为 0            
 *		args			: 函数参数数组的指针              
 *						  （各有长度（整数时）和值/指针）  
 *		nargs			: 参数数组中的个数。  
 *
 * 返回 
 *		成功返回状态为 PGRES_COMMAND_OK 的 PGresult。
 *			*result_len 有返回值时 > 0，没有则为 0。     
 *		后端出错时返回状态为 PGRES_FATAL_ERROR 的 PGresult。     
 *		通信失败时返回 NULL。conn->errorMessage 会被设置。 
 * ----------------
 */

PGresult *
PQfn(PGconn *conn,
	 int fnid,
	 int *result_buf,
	 int *result_len,
	 int result_is_int,
	 const PQArgBlock *args,
	 int nargs)
{
	*result_len = 0;

	if (!conn)
		return NULL;

	/*
	由于这是查询周期的开始，重置错误状态。
	 * 但是，在管线模式下且已有内容排队时，错误缓冲区
	 *
	 * 属于该命令，我们不应该清除它。                      */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "%s not allowed in pipeline mode", "PQfn");
		return NULL;
	}

	if (conn->sock == PGINVALID_SOCKET || conn->asyncStatus != PGASYNC_IDLE ||
		pgHavePendingResult(conn))
	{
		libpq_append_conn_error(conn, "connection in wrong state");
		return NULL;
	}

	return pqFunctionCall3(conn, fnid,
						   result_buf, result_len,
						   result_is_int,
						   args, nargs);
}

/* ====== 管线模式支持 ========    */

/*
 * PQenterPipelineMode
 *		把空闲连接置入管线模式。    
 *
 * 成功返回 1。失败时设置 errorMessage 并返回 0。            
 *
 * 此后提交的命令即可在该连接上管线化执行；     
 * 不必等待某个命令完成后才派发下一个。             
 *            
 *
 * 在 COPY 期间不允许排队新查询或进行同步。    
 *
 * 一组命令由一个 PQpipelineSync 终止。在管线模式      
 * 中可以建立多个同步点。处理完所有结果后，        
 * 通过调用 PQexitPipelineMode() 退出管线模式。                  
 *
 * 它实际上并不在线路上发送任何内容，只是让 libpq 
 * 进入可以管线化工作的状态。 
 */
int
PQenterPipelineMode(PGconn *conn)
{
	if (!conn)
		return 0;

	/* 已在管线模式则无操作成功               */
	if (conn->pipelineStatus != PQ_PIPELINE_OFF)
		return 1;

	if (conn->asyncStatus != PGASYNC_IDLE)
	{
		libpq_append_conn_error(conn, "cannot enter pipeline mode, connection not idle");
		return 0;
	}

	conn->pipelineStatus = PQ_PIPELINE_ON;

	return 1;
}

/*
 * PQexitPipelineMode
 *		结束管线模式并返回正常命令模式。    
 *
 * 成功返回 1（管线模式成功结束，或不在管线模式）。   
 *       
 *
 * 若在管线模式且尚不能结束则返回 0，将设置错误消息。
 *        
 */
int
PQexitPipelineMode(PGconn *conn)
{
	if (!conn)
		return 0;

	if (conn->pipelineStatus == PQ_PIPELINE_OFF &&
		(conn->asyncStatus == PGASYNC_IDLE ||
		 conn->asyncStatus == PGASYNC_PIPELINE_IDLE) &&
		conn->cmd_queue_head == NULL)
		return 1;

	switch (conn->asyncStatus)
	{
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
			/* 有一些未收集的结果        */
			libpq_append_conn_error(conn, "cannot exit pipeline mode with uncollected results");
			return 0;

		case PGASYNC_BUSY:
			libpq_append_conn_error(conn, "cannot exit pipeline mode while busy");
			return 0;

		case PGASYNC_IDLE:
		case PGASYNC_PIPELINE_IDLE:
			/* OK */
			break;

		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
			libpq_append_conn_error(conn, "cannot exit pipeline mode while in COPY");
	}

	/* 仍有工作要处理 */
	if (conn->cmd_queue_head != NULL)
	{
		libpq_append_conn_error(conn, "cannot exit pipeline mode with uncollected results");
		return 0;
	}

	conn->pipelineStatus = PQ_PIPELINE_OFF;
	conn->asyncStatus = PGASYNC_IDLE;

	/* 刷新输出缓冲挂起数据       */
	if (pqFlush(conn) < 0)
		return 0;				/* 错误消息已设置          */
	return 1;
}

/*
pqCommandQueueAdvance
 *   适当地从命令队列中删除一个查询。
 *
 * 如果我们已收到对应命令队列头部元素的所有结果，
 * 则删除它。
 *
 * 在简单查询协议中，我们必须等到收到 ReadyForQuery 消息
 * 后才能推进命令队列。这是因为在简单模式下，一个命令
 * 可以包含多个查询，我们必须处理完所有查询的结果
 * 才能继续下一个命令。
 *
 * 另一个考虑因素是扩展查询协议中错误处理期间的同步：
 * 我们拒绝将队列推进到超过 SYNC 队列元素，
 * 除非我们收到的结果也是 SYNC。这特别保护我们
 *
 *
 * 在收到错误时不恰当地推进。                                                                                                    */
void
pqCommandQueueAdvance(PGconn *conn, bool isReadyForQuery, bool gotSync)
{
	PGcmdQueueEntry *prevquery;

	if (conn->cmd_queue_head == NULL)
		return;

	/*
	 * 如果正在处理简单查询协议的查询，则只有在收到 
	 * ReadyForQuery 消息时才推进队列。              
	 */
	if (conn->cmd_queue_head->queryclass == PGQUERY_SIMPLE && !isReadyForQuery)
		return;

	/*
	如果我们在等待 SYNC，不要推进队列直到收到一个。
 *     */
	if (conn->cmd_queue_head->queryclass == PGQUERY_SYNC && !gotSync)
		return;

	/* 从队列解链           */
	prevquery = conn->cmd_queue_head;
	conn->cmd_queue_head = conn->cmd_queue_head->next;

	/* 如果队列现在为空，也重置尾部    */
	if (conn->cmd_queue_head == NULL)
		conn->cmd_queue_tail = NULL;

	/* 并使队列元素可回收           */
	prevquery->next = NULL;
	pqRecycleCmdQueueEntry(conn, prevquery);
}

/*
pqPipelineProcessQueue：PQgetResult 的子例程
 *
 *   在管线模式下，开始处理队列中下一个查询的结果。           */
static void
pqPipelineProcessQueue(PGconn *conn)
{
	switch (conn->asyncStatus)
	{
		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
		case PGASYNC_BUSY:
			/* 客户端仍需处理当前查询或结果           */
			return;

		case PGASYNC_IDLE:

			/*
		如果我们处于 IDLE 模式且队列中有命令，
			 * 让我们进入 PIPELINE_IDLE 模式并正常处理。
 *
			 * 否则没有事情可做。                       */
			if (conn->cmd_queue_head != NULL)
			{
				conn->asyncStatus = PGASYNC_PIPELINE_IDLE;
				break;
			}
			return;

		case PGASYNC_PIPELINE_IDLE:
			Assert(conn->pipelineStatus != PQ_PIPELINE_OFF);
			/* 下一查询      */
			break;
	}

	/*
	重置单行模式。（客户端须为每查询单设。）
 *
 *                                */
	conn->singleRowMode = false;

	/*
	 * 如果队列中没有更多待处理命令，现在就进入       
	 * "real idle" 模式。
	 */
	if (conn->cmd_queue_head == NULL)
	{
		conn->asyncStatus = PGASYNC_IDLE;
		return;
	}

	/*
	 * 重置错误状态。这一步和接下来的几步对应于           
	 * PQsendQueryStart 未曾为此查询做的事。
	 */
	pqClearConnErrorState(conn);

	/* 初始化异步结果累积状态          */
	pqClearAsyncResult(conn);

	if (conn->pipelineStatus == PQ_PIPELINE_ABORTED &&
		conn->cmd_queue_head->queryclass != PGQUERY_SYNC)
	{
		/*
		在中止的管线中，我们不会从服务器收到每个
		 * 结果；我们只是从队列中丢弃命令，直到从服务器
		 * 收到下一个同步。
		 *
		 * PGRES_PIPELINE_ABORTED 结果告诉客户端其查询
		 *
		 * 已被中止。                               */
		conn->result = PQmakeEmptyPGresult(conn, PGRES_PIPELINE_ABORTED);
		if (!conn->result)
		{
			libpq_append_conn_error(conn, "out of memory");
			pqSaveErrorResult(conn);
			return;
		}
		conn->asyncStatus = PGASYNC_READY;
	}
	else
	{
		/* 允许解析继续        */
		conn->asyncStatus = PGASYNC_BUSY;
	}
}

/*
PQpipelineSync
 *   作为管线的一部分发送 Sync 消息，并刷新到服务器
 *
 * 可以立即开始提交管道中的更多命令，
 * 无需等待当前管道的结果。无需结束管线模式
 * 再重新开始。
 *
 * 如果管道中的命令失败，则在 PQpipelineSync 发送的
 * Sync 消息的结果之前（包括该结果）的每个后续命令
 * 都会被设置为 PGRES_PIPELINE_ABORTED 状态。如果整个
 * 管道处理无错，则生成 PGRES_PIPELINE_SYNC 的 PGresult。
 *
 * 可以在调用 PQpipelineSync 之前已经发送了查询，但
 * 在检索命令结果之前需要调用 PQpipelineSync。
 *
 * 连接将保持在管线模式，直到客户端处理完管线的
 *
 *
 * 所有结果后，新的同步命令执行函数才可用。                                                                                                                            */
int
PQpipelineSync(PGconn *conn)
{
	PGcmdQueueEntry *entry;

	if (!conn)
		return 0;

	if (conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "cannot send pipeline when not in pipeline mode");
		return 0;
	}

	switch (conn->asyncStatus)
	{
		case PGASYNC_COPY_IN:
		case PGASYNC_COPY_OUT:
		case PGASYNC_COPY_BOTH:
			/* 应该不可达       */
			appendPQExpBufferStr(&conn->errorMessage,
								 "internal error: cannot send pipeline while in COPY\n");
			return 0;
		case PGASYNC_READY:
		case PGASYNC_READY_MORE:
		case PGASYNC_BUSY:
		case PGASYNC_IDLE:
		case PGASYNC_PIPELINE_IDLE:
			/* 可发同步    */
			break;
	}

	entry = pqAllocCmdQueueEntry(conn);
	if (entry == NULL)
		return 0;				/* 错误消息已设置 */

	entry->queryclass = PGQUERY_SYNC;
	entry->query = NULL;

	/* 构造 Sync 消息         */
	if (pqPutMsgStart('S', conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
		goto sendFailed;

	/*
	 * 推一把数据。非阻塞模式下，如果无法全部发送不要  
	 * 抱怨；PQgetResult() 会完成所需的额外刷新。             
	 */
	if (PQflush(conn) < 0)
		goto sendFailed;

	/* 已启动！       */
	pqAppendCmdQueueEntry(conn, entry);

	return 1;

sendFailed:
	pqRecycleCmdQueueEntry(conn, entry);
	/* 错误消息应该已经设置好      */
	return 0;
}

/*
PQsendFlushRequest
 *   发送请求让服务器刷新其缓冲区。在管线模式中
 *
 *   当不需要同步点时很有用。   */
int
PQsendFlushRequest(PGconn *conn)
{
	if (!conn)
		return 0;

	/* 无活动连接则不发送。                           */
	if (conn->status != CONNECTION_OK)
	{
		libpq_append_conn_error(conn, "no connection to the server");
		return 0;
	}

	/* 在忙碌时也不能发送，除非正在为稍后排队         */
	if (conn->asyncStatus != PGASYNC_IDLE &&
		conn->pipelineStatus == PQ_PIPELINE_OFF)
	{
		libpq_append_conn_error(conn, "another command is already in progress");
		return 0;
	}

	if (pqPutMsgStart('H', conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return 0;
	}

	/*
	给数据一个推送（在管线模式下，仅当我们超过大小
	 * 阈值时）。在非阻塞模式下，如果无法全部发送，
	 *
	 * 不要抱怨；PQgetResult() 会进行所需的额外刷新。  */
	if (pqPipelineFlush(conn) < 0)
		return 0;

	return 1;
}

/* ====== PGresult 的访问器函数 ======== */

ExecStatusType
PQresultStatus(const PGresult *res)
{
	if (!res)
		return PGRES_FATAL_ERROR;
	return res->resultStatus;
}

char *
PQresStatus(ExecStatusType status)
{
	if ((unsigned int) status >= lengthof(pgresStatus))
		return libpq_gettext("invalid ExecStatusType code");
	return pgresStatus[status];
}

char *
PQresultErrorMessage(const PGresult *res)
{
	if (!res || !res->errMsg)
		return "";
	return res->errMsg;
}

char *
PQresultVerboseErrorMessage(const PGresult *res,
							PGVerbosity verbosity,
							PGContextVisibility show_context)
{
	PQExpBufferData workBuf;

	/*
	因为期望调用者释放结果字符串，我们必须
	 * strdup 任何常量结果。使用普通的 strdup 并文档说明
	 *
	 * 如果内存不足，调用者应预期 NULL。       */
	if (!res ||
		(res->resultStatus != PGRES_FATAL_ERROR &&
		 res->resultStatus != PGRES_NONFATAL_ERROR))
		return strdup(libpq_gettext("PGresult is not an error result\n"));

	initPQExpBuffer(&workBuf);

	pqBuildErrorMessage3(&workBuf, res, verbosity, show_context);

	/* 如果内存不足以格式化消息，干净地失败     */
	if (PQExpBufferDataBroken(workBuf))
	{
		termPQExpBuffer(&workBuf);
		return strdup(libpq_gettext("out of memory\n"));
	}

	return workBuf.data;
}

char *
PQresultErrorField(const PGresult *res, int fieldcode)
{
	PGMessageField *pfield;

	if (!res)
		return NULL;
	for (pfield = res->errFields; pfield != NULL; pfield = pfield->next)
	{
		if (pfield->code == fieldcode)
			return pfield->contents;
	}
	return NULL;
}

int
PQntuples(const PGresult *res)
{
	if (!res)
		return 0;
	return res->ntups;
}

int
PQnfields(const PGresult *res)
{
	if (!res)
		return 0;
	return res->numAttributes;
}

int
PQbinaryTuples(const PGresult *res)
{
	if (!res)
		return 0;
	return res->binary;
}

/*
 * 对字段号和元组号进行范围检查的辅助例程。   
 * 合法 true，否则 false     
 */

static int
check_field_number(const PGresult *res, int field_num)
{
	if (!res)
		return false;			/* 无法显示错误消息...        */
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		pqInternalNotice(&res->noticeHooks,
						 "column number %d is out of range 0..%d",
						 field_num, res->numAttributes - 1);
		return false;
	}
	return true;
}

static int
check_tuple_field_number(const PGresult *res,
						 int tup_num, int field_num)
{
	if (!res)
		return false;			/* 无法显示错误消息...        */
	if (tup_num < 0 || tup_num >= res->ntups)
	{
		pqInternalNotice(&res->noticeHooks,
						 "row number %d is out of range 0..%d",
						 tup_num, res->ntups - 1);
		return false;
	}
	if (field_num < 0 || field_num >= res->numAttributes)
	{
		pqInternalNotice(&res->noticeHooks,
						 "column number %d is out of range 0..%d",
						 field_num, res->numAttributes - 1);
		return false;
	}
	return true;
}

static int
check_param_number(const PGresult *res, int param_num)
{
	if (!res)
		return false;			/* 无法显示错误消息...        */
	if (param_num < 0 || param_num >= res->numParameters)
	{
		pqInternalNotice(&res->noticeHooks,
						 "parameter number %d is out of range 0..%d",
						 param_num, res->numParameters - 1);
		return false;
	}

	return true;
}

/*
如果 field_num 无效则返回 NULL
 *     */
char *
PQfname(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return NULL;
	if (res->attDescs)
		return res->attDescs[field_num].name;
	else
		return NULL;
}

/*
 * PQfnumber：根据列名查找列号           
 *
 * 列名的解析方式如同它在 SQL 语句中一样，包括       
 * 大小写折叠与双引号处理。但注意可能存在的陷阱： 
 * 前端的小写转换可能与后端遵循不同                   
 * 的区域设置规则…… 
 *
 * 若无匹配则返回 -1。在当前后端中还可能存在       
 * 多个匹配的情况，此时返回找到的第一个。      
 */
int
PQfnumber(const PGresult *res, const char *field_name)
{
	char	   *field_case;
	bool		in_quotes;
	bool		all_lower = true;
	const char *iptr;
	char	   *optr;
	int			i;

	if (!res)
		return -1;

	/*
	 * 注意：拒绝零长度输入串是正确的；匹配零长度     
	 * 字段名的正确输入是 ""。                   
	 */
	if (field_name == NULL ||
		field_name[0] == '\0' ||
		res->attDescs == NULL)
		return -1;

	/*
	检查是否可以避免 strdup() 和相关工作，因为传入的
	 *
	 * 字符串在检查之前无论如何都不会被改变。        */
	for (iptr = field_name; *iptr; iptr++)
	{
		char		c = *iptr;

		if (c == '"' || c != pg_tolower((unsigned char) c))
		{
			all_lower = false;
			break;
		}
	}

	if (all_lower)
		for (i = 0; i < res->numAttributes; i++)
			if (strcmp(field_name, res->attDescs[i].name) == 0)
				return i;

	/* Fall through to the normal check if that didn't work out. */

	/*
	注意：此代码不会拒绝部分引用的字符串，例如
	 * foo"BAR"foo 将变为 fooBARfoo，而它可能应该是一个
	 *
	 * 错误条件。    */
	field_case = strdup(field_name);
	if (field_case == NULL)
		return -1;				/* 粗糙 */

	in_quotes = false;
	optr = field_case;
	for (iptr = field_case; *iptr; iptr++)
	{
		char		c = *iptr;

		if (in_quotes)
		{
			if (c == '"')
			{
				if (iptr[1] == '"')
				{
					/* 双引号变为单引号             */
					*optr++ = '"';
					iptr++;
				}
				else
					in_quotes = false;
			}
			else
				*optr++ = c;
		}
		else if (c == '"')
			in_quotes = true;
		else
		{
			c = pg_tolower((unsigned char) c);
			*optr++ = c;
		}
	}
	*optr = '\0';

	for (i = 0; i < res->numAttributes; i++)
	{
		if (strcmp(field_case, res->attDescs[i].name) == 0)
		{
			free(field_case);
			return i;
		}
	}
	free(field_case);
	return -1;
}

Oid
PQftable(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].tableid;
	else
		return InvalidOid;
}

int
PQftablecol(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].columnid;
	else
		return 0;
}

int
PQfformat(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].format;
	else
		return 0;
}

Oid
PQftype(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return InvalidOid;
	if (res->attDescs)
		return res->attDescs[field_num].typid;
	else
		return InvalidOid;
}

int
PQfsize(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].typlen;
	else
		return 0;
}

int
PQfmod(const PGresult *res, int field_num)
{
	if (!check_field_number(res, field_num))
		return 0;
	if (res->attDescs)
		return res->attDescs[field_num].atttypmod;
	else
		return 0;
}

char *
PQcmdStatus(PGresult *res)
{
	if (!res)
		return NULL;
	return res->cmdStatus;
}

/*
 * PQoidStatus -
 *	若上一条命令是 INSERT，则返回 oid 字符串  
 *	否则返 ""。  
 */
char *
PQoidStatus(const PGresult *res)
{
	/*
	 * 这必须足以容纳结果。别笑，这已经比                
	 * 此函数过去的行为要好。  
	 */
	static char buf[24];

	size_t		len;

	if (!res || strncmp(res->cmdStatus, "INSERT ", 7) != 0)
		return "";

	len = strspn(res->cmdStatus + 7, "0123456789");
	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;
	memcpy(buf, res->cmdStatus + 7, len);
	buf[len] = '\0';

	return buf;
}

/*
 * PQoidValue -
 *	上述函数的更佳形式，其仅返回               
 *	Oid 类型 
 */
Oid
PQoidValue(const PGresult *res)
{
	char	   *endptr = NULL;
	unsigned long result;

	if (!res ||
		strncmp(res->cmdStatus, "INSERT ", 7) != 0 ||
		res->cmdStatus[7] < '0' ||
		res->cmdStatus[7] > '9')
		return InvalidOid;

	result = strtoul(res->cmdStatus + 7, &endptr, 10);

	if (!endptr || (*endptr != ' ' && *endptr != '\0'))
		return InvalidOid;
	else
		return (Oid) result;
}


/*
PQcmdTuples -
 *   如果最后一个命令是 INSERT/UPDATE/DELETE/MERGE/MOVE/FETCH/COPY，
 *   返回包含插入/影响元组数的字符串。
 *   如果不是，返回 ""。
 *
 *
 *   XXX：这可能应该返回 int    */
char *
PQcmdTuples(PGresult *res)
{
	char	   *p,
			   *c;

	if (!res)
		return "";

	if (strncmp(res->cmdStatus, "INSERT ", 7) == 0)
	{
		p = res->cmdStatus + 7;
		/* INSERT：跳过 oid 空格 */
		while (*p && *p != ' ')
			p++;
		if (*p == 0)
			goto interpret_error;	/* 无空格 */
		p++;
	}
	else if (strncmp(res->cmdStatus, "SELECT ", 7) == 0 ||
			 strncmp(res->cmdStatus, "DELETE ", 7) == 0 ||
			 strncmp(res->cmdStatus, "UPDATE ", 7) == 0)
		p = res->cmdStatus + 7;
	else if (strncmp(res->cmdStatus, "FETCH ", 6) == 0 ||
			 strncmp(res->cmdStatus, "MERGE ", 6) == 0)
		p = res->cmdStatus + 6;
	else if (strncmp(res->cmdStatus, "MOVE ", 5) == 0 ||
			 strncmp(res->cmdStatus, "COPY ", 5) == 0)
		p = res->cmdStatus + 5;
	else
		return "";

	/* 检查为整数（至少一数字）                             */
	for (c = p; *c; c++)
	{
		if (!isdigit((unsigned char) *c))
			goto interpret_error;
	}
	if (c == p)
		goto interpret_error;

	return p;

interpret_error:
	pqInternalNotice(&res->noticeHooks,
					 "could not interpret result from server: %s",
					 res->cmdStatus);
	return "";
}

/*
 * PQgetvalue:
 *	返回第 'tup_num' 行第 'field_num' 字段的值   
 */
char *
PQgetvalue(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return NULL;
	return res->tuples[tup_num][field_num].value;
}

/* PQgetlength：
 *
 *   返字段值实际字节长度。              */
int
PQgetlength(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return 0;
	if (res->tuples[tup_num][field_num].len != NULL_LEN)
		return res->tuples[tup_num][field_num].len;
	else
		return 0;
}

/* PQgetisnull：
 *
 *   返回字段值的 null 状态。   */
int
PQgetisnull(const PGresult *res, int tup_num, int field_num)
{
	if (!check_tuple_field_number(res, tup_num, field_num))
		return 1;				/* 假装它是 null  */
	if (res->tuples[tup_num][field_num].len == NULL_LEN)
		return 1;
	else
		return 0;
}

/* PQnparams:
 *	返回预备语句的输入参数个数。                     
 */
int
PQnparams(const PGresult *res)
{
	if (!res)
		return 0;
	return res->numParameters;
}

/* PQparamtype：
 *
 *   返回指定语句参数的类型 Oid。         */
Oid
PQparamtype(const PGresult *res, int param_num)
{
	if (!check_param_number(res, param_num))
		return InvalidOid;
	if (res->paramDescs)
		return res->paramDescs[param_num].typid;
	else
		return InvalidOid;
}


/* PQsetnonblocking:
 *	若参数为 true，将 PGconn 的数据库连接设为非阻塞，  
 *	参数为 false 则设为阻塞；这不会保护你免受     
 *	PQexec() 的影响；只有使用非阻塞 API 时才是安全的。   
 *	只需在已连接的数据库连接上调用。           
 */
int
PQsetnonblocking(PGconn *conn, int arg)
{
	bool		barg;

	if (!conn || conn->status == CONNECTION_BAD)
		return -1;

	barg = (arg ? true : false);

	/* 如果套接字已经处于请求的状态，提前退出 */
	if (barg == conn->nonblocking)
		return 0;

	/*
	为了保证刷新/查询/结果轮询行为的一致性，
	 * 我们需要在此时刷新发送队列以保证正确行为。
	 * 这是可以的，因为无论是从阻塞模式转换或转换到
	 * 阻塞模式，我们都可以阻塞调用者。
	 *
	 * 清除错误状态以防 pqFlush 向其添加内容，
	 *
	 * 除非正在主动进行管线处理，此时最好不要清除。                      */
	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/* 阻塞转非阻塞时刷新                              */
	if (pqFlush(conn))
		return -1;

	conn->nonblocking = barg;

	return 0;
}

/*
 * 返回数据库连接的阻塞状态                 
 *		true == 非阻塞，false == 阻塞   
 */
int
PQisnonblocking(const PGconn *conn)
{
	if (!conn || conn->status == CONNECTION_BAD)
		return false;
	return pqIsnonblocking(conn);
}

/* libpq 线程安全？ */
int
PQisthreadsafe(void)
{
#ifdef ENABLE_THREAD_SAFETY
	return true;
#else
	return false;
#endif
}


/* 尝试强制输出数据，实际上只对非阻塞用户有用  */
int
PQflush(PGconn *conn)
{
	if (!conn || conn->status == CONNECTION_BAD)
		return -1;
	return pqFlush(conn);
}

/*
pqPipelineFlush
 *
 * 在管线模式下，数据仅当输出缓冲区达到阈值时才会刷新。
 * 在非管线模式下，行为与标准的 pqFlush 相同。
 *
 *
 * 成功返回 0。           */
static int
pqPipelineFlush(PGconn *conn)
{
	if ((conn->pipelineStatus != PQ_PIPELINE_ON) ||
		(conn->outCount >= OUTBUFFER_THRESHOLD))
		return pqFlush(conn);
	return 0;
}


/*
PQfreemem - 安全释放分配的内存
 *
 * 主要用于 Win32，除非使用多线程 DLL（VC6 中的 /MD）
 *
 * 用于释放 PQescapeBytea()/PQunescapeBytea() 的内存      */
void
PQfreemem(void *ptr)
{
	free(ptr);
}

/*
PQfreeNotify - 释放与 PGnotify 关联的内存
 *
 * 此函数仅为了二进制向后兼容而存在。
 * 新代码应使用 PQfreemem()。宏会自动将
 *
 * 调用映射到 PQfreemem。将来应删除此函数。bjm 2003-03-24                                         */

#undef PQfreeNotify
void		PQfreeNotify(PGnotify *notify);

void
PQfreeNotify(PGnotify *notify)
{
	PQfreemem(notify);
}


/*
转义任意字符串以获取有效的 SQL 字面量字符串。
 *
 * 将 "'" 替换为 "''"，如果不在 std_strings 模式下，将 "\" 替换为 "\\"。
 *
 * length 是源字符串的长度。（注意：如果提前遇到终止 NUL，
 * PQescapeString 会在 "length" 之前停止；因此行为
 * 更像 strncpy。）
 *
 * 为安全起见，"to" 处的缓冲区必须至少为 2*length + 1 字节长。
 * 终止 NUL 字符被添加到输出字符串，无论输入
 * 是否以 NUL 终止。
 *
 *
 * 返回输出的实际长度（不包括终止 NUL）。                                */
static size_t
PQescapeStringInternal(PGconn *conn,
					   char *to, const char *from, size_t length,
					   int *error,
					   int encoding, bool std_strings)
{
	const char *source = from;
	char	   *target = to;
	size_t		remaining = strnlen(from, length);
	bool		already_complained = false;

	if (error)
		*error = 0;

	while (remaining > 0)
	{
		char		c = *source;
		int			charlen;
		int			i;

		/* 纯 ASCII 快路径       */
		if (!IS_HIGHBIT_SET(c))
		{
			/* 需要时加引号      */
			if (SQL_STR_DOUBLE(c, !std_strings))
				*target++ = c;
			/* 复制字符       */
			*target++ = c;
			source++;
			remaining--;
			continue;
		}

		/* 可能多字节字符的慢速路径        */
		charlen = pg_encoding_mblen_or_incomplete(encoding,
												  source, remaining);

		if (remaining < charlen ||
			pg_encoding_verifymbchar(encoding, source, charlen) == -1)
		{
			/*
			 * 多字节字符无效。验证这一点很重要，           
			 * 因为无效的多字节字符可能被用来"跳过"       
			 * 引号字符，例如在解析          
			 * 逐个字符时。     
			 *
			 * 如有可能就报告错误，并将该字符的第一个     
			 * 字节替换为无效序列。该无效序列能确保     
			 * 被转义的串将在服务器端触发错误，    
			 * 即使我们无法在此处直接报告错误。            
			 *
			 * 若能向调用者报告错误，这就不是*那么*       
			 * 关键；但若不能报告或调用者忽略了它，调用
			 * 者将不加修改地使用该串，它必须对解析     
			 * 安全  
			 *
			 * 我们知道肯定有足够的空间容纳无效序列，    
			 * 因为 "to" 缓冲区的长度至少为 2 * length + 1，    
			 * 而最坏情况下我们用两个无效字节替换一个    
			 * 字节
			 *
			 * 在首次遇到设置了高位的字节时就验证整个    
			 * 字符串会稍快一些，但这样我们可以只替换   
			 * 无效的数据，这可能让用户更容易在较大的      
			 * 字符串中找到无效编码的部分。               
			 */
			if (error)
				*error = 1;
			if (conn && !already_complained)
			{
				if (remaining < charlen)
					libpq_append_conn_error(conn, "incomplete multibyte character");
				else
					libpq_append_conn_error(conn, "invalid multibyte character");
				/* 每个字符串只发一次投诉      */
				already_complained = true;
			}

			pg_encoding_set_invalid(encoding, target);
			target += 2;

			/*
			 * 将后续字节当作此字节不存在来处理。这样      
			 * 更安全，以防后续字节中包含对调用者重要      
			 * 的字符（如 html 中'>'）。 
			 */
			source++;
			remaining--;
		}
		else
		{
			/* 复制字符       */
			for (i = 0; i < charlen; i++)
			{
				*target++ = *source++;
				remaining--;
			}
		}
	}

	/* 写入终止 NUL 字符。           */
	*target = '\0';

	return target - to;
}

size_t
PQescapeStringConn(PGconn *conn,
				   char *to, const char *from, size_t length,
				   int *error)
{
	if (!conn)
	{
		/* 强制空字符串结果  */
		*to = '\0';
		if (error)
			*error = 1;
		return 0;
	}

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	return PQescapeStringInternal(conn, to, from, length, error,
								  conn->client_encoding,
								  conn->std_strings);
}

size_t
PQescapeString(char *to, const char *from, size_t length)
{
	return PQescapeStringInternal(NULL, to, from, length, NULL,
								  static_client_encoding,
								  static_std_strings);
}


/*
 * 后端 add_size() 函数的前端版本，旨在与                        
 * pg_add_*_overflow() 辅助函数保持 API 兼容。成功时                
 * 结果存入 *dst；加法溢出则返 true。 
 *
 * TODO: 移到 common/int.h 
 */
static bool
add_size_overflow(size_t s1, size_t s2, size_t *dst)
{
	size_t		result;

	result = s1 + s2;
	if (result < s1 || result < s2)
		return true;

	*dst = result;
	return false;
}


/*
 * 转义任意字符串。若 as_ident 为 true，结果被           
 * 转义为标识符；若为 false，则按字面量转义。结果  
 * 在新分配的缓冲区中返回。如果因编码违规或内存不    
 * 足而失败，我们返回 NULL，并将错误消息存入 conn。     
 */
static char *
PQescapeInternal(PGconn *conn, const char *str, size_t len, bool as_ident)
{
	const char *s;
	char	   *result;
	char	   *rp;
	size_t		num_quotes = 0; /* 单或双引，视 as_ident             */
	size_t		num_backslashes = 0;
	size_t		input_len = strnlen(str, len);
	size_t		result_size;
	char		quote_char = as_ident ? '"' : '\'';
	bool		validated_mb = false;

	/* 我们必须有连接，否则立即失败。     */
	if (!conn)
		return NULL;

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	/*
	扫描字符串中必须转义的字符和无效编码的数据。
 *
 *                     */
	s = str;
	for (size_t remaining = input_len; remaining > 0; remaining--, s++)
	{
		if (*s == quote_char)
			++num_quotes;
		else if (*s == '\\')
			++num_backslashes;
		else if (IS_HIGHBIT_SET(*s))
		{
			int			charlen;

			/* 可能多字节字符的慢速路径        */
			charlen = pg_encoding_mblen_or_incomplete(conn->client_encoding,
													  s, remaining);

			if (charlen > remaining)
			{
				/* 多字节字符超出允许长度。           */
				libpq_append_conn_error(conn, "incomplete multibyte character");
				return NULL;
			}

			/*
			 * 如果尚未检查过，则检查多字节字符是否有 
			 * 效。验证这一点很重要，因为无效的多字节 
			 * 字符可能被用来"跳过"引号字符，比如在         
			 * 逐个字符地进行解析的时候。  
			 *
			 * 当首次遇到任何多字节字符时，会对整个        
			 * 字符串其余部分做一次有效性检查。某些
			 * 编码针对较长的字符串有优化过的实现。      
			 */
			if (!validated_mb)
			{
				if (pg_encoding_verifymbstr(conn->client_encoding, s, remaining)
					!= remaining)
				{
					libpq_append_conn_error(conn, "invalid multibyte character");
					return NULL;
				}
				validated_mb = true;
			}

			/* 调整 s，记住 for 循环会递增它。                */
			s += charlen - 1;
			remaining -= charlen - 1;
		}
	}

	/*
	 * 分配输出缓冲区。防止溢出，以防调用者分配了     
	 * 可用的 size_t 空间的大部分。                 
	 */
	if (add_size_overflow(input_len, num_quotes, &result_size) ||
		add_size_overflow(result_size, 3, &result_size))	/* 两引加 NUL         */
		goto overflow;

	if (!as_ident && num_backslashes > 0)
	{
		if (add_size_overflow(result_size, num_backslashes, &result_size) ||
			add_size_overflow(result_size, 2, &result_size))	/* 为 " E" 前缀 */
			goto overflow;
	}

	result = rp = (char *) malloc(result_size);
	if (rp == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}

	/*
	 * 如果我们转义的常量中含有反斜杠，就使用         
	 * 转义字符串语法，使结果在任一                           
	 * standard_conforming_strings 取值下都正确。此种              
	 * 情况下我们还会输出一个前导空格，以防结果   
	 * 被紧接在标识符之后插入。             
	 */
	if (!as_ident && num_backslashes > 0)
	{
		*rp++ = ' ';
		*rp++ = 'E';
	}

	/* 开引号。   */
	*rp++ = quote_char;

	/*
	 * 可能则走快速路径  
	 *
	 * 我们已验证过输入字符串在当前编码下是格式      
	 * 良好的。如果它不含引号，且在常量转义的情   
	 * 况下也不含反斜杠，那么我们就可以把它直接          
	 * 拷入输出缓冲区，加上必要引号。  
	 *
	 * 否则我们必须重新扫描输入，并逐个字符地  
	 * 处理。    
	 */
	if (num_quotes == 0 && (num_backslashes == 0 || as_ident))
	{
		memcpy(rp, str, input_len);
		rp += input_len;
	}
	else
	{
		s = str;
		for (size_t remaining = input_len; remaining > 0; remaining--, s++)
		{
			if (*s == quote_char || (!as_ident && *s == '\\'))
			{
				*rp++ = *s;
				*rp++ = *s;
			}
			else if (!IS_HIGHBIT_SET(*s))
				*rp++ = *s;
			else
			{
				int			i = pg_encoding_mblen(conn->client_encoding, s);

				while (1)
				{
					*rp++ = *s;
					if (--i == 0)
						break;
					remaining--;
					++s;		/* for 循环将提供最终递增           */
				}
			}
		}
	}

	/* 闭引号和终止 NUL。          */
	*rp++ = quote_char;
	*rp = '\0';

	return result;

overflow:
	libpq_append_conn_error(conn,
							"escaped string size exceeds the maximum allowed (%zu)",
							SIZE_MAX);
	return NULL;
}

char *
PQescapeLiteral(PGconn *conn, const char *str, size_t len)
{
	return PQescapeInternal(conn, str, len, false);
}

char *
PQescapeIdentifier(PGconn *conn, const char *str, size_t len)
{
	return PQescapeInternal(conn, str, len, true);
}

/* bytea 的 HEX 编码支持     */
static const char hextbl[] = "0123456789abcdef";

static const int8 hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static inline char
get_hex(char c)
{
	int			res = -1;

	if (c > 0 && c < 127)
		res = hexlookup[(unsigned char) c];

	return (char) res;
}


/*
 *		PQescapeBytea - 将二进制字符串转换为    
 *		在以 bytea 类型列为目标的 SQL INSERT 语句中   
 *		包含该字符串所需的最小编码。              
 *
 *		可用十六进制或 escape（传统）两种编码。
 *		在 escape 模式下，应用以下转换：              
 *		'\0' == ASCII  0 == \000
 *		'\'' == ASCII 39 == ''
 *		'\\' == ASCII 92 == \\
 *		anything < 0x20, or > 0x7e ---> \ooo
 *										（ooo 为八进制表示）      
 *
 *		若非 std_strings，则输出中所有反斜杠都加倍。       
 */
static unsigned char *
PQescapeByteaInternal(PGconn *conn,
					  const unsigned char *from, size_t from_length,
					  size_t *to_length, bool std_strings, bool use_hex)
{
	const unsigned char *vp;
	unsigned char *rp;
	unsigned char *result;
	size_t		i;
	size_t		len;
	const size_t bslash_len = (std_strings ? 1 : 2);

	/*
	 * 计算转义后的长度，像 PQescapeInternal() 中那样      
	 * 注意溢出问题。后面的代码依赖 bslash_len            
	 * 是个小常数，这样小的加法和乘法就不需要              
	 * 溢出检查。     
	 *
	 * 从空串开始，它有 1 个字符（'\0'）。    
	 */
	len = 1;

	if (use_hex)
	{
		/* 前置 "\x" 加倍每输入字符。             */
		if (add_size_overflow(len, bslash_len + 1, &len) ||
			add_size_overflow(len, from_length, &len) ||
			add_size_overflow(len, from_length, &len))
			goto overflow;
	}
	else
	{
		vp = from;
		for (i = from_length; i > 0; i--, vp++)
		{
			if (*vp < 0x20 || *vp > 0x7e)
			{
				if (add_size_overflow(len, bslash_len + 3, &len))	/*八进制\ooo */
					goto overflow;
			}
			else if (*vp == '\'')
			{
				if (add_size_overflow(len, 2, &len))	/* 加倍引号      */
					goto overflow;
			}
			else if (*vp == '\\')
			{
				if (add_size_overflow(len, bslash_len * 2, &len))	/* 加倍反斜杠       */
					goto overflow;
			}
			else
			{
				if (add_size_overflow(len, 1, &len))
					goto overflow;
			}
		}
	}

	*to_length = len;
	rp = result = (unsigned char *) malloc(len);
	if (rp == NULL)
	{
		if (conn)
			libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}

	if (use_hex)
	{
		if (!std_strings)
			*rp++ = '\\';
		*rp++ = '\\';
		*rp++ = 'x';
	}

	vp = from;
	for (i = from_length; i > 0; i--, vp++)
	{
		unsigned char c = *vp;

		if (use_hex)
		{
			*rp++ = hextbl[(c >> 4) & 0xF];
			*rp++ = hextbl[c & 0xF];
		}
		else if (c < 0x20 || c > 0x7e)
		{
			if (!std_strings)
				*rp++ = '\\';
			*rp++ = '\\';
			*rp++ = (c >> 6) + '0';
			*rp++ = ((c >> 3) & 07) + '0';
			*rp++ = (c & 07) + '0';
		}
		else if (c == '\'')
		{
			*rp++ = '\'';
			*rp++ = '\'';
		}
		else if (c == '\\')
		{
			if (!std_strings)
			{
				*rp++ = '\\';
				*rp++ = '\\';
			}
			*rp++ = '\\';
			*rp++ = '\\';
		}
		else
			*rp++ = c;
	}
	*rp = '\0';

	return result;

overflow:
	if (conn)
		libpq_append_conn_error(conn,
								"escaped bytea size exceeds the maximum allowed (%zu)",
								SIZE_MAX);
	return NULL;
}

unsigned char *
PQescapeByteaConn(PGconn *conn,
				  const unsigned char *from, size_t from_length,
				  size_t *to_length)
{
	if (!conn)
		return NULL;

	if (conn->cmd_queue_head == NULL)
		pqClearConnErrorState(conn);

	return PQescapeByteaInternal(conn, from, from_length, to_length,
								 conn->std_strings,
								 (conn->sversion >= 90000));
}

unsigned char *
PQescapeBytea(const unsigned char *from, size_t from_length, size_t *to_length)
{
	return PQescapeByteaInternal(NULL, from, from_length, to_length,
								 static_std_strings,
								 false /* 无法用 hex */ );
}


#define ISFIRSTOCTDIGIT(CH) ((CH) >= '0' && (CH) <= '3')
#define ISOCTDIGIT(CH) ((CH) >= '0' && (CH) <= '7')
#define OCTVAL(CH) ((CH) - '0')

/*
 *		PQunescapeBytea - 将以 null 结尾的字符串表示的           
 *		bytea（strtext）转换为二进制并填入缓冲区。它    
 *		返回指向缓冲区的指针（出错则为 NULL），     
 *		并在 retbuflen 中给出缓冲区的大小。该指针随后 
 *		可作 PQfreemem 的参数。      
 *
 *		会进行以下转换：               
 *		\\	 == ASCII 92 == \
 *		\ooo == 值为 ooo 的字节（ooo 为八进制数）    
 *		\x == x（x 是其余转换未匹配到的任何字符）             
 */
unsigned char *
PQunescapeBytea(const unsigned char *strtext, size_t *retbuflen)
{
	size_t		strtextlen,
				buflen;
	unsigned char *buffer,
			   *tmpbuf;
	size_t		i,
				j;

	if (strtext == NULL)
		return NULL;

	strtextlen = strlen((const char *) strtext);

	if (strtext[0] == '\\' && strtext[1] == 'x')
	{
		const unsigned char *s;
		unsigned char *p;

		buflen = (strtextlen - 2) / 2;
		/* 避不可移植 malloc(0)  */
		buffer = (unsigned char *) malloc(buflen > 0 ? buflen : 1);
		if (buffer == NULL)
			return NULL;

		s = strtext + 2;
		p = buffer;
		while (*s)
		{
			char		v1,
						v2;

			/*
			 * 错误的输入被静默忽略。注意这包括       
			 * 十六进制对间空白，byteain 允许这种空白。  
			 */
			v1 = get_hex(*s++);
			if (!*s || v1 == (char) -1)
				continue;
			v2 = get_hex(*s++);
			if (v2 != (char) -1)
				*p++ = (v1 << 4) | v2;
		}

		buflen = p - buffer;
	}
	else
	{
		/*
		 * 输入长度是输出的最大长度，加一以避免输入 
		 * 为零时出现不可移植的 malloc(0)。  
		 */
		buffer = (unsigned char *) malloc(strtextlen + 1);
		if (buffer == NULL)
			return NULL;

		for (i = j = 0; i < strtextlen;)
		{
			switch (strtext[i])
			{
				case '\\':
					i++;
					if (strtext[i] == '\\')
						buffer[j++] = strtext[i++];
					else
					{
						if ((ISFIRSTOCTDIGIT(strtext[i])) &&
							(ISOCTDIGIT(strtext[i + 1])) &&
							(ISOCTDIGIT(strtext[i + 2])))
						{
							int			byte;

							byte = OCTVAL(strtext[i++]);
							byte = (byte << 3) + OCTVAL(strtext[i++]);
							byte = (byte << 3) + OCTVAL(strtext[i++]);
							buffer[j++] = byte;
						}
					}

					/*
					 * 注意：若 '\' 后跟的不是可识别的转义    
					 * 序列，就会在只推进 i 而未做任何事      
					 * 的情况下循环。因此那个内容会在下一轮 
					 * 循环时作为普通数据输出。极端情况：  
					 * 字符串末尾的 '\' 会被直接丢弃。      
					 */
					break;

				default:
					buffer[j++] = strtext[i++];
					break;
			}
		}
		buflen = j;				/* buflen 是去转义数据的长度        */
	}

	/* 缩小缓冲区使其不比必要的大          */
	/* +1 避免 buflen==0 时的不可移植行为 */
	tmpbuf = realloc(buffer, buflen + 1);

	/* 只有非常脑残的 realloc 才会失败，但...                */
	if (!tmpbuf)
	{
		free(buffer);
		return NULL;
	}

	*retbuflen = buflen;
	return tmpbuf;
}
