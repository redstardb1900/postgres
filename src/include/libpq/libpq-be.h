/*-------------------------------------------------------------------------
 *
 * libpq-be.h
 *	  This file contains definitions for structures and externs used
 *	  by the postmaster during client authentication.
 *
 *	  Note that this is backend-internal and is NOT exported to clients.
 *	  Structs that need to be client-visible are in pqcomm.h.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/libpq-be.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#include <sys/time.h>
#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#include <netinet/tcp.h>

#ifdef ENABLE_GSS
#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#else
#include <gssapi/gssapi.h>
#endif							/* HAVE_GSSAPI_H */
#endif							/* ENABLE_GSS */

#ifdef ENABLE_SSPI
#define SECURITY_WIN32
#if defined(WIN32) && !defined(_MSC_VER)
#include <ntsecapi.h>
#endif
#include <security.h>
#undef SECURITY_WIN32

#ifndef ENABLE_GSS
/*
 * 定义一个与 Unix 上 GSSAPI 兼容的假结构。
 */
typedef struct
{
	void	   *value;
	int			length;
} gss_buffer_desc;
#endif
#endif							/* ENABLE_SSPI */

#include "datatype/timestamp.h"
#include "libpq/hba.h"
#include "libpq/pqcomm.h"


typedef enum CAC_state
{
	CAC_OK,
	CAC_STARTUP,
	CAC_SHUTDOWN,
	CAC_RECOVERY,
	CAC_NOTCONSISTENT,
	CAC_TOOMANY
} CAC_state;


/*
 * GSSAPI 专用的状态信息
 */
#if defined(ENABLE_GSS) | defined(ENABLE_SSPI)
typedef struct
{
	gss_buffer_desc outbuf;		/* GSSAPI 输出 token 缓冲区 */
#ifdef ENABLE_GSS
	gss_cred_id_t cred;			/* GSSAPI 连接凭证 */
	gss_ctx_id_t ctx;			/* GSSAPI 连接上下文 */
	gss_name_t	name;			/* GSSAPI 客户端名 */
	char	   *princ;			/* 用于认证的 GSSAPI Principal；若未使用
								 * GSSAPI 认证则为 NULL */
	bool		auth;			/* 使用了 GSSAPI 认证 */
	bool		enc;			/* 正在使用 GSSAPI 加密 */
	bool		delegated_creds;	/* GSSAPI 委托的凭证 */
#endif
} pg_gssinfo;
#endif

/*
 * ClientConnectionInfo 包含描述客户端连接的字段，这些字段
 * 会复制给并行工作进程，因为 Port 里没有任何东西会那样做。
 * 这里的分配规则与 Port 相同（一切都必须 malloc，或在
 * TopMemoryContext 中 palloc）。
 *
 * 若在此增加结构体成员，记得同时处理
 * SerializeClientConnectionInfo() 及其相关函数中的序列化。
 */
typedef struct ClientConnectionInfo
{
	/*
 * 已认证的身份。这个标识符的含义取决于 auth_method；它是
 * 用户在认证过程中、被赋予数据库角色之前所出示的身份
 * （若有的话）。（它实际上是 pg_ident 用户映射的
 * "SYSTEM-USERNAME"，不过实际使用的字符串可能不同，
 * 取决于 pg_hba 选项。）
 *
 * 若用户实际上并未被认证，例如使用 "trust" 认证方法，
 * 则 authn_id 为 NULL。
 *
 */
	const char *authn_id;

	/*
 * 决定上述 authn_id 的 HBA 方法。仅当 authn_id 不为
 * NULL 时才有意义；否则它是未定义的。
 */
	UserAuth	auth_method;
} ClientConnectionInfo;

/*
 * postmaster 用它与前端通信。它包含运行后端之前这次通信
 * 所需的全部状态。Port 结构放在 malloc 的内存中，后端
 * 运行时仍可用（见 MyProcPort）。它指向的数据也必须用
 * malloc 分配，或在 TopMemoryContext 里 palloc，以便
 * 存活到 PostgresMain 执行阶段！
 *
 * 连接建立期间若对客户端 IP 的反向查找成功，则设置
 * remote_hostname。remote_hostname_resolv 跟踪主机名
 * 验证状态：+1 = 已知解析到客户端 IP；-1 = 已知不会
 * 解析到该 IP；0 = 尚未做正向 DNS 查找；-2 = 解析出错。
 * 反向查找失败时 remote_hostname 保持 NULL，同时
 * remote_hostname_resolv 为 -2。反向成功但正向失败时
 * 也为 -2（可区分，因为 remote_hostname 非 NULL）。
 * 这两种 -2 情况下，remote_hostname_errcode 保存返回码，
 * 供稍后配合 gai_strerror 使用。
 *
 *
 *
 *
 *
 */

typedef struct Port
{
	pgsocket	sock;			/* 文件描述符 */
	bool		noblock;		/* 套接字是否处于非阻塞模式？ */
	ProtocolVersion proto;		/* 前端/后端协议版本 */
	SockAddr	laddr;			/* 本地地址（postmaster） */
	SockAddr	raddr;			/* 远程地址（客户端） */
	char	   *remote_host;	/* 远程主机的名字（或 IP 地址） */
	char	   *remote_hostname;	/* 远程主机名（不是 IP 地址），
									 * 若有的话 */
	int			remote_hostname_resolv; /* 见上文 */
	int			remote_hostname_errcode;	/* 见上文 */
	char	   *remote_port;	/* 远程端口的文本表示 */
	CAC_state	canAcceptConnections;	/* postmaster 连接状态 */

	/*
 * 需要从启动包保存并传入后端执行的信息。"char *"
 * 字段未设置时为 NULL。guc_options 指向一个 List，
 * 其中选项名和值交替存放。
 */
	char	   *database_name;
	char	   *user_name;
	char	   *cmdline_options;
	List	   *guc_options;

	/*
 * 启动包中的应用名，这里只用于 "connection authorized"
 * 日志消息。启动之后不应使用它，而应使用 GUC，因为
 * 应用之后可以更改它。
 */
	char	   *application_name;

	/*
 * 认证过程期间需要保存的信息。
 */
	HbaLine    *hba;

	/*
 * TCP 保活和用户超时设置。
 *
 * 默认值在 AF_UNIX 或尚未得知时为 0；当前值在 AF_UNIX
 * 或使用默认值时为 0。另外，默认值为 -1 表示我们无法
 * 查出默认值（getsockopt 失败）。
 */
	int			default_keepalives_idle;
	int			default_keepalives_interval;
	int			default_keepalives_count;
	int			default_tcp_user_timeout;
	int			keepalives_idle;
	int			keepalives_interval;
	int			keepalives_count;
	int			tcp_user_timeout;

	/*
 * GSSAPI 结构。
 */
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)

	/*
 * 若本连接支持并使用了 GSSAPI，则存放 GSSAPI 信息。
 * 即使没有编译进 GSSAPI，也存放一个 NULL 指针，以保持
 * 结构体偏移相同（为了扩展 ABI 兼容）。
 */
	pg_gssinfo *gss;
#else
	void	   *gss;
#endif

	/*
 * SSL 结构。
 */
	bool		ssl_in_use;
	char	   *peer_cn;
	char	   *peer_dn;
	bool		peer_cert_valid;

	/*
 * OpenSSL 结构。（把它们放在最后，这样无论是否启用 SSL
 * 构建，其他字段的位置都相同。）
 */
#ifdef USE_OPENSSL
	SSL		   *ssl;
	X509	   *peer;
#endif
} Port;

#ifdef USE_SSL
/*
 * 临时 DH 密钥交换使用的硬编码 DH 参数。（关于 EDH 的
 * 更多细节见 README.SSL。）
 *
 * 这是 RFC 3526 中的 2048 位 DH 参数。素数的生成在
 * RFC 2412 附录 E 中规定，那里也讨论了生成元的设计
 * 选择。注意，用 OpenSSL 加载时，这会使 DH_check() 因
 * DH_NOT_SUITABLE_GENERATOR 失败，此时宁可泄漏一个位。
 *
 */
#define FILE_DH2048 \
"-----BEGIN DH PARAMETERS-----\n\
MIIBCAKCAQEA///////////JD9qiIWjCNMTGYouA3BzRKQJOCIpnzHQCC76mOxOb\n\
IlFKCHmONATd75UZs806QxswKwpt8l8UN0/hNW1tUcJF5IW1dmJefsb0TELppjft\n\
awv/XLb0Brft7jhr+1qJn6WunyQRfEsf5kkoZlHs5Fs9wgB8uKFjvwWY2kg2HFXT\n\
mmkWP6j9JM9fg2VdI9yjrZYcYvNWIIVSu57VKQdwlpZtZww1Tkq8mATxdGwIyhgh\n\
fDKQXkYuNs474553LBgOhgObJ4Oi7Aeij7XFXfBvTFLJ3ivL9pVYFxg5lUl86pVq\n\
5RXSJhiY+gUQFXKOWoqsqmj//////////wIBAg==\n\
-----END DH PARAMETERS-----\n"

/*
 * 这些函数由各 SSL 实现专用的粘合代码实现
 * （例如 be-secure-openssl.c）。
 */

/*
 * 初始化全局 SSL 上下文。
 *
 * 若 isServerStart 为真，把任何错误报告为 FATAL（这样
 * 我们就不会返回）。否则以 LOG 级别记录错误并返回 -1
 * 表示有麻烦，若已有旧 SSL 状态则保留它。成功则返回 0。
 */
extern int	be_tls_init(bool isServerStart);

/*
 * 销毁全局 SSL 上下文，若有的话。
 */
extern void be_tls_destroy(void);

/*
 * 尝试协商 SSL 连接。
 */
extern int	be_tls_open_server(Port *port);

/*
 * 关闭 SSL 连接。
 */
extern void be_tls_close(Port *port);

/*
 * 从安全连接读取数据。
 */
extern ssize_t be_tls_read(Port *port, void *ptr, size_t len, int *waitfor);

/*
 * 向安全连接写入数据。
 */
extern ssize_t be_tls_write(Port *port, void *ptr, size_t len, int *waitfor);

/*
 * 返回关于 SSL 连接的信息。
 */
extern int	be_tls_get_cipher_bits(Port *port);
extern const char *be_tls_get_version(Port *port);
extern const char *be_tls_get_cipher(Port *port);
extern void be_tls_get_peer_subject_name(Port *port, char *ptr, size_t len);
extern void be_tls_get_peer_issuer_name(Port *port, char *ptr, size_t len);
extern void be_tls_get_peer_serial(Port *port, char *ptr, size_t len);

/*
 * 取得 SCRAM 信道绑定类型 tls-server-end-point 所用的
 * 服务器证书哈希。
 *
 * 结果是服务器证书经 palloc 分配的哈希及其大小；若没有
 * 可用证书则为 NULL。
 *
 * 缺少 X509_get_signature_nid() 函数的旧版 OpenSSL
 * 不支持这一点。
 */
#if defined(USE_OPENSSL) && (defined(HAVE_X509_GET_SIGNATURE_NID) || defined(HAVE_X509_GET_SIGNATURE_INFO))
#define HAVE_BE_TLS_GET_CERTIFICATE_HASH
extern char *be_tls_get_certificate_hash(Port *port, size_t *len);
#endif

/* SSL 的初始化钩子；默认实现会在适当时设置口令回调 */
#ifdef USE_OPENSSL
typedef void (*openssl_tls_init_hook_typ) (SSL_CTX *context, bool isServerStart);
extern PGDLLIMPORT openssl_tls_init_hook_typ openssl_tls_init_hook;
#endif

#endif							/* USE_SSL */

#ifdef ENABLE_GSS
/*
 * 返回关于 GSSAPI 已认证连接的信息
 */
extern bool be_gssapi_get_auth(Port *port);
extern bool be_gssapi_get_enc(Port *port);
extern const char *be_gssapi_get_princ(Port *port);
extern bool be_gssapi_get_delegation(Port *port);

/* 对 GSSAPI 加密的连接进行读写。 */
extern ssize_t be_gssapi_read(Port *port, void *ptr, size_t len);
extern ssize_t be_gssapi_write(Port *port, void *ptr, size_t len);
#endif							/* ENABLE_GSS */

extern PGDLLIMPORT ProtocolVersion FrontendProtocol;
extern PGDLLIMPORT ClientConnectionInfo MyClientConnectionInfo;

/* TCP 保活配置。在 AF_UNIX 套接字上这些是空操作。 */

extern int	pq_getkeepalivesidle(Port *port);
extern int	pq_getkeepalivesinterval(Port *port);
extern int	pq_getkeepalivescount(Port *port);
extern int	pq_gettcpusertimeout(Port *port);

extern int	pq_setkeepalivesidle(int idle, Port *port);
extern int	pq_setkeepalivesinterval(int interval, Port *port);
extern int	pq_setkeepalivescount(int count, Port *port);
extern int	pq_settcpusertimeout(int timeout, Port *port);

#endif							/* LIBPQ_BE_H */
