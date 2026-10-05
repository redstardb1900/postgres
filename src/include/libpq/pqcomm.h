/*-------------------------------------------------------------------------
 *
 * pqcomm.h
 *		前端与后端共用的定义。
 *
 * 注意：由于历史原因，本文件并不对应 pqcomm.c。
 * pqcomm.c 中的例程在 libpq.h 里声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqcomm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQCOMM_H
#define PQCOMM_H

#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>

typedef struct
{
	struct sockaddr_storage addr;
	socklen_t	salen;
} SockAddr;

typedef struct
{
	int			family;
	SockAddr	addr;
} AddrInfo;

/* 为众所周知的端口配置 UNIX 套接字位置。 */

#define UNIXSOCK_PATH(path, port, sockdir) \
	   (AssertMacro(sockdir), \
		AssertMacro(*(sockdir) != '\0'), \
		snprintf(path, sizeof(path), "%s/.s.PGSQL.%d", \
				 (sockdir), (port)))

/*
 * 套接字路径可用的最大长度是能放进 struct sockaddr_un
 * 的长度。这通常只有 100 来个字节 :-(。
 *
 * 为了一致，总是向 UNIXSOCK_PATH() 传入 MAXPGPATH 大小
 * 的缓冲区，然后若结果字符串 >= UNIXSOCK_PATH_BUFLEN
 * 字节就报错。（因为 getaddrinfo 的标准 API 在套接字
 * 路径名太长时无法以有用的方式报错，我们必须显式测试，
 * 而不是只让该子程序返回一个错误。）
 */
#define UNIXSOCK_PATH_BUFLEN sizeof(((struct sockaddr_un *) NULL)->sun_path)

/*
 * 看起来像绝对路径、或以 @ 开头的主机，被解释为
 * Unix 域套接字地址。
 */
static inline bool
is_unixsock_path(const char *path)
{
	return is_absolute_path(path) || path[0] == '@';
}

/*
 * 这些宏操作前端/后端协议版本号。
 *
 * 不兼容的变更应增加主版本号。兼容的变更（例如增加
 * 功能）应增加次版本号。
 *
 * 若后端支持协议版本 m.n，它实际上必须支持
 * m.[0..n]。经过一段 "合理" 的时间后，可以去掉对
 * 版本 m-1 的后端支持。
 *
 * 前端除当前版本外不要求支持任何其他版本。
 *
 *
 */

#define PG_PROTOCOL_MAJOR(v)	((v) >> 16)
#define PG_PROTOCOL_MINOR(v)	((v) & 0x0000ffff)
#define PG_PROTOCOL(m,n)	(((m) << 16) | (n))

/*
 * 所支持的最早和最晚的前端/后端协议版本。
 * （目前只支持协议版本 3）
 */

#define PG_PROTOCOL_EARLIEST	PG_PROTOCOL(3,0)
#define PG_PROTOCOL_LATEST		PG_PROTOCOL(3,0)

typedef uint32 ProtocolVersion; /* 前端/后端协议版本号 */

typedef ProtocolVersion MsgType;


/*
 * 包长度是网络字节序的 4 字节。
 *
 * 下面出现的包布局省略了开头的长度。
 */

typedef uint32 PacketLen;

extern PGDLLIMPORT bool Db_user_namespace;

/*
 * 在协议 3.0 及以后，启动包长度不固定，但我们仍然
 * 对它设置一个任意上限。这只是为了防止通过发送足够
 * 多的数据把服务器耗到内存不足这种简单的拒绝服务攻击。
 *
 */
#define MAX_STARTUP_PACKET_LENGTH 10000


/* 这些是后端发送的认证请求码。 */

#define AUTH_REQ_OK			0	/* 用户已通过认证 */
#define AUTH_REQ_KRB4		1	/* Kerberos V4。已不再支持。 */
#define AUTH_REQ_KRB5		2	/* Kerberos V5。已不再支持。 */
#define AUTH_REQ_PASSWORD	3	/* 口令 */
#define AUTH_REQ_CRYPT		4	/* crypt 口令。已不再支持。 */
#define AUTH_REQ_MD5		5	/* md5 口令 */
/* 6 可用。它曾用于 SCM 凭证，现已不再支持。 */
#define AUTH_REQ_GSS		7	/* 不带 wrap() 的 GSSAPI */
#define AUTH_REQ_GSS_CONT	8	/* 继续 GSS 交换 */
#define AUTH_REQ_SSPI		9	/* 不带 wrap() 的 SSPI 协商 */
#define AUTH_REQ_SASL	   10	/* 开始 SASL 认证 */
#define AUTH_REQ_SASL_CONT 11	/* 继续 SASL 认证 */
#define AUTH_REQ_SASL_FIN  12	/* 最终 SASL 消息 */
#define AUTH_REQ_MAX	   AUTH_REQ_SASL_FIN	/* 最大的 AUTH_REQ_* 值 */

typedef uint32 AuthRequest;


/*
 * 客户端也可以向 postmaster 发送取消当前操作的请求。
 * 这比直接发给客户端的后端更难看，但避免了依赖
 * 带外通信设施。
 *
 * 取消请求码不得与我们可能使用的任何协议版本号
 * 匹配。这个随机选择应该可以。
 */
#define CANCEL_REQUEST_CODE PG_PROTOCOL(1234,5678)

typedef struct CancelRequestPacket
{
	/* 注意每个字段都以网络字节序存储！ */
	MsgType		cancelRequestCode;	/* 标识取消请求的代码 */
	uint32		backendPID;		/* 客户端后端的 PID */
	uint32		cancelAuthCode; /* 授权取消的秘密键 */
} CancelRequestPacket;


/*
 * 客户端也可以一开始就发送 SSL 或 GSSAPI 协商请求，
 * 以获得安全信道。
 */
#define NEGOTIATE_SSL_CODE PG_PROTOCOL(1234,5679)
#define NEGOTIATE_GSS_CODE PG_PROTOCOL(1234,5680)

#endif							/* PQCOMM_H */
