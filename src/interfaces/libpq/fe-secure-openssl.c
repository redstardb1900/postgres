/*-------------------------------------------------------------------------
 *
 * fe-secure-openssl.c
 *	  OpenSSL support
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-openssl.c
 *
 * NOTES
 *
 *	  We don't provide informational callbacks here (like
 *	  info_cb() in be-secure-openssl.c), since there's no good mechanism to
 *	  display such information to the user.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <signal.h>
#include <fcntl.h>
#include <ctype.h>

#include "libpq-fe.h"
#include "fe-auth.h"
#include "fe-secure-common.h"
#include "libpq-int.h"

#ifdef WIN32
#include "win32.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#include <sys/stat.h>

#ifdef ENABLE_THREAD_SAFETY
#ifdef WIN32
#include "pthread-win32.h"
#else
#include <pthread.h>
#endif
#endif

/*
 * 这些 SSL 相关的 #include 必须位于所有系统提供的头文件之后。
 * 这确保 OpenSSL 可通过 #undef'ing 冲突宏来处理与 Windows
 * <wincrypt.h> 的冲突。（我们不直接包含 <wincrypt.h>，
 * 但某些其他 Windows 头文件会包含它。）
 */
#include "common/openssl.h"
#include <openssl/conf.h>
#ifdef USE_SSL_ENGINE
#include <openssl/engine.h>
#endif
#include <openssl/x509v3.h>


static int	verify_cb(int ok, X509_STORE_CTX *ctx);
static int	openssl_verify_peer_name_matches_certificate_name(PGconn *conn,
															  ASN1_STRING *name_entry,
															  char **store_name);
static int	openssl_verify_peer_name_matches_certificate_ip(PGconn *conn,
															ASN1_OCTET_STRING *addr_entry,
															char **store_name);
static void destroy_ssl_system(void);
static int	initialize_SSL(PGconn *conn);
static PostgresPollingStatusType open_client_SSL(PGconn *conn);
static char *SSLerrmessage(unsigned long ecode);
static void SSLerrfree(char *buf);
static int	PQssl_passwd_cb(char *buf, int size, int rwflag, void *userdata);

static int	my_sock_read(BIO *h, char *buf, int size);
static int	my_sock_write(BIO *h, const char *buf, int size);
static BIO_METHOD *my_BIO_s_socket(void);
static int	my_SSL_set_fd(PGconn *conn, int fd);


static bool pq_init_ssl_lib = true;
static bool pq_init_crypto_lib = true;

static bool ssl_lib_initialized = false;

#ifdef ENABLE_THREAD_SAFETY
static long crypto_open_connections = 0;

static pthread_mutex_t ssl_config_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif							/* ENABLE_THREAD_SAFETY */

static PQsslKeyPassHook_OpenSSL_type PQsslKeyPassHook = NULL;
static int	ssl_protocol_version_to_openssl(const char *protocol);

/* ------------------------------------------------------------ */
/*			 所有安全会话通用的过程       			*/
/* ------------------------------------------------------------ */

void
pgtls_init_library(bool do_ssl, int do_crypto)
{
#ifdef ENABLE_THREAD_SAFETY

	/*
	 * 禁止在仍有打开连接时更改标志，否则我们会
	 * 完全混乱。
	                   */
	if (crypto_open_connections != 0)
		return;
#endif

	pq_init_ssl_lib = do_ssl;
	pq_init_crypto_lib = do_crypto;
}

PostgresPollingStatusType
pgtls_open_client(PGconn *conn)
{
	/* 第一次进入？  */
	if (conn->ssl == NULL)
	{
		/*
		 * 创建连接特定的 SSL 对象，并加载客户端
		 * 证书、私钥和受信任的 CA 证书。
		        */
		if (initialize_SSL(conn) != 0)
		{
			/* initialize_SSL 已向 conn->errorMessage 写入消息      */
			pgtls_close(conn);
			return PGRES_POLLING_FAILED;
		}
	}

	/* 开始或继续实际的握手过程   */
	return open_client_SSL(conn);
}

ssize_t
pgtls_read(PGconn *conn, void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];
	int			err;
	unsigned long ecode;

rloop:

	/*
	 * 在调用 SSL_get_error() 之前，清除线程的 OpenSSL 错误
	 * 队列。通常，在尝试 TLS/SSL I/O 操作之前，当前线程的
	 * 错误队列必须为空，否则 SSL_get_error() 将无法可靠工作。
	 * 由于可能存在运行在同一线程中但不受我们控制的其他
	 * OpenSSL 客户端，它们可能不会自己调用 ERR_get_error()
	 * （在其自身的 I/O 操作之后），因此现在主动清除每线程
	 * 错误队列。
	                      */
	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	n = SSL_read(conn->ssl, ptr, len);
	err = SSL_get_error(conn->ssl, n);

	/*
	 * 其他 OpenSSL 客户端可能不会调用 ERR_get_error()，    
	 * 但我们总是这样做，以免给不防御性调用               
	 * ERR_clear_error() 的 OpenSSL 客户端带来问题。           
	 * 现在就调用即可确保这一点。SSL_get_error() 依赖       
	 * 于 OpenSSL 每线程错误队列完好，因此这里是 
	 * ERR_get_error() 的最早点  
	 */
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			if (n < 0)
			{
				/* 不应该发生，因此我们不翻译该消息      */
				appendPQExpBufferStr(&conn->errorMessage,
									 "SSL_read failed but did not provide error information\n");
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
			}
			break;
		case SSL_ERROR_WANT_READ:
			n = 0;
			break;
		case SSL_ERROR_WANT_WRITE:

			/*
			 * 此处返回 0 会导致调用者等待读就绪，
			 * 但 SSL 想要的是等待写就绪，因此不正确。
			 * 前者可能让我们陷入无限等待，所以不冒这个险；
			 * 改为忙循环。
			                      */
			goto rloop;
		case SSL_ERROR_SYSCALL:
			if (n < 0 && SOCK_ERRNO != 0)
			{
				result_errno = SOCK_ERRNO;
				if (result_errno == EPIPE ||
					result_errno == ECONNRESET)
					libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
											"\tThis probably means the server terminated abnormally\n"
											"\tbefore or while processing the request.");
				else
					libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
											SOCK_STRERROR(result_errno,
														  sebuf, sizeof(sebuf)));
			}
			else
			{
				libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			{
				char	   *errm = SSLerrmessage(ecode);

				libpq_append_conn_error(conn, "SSL error: %s", errm);
				SSLerrfree(errm);
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
				n = -1;
				break;
			}
		case SSL_ERROR_ZERO_RETURN:

			/*
			 * 根据 OpenSSL 文档，此错误码仅在干净关闭
			 * 连接时返回，因此我们不应将其报告为
			 * 服务器崩溃。
			             */
			libpq_append_conn_error(conn, "SSL connection has been closed unexpectedly");
			result_errno = ECONNRESET;
			n = -1;
			break;
		default:
			libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
			/* 假设连接已断开           */
			result_errno = ECONNRESET;
			n = -1;
			break;
	}

	/* 确保向调用者返回预期的 errno       */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

bool
pgtls_read_pending(PGconn *conn)
{
	return SSL_pending(conn->ssl) > 0;
}

ssize_t
pgtls_write(PGconn *conn, const void *ptr, size_t len)
{
	ssize_t		n;
	int			result_errno = 0;
	char		sebuf[PG_STRERROR_R_BUFLEN];
	int			err;
	unsigned long ecode;

	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	n = SSL_write(conn->ssl, ptr, len);
	err = SSL_get_error(conn->ssl, n);
	ecode = (err != SSL_ERROR_NONE || n < 0) ? ERR_get_error() : 0;
	switch (err)
	{
		case SSL_ERROR_NONE:
			if (n < 0)
			{
				/* 不应该发生，因此我们不翻译该消息      */
				appendPQExpBufferStr(&conn->errorMessage,
									 "SSL_write failed but did not provide error information\n");
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
			}
			break;
		case SSL_ERROR_WANT_READ:

			/*
			 * 此处返回 0 会导致调用者等待写就绪，
			 * 这并非完全正确，但这是我们能做的最佳选择。
			       */
			n = 0;
			break;
		case SSL_ERROR_WANT_WRITE:
			n = 0;
			break;
		case SSL_ERROR_SYSCALL:

			/*
			 * 如果 errno 仍为零，则假定为读 EOF 并报告。    
			 * （这有可能，因为 SSL_write 也会做读取。）   
			 *                
			 */
			if (n < 0 && SOCK_ERRNO != 0)
			{
				result_errno = SOCK_ERRNO;
				if (result_errno == EPIPE || result_errno == ECONNRESET)
					libpq_append_conn_error(conn, "server closed the connection unexpectedly\n"
											"\tThis probably means the server terminated abnormally\n"
											"\tbefore or while processing the request.");
				else
					libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
											SOCK_STRERROR(result_errno,
														  sebuf, sizeof(sebuf)));
			}
			else
			{
				libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
				n = -1;
			}
			break;
		case SSL_ERROR_SSL:
			{
				char	   *errm = SSLerrmessage(ecode);

				libpq_append_conn_error(conn, "SSL error: %s", errm);
				SSLerrfree(errm);
				/* 假设连接已断开           */
				result_errno = ECONNRESET;
				n = -1;
				break;
			}
		case SSL_ERROR_ZERO_RETURN:

			/*
			 * 根据 OpenSSL 文档，此错误码仅在干净关闭
			 * 连接时返回，因此我们不应将其报告为
			 * 服务器崩溃。
			             */
			libpq_append_conn_error(conn, "SSL connection has been closed unexpectedly");
			result_errno = ECONNRESET;
			n = -1;
			break;
		default:
			libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
			/* 假设连接已断开           */
			result_errno = ECONNRESET;
			n = -1;
			break;
	}

	/* 确保向调用者返回预期的 errno       */
	SOCK_ERRNO_SET(result_errno);

	return n;
}

#if defined(HAVE_X509_GET_SIGNATURE_NID) || defined(HAVE_X509_GET_SIGNATURE_INFO)
char *
pgtls_get_peer_certificate_hash(PGconn *conn, size_t *len)
{
	X509	   *peer_cert;
	const EVP_MD *algo_type;
	unsigned char hash[EVP_MAX_MD_SIZE];	/* SHA-512 尺寸   */
	unsigned int hash_size;
	int			algo_nid;
	char	   *cert_hash;

	*len = 0;

	if (!conn->peer)
		return NULL;

	peer_cert = conn->peer;

	/*
	 * 获取证书的签名算法以确定用于结果的
	 * 哈希算法。优先使用 X509_get_signature_info()，
	 * 该函数在 OpenSSL 1.1.1 中引入，可以处理 RSA-PSS 签名。
	                         */
#if HAVE_X509_GET_SIGNATURE_INFO
	if (!X509_get_signature_info(peer_cert, &algo_nid, NULL, NULL, NULL))
#else
	if (!OBJ_find_sigid_algs(X509_get_signature_nid(peer_cert),
							 &algo_nid, NULL))
#endif
	{
		libpq_append_conn_error(conn, "could not determine server certificate signature algorithm");
		return NULL;
	}

	/*
	 * TLS 服务器的证书字节需要使用 SHA-256 进行哈希，如果其
	 * 签名算法是 MD5 或 SHA-1，根据 RFC 5929
	 * (https://tools.ietf.org/html/rfc5929#section-4.1)。
	 * 如果使用其他算法，则使用与签名算法相同的哈希。
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
			{
				libpq_append_conn_error(conn, "could not find digest for NID %s",
										OBJ_nid2sn(algo_nid));
				return NULL;
			}
			break;
	}

	if (!X509_digest(peer_cert, algo_type, hash, &hash_size))
	{
		libpq_append_conn_error(conn, "could not generate peer certificate hash");
		return NULL;
	}

	/* 保存      */
	cert_hash = malloc(hash_size);
	if (cert_hash == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return NULL;
	}
	memcpy(cert_hash, hash, hash_size);
	*len = hash_size;

	return cert_hash;
}
#endif							/* HAVE_X509_GET_SIGNATURE_NID */

/* ------------------------------------------------------------ */
/*						OpenSSL 专用代码 					*/
/* ------------------------------------------------------------ */

/*
 *	证书验证回调
 *
 *	此回调允许我们记录验证过程中的中间问题，
 *	但似乎没有干净的方法来获取我们的 PGconn * 结构。
 *	因此我们无法记录任何内容！
 *
 *	此回调还允许我们覆盖默认的接受
 *	标准（例如，接受自签名或过期的证书），但
 *	目前我们接受默认检查。
                                          */
static int
verify_cb(int ok, X509_STORE_CTX *ctx)
{
	return ok;
}

#ifdef HAVE_SSL_CTX_SET_CERT_CB
/*
 * 证书选择回调
 *
 * 此回调允许我们在看到服务器的 CertificateRequest 后选择
 * 要发送给服务器的客户端证书。我们仅支持通过 sslcert 发送
 * 单个硬编码证书，因此实际上不在此处设置任何证书；
 * 我们只是用它来记录服务器是否实际请求了证书以及
 * 我们是否有证书可发送。
                         */
static int
cert_cb(SSL *ssl, void *arg)
{
	PGconn	   *conn = arg;

	conn->ssl_cert_requested = true;

	/* 我们有已加载的证书可以发回吗？ */
	if (SSL_get_certificate(ssl))
		conn->ssl_cert_sent = true;

	/*
	 * 告诉 OpenSSL 回调成功；我们不需要
	 * 实际对 SSL 句柄进行任何更改。
	                      */
	return 1;
}
#endif

/*
 * pq_verify_peer_name_matches_certificate_name() 的
 * OpenSSL 特定包装器，将 ASN1_STRING 转换为普通的
 * C 字符串。
    */
static int
openssl_verify_peer_name_matches_certificate_name(PGconn *conn, ASN1_STRING *name_entry,
												  char **store_name)
{
	int			len;
	const unsigned char *namedata;

	/* 不应该发生...   */
	if (name_entry == NULL)
	{
		libpq_append_conn_error(conn, "SSL certificate's name entry is missing");
		return -1;
	}

	/*
	 * GEN_DNS 只能是 IA5String，相当于 US ASCII。
	    */
#ifdef HAVE_ASN1_STRING_GET0_DATA
	namedata = ASN1_STRING_get0_data(name_entry);
#else
	namedata = ASN1_STRING_data(name_entry);
#endif
	len = ASN1_STRING_length(name_entry);

	/* 从 unsigned 转成普通 char 没问题，都是 ASCII。    */
	return pq_verify_peer_name_matches_certificate_name(conn, (const char *) namedata, len, store_name);
}

/*
 * pq_verify_peer_name_matches_certificate_ip() 的
 * OpenSSL 特定包装器，将 ASN1_OCTET_STRING 转换为
 * 普通的 C 字符串。
   */
static int
openssl_verify_peer_name_matches_certificate_ip(PGconn *conn,
												ASN1_OCTET_STRING *addr_entry,
												char **store_name)
{
	int			len;
	const unsigned char *addrdata;

	/* 不应该发生...   */
	if (addr_entry == NULL)
	{
		libpq_append_conn_error(conn, "SSL certificate's address entry is missing");
		return -1;
	}

	/*
	 * GEN_IPADD 是 OCTET STRING，包含以网络字节序表示的 IP    
	 * 地址
	 */
#ifdef HAVE_ASN1_STRING_GET0_DATA
	addrdata = ASN1_STRING_get0_data(addr_entry);
#else
	addrdata = ASN1_STRING_data(addr_entry);
#endif
	len = ASN1_STRING_length(addr_entry);

	return pq_verify_peer_name_matches_certificate_ip(conn, addrdata, len, store_name);
}

static bool
is_ip_address(const char *host)
{
	struct in_addr dummy4;
#ifdef HAVE_INET_PTON
	struct in6_addr dummy6;
#endif

	return inet_aton(host, &dummy4)
#ifdef HAVE_INET_PTON
		|| (inet_pton(AF_INET6, host, &dummy6) == 1)
#endif
		;
}

/*
 *	验证服务器证书是否与我们连接的主机名匹配。
 *
 * 会考虑证书的 Common Name 和 Subject Alternative Names。
                      */
int
pgtls_verify_peer_name_matches_certificate_guts(PGconn *conn,
												int *names_examined,
												char **first_name)
{
	STACK_OF(GENERAL_NAME) * peer_san;
	int			i;
	int			rc = 0;
	char	   *host = conn->connhost[conn->whichhost].host;
	int			host_type;
	bool		check_cn = true;

	Assert(host && host[0]);	/* 应由调用者保证          */

	/*
	 * 我们尝试匹配 NSS 的行为，这稍微偏离了规范，         
	 * 但似乎更符合直觉：                     
	 *
	 * 如果 connhost 含 DNS 名称，证书 SAN 有 dNSName 条目，      
	 * 则我们将完全忽略主题公用名（Subject Common Name）； 
	 * 否则，我们回退到检查 CN。（此行为与 RFC 一致。）  
	 *      
	 *
	 * 如果 connhost 含 IP 地址，且 SAN 有 iPAddress 条目，   
	 * 也会忽略 CN。否则允许 CN 匹配，即使 SAN 中存在      
	 * 有 dNSName 也无妨。（RFC 6125 禁止这样做："如果       
	 * 所提供的标识符中包含 DNS-ID、SRV-ID、URI-ID、             
	 * 或客户端支持的任何应用程序专用                 
	 * 标识符类型时，绝不能为 CN-ID 寻求匹配。")       
	 *
	 * 注意：以前版本的 libpq 不考虑 iPAddress 条目，       
	 * 所以这一新行为可能会破坏主题 CN 与 SANs 中 IP 地址  
	 * 不同的证书。                          
	 */
	if (is_ip_address(host))
		host_type = GEN_IPADD;
	else
		host_type = GEN_DNS;

	/*
	 * 首先，从证书中获取 Subject Alternative Names (SANs)，
	 * 并将其与原始给定的主机名进行比较。
	           */
	peer_san = (STACK_OF(GENERAL_NAME) *)
		X509_get_ext_d2i(conn->peer, NID_subject_alt_name, NULL, NULL);

	if (peer_san)
	{
		int			san_len = sk_GENERAL_NAME_num(peer_san);

		for (i = 0; i < san_len; i++)
		{
			const GENERAL_NAME *name = sk_GENERAL_NAME_value(peer_san, i);
			char	   *alt_name = NULL;

			if (name->type == host_type)
			{
				/*
				 * 此 SAN 与我们的主机名类型相同（IP 或 DNS），
				 * 因此不允许回退检查 CN。
				        */
				check_cn = false;
			}

			if (name->type == GEN_DNS)
			{
				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(conn,
																	   name->d.dNSName,
																	   &alt_name);
			}
			else if (name->type == GEN_IPADD)
			{
				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_ip(conn,
																	 name->d.iPAddress,
																	 &alt_name);
			}

			if (alt_name)
			{
				if (!*first_name)
					*first_name = alt_name;
				else
					free(alt_name);
			}

			if (rc != 0)
			{
				/*
				 * 要么遇到错误，要么得到匹配，此时都不     
				 * 应回退到 CN。      
				 */
				check_cn = false;
				break;
			}
		}
		sk_GENERAL_NAME_pop_free(peer_san, GENERAL_NAME_free);
	}

	/*
	 * 如果没有匹配类型的 subjectAltName 扩展，则检查公用名 
	 * （CN）。 
	 *
	 * （根据 RFC 2818 和 RFC 6125，如果存在 dNSName 类型的   
	 * subjectAltName 扩展，则必须忽略 CN；但如果 host 是       
	 * IP 地址，破坏规则（见上）。  
	 */
	if (check_cn)
	{
		X509_NAME  *subject_name;

		subject_name = X509_get_subject_name(conn->peer);
		if (subject_name != NULL)
		{
			int			cn_index;

			cn_index = X509_NAME_get_index_by_NID(subject_name,
												  NID_commonName, -1);
			if (cn_index >= 0)
			{
				char	   *common_name = NULL;

				(*names_examined)++;
				rc = openssl_verify_peer_name_matches_certificate_name(conn,
																	   X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject_name, cn_index)),
																	   &common_name);

				if (common_name)
				{
					if (!*first_name)
						*first_name = common_name;
					else
						free(common_name);
				}
			}
		}
	}

	return rc;
}

#if defined(ENABLE_THREAD_SAFETY) && defined(HAVE_CRYPTO_LOCK)
/*
 *	用于 OpenSSL 内部锁定的回调函数。（OpenSSL 1.1.0
 *	自行进行锁定，不再需要这些回调函数。
 *	CRYPTO_lock() 函数在 1.1.0 中被移除，此时回调
 *	已过时，因此我们假设如果 CRYPTO_lock() 存在，
 *	则仍然需要回调。）
                       */

static unsigned long
pq_threadidcallback(void)
{
	/*
	 * 这不符合标准。pthread_self() 返回 pthread_t，不应强制    
	 * 转换为 unsigned long，但 CRYPTO_set_id_callback 要求如此，   
	 * 所以只能如此。   
	 */
	return (unsigned long) pthread_self();
}

static pthread_mutex_t *pq_lockarray;

static void
pq_lockingcallback(int mode, int n, const char *file, int line)
{
	/*
	 * 没有办法报告互斥量原语失败，因此我们只在
	 * 开发构建中 Assert，否则忽略任何错误。幸运的是，
	 * 这在现代 OpenSSL 中都已过时。
	           */
	if (mode & CRYPTO_LOCK)
	{
		if (pthread_mutex_lock(&pq_lockarray[n]))
			Assert(false);
	}
	else
	{
		if (pthread_mutex_unlock(&pq_lockarray[n]))
			Assert(false);
	}
}
#endif							/* ENABLE_THREAD_SAFETY && HAVE_CRYPTO_LOCK */

/*
 * 初始化 SSL 库。
 *
 * 在线程安全模式下，这包括设置 libcrypto 回调函数
 * 来进行线程锁定。
 *
 * 如果调用者已通过 PQinitOpenSSL 告诉我们他正在管理
 * libcrypto，我们期望回调已设置，不会尝试
 * 覆盖它。
                                */
int
pgtls_init(PGconn *conn, bool do_ssl, bool do_crypto)
{
#ifdef ENABLE_THREAD_SAFETY
	if (pthread_mutex_lock(&ssl_config_mutex))
		return -1;

#ifdef HAVE_CRYPTO_LOCK
	if (pq_init_crypto_lib)
	{
		/*
		 * 必要时，设置一个数组保存 libcrypto 的锁     
		 * libcrypto 会告诉我们该数组的大小。    
		 */
		if (pq_lockarray == NULL)
		{
			int			i;

			pq_lockarray = malloc(sizeof(pthread_mutex_t) * CRYPTO_num_locks());
			if (!pq_lockarray)
			{
				pthread_mutex_unlock(&ssl_config_mutex);
				return -1;
			}
			for (i = 0; i < CRYPTO_num_locks(); i++)
			{
				if (pthread_mutex_init(&pq_lockarray[i], NULL))
				{
					free(pq_lockarray);
					pq_lockarray = NULL;
					pthread_mutex_unlock(&ssl_config_mutex);
					return -1;
				}
			}
		}

		if (do_crypto && !conn->crypto_loaded)
		{
			if (crypto_open_connections++ == 0)
			{
				/*
				 * 仅线程化 libcrypto 应用需要这些     
				 * 但请确保：如果它们已被设置，则不要  
				 * 覆盖它们。     
				 */
				if (CRYPTO_get_id_callback() == NULL)
					CRYPTO_set_id_callback(pq_threadidcallback);
				if (CRYPTO_get_locking_callback() == NULL)
					CRYPTO_set_locking_callback(pq_lockingcallback);
			}

			conn->crypto_loaded = true;
		}
	}
#endif							/* HAVE_CRYPTO_LOCK */
#endif							/* ENABLE_THREAD_SAFETY */

	if (!ssl_lib_initialized && do_ssl)
	{
		if (pq_init_ssl_lib)
		{
#ifdef HAVE_OPENSSL_INIT_SSL
			OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
			OPENSSL_config(NULL);
			SSL_library_init();
			SSL_load_error_strings();
#endif
		}
		ssl_lib_initialized = true;
	}

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif
	return 0;
}

/*
 *	需要此函数是因为：libpq 库从应用程序卸载后， 
 *	当系统其他部分使用 libcrypto 时，回调函数将不复     
 *	存在。为此，我们在最后一个 libpq 连接关闭时    
 *	注销回调函数。                                   
 *	（如果拥有 OpenSSL 回调，                                 
 *	亦然。）   
 *
 *	回调仅在线程安全模式编译时才会被设置，        
 *	所以只需在这种情况下移除它们。在 OpenSSL          
 *	1.1.0 中不再需要。   
 */
static void
destroy_ssl_system(void)
{
#if defined(ENABLE_THREAD_SAFETY) && defined(HAVE_CRYPTO_LOCK)
	if (pthread_mutex_lock(&ssl_config_mutex))
		return;

	if (pq_init_crypto_lib && crypto_open_connections > 0)
		--crypto_open_connections;

	if (pq_init_crypto_lib && crypto_open_connections == 0)
	{
		/*
		 * 已无剩余连接。若期间无人注册其他回调，     
		 * 则注销 libcrypto 回调。             
		 */
		if (CRYPTO_get_locking_callback() == pq_lockingcallback)
			CRYPTO_set_locking_callback(NULL);
		if (CRYPTO_get_id_callback() == pq_threadidcallback)
			CRYPTO_set_id_callback(NULL);

		/*
		 * 我们不会释放锁数组。若此进程中又有连接进来，
		 * 我们将让它们与现有互斥量一起重用。         
		 *
		 * 这意味着在反复加载/卸载时会泄漏少量             
		 * 内存  
		 */
	}

	pthread_mutex_unlock(&ssl_config_mutex);
#endif
}

/*
 *	创建每连接的 SSL 对象，并加载客户端证书、       
 *	私钥及受信 CA 证书。      
 *
 *	成功返回 0，失败返回 -1（conn->errorMessage 中有消息） 
 */
static int
initialize_SSL(PGconn *conn)
{
	SSL_CTX    *SSL_context;
	struct stat buf;
	char		homedir[MAXPGPATH];
	char		fnbuf[MAXPGPATH];
	char		sebuf[PG_STRERROR_R_BUFLEN];
	bool		have_homedir;
	bool		have_cert;
	bool		have_rootcert;

	/*
	 * 如果任何相关参数使用默认值，我们将需要主目录。
	 * 如果 pqGetHomeDirectory 失败，则表现得好像找不到
	 * 任何文件一样。
	   */
	if (!(conn->sslcert && strlen(conn->sslcert) > 0) ||
		!(conn->sslkey && strlen(conn->sslkey) > 0) ||
		!(conn->sslrootcert && strlen(conn->sslrootcert) > 0) ||
		!((conn->sslcrl && strlen(conn->sslcrl) > 0) ||
		  (conn->sslcrldir && strlen(conn->sslcrldir) > 0)))
		have_homedir = pqGetHomeDirectory(homedir, sizeof(homedir));
	else						/* 不需要它  */
		have_homedir = false;

	/*
	 * 创建新 SSL_CTX 对象。 
	 *
	 * 我们曾在所有连接间共享单个 SSL_CTX，但当连接使用 
	 * 不同证书时会很复杂。因此现在我们为每个连接  
	 * 创建单独的上下文，并接受这一开销。                    
	 */
	SSL_context = SSL_CTX_new(SSLv23_method());
	if (!SSL_context)
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "could not create SSL context: %s", err);
		SSLerrfree(err);
		return -1;
	}

	/*
	 * 若定义了 libpq 包装回调，则将客户端证书口令         
	 * 提示交给它   
	 *
	 * 若应用程序未安装自己的回调且 sslpassword         
	 * 参数非空，现在就安装我们的回调，以确保向     
	 * OpenSSL 提供 PGconn->sslpassword，而非让它提示 stdin。     
	 *
	 * 这将替换 OpenSSL 默认的 PEM_def_callback（stdin 提示），  
	 * 但我们只为该 SSL 上下文设置它，因此是无      
	 * 害的   
	 */
	if (PQsslKeyPassHook
		|| (conn->sslpassword && strlen(conn->sslpassword) > 0))
	{
		SSL_CTX_set_default_passwd_cb(SSL_context, PQssl_passwd_cb);
		SSL_CTX_set_default_passwd_cb_userdata(SSL_context, conn);
	}

#ifdef HAVE_SSL_CTX_SET_CERT_CB
	/* 设置证书选择回调。              */
	SSL_CTX_set_cert_cb(SSL_context, cert_cb, conn);
#endif

	/* 禁用旧的协议版本      */
	SSL_CTX_set_options(SSL_context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

	/* 如有必要，设置最小和最大协议版本           */
	if (conn->ssl_min_protocol_version &&
		strlen(conn->ssl_min_protocol_version) != 0)
	{
		int			ssl_min_ver;

		ssl_min_ver = ssl_protocol_version_to_openssl(conn->ssl_min_protocol_version);

		if (ssl_min_ver == -1)
		{
			libpq_append_conn_error(conn, "invalid value \"%s\" for minimum SSL protocol version",
									conn->ssl_min_protocol_version);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if (!SSL_CTX_set_min_proto_version(SSL_context, ssl_min_ver))
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not set minimum SSL protocol version: %s", err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
	}

	if (conn->ssl_max_protocol_version &&
		strlen(conn->ssl_max_protocol_version) != 0)
	{
		int			ssl_max_ver;

		ssl_max_ver = ssl_protocol_version_to_openssl(conn->ssl_max_protocol_version);

		if (ssl_max_ver == -1)
		{
			libpq_append_conn_error(conn, "invalid value \"%s\" for maximum SSL protocol version",
									conn->ssl_max_protocol_version);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if (!SSL_CTX_set_max_proto_version(SSL_context, ssl_max_ver))
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not set maximum SSL protocol version: %s", err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
	}

	/*
	 * 禁用 OpenSSL 的移动写缓冲区健全性检查，因为它会   
	 * 在非阻塞发送时会不必要地失败。  
	 */
	SSL_CTX_set_mode(SSL_context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	/*
	 * 如果根证书文件存在，加载它以执行证书
	 * 验证。如果 sslmode 是 "verify-full"，我们还将在
	 * 连接完成后进行进一步验证。
	                                  */
	if (conn->sslrootcert && strlen(conn->sslrootcert) > 0)
		strlcpy(fnbuf, conn->sslrootcert, sizeof(fnbuf));
	else if (have_homedir)
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CERT_FILE);
	else
		fnbuf[0] = '\0';

	if (strcmp(fnbuf, "system") == 0)
	{
		/*
		 * "system" 哨兵值表示我们应当加载任何为 OpenSSL       
		 * 安装的根证书；位置因平台而异。注意，默认系统 
		 * 位置还可能被 SSL_CERT_DIR 与 SSL_CERT_FILE                
		 * 环境变量                                            
		 * 进一步覆盖。    
		 */
		if (SSL_CTX_set_default_verify_paths(SSL_context) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not load system root certificate paths: %s",
									err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_rootcert = true;
	}
	else if (fnbuf[0] != '\0' &&
			 stat(fnbuf, &buf) == 0)
	{
		X509_STORE *cvstore;

		if (SSL_CTX_load_verify_locations(SSL_context, fnbuf, NULL) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not read root certificate file \"%s\": %s",
									fnbuf, err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		if ((cvstore = SSL_CTX_get_cert_store(SSL_context)) != NULL)
		{
			char	   *fname = NULL;
			char	   *dname = NULL;

			if (conn->sslcrl && strlen(conn->sslcrl) > 0)
				fname = conn->sslcrl;
			if (conn->sslcrldir && strlen(conn->sslcrldir) > 0)
				dname = conn->sslcrldir;

			/* 默认使用默认的 CRL 文件     */
			if (!fname && !dname && have_homedir)
			{
				snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, ROOT_CRL_FILE);
				fname = fnbuf;
			}

			/* 设置标志以检查完整的 CRL 链                */
			if ((fname || dname) &&
				X509_STORE_load_locations(cvstore, fname, dname) == 1)
			{
				X509_STORE_set_flags(cvstore,
									 X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
			}

			/* 如果未找到，静默忽略；我们不要求 CRL  */
			ERR_clear_error();
		}
		have_rootcert = true;
	}
	else
	{
		/*
		 * stat() 失败；假定根文件不存在。如果 sslmode
		 * 是 verify-ca 或 verify-full，这是错误。否则，
		 * 继续而不执行任何服务器证书验证。
		             */
		if (conn->sslmode[0] == 'v')	/* "verify-ca"或"verify-full"  */
		{
			/*
			 * 到达此处且文件名为空的唯一方式是
			 * pqGetHomeDirectory 失败。这是一个足够罕见的
			 * 情况，值得为其提供专门定制的错误消息。
			                    */
			if (fnbuf[0] == '\0')
				libpq_append_conn_error(conn, "could not get home directory to locate root certificate file\n"
										"Either provide the file, use the system's trusted roots with sslrootcert=system, or change sslmode to disable server certificate verification.");
			else
				libpq_append_conn_error(conn, "root certificate file \"%s\" does not exist\n"
										"Either provide the file, use the system's trusted roots with sslrootcert=system, or change sslmode to disable server certificate verification.", fnbuf);
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_rootcert = false;
	}

	/* 读取客户端证书文件      */
	if (conn->sslcert && strlen(conn->sslcert) > 0)
		strlcpy(fnbuf, conn->sslcert, sizeof(fnbuf));
	else if (have_homedir)
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_CERT_FILE);
	else
		fnbuf[0] = '\0';

	if (conn->sslcertmode[0] == 'd')	/* 禁用  */
	{
		/* 即使有客户端证书也不发送         */
		have_cert = false;
	}
	else if (fnbuf[0] == '\0')
	{
		/* 无主目录，不使用客户端证书继续    */
		have_cert = false;
	}
	else if (stat(fnbuf, &buf) != 0)
	{
		/*
		 * 若文件不存在，则不带客户端证书继续；服务器 
		 * 可能接受也可能不接受连接。任何其他错误  
		 * 都值得投诉。                
		 */
		if (errno != ENOENT && errno != ENOTDIR)
		{
			libpq_append_conn_error(conn, "could not open certificate file \"%s\": %s",
									fnbuf, strerror_r(errno, sebuf, sizeof(sebuf)));
			SSL_CTX_free(SSL_context);
			return -1;
		}
		have_cert = false;
	}
	else
	{
		/*
		 * 证书文件存在，因此加载它。由于 OpenSSL 不提供
		 * "SSL_use_certificate_chain_file" 的等价物，我们必须把它
		 * 加载到 SSL 上下文而非 SSL 对象中。   
		 */
		if (SSL_CTX_use_certificate_chain_file(SSL_context, fnbuf) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			libpq_append_conn_error(conn, "could not read certificate file \"%s\": %s",
									fnbuf, err);
			SSLerrfree(err);
			SSL_CTX_free(SSL_context);
			return -1;
		}

		/* 还需要加载关联的私钥               */
		have_cert = true;
	}

	/*
	 * SSL 上下文现已加载了正确的根证书和客户端
	 * 证书。创建连接特定的 SSL 对象。私钥直接
	 * 加载到 SSL 对象中。（我们也可以将私钥加载到
	 * 上下文中，但历史上我们一直这样做，实际上
	 * 没有区别。）
	                                            */
	if (!(conn->ssl = SSL_new(SSL_context)) ||
		!SSL_set_app_data(conn->ssl, conn) ||
		!my_SSL_set_fd(conn, conn->sock))
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "could not establish SSL connection: %s", err);
		SSLerrfree(err);
		SSL_CTX_free(SSL_context);
		return -1;
	}
	conn->ssl_in_use = true;

	/*
	 * SSL 上下文由 OpenSSL 进行引用计数。我们可以在
	 * 创建 SSL 对象后立即释放它，它会在实际需要时
	 * 继续存在。
	                           */
	SSL_CTX_free(SSL_context);
	SSL_context = NULL;

	/*
	 * 若连接参数启用，则设置服务器名称指示（SNI）。    
	 * 根据 RFC 6066，若 host 为字面 IP（IPv4 或 IPv6），        
	 * 则不设
	 */
	if (conn->sslsni && conn->sslsni[0] == '1')
	{
		const char *host = conn->connhost[conn->whichhost].host;

		if (host && host[0] &&
			!(strspn(host, "0123456789.") == strlen(host) ||
			  strchr(host, ':')))
		{
			if (SSL_set_tlsext_host_name(conn->ssl, host) != 1)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not set SSL Server Name Indication (SNI): %s", err);
				SSLerrfree(err);
				return -1;
			}
		}
	}

	/*
	 * 读取 SSL 密钥。如果指定了密钥，且存在冒号，
	 * 则将其视为 engine:key 组合 — 我们不支持文件名中
	 * 包含冒号。例外情况是第二个字符是冒号，
	 * 此时它可以是带驱动器规范的 Windows 文件名。
	                             */
	if (have_cert && conn->sslkey && strlen(conn->sslkey) > 0)
	{
#ifdef USE_SSL_ENGINE
		if (strchr(conn->sslkey, ':')
#ifdef WIN32
			&& conn->sslkey[1] != ':'
#endif
			)
		{
			/* 有冒号但不在第二字符处，视为 engine:key   */
			char	   *engine_str = strdup(conn->sslkey);
			char	   *engine_colon;
			EVP_PKEY   *pkey;

			if (engine_str == NULL)
			{
				libpq_append_conn_error(conn, "out of memory");
				return -1;
			}

			/* 不能返回 NULL，因为我们在 strdup 之前已检查  */
			engine_colon = strchr(engine_str, ':');

			*engine_colon = '\0';	/* engine_str 现在是引擎名  */
			engine_colon++;		/* engine_colon 已是密钥名  */

			conn->engine = ENGINE_by_id(engine_str);
			if (conn->engine == NULL)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not load SSL engine \"%s\": %s",
										engine_str, err);
				SSLerrfree(err);
				free(engine_str);
				return -1;
			}

			if (ENGINE_init(conn->engine) == 0)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not initialize SSL engine \"%s\": %s",
										engine_str, err);
				SSLerrfree(err);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}

			pkey = ENGINE_load_private_key(conn->engine, engine_colon,
										   NULL, NULL);
			if (pkey == NULL)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not read private SSL key \"%s\" from engine \"%s\": %s",
										engine_colon, engine_str, err);
				SSLerrfree(err);
				ENGINE_finish(conn->engine);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}
			if (SSL_use_PrivateKey(conn->ssl, pkey) != 1)
			{
				char	   *err = SSLerrmessage(ERR_get_error());

				libpq_append_conn_error(conn, "could not load private SSL key \"%s\" from engine \"%s\": %s",
										engine_colon, engine_str, err);
				SSLerrfree(err);
				ENGINE_finish(conn->engine);
				ENGINE_free(conn->engine);
				conn->engine = NULL;
				free(engine_str);
				return -1;
			}

			free(engine_str);

			fnbuf[0] = '\0';	/* 表明我们将不从文件加         
								 * 载  */
		}
		else
#endif							/* USE_SSL_ENGINE */
		{
			/* PGSSLKEY 不是引擎，将其视为文件名     */
			strlcpy(fnbuf, conn->sslkey, sizeof(fnbuf));
		}
	}
	else if (have_homedir)
	{
		/* 未指定 PGSSLKEY，加载默认文件  */
		snprintf(fnbuf, sizeof(fnbuf), "%s/%s", homedir, USER_KEY_FILE);
	}
	else
		fnbuf[0] = '\0';

	if (have_cert && fnbuf[0] != '\0')
	{
		/* 从文件读客户端密钥   */

		if (stat(fnbuf, &buf) != 0)
		{
			if (errno == ENOENT)
				libpq_append_conn_error(conn, "certificate present, but not private key file \"%s\"",
										fnbuf);
			else
				libpq_append_conn_error(conn, "could not stat private key file \"%s\": %m",
										fnbuf);
			return -1;
		}

		/* 密钥文件须是常规文件  */
		if (!S_ISREG(buf.st_mode))
		{
			libpq_append_conn_error(conn, "private key file \"%s\" is not a regular file",
									fnbuf);
			return -1;
		}

		/*
		 * 拒绝加载全局可读的密钥文件。我们接受 root 拥有、
		 * 模式为 0640 或更低的文件，以便在具有补充组成员资格
		 * 允许我们读取它们时可以访问系统范围的证书。
		 * 对于非 root 拥有的文件，要求模式为 0600 或更低。
		 * 我们不需要精确检查文件的所有权；如果尽管
		 * 权限如此严格我们仍然能够读取它，那么它必然
		 * 具有正确的所有权。
		 *
		 * 注意：收紧这些规则时要非常小心。有些人
		 * 期望，例如，以 root 运行的客户端进程应该能够
		 * 使用非 root 拥有的密钥文件。
		 *
		 * 注意，在 src/backend/libpq/be-secure-common.c 中
		 * 执行了大致类似的检查，因此此处的任何更改可能
		 * 也需要在那里进行。然而，此代码处理当前用户
		 * == root 的情况，而那个代码不处理。
		 *
		 * 理想情况下，我们也会在 Windows 上执行类似的权限
		 * 检查，但不清楚这如何实现，因为 Unix 风格的权限
		 * 可能不可用。
		             */
#if !defined(WIN32) && !defined(__CYGWIN__)
		if (buf.st_uid == 0 ?
			buf.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO) :
			buf.st_mode & (S_IRWXG | S_IRWXO))
		{
			libpq_append_conn_error(conn,
									"private key file \"%s\" has group or world access; file must have permissions u=rw (0600) or less if owned by the current user, or permissions u=rw,g=r (0640) or less if owned by root",
									fnbuf);
			return -1;
		}
#endif

		if (SSL_use_PrivateKey_file(conn->ssl, fnbuf, SSL_FILETYPE_PEM) != 1)
		{
			char	   *err = SSLerrmessage(ERR_get_error());

			/*
			 * 我们将尝试以 DER（ASN.1 二进制）格式加载，若  
			 * 也失败了，就报告原始错误。这可能掩盖    
			 * DER 格式证书本身有问题的情形，但想做到     
			 * 比这更聪明，就得重复 openssl 的格式检测。  
			 * 我们不能探测开头的 -----BEGIN，因为 PEM 可能     
			 * 带有不匹配前导行和空白。OpenSSL 未公开其   
			 * get_name(...)，且其 PEM 例程对失败模式的区分    
			 * 得不够细致，以致我们无法分辨"非 PEM，试 DER"
			 * 与"密码错误"两种失败情形                      
			 * 的区别。     
			 */
			if (SSL_use_PrivateKey_file(conn->ssl, fnbuf, SSL_FILETYPE_ASN1) != 1)
			{
				libpq_append_conn_error(conn, "could not load private key file \"%s\": %s",
										fnbuf, err);
				SSLerrfree(err);
				return -1;
			}

			SSLerrfree(err);
		}
	}

	/* 验证证书和密钥是否匹配        */
	if (have_cert &&
		SSL_check_private_key(conn->ssl) != 1)
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "certificate does not match private key file \"%s\": %s",
								fnbuf, err);
		SSLerrfree(err);
		return -1;
	}

	/*
	 * 如果加载了根证书，也设置我们的证书验证
	 * 回调。
	        */
	if (have_rootcert)
		SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, verify_cb);

	/*
	 * 如有必要，设置压缩选项。
	 */
	if (conn->sslcompression && conn->sslcompression[0] == '0')
		SSL_set_options(conn->ssl, SSL_OP_NO_COMPRESSION);
	else
		SSL_clear_options(conn->ssl, SSL_OP_NO_COMPRESSION);

	return 0;
}

/*
 *	尝试协商 SSL 连接。
           */
static PostgresPollingStatusType
open_client_SSL(PGconn *conn)
{
	int			r;

	SOCK_ERRNO_SET(0);
	ERR_clear_error();
	r = SSL_connect(conn->ssl);
	if (r <= 0)
	{
		int			save_errno = SOCK_ERRNO;
		int			err = SSL_get_error(conn->ssl, r);
		unsigned long ecode;

		ecode = ERR_get_error();
		switch (err)
		{
			case SSL_ERROR_WANT_READ:
				return PGRES_POLLING_READING;

			case SSL_ERROR_WANT_WRITE:
				return PGRES_POLLING_WRITING;

			case SSL_ERROR_SYSCALL:
				{
					char		sebuf[PG_STRERROR_R_BUFLEN];
					unsigned long vcode;

					vcode = SSL_get_verify_result(conn->ssl);

					/*
					 * 如果此处收到 X509 错误，表明加载
					 * 本地颁发者证书失败，而套接字层
					 * 没有错误，则意味着验证失败是由于
					 * 缺少系统 CA 池，而非协议错误。我们
					 * 检查 sslrootcert 设置以确保用户
					 * 使用的是系统 CA 池。对于其他错误，
					 * 使用正常的 SYSCALL 日志记录。
					                                       */
					if (save_errno == 0 &&
						vcode == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY &&
						strcmp(conn->sslrootcert, "system") == 0)
						libpq_append_conn_error(conn, "SSL error: certificate verify failed: %s",
												X509_verify_cert_error_string(vcode));
					else if (r == -1 && save_errno != 0)
						libpq_append_conn_error(conn, "SSL SYSCALL error: %s",
												SOCK_STRERROR(save_errno, sebuf, sizeof(sebuf)));
					else
						libpq_append_conn_error(conn, "SSL SYSCALL error: EOF detected");
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}
			case SSL_ERROR_SSL:
				{
					char	   *err = SSLerrmessage(ecode);

					libpq_append_conn_error(conn, "SSL error: %s", err);
					SSLerrfree(err);
					switch (ERR_GET_REASON(ecode))
					{
							/*
							 * UNSUPPORTED_PROTOCOL、WRONG_VERSION_NUMBER    
							 * 和 TLSV1_ALERT_PROTOCOL_VERSION 曾           
							 * 在与旧 OpenSSL 通信时，或双方协议 
							 * 范围不相交时出现。                   
							 * NO_PROTOCOLS_AVAILABLE   
							 * 见于本地配置有误时（如果      
							 * openssl.cnf 注入了我方             
							 * 未考虑的限制）。至于 OpenSSL      
							 * 返回所列其他码，尚不清楚，     
							 * 但协议版本提示                         
							 * 似乎对所有这些码都                 
							 * 是合适的。                              
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
							libpq_append_conn_error(conn, "This may indicate that the server does not support any SSL protocol version between %s and %s.",
													conn->ssl_min_protocol_version ?
													conn->ssl_min_protocol_version :
													MIN_OPENSSL_TLS_VERSION,
													conn->ssl_max_protocol_version ?
													conn->ssl_max_protocol_version :
													MAX_OPENSSL_TLS_VERSION);
							break;
						default:
							break;
					}
					pgtls_close(conn);
					return PGRES_POLLING_FAILED;
				}

			default:
				libpq_append_conn_error(conn, "unrecognized SSL error code: %d", err);
				pgtls_close(conn);
				return PGRES_POLLING_FAILED;
		}
	}

	/*
	 * 如果 root.crt 存在，服务器证书已在 initialize_SSL()    
	 * 中经 SSL_CTX_set_verify() 检查。    
	 */

	/* 获取服务器证书  */
	conn->peer = SSL_get_peer_certificate(conn->ssl);
	if (conn->peer == NULL)
	{
		char	   *err = SSLerrmessage(ERR_get_error());

		libpq_append_conn_error(conn, "certificate could not be obtained: %s", err);
		SSLerrfree(err);
		pgtls_close(conn);
		return PGRES_POLLING_FAILED;
	}

	if (!pq_verify_peer_name_matches_certificate(conn))
	{
		pgtls_close(conn);
		return PGRES_POLLING_FAILED;
	}

	/* SSL 握手已完成       */
	return PGRES_POLLING_OK;
}

void
pgtls_close(PGconn *conn)
{
	bool		destroy_needed = false;

	if (conn->ssl_in_use)
	{
		if (conn->ssl)
		{
			/*
			 * 此处不能销毁所有 SSL 相关物，因为后续  
			 * 可能调用需要我们线程回调的 OpenSSL 例程，  
			 * 所以这里设置一个标志，最后再作检查。    
			 */

			SSL_shutdown(conn->ssl);
			SSL_free(conn->ssl);
			conn->ssl = NULL;
			conn->ssl_in_use = false;

			destroy_needed = true;
		}

		if (conn->peer)
		{
			X509_free(conn->peer);
			conn->peer = NULL;
		}

#ifdef USE_SSL_ENGINE
		if (conn->engine)
		{
			ENGINE_finish(conn->engine);
			ENGINE_free(conn->engine);
			conn->engine = NULL;
		}
#endif
	}
	else
	{
		/*
		 * 在非 SSL 情况下，如果连接已加载，
		 * 则只需移除加密回调。此代码路径
		 * 不依赖任何挂起的 SSL 调用。
		                 */
		if (conn->crypto_loaded)
			destroy_needed = true;
	}

	/*
	 * 如果这是使用 libcrypto 的最后一个连接，
	 * 这将移除我们的加密锁定挂钩，这意味着我们必须等到
	 * 所有潜在的 SSL 调用都已完成后才能调用它，
	 * 否则可能会导致竞争条件和可能的死锁。
	 *
	 * 请参见 destroy_ssl_system() 上方的注释。
	 */
	if (destroy_needed)
	{
		destroy_ssl_system();
		conn->crypto_loaded = false;
	}
}


/*
 * 为传递的 SSL 错误码获取原因字符串
 *
 * 调用者使用 ERR_get_error() 获取要传递到此处
 * 的错误码。结果在使用后必须使用 SSLerrfree 释放。
 *
 * 此处需要一些谨慎，因为 ERR_reason_error_string 会
 * 在不识别错误码时返回 NULL，或者（在 OpenSSL >= 3 中）
 * 如果代码表示系统 errno 值。我们永远不想返回 NULL。
    */
static char ssl_nomem[] = "out of memory allocating error description";

#define SSL_ERR_LEN 128

static char *
SSLerrmessage(unsigned long ecode)
{
	const char *errreason;
	char	   *errbuf;

	errbuf = malloc(SSL_ERR_LEN);
	if (!errbuf)
		return ssl_nomem;
	if (ecode == 0)
	{
		snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("no SSL error reported"));
		return errbuf;
	}
	errreason = ERR_reason_error_string(ecode);
	if (errreason != NULL)
	{
		strlcpy(errbuf, errreason, SSL_ERR_LEN);
		return errbuf;
	}

	/*
	 * 在 OpenSSL 3.0.0 及更高版本中，ERR_reason_error_string
	 * 不再映射系统 errno 值。（参见 OpenSSL 源代码中的解释。）
	 * 我们可以通过此段代码弥补这个缺陷。较旧的 OpenSSL
	 * 版本没有 ERR_SYSTEM_ERROR 宏，但没关系，因为它们
	 * 也没有这个缺陷。
	                         */
#ifdef ERR_SYSTEM_ERROR
	if (ERR_SYSTEM_ERROR(ecode))
	{
		strerror_r(ERR_GET_REASON(ecode), errbuf, SSL_ERR_LEN);
		return errbuf;
	}
#endif

	/* 别无他法，只能报告数字 ecode   */
	snprintf(errbuf, SSL_ERR_LEN, libpq_gettext("SSL error code %lu"), ecode);
	return errbuf;
}

static void
SSLerrfree(char *buf)
{
	if (buf != ssl_nomem)
		free(buf);
}

/* ------------------------------------------------------------ */
/*					SSL 信息函数         					*/
/* ------------------------------------------------------------ */

/*
 *	返回 OpenSSL 对象的指针。
 */
void *
PQgetssl(PGconn *conn)
{
	if (!conn)
		return NULL;
	return conn->ssl;
}

void *
PQsslStruct(PGconn *conn, const char *struct_name)
{
	if (!conn)
		return NULL;
	if (strcmp(struct_name, "OpenSSL") == 0)
		return conn->ssl;
	return NULL;
}

const char *const *
PQsslAttributeNames(PGconn *conn)
{
	static const char *const openssl_attrs[] = {
		"library",
		"key_bits",
		"cipher",
		"compression",
		"protocol",
		NULL
	};
	static const char *const empty_attrs[] = {NULL};

	if (!conn)
	{
		/* 返回默认 SSL 库的属性            */
		return openssl_attrs;
	}

	/* 未加密连接没有属性         */
	if (conn->ssl == NULL)
		return empty_attrs;

	return openssl_attrs;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
	if (!conn)
	{
		/* PQsslAttribute(NULL, "library") 报告默认 SSL 库            */
		if (strcmp(attribute_name, "library") == 0)
			return "OpenSSL";
		return NULL;
	}

	/* 对于非加密连接，所有属性读取为 NULL         */
	if (conn->ssl == NULL)
		return NULL;

	if (strcmp(attribute_name, "library") == 0)
		return "OpenSSL";

	if (strcmp(attribute_name, "key_bits") == 0)
	{
		static char sslbits_str[12];
		int			sslbits;

		SSL_get_cipher_bits(conn->ssl, &sslbits);
		snprintf(sslbits_str, sizeof(sslbits_str), "%d", sslbits);
		return sslbits_str;
	}

	if (strcmp(attribute_name, "cipher") == 0)
		return SSL_get_cipher(conn->ssl);

	if (strcmp(attribute_name, "compression") == 0)
		return SSL_get_current_compression(conn->ssl) ? "on" : "off";

	if (strcmp(attribute_name, "protocol") == 0)
		return SSL_get_version(conn->ssl);

	return NULL;				/* 未知属性      */
}

/*
 * 私有替代 BIO：改用 pqsecure_raw_write() 和               
 * pqsecure_raw_read() 来进行发送和接收，以便这些函数   
 * 能够在 I/O 错误时禁用 SIGPIPE 并给出更好的错误消息。   
 *
 * 这些函数紧密仿照 OpenSSL 中的标准套接字 BIO；参见        
 * OpenSSL 的 crypto/bio/bss_sock.c 中的 sock_read() 和            
 * sock_write()。XXX OpenSSL 1.0.1e 将比 EINTR 多得多的               
 * 错误码也视为重试原因；要采用吗？   
 */

/* 受 ssl_config_mutex 保护   */
static BIO_METHOD *my_bio_methods;

static int
my_sock_read(BIO *h, char *buf, int size)
{
	int			res;

	res = pqsecure_raw_read((PGconn *) BIO_get_app_data(h), buf, size);
	BIO_clear_retry_flags(h);
	if (res < 0)
	{
		/* 如果被中断，告诉调用者重试      */
		switch (SOCK_ERRNO)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				BIO_set_retry_read(h);
				break;

			default:
				break;
		}
	}

	return res;
}

static int
my_sock_write(BIO *h, const char *buf, int size)
{
	int			res;

	res = pqsecure_raw_write((PGconn *) BIO_get_app_data(h), buf, size);
	BIO_clear_retry_flags(h);
	if (res < 0)
	{
		/* 如果被中断，告诉调用者重试      */
		switch (SOCK_ERRNO)
		{
#ifdef EAGAIN
			case EAGAIN:
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			case EWOULDBLOCK:
#endif
			case EINTR:
				BIO_set_retry_write(h);
				break;

			default:
				break;
		}
	}

	return res;
}

static BIO_METHOD *
my_BIO_s_socket(void)
{
	BIO_METHOD *res;

#ifdef ENABLE_THREAD_SAFETY
	if (pthread_mutex_lock(&ssl_config_mutex))
		return NULL;
#endif

	res = my_bio_methods;

	if (!my_bio_methods)
	{
		BIO_METHOD *biom = (BIO_METHOD *) BIO_s_socket();
#ifdef HAVE_BIO_METH_NEW
		int			my_bio_index;

		my_bio_index = BIO_get_new_index();
		if (my_bio_index == -1)
			goto err;
		my_bio_index |= (BIO_TYPE_DESCRIPTOR | BIO_TYPE_SOURCE_SINK);
		res = BIO_meth_new(my_bio_index, "libpq socket");
		if (!res)
			goto err;

		/*
		 * 截至撰写本文时，这些函数从不失败。但还是像  
		 * OpenSSL 示例那样检查。  
		 */
		if (!BIO_meth_set_write(res, my_sock_write) ||
			!BIO_meth_set_read(res, my_sock_read) ||
			!BIO_meth_set_gets(res, BIO_meth_get_gets(biom)) ||
			!BIO_meth_set_puts(res, BIO_meth_get_puts(biom)) ||
			!BIO_meth_set_ctrl(res, BIO_meth_get_ctrl(biom)) ||
			!BIO_meth_set_create(res, BIO_meth_get_create(biom)) ||
			!BIO_meth_set_destroy(res, BIO_meth_get_destroy(biom)) ||
			!BIO_meth_set_callback_ctrl(res, BIO_meth_get_callback_ctrl(biom)))
		{
			goto err;
		}
#else
		res = malloc(sizeof(BIO_METHOD));
		if (!res)
			goto err;
		memcpy(res, biom, sizeof(BIO_METHOD));
		res->bread = my_sock_read;
		res->bwrite = my_sock_write;
#endif
	}

	my_bio_methods = res;

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif

	return res;

err:
#ifdef HAVE_BIO_METH_NEW
	if (res)
		BIO_meth_free(res);
#else
	if (res)
		free(res);
#endif

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&ssl_config_mutex);
#endif
	return NULL;
}

/* 这应与 OpenSSL 的 SSL_set_fd 完全匹配，只是使用我的 BIO */
static int
my_SSL_set_fd(PGconn *conn, int fd)
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
	BIO_set_app_data(bio, conn);

	SSL_set_bio(conn->ssl, bio, bio);
	BIO_set_fd(bio, fd, BIO_NOCLOSE);
	ret = 1;
err:
	return ret;
}

/*
 * 这是从 conn->sslpassword 返回客户端证书密码的
 * 默认处理程序。应用程序可以显式安装它，如果它们
 * 想阻止 OpenSSL 在 stdin 上提示。
         */
int
PQdefaultSSLKeyPassHook_OpenSSL(char *buf, int size, PGconn *conn)
{
	if (conn && conn->sslpassword)
	{
		if (strlen(conn->sslpassword) + 1 > size)
			fprintf(stderr, libpq_gettext("WARNING: sslpassword truncated\n"));
		strncpy(buf, conn->sslpassword, size);
		buf[size - 1] = '\0';
		return strlen(buf);
	}
	else
	{
		buf[0] = '\0';
		return 0;
	}
}

PQsslKeyPassHook_OpenSSL_type
PQgetSSLKeyPassHook_OpenSSL(void)
{
	return PQsslKeyPassHook;
}

void
PQsetSSLKeyPassHook_OpenSSL(PQsslKeyPassHook_OpenSSL_type hook)
{
	PQsslKeyPassHook = hook;
}

/*
 * 提供密码以解密客户端证书。
 *
 * 这必须匹配 OpenSSL 类型 pem_password_cb。
        */
static int
PQssl_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
	PGconn	   *conn = userdata;

	if (PQsslKeyPassHook)
		return PQsslKeyPassHook(buf, size, conn);
	else
		return PQdefaultSSLKeyPassHook_OpenSSL(buf, size, conn);
}

/*
 * 将 TLS 协议版本字符串转换为 OpenSSL 值
 *
 * 如果传递的版本不受当前 OpenSSL 版本支持，
 * 则返回 -1。如果返回非负值，后续代码可以假定
 * 它正在使用受支持的版本。
 *
 * 注意：这与 be-secure-openssl.c 中的后端例程相当相似，
 * 因此如果更改此例程，请确保同时更新两者。
                                                 */
static int
ssl_protocol_version_to_openssl(const char *protocol)
{
	if (pg_strcasecmp("TLSv1", protocol) == 0)
		return TLS1_VERSION;

#ifdef TLS1_1_VERSION
	if (pg_strcasecmp("TLSv1.1", protocol) == 0)
		return TLS1_1_VERSION;
#endif

#ifdef TLS1_2_VERSION
	if (pg_strcasecmp("TLSv1.2", protocol) == 0)
		return TLS1_2_VERSION;
#endif

#ifdef TLS1_3_VERSION
	if (pg_strcasecmp("TLSv1.3", protocol) == 0)
		return TLS1_3_VERSION;
#endif

	return -1;
}
