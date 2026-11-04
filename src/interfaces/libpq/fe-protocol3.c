/*-------------------------------------------------------------------------
 *
 * fe-protocol3.c
 *	  functions that are specific to frontend/backend protocol version 3
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-protocol3.c
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
#include <netinet/tcp.h>
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "mb/pg_wchar.h"
#include "port/pg_bswap.h"

/*
 * 本宏列出可能为"长"（超过几千字节）的后端          
 * 消息类型。             
 */
#define VALID_LONG_MESSAGE_TYPE(id) \
	((id) == 'T' || (id) == 'D' || (id) == 'd' || (id) == 'V' || \
	 (id) == 'E' || (id) == 'N' || (id) == 'A')


static void handleSyncLoss(PGconn *conn, char id, int msgLength);
static int	getRowDescriptions(PGconn *conn, int msgLength);
static int	getParamDescriptions(PGconn *conn, int msgLength);
static int	getAnotherTuple(PGconn *conn, int msgLength);
static int	getParameterStatus(PGconn *conn);
static int	getNotify(PGconn *conn);
static int	getCopyStart(PGconn *conn, ExecStatusType copytype);
static int	getReadyForQuery(PGconn *conn);
static void reportErrorPosition(PQExpBuffer msg, const char *query,
								int loc, int encoding);
static size_t build_startup_packet(const PGconn *conn, char *packet,
								   const PQEnvironmentOption *options);


/*
 * parseInput：如果适当，解析来自后端的输入数据，
 * 直到输入耗尽或达到停止状态。
 * 注意，此函数不会尝试从后端读取更多数据。
                        */
void
pqParseInput3(PGconn *conn)
{
	char		id;
	int			msgLength;
	int			avail;

	/*
	 * 循环解析缓冲区中可用的连续完整消息。
	              */
	for (;;)
	{
		/*
		 * 尝试读取一条消息。首先获取类型代码和长度，   
		 * 不足则返回。 
		 */
		conn->inCursor = conn->inStart;
		if (pqGetc(&id, conn))
			return;
		if (pqGetInt(&msgLength, 4, conn))
			return;

		/*
		 * 在此处验证消息类型/长度。长度小于 4 必然        
		 * 已损坏。只有少数消息类型的较大长度才可          
		 * 采信。     
		 */
		if (msgLength < 4)
		{
			handleSyncLoss(conn, id, msgLength);
			return;
		}
		if (msgLength > 30000 && !VALID_LONG_MESSAGE_TYPE(id))
		{
			handleSyncLoss(conn, id, msgLength);
			return;
		}

		/*
		 * 消息体未全到达则无法处理。          
		 */
		msgLength -= 4;
		avail = conn->inEnd - conn->inCursor;
		if (avail < msgLength)
		{
			/*
			 * 返回前，若需要则扩大输入缓冲区，以容纳   
			 * 整个消息。这比留给 pqReadData 更好，     
			 * 因为消息较大时能避免多轮 realloc()；而且，  
			 * 若无法把缓冲区弄得足够大，我们还能实现     
			 * 一个合理的恢复策略。                           
			 *        
			 */
			if (pqCheckInBufferSpace(conn->inCursor + (size_t) msgLength,
									 conn))
			{
				/*
				 * XXX 应补充更好的恢复代码...计划是利用其  
				 * 长度跳过该消息，然后报告一个错误。目前  
				 * 只好把它当作失去同步来处理(很可能确实   
				 * 如此!)  
				 */
				handleSyncLoss(conn, id, msgLength);
			}
			return;
		}

		/*
		 * NOTIFY 和 NOTICE 消息可出现在任何状态；总是         
		 * 立即处理。 
		 *
		 * 大多数其他消息只应在 BUSY 状态下处理。（特别  
		 * 是，在 READY 状态下我们会暂停解析，直到应用取
		 * 走当前 PGresult。）                       
		 *
		 * 然而，如果状态是 IDLE，那就麻烦了；我们需要以 
		 * 某种方式处理意外消息。   
		 *
		 * ParameterStatus（'S'）消息是特例：IDLE 状态下           
		 * 必须立即处理（若后端因 SIGHUP 而从配置文件        
		 * 采纳新值时就会出现这种情况），否则等到 BUSY  
		 *  状态。 
		 */
		if (id == 'A')
		{
			if (getNotify(conn))
				return;
		}
		else if (id == 'N')
		{
			if (pqGetErrorNotice3(conn, false))
				return;
		}
		else if (conn->asyncStatus != PGASYNC_BUSY)
		{
			/* 若非 IDLE 状态则等待 ...  */
			if (conn->asyncStatus != PGASYNC_IDLE)
				return;

			/*
			 * IDLE 状态下的意外消息；需要以某种方式恢复。
			 * ERROR 消息使用通知处理器处理；
			 * ParameterStatus 正常处理；其他任何消息在显示
			 * 适当的警告通知后直接丢弃。
			 * （ERROR 很可能是后端在告诉我们为什么
			 * 即将关闭连接，所以我们不想直接
			 * 丢弃它...）
			                                                    */
			if (id == 'E')
			{
				if (pqGetErrorNotice3(conn, false /* 视为通知    */ ))
					return;
			}
			else if (id == 'S')
			{
				if (getParameterStatus(conn))
					return;
			}
			else
			{
				/* 其他情况均属意外，简要跳过               */
				pqInternalNotice(&conn->noticeHooks,
								 "message type 0x%02x arrived from server while idle",
								 id);
				/* 丢弃意外消息             */
				conn->inCursor += msgLength;
			}
		}
		else
		{
			/*
			 * BUSY 状态下可处理所有内容。
			    */
			switch (id)
			{
				case 'C':		/* 命令完成     */
					if (pqGets(&conn->workBuffer, conn))
						return;
					if (!pgHavePendingResult(conn))
					{
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_COMMAND_OK);
						if (!conn->result)
						{
							libpq_append_conn_error(conn, "out of memory");
							pqSaveErrorResult(conn);
						}
					}
					if (conn->result)
						strlcpy(conn->result->cmdStatus, conn->workBuffer.data,
								CMDSTATUS_LEN);
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'E':		/* 错误返回 */
					if (pqGetErrorNotice3(conn, true))
						return;
					conn->asyncStatus = PGASYNC_READY;
					break;
				case 'Z':		/* 同步响应，后端准备好接受新
								 * 查询*/
					if (getReadyForQuery(conn))
						return;
					if (conn->pipelineStatus != PQ_PIPELINE_OFF)
					{
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_PIPELINE_SYNC);
						if (!conn->result)
						{
							libpq_append_conn_error(conn, "out of memory");
							pqSaveErrorResult(conn);
						}
						else
						{
							conn->pipelineStatus = PQ_PIPELINE_ON;
							conn->asyncStatus = PGASYNC_READY;
						}
					}
					else
					{
						/* 推进命令队列，设为空闲         */
						pqCommandQueueAdvance(conn, true, false);
						conn->asyncStatus = PGASYNC_IDLE;
					}
					break;
				case 'I':		/* 空查询   */
					if (!pgHavePendingResult(conn))
					{
						conn->result = PQmakeEmptyPGresult(conn,
														   PGRES_EMPTY_QUERY);
						if (!conn->result)
						{
							libpq_append_conn_error(conn, "out of memory");
							pqSaveErrorResult(conn);
						}
					}
					conn->asyncStatus = PGASYNC_READY;
					break;
				case '1':		/* 解析完成   */
					/* 若执行 PQprepare 则已完成；否则忽略   */
					if (conn->cmd_queue_head &&
						conn->cmd_queue_head->queryclass == PGQUERY_PREPARE)
					{
						if (!pgHavePendingResult(conn))
						{
							conn->result = PQmakeEmptyPGresult(conn,
															   PGRES_COMMAND_OK);
							if (!conn->result)
							{
								libpq_append_conn_error(conn, "out of memory");
								pqSaveErrorResult(conn);
							}
						}
						conn->asyncStatus = PGASYNC_READY;
					}
					break;
				case '2':		/* 绑定完成  */
				case '3':		/* 关闭完成   */
					/* 这些消息类型无需处理        */
					break;
				case 'S':		/* 参数状态     */
					if (getParameterStatus(conn))
						return;
					break;
				case 'K':		/* 来自后端的密钥数据      */

					/*
				 * 这仅在启动期间预期发生，但作为
				 * 主循环的一部分处理也同样简单。
				 * 保存数据并继续处理。
				                            */
					if (pqGetInt(&(conn->be_pid), 4, conn))
						return;
					if (pqGetInt(&(conn->be_key), 4, conn))
						return;
					break;
				case 'T':		/* 行描述       */
					if (conn->error_result ||
						(conn->result != NULL &&
						 conn->result->resultStatus == PGRES_FATAL_ERROR))
					{
						/*
				 * 由于某种原因我们已经失败了。只需丢弃
				 * 数据，直到到达查询末尾。
				             */
						conn->inCursor += msgLength;
					}
					else if (conn->result == NULL ||
							 (conn->cmd_queue_head &&
							  conn->cmd_queue_head->queryclass == PGQUERY_DESCRIBE))
					{
						/* 查询序列中的首个 'T'  */
						if (getRowDescriptions(conn, msgLength))
							return;
					}
					else
					{
						/*
				 * 新的 'T' 消息被视为另一个 PGresult 的
				 * 开始。（不清楚当前的
				 * 后端是否真的可能发生这种情况。）我们停止
				 * 解析，直到应用程序接受当前
				 * 结果。
				                         */
						conn->asyncStatus = PGASYNC_READY;
						return;
					}
					break;
				case 'n':		/*无数据*/

					/*
				 * NoData 表示我们不会看到
				 * RowDescription 消息，因为所查询
				 * 的语句或门户不返回行。
				 *
				 * 如果我们正在进行 Describe，我们必须向
				 * 客户端返回一些内容，因此设置 COMMAND_OK 结果，
				 * 而不是 PGRES_TUPLES_OK。否则我们可以
				 * 直接忽略此消息。
				                             */
					if (conn->cmd_queue_head &&
						conn->cmd_queue_head->queryclass == PGQUERY_DESCRIBE)
					{
						if (!pgHavePendingResult(conn))
						{
							conn->result = PQmakeEmptyPGresult(conn,
															   PGRES_COMMAND_OK);
							if (!conn->result)
							{
								libpq_append_conn_error(conn, "out of memory");
								pqSaveErrorResult(conn);
							}
						}
						conn->asyncStatus = PGASYNC_READY;
					}
					break;
				case 't':		/* 参数描述          */
					if (getParamDescriptions(conn, msgLength))
						return;
					break;
				case 'D':		/*数据行 */
					if (conn->result != NULL &&
						conn->result->resultStatus == PGRES_TUPLES_OK)
					{
						/* 读取正常查询响应的另一个元组    */
						if (getAnotherTuple(conn, msgLength))
							return;
					}
					else if (conn->error_result ||
							 (conn->result != NULL &&
							  conn->result->resultStatus == PGRES_FATAL_ERROR))
					{
						/*
				 * 由于某种原因我们已经失败了。只需丢弃
				 * 元组，直到到达查询末尾。
				           */
						conn->inCursor += msgLength;
					}
					else
					{
						/* 设置在查询结束时报告错误   */
						libpq_append_conn_error(conn, "server sent data (\"D\" message) without prior row description (\"T\" message)");
						pqSaveErrorResult(conn);
						/* 丢弃意外消息             */
						conn->inCursor += msgLength;
					}
					break;
				case 'G':		/*开始 Copy In */
					if (getCopyStart(conn, PGRES_COPY_IN))
						return;
					conn->asyncStatus = PGASYNC_COPY_IN;
					break;
				case 'H':		/*开始 Copy Out */
					if (getCopyStart(conn, PGRES_COPY_OUT))
						return;
					conn->asyncStatus = PGASYNC_COPY_OUT;
					conn->copy_already_done = 0;
					break;
				case 'W':		/*开始 Copy Both */
					if (getCopyStart(conn, PGRES_COPY_BOTH))
						return;
					conn->asyncStatus = PGASYNC_COPY_BOTH;
					conn->copy_already_done = 0;
					break;
				case 'd':		/* Copy Data */

					/*
				 * 如果看到 Copy Data，只是静默丢弃。
				 * 这仅在应用程序过早退出 COPY OUT 模式时
				 * 才会发生。
				  */
					conn->inCursor += msgLength;
					break;
				case 'c':		/* Copy Done */

					/*
				 * 如果看到 Copy Done，只是静默丢弃。
				 * 这是 PQendcopy 期间的正常情况。我们将继续
				 * 消费数据，期望看到 COPY 命令的
				 * command-complete。
				             */
					break;
				default:
					libpq_append_conn_error(conn, "unexpected response from server; first received character was \"%c\"", id);
					/* 构建一个包含错误消息的错误结果   */
					pqSaveErrorResult(conn);
					/* 不确定是否还有更多，转到就绪状态   */
					conn->asyncStatus = PGASYNC_READY;
					/* 丢弃意外消息             */
					conn->inCursor += msgLength;
					break;
			}					/* 按协议字符切换        */
		}
		/* 成功消费此消息              */
		if (conn->inCursor == conn->inStart + 5 + msgLength)
		{
			/* 跟踪来自服务器的消息 */
			if (conn->Pfdebug)
				pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

			/* 正常情况：解析与指定长度一致        */
			conn->inStart = conn->inCursor;
		}
		else
		{
			/* 出问题——报告 */
			libpq_append_conn_error(conn, "message contents do not agree with length in message type \"%c\"", id);
			/* 构建一个包含错误消息的错误结果   */
			pqSaveErrorResult(conn);
			conn->asyncStatus = PGASYNC_READY;
			/* 信任指定的消息长度作为跳过量         */
			conn->inStart += 5 + msgLength;
		}
	}
}

/*
 * handleSyncLoss：清理消息边界同步丢失后的状态
 *
 * 除了放弃连接，这里我们能做的事情不多。
               */
static void
handleSyncLoss(PGconn *conn, char id, int msgLength)
{
	libpq_append_conn_error(conn, "lost synchronization with server: got message type \"%c\", length %d",
							id, msgLength);
	/* 构建一个包含错误消息的错误结果   */
	pqSaveErrorResult(conn);
	conn->asyncStatus = PGASYNC_READY;	/* 退出 PQgetResult 等待循环   */
	/* 由于我们要放弃处理，刷新输入数据        */
	pqDropConnection(conn, true);
	conn->status = CONNECTION_BAD;	/* 不再有到后端的连接   */
}

/*
 * parseInput 子例程，用于读取 'T'（行描述）消息。
 * 我们将构建一个新的 PGresult 结构（除非是为预备语句的
 * Describe 命令调用），包含属性数据。
 * 返回值：0 表示成功处理消息，EOF 表示暂停解析
 * （后一种情况目前未实际使用）。
                   */
static int
getRowDescriptions(PGconn *conn, int msgLength)
{
	PGresult   *result;
	int			nfields;
	const char *errmsg;
	int			i;

	/*
	 * 为预备语句执行 Describe 时，
	 * getParamDescriptions 已创建 PGresult，
	 * 我们将数据填入其中。否则创建新的空 PGresult。
	                                           */
	if (!conn->cmd_queue_head ||
		(conn->cmd_queue_head &&
		 conn->cmd_queue_head->queryclass == PGQUERY_DESCRIBE))
	{
		if (conn->result)
			result = conn->result;
		else
			result = PQmakeEmptyPGresult(conn, PGRES_COMMAND_OK);
	}
	else
		result = PQmakeEmptyPGresult(conn, PGRES_TUPLES_OK);
	if (!result)
	{
		errmsg = NULL;			/* 表示"内存不足"，见下文 */
		goto advance_and_error;
	}

	/* parseInput 已读取 'T' 标签和消息长度。         */
	/* 接下来的两个字节是字段数        */
	if (pqGetInt(&(result->numAttributes), 2, conn))
	{
		/* 此处不应耗尽数据，因此报错         */
		errmsg = libpq_gettext("insufficient data in \"T\" message");
		goto advance_and_error;
	}
	nfields = result->numAttributes;

	/* 为属性描述符分配空间               */
	if (nfields > 0)
	{
		result->attDescs = (PGresAttDesc *)
			pqResultAlloc(result, nfields * sizeof(PGresAttDesc), true);
		if (!result->attDescs)
		{
			errmsg = NULL;		/* 表示"内存不足"，见下文 */
			goto advance_and_error;
		}
		MemSet(result->attDescs, 0, nfields * sizeof(PGresAttDesc));
	}

	/* 所有列均为二进制时 result->binary 才为真  */
	result->binary = (nfields > 0) ? 1 : 0;

	/*取类型信息*/
	for (i = 0; i < nfields; i++)
	{
		int			tableid;
		int			columnid;
		int			typid;
		int			typlen;
		int			atttypmod;
		int			format;

		if (pqGets(&conn->workBuffer, conn) ||
			pqGetInt(&tableid, 4, conn) ||
			pqGetInt(&columnid, 2, conn) ||
			pqGetInt(&typid, 4, conn) ||
			pqGetInt(&typlen, 2, conn) ||
			pqGetInt(&atttypmod, 4, conn) ||
			pqGetInt(&format, 2, conn))
		{
			/* 此处不应耗尽数据，因此报错         */
			errmsg = libpq_gettext("insufficient data in \"T\" message");
			goto advance_and_error;
		}

		/*
		 * pqGetInt 将两字节整数视为无符号，需将
		 * 结果强制转为有符号形式。
		           */
		columnid = (int) ((int16) columnid);
		typlen = (int) ((int16) typlen);
		format = (int) ((int16) format);

		result->attDescs[i].name = pqResultStrdup(result,
												  conn->workBuffer.data);
		if (!result->attDescs[i].name)
		{
			errmsg = NULL;		/* 表示"内存不足"，见下文 */
			goto advance_and_error;
		}
		result->attDescs[i].tableid = tableid;
		result->attDescs[i].columnid = columnid;
		result->attDescs[i].format = format;
		result->attDescs[i].typid = typid;
		result->attDescs[i].typlen = typlen;
		result->attDescs[i].atttypmod = atttypmod;

		if (format != 1)
			result->binary = 0;
	}

	/* 成功   */
	conn->result = result;

	/*
	 * 若正执行 Describe，我们已完成，
	 * 准备将结果传回客户端。
	            */
	if ((!conn->cmd_queue_head) ||
		(conn->cmd_queue_head &&
		 conn->cmd_queue_head->queryclass == PGQUERY_DESCRIBE))
	{
		conn->asyncStatus = PGASYNC_READY;
		return 0;
	}

	/*
	 * 我们可以在此处为新结果集执行额外的设置，
	 * 但目前无需做任何其他事。
	      */

	/* 完成。       */
	return 0;

advance_and_error:
	/* 如有则丢弃未保存结果 */
	if (result && result != conn->result)
		PQclear(result);

	/*
	 * 将部分构建的结果替换为错误结果。首先
	 * 丢弃旧结果以尝试回收一些内存。
	                    */
	pqClearAsyncResult(conn);

	/*
	 * 如果前面的代码未提供错误消息，则假定其意为  
	 * "内存不足"。这种特例的好处在于，先释放旧结果    
	 * 能大大提高 gettext() 成功提供翻译                         
	 * 的机会。                            
	 */
	if (!errmsg)
		errmsg = libpq_gettext("out of memory for query result");

	appendPQExpBuffer(&conn->errorMessage, "%s\n", errmsg);
	pqSaveErrorResult(conn);

	/*
	 * 将消息显示为完全消费，否则 pqParseInput3 会
	 * 用相关投诉覆盖我们的错误。
	            */
	conn->inCursor = conn->inStart + 5 + msgLength;

	/*
	 * 返回零以允许输入解析继续进行。后续的 "D"
	 * 消息将被忽略，直到我们到达数据末尾，因为错误
	 * 结果已设置。
	               */
	return 0;
}

/*
 * parseInput 子例程，用于读取 't'（ParameterDescription）消息。
 * 我们将构建一个包含参数数据的新 PGresult 结构。
 * 返回值：0 表示成功处理消息，EOF 表示暂停解析
 * （后一种情况目前未实际使用）。
      */
static int
getParamDescriptions(PGconn *conn, int msgLength)
{
	PGresult   *result;
	const char *errmsg = NULL;	/* 表示"内存不足"，见下文 */
	int			nparams;
	int			i;

	result = PQmakeEmptyPGresult(conn, PGRES_COMMAND_OK);
	if (!result)
		goto advance_and_error;

	/* parseInput 已读取 't' 标签和消息长度。         */
	/* 接下来的两个字节是参数数量         */
	if (pqGetInt(&(result->numParameters), 2, conn))
		goto not_enough_data;
	nparams = result->numParameters;

	/* 为参数描述符分配空间               */
	if (nparams > 0)
	{
		result->paramDescs = (PGresParamDesc *)
			pqResultAlloc(result, nparams * sizeof(PGresParamDesc), true);
		if (!result->paramDescs)
			goto advance_and_error;
		MemSet(result->paramDescs, 0, nparams * sizeof(PGresParamDesc));
	}

	/* 获取参数信息 */
	for (i = 0; i < nparams; i++)
	{
		int			typid;

		if (pqGetInt(&typid, 4, conn))
			goto not_enough_data;
		result->paramDescs[i].typid = typid;
	}

	/* 成功   */
	conn->result = result;

	return 0;

not_enough_data:
	errmsg = libpq_gettext("insufficient data in \"t\" message");

advance_and_error:
	/* 如有则丢弃未保存结果 */
	if (result && result != conn->result)
		PQclear(result);

	/*
	 * 将部分构建的结果替换为错误结果。首先
	 * 丢弃旧结果以尝试回收一些内存。
	                    */
	pqClearAsyncResult(conn);

	/*
	 * 如果前面的代码未提供错误消息，则假定其意为  
	 * "内存不足"。这种特例的好处在于，先释放旧结果    
	 * 能大大提高 gettext() 成功提供翻译                         
	 * 的机会。                            
	 */
	if (!errmsg)
		errmsg = libpq_gettext("out of memory");
	appendPQExpBuffer(&conn->errorMessage, "%s\n", errmsg);
	pqSaveErrorResult(conn);

	/*
	 * 将消息显示为完全消费，否则 pqParseInput3 会
	 * 用相关投诉覆盖我们的错误。
	            */
	conn->inCursor = conn->inStart + 5 + msgLength;

	/*
	 * 返回零以允许输入解析继续进行。本质上，我们已将
	 * COMMAND_OK 结果替换为错误结果，但由于这不影响
	 * 协议状态，所以没问题。
	                */
	return 0;
}

/*
 * parseInput 子例程，用于读取 'D'（行数据）消息。
 * 我们用列指针填充 rowbuf，然后调用行处理器。
 * 返回值：0 表示成功处理消息，EOF 表示暂停解析
 * （后一种情况目前未实际使用）。
         */
static int
getAnotherTuple(PGconn *conn, int msgLength)
{
	PGresult   *result = conn->result;
	int			nfields = result->numAttributes;
	const char *errmsg;
	PGdataValue *rowbuf;
	int			tupnfields;		/* 元组字段数     */
	int			vlen;			/* 当前字段值的长度          */
	int			i;

	/* 获取字段计数并确保与预期一致            */
	if (pqGetInt(&tupnfields, 2, conn))
	{
		/* 此处不应耗尽数据，因此报错         */
		errmsg = libpq_gettext("insufficient data in \"D\" message");
		goto advance_and_error;
	}

	if (tupnfields != nfields)
	{
		errmsg = libpq_gettext("unexpected field count in \"D\" message");
		goto advance_and_error;
	}

	/* 必要时调整行缓冲区 */
	rowbuf = conn->rowBuf;
	if (nfields > conn->rowBufLen)
	{
		rowbuf = (PGdataValue *) realloc(rowbuf,
										 nfields * sizeof(PGdataValue));
		if (!rowbuf)
		{
			errmsg = NULL;		/* 表示"内存不足"，见下文 */
			goto advance_and_error;
		}
		conn->rowBuf = rowbuf;
		conn->rowBufLen = nfields;
	}

	/* 扫描各字段 */
	for (i = 0; i < nfields; i++)
	{
		/* 获取值的长度   */
		if (pqGetInt(&vlen, 4, conn))
		{
			/* 此处不应耗尽数据，因此报错         */
			errmsg = libpq_gettext("insufficient data in \"D\" message");
			goto advance_and_error;
		}
		rowbuf[i].len = vlen;

		/*
		 * 即使值为 NULL，rowbuf[i].value 也始终指向数据缓冲区中
		 * 的下一个地址。这允许行处理器更轻松地
		 * 估算数据大小。
		              */
		rowbuf[i].value = conn->inBuffer + conn->inCursor;

		/* 跳过数据值          */
		if (vlen > 0)
		{
			if (pqSkipnchar(vlen, conn))
			{
				/* 此处不应耗尽数据，因此报错         */
				errmsg = libpq_gettext("insufficient data in \"D\" message");
				goto advance_and_error;
			}
		}
	}

	/* 处理收集的行        */
	errmsg = NULL;
	if (pqRowProcessor(conn, &errmsg))
		return 0;				/* 正常，成功退出   */

	/* pqRowProcessor failed, fall through to report it */

advance_and_error:

	/*
	 * 将部分构建的结果替换为错误结果。首先
	 * 丢弃旧结果以尝试回收一些内存。
	                    */
	pqClearAsyncResult(conn);

	/*
	 * 如果前面的代码未提供错误消息，则假定其意为  
	 * "内存不足"。这种特例的好处在于，先释放旧结果    
	 * 能大大提高 gettext() 成功提供翻译                         
	 * 的机会。                            
	 */
	if (!errmsg)
		errmsg = libpq_gettext("out of memory for query result");

	appendPQExpBuffer(&conn->errorMessage, "%s\n", errmsg);
	pqSaveErrorResult(conn);

	/*
	 * 将消息显示为完全消费，否则 pqParseInput3 会
	 * 用相关投诉覆盖我们的错误。
	            */
	conn->inCursor = conn->inStart + 5 + msgLength;

	/*
	 * 返回零以允许输入解析继续进行。后续的 "D"
	 * 消息将被忽略，直到我们到达数据末尾，因为错误
	 * 结果已设置。
	               */
	return 0;
}


/*
 * 尝试读取 Error 或 Notice 响应消息。
 * 这在多个地方都可能发生，因此我们将其提取为子例程。
 * 入口：'E' 或 'N' 消息类型和长度已消费。
 * 出口：成功消费消息返回 0。
 *		 数据不足返回 EOF。
                                    */
int
pqGetErrorNotice3(PGconn *conn, bool isError)
{
	PGresult   *res = NULL;
	bool		have_position = false;
	PQExpBufferData workBuf;
	char		id;

	/* 流水线模式下为其设置错误指示符   */
	if (isError && conn->pipelineStatus != PQ_PIPELINE_OFF)
		conn->pipelineStatus = PQ_PIPELINE_ABORTED;

	/*
	 * 如果这是错误消息，抢先清除任何不完整的查询
	 * 结果。我们反正下面也会丢弃它，在收集错误
	 * 之前释放它可能会避免内存不足。
	                               */
	if (isError)
		pqClearAsyncResult(conn);

	/*
	 * 由于字段可能相当长，我们创建一个临时的
	 * PQExpBuffer 而不是使用 conn->workBuffer。workBuffer
	 * 用于预期较短的内容。我们也不应使用
	 * conn->errorMessage，因为这可能只是一个通知。
	                            */
	initPQExpBuffer(&workBuf);

	/*
	 * 创建一个 PGresult 来保存累积的字段。我们临时对结果状态
	 * 撒谎，以便 PQmakeEmptyPGresult 不会无用地复制
	 * conn->errorMessage。
	 *
	 * 注意：此分配可能失败，如果内存不足。函数的其余部分
	 * 优雅地处理该情况，我们仍尝试将错误消息设置为
	 * 连接的错误消息。
	                       */
	res = PQmakeEmptyPGresult(conn, PGRES_EMPTY_QUERY);
	if (res)
		res->resultStatus = isError ? PGRES_FATAL_ERROR : PGRES_NONFATAL_ERROR;

	/*
	 * 读取字段并保存到 res。
	 *
	 * 同时将 SQLSTATE 保存到 conn->last_sqlstate，
	 * 并记录是否看到 PG_DIAG_STATEMENT_POSITION 字段。
	        */
	for (;;)
	{
		if (pqGetc(&id, conn))
			goto fail;
		if (id == '\0')
			break;				/* 找到终止符  */
		if (pqGets(&workBuf, conn))
			goto fail;
		pqSaveMessageField(res, id, workBuf.data);
		if (id == PG_DIAG_SQLSTATE)
			strlcpy(conn->last_sqlstate, workBuf.data,
					sizeof(conn->last_sqlstate));
		else if (id == PG_DIAG_STATEMENT_POSITION)
			have_position = true;
	}

	/*
	 * 也将活动查询文本（若有）保存到 res；但仅
	 * 在可能需要错误游标显示时才这样做，即仅有
	 * PG_DIAG_STATEMENT_POSITION 字段时。
	                    */
	if (have_position && res && conn->cmd_queue_head && conn->cmd_queue_head->query)
		res->errQuery = pqResultStrdup(res, conn->cmd_queue_head->query);

	/*
	 * 现在为 PQresultErrorMessage 构建"总体"错误消息。
	    */
	resetPQExpBuffer(&workBuf);
	pqBuildErrorMessage3(&workBuf, res, conn->verbosity, conn->show_context);

	/*
	 * 将错误保存为当前异步结果，或仅发出通知。
	        */
	if (isError)
	{
		pqClearAsyncResult(conn);	/* 冗余但安全        */
		if (res)
		{
			pqSetResultError(res, &workBuf, 0);
			conn->result = res;
		}
		else
		{
			/* 回退到使用内部错误处理路径                */
			conn->error_result = true;
		}

		if (PQExpBufferDataBroken(workBuf))
			libpq_append_conn_error(conn, "out of memory");
		else
			appendPQExpBufferStr(&conn->errorMessage, workBuf.data);
	}
	else
	{
		/* 如果我们无法分配结果集，只需丢弃 NOTICE         */
		if (res)
		{
			/*
			 * 我们可以在这里稍微取巧，不复制消息。但如果
			 * 我们不幸在填充 workBuf 时内存耗尽，
			 * 像 pqSetResultError 那样插入"内存不足"。
			             */
			if (PQExpBufferDataBroken(workBuf))
				res->errMsg = libpq_gettext("out of memory\n");
			else
				res->errMsg = workBuf.data;
			if (res->noticeHooks.noticeRec != NULL)
				res->noticeHooks.noticeRec(res->noticeHooks.noticeRecArg, res);
			PQclear(res);
		}
	}

	termPQExpBuffer(&workBuf);
	return 0;

fail:
	PQclear(res);
	termPQExpBuffer(&workBuf);
	return EOF;
}

/*
 * 根据给定 PGresult 中的字段构建错误消息，
 * 将其追加到 "msg" 的内容中。
            */
void
pqBuildErrorMessage3(PQExpBuffer msg, const PGresult *res,
					 PGVerbosity verbosity, PGContextVisibility show_context)
{
	const char *val;
	const char *querytext = NULL;
	int			querypos = 0;

	/* 如果我们无法分配 PGresult，只说"内存不足"     */
	if (res == NULL)
	{
		appendPQExpBufferStr(msg, libpq_gettext("out of memory\n"));
		return;
	}

	/*
	 * 如果我们没有任何分解的字段，只返回基本消息。
	 * 这主要适用于我们被给予一个 libpq 生成的错误结果。
	 */
	if (res->errFields == NULL)
	{
		if (res->errMsg && res->errMsg[0])
			appendPQExpBufferStr(msg, res->errMsg);
		else
			appendPQExpBufferStr(msg, libpq_gettext("no error message available\n"));
		return;
	}

	/* 否则从相关字段构建错误消息       */
	val = PQresultErrorField(res, PG_DIAG_SEVERITY);
	if (val)
		appendPQExpBuffer(msg, "%s:  ", val);

	if (verbosity == PQERRORS_SQLSTATE)
	{
		/*
		 * 若有 SQLSTATE 则仅打印它。若没有（服务器
		 * 生成的错误不应发生，但 libpq 生成的错误
		 * 可能发生），回退到 TERSE 格式，比完全不
		 * 打印更好。
		                                                             */
		val = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		if (val)
		{
			appendPQExpBuffer(msg, "%s\n", val);
			return;
		}
		verbosity = PQERRORS_TERSE;
	}

	if (verbosity == PQERRORS_VERBOSE)
	{
		val = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		if (val)
			appendPQExpBuffer(msg, "%s: ", val);
	}
	val = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	if (val)
		appendPQExpBufferStr(msg, val);
	val = PQresultErrorField(res, PG_DIAG_STATEMENT_POSITION);
	if (val)
	{
		if (verbosity != PQERRORS_TERSE && res->errQuery != NULL)
		{
			/* 将位置作为语法游标显示发出  */
			querytext = res->errQuery;
			querypos = atoi(val);
		}
		else
		{
			/* 将位置作为文本附加到主消息发出     */
			/* translator: %s represents a digit string */
			appendPQExpBuffer(msg, libpq_gettext(" at character %s"),
							  val);
		}
	}
	else
	{
		val = PQresultErrorField(res, PG_DIAG_INTERNAL_POSITION);
		if (val)
		{
			querytext = PQresultErrorField(res, PG_DIAG_INTERNAL_QUERY);
			if (verbosity != PQERRORS_TERSE && querytext != NULL)
			{
				/* 将位置作为语法游标显示发出  */
				querypos = atoi(val);
			}
			else
			{
				/* 将位置作为文本附加到主消息发出     */
				/* translator: %s represents a digit string */
				appendPQExpBuffer(msg, libpq_gettext(" at character %s"),
								  val);
			}
		}
	}
	appendPQExpBufferChar(msg, '\n');
	if (verbosity != PQERRORS_TERSE)
	{
		if (querytext && querypos > 0)
			reportErrorPosition(msg, querytext, querypos,
								res->client_encoding);
		val = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
		if (val)
			appendPQExpBuffer(msg, libpq_gettext("DETAIL:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
		if (val)
			appendPQExpBuffer(msg, libpq_gettext("HINT:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_INTERNAL_QUERY);
		if (val)
			appendPQExpBuffer(msg, libpq_gettext("QUERY:  %s\n"), val);
		if (show_context == PQSHOW_CONTEXT_ALWAYS ||
			(show_context == PQSHOW_CONTEXT_ERRORS &&
			 res->resultStatus == PGRES_FATAL_ERROR))
		{
			val = PQresultErrorField(res, PG_DIAG_CONTEXT);
			if (val)
				appendPQExpBuffer(msg, libpq_gettext("CONTEXT:  %s\n"),
								  val);
		}
	}
	if (verbosity == PQERRORS_VERBOSE)
	{
		val = PQresultErrorField(res, PG_DIAG_SCHEMA_NAME);
		if (val)
			appendPQExpBuffer(msg,
							  libpq_gettext("SCHEMA NAME:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_TABLE_NAME);
		if (val)
			appendPQExpBuffer(msg,
							  libpq_gettext("TABLE NAME:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_COLUMN_NAME);
		if (val)
			appendPQExpBuffer(msg,
							  libpq_gettext("COLUMN NAME:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_DATATYPE_NAME);
		if (val)
			appendPQExpBuffer(msg,
							  libpq_gettext("DATATYPE NAME:  %s\n"), val);
		val = PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME);
		if (val)
			appendPQExpBuffer(msg,
							  libpq_gettext("CONSTRAINT NAME:  %s\n"), val);
	}
	if (verbosity == PQERRORS_VERBOSE)
	{
		const char *valf;
		const char *vall;

		valf = PQresultErrorField(res, PG_DIAG_SOURCE_FILE);
		vall = PQresultErrorField(res, PG_DIAG_SOURCE_LINE);
		val = PQresultErrorField(res, PG_DIAG_SOURCE_FUNCTION);
		if (val || valf || vall)
		{
			appendPQExpBufferStr(msg, libpq_gettext("LOCATION:  "));
			if (val)
				appendPQExpBuffer(msg, libpq_gettext("%s, "), val);
			if (valf && vall)	/* 不太可能只有一个    */
				appendPQExpBuffer(msg, libpq_gettext("%s:%s"),
								  valf, vall);
			appendPQExpBufferChar(msg, '\n');
		}
	}
}

/*
 * 向正在构建的错误消息添加错误位置显示。
 *
 * 游标位置以逻辑字符度量；假定查询字符串
 * 使用指定的编码。
                                                */
static void
reportErrorPosition(PQExpBuffer msg, const char *query, int loc, int encoding)
{
#define DISPLAY_SIZE	60		/* 屏幕宽度限制（屏幕列）  */
#define MIN_RIGHT_CUT	10		/* 尽量保持远离行尾           */

	char	   *wquery;
	int			slen,
				cno,
				i,
			   *qidx,
			   *scridx,
				qoffset,
				scroffset,
				ibeg,
				iend,
				loc_line;
	bool		mb_encoding,
				beg_trunc,
				end_trunc;

	/* 将 loc 从 1 基转 0 基；超范围则为空操作      */
	loc--;
	if (loc < 0)
		return;

	/* 需要查询的可写副本       */
	wquery = strdup(query);
	if (wquery == NULL)
		return;					/* 内存不足时静默失败    */

	/*
	 * 一个字符在字符串中可能占据多个物理字节，而且在 
	 * 某些远东字符集中它还可能占据多个屏幕列。我们   
	 * 计算每个逻辑字符的起始字节偏移与屏幕列，     
	 * 并将它们分别存入 qidx[] 与                                   
	 * scridx[] 中。       
	 */

	/*
	 * 我们需要一个安全的分配大小。
	 *
	 * reportErrorPosition() 的唯一调用者是
	 * pqBuildErrorMessage3()；它从 PQresultErrorField() 或
	 * PGcmdQueueEntry 获取查询，这两者都必须适合
	 * conn->inBuffer/outBuffer。因此 slen 适配 int，但我们不能
	 * 假定 (slen * sizeof(int)) 适配 size_t。
	                  */
	slen = strlen(wquery) + 1;
	if (slen > SIZE_MAX / sizeof(int))
	{
		free(wquery);
		return;
	}

	qidx = (int *) malloc(slen * sizeof(int));
	if (qidx == NULL)
	{
		free(wquery);
		return;
	}
	scridx = (int *) malloc(slen * sizeof(int));
	if (scridx == NULL)
	{
		free(qidx);
		free(wquery);
		return;
	}

	/* 如果是单字节编码，我们可以稍微优化  */
	mb_encoding = (pg_encoding_max_length(encoding) != 1);

	/*
	 * 在扫描循环内，cno 是当前字符的逻辑
	 * 编号，qoffset 是其在 wquery 中的偏移量，scroffset 是其起始
	 * 逻辑屏幕列（均从 0 索引）。"loc" 是错误位置
	 * 的逻辑字符编号。我们扫描以确定 loc_line
	 * （包含 loc 的 1 基线号）和 ibeg/iend（包含 loc 的行的
	 * 第一个字符编号和最后+1 字符编号）。注意
	 * qidx[] 和 scridx[] 仅填充到 iend。
	                                                               */
	qoffset = 0;
	scroffset = 0;
	loc_line = 1;
	ibeg = 0;
	iend = -1;					/* -1 表示未设置   */

	for (cno = 0; wquery[qoffset] != '\0'; cno++)
	{
		char		ch = wquery[qoffset];

		qidx[cno] = qoffset;
		scridx[cno] = scroffset;

		/*
		 * 在可写副本中将制表符替换为空格。（稍后我们可能
		 * 想要考虑处理其可变屏幕宽度，但
		 * 不是今天。）
		       */
		if (ch == '\t')
			wquery[qoffset] = ' ';

		/*
		 * 如果是行尾，计数行并标记位置。每个 \r 或 \n
		 * 计为一行，除非 \r \n 一起出现。
		         */
		else if (ch == '\r' || ch == '\n')
		{
			if (cno < loc)
			{
				if (ch == '\r' ||
					cno == 0 ||
					wquery[qidx[cno - 1]] != '\r')
					loc_line++;
				/* 提取起点 = loc 前最后行首              */
				ibeg = cno + 1;
			}
			else
			{
				/*设置提取结束*/
				iend = cno;
				/* 扫描完毕   */
				break;
			}
		}

		/* 前进  */
		if (mb_encoding)
		{
			int			w;

			w = pg_encoding_dsplen(encoding, &wquery[qoffset]);
			/* 非制表控制字符视为宽度 1        */
			if (w <= 0)
				w = 1;
			scroffset += w;
			qoffset += PQmblenBounded(&wquery[qoffset], encoding);
		}
		else
		{
			/* 我们假设宽字符仅存在于多字节编码中    */
			scroffset++;
			qoffset++;
		}
	}
	/* 如果在 loc 之后未找到行尾则修正      */
	if (iend < 0)
	{
		iend = cno;				/* 字符计查询长度 +1  */
		qidx[iend] = qoffset;
		scridx[iend] = scroffset;
	}

	/* 仅在 loc 在计算范围内时打印            */
	if (loc <= cno)
	{
		/* 如果提取的行太长，我们截断它。      */
		beg_trunc = false;
		end_trunc = false;
		if (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
		{
			/*
			 * 如果足够，我们先截断右侧。此代码可能在
			 * 强制执行 MIN_RIGHT_CUT 时偏差一个空格左右，
			 * 如果那里有一个宽字符，但应该没问题。
			  */
			if (scridx[ibeg] + DISPLAY_SIZE >= scridx[loc] + MIN_RIGHT_CUT)
			{
				while (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
					iend--;
				end_trunc = true;
			}
			else
			{
				/* 若离 loc 不太近则截断右侧     */
				while (scridx[loc] + MIN_RIGHT_CUT < scridx[iend])
				{
					iend--;
					end_trunc = true;
				}

				/* 若仍太长则截断左侧      */
				while (scridx[iend] - scridx[ibeg] > DISPLAY_SIZE)
				{
					ibeg++;
					beg_trunc = true;
				}
			}
		}

		/* 在所需端点处截断工作副本      */
		wquery[qidx[iend]] = '\0';

		/* 开始构建完成的消息。       */
		i = msg->len;
		appendPQExpBuffer(msg, libpq_gettext("LINE %d: "), loc_line);
		if (beg_trunc)
			appendPQExpBufferStr(msg, "...");

		/*
		 * 消息前缀仍在 msg 缓冲区时，计算其屏幕宽度。
		 *       
		 */
		scroffset = 0;
		for (; i < msg->len; i += PQmblenBounded(&msg->data[i], encoding))
		{
			int			w = pg_encoding_dsplen(encoding, &msg->data[i]);

			if (w <= 0)
				w = 1;
			scroffset += w;
		}

		/* 完成 LINE 消息行。         */
		appendPQExpBufferStr(msg, &wquery[qidx[ibeg]]);
		if (end_trunc)
			appendPQExpBufferStr(msg, "...");
		appendPQExpBufferChar(msg, '\n');

		/* 现在发出游标标记行。   */
		scroffset += scridx[loc] - scridx[ibeg];
		for (i = 0; i < scroffset; i++)
			appendPQExpBufferChar(msg, ' ');
		appendPQExpBufferChar(msg, '^');
		appendPQExpBufferChar(msg, '\n');
	}

	/* 清理。 */
	free(scridx);
	free(qidx);
	free(wquery);
}


/*
 * 尝试读取 NegotiateProtocolVersion 消息。
 * 入口：'v' 消息类型和长度已消费。
 * 出口：成功消费消息返回 0。
 *		 数据不足返回 EOF。
                                      */
int
pqGetNegotiateProtocolVersion3(PGconn *conn)
{
	int			tmp;
	ProtocolVersion their_version;
	int			num;
	PQExpBufferData buf;

	if (pqGetInt(&tmp, 4, conn) != 0)
		return EOF;
	their_version = tmp;

	if (pqGetInt(&num, 4, conn) != 0)
		return EOF;

	initPQExpBuffer(&buf);
	for (int i = 0; i < num; i++)
	{
		if (pqGets(&conn->workBuffer, conn))
		{
			termPQExpBuffer(&buf);
			return EOF;
		}
		if (buf.len > 0)
			appendPQExpBufferChar(&buf, ' ');
		appendPQExpBufferStr(&buf, conn->workBuffer.data);
	}

	if (their_version < conn->pversion)
		libpq_append_conn_error(conn, "protocol version not supported by server: client uses %u.%u, server supports up to %u.%u",
								PG_PROTOCOL_MAJOR(conn->pversion), PG_PROTOCOL_MINOR(conn->pversion),
								PG_PROTOCOL_MAJOR(their_version), PG_PROTOCOL_MINOR(their_version));
	if (num > 0)
	{
		appendPQExpBuffer(&conn->errorMessage,
						  libpq_ngettext("protocol extension not supported by server: %s",
										 "protocol extensions not supported by server: %s", num),
						  buf.data);
		appendPQExpBufferChar(&conn->errorMessage, '\n');
	}

	/* 两者皆非——服务器不应发送  */
	if (!(their_version < conn->pversion) && !(num > 0))
		libpq_append_conn_error(conn, "invalid %s message", "NegotiateProtocolVersion");

	termPQExpBuffer(&buf);
	return 0;
}


/*
 * 尝试读取 ParameterStatus 消息。
 * 这在多个地方都可能发生，因此我们将其提取为子例程。
 * 入口：'S' 消息类型和长度已消费。
 * 出口：成功消费消息返回 0。
 *		 数据不足返回 EOF。
                                  */
static int
getParameterStatus(PGconn *conn)
{
	PQExpBufferData valueBuf;

	/* 获取参数名称     */
	if (pqGets(&conn->workBuffer, conn))
		return EOF;
	/* 获取参数值（可能很大）        */
	initPQExpBuffer(&valueBuf);
	if (pqGets(&valueBuf, conn))
	{
		termPQExpBuffer(&valueBuf);
		return EOF;
	}
	/* 保存      */
	pqSaveParameterStatus(conn, conn->workBuffer.data, valueBuf.data);
	termPQExpBuffer(&valueBuf);
	return 0;
}


/*
 * 尝试读取 Notify 响应消息。
 * 这在多个地方都可能发生，因此我们将其提取为子例程。
 * 入口：'A' 消息类型和长度已消费。
 * 出口：成功消费 Notify 消息返回 0。
 *		 数据不足返回 EOF。
                                    */
static int
getNotify(PGconn *conn)
{
	int			be_pid;
	char	   *svname;
	int			nmlen;
	int			extralen;
	PGnotify   *newNotify;

	if (pqGetInt(&be_pid, 4, conn))
		return EOF;
	if (pqGets(&conn->workBuffer, conn))
		return EOF;
	/* 获取额外字符串时须保存名称   */
	svname = strdup(conn->workBuffer.data);
	if (!svname)
		return EOF;
	if (pqGets(&conn->workBuffer, conn))
	{
		free(svname);
		return EOF;
	}

	/*
	 * 将字符串存于 PGnotify 结构之后以便
	 * 一次性释放。不用 NAMEDATALEN 以免
	 * 将此接口绑定到特定服务器名称长度限制。
	                                         */
	nmlen = strlen(svname);
	extralen = strlen(conn->workBuffer.data);
	newNotify = (PGnotify *) malloc(sizeof(PGnotify) + nmlen + extralen + 2);
	if (newNotify)
	{
		newNotify->relname = (char *) newNotify + sizeof(PGnotify);
		strcpy(newNotify->relname, svname);
		newNotify->extra = newNotify->relname + nmlen + 1;
		strcpy(newNotify->extra, conn->workBuffer.data);
		newNotify->be_pid = be_pid;
		newNotify->next = NULL;
		if (conn->notifyTail)
			conn->notifyTail->next = newNotify;
		else
			conn->notifyHead = newNotify;
		conn->notifyTail = newNotify;
	}

	free(svname);
	return 0;
}

/*
 * getCopyStart - 处理 CopyInResponse、CopyOutResponse 或
 * CopyBothResponse 消息
 *
 * parseInput 已读取消息类型和长度。
         */
static int
getCopyStart(PGconn *conn, ExecStatusType copytype)
{
	PGresult   *result;
	int			nfields;
	int			i;

	result = PQmakeEmptyPGresult(conn, copytype);
	if (!result)
		goto failure;

	if (pqGetc(&conn->copy_is_binary, conn))
		goto failure;
	result->binary = conn->copy_is_binary;
	/* 接下来的两个字节是字段数        */
	if (pqGetInt(&(result->numAttributes), 2, conn))
		goto failure;
	nfields = result->numAttributes;

	/* 为属性描述符分配空间               */
	if (nfields > 0)
	{
		result->attDescs = (PGresAttDesc *)
			pqResultAlloc(result, nfields * sizeof(PGresAttDesc), true);
		if (!result->attDescs)
			goto failure;
		MemSet(result->attDescs, 0, nfields * sizeof(PGresAttDesc));
	}

	for (i = 0; i < nfields; i++)
	{
		int			format;

		if (pqGetInt(&format, 2, conn))
			goto failure;

		/*
		 * pqGetInt 将两字节整数视为无符号，需将
		 * 结果强制转为有符号形式。
		           */
		format = (int) ((int16) format);
		result->attDescs[i].format = format;
	}

	/* 成功   */
	conn->result = result;
	return 0;

failure:
	PQclear(result);
	return EOF;
}

/*
 * getReadyForQuery - 处理 ReadyForQuery 消息
   */
static int
getReadyForQuery(PGconn *conn)
{
	char		xact_status;

	if (pqGetc(&xact_status, conn))
		return EOF;
	switch (xact_status)
	{
		case 'I':
			conn->xactStatus = PQTRANS_IDLE;
			break;
		case 'T':
			conn->xactStatus = PQTRANS_INTRANS;
			break;
		case 'E':
			conn->xactStatus = PQTRANS_INERROR;
			break;
		default:
			conn->xactStatus = PQTRANS_UNKNOWN;
			break;
	}

	return 0;
}

/*
 * getCopyDataMessage - 获取下条 CopyData 消息，处理异步消息
 *
 * 返回 CopyData 消息长度字（> 0），无完整消息则 0，
 * 复制结束则 -1，错误则 -2。
                 */
static int
getCopyDataMessage(PGconn *conn)
{
	char		id;
	int			msgLength;
	int			avail;

	for (;;)
	{
		/*
		 * 我们有下一条输入消息吗？为了让异步调用者
		 * 生活更简单，我们持续返回 0，直到下一条消息完全
		 * 可用，即使它不是 Copy Data。
		 */
		conn->inCursor = conn->inStart;
		if (pqGetc(&id, conn))
			return 0;
		if (pqGetInt(&msgLength, 4, conn))
			return 0;
		if (msgLength < 4)
		{
			handleSyncLoss(conn, id, msgLength);
			return -2;
		}
		avail = conn->inEnd - conn->inCursor;
		if (avail < msgLength - 4)
		{
			/*
			 * 返回前若需扩大输入缓冲区以容纳
			 * 整个消息。见 parseInput 注释。
			                     */
			if (pqCheckInBufferSpace(conn->inCursor + (size_t) msgLength - 4,
									 conn))
			{
				/*
				 * XXX 应补充更好的恢复代码...计划是利用其  
				 * 长度跳过该消息，然后报告一个错误。目前  
				 * 只好把它当作失去同步来处理(很可能确实   
				 * 如此!)  
				 */
				handleSyncLoss(conn, id, msgLength);
				return -2;
			}
			return 0;
		}

		/*
		 * 如果它是合法的异步消息类型，处理它。（NOTIFY
		 * 消息当前在此处不可能，但为了完整性我们
		 * 也处理它们。）否则，如果它不是 Copy Data，
		 * 假定是复制结束。
		    */
		switch (id)
		{
			case 'A':			/* NOTIFY */
				if (getNotify(conn))
					return 0;
				break;
			case 'N':			/* NOTICE */
				if (pqGetErrorNotice3(conn, false))
					return 0;
				break;
			case 'S':			/* ParameterStatus */
				if (getParameterStatus(conn))
					return 0;
				break;
			case 'd':			/* Copy Data，传回调用者       */
				return msgLength;
			case 'c':

				/*
				 * 若为 CopyDone 消息，退出 COPY_OUT 模式
				 * 让调用者用 PQgetResult() 读状态。
				 * 若为 COPY_BOTH 模式，返回 COPY_IN 模式。
				         */
				if (conn->asyncStatus == PGASYNC_COPY_BOTH)
					conn->asyncStatus = PGASYNC_COPY_IN;
				else
					conn->asyncStatus = PGASYNC_BUSY;
				return -1;
			default:			/* 视为复制结束   */

				/*
				 * 任何其他消息终止 COPY_IN 或 COPY_BOTH
				 * 模式。
				       */
				conn->asyncStatus = PGASYNC_BUSY;
				return -1;
		}

		/* 跟踪来自服务器的消息 */
		if (conn->Pfdebug)
			pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

		/* 丢弃已处理的消息并循环以获取下一条    */
		conn->inStart = conn->inCursor;
	}
}

/*
 * PQgetCopyData - 读取 COPY OUT 或 COPY BOTH 后端数据行。   
 *             
 *
 * 成功时，*buffer 将指向一行 malloc 分配的数据，并   
 * 返回行长度（总是 > 0）。        
 * 尚无可用行时返回 0（仅 async 为真时可能），       
 * 复制结束则返回 -1（见 PQgetResult），出错时详见 
 * PQerrorMessage  
 */
int
pqGetCopyData3(PGconn *conn, char **buffer, int async)
{
	int			msgLength;

	for (;;)
	{
		/*
		 * 收集下一条输入消息。为了让异步调用者
		 * 生活更简单，我们持续返回 0，直到下一条消息完全
		 * 可用，即使它不是 Copy Data。
		    */
		msgLength = getCopyDataMessage(conn);
		if (msgLength < 0)
			return msgLength;	/*复制结束或出错 */
		if (msgLength == 0)
		{
			/* 异步读取时不阻塞            */
			if (async)
				return 0;
			/* 需加载更多        */
			if (pqWait(true, false, conn) ||
				pqReadData(conn) < 0)
				return -2;
			continue;
		}

		/*
		 * 丢弃零长度消息（无论如何不应发生）。否则
		 * 将数据传回给调用者。
		       */
		msgLength -= 4;
		if (msgLength > 0)
		{
			*buffer = (char *) malloc(msgLength + 1);
			if (*buffer == NULL)
			{
				libpq_append_conn_error(conn, "out of memory");
				return -2;
			}
			memcpy(*buffer, &conn->inBuffer[conn->inCursor], msgLength);
			(*buffer)[msgLength] = '\0';	/* 添加终止 null    */

			/* 标记消息已消费 */
			conn->inStart = conn->inCursor + msgLength;

			return msgLength;
		}

		/* 空消息，丢弃并循环                   */
		conn->inStart = conn->inCursor;
	}
}

/*
 * PQgetline - 从后端获取换行终止的字符串。
 *
 * 见 fe-exec.c 文档。
                  */
int
pqGetline3(PGconn *conn, char *s, int maxlen)
{
	int			status;

	if (conn->sock == PGINVALID_SOCKET ||
		(conn->asyncStatus != PGASYNC_COPY_OUT &&
		 conn->asyncStatus != PGASYNC_COPY_BOTH) ||
		conn->copy_is_binary)
	{
		libpq_append_conn_error(conn, "PQgetline: not doing text COPY OUT");
		*s = '\0';
		return EOF;
	}

	while ((status = PQgetlineAsync(conn, s, maxlen - 1)) == 0)
	{
		/* 需加载更多        */
		if (pqWait(true, false, conn) ||
			pqReadData(conn) < 0)
		{
			*s = '\0';
			return EOF;
		}
	}

	if (status < 0)
	{
		/* 检测到复制结束；伪造旧式终止符     */
		strcpy(s, "\\.");
		return 0;
	}

	/* 加 null 终止，去除尾部 \n（若有）         */
	if (s[status - 1] == '\n')
	{
		s[status - 1] = '\0';
		return 0;
	}
	else
	{
		s[status] = '\0';
		return 1;
	}
}

/*
 * PQgetlineAsync - 不阻塞获取 COPY 数据行。
 *
 * 见 fe-exec.c 文档。
               */
int
pqGetlineAsync3(PGconn *conn, char *buffer, int bufsize)
{
	int			msgLength;
	int			avail;

	if (conn->asyncStatus != PGASYNC_COPY_OUT
		&& conn->asyncStatus != PGASYNC_COPY_BOTH)
		return -1;				/* 未进行复制...         */

	/*
	 * 识别下一条输入消息。为简化异步调用，
	 * 持续返回 0 直到下条消息完全可用，即使
	 * 不是 Copy Data，避免阻塞 PQendcopy。
	 * （注意：与 pqGetCopyData3 不同，不更改 asyncStatus。）
	                                                     */
	msgLength = getCopyDataMessage(conn);
	if (msgLength < 0)
		return -1;				/*复制结束或出错 */
	if (msgLength == 0)
		return 0;				/*尚无数据 */

	/*
	 * 把数据从 libpq 的缓冲区移到调用者的缓冲区。      
	 * 如果前一次调用发现调用者缓冲区太小，
	 * 就用 conn->copy_already_done 记住已向调用者               
	 * 返回了多少。     
	 */
	conn->inCursor += conn->copy_already_done;
	avail = msgLength - 4 - conn->copy_already_done;
	if (avail <= bufsize)
	{
		/* 能够消费整个消息          */
		memcpy(buffer, &conn->inBuffer[conn->inCursor], avail);
		/* 标记消息已消费 */
		conn->inStart = conn->inCursor + avail;
		/* 为下次重置状态     */
		conn->copy_already_done = 0;
		return avail;
	}
	else
	{
		/* 我们必须返回部分消息   */
		memcpy(buffer, &conn->inBuffer[conn->inCursor], bufsize);
		/* 该消息不从 libpq 的缓冲区中消费     */
		conn->copy_already_done += bufsize;
		return bufsize;
	}
}

/*
 * PQendcopy
 *
 * 见 fe-exec.c 文档。
          */
int
pqEndcopy3(PGconn *conn)
{
	PGresult   *result;

	if (conn->asyncStatus != PGASYNC_COPY_IN &&
		conn->asyncStatus != PGASYNC_COPY_OUT &&
		conn->asyncStatus != PGASYNC_COPY_BOTH)
	{
		libpq_append_conn_error(conn, "no COPY in progress");
		return 1;
	}

	/* 需要时发送 CopyDone            */
	if (conn->asyncStatus == PGASYNC_COPY_IN ||
		conn->asyncStatus == PGASYNC_COPY_BOTH)
	{
		if (pqPutMsgStart('c', conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			return 1;

		/*
		 * 若在扩展查询模式发送了 COPY 命令，
		 * 还需发出 Sync。
		             */
		if (conn->cmd_queue_head &&
			conn->cmd_queue_head->queryclass != PGQUERY_SIMPLE)
		{
			if (pqPutMsgStart('S', conn) < 0 ||
				pqPutMsgEnd(conn) < 0)
				return 1;
		}
	}

	/*
	 * 确保没有数据等待发送，如果我们是非阻塞的且刷新
	 * 失败则终止
	     */
	if (pqFlush(conn) && pqIsnonblocking(conn))
		return 1;

	/* 返回活动状态    */
	conn->asyncStatus = PGASYNC_BUSY;

	/*
	 * 非阻塞连接或需在此终止。
	 * 若均合规应无问题，但错误场景
	 * 中预期消息或未到达。（假设
	 * 后端分组确保 CommandComplete
	 * 随 CopyDone 到达？）
	                                                                                                                                                                            */
	if (pqIsnonblocking(conn) && PQisBusy(conn))
		return 1;

	/* 等待完成响应               */
	result = PQgetResult(conn);

	/* 期望一个成功的结果   */
	if (result && result->resultStatus == PGRES_COMMAND_OK)
	{
		PQclear(result);
		return 0;
	}

	/*
	 * 出问题了。为向后兼容，像通知一样
	 * 发出错误消息（能消除此愚蠢行为最好，
	 * 但太多应用可能不处理 PQendcopy 的错误
	 * 结果）。应用仍可从 PGconn 对象获取
	 * 错误状态。
	                                                                           */
	if (conn->errorMessage.len > 0)
	{
		/* 我们必须去除尾随的换行符... 真是麻烦...   */
		char		svLast = conn->errorMessage.data[conn->errorMessage.len - 1];

		if (svLast == '\n')
			conn->errorMessage.data[conn->errorMessage.len - 1] = '\0';
		pqInternalNotice(&conn->noticeHooks, "%s", conn->errorMessage.data);
		conn->errorMessage.data[conn->errorMessage.len - 1] = svLast;
	}

	PQclear(result);

	return 1;
}


/*
 * PQfn - 向 POSTGRES 后端发送函数调用。
 *
 * 请参见 fe-exec.c 中的文档。
   */
PGresult *
pqFunctionCall3(PGconn *conn, Oid fnid,
				int *result_buf, int *actual_result_len,
				int result_is_int,
				const PQArgBlock *args, int nargs)
{
	bool		needInput = false;
	ExecStatusType status = PGRES_FATAL_ERROR;
	char		id;
	int			msgLength;
	int			avail;
	int			i;

	/* 已由 PQfn 验证        */
	Assert(conn->pipelineStatus == PQ_PIPELINE_OFF);

	/* PQfn 已验证连接状态              */

	if (pqPutMsgStart('F', conn) < 0 || /*函数调用消息 */
		pqPutInt(fnid, 4, conn) < 0 ||	/* 函数 ID   */
		pqPutInt(1, 2, conn) < 0 || /* 格式码数量   */
		pqPutInt(1, 2, conn) < 0 || /* 格式码：BINARY  */
		pqPutInt(nargs, 2, conn) < 0)	/* 参数数 */
	{
		/* 错误消息应已设置               */
		return NULL;
	}

	for (i = 0; i < nargs; ++i)
	{							/* len.int4 + 内容      */
		if (pqPutInt(args[i].len, 4, conn))
			return NULL;
		if (args[i].len == -1)
			continue;			/* 为 NULL  */

		if (args[i].isint)
		{
			if (pqPutInt(args[i].u.integer, args[i].len, conn))
				return NULL;
		}
		else
		{
			if (pqPutnchar((char *) args[i].u.ptr, args[i].len, conn))
				return NULL;
		}
	}

	if (pqPutInt(1, 2, conn) < 0)	/* 结果格式码：BINARY   */
		return NULL;

	if (pqPutMsgEnd(conn) < 0 ||
		pqFlush(conn))
		return NULL;

	for (;;)
	{
		if (needInput)
		{
			/* 等待一些数据到达（或通道关闭）              */
			if (pqWait(true, false, conn) ||
				pqReadData(conn) < 0)
				break;
		}

		/*
		 * 扫描消息。如果数据用完，循环以重试。
		             */
		needInput = true;

		conn->inCursor = conn->inStart;
		if (pqGetc(&id, conn))
			continue;
		if (pqGetInt(&msgLength, 4, conn))
			continue;

		/*
		 * 在此处验证消息类型/长度。长度小于 4 必然        
		 * 已损坏。只有少数消息类型的较大长度才可          
		 * 采信。     
		 */
		if (msgLength < 4)
		{
			handleSyncLoss(conn, id, msgLength);
			break;
		}
		if (msgLength > 30000 && !VALID_LONG_MESSAGE_TYPE(id))
		{
			handleSyncLoss(conn, id, msgLength);
			break;
		}

		/*
		 * 消息体未全到达则无法处理。
		           */
		msgLength -= 4;
		avail = conn->inEnd - conn->inCursor;
		if (avail < msgLength)
		{
			/*
			 * 循环前若需扩大输入缓冲区以
			 * 容纳整个消息。见 parseInput 注释。
			                   */
			if (pqCheckInBufferSpace(conn->inCursor + (size_t) msgLength,
									 conn))
			{
				/*
				 * XXX 应补充更好的恢复代码...计划是利用其  
				 * 长度跳过该消息，然后报告一个错误。目前  
				 * 只好把它当作失去同步来处理(很可能确实   
				 * 如此!)  
				 */
				handleSyncLoss(conn, id, msgLength);
				break;
			}
			continue;
		}

		/*
		 * 应看到命令的 V 或 E 响应，但可先收
		 * N 和/或 A 通知。返回前还需吞下
		 * 最终 Z。
		                                     */
		switch (id)
		{
			case 'V':			/* 函数结果    */
				if (pqGetInt(actual_result_len, 4, conn))
					continue;
				if (*actual_result_len != -1)
				{
					if (result_is_int)
					{
						if (pqGetInt(result_buf, *actual_result_len, conn))
							continue;
					}
					else
					{
						if (pqGetnchar((char *) result_buf,
									   *actual_result_len,
									   conn))
							continue;
					}
				}
				/* 正确完成了函数结果消息          */
				status = PGRES_COMMAND_OK;
				break;
			case 'E':			/* 错误返回 */
				if (pqGetErrorNotice3(conn, true))
					continue;
				status = PGRES_FATAL_ERROR;
				break;
			case 'A':			/* 通知消息   */
				/* 处理通知并返回继续处理返回值            */
				if (getNotify(conn))
					continue;
				break;
			case 'N':			/* 通知 */
				/* 处理通知并返回继续处理返回值            */
				if (pqGetErrorNotice3(conn, false))
					continue;
				break;
			case 'Z':			/* 后端就绪接受新查询    */
				if (getReadyForQuery(conn))
					continue;
				/* 消费消息并退出        */
				conn->inStart += 5 + msgLength;

				/*
				 * 若已有结果对象（多半是错误），就用它。  
				 * 否则，若看到了函数结果消息，则报告   
				 * COMMAND_OK。否则就是后端违反协议，需要   
				 * 报错。             
				 */
				if (!pgHavePendingResult(conn))
				{
					if (status == PGRES_COMMAND_OK)
					{
						conn->result = PQmakeEmptyPGresult(conn, status);
						if (!conn->result)
						{
							libpq_append_conn_error(conn, "out of memory");
							pqSaveErrorResult(conn);
						}
					}
					else
					{
						libpq_append_conn_error(conn, "protocol error: no function result");
						pqSaveErrorResult(conn);
					}
				}
				return pqPrepareAsyncResult(conn);
			case 'S':			/* 参数状态     */
				if (getParameterStatus(conn))
					continue;
				break;
			default:
				/* 后端违反了协议。           */
				libpq_append_conn_error(conn, "protocol error: id=0x%x", id);
				pqSaveErrorResult(conn);
				/* 信任指定消息长度作为跳过量            */
				conn->inStart += 5 + msgLength;
				return pqPrepareAsyncResult(conn);
		}

		/* 跟踪来自服务器的消息 */
		if (conn->Pfdebug)
			pqTraceOutputMessage(conn, conn->inBuffer + conn->inStart, false);

		/* 已完成此消息，继续        */
		/* 信任指定消息长度作为跳过量            */
		conn->inStart += 5 + msgLength;
		needInput = false;
	}

	/*
	 * 仅在读数据失败时退出循环。
	 * conn->errorMessage 已由 pqWait/pqReadData 设置。
	 * 将其追加到已接收的错误消息后。
	                                  */
	pqSaveErrorResult(conn);
	return pqPrepareAsyncResult(conn);
}


/*
 * 构建启动包
 *
 * 返回 malloc 的包缓冲区，如果内存不足则返回 NULL
    */
char *
pqBuildStartupPacket3(PGconn *conn, int *packetlen,
					  const PQEnvironmentOption *options)
{
	char	   *startpacket;
	size_t		len;

	len = build_startup_packet(conn, NULL, options);
	if (len == 0 || len > INT_MAX)
		return NULL;

	*packetlen = len;
	startpacket = (char *) malloc(*packetlen);
	if (!startpacket)
		return NULL;

	len = build_startup_packet(conn, startpacket, options);
	Assert(*packetlen == len);

	return startpacket;
}

/*
 * 后端 add_size() 的前端版本，意在与 pg_add_*_overflow()          
 * 助手 API 兼容。成功时把结果存入 *dst；若加法                
 * 溢出则改为返回真。                    
 *
 * TODO: 移至 common/int.h 
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
 * 根据填好的 PGconn 结构构建一个启动包。     
 *
 * 先算出所需空间，然后填入。为避免逻辑重复， 
 * 本例程会被调用两次：第一次（packet == NULL）           
 * 只统计所需空间，第二次（packet == 已分配空间）   
 * 则执行填入。返回值是所用字节数；若发生                
 * size_t 溢出（可能性极小）则为零。                  
 */
static size_t
build_startup_packet(const PGconn *conn, char *packet,
					 const PQEnvironmentOption *options)
{
	size_t		packet_len = 0;
	const PQEnvironmentOption *next_eo;
	const char *val;

	/* 首先放置协议版本。   */
	if (packet)
	{
		ProtocolVersion pv = pg_hton32(conn->pversion);

		memcpy(packet + packet_len, &pv, sizeof(ProtocolVersion));
	}
	packet_len += sizeof(ProtocolVersion);

	/*加入用户名、数据库名和选项*/

#define ADD_STARTUP_OPTION(optname, optval) \
	do { \
		if (packet) \
			strcpy(packet + packet_len, optname); \
		if (add_size_overflow(packet_len, strlen(optname) + 1, &packet_len)) \
			return 0; \
		if (packet) \
			strcpy(packet + packet_len, optval); \
		if (add_size_overflow(packet_len, strlen(optval) + 1, &packet_len)) \
			return 0; \
	} while(0)

	if (conn->pguser && conn->pguser[0])
		ADD_STARTUP_OPTION("user", conn->pguser);
	if (conn->dbName && conn->dbName[0])
		ADD_STARTUP_OPTION("database", conn->dbName);
	if (conn->replication && conn->replication[0])
		ADD_STARTUP_OPTION("replication", conn->replication);
	if (conn->pgoptions && conn->pgoptions[0])
		ADD_STARTUP_OPTION("options", conn->pgoptions);
	if (conn->send_appname)
	{
		/* 有 appname 则用它，否则用回退值     */
		val = conn->appname ? conn->appname : conn->fbappname;
		if (val && val[0])
			ADD_STARTUP_OPTION("application_name", val);
	}

	if (conn->client_encoding_initial && conn->client_encoding_initial[0])
		ADD_STARTUP_OPTION("client_encoding", conn->client_encoding_initial);

	/* 添加任何需要的环境驱动 GUC 设置   */
	for (next_eo = options; next_eo->envName; next_eo++)
	{
		if ((val = getenv(next_eo->envName)) != NULL)
		{
			if (pg_strcasecmp(val, "default") != 0)
				ADD_STARTUP_OPTION(next_eo->pgName, val);
		}
	}

	/* 添加尾部终止符   */
	if (packet)
		packet[packet_len] = '\0';
	if (add_size_overflow(packet_len, 1, &packet_len))
		return 0;

	return packet_len;
}
