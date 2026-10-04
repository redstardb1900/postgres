/*-------------------------------------------------------------------------
 *
 * be-secure-openssl.c
 *	  functions for OpenSSL support in the backend.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure-openssl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "common/string.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/*
 * 这些与 SSL 相关的 #include 必须放在所有系统提供的头文件之后。
 * 这样 OpenSSL 才能处理与 Windows 的
 * <wincrypt.h> 的冲突，办法是 #undef 掉冲突的宏。（我们并不直接
 * 包含 <wincrypt.h>，但其他一些 Windows 头文件会包含。）
 */
#include "common/openssl.h"
#include <openssl/conf.h>
#include <openssl/dh.h>
#ifndef OPENSSL_NO_ECDH
#include <openssl/ec.h>
#endif
#include <openssl/x509v3.h>


/* 默认的初始化钩子可以被共享库覆盖 */
static void default_openssl_tls_init(SSL_CTX *context, bool isServerStart);
openssl_tls_init_hook_typ openssl_tls_init_hook = default_openssl_tls_init;

static int	my_sock_read(BIO *h, char *buf, int size);
static int	my_sock_write(BIO *h, const char *buf, int size);
static BIO_METHOD *my_BIO_s_socket(void);
static int	my_SSL_set_fd(Port *port, int fd);

static DH  *load_dh_file(char *filename, bool isServerStart);
static DH  *load_dh_buffer(const char *buffer, size_t len);
static int	ssl_external_passwd_cb(char *buf, int size, int rwflag, void *userdata);
static int	dummy_ssl_passwd_cb(char *buf, int size, int rwflag, void *userdata);
static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static void info_cb(const SSL *ssl, int type, int args);
static bool initialize_dh(SSL_CTX *context, bool isServerStart);
static bool initialize_ecdh(SSL_CTX *context, bool isServerStart);
static const char *SSLerrmessage(unsigned long ecode);

static char *X509_NAME_to_cstring(X509_NAME *name);

static SSL_CTX *SSL_context = NULL;
static bool SSL_initialized = false;
static bool dummy_ssl_passwd_cb_called = false;
static bool ssl_is_server_start;

static int	ssl_protocol_version_to_openssl(int v);
static const char *ssl_protocol_version_to_string(int v);

/* 用于从 verify_cb() 传回数据 */
static const char *cert_errdetail;

/* ------------------------------------------------------------ */
/*						 公共接口						*/
/* ------------------------------------------------------------ */

int
be_tls_init(bool isServerStart)
{
	SSL_CTX    *context;
	int			ssl_ver_min = -1;
	int			ssl_ver_max = -1;

	/* 这些事情只需要做一次。 */
	if (!SSL_initialized)
	{
#ifdef HAVE_OPENSSL_INIT_SSL
		OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
		OPENSSL_config(NULL);
		SSL_library_init();
		SSL_load_error_strings();
#endif
		SSL_initialized = true;
	}

	/*
	 * 创建一个新的 SSL 上下文，我们将把全部配置
	 * 设置装进去。若中途失败，可以通过释放该上下文
	 * 避免内存泄漏；直到最后我们才把它安装为活动上下文。
	 *
	 * 我们使用 SSLv23_method()，因为它能协商双方
	 * 共同支持的最高协议版本，而像
	 * TLSv1_2_method() 这样的替代方案只允许一个特定版本。注意我们并不
	 * 真正允许 SSL v2 或 v3，只允许 TLS 协议（见下文）。
	 */
	context = SSL_CTX_new(SSLv23_method());
	if (!context)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errmsg("could not create SSL context: %s",
						SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	/*
	 * 禁用 OpenSSL 的移动写缓冲区健全性检查，因为它会在
	 * 非阻塞发送情况下造成不必要的失败。
	 */
	SSL_CTX_set_mode(context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	/*
	 * 调用初始化钩子（通常用来设置口令回调）
	 */
	(*openssl_tls_init_hook) (context, isServerStart);

	/* 供回调使用 */
	ssl_is_server_start = isServerStart;

	/*
	 * 加载并验证服务器的证书和私钥
	 */
	if (SSL_CTX_use_certificate_chain_file(context, ssl_cert_file) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not load server certificate file \"%s\": %s",
						ssl_cert_file, SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (!check_ssl_key_file_permissions(ssl_key_file, isServerStart))
		goto error;

	/*
	 * 好，尝试加载私钥文件。
	 */
	dummy_ssl_passwd_cb_called = false;

	if (SSL_CTX_use_PrivateKey_file(context,
									ssl_key_file,
									SSL_FILETYPE_PEM) != 1)
	{
		if (dummy_ssl_passwd_cb_called)
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("private key file \"%s\" cannot be reloaded because it requires a passphrase",
							ssl_key_file)));
		else
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load private key file \"%s\": %s",
							ssl_key_file, SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (SSL_CTX_check_private_key(context) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("check of private key failed: %s",
						SSLerrmessage(ERR_get_error()))));
		goto error;
	}

	if (ssl_min_protocol_version)
	{
		ssl_ver_min = ssl_protocol_version_to_openssl(ssl_min_protocol_version);

		if (ssl_ver_min == -1)
		{
			ereport(isServerStart ? FATAL : LOG,
			/*- translator: first %s is a GUC option name, second %s is its value */
					(errmsg("\"%s\" setting \"%s\" not supported by this build",
							"ssl_min_protocol_version",
							GetConfigOption("ssl_min_protocol_version",
											false, false))));
			goto error;
		}

		if (!SSL_CTX_set_min_proto_version(context, ssl_ver_min))
		{
			ereport(isServerStart ? FATAL : LOG,
					(errmsg("could not set minimum SSL protocol version")));
			goto error;
		}
	}

	if (ssl_max_protocol_version)
	{
		ssl_ver_max = ssl_protocol_version_to_openssl(ssl_max_protocol_version);

		if (ssl_ver_max == -1)
		{
			ereport(isServerStart ? FATAL : LOG,
			/*- translator: first %s is a GUC option name, second %s is its value */
					(errmsg("\"%s\" setting \"%s\" not supported by this build",
							"ssl_max_protocol_version",
							GetConfigOption("ssl_max_protocol_version",
											false, false))));
			goto error;
		}

		if (!SSL_CTX_set_max_proto_version(context, ssl_ver_max))
		{
			ereport(isServerStart ? FATAL : LOG,
					(errmsg("could not set maximum SSL protocol version")));
			goto error;
		}
	}

	/* 检查最小/最大协议的兼容性 */
	if (ssl_min_protocol_version &&
		ssl_max_protocol_version)
	{
		/*
		 * 不必检查每个协议号的无效值（-1），
		 * 因为上面的代码应该已经产生了错误。
		 */
		if (ssl_ver_min > ssl_ver_max)
		{
			ereport(isServerStart ? FATAL : LOG,
					(errmsg("could not set SSL protocol version range"),
					 errdetail("\"%s\" cannot be higher than \"%s\"",
							   "ssl_min_protocol_version",
							   "ssl_max_protocol_version")));
			goto error;
		}
	}

	/*
	 * 禁止 SSL 会话票证。OpenSSL 对 TLSv1.3 同时使用有状态和无状态
	 * 票证，对 TLSv1.2 使用无状态票证。SSL_OP_NO_TICKET
	 * 自 0.9.8f 起可用，但只关掉无状态票证。为了
	 * 关掉有状态票证，我们需要 SSL_CTX_set_num_tickets，
	 * 它自 OpenSSL 1.1.1 起可用。LibreSSL 3.5.4（来自 OpenBSD
	 * 7.1）为了兼容引入了这个 API，但根本不支持会话
	 * 票证，所以在那里它是空操作。
	 */
#ifdef HAVE_SSL_CTX_SET_NUM_TICKETS
	SSL_CTX_set_num_tickets(context, 0);
#endif
	SSL_CTX_set_options(context, SSL_OP_NO_TICKET);

	/* 也禁止 SSL 会话缓存 */
	SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);

	/* 禁止 SSL 压缩 */
	SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);

#ifdef SSL_OP_NO_RENEGOTIATION

	/*
	 * 禁止 SSL 重协商，该选项自 1.1.0h 起可用。这
	 * 只涉及 TLSv1.2 及更老的协议版本，因为 TLSv1.3 没有
	 * 对重协商的支持。
	 */
	SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION);
#endif

	/* 设置临时 DH 和 ECDH 密钥 */
	if (!initialize_dh(context, isServerStart))
		goto error;
	if (!initialize_ecdh(context, isServerStart))
		goto error;

	/* 设置允许的密码套件列表 */
	if (SSL_CTX_set_cipher_list(context, SSLCipherSuites) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not set the cipher list (no valid ciphers available)")));
		goto error;
	}

	/* 让服务器选择顺序 */
	if (SSLPreferServerCiphers)
		SSL_CTX_set_options(context, SSL_OP_CIPHER_SERVER_PREFERENCE);

	/*
	 * 加载 CA 存储，以便在需要时验证客户端证书。
	 */
	if (ssl_ca_file[0])
	{
		STACK_OF(X509_NAME) * root_cert_list;

		if (SSL_CTX_load_verify_locations(context, ssl_ca_file, NULL) != 1 ||
			(root_cert_list = SSL_load_client_CA_file(ssl_ca_file)) == NULL)
		{
			ereport(isServerStart ? FATAL : LOG,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not load root certificate file \"%s\": %s",
							ssl_ca_file, SSLerrmessage(ERR_get_error()))));
			goto error;
		}

		/*
		 * 让 OpenSSL 把我们信任的根证书列表放在
		 * CertificateRequests 里发给客户端。这样持有密钥库的客户端就能选择
		 * 要发给我们的合适客户端证书。同时，这也确保
		 * SSL 上下文会 "拥有" root_cert_list，并记得在
		 * 不再需要时释放它。
		 */
		SSL_CTX_set_client_CA_list(context, root_cert_list);

		/*
		 * 总是索取 SSL 客户端证书，但若未提供也不失败。
		 * 以后我们可能会让这类连接失败，取决于在
		 * pg_hba.conf 里发现了什么。
		 */
		SSL_CTX_set_verify(context,
						   (SSL_VERIFY_PEER |
							SSL_VERIFY_CLIENT_ONCE),
						   verify_cb);
	}

	/*----------
	 * Load the Certificate Revocation List (CRL).
	 * http://searchsecurity.techtarget.com/sDefinition/0,,sid14_gci803160,00.html
	 *----------
	 */
	if (ssl_crl_file[0] || ssl_crl_dir[0])
	{
		X509_STORE *cvstore = SSL_CTX_get_cert_store(context);

		if (cvstore)
		{
			/* 设置标志，对照完整的 CRL 链进行检查 */
			if (X509_STORE_load_locations(cvstore,
										  ssl_crl_file[0] ? ssl_crl_file : NULL,
										  ssl_crl_dir[0] ? ssl_crl_dir : NULL)
				== 1)
			{
				X509_STORE_set_flags(cvstore,
									 X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
			}
			else if (ssl_crl_dir[0] == 0)
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list file \"%s\": %s",
								ssl_crl_file, SSLerrmessage(ERR_get_error()))));
				goto error;
			}
			else if (ssl_crl_file[0] == 0)
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list directory \"%s\": %s",
								ssl_crl_dir, SSLerrmessage(ERR_get_error()))));
				goto error;
			}
			else
			{
				ereport(isServerStart ? FATAL : LOG,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load SSL certificate revocation list file \"%s\" or directory \"%s\": %s",
								ssl_crl_file, ssl_crl_dir,
								SSLerrmessage(ERR_get_error()))));
				goto error;
			}
		}
	}

	/*
	 * 成功！替换任何已有的 SSL_context。
	 */
	if (SSL_context)
		SSL_CTX_free(SSL_context);

	SSL_context = context;

	/*
	 * 设置标志，记住 CA 存储是否已加载进 SSL_context。
	 */
	if (ssl_ca_file[0])
		ssl_loaded_verify_locations = true;
	else
		ssl_loaded_verify_locations = false;

	return 0;

	/* 通过释放工作上下文来清理。 */
error:
	if (context)
		SSL_CTX_free(context);
	return -1;
}

void
be_tls_destroy(void)
{
	if (SSL_context)
		SSL_CTX_free(SSL_context);
	SSL_context = NULL;
	ssl_loaded_verify_locations = false;
}

int
be_tls_open_server(Port *port)
{
	int			r;
	int			err;
	int			waitfor;
	unsigned long ecode;
	bool		give_proto_hint;

	Assert(!port->ssl);
	Assert(!port->peer);

	if (!SSL_context)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not initialize SSL connection: SSL context not set up")));
		return -1;
	}

	/* 设置调试/信息回调 */
	SSL_CTX_set_info_callback(SSL_context, info_cb);

	if (!(port->ssl = SSL_new(SSL_context)))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not initialize SSL connection: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}
	if (!my_SSL_set_fd(port, port->sock))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not set SSL socket: %s",
						SSLerrmessage(ERR_get_error()))));
		return -1;
	}
	port->ssl_in_use = true;

aloop:

	/*
	 * 准备调用 SSL_get_error()，先清空线程的 OpenSSL 错误
	 * 队列。一般而言，尝试 TLS/SSL I/O 操作之前，当前线程的
	 * 错误队列必须为空，否则 SSL_get_error() 就
	 * 不可靠。某个扩展可能在另一次 OpenSSL I/O
	 * 例程调用之后，没有清空每线程
	 * 错误队列。
	 */
	errno = 0;
	ERR_clear_error();
	r = SSL_accept(port->ssl);
	if (r <= 0)
	{
		err = SSL_get_error(port->ssl, r);

		/*
		 * 后端里 OpenSSL 的其他客户端可能没有调用
		 * ERR_get_error()，但我们总会调用，以免给那些
		 * 不防御性地调用 ERR_clear_error() 的 OpenSSL 客户端
		 * 带来问题。现在就调用，确保这一点发生。SSL_get_error() 依赖于
		 * OpenSSL 的每线程错误队列保持完好，因此这是
		 * 可以调用 ERR_get_error() 的最早时机。
		 */
		ecode = ERR_get_error();
		switch (err)
		{
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				/* 连接建立期间不允许 */
				Assert(!port->noblock);

				/*
				 * 这里不必关心超时/中断。此时
				 * authentication_timeout 仍使用
				 * StartupPacketTimeoutHandler()，它会直接退出。
				 */
				if (err == SSL_ERROR_WANT_READ)
					waitfor = WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH;
				else
					waitfor = WL_SOCKET_WRITEABLE | WL_EXIT_ON_PM_DEATH;

				(void) WaitLatchOrSocket(MyLatch, waitfor, port->sock, 0,
										 WAIT_EVENT_SSL_OPEN_SERVER);
				goto aloop;
			case SSL_ERROR_SYSCALL:
				if (r < 0 && errno != 0)
					ereport(COMMERROR,
							(errcode_for_socket_access(),
							 errmsg("could not accept SSL connection: %m")));
				else
					ereport(COMMERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg("could not accept SSL connection: EOF detected")));
				break;
			case SSL_ERROR_SSL:
				switch (ERR_GET_REASON(ecode))
				{
						/*
						 * 已观察到 UNSUPPORTED_PROTOCOL、WRONG_VERSION_NUMBER 以及
						 * TLSV1_ALERT_PROTOCOL_VERSION，出现于
						 * 试图与旧的 OpenSSL
						 * 库通信时，或客户端与服务器指定了
						 * 互不相交的协议范围时。NO_PROTOCOLS_AVAILABLE
						 * 出现于本地配置错误（尽管我们做了检查，
						 * 若 openssl.cnf
						 * 注入了我们没有计入的限制，仍可能发生）。并不
						 * 很清楚什么会让 OpenSSL 返回这里列出的其他
						 * 代码，但关于协议
						 * 版本的提示对所有这些情况似乎都合适。
						 */
					case SSL_R_NO_PROTOCOLS_AVAILABLE:
					case SSL_R_UNSUPPORTED_PROTOCOL:
					case SSL_R_BAD_PROTOCOL_VERSION_NUMBER:
					case SSL_R_UNKNOWN_PROTOCOL:
					case SSL_R_UNKNOWN_SSL_VERSION:
					case SSL_R_UNSUPPORTED_SSL_VERSION:
					case SSL_R_WRONG_SSL_VERSION:
					case SSL_R_WRONG_VERSION_NUMBER:
					case SSL_R_TLSV1_ALERT_PROTOCOL_VERSION:
#ifdef SSL_R_VERSION_TOO_HIGH
					case SSL_R_VERSION_TOO_HIGH:
					case SSL_R_VERSION_TOO_LOW:
#endif
						give_proto_hint = true;
						break;
					default:
						give_proto_hint = false;
						break;
				}
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("could not accept SSL connection: %s",
								SSLerrmessage(ecode)),
						 cert_errdetail ? errdetail_internal("%s", cert_errdetail) : 0,
						 give_proto_hint ?
						 errhint("This may indicate that the client does not support any SSL protocol version between %s and %s.",
								 ssl_min_protocol_version ?
								 ssl_protocol_version_to_string(ssl_min_protocol_version) :
								 MIN_OPENSSL_TLS_VERSION,
								 ssl_max_protocol_version ?
								 ssl_protocol_version_to_string(ssl_max_protocol_version) :
								 MAX_OPENSSL_TLS_VERSION) : 0));
				cert_errdetail = NULL;
				break;
			case SSL_ERROR_ZERO_RETURN:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("could not accept SSL connection: EOF detected")));
				break;
			default:
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unrecognized SSL error code: %d",
								err)));
				break;
		}
		return -1;
	}

	/* 取得客户端证书，若有的话。 */
	port->peer = SSL_get_peer_certificate(port->ssl);

	/* 并从中提取公用名和识别名。 */
	port->peer_cn = NULL;
	port->peer_dn = NULL;
	port->peer_cert_valid = false;
	if (port->peer != NULL)
	{
		int			len;
		X509_NAME  *x509name = X509_get_subject_name(port->peer);
		char	   *peer_dn;
		BIO		   *bio = NULL;
		BUF_MEM    *bio_buf = NULL;

		len = X509_NAME_get_text_by_NID(x509name, NID_commonName, NULL, 0);
		if (len != -1)
		{
			char	   *peer_cn;

			peer_cn = MemoryContextAlloc(TopMemoryContext, len + 1);
			r = X509_NAME_get_text_by_NID(x509name, NID_commonName, peer_cn,
										  len + 1);
			peer_cn[len] = '\0';
			if (r != len)
			{
				/* 不该发生 */
				pfree(peer_cn);
				return -1;
			}

			/*
			 * 拒绝证书公用名中嵌入的 NULLs，以防止
			 * 类似 CVE-2009-4034 的攻击。
			 */
			if (len != strlen(peer_cn))
			{
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("SSL certificate's common name contains embedded null")));
				pfree(peer_cn);
				return -1;
			}

			port->peer_cn = peer_cn;
		}

		bio = BIO_new(BIO_s_mem());
		if (!bio)
		{
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}

		/*
		 * RFC2253 是最接近公认标准的 DN
		 * 格式。我们已经说明了如何从
		 * 证书产生这种格式。它用逗号而不是斜线作分隔符，
		 * 这让正则表达式匹配容易一些。还要注意
		 * 它按相反顺序打印 Subject 字段。
		 */
		if (X509_NAME_print_ex(bio, x509name, 0, XN_FLAG_RFC2253) == -1 ||
			BIO_get_mem_ptr(bio, &bio_buf) <= 0)
		{
			BIO_free(bio);
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}
		peer_dn = MemoryContextAlloc(TopMemoryContext, bio_buf->length + 1);
		memcpy(peer_dn, bio_buf->data, bio_buf->length);
		len = bio_buf->length;
		BIO_free(bio);
		peer_dn[len] = '\0';
		if (len != strlen(peer_dn))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL certificate's distinguished name contains embedded null")));
			pfree(peer_dn);
			if (port->peer_cn != NULL)
			{
				pfree(port->peer_cn);
				port->peer_cn = NULL;
			}
			return -1;
		}

		port->peer_dn = peer_dn;

		port->peer_cert_valid = true;
	}

	return 0;
}

void
be_tls_close(Port *port)
{
	if (port->ssl)
	{
		SSL_shutdown(port->ssl);
		SSL_free(port->ssl);
		port->ssl = NULL;
		port->ssl_in_use = false;
	}

	if (port->peer)
	{
		X509_free(port->peer);
		port->peer = NULL;
	}

	if (port->peer_cn)
	{
		pfree(port->peer_cn);
		port->peer_cn = NULL;
	}

	if (port->peer_dn)
	{
		pfree(port->peer_dn);
		port->peer_dn = NULL;
	}
}

ssize_t
be_tls_read(Port *port, void *ptr, size_t len, int *waitfor)
{
	ssize_t		n;
	int			err;
	unsigned long ecode;

	errno = 0;
	ERR_clear_error();
	n = SSL_read(port->ssl, ptr, len);
	err = SSL_get_error(port->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			/* 正常 */
			break;
		case SSL_ERROR_WANT_READ:
			*waitfor = WL_SOCKET_READABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_WANT_WRITE:
			*waitfor = WL_SOCKET_WRITEABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_SYSCALL:
			/* 把 errno 的值留给调用方去 ereport */
			if (n != -1 || errno == 0)
			{
				errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL error: %s", SSLerrmessage(ecode))));
			errno = ECONNRESET;
			n = -1;
			break;
		case SSL_ERROR_ZERO_RETURN:
			/* 连接已被对端干净地关闭 */
			n = 0;
			break;
		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unrecognized SSL error code: %d",
							err)));
			errno = ECONNRESET;
			n = -1;
			break;
	}

	return n;
}

ssize_t
be_tls_write(Port *port, void *ptr, size_t len, int *waitfor)
{
	ssize_t		n;
	int			err;
	unsigned long ecode;

	errno = 0;
	ERR_clear_error();
	n = SSL_write(port->ssl, ptr, len);
	err = SSL_get_error(port->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			/* 正常 */
			break;
		case SSL_ERROR_WANT_READ:
			*waitfor = WL_SOCKET_READABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_WANT_WRITE:
			*waitfor = WL_SOCKET_WRITEABLE;
			errno = EWOULDBLOCK;
			n = -1;
			break;
		case SSL_ERROR_SYSCALL:

			/*
			 * 把 errno 的值留给调用方去 ereport。不过，若
			 * errno 仍为零，就假定这是读到 EOF 的情况，并
			 * 报告 ECONNRESET。（这似乎是可能的，因为 SSL_write 也能
			 * 执行读取。）
			 */
			if (n != -1 || errno == 0)
			{
				errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("SSL error: %s", SSLerrmessage(ecode))));
			errno = ECONNRESET;
			n = -1;
			break;
		case SSL_ERROR_ZERO_RETURN:

			/*
			 * SSL 连接已关闭，留给调用方去
			 * ereport
			 */
			errno = ECONNRESET;
			n = -1;
			break;
		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unrecognized SSL error code: %d",
							err)));
			errno = ECONNRESET;
			n = -1;
			break;
	}

	return n;
}

/* ------------------------------------------------------------ */
/*						内部函数						*/
/* ------------------------------------------------------------ */

/*
 * 私有替代 BIO：改用 send() 和
 * recv() 来发送和接收。这样我们就能启用和禁用中断，
 * 并且只在调用 recv() 期间这样做。OpenSSL 主体运行时我们无法让中断发生，
 * 因为它使用 malloc() 以及其他
 * 不可重入的 libc 设施。我们还需要直接调用 send() 和 recv()，
 * 以便在 Win32 上穿过套接字/信号层。
 *
 * 这些函数紧密仿照 OpenSSL 中的标准套接字 BIO；
 * 参见 OpenSSL 的 crypto/bio/bss_sock.c 中的 sock_read() 和 sock_write()。
 * XXX OpenSSL 1.0.1e 把远不止 EINTR 的许多错误码当作重试
 * 原因；我们是否需要采用它们的那套逻辑？
 */

static BIO_METHOD *my_bio_methods = NULL;

static int
my_sock_read(BIO *h, char *buf, int size)
{
	int			res = 0;

	if (buf != NULL)
	{
		res = secure_raw_read(((Port *) BIO_get_app_data(h)), buf, size);
		BIO_clear_retry_flags(h);
		if (res <= 0)
		{
			/* 若被中断，告诉调用方重试 */
			if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
			{
				BIO_set_retry_read(h);
			}
		}
	}

	return res;
}

static int
my_sock_write(BIO *h, const char *buf, int size)
{
	int			res = 0;

	res = secure_raw_write(((Port *) BIO_get_app_data(h)), buf, size);
	BIO_clear_retry_flags(h);
	if (res <= 0)
	{
		/* 若被中断，告诉调用方重试 */
		if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
		{
			BIO_set_retry_write(h);
		}
	}

	return res;
}

static BIO_METHOD *
my_BIO_s_socket(void)
{
	if (!my_bio_methods)
	{
		BIO_METHOD *biom = (BIO_METHOD *) BIO_s_socket();
#ifdef HAVE_BIO_METH_NEW
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			return NULL;
		my_bio_index |= (BIO_TYPE_DESCRIPTOR | BIO_TYPE_SOURCE_SINK);
		my_bio_methods = BIO_meth_new(my_bio_index, "PostgreSQL backend socket");
		if (!my_bio_methods)
			return NULL;
		if (!BIO_meth_set_write(my_bio_methods, my_sock_write) ||
			!BIO_meth_set_read(my_bio_methods, my_sock_read) ||
			!BIO_meth_set_gets(my_bio_methods, BIO_meth_get_gets(biom)) ||
			!BIO_meth_set_puts(my_bio_methods, BIO_meth_get_puts(biom)) ||
			!BIO_meth_set_ctrl(my_bio_methods, BIO_meth_get_ctrl(biom)) ||
			!BIO_meth_set_create(my_bio_methods, BIO_meth_get_create(biom)) ||
			!BIO_meth_set_destroy(my_bio_methods, BIO_meth_get_destroy(biom)) ||
			!BIO_meth_set_callback_ctrl(my_bio_methods, BIO_meth_get_callback_ctrl(biom)))
		{
			BIO_meth_free(my_bio_methods);
			my_bio_methods = NULL;
			return NULL;
		}
#else
		my_bio_methods = malloc(sizeof(BIO_METHOD));
		if (!my_bio_methods)
			return NULL;
		memcpy(my_bio_methods, biom, sizeof(BIO_METHOD));
		my_bio_methods->bread = my_sock_read;
		my_bio_methods->bwrite = my_sock_write;
#endif
	}
	return my_bio_methods;
}

/* 除了使用我的 BIO 之外，这应当与 OpenSSL 的 SSL_set_fd 完全一致 */
static int
my_SSL_set_fd(Port *port, int fd)
{
	int			ret = 0;
	BIO		   *bio;
	BIO_METHOD *bio_method;

	bio_method = my_BIO_s_socket();
	if (bio_method == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	bio = BIO_new(bio_method);

	if (bio == NULL)
	{
		SSLerr(SSL_F_SSL_SET_FD, ERR_R_BUF_LIB);
		goto err;
	}
	BIO_set_app_data(bio, port);

	BIO_set_fd(bio, fd, BIO_NOCLOSE);
	SSL_set_bio(port->ssl, bio, bio);
	ret = 1;
err:
	return ret;
}

/*
 *	加载预先计算的 DH 参数。
 *
 *	为防止 "降级" 攻击，我们做若干检查，
 *	以验证 DBA 生成的 DH 参数文件包含
 *	我们所预期的内容。
 */
static DH  *
load_dh_file(char *filename, bool isServerStart)
{
	FILE	   *fp;
	DH		   *dh = NULL;
	int			codes;

	/* 尝试打开文件。文件不存在并不是错误。 */
	if ((fp = AllocateFile(filename, "r")) == NULL)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode_for_file_access(),
				 errmsg("could not open DH parameters file \"%s\": %m",
						filename)));
		return NULL;
	}

	dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
	FreeFile(fp);

	if (dh == NULL)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not load DH parameters file: %s",
						SSLerrmessage(ERR_get_error()))));
		return NULL;
	}

	/* 确保 DH 参数可用 */
	if (DH_check(dh, &codes) == 0)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: %s",
						SSLerrmessage(ERR_get_error()))));
		DH_free(dh);
		return NULL;
	}
	if (codes & DH_CHECK_P_NOT_PRIME)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: p is not prime")));
		DH_free(dh);
		return NULL;
	}
	if ((codes & DH_NOT_SUITABLE_GENERATOR) &&
		(codes & DH_CHECK_P_NOT_SAFE_PRIME))
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid DH parameters: neither suitable generator or safe prime")));
		DH_free(dh);
		return NULL;
	}

	return dh;
}

/*
 *	加载硬编码的 DH 参数。
 *
 *	若无法从指定文件加载 DH 参数，我们可以加载
 *	后端自带的硬编码 DH 参数，以防止
 *	出问题。
 */
static DH  *
load_dh_buffer(const char *buffer, size_t len)
{
	BIO		   *bio;
	DH		   *dh = NULL;

	bio = BIO_new_mem_buf(unconstify(char *, buffer), len);
	if (bio == NULL)
		return NULL;
	dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
	if (dh == NULL)
		ereport(DEBUG2,
				(errmsg_internal("DH load buffer: %s",
								 SSLerrmessage(ERR_get_error()))));
	BIO_free(bio);

	return dh;
}

/*
 *	使用 ssl_passphrase_command 的口令收集回调
 */
static int
ssl_external_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	/* 与 OpenSSL 内部使用的提示相同 */
	const char *prompt = "Enter PEM pass phrase:";

	Assert(rwflag == 0);

	return run_ssl_passphrase_command(prompt, ssl_is_server_start, buf, size);
}

/*
 * 哑口令回调
 *
 * 若 OpenSSL 被告知使用口令保护的服务器密钥，默认情况下
 * 它会在 /dev/tty 上发出提示，并试图从那里读取密钥。
 * 在 postmaster 的 SIGHUP 周期里这不行，更不用说 SSL 上下文
 * 在 EXEC_BACKEND postmaster 子进程里重新加载。因此用这个哑
 * 函数覆盖它，该函数只返回空口令，从而保证失败。
 */
static int
dummy_ssl_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	/* 设置标志，以改变我们将报告的错误消息 */
	dummy_ssl_passwd_cb_called = true;
	/* 并返回空字符串 */
	Assert(size > 0);
	buf[0] = '\0';
	return 0;
}

/*
 * 检查给出的证书名，若它长到不适合记入日志，或
 * 含有不可打印的 ASCII，就对其转义并截断。返回值
 * 总是一个新 palloc 出来的字符串。（输入字符串仍会被就地修改，
 * 以便实现。）
 */
static char *
prepare_cert_name(char *name)
{
	size_t		namelen = strlen(name);
	char	   *truncated = name;

	/*
	 * 公用名最长 64 个字符，因此在 CN 是
	 * 最后一个字段的常见情况下，我们仍能打印最长的 CN，并带一个
	 * 7 字符前缀（".../CN=[64 chars]"），合理上限是 71
	 * 个字符。
	 */
#define MAXLEN 71

	if (namelen > MAXLEN)
	{
		/*
		 * 保留名字的末尾而不是开头，因为最
		 * 具体的字段很可能给用户最多信息。
		 */
		truncated = name + namelen - MAXLEN;
		truncated[0] = truncated[1] = truncated[2] = '.';
		namelen = MAXLEN;
	}

#undef MAXLEN

	return pg_clean_ascii(truncated, 0);
}

/*
 *	证书验证回调
 *
 *	这个回调让我们在
 *	验证期间检查中间问题，以便稍后记入日志。
 *
 *	这个回调也让我们覆盖默认的接受
 *	标准（例如接受自签名或过期的证书），但
 *	目前我们接受默认检查。
 */
static int
verify_cb(int ok, X509_STORE_CTX *ctx)
{
	int			depth;
	int			errcode;
	const char *errstring;
	StringInfoData str;
	X509	   *cert;

	if (ok)
	{
		/* 成功情况下无事可做。 */
		return ok;
	}

	/* 取出我们掌握的关于验证失败的全部信息。 */
	depth = X509_STORE_CTX_get_error_depth(ctx);
	errcode = X509_STORE_CTX_get_error(ctx);
	errstring = X509_verify_cert_error_string(errcode);

	initStringInfo(&str);
	appendStringInfo(&str,
					 _("Client certificate verification failed at depth %d: %s."),
					 depth, errstring);

	cert = X509_STORE_CTX_get_current_cert(ctx);
	if (cert)
	{
		char	   *subject,
				   *issuer;
		char	   *sub_prepared,
				   *iss_prepared;
		char	   *serialno;
		ASN1_INTEGER *sn;
		BIGNUM	   *b;

		/*
		 * 取出 Subject 和 Issuer 以便记入日志，但不要让恶意的
		 * 巨型证书灌满日志，也不要把非 ASCII 字节反映
		 * 进去。
		 */
		subject = X509_NAME_to_cstring(X509_get_subject_name(cert));
		sub_prepared = prepare_cert_name(subject);
		pfree(subject);

		issuer = X509_NAME_to_cstring(X509_get_issuer_name(cert));
		iss_prepared = prepare_cert_name(issuer);
		pfree(issuer);

		/*
		 * 也取出序列号，以防 Subject 仍然有歧义。
		 * 这与 be_tls_get_peer_serial() 一致。
		 */
		sn = X509_get_serialNumber(cert);
		b = ASN1_INTEGER_to_BN(sn, NULL);
		serialno = BN_bn2dec(b);

		appendStringInfoChar(&str, '\n');
		appendStringInfo(&str,
						 _("Failed certificate data (unverified): subject \"%s\", serial number %s, issuer \"%s\"."),
						 sub_prepared, serialno ? serialno : _("unknown"),
						 iss_prepared);

		BN_free(b);
		OPENSSL_free(serialno);
		pfree(iss_prepared);
		pfree(sub_prepared);
	}

	/* 把详细消息存起来，稍后记入日志。 */
	cert_errdetail = str.data;

	return ok;
}

/*
 *	这个回调用于把 SSL 信息消息
 *	复制到 PostgreSQL 日志中。
 */
static void
info_cb(const SSL *ssl, int type, int args)
{
	const char *desc;

	desc = SSL_state_string_long(ssl);

	switch (type)
	{
		case SSL_CB_HANDSHAKE_START:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake start: \"%s\"", desc)));
			break;
		case SSL_CB_HANDSHAKE_DONE:
			ereport(DEBUG4,
					(errmsg_internal("SSL: handshake done: \"%s\"", desc)));
			break;
		case SSL_CB_ACCEPT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept loop: \"%s\"", desc)));
			break;
		case SSL_CB_ACCEPT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: accept exit (%d): \"%s\"", args, desc)));
			break;
		case SSL_CB_CONNECT_LOOP:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect loop: \"%s\"", desc)));
			break;
		case SSL_CB_CONNECT_EXIT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: connect exit (%d): \"%s\"", args, desc)));
			break;
		case SSL_CB_READ_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: read alert (0x%04x): \"%s\"", args, desc)));
			break;
		case SSL_CB_WRITE_ALERT:
			ereport(DEBUG4,
					(errmsg_internal("SSL: write alert (0x%04x): \"%s\"", args, desc)));
			break;
	}
}

/*
 * 设置用于生成临时 DH 密钥的 DH 参数。
 * DH 参数的计算可能很久，因此必须
 * 预先计算。
 *
 * 由于很少有站点会去创建参数文件，我们也
 * 提供一个后备，使用 OpenSSL
 * 项目提供的参数。
 *
 * 这些值可以是静态的（一旦加载或计算完成），因为
 * OpenSSL 库能根据所提供的
 * 信息高效地生成随机密钥。
 */
static bool
initialize_dh(SSL_CTX *context, bool isServerStart)
{
	DH		   *dh = NULL;

	SSL_CTX_set_options(context, SSL_OP_SINGLE_DH_USE);

	if (ssl_dh_params_file[0])
		dh = load_dh_file(ssl_dh_params_file, isServerStart);
	if (!dh)
		dh = load_dh_buffer(FILE_DH2048, sizeof(FILE_DH2048));
	if (!dh)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("DH: could not load DH parameters")));
		return false;
	}

	if (SSL_CTX_set_tmp_dh(context, dh) != 1)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("DH: could not set DH parameters: %s",
						SSLerrmessage(ERR_get_error()))));
		DH_free(dh);
		return false;
	}

	DH_free(dh);
	return true;
}

/*
 * 设置用于生成临时椭圆曲线 DH
 * 密钥的 ECDH 参数。这比 DH 参数简单得多，因为我们只需
 * 向 OpenSSL 提供曲线名称。
 */
static bool
initialize_ecdh(SSL_CTX *context, bool isServerStart)
{
#ifndef OPENSSL_NO_ECDH
	EC_KEY	   *ecdh;
	int			nid;

	nid = OBJ_sn2nid(SSLECDHCurve);
	if (!nid)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ECDH: unrecognized curve name: %s", SSLECDHCurve)));
		return false;
	}

	ecdh = EC_KEY_new_by_curve_name(nid);
	if (!ecdh)
	{
		ereport(isServerStart ? FATAL : LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ECDH: could not create key")));
		return false;
	}

	SSL_CTX_set_options(context, SSL_OP_SINGLE_ECDH_USE);
	SSL_CTX_set_tmp_ecdh(context, ecdh);
	EC_KEY_free(ecdh);
#endif

	return true;
}

/*
 * 取得传入的 SSL 错误码的原因字符串
 *
 * 调用方用 ERR_get_error() 取得要传到这里的错误码。
 *
 * 这里需要小心，因为若无法识别错误码，ERR_reason_error_string 会返回 NULL，
 * 或者（在 OpenSSL >= 3 中）该码
 * 表示系统 errno 时也会返回 NULL。我们绝不希望返回 NULL。
 */
static const char *
SSLerrmessage(unsigned long ecode)
{
	const char *errreason;
	static char errbuf[36];

	if (ecode == 0)
		return _("no SSL error reported");
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
		return errreason;

	/*
	 * 在 OpenSSL 3.0.0 及以后，ERR_reason_error_string 不再映射系统
	 * errno 值。（解释见 OpenSSL 源代码。）
	 * 我们可以用这段代码补上该不足。较旧的 OpenSSL
	 * 版本没有 ERR_SYSTEM_ERROR 宏，但这没关系，因为
	 * 它们也没有这个不足。
	 */
#ifdef ERR_SYSTEM_ERROR
	if (ERR_SYSTEM_ERROR(ecode))
		return strerror(ERR_GET_REASON(ecode));
#endif

	/* 只好报告数值形式的错误码 */
	snprintf(errbuf, sizeof(errbuf), _("SSL error code %lu"), ecode);
	return errbuf;
}

int
be_tls_get_cipher_bits(Port *port)
{
	int			bits;

	if (port->ssl)
	{
		SSL_get_cipher_bits(port->ssl, &bits);
		return bits;
	}
	else
		return 0;
}

const char *
be_tls_get_version(Port *port)
{
	if (port->ssl)
		return SSL_get_version(port->ssl);
	else
		return NULL;
}

const char *
be_tls_get_cipher(Port *port)
{
	if (port->ssl)
		return SSL_get_cipher(port->ssl);
	else
		return NULL;
}

void
be_tls_get_peer_subject_name(Port *port, char *ptr, size_t len)
{
	if (port->peer)
		strlcpy(ptr, X509_NAME_to_cstring(X509_get_subject_name(port->peer)), len);
	else
		ptr[0] = '\0';
}

void
be_tls_get_peer_issuer_name(Port *port, char *ptr, size_t len)
{
	if (port->peer)
		strlcpy(ptr, X509_NAME_to_cstring(X509_get_issuer_name(port->peer)), len);
	else
		ptr[0] = '\0';
}

void
be_tls_get_peer_serial(Port *port, char *ptr, size_t len)
{
	if (port->peer)
	{
		ASN1_INTEGER *serial;
		BIGNUM	   *b;
		char	   *decimal;

		serial = X509_get_serialNumber(port->peer);
		b = ASN1_INTEGER_to_BN(serial, NULL);
		decimal = BN_bn2dec(b);

		BN_free(b);
		strlcpy(ptr, decimal, len);
		OPENSSL_free(decimal);
	}
	else
		ptr[0] = '\0';
}

#if defined(HAVE_X509_GET_SIGNATURE_NID) || defined(HAVE_X509_GET_SIGNATURE_INFO)
char *
be_tls_get_certificate_hash(Port *port, size_t *len)
{
	X509	   *server_cert;
	char	   *cert_hash;
	const EVP_MD *algo_type = NULL;
	unsigned char hash[EVP_MAX_MD_SIZE];	/* SHA-512 用的大小 */
	unsigned int hash_size;
	int			algo_nid;

	*len = 0;
	server_cert = SSL_get_certificate(port->ssl);
	if (server_cert == NULL)
		return NULL;

	/*
	 * 取得证书的签名算法，以确定结果要使用的散列
	 * 算法。优先使用 X509_get_signature_info()，
	 * 它在 OpenSSL 1.1.1 引入，能处理 RSA-PSS 签名。
	 */
#if HAVE_X509_GET_SIGNATURE_INFO
	if (!X509_get_signature_info(server_cert, &algo_nid, NULL, NULL, NULL))
#else
	if (!OBJ_find_sigid_algs(X509_get_signature_nid(server_cert),
							 &algo_nid, NULL))
#endif
		elog(ERROR, "could not determine server certificate signature algorithm");

	/*
	 * 若 TLS 服务器证书的签名算法是 MD5 或 SHA-1，按 RFC 5929
	 * 其证书字节需要用 SHA-256 做散列
	 * （https://tools.ietf.org/html/rfc5929#section-4.1）。若使用的是别的
	 * 算法，就使用与签名算法相同的散列。
	 */
	switch (algo_nid)
	{
		case NID_md5:
		case NID_sha1:
			algo_type = EVP_sha256();
			break;
		default:
			algo_type = EVP_get_digestbynid(algo_nid);
			if (algo_type == NULL)
				elog(ERROR, "could not find digest for NID %s",
					 OBJ_nid2sn(algo_nid));
			break;
	}

	/* 生成并保存证书散列 */
	if (!X509_digest(server_cert, algo_type, hash, &hash_size))
		elog(ERROR, "could not generate server certificate hash");

	cert_hash = palloc(hash_size);
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
}
#endif

/*
 * 把 X509 主题名转换成 cstring。
 *
 */
static char *
X509_NAME_to_cstring(X509_NAME *name)
{
	BIO		   *membuf = BIO_new(BIO_s_mem());
	int			i,
				nid,
				count = X509_NAME_entry_count(name);
	X509_NAME_ENTRY *e;
	ASN1_STRING *v;
	const char *field_name;
	size_t		size;
	char		nullterm;
	char	   *sp;
	char	   *dp;
	char	   *result;

	if (membuf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create BIO")));

	(void) BIO_set_close(membuf, BIO_CLOSE);
	for (i = 0; i < count; i++)
	{
		e = X509_NAME_get_entry(name, i);
		nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(e));
		if (nid == NID_undef)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not get NID for ASN1_OBJECT object")));
		v = X509_NAME_ENTRY_get_data(e);
		field_name = OBJ_nid2sn(nid);
		if (field_name == NULL)
			field_name = OBJ_nid2ln(nid);
		if (field_name == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not convert NID %d to an ASN1_OBJECT structure", nid)));
		BIO_printf(membuf, "/%s=", field_name);
		ASN1_STRING_print_ex(membuf, v,
							 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
							  | ASN1_STRFLGS_UTF8_CONVERT));
	}

	/* 确保 BIO 内容以空字符结尾 */
	nullterm = '\0';
	BIO_write(membuf, &nullterm, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = pg_any_to_server(sp, size - 1, PG_UTF8);

	result = pstrdup(dp);
	if (dp != sp)
		pfree(dp);
	if (BIO_free(membuf) != 1)
		elog(ERROR, "could not free OpenSSL BIO structure");

	return result;
}

/*
 * 把 TLS 协议版本 GUC 枚举转换成 OpenSSL 的值
 *
 * 这是直接的一一映射，但这样做使
 * ssl_min_protocol_version 和 ssl_max_protocol_version
 * 的定义独立于 OpenSSL 是否可用及其版本。
 *
 * 若传入的版本不受当前 OpenSSL
 * 版本支持，我们就返回 -1。若返回了非负值，
 * 后续代码可以假定它面对的是受支持的版本。
 *
 * 注意：这与 libpq 在 fe-secure-openssl.c 中的例程相当相似，
 * 因此若修改这一处，请务必同时更新两处例程。
 */
static int
ssl_protocol_version_to_openssl(int v)
{
	switch (v)
	{
		case PG_TLS_ANY:
			return 0;
		case PG_TLS1_VERSION:
			return TLS1_VERSION;
		case PG_TLS1_1_VERSION:
#ifdef TLS1_1_VERSION
			return TLS1_1_VERSION;
#else
			break;
#endif
		case PG_TLS1_2_VERSION:
#ifdef TLS1_2_VERSION
			return TLS1_2_VERSION;
#else
			break;
#endif
		case PG_TLS1_3_VERSION:
#ifdef TLS1_3_VERSION
			return TLS1_3_VERSION;
#else
			break;
#endif
	}

	return -1;
}

/*
 * 同样提供到字符串的映射。
 */
static const char *
ssl_protocol_version_to_string(int v)
{
	switch (v)
	{
		case PG_TLS_ANY:
			return "any";
		case PG_TLS1_VERSION:
			return "TLSv1";
		case PG_TLS1_1_VERSION:
			return "TLSv1.1";
		case PG_TLS1_2_VERSION:
			return "TLSv1.2";
		case PG_TLS1_3_VERSION:
			return "TLSv1.3";
	}

	return "(unrecognized)";
}


static void
default_openssl_tls_init(SSL_CTX *context, bool isServerStart)
{
	if (isServerStart)
	{
		if (ssl_passphrase_command[0])
			SSL_CTX_set_default_passwd_cb(context, ssl_external_passwd_cb);
	}
	else
	{
		if (ssl_passphrase_command[0] && ssl_passphrase_command_supports_reload)
			SSL_CTX_set_default_passwd_cb(context, ssl_external_passwd_cb);
		else

			/*
			 * 若正在重新加载且没有配置外部命令，就覆盖
			 * OpenSSL 对口令保护文件的默认处理，
			 * 因为我们不希望在已经运行的服务器里
			 * 提示输入口令。
			 */
			SSL_CTX_set_default_passwd_cb(context, dummy_ssl_passwd_cb);
	}
}
