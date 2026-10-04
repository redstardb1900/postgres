/*-------------------------------------------------------------------------
 *
 * be-secure-gssapi.c
 *  GSSAPI encryption support
 *
 * Portions Copyright (c) 2018-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *  src/backend/libpq/be-secure-gssapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "libpq/auth.h"
#include "libpq/be-gssapi-common.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"


/*
 * 用 GSSAPI 处理数据的加密/解密。
 *
 * 在线路上的加密数据流中，我们把数据
 * 拆成数据包，每个包以一个 uint32 长度
 * 字开头（网络字节序），紧接着是该长度的
 * 加密数据。解密得到的数据流
 * 与不使用加密时出现的数据流相同。
 *
 * 加密后的数据通常比同样的未加密数据更大，
 * 因此我们用固定大小的缓冲区处理
 * 加密/解密。这些缓冲区比 PQComm 的缓冲区
 * 通常更大，以便尽量减少必须做成多个包
 * 的次数（从而对一次读/写调用减少多次
 * recv/send 调用）。
 *
 * 注意：客户端和服务器必须就最大包大小
 * 达成一致，因为我们一次必须把整个包交给
 * GSSAPI，而且我们不希望对端发送任意巨大的
 * 包，否则我们得为它们分配内存，再交给 GSSAPI。
 *
 * 因此，这个 #define 实际上是协议规范的一部分，
 * 永远无法更改。
 */
#define PQ_GSS_MAX_PACKET_SIZE 16384	/* 含 uint32 头字 */

/*
 * 不过，在认证交换期间，我们必须应付 GSSAPI 库想要
 * 发送的任何消息大小（因为我们的协议不支持拆分
 * 那些消息）。视配置而定，那些消息可能大到
 * 64kB。
 */
#define PQ_GSS_AUTH_BUFFER_SIZE 65536	/* 含 uint32 头字 */

/*
 * 由于每个后端最多管理一条 GSS 加密连接，
 * 我们可以把这些状态都放在静态变量里。char *
 * 变量指向只分配一次并重复使用的缓冲区。
 */
static char *PqGSSSendBuffer;	/* 等待发送的加密数据 */
static int	PqGSSSendLength;	/* PqGSSSendBuffer 中可用数据的末尾 */
static int	PqGSSSendNext;		/* 下一个要发出字节的下标，来自
								 * PqGSSSendBuffer */
static int	PqGSSSendConsumed;	/* 已加密但尚未报告为已发送的
								 * 源字节数 */

static char *PqGSSRecvBuffer;	/* 已收到的加密数据 */
static int	PqGSSRecvLength;	/* PqGSSRecvBuffer 中可用数据的末尾 */

static char *PqGSSResultBuffer; /* gss_RecvBuffer 中数据的解密结果 */
static int	PqGSSResultLength;	/* PqGSSResultBuffer 中可用数据的末尾 */
static int	PqGSSResultNext;	/* 下一个要读取字节的下标，来自
								 * PqGSSResultBuffer */

static uint32 PqGSSMaxPktSize;	/* 我们能加密并放进输出缓冲区的
								 * 最大尺寸 */


/*
 * 尝试把 ptr 中 len 字节的数据写入一条 GSSAPI 加密连接。
 *
 * 该连接必须已经为 GSSAPI 加密设置好（即 GSSAPI
 * 传输协商已经完成）。
 *
 * 成功时，返回消耗的数据字节数（可能小于
 * len）。失败时，返回 -1 并适当设置 errno。对可重试的
 * 错误，调用方应在套接字就绪后再调用一次
 * （传入相同或更多的数据）。
 *
 * 在这里处理致命错误有点棘手：我们无法调用 elog(FATAL)，
 * 因为它会试图写给客户端，很可能导致无限
 * 递归。改为用 elog(COMMERROR) 在必要时记录关于
 * 失败的额外信息，然后返回一个表示连接丢失的 errno。
 */
ssize_t
be_gssapi_write(Port *port, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output;
	size_t		bytes_to_encrypt;
	size_t		bytes_encrypted;
	gss_ctx_id_t gctx = port->gss->ctx;

	/*
 * 遇到可重试的失败时，我们不得告诉调用方
 * 已经成功传完了一切，否则它就不会重试。为了
 * 简单，在成功传完全部 "len" 字节之前，我们声称
 * 什么都还没传。在两次调用之间，当前输入数据中
 * 已经加密并放入 PqGSSSendBuffer（也许已经发出）
 * 的数量记在 PqGSSSendConsumed 里。重试时，调用方
 * *必须* 再次发送那些数据，因此若它给出的 len
 * 比这更小，就有问题。
 *
 * 注意：一旦成功发出任何加密包就报告部分写完成，
 * 看起来很诱人。但那样做会扩大这段处理的状态
 * 空间，而且过去因此出过 bug（参见 commit d053a879b）。
 * 通常让调用方提早丢弃数据也省不了多少，所以
 * 不要去冒这个险。
 */
	if (len < PqGSSSendConsumed)
	{
		elog(COMMERROR, "GSSAPI caller failed to retransmit all data needing to be retried");
		errno = ECONNRESET;
		return -1;
	}

	/* 扣掉我们已经加密过的源数据。 */
	bytes_to_encrypt = len - PqGSSSendConsumed;
	bytes_encrypted = PqGSSSendConsumed;

	/*
 * 循环加密数据并发送出去，直到全部完成，或
 * secure_raw_write() 报错（这很可能意味着套接字
 * 是非阻塞的，所请求的 send() 会阻塞，或者发生了
 * 某种实际错误）。
 */
	while (bytes_to_encrypt || PqGSSSendLength)
	{
		int			conf_state = 0;
		uint32		netlen;

		/*
 * 检查加密输出缓冲区里是否有需要发送的数据
 * （可能是上次调用留下的），若有就尝试发送。
 * 若发不出去，就把这个事实返回给
 * 调用方。
 */
		if (PqGSSSendLength)
		{
			ssize_t		ret;
			ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

			ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendNext, amount);
			if (ret <= 0)
				return ret;

			/*
 * 检查这是否为部分写入，若是，就在缓冲区里
 * 前进那么远并重试。
 */
			if (ret < amount)
			{
				PqGSSSendNext += ret;
				continue;
			}

			/* 缓冲区里的数据已经成功发出。 */
			PqGSSSendLength = PqGSSSendNext = 0;
		}

		/*
 * 检查是否还有字节要加密。若没有，就完成了。
 */
		if (!bytes_to_encrypt)
			break;

		/*
 * 检查要我们发送多少；若过多，就得循环，
 * 并且可能被多次调用才能处理完
 * 全部数据。
 */
		if (bytes_to_encrypt > PqGSSMaxPktSize)
			input.length = PqGSSMaxPktSize;
		else
			input.length = bytes_to_encrypt;

		input.value = (char *) ptr + bytes_encrypted;

		output.value = NULL;
		output.length = 0;

		/*
 * 创建下一个加密包。这里的任何失败都视为硬失败，
 * 因此即使已经发出一些数据，我们也返回 -1。
 */
		major = gss_wrap(&minor, gctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(_("GSSAPI wrap error"), major, minor);
			errno = ECONNRESET;
			return -1;
		}
		if (conf_state == 0)
		{
			ereport(COMMERROR,
					(errmsg("outgoing GSSAPI message would not use confidentiality")));
			errno = ECONNRESET;
			return -1;
		}
		if (output.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			ereport(COMMERROR,
					(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
							(size_t) output.length,
							PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))));
			errno = ECONNRESET;
			return -1;
		}

		bytes_encrypted += input.length;
		bytes_to_encrypt -= input.length;
		PqGSSSendConsumed += input.length;

		/* 4 个网络序长度字节，然后是负载 */
		netlen = pg_hton32(output.length);
		memcpy(PqGSSSendBuffer + PqGSSSendLength, &netlen, sizeof(uint32));
		PqGSSSendLength += sizeof(uint32);

		memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
		PqGSSSendLength += output.length;

		/* 释放 GSSAPI 分配的缓冲区存储 */
		gss_release_buffer(&minor, &output);
	}

	/* 若走到这里，各计数器应该全都对得上。 */
	Assert(len == PqGSSSendConsumed);
	Assert(len == bytes_encrypted);

	/* 我们把全部数据都报告为已发送，因此重置 PqGSSSendConsumed。 */
	PqGSSSendConsumed = 0;

	return bytes_encrypted;
}

/*
 * 从一条 GSSAPI 加密连接读取最多 len 字节到 ptr。
 *
 * 该连接必须已经为 GSSAPI 加密设置好（即 GSSAPI
 * 传输协商已经完成）。
 *
 * 返回读到的数据字节数；失败时返回 -1
 * 并适当设置 errno。对可重试的错误，调用方应在
 * 套接字就绪后再调用一次。
 *
 * 我们对致命错误的处理与 be_gssapi_write() 相同，尽管
 * 关于无限递归的论点在这里并不适用。
 */
ssize_t
be_gssapi_read(Port *port, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output;
	ssize_t		ret;
	size_t		bytes_returned = 0;
	gss_ctx_id_t gctx = port->gss->ctx;

	/*
 * 这里的计划是把一个传入的加密包读进
 * PqGSSRecvBuffer，解密到 PqGSSResultBuffer，然后从那里
 * 把数据分给调用方。当前输入包用尽后，再读
 * 下一个。
 */
	while (bytes_returned < len)
	{
		int			conf_state = 0;

		/* 检查缓冲区里是否有可以立刻返回的数据 */
		if (PqGSSResultNext < PqGSSResultLength)
		{
			size_t		bytes_in_buffer = PqGSSResultLength - PqGSSResultNext;
			size_t		bytes_to_copy = Min(bytes_in_buffer, len - bytes_returned);

			/*
 * 把数据从我们的结果缓冲区复制到调用方的缓冲区，
 * 放在我们上次停下、正在填充其缓冲区的位置。
 */
			memcpy((char *) ptr + bytes_returned, PqGSSResultBuffer + PqGSSResultNext, bytes_to_copy);
			PqGSSResultNext += bytes_to_copy;
			bytes_returned += bytes_to_copy;

			/*
 * 到这里，要么填满了调用方的缓冲区，要么
 * 清空了我们的结果缓冲区。无论哪种，都返回给
 * 调用方。第二种情况下，我们可以尝试再读一个
 * 加密包，但很可能并没有包可读。（若并非
 * 如此，就是我们选的最大包太小了。）无论如何，
 * 让调用方处理我们已经返回的数据
 * 并没有坏处。
 */
			break;
		}

		/* 结果缓冲区是空的，因此重置缓冲区指针 */
		PqGSSResultLength = PqGSSResultNext = 0;

		/*
 * 因为上面我们选择了一发出一些数据就立刻返回，
 * 此时 bytes_returned 必须为零。因此下面的失败出口
 * 可以直接返回 -1，不必担心是否已经发出了
 * 一些数据。
 */
		Assert(bytes_returned == 0);

		/*
 * 到这里，我们的结果缓冲区是空的，还有更多字节
 * 被请求读取。我们现在准备装入下一个包，并把它
 * （完整地）解密到结果缓冲区。
 */

		/* 若还没有长度，就收集它 */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
								  sizeof(uint32) - PqGSSRecvLength);

			/* 若 ret <= 0，secure_raw_read 已经设置了正确的 errno */
			if (ret <= 0)
				return ret;

			PqGSSRecvLength += ret;

			/* 若仍然没有拿到长度，就返回给调用方 */
			if (PqGSSRecvLength < sizeof(uint32))
			{
				errno = EWOULDBLOCK;
				return -1;
			}
		}

		/* 解码包长度，并检查过长的包 */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);

		if (input.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			ereport(COMMERROR,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %zu)",
							(size_t) input.length,
							PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))));
			errno = ECONNRESET;
			return -1;
		}

		/*
 * 在这次调用中，尽量把这个包读进我们上次
 * 被调用时停下的位置。
 */
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
							  input.length - (PqGSSRecvLength - sizeof(uint32)));
		/* 若 ret <= 0，secure_raw_read 已经设置了正确的 errno */
		if (ret <= 0)
			return ret;

		PqGSSRecvLength += ret;

		/* 若还没有整个包，就返回给调用方 */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
		{
			errno = EWOULDBLOCK;
			return -1;
		}

		/*
 * 现在我们已经有了完整的包，可以执行解密并
 * 重新填满结果缓冲区，然后循环回去把数据
 * 传回调用方。
 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, gctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(_("GSSAPI unwrap error"), major, minor);
			errno = ECONNRESET;
			return -1;
		}
		if (conf_state == 0)
		{
			ereport(COMMERROR,
					(errmsg("incoming GSSAPI message did not use confidentiality")));
			errno = ECONNRESET;
			return -1;
		}

		memcpy(PqGSSResultBuffer, output.value, output.length);
		PqGSSResultLength = output.length;

		/* 接收缓冲区现在是空的，重置它 */
		PqGSSRecvLength = 0;

		/* 释放 GSSAPI 分配的缓冲区存储 */
		gss_release_buffer(&minor, &output);
	}

	return bytes_returned;
}

/*
 * 从线路上读取指定字节数，若会阻塞就用
 * WaitLatchOrSocket 等待。
 *
 * 结果读入 PqGSSRecvBuffer。
 *
 * 总是返回 -1（表示永久性错误）或 len。
 */
static ssize_t
read_or_wait(Port *port, ssize_t len)
{
	ssize_t		ret;

	/*
 * 一直继续，直到读入了所请求的全部内容，或者我们
 * 出错退出。
 */
	while (PqGSSRecvLength < len)
	{
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength, len - PqGSSRecvLength);

		/*
 * 若返回了错误，并且不只是
 * EWOULDBLOCK/EAGAIN/EINTR，就放弃。
 */
		if (ret < 0 &&
			!(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR))
			return -1;

		/*
 * 好，我们得到的要么是正值、零，要么是表示
 * 应该重试的负结果。
 *
 * 若是零或负值，就等待套接字再次
 * 可读。
 */
		if (ret <= 0)
		{
			WaitLatchOrSocket(MyLatch,
							  WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
							  port->sock, 0, WAIT_EVENT_GSS_OPEN_SERVER);

			/*
			 * If we got back zero bytes, and then waited on the socket to be
			 * readable and got back zero bytes on a second read, then this is
			 * EOF and the client hung up on us.
			 *
			 * If we did get data here, then we can just fall through and
			 * handle it just as if we got data the first time.
			 *
			 * Otherwise loop back to the top and try again.
			 */
			if (ret == 0)
			{
				ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength, len - PqGSSRecvLength);
				if (ret == 0)
					return -1;
			}
			if (ret < 0)
				continue;
		}

		PqGSSRecvLength += ret;
	}

	return len;
}

/*
 * 启动一条 GSSAPI 加密连接。这里执行 GSSAPI
 * 认证；本函数完成后，就可以安全地调用
 * be_gssapi_read 和 be_gssapi_write。失败时返回 -1
 * 并写日志；否则返回 0，并把连接标记为已准备好
 * 做 GSSAPI 加密。
 *
 * 注意，与 be_gssapi_read/be_gssapi_write 函数不同，
 * 本函数在建立 GSSAPI 会话时，会在套接字准备好
 * 读/写时按需阻塞（使用
 * WaitLatchOrSocket）。
 */
ssize_t
secure_open_gssapi(Port *port)
{
	bool		complete_next = false;
	OM_uint32	major,
				minor;
	gss_cred_id_t delegated_creds;

	/*
 * 为 GSSAPI 操作分配从属的 Port 数据。
 */
	port->gss = (pg_gssinfo *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(pg_gssinfo));

	delegated_creds = GSS_C_NO_CREDENTIAL;
	port->gss->delegated_creds = false;

	/*
 * 分配缓冲区并初始化状态变量。此时用 malloc
 * 分配缓冲区，可以避免在永远不会使用它们的进程里
 * 浪费静态数据空间，并确保缓冲区对
 * 本文件某些地方所做的长度字访问有足够的对齐。
 *
 * 在传输协商完成之前，我们使用大小为
 * PQ_GSS_AUTH_BUFFER_SIZE 的缓冲区，然后切换到
 * PQ_GSS_MAX_PACKET_SIZE。
 */
	PqGSSSendBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
	PqGSSRecvBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
	PqGSSResultBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
	if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
	PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;

	/*
 * 若配置了 keytab，就使用它。由于我们现在需要 MIT
 * Kerberos，将来也许可以考虑改用凭证存储扩展，
 * 而不用环境变量。
 */
	if (pg_krb_server_keyfile != NULL && pg_krb_server_keyfile[0] != '\0')
	{
		if (setenv("KRB5_KTNAME", pg_krb_server_keyfile, 1) != 0)
		{
			/* 唯一可能的失败原因是内存不足，因此使用该错误码 */
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not set environment: %m")));
		}
	}

	while (true)
	{
		ssize_t		ret;
		gss_buffer_desc input,
					output = GSS_C_EMPTY_BUFFER;

		/*
 * 客户端总是先发送，因此试着先去读长度，若失败
 * 就等待套接字再次可读。
 */
		ret = read_or_wait(port, sizeof(uint32));
		if (ret < 0)
			return ret;

		/*
 * 从长度头取出这个包的长度。
 */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);

		/* 长度已用完，重置缓冲区 */
		PqGSSRecvLength = 0;

		/*
 * 初始化期间，包总是被完全消耗，并且总长度
 * 不应超过 PQ_GSS_AUTH_BUFFER_SIZE。
 *
 * 在我们这边确认客户端没有做奇怪的事。
 */
		if (input.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
		{
			ereport(COMMERROR,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %zu)",
							(size_t) input.length,
							PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))));
			return -1;
		}

		/*
 * 取包的其余部分，以便交给 GSSAPI 来接受
 * 上下文。
 */
		ret = read_or_wait(port, input.length);
		if (ret < 0)
			return ret;

		input.value = PqGSSRecvBuffer;

		/* 处理传入数据。（客户端先发送。） */
		major = gss_accept_sec_context(&minor, &port->gss->ctx,
									   GSS_C_NO_CREDENTIAL, &input,
									   GSS_C_NO_CHANNEL_BINDINGS,
									   &port->gss->name, NULL, &output, NULL,
									   NULL, pg_gss_accept_delegation ? &delegated_creds : NULL);

		if (GSS_ERROR(major))
		{
			pg_GSS_error(_("could not accept GSSAPI security context"),
						 major, minor);
			gss_release_buffer(&minor, &output);
			return -1;
		}
		else if (!(major & GSS_S_CONTINUE_NEEDED))
		{
			/*
 * rfc2744 在技术上允许上下文协商在有包要发
 * 和没有包要发两种情况下都完成。
 */
			complete_next = true;
		}

		if (delegated_creds != GSS_C_NO_CREDENTIAL)
		{
			pg_store_delegated_credential(delegated_creds);
			port->gss->delegated_creds = true;
		}

		/* 传入包已处理完，重置缓冲区 */
		PqGSSRecvLength = 0;

		/*
 * 检查是否有数据要发送，若有，确保全部
 * 发出去。
 */
		if (output.length > 0)
		{
			uint32		netlen = pg_hton32(output.length);

			if (output.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
			{
				ereport(COMMERROR,
						(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
								(size_t) output.length,
								PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))));
				gss_release_buffer(&minor, &output);
				return -1;
			}

			memcpy(PqGSSSendBuffer, (char *) &netlen, sizeof(uint32));
			PqGSSSendLength += sizeof(uint32);

			memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
			PqGSSSendLength += output.length;

			/* 这里我们不费心处理 PqGSSSendConsumed */

			while (PqGSSSendNext < PqGSSSendLength)
			{
				ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendNext,
									   PqGSSSendLength - PqGSSSendNext);

				/*
 * 若返回了错误，并且不只是
 * EWOULDBLOCK/EAGAIN/EINTR，就放弃。
 */
				if (ret < 0 &&
					!(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR))
				{
					gss_release_buffer(&minor, &output);
					return -1;
				}

				/* 若还写不出去，就等待并重试 */
				if (ret <= 0)
				{
					WaitLatchOrSocket(MyLatch,
									  WL_SOCKET_WRITEABLE | WL_EXIT_ON_PM_DEATH,
									  port->sock, 0, WAIT_EVENT_GSS_OPEN_SERVER);
					continue;
				}

				PqGSSSendNext += ret;
			}

			/* 包已发送完，重置缓冲区 */
			PqGSSSendLength = PqGSSSendNext = 0;

			gss_release_buffer(&minor, &output);
		}

		/*
 * 若返回的是连接已经建立完成，并且我们已经发出
 * 了最后一个包，就退出循环。
 */
		if (complete_next)
			break;
	}

	/*
 * 释放大的认证缓冲区，并分配正常操作
 * 要用的那些。
 */
	free(PqGSSSendBuffer);
	free(PqGSSRecvBuffer);
	free(PqGSSResultBuffer);
	PqGSSSendBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
	PqGSSRecvBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
	PqGSSResultBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
	if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
	PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;

	/*
 * 计入长度之后，确定能放进缓冲区的最大包大小。
 * be_gssapi_write 会需要它。
 */
	major = gss_wrap_size_limit(&minor, port->gss->ctx, 1, GSS_C_QOP_DEFAULT,
								PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32),
								&PqGSSMaxPktSize);

	if (GSS_ERROR(major))
	{
		pg_GSS_error(_("GSSAPI size check error"), major, minor);
		return -1;
	}

	port->gss->enc = true;

	return 0;
}

/*
 * 返回这条连接是否使用了 GSSAPI 认证。
 */
bool
be_gssapi_get_auth(Port *port)
{
	if (!port || !port->gss)
		return false;

	return port->gss->auth;
}

/*
 * 返回这条连接是否启用并正在使用 GSSAPI 加密。
 */
bool
be_gssapi_get_enc(Port *port)
{
	if (!port || !port->gss)
		return false;

	return port->gss->enc;
}

/*
 * 返回这条连接上用于认证的 GSSAPI principal
 * （若未做 GSSAPI 认证则为 NULL）。
 */
const char *
be_gssapi_get_princ(Port *port)
{
	if (!port || !port->gss)
		return NULL;

	return port->gss->princ;
}

/*
 * 返回这条连接是否包含了 GSSAPI 委托
 * 凭证。
 */
bool
be_gssapi_get_delegation(Port *port)
{
	if (!port || !port->gss)
		return false;

	return port->gss->delegated_creds;
}
