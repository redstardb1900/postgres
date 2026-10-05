/*-------------------------------------------------------------------------
 *
 * auth-sasl.c
 *	  经由 SASL 进行认证的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth-sasl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/sasl.h"

/*
 * 接受的 SASL 消息最大尺寸。
 *
 * 服务器或 libpq 生成的消息比这小得多，但留了一些余量。
 *
 */
#define PG_MAX_SASL_MESSAGE_LENGTH	1024

/*
 * 使用特定机制实现，与 libpq 客户端进行一次 SASL 交换。
 *
 * shadow_pass 是可选指针，指向已认证角色的已存储秘密，来自
 * pg_authid.rolpassword。对使用已存储口令的机制，这里的
 * NULL 指针表示找不到该角色的条目（或用户不存在），机制
 * 应使这次认证交换失败。
 *
 * 机制必须注意不要向客户端透露用户条目不存在；理想情况下，
 * 对外的失败表现应与口令错误完全相同。机制也可以改用
 * logdetail 输出参数，在内部区分失败情形，帮助服务器管理员
 * 调试。
 *
 * 机制不必使用影子条目，甚至可以完全不用口令系统；对这些
 * 情况，可以忽略 shadow_pass，调用者只需传入 NULL。
 *
 *
 *
 *
 */
int
CheckSASLAuth(const pg_be_sasl_mech *mech, Port *port, char *shadow_pass,
			  const char **logdetail)
{
	StringInfoData sasl_mechs;
	int			mtype;
	StringInfoData buf;
	void	   *opaq = NULL;
	char	   *output = NULL;
	int			outputlen = 0;
	const char *input;
	int			inputlen;
	int			result;
	bool		initial;

	/*
 * 向用户发送 SASL 认证请求。其中包含所支持的认证机制列表。
 *
 */
	initStringInfo(&sasl_mechs);

	mech->get_mechanisms(port, &sasl_mechs);
	/* 再放一个 '\0'，标记列表结束。 */
	appendStringInfoChar(&sasl_mechs, '\0');

	sendAuthRequest(port, AUTH_REQ_SASL, sasl_mechs.data, sasl_mechs.len);
	pfree(sasl_mechs.data);

	/*
 * 循环进行 SASL 消息交换。这次交换可以包含双向发送的多条
 * 消息。第一条消息总是来自客户端。客户端发往服务器的所有
 * 消息都是口令包（类型 'p'）。
 *
 */
	initial = true;
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* 仅当客户端没有断开时才记录错误。 */
			if (mtype != EOF)
			{
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected SASL response, got message type %d",
								mtype)));
			}
			else
				return STATUS_EOF;
		}

		/* 取得实际的 SASL 消息 */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_SASL_MESSAGE_LENGTH))
		{
			/* EOF，pq_getmessage 已经记录了错误 */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		elog(DEBUG4, "processing received SASL response of length %d", buf.len);

		/*
 * 第一条 SASLInitialResponse 消息与其他消息不同。它指出
 * 客户端选择了哪种 SASL 机制，并包含可选的 Initial Client
 * Response 负载。随后的 SASLResponse 消息只包含 SASL 负载。
 *
 */
		if (initial)
		{
			const char *selected_mech;

			selected_mech = pq_getmsgrawstring(&buf);

			/*
 * 初始化消息交换的状态跟踪。
 *
 * 即使用户不存在，或没有有效口令，或口令已过期，我们仍走
 * 一遍 SASL 认证的步骤，但告诉认证方法这次认证 "注定失败"。
 * 也就是说，无论怎样它都会失败。
 *
 * 这是因为我们不想向攻击者透露哪些用户名有效，也不想透露
 * 哪些用户有有效口令。
 *
 *
 */
			opaq = mech->init(port, selected_mech, shadow_pass);

			inputlen = pq_getmsgint(&buf, 4);
			if (inputlen == -1)
				input = NULL;
			else
				input = pq_getmsgbytes(&buf, inputlen);

			initial = false;
		}
		else
		{
			inputlen = buf.len;
			input = pq_getmsgbytes(&buf, buf.len);
		}
		pq_getmsgend(&buf);

		/*
 * StringInfo 保证应答后面有一个 \0 字节。
 *
 */
		Assert(input == NULL || input[inputlen] == '\0');

		/*
 * 把收到的消息交给机制实现。
 */
		result = mech->exchange(opaq, input, inputlen,
								&output, &outputlen,
								logdetail);

		/* 输入缓冲区不再使用 */
		pfree(buf.data);

		if (output)
		{
			/*
 * SASL 禁止 PG_SASL_EXCHANGE_FAILURE 还带有某些输出。在这里
 * 确认所用机制做对了这一点。
 */
			if (result == PG_SASL_EXCHANGE_FAILURE)
				elog(ERROR, "output message found after SASL exchange failure");

			/*
 * 协商产生了要发给客户端的数据。
 */
			elog(DEBUG4, "sending SASL challenge of length %d", outputlen);

			if (result == PG_SASL_EXCHANGE_SUCCESS)
				sendAuthRequest(port, AUTH_REQ_SASL_FIN, output, outputlen);
			else
				sendAuthRequest(port, AUTH_REQ_SASL_CONT, output, outputlen);

			pfree(output);
		}
	} while (result == PG_SASL_EXCHANGE_CONTINUE);

	/* 糟糕，出了坏事 */
	if (result != PG_SASL_EXCHANGE_SUCCESS)
	{
		return STATUS_ERROR;
	}

	return STATUS_OK;
}
