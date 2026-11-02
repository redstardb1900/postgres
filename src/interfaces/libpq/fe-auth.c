/*-------------------------------------------------------------------------
 *
 * fe-auth.c
 *	   The front-end (client) authorization routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * 接口例程
 *	  前端（客户端）例程：
 *		pg_fe_sendauth			发送认证信息
 *		pg_fe_getauthname		根据客户端认证系统获取用户名
 *
 */

#include "postgres_fe.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/param.h>			/* 大多数系统上的 MAXHOSTNAMELEN */
#include <sys/socket.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* 某些系统上的 MAXHOSTNAMELEN */
#endif
#endif

#include "common/md5.h"
#include "common/scram-common.h"
#include "fe-auth.h"
#include "fe-auth-sasl.h"
#include "libpq-fe.h"

#ifdef ENABLE_GSS
/*
 * GSSAPI 认证系统。
 */

#include "fe-gssapi-common.h"

/*
 * 根据需要继续 GSS 认证，处理下一个令牌。
 */
static int
pg_GSS_continue(PGconn *conn, int payloadlen)
{
	OM_uint32	maj_stat,
				min_stat,
				lmin_s,
				gss_flags = GSS_C_MUTUAL_FLAG;
	gss_buffer_desc ginbuf;
	gss_buffer_desc goutbuf;

	/*
	 * 首次调用时没有输入令牌。后续调用时，读取
	 * 输入令牌到 GSS 缓冲区中。
	 */
	if (conn->gctx != GSS_C_NO_CONTEXT)
	{
		ginbuf.length = payloadlen;
		ginbuf.value = malloc(payloadlen);
		if (!ginbuf.value)
		{
			libpq_append_conn_error(conn, "out of memory allocating GSSAPI buffer (%d)",
									payloadlen);
			return STATUS_ERROR;
		}
		if (pqGetnchar(ginbuf.value, payloadlen, conn))
		{
			/*
			 * 不应发生，因为调用者应已确保整个消息已在
			 * 输入缓冲区中。
			 */
			free(ginbuf.value);
			return STATUS_ERROR;
		}
	}
	else
	{
		ginbuf.length = 0;
		ginbuf.value = NULL;
	}

	/* 仅在未禁用 GSS 委派时尝试获取凭据。 */
	if (!pg_GSS_have_cred_cache(&conn->gcred))
		conn->gcred = GSS_C_NO_CREDENTIAL;

	if (conn->gssdelegation && conn->gssdelegation[0] == '1')
		gss_flags |= GSS_C_DELEG_FLAG;

	maj_stat = gss_init_sec_context(&min_stat,
									conn->gcred,
									&conn->gctx,
									conn->gtarg_nam,
									GSS_C_NO_OID,
									gss_flags,
									0,
									GSS_C_NO_CHANNEL_BINDINGS,
									(ginbuf.value == NULL) ? GSS_C_NO_BUFFER : &ginbuf,
									NULL,
									&goutbuf,
									NULL,
									NULL);

	free(ginbuf.value);

	if (goutbuf.length != 0)
	{
		/*
		 * GSS 生成了要发送到服务器的数据。我们不在乎是
		 * 第一个还是后续的包，只需发送相同类型的
		 * 密码包。
		 */
		if (pqPacketSend(conn, 'p',
						 goutbuf.value, goutbuf.length) != STATUS_OK)
		{
			gss_release_buffer(&lmin_s, &goutbuf);
			return STATUS_ERROR;
		}
	}
	gss_release_buffer(&lmin_s, &goutbuf);

	if (maj_stat != GSS_S_COMPLETE && maj_stat != GSS_S_CONTINUE_NEEDED)
	{
		pg_GSS_error(libpq_gettext("GSSAPI continuation error"),
					 conn,
					 maj_stat, min_stat);
		gss_release_name(&lmin_s, &conn->gtarg_nam);
		if (conn->gctx)
			gss_delete_sec_context(&lmin_s, &conn->gctx, GSS_C_NO_BUFFER);
		return STATUS_ERROR;
	}

	if (maj_stat == GSS_S_COMPLETE)
	{
		conn->client_finished_auth = true;
		gss_release_name(&lmin_s, &conn->gtarg_nam);
		conn->gssapi_used = true;
	}

	return STATUS_OK;
}

/*
 * 发送初始 GSS 认证令牌
 */
static int
pg_GSS_startup(PGconn *conn, int payloadlen)
{
	int			ret;
	char	   *host = conn->connhost[conn->whichhost].host;

	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return STATUS_ERROR;
	}

	if (conn->gctx)
	{
		libpq_append_conn_error(conn, "duplicate GSS authentication request");
		return STATUS_ERROR;
	}

	ret = pg_GSS_load_servicename(conn);
	if (ret != STATUS_OK)
		return ret;

	/*
	 * 初始包与没有初始上下文的继续包相同。
	 *
	 */
	conn->gctx = GSS_C_NO_CONTEXT;

	return pg_GSS_continue(conn, payloadlen);
}
#endif							/* ENABLE_GSS */


#ifdef ENABLE_SSPI
/*
 * SSPI 认证系统（仅 Windows）
 */

static void
pg_SSPI_error(PGconn *conn, const char *mprefix, SECURITY_STATUS r)
{
	char		sysmsg[256];

	if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, r, 0,
					  sysmsg, sizeof(sysmsg), NULL) == 0)
		appendPQExpBuffer(&conn->errorMessage, "%s: SSPI error %x\n",
						  mprefix, (unsigned int) r);
	else
		appendPQExpBuffer(&conn->errorMessage, "%s: %s (%x)\n",
						  mprefix, sysmsg, (unsigned int) r);
}

/*
 * 根据需要继续 SSPI 认证，处理下一个令牌。
 */
static int
pg_SSPI_continue(PGconn *conn, int payloadlen)
{
	SECURITY_STATUS r;
	CtxtHandle	newContext;
	ULONG		contextAttr;
	SecBufferDesc inbuf;
	SecBufferDesc outbuf;
	SecBuffer	OutBuffers[1];
	SecBuffer	InBuffers[1];
	char	   *inputbuf = NULL;

	if (conn->sspictx != NULL)
	{
		/*
		 * 非首次运行时，有一些数据要发送。将这些
		 * 数据放入 SecBuffer 类型的结构中。
		 */
		inputbuf = malloc(payloadlen);
		if (!inputbuf)
		{
			libpq_append_conn_error(conn, "out of memory allocating SSPI buffer (%d)",
									payloadlen);
			return STATUS_ERROR;
		}
		if (pqGetnchar(inputbuf, payloadlen, conn))
		{
			/*
			 * 不应发生，因为调用者应已确保整个消息已在
			 * 输入缓冲区中。
			 */
			free(inputbuf);
			return STATUS_ERROR;
		}

		inbuf.ulVersion = SECBUFFER_VERSION;
		inbuf.cBuffers = 1;
		inbuf.pBuffers = InBuffers;
		InBuffers[0].pvBuffer = inputbuf;
		InBuffers[0].cbBuffer = payloadlen;
		InBuffers[0].BufferType = SECBUFFER_TOKEN;
	}

	OutBuffers[0].pvBuffer = NULL;
	OutBuffers[0].BufferType = SECBUFFER_TOKEN;
	OutBuffers[0].cbBuffer = 0;
	outbuf.cBuffers = 1;
	outbuf.pBuffers = OutBuffers;
	outbuf.ulVersion = SECBUFFER_VERSION;

	r = InitializeSecurityContext(conn->sspicred,
								  conn->sspictx,
								  conn->sspitarget,
								  ISC_REQ_ALLOCATE_MEMORY,
								  0,
								  SECURITY_NETWORK_DREP,
								  (conn->sspictx == NULL) ? NULL : &inbuf,
								  0,
								  &newContext,
								  &outbuf,
								  &contextAttr,
								  NULL);

	/* 不再需要输入数据 */
	free(inputbuf);

	if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
	{
		pg_SSPI_error(conn, libpq_gettext("SSPI continuation error"), r);

		return STATUS_ERROR;
	}

	if (conn->sspictx == NULL)
	{
		/* 首次运行，传递已检索的上下文句柄 */
		conn->sspictx = malloc(sizeof(CtxtHandle));
		if (conn->sspictx == NULL)
		{
			libpq_append_conn_error(conn, "out of memory");
			return STATUS_ERROR;
		}
		memcpy(conn->sspictx, &newContext, sizeof(CtxtHandle));
	}

	/*
	 * 如果 SSPI 返回了要发送到服务器的任何数据（通常如此），
	 * 将此数据作为密码包发送。
	 */
	if (outbuf.cBuffers > 0)
	{
		if (outbuf.cBuffers != 1)
		{
			/*
			 * 这应该永远不会发生，至少对 Kerberos 认证不会。
			 * 保留此检查以防将来与其他认证方法
			 * 一起使用时出现。
			 */
			appendPQExpBufferStr(&conn->errorMessage,
								 "SSPI returned invalid number of output buffers\n");
			return STATUS_ERROR;
		}

		/*
		 * 如果协商完成，可能有零字节要发送。
		 * 服务器此时不期望任何更多数据，所以不要
		 * 发送它。
		 */
		if (outbuf.pBuffers[0].cbBuffer > 0)
		{
			if (pqPacketSend(conn, 'p',
							 outbuf.pBuffers[0].pvBuffer, outbuf.pBuffers[0].cbBuffer))
			{
				FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
				return STATUS_ERROR;
			}
		}
		FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
	}

	if (r == SEC_E_OK)
		conn->client_finished_auth = true;

	/* 清理由 freePGconn() 中的代码处理 */
	return STATUS_OK;
}

/*
 * 发送初始 SSPI 认证令牌。
 * 如果 use_negotiate 为 0，使用与 Unix 兼容的 Kerberos 认证包。
 * 如果 use_negotiate 为 1，使用协商包，它同时支持 Kerberos 和
 * NTLM，但与 Unix 不兼容。
 */
static int
pg_SSPI_startup(PGconn *conn, int use_negotiate, int payloadlen)
{
	SECURITY_STATUS r;
	TimeStamp	expire;
	char	   *host = conn->connhost[conn->whichhost].host;

	if (conn->sspictx)
	{
		libpq_append_conn_error(conn, "duplicate SSPI authentication request");
		return STATUS_ERROR;
	}

	/*
	 * 检索凭据句柄
	 */
	conn->sspicred = malloc(sizeof(CredHandle));
	if (conn->sspicred == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return STATUS_ERROR;
	}

	r = AcquireCredentialsHandle(NULL,
								 use_negotiate ? "negotiate" : "kerberos",
								 SECPKG_CRED_OUTBOUND,
								 NULL,
								 NULL,
								 NULL,
								 NULL,
								 conn->sspicred,
								 &expire);
	if (r != SEC_E_OK)
	{
		pg_SSPI_error(conn, libpq_gettext("could not acquire SSPI credentials"), r);
		free(conn->sspicred);
		conn->sspicred = NULL;
		return STATUS_ERROR;
	}

	/*
	 * 计算目标主体名称。SSPI 的格式与 GSSAPI 不同，
	 * 但并不更复杂。我们可以跳过 @REALM 部分，因为 Windows
	 * 会自动为我们填充。
	 */
	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return STATUS_ERROR;
	}
	conn->sspitarget = malloc(strlen(conn->krbsrvname) + strlen(host) + 2);
	if (!conn->sspitarget)
	{
		libpq_append_conn_error(conn, "out of memory");
		return STATUS_ERROR;
	}
	sprintf(conn->sspitarget, "%s/%s", conn->krbsrvname, host);

	/*
	 * 指示我们处于 SSPI 认证模式，以确保在下一次协商时
	 * 调用 pg_SSPI_continue。
	 */
	conn->usesspi = 1;

	return pg_SSPI_continue(conn, payloadlen);
}
#endif							/* ENABLE_SSPI */

/*
 * 初始化 SASL 认证交换。
 */
static int
pg_SASL_init(PGconn *conn, int payloadlen)
{
	char	   *initialresponse = NULL;
	int			initialresponselen;
	bool		done;
	bool		success;
	const char *selected_mechanism;
	PQExpBufferData mechanism_buf;
	char	   *password;

	initPQExpBuffer(&mechanism_buf);

	if (conn->channel_binding[0] == 'r' &&	/* require — 要求 */
		!conn->ssl_in_use)
	{
		libpq_append_conn_error(conn, "channel binding required, but SSL not in use");
		goto error;
	}

	if (conn->sasl_state)
	{
		libpq_append_conn_error(conn, "duplicate SASL authentication request");
		goto error;
	}

	/*
	 * 解析 AuthenticationSASL 消息中的 SASL 认证机制列表，
	 * 并选择我们支持的最佳机制。目前仅支持
	 * SCRAM-SHA-256-PLUS 和 SCRAM-SHA-256，按重要性
	 * 降序排列。
	 */
	selected_mechanism = NULL;
	for (;;)
	{
		if (pqGets(&mechanism_buf, conn))
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 "fe_sendauth: invalid authentication request from server: invalid list of authentication mechanisms\n");
			goto error;
		}
		if (PQExpBufferDataBroken(mechanism_buf))
			goto oom_error;

		/* 空字符串表示列表结束 */
		if (mechanism_buf.data[0] == '\0')
			break;

		/*
		 * 选择要使用的机制。如果设置了通道绑定类型且
		 * 客户端支持（且未设置 channel_binding=disable），
		 * 优先选择 SCRAM-SHA-256-PLUS。如果没有选中其他机制，
		 * 则选择 SCRAM-SHA-256。如果添加更多机制，
		 * 可能需要更精细的优先级机制。
		 */
		if (strcmp(mechanism_buf.data, SCRAM_SHA_256_PLUS_NAME) == 0)
		{
			if (conn->ssl_in_use)
			{
				/* 服务器已提供 SCRAM-SHA-256-PLUS。 */

#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
				/*
				 * 客户端支持通道绑定，如果 channel_binding
				 * 未被禁用，则选择它。
				 */
				if (conn->channel_binding[0] != 'd')	/* disable — 禁用 */
				{
					selected_mechanism = SCRAM_SHA_256_PLUS_NAME;
					conn->sasl = &pg_scram_mech;
				}
#else
				/*
				 * 客户端不支持通道绑定。如果需要通道绑定，
				 * 立即报错，而不是在下面产生令人困惑的错误信息，
				 * 因为服务器已公布了
				 * SCRAM-SHA-256-PLUS。
				 */
				if (conn->channel_binding[0] == 'r')	/* require — 要求 */
				{
					libpq_append_conn_error(conn, "channel binding is required, but client does not support it");
					goto error;
				}
#endif
			}
			else
			{
				/*
				 * 服务器提供了 SCRAM-SHA-256-PLUS，但连接
				 * 不是 SSL 加密的。这不合逻辑。也许 SSL 被代理
				 * 剥离了？继续下去没有意义，因为即使客户端和
				 * 服务器都支持通道绑定，服务器也会拒绝尝试
				 * 不带通道绑定的认证。SCRAM 交换会
				 * 检查这一点，以防止降级攻击。
				 *
				 */
				libpq_append_conn_error(conn, "server offered SCRAM-SHA-256-PLUS authentication over a non-SSL connection");
				goto error;
			}
		}
		else if (strcmp(mechanism_buf.data, SCRAM_SHA_256_NAME) == 0 &&
				 !selected_mechanism)
		{
			selected_mechanism = SCRAM_SHA_256_NAME;
			conn->sasl = &pg_scram_mech;
		}
	}

	if (!selected_mechanism)
	{
		libpq_append_conn_error(conn, "none of the server's SASL authentication mechanisms are supported");
		goto error;
	}

	if (conn->channel_binding[0] == 'r' &&	/* require — 要求 */
		strcmp(selected_mechanism, SCRAM_SHA_256_PLUS_NAME) != 0)
	{
		libpq_append_conn_error(conn, "channel binding is required, but server did not offer an authentication method that supports channel binding");
		goto error;
	}

	/*
	 * SASL 机制已为本次交换选定，
	 * 初始化其状态信息。
	 */

	/*
	 * 首先，选择用于交换的密码。如果没有密码则报错。
	 * 目前，所有支持的 SASL 机制都需要密码，
	 * 所以我们可以直接在这里进行，无需进一步区分。
	 */
	conn->password_needed = true;
	password = conn->connhost[conn->whichhost].password;
	if (password == NULL)
		password = conn->pgpass;
	if (password == NULL || password[0] == '\0')
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 PQnoPasswordSupplied);
		goto error;
	}

	Assert(conn->sasl);

	/*
	 * 使用在初始交换期间收集的所有信息
	 * 初始化 SASL 状态信息。
	 *
	 * 注意：目前仅支持 tls-unique。
	 */
	conn->sasl_state = conn->sasl->init(conn,
										password,
										selected_mechanism);
	if (!conn->sasl_state)
		goto oom_error;

	/* 获取特定于机制的初始客户端响应（如果有的话） */
	conn->sasl->exchange(conn->sasl_state,
						 NULL, -1,
						 &initialresponse, &initialresponselen,
						 &done, &success);

	if (done && !success)
		goto error;

	/*
	 * 构建 SASLInitialResponse 消息并发送。
	 */
	if (pqPutMsgStart('p', conn))
		goto error;
	if (pqPuts(selected_mechanism, conn))
		goto error;
	if (initialresponse)
	{
		if (pqPutInt(initialresponselen, 4, conn))
			goto error;
		if (pqPutnchar(initialresponse, initialresponselen, conn))
			goto error;
	}
	if (pqPutMsgEnd(conn))
		goto error;
	if (pqFlush(conn))
		goto error;

	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);

	return STATUS_OK;

error:
	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);
	return STATUS_ERROR;

oom_error:
	termPQExpBuffer(&mechanism_buf);
	free(initialresponse);
	libpq_append_conn_error(conn, "out of memory");
	return STATUS_ERROR;
}

/*
 * 与后端交换 SASL 通信协议的消息。
 * 应在调用 pg_SASL_init 设置协议状态后使用。
 *
 */
static int
pg_SASL_continue(PGconn *conn, int payloadlen, bool final)
{
	char	   *output;
	int			outputlen;
	bool		done;
	bool		success;
	int			res;
	char	   *challenge;

	/* 从 AuthenticationSASLContinue 消息读取 SASL 质询。 */
	challenge = malloc(payloadlen + 1);
	if (!challenge)
	{
		libpq_append_conn_error(conn, "out of memory allocating SASL buffer (%d)",
								payloadlen);
		return STATUS_ERROR;
	}

	if (pqGetnchar(challenge, payloadlen, conn))
	{
		free(challenge);
		return STATUS_ERROR;
	}
	/* 为安全和方便，确保缓冲区以 NULL 结尾。 */
	challenge[payloadlen] = '\0';

	conn->sasl->exchange(conn->sasl_state,
						 challenge, payloadlen,
						 &output, &outputlen,
						 &done, &success);
	free(challenge);			/* 不再需要输入数据 */

	if (final && !done)
	{
		if (outputlen != 0)
			free(output);

		libpq_append_conn_error(conn, "AuthenticationSASLFinal received from server, but SASL authentication was not completed");
		return STATUS_ERROR;
	}

	/*
	 * 如果交换尚未完成，我们需要确保
	 * SASL 机制已生成要发送回的消息。
	 */
	if (output == NULL && !done)
	{
		libpq_append_conn_error(conn, "no client response found after SASL exchange success");
		return STATUS_ERROR;
	}

	/*
	 * SASL 允许零长度响应，因此此检查使用"output"而非
	 * "outputlen"以允许空消息的情况。
	 */
	if (output)
	{
		/*
		 * 向服务器发送 SASL 响应。
		 */
		res = pqPacketSend(conn, 'p', output, outputlen);
		free(output);

		if (res != STATUS_OK)
			return STATUS_ERROR;
	}

	if (done && !success)
		return STATUS_ERROR;

	return STATUS_OK;
}

static int
pg_password_sendauth(PGconn *conn, const char *password, AuthRequest areq)
{
	int			ret;
	char	   *crypt_pwd = NULL;
	const char *pwd_to_send;
	char		md5Salt[4];

	/* 从 AuthenticationMD5Password 消息读取盐值。 */
	if (areq == AUTH_REQ_MD5)
	{
		if (pqGetnchar(md5Salt, 4, conn))
			return STATUS_ERROR;	/* 不应发生 */
	}

	/* 如果需要则加密密码。 */

	switch (areq)
	{
		case AUTH_REQ_MD5:
			{
				char	   *crypt_pwd2;
				const char *errstr = NULL;

				/* 分配足够空间存放两个 MD5 哈希 */
				crypt_pwd = malloc(2 * (MD5_PASSWD_LEN + 1));
				if (!crypt_pwd)
				{
					libpq_append_conn_error(conn, "out of memory");
					return STATUS_ERROR;
				}

				crypt_pwd2 = crypt_pwd + MD5_PASSWD_LEN + 1;
				if (!pg_md5_encrypt(password, conn->pguser,
									strlen(conn->pguser), crypt_pwd2,
									&errstr))
				{
					libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
					free(crypt_pwd);
					return STATUS_ERROR;
				}
				if (!pg_md5_encrypt(crypt_pwd2 + strlen("md5"), md5Salt,
									4, crypt_pwd, &errstr))
				{
					libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
					free(crypt_pwd);
					return STATUS_ERROR;
				}

				pwd_to_send = crypt_pwd;
				break;
			}
		case AUTH_REQ_PASSWORD:
			pwd_to_send = password;
			break;
		default:
			return STATUS_ERROR;
	}
	ret = pqPacketSend(conn, 'p', pwd_to_send, strlen(pwd_to_send) + 1);
	free(crypt_pwd);
	return ret;
}

/*
 * 将不允许的 AuthRequest 代码转换为错误消息。
 */
static const char *
auth_method_description(AuthRequest areq)
{
	switch (areq)
	{
		case AUTH_REQ_PASSWORD:
			return libpq_gettext("server requested a cleartext password");
		case AUTH_REQ_MD5:
			return libpq_gettext("server requested a hashed password");
		case AUTH_REQ_GSS:
		case AUTH_REQ_GSS_CONT:
			return libpq_gettext("server requested GSSAPI authentication");
		case AUTH_REQ_SSPI:
			return libpq_gettext("server requested SSPI authentication");
		case AUTH_REQ_SASL:
		case AUTH_REQ_SASL_CONT:
		case AUTH_REQ_SASL_FIN:
			return libpq_gettext("server requested SASL authentication");
	}

	return libpq_gettext("server requested an unknown authentication type");
}

/*
 * 检查 allowed_auth_methods 位掩码的便捷宏。调用者
 * 必须确保 type 不大于 31（位掩码的高位）。
 */
#define auth_method_allowed(conn, type) \
	(((conn)->allowed_auth_methods & (1 << (type))) != 0)

/*
 * 验证认证请求是否符合预期，给定连接参数。
 * 当客户端希望在交换任何敏感信息之前
 * 认证服务器时，这一点尤为重要。
 */
static bool
check_expected_areq(AuthRequest areq, PGconn *conn)
{
	bool		result = true;
	const char *reason = NULL;

	StaticAssertDecl((sizeof(conn->allowed_auth_methods) * CHAR_BIT) > AUTH_REQ_MAX,
					 "AUTH_REQ_MAX overflows the allowed_auth_methods bitmask");

	if (conn->sslcertmode[0] == 'r' /* require — 要求 */
		&& areq == AUTH_REQ_OK)
	{
		/*
		 * 牺牲一点复杂性，尽量让这些错误消息
		 * 尽可能精确。
		 */
		if (!conn->ssl_cert_requested)
		{
			libpq_append_conn_error(conn, "server did not request an SSL certificate");
			return false;
		}
		else if (!conn->ssl_cert_sent)
		{
			libpq_append_conn_error(conn, "server accepted connection without a valid SSL certificate");
			return false;
		}
	}

	/*
	 * 如果用户要求了特定的认证方法，或指定了允许的
	 * 集合，那么在此拒绝所有其他方法，并确保服务器实际
	 * 完成了认证交换。
	 */
	if (conn->require_auth)
	{
		switch (areq)
		{
			case AUTH_REQ_OK:

				/*
				 * 确保我们确实完成了交换（或者用户允许了
				 * 无认证的连接）。
				 *
				 * 如果用户同时允许了 SCRAM 和未认证（trust）
				 * 连接，那么此检查将静默接受部分 SCRAM 交换，
				 * 即行为异常的服务器在发送 OK 之前不提供其
				 * 验证器。这与历史行为一致，但可能是未来
				 * 重新审视的一点，因为它可能允许不知道
				 * 用户密码的服务器静默收集暴力攻击的材料。
				 *
				 *
				 *
				 */
				if (!conn->auth_required || conn->client_finished_auth)
					break;

				/*
				 * 服务器没有发出显式的认证请求——
				 * 或者发出了但未完成（在 SCRAM 的情况下）——
				 * 但有一个特殊情况需要检查。如果用户允许了
				 * "gss"，那么 GSS 加密的通道也可以满足
				 * 此检查。
				 */
#ifdef ENABLE_GSS
				if (auth_method_allowed(conn, AUTH_REQ_GSS) && conn->gssenc)
				{
					/*
					 * 如果已经通过 GSS 加密执行了隐式 GSS 认证，
					 * 我们不需要执行 AUTH_REQ_GSS 交换。这允许
					 * require_auth=gss 与 gssencmode 结合使用，
					 * 因为在这种情况下不会有显式的
					 * 认证请求。
					 */
				}
				else
#endif
				{
					reason = libpq_gettext("server did not complete authentication");
					result = false;
				}

				break;

			case AUTH_REQ_PASSWORD:
			case AUTH_REQ_MD5:
			case AUTH_REQ_GSS:
			case AUTH_REQ_GSS_CONT:
			case AUTH_REQ_SSPI:
			case AUTH_REQ_SASL:
			case AUTH_REQ_SASL_CONT:
			case AUTH_REQ_SASL_FIN:

				/*
				 * 我们不使用默认 case 处理这些，以避免
				 * 当服务器发送意外的 AuthRequest 时超出
				 * allowed_auth_methods 掩码的位移范围。
				 */
				result = auth_method_allowed(conn, areq);
				break;

			default:
				result = false;
				break;
		}
	}

	if (!result)
	{
		if (!reason)
			reason = auth_method_description(areq);

		libpq_append_conn_error(conn, "authentication method requirement \"%s\" failed: %s",
								conn->require_auth, reason);
		return result;
	}

	/*
	 * 当 channel_binding=require 时，我们必须防范两种情况：
	 * （1）不响应非 SASL 认证请求，这可能泄露客户端的
	 * 密码等信息；以及（2）即使收到 AUTH_REQ_OK，我们仍需
	 * 确保已发生通道绑定以认证服务器。
	 *
	 */
	if (conn->channel_binding[0] == 'r' /* require — 要求 */ )
	{
		switch (areq)
		{
			case AUTH_REQ_SASL:
			case AUTH_REQ_SASL_CONT:
			case AUTH_REQ_SASL_FIN:
				break;
			case AUTH_REQ_OK:
				if (!conn->sasl || !conn->sasl->channel_bound(conn->sasl_state))
				{
					libpq_append_conn_error(conn, "channel binding required, but server authenticated client without channel binding");
					result = false;
				}
				break;
			default:
				libpq_append_conn_error(conn, "channel binding required but not supported by server's authentication request");
				result = false;
				break;
		}
	}

	return result;
}

/*
 * pg_fe_sendauth
 *		处理认证请求的客户端分发例程
 *
 * 服务器已向我们发送了一个认证质询（或 OK）。发送适当
 * 的响应。调用者已确保整个消息现在在输入缓冲区中，
 * 并已读取其类型和长度。我们负责读取
 * 针对认证方法的任何剩余额外数据。
 * 'payloadlen' 是消息中剩余的长度。
 *
 */
int
pg_fe_sendauth(AuthRequest areq, int payloadlen, PGconn *conn)
{
	int			oldmsglen;

	if (!check_expected_areq(areq, conn))
		return STATUS_ERROR;

	switch (areq)
	{
		case AUTH_REQ_OK:
			break;

		case AUTH_REQ_KRB4:
			libpq_append_conn_error(conn, "Kerberos 4 authentication not supported");
			return STATUS_ERROR;

		case AUTH_REQ_KRB5:
			libpq_append_conn_error(conn, "Kerberos 5 authentication not supported");
			return STATUS_ERROR;

#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
		case AUTH_REQ_GSS:
#if !defined(ENABLE_SSPI)
			/* no native SSPI, so use GSSAPI library for it */
		case AUTH_REQ_SSPI:
#endif
			{
				int			r;

				pglock_thread();

				/*
				 * 如果我们同时编译了 GSS 和 SSPI 支持，默认使用 SSPI
				 * 支持。这可通过连接字符串参数覆盖。注意，
				 * 使用 SSPI 时我们仍然关闭协商参数，因为我们希望
				 * SSPI 使用 GSSAPI Kerberos 协议。对于实际的 SSPI
				 * 协商协议，我们使用
				 * AUTH_REQ_SSPI。
				 */
#if defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				if (conn->gsslib && (pg_strcasecmp(conn->gsslib, "gssapi") == 0))
					r = pg_GSS_startup(conn, payloadlen);
				else
					r = pg_SSPI_startup(conn, 0, payloadlen);
#elif defined(ENABLE_GSS) && !defined(ENABLE_SSPI)
				r = pg_GSS_startup(conn, payloadlen);
#elif !defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				r = pg_SSPI_startup(conn, 0, payloadlen);
#endif
				if (r != STATUS_OK)
				{
					/* 错误消息已填充。 */
					pgunlock_thread();
					return STATUS_ERROR;
				}
				pgunlock_thread();
			}
			break;

		case AUTH_REQ_GSS_CONT:
			{
				int			r;

				pglock_thread();
#if defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				if (conn->usesspi)
					r = pg_SSPI_continue(conn, payloadlen);
				else
					r = pg_GSS_continue(conn, payloadlen);
#elif defined(ENABLE_GSS) && !defined(ENABLE_SSPI)
				r = pg_GSS_continue(conn, payloadlen);
#elif !defined(ENABLE_GSS) && defined(ENABLE_SSPI)
				r = pg_SSPI_continue(conn, payloadlen);
#endif
				if (r != STATUS_OK)
				{
					/* 错误消息已填充。 */
					pgunlock_thread();
					return STATUS_ERROR;
				}
				pgunlock_thread();
			}
			break;
#else							/* defined(ENABLE_GSS) || defined(ENABLE_SSPI) */
			/* No GSSAPI *or* SSPI support */
		case AUTH_REQ_GSS:
		case AUTH_REQ_GSS_CONT:
			libpq_append_conn_error(conn, "GSSAPI authentication not supported");
			return STATUS_ERROR;
#endif							/* defined(ENABLE_GSS) || defined(ENABLE_SSPI) */

#ifdef ENABLE_SSPI
		case AUTH_REQ_SSPI:

			/*
			 * SSPI 有自己的启动消息，因此 libpq 可以决定使用
			 * 哪种方法。告诉 pg_SSPI_startup 我们需要 SSPI
			 * 协商而非 Kerberos。
			 */
			pglock_thread();
			if (pg_SSPI_startup(conn, 1, payloadlen) != STATUS_OK)
			{
				/* 错误消息已填充。 */
				pgunlock_thread();
				return STATUS_ERROR;
			}
			pgunlock_thread();
			break;
#else

			/*
			 * 没有 SSPI 支持。但是，如果我们有 GSSAPI 但没有 SSPI
			 * 支持，AUTH_REQ_SSPI 将在上面的 AUTH_REQ_GSS
			 * 代码路径中处理，因此在这种情况下不要
			 * 重复 case 标签。
			 */
#if !defined(ENABLE_GSS)
		case AUTH_REQ_SSPI:
			libpq_append_conn_error(conn, "SSPI authentication not supported");
			return STATUS_ERROR;
#endif							/* !define(ENABLE_GSS) */
#endif							/* ENABLE_SSPI */


		case AUTH_REQ_CRYPT:
			libpq_append_conn_error(conn, "Crypt authentication not supported");
			return STATUS_ERROR;

		case AUTH_REQ_MD5:
		case AUTH_REQ_PASSWORD:
			{
				char	   *password;

				conn->password_needed = true;
				password = conn->connhost[conn->whichhost].password;
				if (password == NULL)
					password = conn->pgpass;
				if (password == NULL || password[0] == '\0')
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 PQnoPasswordSupplied);
					return STATUS_ERROR;
				}
				if (pg_password_sendauth(conn, password, areq) != STATUS_OK)
				{
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error sending password authentication\n");
					return STATUS_ERROR;
				}

				/* 我们期望不再有进一步的认证请求。 */
				conn->client_finished_auth = true;
				break;
			}

		case AUTH_REQ_SASL:

			/*
			 * 请求包含认证机制的名称
			 * （由 IANA 分配）。
			 */
			if (pg_SASL_init(conn, payloadlen) != STATUS_OK)
			{
				/* pg_SASL_init 已设置错误消息 */
				return STATUS_ERROR;
			}
			break;

		case AUTH_REQ_SASL_CONT:
		case AUTH_REQ_SASL_FIN:
			if (conn->sasl_state == NULL)
			{
				appendPQExpBufferStr(&conn->errorMessage,
									 "fe_sendauth: invalid authentication request from server: AUTH_REQ_SASL_CONT without AUTH_REQ_SASL\n");
				return STATUS_ERROR;
			}
			oldmsglen = conn->errorMessage.len;
			if (pg_SASL_continue(conn, payloadlen,
								 (areq == AUTH_REQ_SASL_FIN)) != STATUS_OK)
			{
				/* 如果 pg_SASL_continue 未提供消息，则使用此消息 */
				if (conn->errorMessage.len == oldmsglen)
					appendPQExpBufferStr(&conn->errorMessage,
										 "fe_sendauth: error in SASL authentication\n");
				return STATUS_ERROR;
			}
			break;

		default:
			libpq_append_conn_error(conn, "authentication method %u not supported", areq);
			return STATUS_ERROR;
	}

	return STATUS_OK;
}


/*
 * pg_fe_getusername
 *
 * 返回指向 malloc 分配空间的指针，包含指定
 * user_id 的名称。如果有错误，返回 NULL，
 * 并追加合适的错误消息到 *errorMessage（如果不为 NULL）。
 *
 * 注意：在 Windows 上忽略 user_id 参数，总是
 * 获取当前用户的名称。
 */
char *
pg_fe_getusername(uid_t user_id, PQExpBuffer errorMessage)
{
	char	   *result = NULL;
	const char *name = NULL;

#ifdef WIN32
	/* Microsoft 建议缓冲区大小为 UNLEN+1，其中 UNLEN = 256 */
	char		username[256 + 1];
	DWORD		namesize = sizeof(username);
#else
	char		pwdbuf[BUFSIZ];
#endif

	/*
	 * 一些用户使用 configure --enable-thread-safety-force，
	 * 所以我们不妨在库内进行锁定以保护 getpwuid()。
	 * 实际上，应用开发者可以在应用中使用 getpwuid()，
	 * 如果他们使用我们提供的锁定调用，或安装
	 * 自己的锁定函数使用 PQregisterThreadLock()。
	 */
	pglock_thread();

#ifdef WIN32
	if (GetUserName(username, &namesize))
		name = username;
	else if (errorMessage)
		libpq_append_error(errorMessage,
						   "user name lookup failure: error code %lu",
						   GetLastError());
#else
	if (pg_get_user_name(user_id, pwdbuf, sizeof(pwdbuf)))
		name = pwdbuf;
	else if (errorMessage)
		appendPQExpBuffer(errorMessage, "%s\n", pwdbuf);
#endif

	if (name)
	{
		result = strdup(name);
		if (result == NULL && errorMessage)
			libpq_append_error(errorMessage, "out of memory");
	}

	pgunlock_thread();

	return result;
}

/*
 * pg_fe_getauthname
 *
 * 返回指向 malloc 分配空间的指针，包含用户已向系统
 * 认证的任何名称。如果有错误，
 * 返回 NULL 并追加合适的错误消息到 *errorMessage（如果不为 NULL）。
 */
char *
pg_fe_getauthname(PQExpBuffer errorMessage)
{
#ifdef WIN32
	return pg_fe_getusername(0, errorMessage);
#else
	return pg_fe_getusername(geteuid(), errorMessage);
#endif
}


/*
 * PQencryptPassword — 使用 MD5 加密密码的导出例程
 *
 * 此函数等同于使用"md5"作为加密方法调用
 * PQencryptPasswordConn，区别在于不需要连接对象。
 * 此函数已弃用，请改用
 * PQencryptPasswordConn。
 */
char *
PQencryptPassword(const char *passwd, const char *user)
{
	char	   *crypt_pwd;
	const char *errstr = NULL;

	crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
	if (!crypt_pwd)
		return NULL;

	if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd, &errstr))
	{
		free(crypt_pwd);
		return NULL;
	}

	return crypt_pwd;
}

/*
 * PQencryptPasswordConn — 加密密码的导出例程
 *
 * 旨在供希望发送 ALTER USER joe PASSWORD 'pwd' 等命令
 * 的客户端应用程序使用。如果在客户端加密，密码
 * 无需以明文发送。这很好，因为它确保
 * 明文密码不会出现在日志、pg_stat 显示等中。
 * 我们导出此函数，以便客户端不依赖于低层细节，
 * 例如加密是 MD5 还是其他算法。
 *
 * 参数为连接对象、明文密码、目标用户的 SQL 名称，
 * 以及表示用于加密密码的算法的字符串。
 * 如果 algorithm 为 NULL，此函数查询服务器的
 * 当前 'password_encryption' 值。如果您希望避免
 * 此操作（例如避免阻塞），可在调用此函数前
 * 自行执行 'show password_encryption' 并将其作为
 * algorithm 传入。
 *
 * 返回值为 malloc 分配的字符串。客户端可假设
 * 该字符串不包含任何需要转义的特殊字符。
 * 出错时，将错误消息存储在连接对象中并
 * 返回 NULL。
 *
 */
char *
PQencryptPasswordConn(PGconn *conn, const char *passwd, const char *user,
					  const char *algorithm)
{
#define MAX_ALGORITHM_NAME_LEN 50
	char		algobuf[MAX_ALGORITHM_NAME_LEN + 1];
	char	   *crypt_pwd = NULL;

	if (!conn)
		return NULL;

	pqClearConnErrorState(conn);

	/* 如果未提供算法，询问服务器。 */
	if (algorithm == NULL)
	{
		PGresult   *res;
		char	   *val;

		res = PQexec(conn, "show password_encryption");
		if (res == NULL)
		{
			/* PQexec() 应已设置 conn->errorMessage */
			return NULL;
		}
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			/* PQexec() 应已设置 conn->errorMessage */
			PQclear(res);
			return NULL;
		}
		if (PQntuples(res) != 1 || PQnfields(res) != 1)
		{
			PQclear(res);
			libpq_append_conn_error(conn, "unexpected shape of result set returned for SHOW");
			return NULL;
		}
		val = PQgetvalue(res, 0, 0);

		if (strlen(val) > MAX_ALGORITHM_NAME_LEN)
		{
			PQclear(res);
			libpq_append_conn_error(conn, "password_encryption value too long");
			return NULL;
		}
		strcpy(algobuf, val);
		PQclear(res);

		algorithm = algobuf;
	}

	/*
	 * 同时接受"on"和"off"作为"md5"的别名，因为
	 * password_encryption 在 PostgreSQL 10 之前是一个布尔值。即使
	 * 值是"off"，我们也拒绝以明文发送密码。
	 */
	if (strcmp(algorithm, "on") == 0 ||
		strcmp(algorithm, "off") == 0)
		algorithm = "md5";

	/*
 * 好，现在我们知道要使用什么算法了
 */
	if (strcmp(algorithm, "scram-sha-256") == 0)
	{
		const char *errstr = NULL;

		crypt_pwd = pg_fe_scram_build_secret(passwd,
											 conn->scram_sha_256_iterations,
											 &errstr);
		if (!crypt_pwd)
			libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
	}
	else if (strcmp(algorithm, "md5") == 0)
	{
		crypt_pwd = malloc(MD5_PASSWD_LEN + 1);
		if (crypt_pwd)
		{
			const char *errstr = NULL;

			if (!pg_md5_encrypt(passwd, user, strlen(user), crypt_pwd, &errstr))
			{
				libpq_append_conn_error(conn, "could not encrypt password: %s", errstr);
				free(crypt_pwd);
				crypt_pwd = NULL;
			}
		}
		else
			libpq_append_conn_error(conn, "out of memory");
	}
	else
	{
		libpq_append_conn_error(conn, "unrecognized password encryption algorithm \"%s\"",
								algorithm);
		return NULL;
	}

	return crypt_pwd;
}
