/*-------------------------------------------------------------------------
 *
 * fe-auth-scram.c
 *	   The front-end (client) implementation of SCRAM authentication.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-auth-scram.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/base64.h"
#include "common/hmac.h"
#include "common/saslprep.h"
#include "common/scram-common.h"
#include "fe-auth.h"


/* 导出的 SCRAM 回调机制。        */
static void *scram_init(PGconn *conn, const char *password,
						const char *sasl_mechanism);
static void scram_exchange(void *opaq, char *input, int inputlen,
						   char **output, int *outputlen,
						   bool *done, bool *success);
static bool scram_channel_bound(void *opaq);
static void scram_free(void *opaq);

const pg_fe_sasl_mech pg_scram_mech = {
	scram_init,
	scram_exchange,
	scram_channel_bound,
	scram_free
};

/*通过 SASL 协议进行 SCRAM 认证的交换消息状态。
 * 
 * 
                   */
typedef enum
{
	FE_SCRAM_INIT,
	FE_SCRAM_NONCE_SENT,
	FE_SCRAM_PROOF_SENT,
	FE_SCRAM_FINISHED
} fe_scram_state_enum;

typedef struct
{
	fe_scram_state_enum state;

	/* 这些由用户提供          */
	PGconn	   *conn;
	char	   *password;
	char	   *sasl_mechanism;

	/* 取决于哈希类型的状态数据  */
	pg_cryptohash_type hash_type;
	int			key_length;

	/* 我们构造这些 */
	uint8		SaltedPassword[SCRAM_MAX_KEY_LEN];
	char	   *client_nonce;
	char	   *client_first_message_bare;
	char	   *client_final_message_without_proof;

	/* 这些来自服务器的第一条消息  */
	char	   *server_first_message;
	char	   *salt;
	int			saltlen;
	int			iterations;
	char	   *nonce;

	/* 这些来自服务器的最终消息     */
	char	   *server_final_message;
	char		ServerSignature[SCRAM_MAX_KEY_LEN];
} fe_scram_state;

static bool read_server_first_message(fe_scram_state *state, char *input);
static bool read_server_final_message(fe_scram_state *state, char *input);
static char *build_client_first_message(fe_scram_state *state);
static char *build_client_final_message(fe_scram_state *state);
static bool verify_server_signature(fe_scram_state *state, bool *match,
									const char **errstr);
static bool calculate_client_proof(fe_scram_state *state,
								   const char *client_final_message_without_proof,
								   uint8 *result, const char **errstr);

/*初始化 SCRAM 交换状态。
 * 
   */
static void *
scram_init(PGconn *conn,
		   const char *password,
		   const char *sasl_mechanism)
{
	fe_scram_state *state;
	char	   *prep_password;
	pg_saslprep_rc rc;

	Assert(sasl_mechanism != NULL);

	state = (fe_scram_state *) malloc(sizeof(fe_scram_state));
	if (!state)
		return NULL;
	memset(state, 0, sizeof(fe_scram_state));
	state->conn = conn;
	state->state = FE_SCRAM_INIT;
	state->key_length = SCRAM_SHA_256_KEY_LEN;
	state->hash_type = PG_SHA256;

	state->sasl_mechanism = strdup(sasl_mechanism);
	if (!state->sasl_mechanism)
	{
		free(state);
		return NULL;
	}

	/* 如果可能，使用 SASLprep 规范化密码    */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_OOM)
	{
		free(state->sasl_mechanism);
		free(state);
		return NULL;
	}
	if (rc != SASLPREP_SUCCESS)
	{
		prep_password = strdup(password);
		if (!prep_password)
		{
			free(state->sasl_mechanism);
			free(state);
			return NULL;
		}
	}
	state->password = prep_password;

	return state;
}

/*如果使用了通道绑定且 SCRAM 交换完成，则返回 true。
 * 这应在交换成功后用于确定服务器是否已向客户端认证自己。
 * 
 * 注意调用者还必须确保交换确实成功了。
 *
 * 
 * 
                                                                   */
static bool
scram_channel_bound(void *opaq)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

	/* 未进行 SCRAM 交换 */
	if (state == NULL)
		return false;

	/* SCRAM 交换未完成        */
	if (state->state != FE_SCRAM_FINISHED)
		return false;

	/* 未使用通道绑定机制        */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) != 0)
		return false;

	/* 就绪!    */
	return true;
}

/*释放 SCRAM 交换状态
 * 
  */
static void
scram_free(void *opaq)
{
	fe_scram_state *state = (fe_scram_state *) opaq;

	free(state->password);
	free(state->sasl_mechanism);

	/* 客户端消息 */
	free(state->client_nonce);
	free(state->client_first_message_bare);
	free(state->client_final_message_without_proof);

	/* 服务器的首条消息  */
	free(state->server_first_message);
	free(state->salt);
	free(state->nonce);

	/* 服务器的最后消息  */
	free(state->server_final_message);

	free(state);
}

/*与后端交换 SCRAM 消息。
 * 
        */
static void
scram_exchange(void *opaq, char *input, int inputlen,
			   char **output, int *outputlen,
			   bool *done, bool *success)
{
	fe_scram_state *state = (fe_scram_state *) opaq;
	PGconn	   *conn = state->conn;
	const char *errstr = NULL;

	*done = false;
	*success = false;
	*output = NULL;
	*outputlen = 0;

	/*检查输入长度是否与输入的字符串长度一致。此后我们可以忽略 inputlen。
	 * 
	 * 
	          */
	if (state->state != FE_SCRAM_INIT)
	{
		if (inputlen == 0)
		{
			libpq_append_conn_error(conn, "malformed SCRAM message (empty message)");
			goto error;
		}
		if (inputlen != strlen(input))
		{
			libpq_append_conn_error(conn, "malformed SCRAM message (length mismatch)");
			goto error;
		}
	}

	switch (state->state)
	{
		case FE_SCRAM_INIT:
			/* 开始 SCRAM 握手，发送客户端 nonce        */
			*output = build_client_first_message(state);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_NONCE_SENT;
			break;

		case FE_SCRAM_NONCE_SENT:
			/* 接收盐和服务器 nonce，发送响应。 */
			if (!read_server_first_message(state, input))
				goto error;

			*output = build_client_final_message(state);
			if (*output == NULL)
				goto error;

			*outputlen = strlen(*output);
			*done = false;
			state->state = FE_SCRAM_PROOF_SENT;
			break;

		case FE_SCRAM_PROOF_SENT:
			/* 接收服务器签名    */
			if (!read_server_final_message(state, input))
				goto error;

			/*验证服务器签名，确保我们在与真正的服务器通信。
	
	
	                 */
			if (!verify_server_signature(state, success, &errstr))
			{
				libpq_append_conn_error(conn, "could not verify server signature: %s", errstr);
				goto error;
			}

			if (!*success)
			{
				libpq_append_conn_error(conn, "incorrect server signature");
			}
			*done = true;
			state->state = FE_SCRAM_FINISHED;
			state->conn->client_finished_auth = true;
			break;

		default:
			/* 不应该发生  */
			libpq_append_conn_error(conn, "invalid SCRAM exchange state");
			goto error;
	}
	return;

error:
	*done = true;
	*success = false;
}

/*读取 SCRAM 消息中属性部分的值。
 * 
 ***input 处的缓冲区会被破坏性修改，*input 会前进越过
 * "attr=value" 字符串及任何后续逗号。
 * 
 *失败时，向 *errorMessage 追加错误消息并返回 NULL。
 * 
                       */
static char *
read_attr_value(char **input, char attr, PQExpBuffer errorMessage)
{
	char	   *begin = *input;
	char	   *end;

	if (*begin != attr)
	{
		libpq_append_error(errorMessage,
						   "malformed SCRAM message (attribute \"%c\" expected)",
						   attr);
		return NULL;
	}
	begin++;

	if (*begin != '=')
	{
		libpq_append_error(errorMessage,
						   "malformed SCRAM message (expected character \"=\" for attribute \"%c\")",
						   attr);
		return NULL;
	}
	begin++;

	end = begin;
	while (*end && *end != ',')
		end++;

	if (*end)
	{
		*end = '\0';
		*input = end + 1;
	}
	else
		*input = end;

	return begin;
}

/*构建客户端发送的第一条交换消息。
 * 
     */
static char *
build_client_first_message(fe_scram_state *state)
{
	PGconn	   *conn = state->conn;
	char		raw_nonce[SCRAM_RAW_NONCE_LEN + 1];
	char	   *result;
	int			channel_info_len;
	int			encoded_len;
	PQExpBufferData buf;

	/*生成一个"原始"nonce。通过 base64 编码将其转换为 ASCII 可打印形式。
	 * 
	 * 
	 */
	if (!pg_strong_random(raw_nonce, SCRAM_RAW_NONCE_LEN))
	{
		libpq_append_conn_error(conn, "could not generate nonce");
		return NULL;
	}

	encoded_len = pg_b64_enc_len(SCRAM_RAW_NONCE_LEN);
	/* 不要忘记零终止符         */
	state->client_nonce = malloc(encoded_len + 1);
	if (state->client_nonce == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}
	encoded_len = pg_b64_encode(raw_nonce, SCRAM_RAW_NONCE_LEN,
								state->client_nonce, encoded_len);
	if (encoded_len < 0)
	{
		libpq_append_conn_error(conn, "could not encode nonce");
		return NULL;
	}
	state->client_nonce[encoded_len] = '\0';

	/*生成消息。用户名为空，因为后端使用启动数据包提供的值。
	 * 此外，由于此用户名未使用 SASLprep 准备，如果包含 '=' 或
	 * ',' 字符，消息解析将失败。
	 * 
	 * 
	                                    */

	initPQExpBuffer(&buf);

	/*首先构建包含通道绑定信息的 gs2-header。
	 * 
	        */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) == 0)
	{
		Assert(conn->ssl_in_use);
		appendPQExpBufferStr(&buf, "p=tls-server-end-point");
	}
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
	else if (conn->channel_binding[0] != 'd' && /* 禁用  */
			 conn->ssl_in_use)
	{
		/*客户端支持通道绑定，但认为服务器不支持。
	
	          */
		appendPQExpBufferChar(&buf, 'y');
	}
#endif
	else
	{
		/*客户端不支持通道绑定，或已禁用它。
	
	               */
		appendPQExpBufferChar(&buf, 'n');
	}

	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	channel_info_len = buf.len;

	appendPQExpBuffer(&buf, ",,n=,r=%s", state->client_nonce);
	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	/*第一条消息内容需要在不包含通道绑定信息的情况下保存。
	 * 
	 * 
	  */
	state->client_first_message_bare = strdup(buf.data + channel_info_len + 2);
	if (!state->client_first_message_bare)
		goto oom_error;

	result = strdup(buf.data);
	if (result == NULL)
		goto oom_error;

	termPQExpBuffer(&buf);
	return result;

oom_error:
	termPQExpBuffer(&buf);
	libpq_append_conn_error(conn, "out of memory");
	return NULL;
}

/*构建客户端发送的最后一条交换消息。
 * 
    */
static char *
build_client_final_message(fe_scram_state *state)
{
	PQExpBufferData buf;
	PGconn	   *conn = state->conn;
	uint8		client_proof[SCRAM_MAX_KEY_LEN];
	char	   *result;
	int			encoded_len;
	const char *errstr = NULL;

	initPQExpBuffer(&buf);

	/*构造 client-final-message-without-proof。我们需要记住它，
	 * 以便在认证的最后一步验证服务器证明。
	 * 
	 *通道绑定标志处理（p/y/n）必须与
	 * build_client_first_message() 一致，因为服务器会检查两次
	 * 的标志是否相同。
	 * 
	                                      */
	if (strcmp(state->sasl_mechanism, SCRAM_SHA_256_PLUS_NAME) == 0)
	{
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
		char	   *cbind_data = NULL;
		size_t		cbind_data_len = 0;
		size_t		cbind_header_len;
		char	   *cbind_input;
		size_t		cbind_input_len;
		int			encoded_cbind_len;

		/* 获取服务器 SSL 证书的哈希数据   */
		cbind_data =
			pgtls_get_peer_certificate_hash(state->conn,
											&cbind_data_len);
		if (cbind_data == NULL)
		{
			/* 错误消息已在出错时设置     */
			termPQExpBuffer(&buf);
			return NULL;
		}

		appendPQExpBufferStr(&buf, "c=");

		/* p=type,, */
		cbind_header_len = strlen("p=tls-server-end-point,,");
		cbind_input_len = cbind_header_len + cbind_data_len;
		cbind_input = malloc(cbind_input_len);
		if (!cbind_input)
		{
			free(cbind_data);
			goto oom_error;
		}
		memcpy(cbind_input, "p=tls-server-end-point,,", cbind_header_len);
		memcpy(cbind_input + cbind_header_len, cbind_data, cbind_data_len);

		encoded_cbind_len = pg_b64_enc_len(cbind_input_len);
		if (!enlargePQExpBuffer(&buf, encoded_cbind_len))
		{
			free(cbind_data);
			free(cbind_input);
			goto oom_error;
		}
		encoded_cbind_len = pg_b64_encode(cbind_input, cbind_input_len,
										  buf.data + buf.len,
										  encoded_cbind_len);
		if (encoded_cbind_len < 0)
		{
			free(cbind_data);
			free(cbind_input);
			termPQExpBuffer(&buf);
			appendPQExpBufferStr(&conn->errorMessage,
								 "could not encode cbind data for channel binding\n");
			return NULL;
		}
		buf.len += encoded_cbind_len;
		buf.data[buf.len] = '\0';

		free(cbind_data);
		free(cbind_input);
#else
		/*选择了通道绑定，但 SSL 库不支持。不应该发生。
	
	
	                        */
		termPQExpBuffer(&buf);
		appendPQExpBufferStr(&conn->errorMessage,
							 "channel binding not supported by this build\n");
		return NULL;
#endif							/* HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH */
	}
#ifdef HAVE_PGTLS_GET_PEER_CERTIFICATE_HASH
	else if (conn->channel_binding[0] != 'd' && /* 禁用  */
			 conn->ssl_in_use)
		appendPQExpBufferStr(&buf, "c=eSws");	/* "y,," 的base64 */
#endif
	else
		appendPQExpBufferStr(&buf, "c=biws");	/* "n,," 的base64 */

	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	appendPQExpBuffer(&buf, ",r=%s", state->nonce);
	if (PQExpBufferDataBroken(buf))
		goto oom_error;

	state->client_final_message_without_proof = strdup(buf.data);
	if (state->client_final_message_without_proof == NULL)
		goto oom_error;

	/* 追加证明，构成 client-final-message。     */
	if (!calculate_client_proof(state,
								state->client_final_message_without_proof,
								client_proof, &errstr))
	{
		termPQExpBuffer(&buf);
		libpq_append_conn_error(conn, "could not calculate client proof: %s", errstr);
		return NULL;
	}

	appendPQExpBufferStr(&buf, ",p=");
	encoded_len = pg_b64_enc_len(state->key_length);
	if (!enlargePQExpBuffer(&buf, encoded_len))
		goto oom_error;
	encoded_len = pg_b64_encode((char *) client_proof,
								state->key_length,
								buf.data + buf.len,
								encoded_len);
	if (encoded_len < 0)
	{
		termPQExpBuffer(&buf);
		libpq_append_conn_error(conn, "could not encode client proof");
		return NULL;
	}
	buf.len += encoded_len;
	buf.data[buf.len] = '\0';

	result = strdup(buf.data);
	if (result == NULL)
		goto oom_error;

	termPQExpBuffer(&buf);
	return result;

oom_error:
	termPQExpBuffer(&buf);
	libpq_append_conn_error(conn, "out of memory");
	return NULL;
}

/*读取来自服务器的第一条交换消息。
 * 
        */
static bool
read_server_first_message(fe_scram_state *state, char *input)
{
	PGconn	   *conn = state->conn;
	char	   *iterations_str;
	char	   *endptr;
	char	   *encoded_salt;
	char	   *nonce;
	int			decoded_salt_len;

	state->server_first_message = strdup(input);
	if (state->server_first_message == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return false;
	}

	/* 解析消息      */
	nonce = read_attr_value(&input, 'r',
							&conn->errorMessage);
	if (nonce == NULL)
	{
		/* read_attr_value() 已追加错误字符串     */
		return false;
	}

	/* 立即验证服务器使用了我们那部分的 nonce        */
	if (strlen(nonce) < strlen(state->client_nonce) ||
		memcmp(nonce, state->client_nonce, strlen(state->client_nonce)) != 0)
	{
		libpq_append_conn_error(conn, "invalid SCRAM response (nonce mismatch)");
		return false;
	}

	state->nonce = strdup(nonce);
	if (state->nonce == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return false;
	}

	encoded_salt = read_attr_value(&input, 's', &conn->errorMessage);
	if (encoded_salt == NULL)
	{
		/* read_attr_value() 已追加错误字符串     */
		return false;
	}
	decoded_salt_len = pg_b64_dec_len(strlen(encoded_salt));
	state->salt = malloc(decoded_salt_len);
	if (state->salt == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return false;
	}
	state->saltlen = pg_b64_decode(encoded_salt,
								   strlen(encoded_salt),
								   state->salt,
								   decoded_salt_len);
	if (state->saltlen < 0)
	{
		libpq_append_conn_error(conn, "malformed SCRAM message (invalid salt)");
		return false;
	}

	iterations_str = read_attr_value(&input, 'i', &conn->errorMessage);
	if (iterations_str == NULL)
	{
		/* read_attr_value() 已追加错误字符串     */
		return false;
	}
	state->iterations = strtol(iterations_str, &endptr, 10);
	if (*endptr != '\0' || state->iterations < 1)
	{
		libpq_append_conn_error(conn, "malformed SCRAM message (invalid iteration count)");
		return false;
	}

	if (*input != '\0')
		libpq_append_conn_error(conn, "malformed SCRAM message (garbage at end of server-first-message)");

	return true;
}

/*读取来自服务器的最后一条交换消息。
 * 
     */
static bool
read_server_final_message(fe_scram_state *state, char *input)
{
	PGconn	   *conn = state->conn;
	char	   *encoded_server_signature;
	char	   *decoded_server_signature;
	int			server_signature_len;

	state->server_final_message = strdup(input);
	if (!state->server_final_message)
	{
		libpq_append_conn_error(conn, "out of memory");
		return false;
	}

	/* 检查错误结果。   */
	if (*input == 'e')
	{
		char	   *errmsg = read_attr_value(&input, 'e',
											 &conn->errorMessage);

		if (errmsg == NULL)
		{
			/* read_attr_value() 已追加错误消息         */
			return false;
		}
		libpq_append_conn_error(conn, "error received from server in SCRAM exchange: %s",
								errmsg);
		return false;
	}

	/* 解析消息。    */
	encoded_server_signature = read_attr_value(&input, 'v',
											   &conn->errorMessage);
	if (encoded_server_signature == NULL)
	{
		/* read_attr_value() 已追加错误消息         */
		return false;
	}

	if (*input != '\0')
		libpq_append_conn_error(conn, "malformed SCRAM message (garbage at end of server-final-message)");

	server_signature_len = pg_b64_dec_len(strlen(encoded_server_signature));
	decoded_server_signature = malloc(server_signature_len);
	if (!decoded_server_signature)
	{
		libpq_append_conn_error(conn, "out of memory");
		return false;
	}

	server_signature_len = pg_b64_decode(encoded_server_signature,
										 strlen(encoded_server_signature),
										 decoded_server_signature,
										 server_signature_len);
	if (server_signature_len != state->key_length)
	{
		free(decoded_server_signature);
		libpq_append_conn_error(conn, "malformed SCRAM message (invalid server signature)");
		return false;
	}
	memcpy(state->ServerSignature, decoded_server_signature,
		   state->key_length);
	free(decoded_server_signature);

	return true;
}

/*计算客户端证明，这是客户端发送的最后一条交换消息的一部分。
 * 成功时返回 true，失败时返回 false，*errstr 指向有关错误
 * 详情的消息。
 * 
      */
static bool
calculate_client_proof(fe_scram_state *state,
					   const char *client_final_message_without_proof,
					   uint8 *result, const char **errstr)
{
	uint8		StoredKey[SCRAM_MAX_KEY_LEN];
	uint8		ClientKey[SCRAM_MAX_KEY_LEN];
	uint8		ClientSignature[SCRAM_MAX_KEY_LEN];
	int			i;
	pg_hmac_ctx *ctx;

	ctx = pg_hmac_create(state->hash_type);
	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* 返回 OOM  */
		return false;
	}

	/*
	 * 计算 SaltedPassword，并将其存入 'state'，以便稍后供     
	 * verify_server_signature 使用。   
	 */
	if (scram_SaltedPassword(state->password, state->hash_type,
							 state->key_length, state->salt, state->saltlen,
							 state->iterations, state->SaltedPassword,
							 errstr) < 0 ||
		scram_ClientKey(state->SaltedPassword, state->hash_type,
						state->key_length, ClientKey, errstr) < 0 ||
		scram_H(ClientKey, state->hash_type, state->key_length,
				StoredKey, errstr) < 0)
	{
		/* errstr 已在此处填充     */
		pg_hmac_free(ctx);
		return false;
	}

	if (pg_hmac_init(ctx, StoredKey, state->key_length) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_first_message_bare,
					   strlen(state->client_first_message_bare)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->server_first_message,
					   strlen(state->server_first_message)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) client_final_message_without_proof,
					   strlen(client_final_message_without_proof)) < 0 ||
		pg_hmac_final(ctx, ClientSignature, state->key_length) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return false;
	}

	for (i = 0; i < state->key_length; i++)
		result[i] = ClientKey[i] ^ ClientSignature[i];

	pg_hmac_free(ctx);
	return true;
}

/*验证服务器签名，作为从服务器接收的最后一条交换消息的一部分。
 * *match 跟踪服务器签名是否匹配。如果服务器签名得到验证则返回
 * true，处理错误时返回 false，*errstr 指向有关错误详情的消息。
 * 
 * 
 * 
                                           */
static bool
verify_server_signature(fe_scram_state *state, bool *match,
						const char **errstr)
{
	uint8		expected_ServerSignature[SCRAM_MAX_KEY_LEN];
	uint8		ServerKey[SCRAM_MAX_KEY_LEN];
	pg_hmac_ctx *ctx;

	ctx = pg_hmac_create(state->hash_type);
	if (ctx == NULL)
	{
		*errstr = pg_hmac_error(NULL);	/* 返回 OOM  */
		return false;
	}

	if (scram_ServerKey(state->SaltedPassword, state->hash_type,
						state->key_length, ServerKey, errstr) < 0)
	{
		/* errstr 已填充         */
		pg_hmac_free(ctx);
		return false;
	}

	/* 计算 ServerSignature    */
	if (pg_hmac_init(ctx, ServerKey, state->key_length) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_first_message_bare,
					   strlen(state->client_first_message_bare)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->server_first_message,
					   strlen(state->server_first_message)) < 0 ||
		pg_hmac_update(ctx, (uint8 *) ",", 1) < 0 ||
		pg_hmac_update(ctx,
					   (uint8 *) state->client_final_message_without_proof,
					   strlen(state->client_final_message_without_proof)) < 0 ||
		pg_hmac_final(ctx, expected_ServerSignature,
					  state->key_length) < 0)
	{
		*errstr = pg_hmac_error(ctx);
		pg_hmac_free(ctx);
		return false;
	}

	pg_hmac_free(ctx);

	/* 签名已处理，现在检查它          */
	if (memcmp(expected_ServerSignature, state->ServerSignature,
			   state->key_length) != 0)
		*match = false;
	else
		*match = true;

	return true;
}

/*构建新的 SCRAM 密钥。
 * 
 *出错时返回 NULL，并设置 *errstr 指向有关错误详情的消息。
 * 
 * 
      */
char *
pg_fe_scram_build_secret(const char *password, int iterations, const char **errstr)
{
	char	   *prep_password;
	pg_saslprep_rc rc;
	char		saltbuf[SCRAM_DEFAULT_SALT_LEN];
	char	   *result;

	/*使用 SASLprep 规范化密码。如果不行（因为密码不是有效的
	 * UTF-8 或包含禁止字符），则直接使用原始密码继续。
	 * （参见 auth-scram.c 顶部的注释。）
	 * 
	 * 
	                            */
	rc = pg_saslprep(password, &prep_password);
	if (rc == SASLPREP_OOM)
	{
		*errstr = libpq_gettext("out of memory");
		return NULL;
	}
	if (rc == SASLPREP_SUCCESS)
		password = (const char *) prep_password;

	/* 生成随机 salt      */
	if (!pg_strong_random(saltbuf, SCRAM_DEFAULT_SALT_LEN))
	{
		*errstr = libpq_gettext("could not generate random salt");
		free(prep_password);
		return NULL;
	}

	result = scram_build_secret(PG_SHA256, SCRAM_SHA_256_KEY_LEN, saltbuf,
								SCRAM_DEFAULT_SALT_LEN,
								iterations, password,
								errstr);

	free(prep_password);

	return result;
}
