/*-------------------------------------------------------------------------
 *
 * fe-secure-gssapi.c
 *   The front-end (client) encryption support for GSSAPI
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *  src/interfaces/libpq/fe-secure-gssapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "fe-gssapi-common.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/*
 * 要求加密支持，以及相互认证和                      
 * 防篡改措施。      
 */
#define GSS_REQUIRED_FLAGS GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | \
	GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG

/*
 * 使用 GSSAPI 处理数据的加密和解密。       
 *
 * 在线上的加密数据流中，我们将数据分割        
 * 成数据包，每个数据包以一个 uint32                 
 * 大小的长度字（网络字节序）开始，然后          
 * 紧跟该长度的加密数据。解密后产生与           
 * 不使用加密时相同的数据流。     
 *
 * 加密数据通常比相同的未加密数据更大，因此    
 * 我们使用固定大小的缓冲区来处理加密/      
 * 解密，这些缓冲区比 PQComm 的缓冲区通常要        
 * 大，以最小化我们必须为单个读/写调用             
 * 创建多个数据包（从而进行多次 recv/send        
 * 调用）的情况。  
 *
 * 注意：客户端和服务器必须就最大数据包大小     
 * 达成一致，因为我们必须一次整包传给 GSSAPI，      
 * 而且我们不希望对方发送任意巨大的数据包，    
 * 否则我们将不得不为其分配内存再传给 GSSAPI。      
 *
 * 因此，此 #define 实为协议规范的一部分，     
 * 不可更改。                
 */
#define PQ_GSS_MAX_PACKET_SIZE 16384	/* 包含 uint32 头部字     */

/*
 * 但是，在认证交换期间，我们必须处理 GSSAPI            
 * 库想要发送的任何消息大小（因为我们的             
 * 协议不支持分割这些消息）。取决于配置，             
 * 这些消息可能高达 64kB。        
 */
#define PQ_GSS_AUTH_BUFFER_SIZE 65536	/* 包含 uint32 头部字     */

/*
 * 我们需要每个连接的状态变量。为了让此文件         
 * 的函数看起来与 be-secure-gssapi.c 中的相似，设置了   
 * 这些宏。 
 */
#define PqGSSSendBuffer (conn->gss_SendBuffer)
#define PqGSSSendLength (conn->gss_SendLength)
#define PqGSSSendNext (conn->gss_SendNext)
#define PqGSSSendConsumed (conn->gss_SendConsumed)
#define PqGSSRecvBuffer (conn->gss_RecvBuffer)
#define PqGSSRecvLength (conn->gss_RecvLength)
#define PqGSSResultBuffer (conn->gss_ResultBuffer)
#define PqGSSResultLength (conn->gss_ResultLength)
#define PqGSSResultNext (conn->gss_ResultNext)
#define PqGSSMaxPktSize (conn->gss_MaxPktSize)


/*
 * 尝试将 len 字节的数据从 ptr 写入 GSSAPI 加密连接。           
 *
 * 连接必须已经设置为 GSSAPI 加密（即 GSSAPI 传输协商      
 * 已完成）。                    
 *
 * 成功时，返回消耗的数据字节数（可能小于 len）。      
 * 失败时，返回 -1 并适当设置 errno。如果 errno 指示        
 * 不可重试的错误，则向 conn->errorMessage 添加一条消息。   
 * 可重试错误时，调用者应在套接字就绪后再次调用（传
 * 递相同或更多数据）。 
 */
ssize_t
pg_GSS_write(PGconn *conn, const void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output = GSS_C_EMPTY_BUFFER;
	ssize_t		ret = -1;
	size_t		bytes_to_encrypt;
	size_t		bytes_encrypted;
	gss_ctx_id_t gctx = conn->gctx;

	/*当我们遇到可重试的失败时，我们不能告诉调用者我们已成功传输
	 * 了所有内容，否则它不会重试。为简单起见，我们声称在成功传输
	 * 所有 "len" 字节之前没有传输任何内容。在两次调用之间，当前
	 * 输入数据中已被加密并放入 PqGSSSendBuffer（可能已传输）的数
	 * 量记录在 PqGSSSendConsumed 中。重试时，调用者*必须*再次发
	 * 送该数据，因此如果它提供的 len 小于该值，则出错了。
	 * 
	 * 注意：一旦我们成功发送了任何加密数据包，报告部分写入完成
	 * 可能看起来很有吸引力。然而，这样做会扩展此处理的状态空间，
	 *并且过去曾导致错误（参见提交 d053a879b）。通常，通过让调用
	 * 者提前丢弃数据不会节省太多，所以不要冒这个风险。
	 * 
	 * 
	 * 
	 * 
	                                                     */
	if (len < PqGSSSendConsumed)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 "GSSAPI caller failed to retransmit all data needing to be retried\n");
		SOCK_ERRNO_SET(EINVAL);
		return -1;
	}

	/* 扣除我们已经加密的源数据。             */
	bytes_to_encrypt = len - PqGSSSendConsumed;
	bytes_encrypted = PqGSSSendConsumed;

	/*
	 * 循环加密数据并发送，直到全部完成或                   
	 * pqsecure_raw_write() 报错（这可能意味着套接字是非阻塞  
	 * 的且请求的 send() 会阻塞，或者发生了某种               
	 * 实际错误）。    
	 */
	while (bytes_to_encrypt || PqGSSSendLength)
	{
		int			conf_state = 0;
		uint32		netlen;

		/*
		 * 检查加密输出缓冲区中是否有需要发送的            
		 * 数据（可能是上次调用遗留的），如果有，        
		 * 尝试发送。若无法发送，则将此事返回给调用者。
		 *        
		 */
		if (PqGSSSendLength)
		{
			ssize_t		retval;
			ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

			retval = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendNext, amount);
			if (retval <= 0)
				return retval;

			/*
			 * 检查这是否是部分写入，如果是，则在缓冲区   
			 * 中前进那么多并重试。  
			 */
			if (retval < amount)
			{
				PqGSSSendNext += retval;
				continue;
			}

			/* 我们已成功发送缓冲区中的所有数据。      */
			PqGSSSendLength = PqGSSSendNext = 0;
		}

		/*
		 * 检查是否还有待加密的字节。若没有，就完成了。
		 */
		if (!bytes_to_encrypt)
			break;

		/*
		 * 检查我们要发送多少数据，如果太多，则必        
		 * 须循环，并且可能需要多次调用才能处理完        
		 * 所有数据。      
		 */
		if (bytes_to_encrypt > PqGSSMaxPktSize)
			input.length = PqGSSMaxPktSize;
		else
			input.length = bytes_to_encrypt;

		input.value = (char *) ptr + bytes_encrypted;

		output.value = NULL;
		output.length = 0;

		/*
		 * 创建下一个加密数据包。这里的任何失败都视为    
		 * 硬失败，因此即使已发出部分数据也返回 -1。  
		 */
		major = gss_wrap(&minor, gctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI wrap error"), conn, major, minor);
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			goto cleanup;
		}

		if (conf_state == 0)
		{
			libpq_append_conn_error(conn, "outgoing GSSAPI message would not use confidentiality");
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			goto cleanup;
		}

		if (output.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "client tried to send oversize GSSAPI packet (%zu > %zu)",
									(size_t) output.length,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32));
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			goto cleanup;
		}

		bytes_encrypted += input.length;
		bytes_to_encrypt -= input.length;
		PqGSSSendConsumed += input.length;

		/* 4 个网络序长度字节，然后是负载  */
		netlen = pg_hton32(output.length);
		memcpy(PqGSSSendBuffer + PqGSSSendLength, &netlen, sizeof(uint32));
		PqGSSSendLength += sizeof(uint32);

		memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
		PqGSSSendLength += output.length;

		/* 释放 GSSAPI 分配的缓冲区存储     */
		gss_release_buffer(&minor, &output);
	}

	/* 若走到这里，各计数器应全对得上。  */
	Assert(len == PqGSSSendConsumed);
	Assert(len == bytes_encrypted);

	/* 数据全部报告为已发送，故重置 PqGSSSendConsumed。   */
	PqGSSSendConsumed = 0;

	ret = bytes_encrypted;

cleanup:
	/* 释放 GSSAPI 缓冲区存储（若尚未释放）  */
	if (output.value != NULL)
		gss_release_buffer(&minor, &output);
	return ret;
}

/*
 * 从 GSSAPI 加密连接中读取最多 len 字节的数据到 ptr。     
 *
 * 连接必须已经设置为 GSSAPI 加密（即 GSSAPI 传输协商      
 * 已完成）。                    
 *
 * 返回读取的数据字节数，失败时返回 -1 并         
 * 适当设置 errno。若 errno 指示不可重试的错误，        
 * 则向 conn->errorMessage 添加消息。对于可重试的错误，   
 * 调用者应在套接字就绪后再次调用。  
 */
ssize_t
pg_GSS_read(PGconn *conn, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;
	ssize_t		ret;
	size_t		bytes_returned = 0;
	gss_ctx_id_t gctx = conn->gctx;

	/*
	 * 这里的计划是将一个传入的加密数据包读入  
	 *  PqGSSRecvBuffer，解密到 PqGSSResultBuffer 中，然后          
	 * 从中分发数据给调用者。当前数据包用尽           
	 * 时读取下一个。
	 */
	while (bytes_returned < len)
	{
		int			conf_state = 0;

		/* 检查缓冲区中是否有可以立即返回的数据             */
		if (PqGSSResultNext < PqGSSResultLength)
		{
			size_t		bytes_in_buffer = PqGSSResultLength - PqGSSResultNext;
			size_t		bytes_to_copy = Min(bytes_in_buffer, len - bytes_returned);

			/*将数据从我们的结果缓冲区复制到调用者的缓冲区中，
	位置是上次填充其缓冲区时离开的位置。
	
	      */
			memcpy((char *) ptr + bytes_returned, PqGSSResultBuffer + PqGSSResultNext, bytes_to_copy);
			PqGSSResultNext += bytes_to_copy;
			bytes_returned += bytes_to_copy;

			/*
			 * 此时，要么已填满调用者的缓冲区，要么   
			 * 已清空结果缓冲区。无论哪种情况，都返回    
			 * 给调用者。在第二种情况下，我们可以尝试      
			 * 读取另一个加密数据包，但很可能没有可用      
			 * 的。（如果不是这样，说明我们选的最大包 
			 * 大小太小。）无论如何，让调用者处理已返回的
			 * 数据无害。  
			 */
			break;
		}

		/* 结果缓冲区为空，故重置缓冲区指针 */
		PqGSSResultLength = PqGSSResultNext = 0;

		/*
		 * 因为我们选择在发出一些数据后立即返回，      
		 * 所以此时 bytes_returned 必须为零。因此下面         
		 * 的失败退出可以直接返回 -1，而无需担心是否     
		 * 已经发出了一些数据。       
		 */
		Assert(bytes_returned == 0);

		/*
		 * 此时，结果缓冲区为空，且请求读取更多字节。
		 * 我们现在准备加载下一个数据包并将其（完          
		 * 整地）解密到结果缓冲区中。      
		 */

		/* 如果尚未获取，则收集长度     */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
									sizeof(uint32) - PqGSSRecvLength);

			/* 若 ret <= 0，pqsecure_raw_read 已设置了正确的 errno */
			if (ret <= 0)
				return ret;

			PqGSSRecvLength += ret;

			/* 如果仍未获取到长度，则返回给调用者      */
			if (PqGSSRecvLength < sizeof(uint32))
			{
				SOCK_ERRNO_SET(EWOULDBLOCK);
				return -1;
			}
		}

		/* 解码数据包长度并检查是否超长               */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);

		if (input.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "oversize GSSAPI packet sent by the server (%zu > %zu)",
									(size_t) input.length,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32));
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			return -1;
		}

		/*在本次调用中尽可能多地读取数据包，从上次调用的
	中断处继续。
	
	                                        */
		ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
								input.length - (PqGSSRecvLength - sizeof(uint32)));
		/* 若 ret <= 0，pqsecure_raw_read 已设置了正确的 errno */
		if (ret <= 0)
			return ret;

		PqGSSRecvLength += ret;

		/* 如果尚未获得完整数据包，则返回给调用者   */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
		{
			SOCK_ERRNO_SET(EWOULDBLOCK);
			return -1;
		}

		/*
		 * 现在有了完整的数据包，可以执行解密并重        
		 * 新填充结果缓冲区，然后循环回去将数据传       
		 * 回给调用者。请注意下面的错误退出必须处      
		 * 理释放 gss 输出缓冲区。
		 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, gctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI unwrap error"), conn,
						 major, minor);
			ret = -1;
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			goto cleanup;
		}

		if (conf_state == 0)
		{
			libpq_append_conn_error(conn, "incoming GSSAPI message did not use confidentiality");
			ret = -1;
			SOCK_ERRNO_SET(EIO);	/* 没有更好的选择     */
			goto cleanup;
		}

		memcpy(PqGSSResultBuffer, output.value, output.length);
		PqGSSResultLength = output.length;

		/* 接收缓冲区现在为空，重置它   */
		PqGSSRecvLength = 0;

		/* 释放 GSSAPI 分配的缓冲区存储     */
		gss_release_buffer(&minor, &output);
	}

	ret = bytes_returned;

cleanup:
	/* 释放 GSSAPI 缓冲区存储（若尚未释放）  */
	if (output.value != NULL)
		gss_release_buffer(&minor, &output);
	return ret;
}

/*
 * 读取 pqsecure_raw_read 的简单包装器。    
 *
 * 此函数的参数与 pqsecure_raw_read 相同，外加一个输出参      
 * 数来返回读取的字节数。此函数处理可能发生的阻塞以及  
 * 检测到连接 EOF 的情况。   
 */
static PostgresPollingStatusType
gss_read(PGconn *conn, void *recv_buffer, size_t length, ssize_t *ret)
{
	*ret = pqsecure_raw_read(conn, recv_buffer, length);
	if (*ret < 0)
	{
		if (SOCK_ERRNO == EAGAIN || SOCK_ERRNO == EWOULDBLOCK ||
			SOCK_ERRNO == EINTR)
			return PGRES_POLLING_READING;
		else
			return PGRES_POLLING_FAILED;
	}

	/* 检查 EOF    */
	if (*ret == 0)
	{
		int			result = pqReadReady(conn);

		if (result < 0)
			return PGRES_POLLING_FAILED;

		if (!result)
			return PGRES_POLLING_READING;

		*ret = pqsecure_raw_read(conn, recv_buffer, length);
		if (*ret < 0)
		{
			if (SOCK_ERRNO == EAGAIN || SOCK_ERRNO == EWOULDBLOCK ||
				SOCK_ERRNO == EINTR)
				return PGRES_POLLING_READING;
			else
				return PGRES_POLLING_FAILED;
		}
		if (*ret == 0)
			return PGRES_POLLING_FAILED;
	}

	return PGRES_POLLING_OK;
}

/*为连接协商 GSSAPI 传输。完成时返回 PGRES_POLLING_OK。
 * 阻塞时适当返回 PGRES_POLLING_READING 或
 * PGRES_POLLING_WRITING，无法协商传输时返回
 * PGRES_POLLING_FAILED。
 * 
                                                           */
PostgresPollingStatusType
pqsecure_open_gss(PGconn *conn)
{
	ssize_t		ret;
	OM_uint32	major,
				minor,
				gss_flags = GSS_REQUIRED_FLAGS;
	uint32		netlen;
	PostgresPollingStatusType result;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;

	/*
	 * 如果是此连接的第一次调用，分配缓冲区并      
	 * 初始化状态变量。通过分别 malloc 缓冲区，             
	 * 我们确保它们对此文件中某些地方的长度字             
	 * 访问具有足够的对齐。         
	 *
	 * 在传输协商完成前，会使用 PQ_GSS_AUTH_BUFFER_SIZE   
	 * 的缓冲区，之后切换到 PQ_GSS_MAX_PACKET_SIZE。       
	 */
	if (PqGSSSendBuffer == NULL)
	{
		PqGSSSendBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		PqGSSRecvBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		PqGSSResultBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}
		PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
		PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;
	}

	/*
	 * 检查上次调用是否有待发送的数据，若有，则发送。  
	 */
	if (PqGSSSendLength)
	{
		ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

		ret = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendNext, amount);
		if (ret < 0)
		{
			if (SOCK_ERRNO == EAGAIN || SOCK_ERRNO == EWOULDBLOCK ||
				SOCK_ERRNO == EINTR)
				return PGRES_POLLING_WRITING;
			else
				return PGRES_POLLING_FAILED;
		}

		if (ret < amount)
		{
			PqGSSSendNext += ret;
			return PGRES_POLLING_WRITING;
		}

		PqGSSSendLength = PqGSSSendNext = 0;
	}

	/*
	 * 客户端首先发送，发送会创建上下文，因此             
	 * 第一次执行时此处为 false，再次被调用时我们         
	 * 将检查传入数据。     
	 */
	if (conn->gctx)
	{
		/* 处理可能已有的传入数据       */

		/* 查看是否仍在尝试获取长度         */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			/* 首先尝试获取长度        */
			result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength, sizeof(uint32) - PqGSSRecvLength, &ret);
			if (result != PGRES_POLLING_OK)
				return result;

			PqGSSRecvLength += ret;

			if (PqGSSRecvLength < sizeof(uint32))
				return PGRES_POLLING_READING;
		}

		/*
		 * 检查是否收到错误包    
		 *
		 * 这样做是安全的，因为数据包不应超过 8192，因此  
		 * 任何真实数据包的长度字节（网络字节序）    
		 * 都会以两个零字节开头。                               
		 *       
		 */
		if (PqGSSRecvBuffer[0] == 'E')
		{
			/*
			 * 对于启动期间的错误数据包，我们不会得到    
			 * 长度，因此只需尽可能多地读入缓冲区（作为   
			 * 字符串，末尾也为 NULL 字节留出位置）并报      
			 * 告给调用者。 
			 */
			result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength, PQ_GSS_AUTH_BUFFER_SIZE - PqGSSRecvLength - 1, &ret);
			if (result != PGRES_POLLING_OK)
				return result;

			PqGSSRecvLength += ret;

			Assert(PqGSSRecvLength < PQ_GSS_AUTH_BUFFER_SIZE);
			PqGSSRecvBuffer[PqGSSRecvLength] = '\0';
			appendPQExpBuffer(&conn->errorMessage, "%s\n", PqGSSRecvBuffer + 1);

			return PGRES_POLLING_FAILED;
		}

		/*
		 * 此时应该已经获得完整长度，因此提取它，        
		 * 然后读取数据包的剩余部分         
		 */

		/* 获取长度并检查是否超长               */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);
		if (input.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "oversize GSSAPI packet sent by the server (%zu > %zu)",
									(size_t) input.length,
									PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32));
			return PGRES_POLLING_FAILED;
		}

		/*在本次调用中尽可能多地读取数据包，从上次调用的
	中断处继续。
	
	                                        */
		result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
						  input.length - (PqGSSRecvLength - sizeof(uint32)), &ret);
		if (result != PGRES_POLLING_OK)
			return result;

		PqGSSRecvLength += ret;

		/*
		 * 如果获取的数据少于数据包剩余部分，则需返     
		 * 回再次调用。  
		 */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
			return PGRES_POLLING_READING;

		input.value = PqGSSRecvBuffer + sizeof(uint32);
	}

	/* 加载服务名（已完成则无操作）   */
	ret = pg_GSS_load_servicename(conn);
	if (ret != STATUS_OK)
		return PGRES_POLLING_FAILED;

	if (conn->gssdelegation && conn->gssdelegation[0] == '1')
	{
		/* 如果可能，获取凭证     */
		if (conn->gcred == GSS_C_NO_CREDENTIAL)
			(void) pg_GSS_have_cred_cache(&conn->gcred);

		/*
		 * 我们有凭证且 gssdelegation 已启用，因此请求   
		 * 凭证委派。这可能会也可能不会实际导致凭     
		 * 证被委派——取决于凭证中是否设置了可转发      
		 * 标志以及服务器是否配置为接受委派凭证。        
		 *                              
		 */
		if (conn->gcred != GSS_C_NO_CREDENTIAL)
			gss_flags |= GSS_C_DELEG_FLAG;
	}

	/*
	 * 调用 GSS init context，输入为空，或用来自服务器的    
	 * 完整数据包。     
	 */
	major = gss_init_sec_context(&minor, conn->gcred, &conn->gctx,
								 conn->gtarg_nam, GSS_C_NO_OID,
								 gss_flags, 0, 0, &input, NULL,
								 &output, NULL, NULL);

	/* GSS Init Sec Context 用整个数据包，故清空它  */
	PqGSSRecvLength = 0;

	if (GSS_ERROR(major))
	{
		pg_GSS_error(libpq_gettext("could not initiate GSSAPI security context"),
					 conn, major, minor);
		return PGRES_POLLING_FAILED;
	}

	if (output.length == 0)
	{
		/*
		 * 完成了——太好了！设置标志通知低级 I/O 例程   
		 * 执行 GSS 包装/解包。   
		 */
		conn->gssenc = true;
		conn->gssapi_used = true;

		/* 清理   */
		gss_release_cred(&minor, &conn->gcred);
		conn->gcred = GSS_C_NO_CREDENTIAL;
		gss_release_buffer(&minor, &output);

		/*
		 * 释放大型认证缓冲区，并分配我们用于正常        
		 * 操作的缓冲区。（此操作安全的前提是            
		 * pqDropConnection 会释放缓冲区；否则重连期间   
		 * 我们可能会在协商中使用过小的缓冲区。）      
		 *              
		 */
		free(PqGSSSendBuffer);
		free(PqGSSRecvBuffer);
		free(PqGSSResultBuffer);
		PqGSSSendBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		PqGSSRecvBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		PqGSSResultBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}
		PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
		PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;

		/*确定在考虑长度后适合缓冲区的最大数据包大小。
	pg_GSS_write 将需要此值。
	
	                                  */
		major = gss_wrap_size_limit(&minor, conn->gctx, 1, GSS_C_QOP_DEFAULT,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32),
									&PqGSSMaxPktSize);

		if (GSS_ERROR(major))
		{
			pg_GSS_error(libpq_gettext("GSSAPI size check error"), conn,
						 major, minor);
			return PGRES_POLLING_FAILED;
		}

		return PGRES_POLLING_OK;
	}

	/* 必须 output.length > 0    */
	if (output.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
	{
		libpq_append_conn_error(conn, "client tried to send oversize GSSAPI packet (%zu > %zu)",
								(size_t) output.length,
								PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32));
		gss_release_buffer(&minor, &output);
		return PGRES_POLLING_FAILED;
	}

	/* 将令牌排队等待写入 */
	netlen = pg_hton32(output.length);

	memcpy(PqGSSSendBuffer, (char *) &netlen, sizeof(uint32));
	PqGSSSendLength += sizeof(uint32);

	memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
	PqGSSSendLength += output.length;

	/* 这里我们不关心 PqGSSSendConsumed     */

	/* 释放 GSSAPI 分配的缓冲区存储     */
	gss_release_buffer(&minor, &output);

	/* 请求再次调用以写入数据    */
	return PGRES_POLLING_WRITING;
}

/*GSSAPI 信息函数。
 * 
        */

/*返回 GSSAPI 上下文本身。
 * 
  */
void *
PQgetgssctx(PGconn *conn)
{
	if (!conn)
		return NULL;

	return conn->gctx;
}

/*
 * 正在使用 GSSAPI 加密则返回 true。
 */
int
PQgssEncInUse(PGconn *conn)
{
	if (!conn || !conn->gctx)
		return 0;

	return conn->gssenc;
}
