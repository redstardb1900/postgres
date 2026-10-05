/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  处理网络认证的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/auth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pwd.h>
#include <unistd.h>

#include "commands/user.h"
#include "common/ip.h"
#include "common/md5.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/sasl.h"
#include "libpq/scram.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */
static void auth_failed(Port *port, int status, const char *logdetail);
static char *recv_password_packet(Port *port);
static void set_authn_id(Port *port, const char *id);


/*----------------------------------------------------------------
 * Password-based authentication methods (password, md5, and scram-sha-256)
 *----------------------------------------------------------------
 */
static int	CheckPasswordAuth(Port *port, const char **logdetail);
static int	CheckPWChallengeAuth(Port *port, const char **logdetail);

static int	CheckMD5Auth(Port *port, char *shadow_pass,
						 const char **logdetail);


/*----------------------------------------------------------------
 * Ident authentication
 *----------------------------------------------------------------
 */
/* Ident 服务器能返回的用户名最大长度（见 RFC 1413） */
#define IDENT_USERNAME_MAX 512

/* Ident 服务的标准 TCP 端口号。由 IANA 分配 */
#define IDENT_PORT 113

static int	ident_inet(hbaPort *port);


/*----------------------------------------------------------------
 * Peer authentication
 *----------------------------------------------------------------
 */
static int	auth_peer(hbaPort *port);


/*----------------------------------------------------------------
 * PAM authentication
 *----------------------------------------------------------------
 */
#ifdef USE_PAM
#ifdef HAVE_PAM_PAM_APPL_H
#include <pam/pam_appl.h>
#endif
#ifdef HAVE_SECURITY_PAM_APPL_H
#include <security/pam_appl.h>
#endif

#define PGSQL_PAM_SERVICE "postgresql"	/* 传给 PAM 的服务名 */

/* 变通办法，弥补最初的 Solaris 在 conv_proc 签名中缺少 "const" */
#ifdef _PAM_LEGACY_NONCONST
#define PG_PAM_CONST
#else
#define PG_PAM_CONST const
#endif

static int	CheckPAMAuth(Port *port, const char *user, const char *password);
static int	pam_passwd_conv_proc(int num_msg,
								 PG_PAM_CONST struct pam_message **msg,
								 struct pam_response **resp, void *appdata_ptr);

static struct pam_conv pam_passw_conv = {
	&pam_passwd_conv_proc,
	NULL
};

static const char *pam_passwd = NULL;	/* 针对 Solaris 2.6 缺陷的
										 * 变通办法 */
static Port *pam_port_cludge;	/* 把 "Port *port" 传进
								 * pam_passwd_conv_proc 的变通办法 */
static bool pam_no_password;	/* 用于检测未给出口令 */
#endif							/* USE_PAM */


/*----------------------------------------------------------------
 * BSD authentication
 *----------------------------------------------------------------
 */
#ifdef USE_BSD_AUTH
#include <bsd_auth.h>

static int	CheckBSDAuth(Port *port, char *user);
#endif							/* USE_BSD_AUTH */


/*----------------------------------------------------------------
 * LDAP authentication
 *----------------------------------------------------------------
 */
#ifdef USE_LDAP
#ifndef WIN32
/* 我们使用已弃用的函数，以便代码路径与 win32 保持一致。 */
#define LDAP_DEPRECATED 1
#include <ldap.h>
#else
#include <winldap.h>

#endif

static int	CheckLDAPAuth(Port *port);

/* LDAP_OPT_DIAGNOSTIC_MESSAGE 是较新的拼写 */
#ifndef LDAP_OPT_DIAGNOSTIC_MESSAGE
#define LDAP_OPT_DIAGNOSTIC_MESSAGE LDAP_OPT_ERROR_STRING
#endif

/* 默认 LDAP 口令变换钩子，可被共享库覆盖 */
static char *dummy_ldap_password_mutator(char *input);
auth_password_hook_typ ldap_password_hook = dummy_ldap_password_mutator;

#endif							/* USE_LDAP */

/*----------------------------------------------------------------
 * Cert authentication
 *----------------------------------------------------------------
 */
#ifdef USE_SSL
static int	CheckCertAuth(Port *port);
#endif


/*----------------------------------------------------------------
 * Kerberos and GSSAPI GUCs
 *----------------------------------------------------------------
 */
char	   *pg_krb_server_keyfile;
bool		pg_krb_caseins_users;
bool		pg_gss_accept_delegation;


/*----------------------------------------------------------------
 * GSSAPI Authentication
 *----------------------------------------------------------------
 */
#ifdef ENABLE_GSS
#include "libpq/be-gssapi-common.h"

static int	pg_GSS_checkauth(Port *port);
static int	pg_GSS_recvauth(Port *port);
#endif							/* ENABLE_GSS */


/*----------------------------------------------------------------
 * SSPI Authentication
 *----------------------------------------------------------------
 */
#ifdef ENABLE_SSPI
typedef SECURITY_STATUS
			(WINAPI * QUERY_SECURITY_CONTEXT_TOKEN_FN) (PCtxtHandle, void **);
static int	pg_SSPI_recvauth(Port *port);
static int	pg_SSPI_make_upn(char *accountname,
							 size_t accountnamesize,
							 char *domainname,
							 size_t domainnamesize,
							 bool update_accountname);
#endif

/*----------------------------------------------------------------
 * RADIUS Authentication
 *----------------------------------------------------------------
 */
static int	CheckRADIUSAuth(Port *port);
static int	PerformRadiusTransaction(const char *server, const char *secret, const char *portstr, const char *identifier, const char *user_name, const char *passwd);


/*
 * 接受的 GSS 和 SSPI 认证 token 的最大长度。
 * 我们也把它用作普通口令报文长度的上限。
 *
 * Kerberos 票据通常很小，但 Windows
 * 域控制器签发的 TGTs 含有一个授权字段，称为权限
 * 属性证书 (PAC)，其中有该用户的 Windows 权限
 * （组成员身份等）。PAC 会被复制进基于此 TGT 取得的
 * 所有票据（即使由该 Windows 领域所信任的 Unix 领域
 * 签发），并且可以有数 kB 之大。Windows 系统接受的最大 token
 * 长度由 Windows 注册表设置 MaxAuthToken 决定。微软建议
 * 不要把它设得高于
 * 65535 字节，因此这对我们来说也像是一个合理上限。
 */
#define PG_MAX_AUTH_TOKEN_LENGTH	65535

/*----------------------------------------------------------------
 * Global authentication functions
 *----------------------------------------------------------------
 */

/*
 * 这个钩子让插件在客户端认证之后、
 * 用户得知结果之前取得控制。它可以用来
 * 记录登录事件、在认证失败后插入延迟，等等。
 */
ClientAuthentication_hook_type ClientAuthentication_hook = NULL;

/*
 * 告诉用户认证失败了，但不要说明多少原因。
 *
 * 这里要在安全顾虑和给正当用户
 * 造成不必要的困难之间权衡。例如，我们不会
 * 想报告我们期望收到的口令……
 * 但报告正在使用的用户名和授权方法
 * 似乎有用，而且必须假定攻击者反正已经
 * 知道这些。
 * 注意，许多失败还会在 postmaster 日志里报告更多信息，
 * 我们希望只有正当的人才能读到它。特别是，
 * 若 logdetail 不是 NULL，我们就把该字符串写进日志。
 */
static void
auth_failed(Port *port, int status, const char *logdetail)
{
	const char *errstr;
	char	   *cdetail;
	int			errcode_return = ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION;

	/*
	 * 若因客户端 EOF 而失败，就直接退出；没有必要
	 * 再给客户端发消息，也没有多大必要把这次失败
	 * 记进 postmaster 日志。（若不是因为：当 libpq 被索取口令
	 * 却没有口令可发时，它会粗鲁地关闭连接，
	 * 记下这次失败也许是可取的。在口令认证下，
	 * 每次 psql 连接都会得到一条无用日志，
	 * 即使完全成功，只要我们记录 STATUS_EOF
	 * 事件。）
	 */
	if (status == STATUS_EOF)
		proc_exit(0);

	switch (port->hba->auth_method)
	{
		case uaReject:
		case uaImplicitReject:
			errstr = gettext_noop("authentication failed for user \"%s\": host rejected");
			break;
		case uaTrust:
			errstr = gettext_noop("\"trust\" authentication failed for user \"%s\"");
			break;
		case uaIdent:
			errstr = gettext_noop("Ident authentication failed for user \"%s\"");
			break;
		case uaPeer:
			errstr = gettext_noop("Peer authentication failed for user \"%s\"");
			break;
		case uaPassword:
		case uaMD5:
		case uaSCRAM:
			errstr = gettext_noop("password authentication failed for user \"%s\"");
			/* 我们用它表示 .pgpass 口令是否失败。 */
			errcode_return = ERRCODE_INVALID_PASSWORD;
			break;
		case uaGSS:
			errstr = gettext_noop("GSSAPI authentication failed for user \"%s\"");
			break;
		case uaSSPI:
			errstr = gettext_noop("SSPI authentication failed for user \"%s\"");
			break;
		case uaPAM:
			errstr = gettext_noop("PAM authentication failed for user \"%s\"");
			break;
		case uaBSD:
			errstr = gettext_noop("BSD authentication failed for user \"%s\"");
			break;
		case uaLDAP:
			errstr = gettext_noop("LDAP authentication failed for user \"%s\"");
			break;
		case uaCert:
			errstr = gettext_noop("certificate authentication failed for user \"%s\"");
			break;
		case uaRADIUS:
			errstr = gettext_noop("RADIUS authentication failed for user \"%s\"");
			break;
		default:
			errstr = gettext_noop("authentication failed for user \"%s\": invalid authentication method");
			break;
	}

	cdetail = psprintf(_("Connection matched file \"%s\" line %d: \"%s\""),
					   port->hba->sourcefile, port->hba->linenumber,
					   port->hba->rawline);
	if (logdetail)
		logdetail = psprintf("%s\n%s", logdetail, cdetail);
	else
		logdetail = cdetail;

	ereport(FATAL,
			(errcode(errcode_return),
			 errmsg(errstr, port->user_name),
			 logdetail ? errdetail_log("%s", logdetail) : 0));

	/* 不会返回 */
}


/*
 * 设置当前用户的已认证身份。所给字符串
 * 会存入 MyClientConnectionInfo，旁边是当前使用的 HBA
 * 方法。若启用了 log_connections，就会记录该身份。
 *
 * 认证方法应当恰好调用本例程一次，就在用户
 * 成功认证之后，即使它们有理由知道
 * 授权稍后会失败。
 *
 * 所给字符串会被复制进 TopMemoryContext，以匹配
 * MyClientConnectionInfo 的生命周期，因此传入由外部库
 * 管理的字符串是安全的。
 */
static void
set_authn_id(Port *port, const char *id)
{
	Assert(id);

	if (MyClientConnectionInfo.authn_id)
	{
		/*
		 * 已有的 authn_id 绝不应该被覆盖；那意味着两个
		 * 认证提供方在互相冲突（或一个在跟自己冲突）。
		 * 不要向客户端泄露任何认证细节，但也不要让
		 * 连接继续。
		 */
		ereport(FATAL,
				(errmsg("authentication identifier set more than once"),
				 errdetail_log("previous identifier: \"%s\"; new identifier: \"%s\"",
							   MyClientConnectionInfo.authn_id, id)));
	}

	MyClientConnectionInfo.authn_id = MemoryContextStrdup(TopMemoryContext, id);
	MyClientConnectionInfo.auth_method = port->hba->auth_method;

	if (Log_connections)
	{
		ereport(LOG,
				errmsg("connection authenticated: identity=\"%s\" method=%s "
					   "(%s:%d)",
					   MyClientConnectionInfo.authn_id,
					   hba_authname(MyClientConnectionInfo.auth_method),
					   port->hba->sourcefile, port->hba->linenumber));
	}
}


/*
 * 客户端认证从这里开始。若出错，本函数
 * 不返回，后端进程会被终止。
 */
void
ClientAuthentication(Port *port)
{
	int			status = STATUS_ERROR;
	const char *logdetail = NULL;

	/*
	 * 取得用于此前端/数据库组合的认证方法。
	 * 注意：此时我们不解析文件；这已经在
	 * 别处完成。若解析 hba 配置文件失败，hba.c 已经把
	 * 错误消息写进了服务器日志。
	 */
	hba_getauthmethod(port);

	CHECK_FOR_INTERRUPTS();

	/*
	 * 这是我们第一次能访问当前连接的 hba 记录的地方，
	 * 因此在这里、在认证之前，执行任何基于 hba
	 * 选项字段、应当事先完成的校验。
	 */
	if (port->hba->clientcert != clientCertOff)
	{
		/* 若尚未加载根证书库，则失败 */
		if (!secure_loaded_verify_locations())
			ereport(FATAL,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("client certificates can only be checked if a root certificate store is available")));

		/*
		 * 若我们加载了根证书库，并且客户端提供了
		 * 证书，那么它已经对照我们的根证书库
		 * 校验过，若校验未通过，连接早已被
		 * 中止。
		 */
		if (!port->peer_cert_valid)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("connection requires a valid client certificate")));
	}

	/*
	 * 现在进行实际的认证检查
	 */
	switch (port->hba->auth_method)
	{
		case uaReject:

			/*
			 * pg_hba.conf 中一条显式的 "reject" 记录。这份报告暴露了
			 * 存在一条显式拒绝记录，从安全角度
			 * 看也许不太理想；但隐式拒绝的
			 * 消息在真实情况是匹配了显式拒绝时，
			 * 会让 DBA 非常困惑。而我们
			 * 不想改变隐式拒绝的消息。如下所述，
			 * 这里显示的额外信息并没有暴露
			 * 攻击者所不知道的任何东西。
			 */
			{
				char		hostinfo[NI_MAXHOST];
				const char *encryption_state;

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

				encryption_state =
#ifdef ENABLE_GSS
					(port->gss && port->gss->enc) ? _("GSS encryption") :
#endif
#ifdef USE_SSL
					port->ssl_in_use ? _("SSL encryption") :
#endif
					_("no encryption");

				if (am_walsender && !am_db_walsender)
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("pg_hba.conf rejects replication connection for host \"%s\", user \"%s\", %s",
									hostinfo, port->user_name,
									encryption_state)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("pg_hba.conf rejects connection for host \"%s\", user \"%s\", database \"%s\", %s",
									hostinfo, port->user_name,
									port->database_name,
									encryption_state)));
				break;
			}

		case uaImplicitReject:

			/*
			 * 没有匹配项，因此告诉用户我们落到了末尾。
			 *
			 * 注意：这里额外报告的信息不是安全漏洞，
			 * 因为这些信息前端都知道，并且必须
			 * 假定攻击者也知道。我们只是在帮助那些
			 * 不太熟悉情况的正当用户。
			 */
			{
				char		hostinfo[NI_MAXHOST];
				const char *encryption_state;

				pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								   hostinfo, sizeof(hostinfo),
								   NULL, 0,
								   NI_NUMERICHOST);

				encryption_state =
#ifdef ENABLE_GSS
					(port->gss && port->gss->enc) ? _("GSS encryption") :
#endif
#ifdef USE_SSL
					port->ssl_in_use ? _("SSL encryption") :
#endif
					_("no encryption");

#define HOSTNAME_LOOKUP_DETAIL(port) \
				(port->remote_hostname ? \
				 (port->remote_hostname_resolv == +1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup matches.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == 0 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup not checked.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -1 ? \
				  errdetail_log("Client IP address resolved to \"%s\", forward lookup does not match.", \
								port->remote_hostname) : \
				  port->remote_hostname_resolv == -2 ? \
				  errdetail_log("Could not translate client host name \"%s\" to IP address: %s.", \
								port->remote_hostname, \
								gai_strerror(port->remote_hostname_errcode)) : \
				  0) \
				 : (port->remote_hostname_resolv == -2 ? \
					errdetail_log("Could not resolve client IP address to a host name: %s.", \
								  gai_strerror(port->remote_hostname_errcode)) : \
					0))

				if (am_walsender && !am_db_walsender)
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("no pg_hba.conf entry for replication connection from host \"%s\", user \"%s\", %s",
									hostinfo, port->user_name,
									encryption_state),
							 HOSTNAME_LOOKUP_DETAIL(port)));
				else
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					/* translator: last %s describes encryption state */
							 errmsg("no pg_hba.conf entry for host \"%s\", user \"%s\", database \"%s\", %s",
									hostinfo, port->user_name,
									port->database_name,
									encryption_state),
							 HOSTNAME_LOOKUP_DETAIL(port)));
				break;
			}

		case uaGSS:
#ifdef ENABLE_GSS
			/* 我们也许已经有 gss 工作区，也许还没有 */
			if (port->gss == NULL)
				port->gss = (pg_gssinfo *)
					MemoryContextAllocZero(TopMemoryContext,
										   sizeof(pg_gssinfo));
			port->gss->auth = true;

			/*
			 * 若在启用加密时已经设置了 GSS 状态，我们只需
			 * 检查客户端的主体。否则就去索取它。
			 */
			if (port->gss->enc)
				status = pg_GSS_checkauth(port);
			else
			{
				sendAuthRequest(port, AUTH_REQ_GSS, NULL, 0);
				status = pg_GSS_recvauth(port);
			}
#else
			Assert(false);
#endif
			break;

		case uaSSPI:
#ifdef ENABLE_SSPI
			if (port->gss == NULL)
				port->gss = (pg_gssinfo *)
					MemoryContextAllocZero(TopMemoryContext,
										   sizeof(pg_gssinfo));
			sendAuthRequest(port, AUTH_REQ_SSPI, NULL, 0);
			status = pg_SSPI_recvauth(port);
#else
			Assert(false);
#endif
			break;

		case uaPeer:
			status = auth_peer(port);
			break;

		case uaIdent:
			status = ident_inet(port);
			break;

		case uaMD5:
		case uaSCRAM:
			status = CheckPWChallengeAuth(port, &logdetail);
			break;

		case uaPassword:
			status = CheckPasswordAuth(port, &logdetail);
			break;

		case uaPAM:
#ifdef USE_PAM
			status = CheckPAMAuth(port, port->user_name, "");
#else
			Assert(false);
#endif							/* USE_PAM */
			break;

		case uaBSD:
#ifdef USE_BSD_AUTH
			status = CheckBSDAuth(port, port->user_name);
#else
			Assert(false);
#endif							/* USE_BSD_AUTH */
			break;

		case uaLDAP:
#ifdef USE_LDAP
			status = CheckLDAPAuth(port);
#else
			Assert(false);
#endif
			break;
		case uaRADIUS:
			status = CheckRADIUSAuth(port);
			break;
		case uaCert:
			/* uaCert 将被视为 clientcert=verify-full（uaTrust） */
		case uaTrust:
			status = STATUS_OK;
			break;
	}

	if ((status == STATUS_OK && port->hba->clientcert == clientCertFull)
		|| port->hba->auth_method == uaCert)
	{
		/*
		 * 确保只有在使用 cert 方法或 verify-full 选项时
		 * 才检查证书。
		 */
#ifdef USE_SSL
		status = CheckCertAuth(port);
#else
		Assert(false);
#endif
	}

	if (ClientAuthentication_hook)
		(*ClientAuthentication_hook) (port, status);

	if (status == STATUS_OK)
		sendAuthRequest(port, AUTH_REQ_OK, NULL, 0);
	else
		auth_failed(port, status, logdetail);
}


/*
 * 向前端发送认证请求报文。
 */
void
sendAuthRequest(Port *port, AuthRequest areq, const char *extradata, int extralen)
{
	StringInfoData buf;

	CHECK_FOR_INTERRUPTS();

	pq_beginmessage(&buf, 'R');
	pq_sendint32(&buf, (int32) areq);
	if (extralen > 0)
		pq_sendbytes(&buf, extradata, extralen);

	pq_endmessage(&buf);

	/*
	 * 刷新消息以便客户端能看到它，但 AUTH_REQ_OK 和
	 * AUTH_REQ_SASL_FIN 除外，它们不必发送，直到我们准备好
	 * 处理查询。
	 */
	if (areq != AUTH_REQ_OK && areq != AUTH_REQ_SASL_FIN)
		pq_flush();

	CHECK_FOR_INTERRUPTS();
}

/*
 * 从前端收集口令响应报文。
 *
 * 若无法取得口令则返回 NULL，否则返回 palloc 出来的字符串。
 */
static char *
recv_password_packet(Port *port)
{
	StringInfoData buf;
	int			mtype;

	pq_startmsgread();

	/* 期望消息类型 'p' */
	mtype = pq_getbyte();
	if (mtype != 'p')
	{
		/*
		 * 若客户端直接断开、不提供口令，不要
		 * 记日志。按协议规范这是合法的，而且 psql
		 * 经常这样做，因此抱怨只会把日志弄乱。
		 */
		if (mtype != EOF)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected password response, got message type %d",
							mtype)));
		return NULL;			/* EOF 或错误的消息类型 */
	}

	initStringInfo(&buf);
	if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))	/* 接收口令 */
	{
		/* EOF：pq_getmessage 已经记了一条合适的消息 */
		pfree(buf.data);
		return NULL;
	}

	/*
	 * 做健全性检查：口令报文长度应与所含
	 * 字符串的长度一致。这里使用 strlen 是安全的，因为
	 * StringInfo 保证附加了一个 '\0'。
	 */
	if (strlen(buf.data) + 1 != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));

	/*
	 * 不要允许空口令。Libpq 把空口令当成
	 * 完全没有口令，甚至不会尝试认证。但其他
	 * 客户端可能会，因此允许它会造成混淆。
	 *
	 * 注意，这只捕捉客户端以明文发来的空口令。
	 * CREATE/ALTER USER 里还有一项检查，防止一开始就把
	 * 空字符串存成用户口令。
	 * 对 MD5 和 SCRAM 认证我们依赖那项检查，但仍需要
	 * 这里的这项检查，以防止空口令被用于
	 * 对照外部系统检查口令的认证方法，
	 * 例如 PAM、LDAP 和 RADIUS。
	 */
	if (buf.len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("empty password returned by client")));

	/* 出于安全，不要把口令回显到日志。 */
	elog(DEBUG5, "received password packet");

	/*
	 * 返回收到的字符串。注意我们不尝试对它做任何
	 * 字符集转换；既然还不知道客户端的
	 * 编码，这样做也没什么意义。
	 */
	return buf.data;
}


/*----------------------------------------------------------------
 * Password-based authentication mechanisms
 *----------------------------------------------------------------
 */

/*
 * 明文口令认证。
 */
static int
CheckPasswordAuth(Port *port, const char **logdetail)
{
	char	   *passwd;
	int			result;
	char	   *shadow_pass;

	sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* 客户端不会发送口令 */

	shadow_pass = get_role_password(port->user_name, logdetail);
	if (shadow_pass)
	{
		result = plain_crypt_verify(port->user_name, shadow_pass, passwd,
									logdetail);
	}
	else
		result = STATUS_ERROR;

	if (shadow_pass)
		pfree(shadow_pass);
	pfree(passwd);

	if (result == STATUS_OK)
		set_authn_id(port, port->user_name);

	return result;
}

/*
 * MD5 和 SCRAM 认证。
 */
static int
CheckPWChallengeAuth(Port *port, const char **logdetail)
{
	int			auth_result;
	char	   *shadow_pass;
	PasswordType pwtype;

	Assert(port->hba->auth_method == uaSCRAM ||
		   port->hba->auth_method == uaMD5);

	/* 先查找该用户的口令。 */
	shadow_pass = get_role_password(port->user_name, logdetail);

	/*
	 * 若用户不存在，或没有口令，或口令已过期，我们
	 * 仍走完认证的动作，以免向客户端泄露
	 * 用户并不存在。若允许 'md5'，我们就根据当前的
	 * password_encryption 设置，选择使用 'md5' 还是
	 * 'scram-sha-256' 认证。想法是，多数真正的用户
	 * 大概有那种类型的口令，若我们假装这个用户
	 * 也有那种类型的口令，就最能混进去。
	 */
	if (!shadow_pass)
		pwtype = Password_encryption;
	else
		pwtype = get_password_type(shadow_pass);

	/*
	 * 若允许 'md5' 认证，就根据用户口令的类型，
	 * 决定做 'md5' 还是 'scram-sha-256' 认证。若它是
	 * MD5 哈希，就必须做 MD5 认证；若它是
	 * SCRAM 密钥，就必须做 SCRAM 认证。
	 *
	 * 若不允许 MD5 认证，就始终使用 SCRAM。若用户
	 * 有的是 MD5 口令，用 SCRAM 机制调用 CheckSASLAuth()
	 * 会失败。
	 */
	if (port->hba->auth_method == uaMD5 && pwtype == PASSWORD_TYPE_MD5)
		auth_result = CheckMD5Auth(port, shadow_pass, logdetail);
	else
		auth_result = CheckSASLAuth(&pg_be_scram_mech, port, shadow_pass,
									logdetail);

	if (shadow_pass)
		pfree(shadow_pass);
	else
	{
		/*
		 * 若 get_role_password() 返回了错误，认证最好没有
		 * 成功。
		 */
		Assert(auth_result != STATUS_OK);
	}

	if (auth_result == STATUS_OK)
		set_authn_id(port, port->user_name);

	return auth_result;
}

static int
CheckMD5Auth(Port *port, char *shadow_pass, const char **logdetail)
{
	char		md5Salt[4];		/* 口令盐 */
	char	   *passwd;
	int			result;

	if (Db_user_namespace)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled")));

	/* 包含用于计算响应的盐 */
	if (!pg_strong_random(md5Salt, 4))
	{
		ereport(LOG,
				(errmsg("could not generate random MD5 salt")));
		return STATUS_ERROR;
	}

	sendAuthRequest(port, AUTH_REQ_MD5, md5Salt, 4);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* 客户端不会发送口令 */

	if (shadow_pass)
		result = md5_crypt_verify(port->user_name, shadow_pass, passwd,
								  md5Salt, 4, logdetail);
	else
		result = STATUS_ERROR;

	pfree(passwd);

	return result;
}


/*----------------------------------------------------------------
 * GSSAPI authentication system
 *----------------------------------------------------------------
 */
#ifdef ENABLE_GSS
static int
pg_GSS_recvauth(Port *port)
{
	OM_uint32	maj_stat,
				min_stat,
				lmin_s,
				gflags;
	int			mtype;
	StringInfoData buf;
	gss_buffer_desc gbuf;
	gss_cred_id_t delegated_creds;

	/*
	 * 若配置了 keytab，就使用它。既然我们现在需要 MIT
	 * Kerberos，将来也许可以考虑使用凭据存储扩展，
	 * 而不是环境变量。
	 */
	if (pg_krb_server_keyfile != NULL && pg_krb_server_keyfile[0] != '\0')
	{
		if (setenv("KRB5_KTNAME", pg_krb_server_keyfile, 1) != 0)
		{
			/* 唯一可能的失败原因是内存不足，因此使用那个错误码 */
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not set environment: %m")));
		}
	}

	/*
	 * 我们接受 keytab 中存在的任何服务主体。这提高了
	 * 那些对大小写敏感性看法不同的 Kerberos 实现之间的互操作性，
	 * 同时又并不真正打开
	 * 任何攻击途径。
	 */
	port->gss->cred = GSS_C_NO_CREDENTIAL;

	/*
	 * 用空上下文初始化序列
	 */
	port->gss->ctx = GSS_C_NO_CONTEXT;

	delegated_creds = GSS_C_NO_CREDENTIAL;
	port->gss->delegated_creds = false;

	/*
	 * 循环进行 GSSAPI 消息交换。这次交换可以包含
	 * 双向发送的多条消息。第一条消息始终来自
	 * 客户端。所有从客户端到服务器的消息都是口令报文
	 * （类型 'p'）。
	 */
	do
	{
		pq_startmsgread();

		CHECK_FOR_INTERRUPTS();

		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			/* 仅当客户端没有断开时才记错误。 */
			if (mtype != EOF)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected GSS response, got message type %d",
								mtype)));
			return STATUS_ERROR;
		}

		/* 取得实际的 GSS token */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))
		{
			/* EOF：pq_getmessage 已经记下了错误 */
			pfree(buf.data);
			return STATUS_ERROR;
		}

		/* 映射为 GSSAPI 风格的缓冲区 */
		gbuf.length = buf.len;
		gbuf.value = buf.data;

		elog(DEBUG4, "processing received GSS token of length %u",
			 (unsigned int) gbuf.length);

		maj_stat = gss_accept_sec_context(&min_stat,
										  &port->gss->ctx,
										  port->gss->cred,
										  &gbuf,
										  GSS_C_NO_CHANNEL_BINDINGS,
										  &port->gss->name,
										  NULL,
										  &port->gss->outbuf,
										  &gflags,
										  NULL,
										  pg_gss_accept_delegation ? &delegated_creds : NULL);

		/* gbuf 不再使用 */
		pfree(buf.data);

		elog(DEBUG5, "gss_accept_sec_context major: %u, "
			 "minor: %u, outlen: %u, outflags: %x",
			 maj_stat, min_stat,
			 (unsigned int) port->gss->outbuf.length, gflags);

		CHECK_FOR_INTERRUPTS();

		if (delegated_creds != GSS_C_NO_CREDENTIAL && gflags & GSS_C_DELEG_FLAG)
		{
			pg_store_delegated_credential(delegated_creds);
			port->gss->delegated_creds = true;
		}

		if (port->gss->outbuf.length != 0)
		{
			/*
			 * 协商产生了要发给客户端的数据。
			 */
			elog(DEBUG4, "sending GSS response token of length %u",
				 (unsigned int) port->gss->outbuf.length);

			sendAuthRequest(port, AUTH_REQ_GSS_CONT,
							port->gss->outbuf.value, port->gss->outbuf.length);

			gss_release_buffer(&lmin_s, &port->gss->outbuf);
		}

		if (maj_stat != GSS_S_COMPLETE && maj_stat != GSS_S_CONTINUE_NEEDED)
		{
			gss_delete_sec_context(&lmin_s, &port->gss->ctx, GSS_C_NO_BUFFER);
			pg_GSS_error(_("accepting GSS security context failed"),
						 maj_stat, min_stat);
			return STATUS_ERROR;
		}

		if (maj_stat == GSS_S_CONTINUE_NEEDED)
			elog(DEBUG4, "GSS continue needed");

	} while (maj_stat == GSS_S_CONTINUE_NEEDED);

	if (port->gss->cred != GSS_C_NO_CREDENTIAL)
	{
		/*
		 * 释放服务主体凭据
		 */
		gss_release_cred(&min_stat, &port->gss->cred);
	}
	return pg_GSS_checkauth(port);
}

/*
 * 检查通过 GSSAPI 认证的用户是否被允许以所声称的用户名
 * 连接。
 */
static int
pg_GSS_checkauth(Port *port)
{
	int			ret;
	OM_uint32	maj_stat,
				min_stat,
				lmin_s;
	gss_buffer_desc gbuf;
	char	   *princ;

	/*
	 * 取得已认证用户的名字，并与为该连接指定的 pg
	 * 用户名比较。
	 */
	maj_stat = gss_display_name(&min_stat, port->gss->name, &gbuf, NULL);
	if (maj_stat != GSS_S_COMPLETE)
	{
		pg_GSS_error(_("retrieving GSS user name failed"),
					 maj_stat, min_stat);
		return STATUS_ERROR;
	}

	/*
	 * gbuf.value 可能没有以空字符结尾，因此把它变成普通的
	 * 以空字符结尾的字符串。
	 */
	princ = palloc(gbuf.length + 1);
	memcpy(princ, gbuf.value, gbuf.length);
	princ[gbuf.length] = '\0';
	gss_release_buffer(&lmin_s, &gbuf);

	/*
	 * 把已认证主体的原始名字复制到我们的后端
	 * 内存中，供以后显示。
	 *
	 * 这也是我们的已认证身份。现在就设置它，而不是
	 * 等到下面的用户映射检查，因为认证已经
	 * 成功，我们希望日志反映出这一点。
	 */
	port->gss->princ = MemoryContextStrdup(TopMemoryContext, princ);
	set_authn_id(port, princ);

	/*
	 * 在领域分隔符处拆分用户名
	 */
	if (strchr(princ, '@'))
	{
		char	   *cp = strchr(princ, '@');

		/*
		 * 若我们不打算把领域包含进传给 ident 映射的
		 * 用户名，就在这里破坏性地修改它以去掉
		 * 领域。然后越过分隔符去检查领域。
		 */
		if (!port->hba->include_realm)
			*cp = '\0';
		cp++;

		if (port->hba->krb_realm != NULL && strlen(port->hba->krb_realm))
		{
			/*
			 * 先匹配名字中的领域部分
			 */
			if (pg_krb_caseins_users)
				ret = pg_strcasecmp(port->hba->krb_realm, cp);
			else
				ret = strcmp(port->hba->krb_realm, cp);

			if (ret)
			{
				/* GSS 领域不匹配 */
				elog(DEBUG2,
					 "GSSAPI realm (%s) and configured realm (%s) don't match",
					 cp, port->hba->krb_realm);
				pfree(princ);
				return STATUS_ERROR;
			}
		}
	}
	else if (port->hba->krb_realm && strlen(port->hba->krb_realm))
	{
		elog(DEBUG2,
			 "GSSAPI did not return realm but realm matching was requested");
		pfree(princ);
		return STATUS_ERROR;
	}

	ret = check_usermap(port->hba->usermap, port->user_name, princ,
						pg_krb_caseins_users);

	pfree(princ);

	return ret;
}
#endif							/* ENABLE_GSS */


/*----------------------------------------------------------------
 * SSPI authentication system
 *----------------------------------------------------------------
 */
#ifdef ENABLE_SSPI

/*
 * 为 SSPI 认证生成一条错误。调用方应当对 errmsg
 * 使用 _()，以便可以翻译。
 */
static void
pg_SSPI_error(int severity, const char *errmsg, SECURITY_STATUS r)
{
	char		sysmsg[256];

	if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS |
					  FORMAT_MESSAGE_FROM_SYSTEM,
					  NULL, r, 0,
					  sysmsg, sizeof(sysmsg), NULL) == 0)
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail_internal("SSPI error %x", (unsigned int) r)));
	else
		ereport(severity,
				(errmsg_internal("%s", errmsg),
				 errdetail_internal("%s (%x)", sysmsg, (unsigned int) r)));
}

static int
pg_SSPI_recvauth(Port *port)
{
	int			mtype;
	StringInfoData buf;
	SECURITY_STATUS r;
	CredHandle	sspicred;
	CtxtHandle *sspictx = NULL,
				newctx;
	TimeStamp	expiry;
	ULONG		contextattr;
	SecBufferDesc inbuf;
	SecBufferDesc outbuf;
	SecBuffer	OutBuffers[1];
	SecBuffer	InBuffers[1];
	HANDLE		token;
	TOKEN_USER *tokenuser;
	DWORD		retlen;
	char		accountname[MAXPGPATH];
	char		domainname[MAXPGPATH];
	DWORD		accountnamesize = sizeof(accountname);
	DWORD		domainnamesize = sizeof(domainname);
	SID_NAME_USE accountnameuse;
	char	   *authn_id;

	/*
	 * 取得服务器凭据的句柄。
	 */
	r = AcquireCredentialsHandle(NULL,
								 "negotiate",
								 SECPKG_CRED_INBOUND,
								 NULL,
								 NULL,
								 NULL,
								 NULL,
								 &sspicred,
								 &expiry);
	if (r != SEC_E_OK)
		pg_SSPI_error(ERROR, _("could not acquire SSPI credentials"), r);

	/*
	 * 循环进行 SSPI 消息交换。这次交换可以包含
	 * 双向发送的多条消息。第一条消息始终来自
	 * 客户端。所有从客户端到服务器的消息都是口令报文
	 * （类型 'p'）。
	 */
	do
	{
		pq_startmsgread();
		mtype = pq_getbyte();
		if (mtype != 'p')
		{
			if (sspictx != NULL)
			{
				DeleteSecurityContext(sspictx);
				free(sspictx);
			}
			FreeCredentialsHandle(&sspicred);

			/* 仅当客户端没有断开时才记错误。 */
			if (mtype != EOF)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("expected SSPI response, got message type %d",
								mtype)));
			return STATUS_ERROR;
		}

		/* 取得实际的 SSPI token */
		initStringInfo(&buf);
		if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))
		{
			/* EOF：pq_getmessage 已经记下了错误 */
			pfree(buf.data);
			if (sspictx != NULL)
			{
				DeleteSecurityContext(sspictx);
				free(sspictx);
			}
			FreeCredentialsHandle(&sspicred);
			return STATUS_ERROR;
		}

		/* 映射为 SSPI 风格的缓冲区 */
		inbuf.ulVersion = SECBUFFER_VERSION;
		inbuf.cBuffers = 1;
		inbuf.pBuffers = InBuffers;
		InBuffers[0].pvBuffer = buf.data;
		InBuffers[0].cbBuffer = buf.len;
		InBuffers[0].BufferType = SECBUFFER_TOKEN;

		/* 准备输出缓冲区 */
		OutBuffers[0].pvBuffer = NULL;
		OutBuffers[0].BufferType = SECBUFFER_TOKEN;
		OutBuffers[0].cbBuffer = 0;
		outbuf.cBuffers = 1;
		outbuf.pBuffers = OutBuffers;
		outbuf.ulVersion = SECBUFFER_VERSION;

		elog(DEBUG4, "processing received SSPI token of length %u",
			 (unsigned int) buf.len);

		r = AcceptSecurityContext(&sspicred,
								  sspictx,
								  &inbuf,
								  ASC_REQ_ALLOCATE_MEMORY,
								  SECURITY_NETWORK_DREP,
								  &newctx,
								  &outbuf,
								  &contextattr,
								  NULL);

		/* 输入缓冲区不再使用 */
		pfree(buf.data);

		if (outbuf.cBuffers > 0 && outbuf.pBuffers[0].cbBuffer > 0)
		{
			/*
			 * 协商产生了要发给客户端的数据。
			 */
			elog(DEBUG4, "sending SSPI response token of length %u",
				 (unsigned int) outbuf.pBuffers[0].cbBuffer);

			port->gss->outbuf.length = outbuf.pBuffers[0].cbBuffer;
			port->gss->outbuf.value = outbuf.pBuffers[0].pvBuffer;

			sendAuthRequest(port, AUTH_REQ_GSS_CONT,
							port->gss->outbuf.value, port->gss->outbuf.length);

			FreeContextBuffer(outbuf.pBuffers[0].pvBuffer);
		}

		if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
		{
			if (sspictx != NULL)
			{
				DeleteSecurityContext(sspictx);
				free(sspictx);
			}
			FreeCredentialsHandle(&sspicred);
			pg_SSPI_error(ERROR,
						  _("could not accept SSPI security context"), r);
		}

		/*
		 * 用我们刚收到的上下文覆盖当前上下文。若
		 * sspictx 为 NULL，这就是第一次循环，需要为它分配
		 * 缓冲区。随后的循环可以直接覆盖缓冲区
		 * 内容，因为大小不变。
		 */
		if (sspictx == NULL)
		{
			sspictx = malloc(sizeof(CtxtHandle));
			if (sspictx == NULL)
				ereport(ERROR,
						(errmsg("out of memory")));
		}

		memcpy(sspictx, &newctx, sizeof(CtxtHandle));

		if (r == SEC_I_CONTINUE_NEEDED)
			elog(DEBUG4, "SSPI continue needed");

	} while (r == SEC_I_CONTINUE_NEEDED);


	/*
	 * 释放服务主体凭据
	 */
	FreeCredentialsHandle(&sspicred);


	/*
	 * SEC_E_OK 表示认证现已完成。
	 *
	 * 取得已认证用户的名字，并把它与为该连接指定的 pg
	 * 用户名比较。
	 */

	r = QuerySecurityContextToken(sspictx, &token);
	if (r != SEC_E_OK)
		pg_SSPI_error(ERROR,
					  _("could not get token from SSPI security context"), r);

	/*
	 * 不再需要安全上下文，此后的一切改用
	 * token。
	 */
	DeleteSecurityContext(sspictx);
	free(sspictx);

	if (!GetTokenInformation(token, TokenUser, NULL, 0, &retlen) && GetLastError() != 122)
		ereport(ERROR,
				(errmsg_internal("could not get token information buffer size: error code %lu",
								 GetLastError())));

	tokenuser = malloc(retlen);
	if (tokenuser == NULL)
		ereport(ERROR,
				(errmsg("out of memory")));

	if (!GetTokenInformation(token, TokenUser, tokenuser, retlen, &retlen))
		ereport(ERROR,
				(errmsg_internal("could not get token information: error code %lu",
								 GetLastError())));

	CloseHandle(token);

	if (!LookupAccountSid(NULL, tokenuser->User.Sid, accountname, &accountnamesize,
						  domainname, &domainnamesize, &accountnameuse))
		ereport(ERROR,
				(errmsg_internal("could not look up account SID: error code %lu",
								 GetLastError())));

	free(tokenuser);

	if (!port->hba->compat_realm)
	{
		int			status = pg_SSPI_make_upn(accountname, sizeof(accountname),
											  domainname, sizeof(domainname),
											  port->hba->upn_username);

		if (status != STATUS_OK)
			/* 错误已由 pg_SSPI_make_upn 报告 */
			return status;
	}

	/*
	 * 我们已经有了构造已认证身份所需的全部信息。
	 * 现在就设置它，而不是等到下面的 check_usermap，因为
	 * 认证已经成功，我们希望日志
	 * 反映出这一点。
	 */
	if (port->hba->compat_realm)
	{
		/* SAM 兼容格式。 */
		authn_id = psprintf("%s\\%s", domainname, accountname);
	}
	else
	{
		/* Kerberos 主体格式。 */
		authn_id = psprintf("%s@%s", accountname, domainname);
	}

	set_authn_id(port, authn_id);
	pfree(authn_id);

	/*
	 * 若指定了，就比较领域/域。在 SSPI 中，比较始终
	 * 不区分大小写。
	 */
	if (port->hba->krb_realm && strlen(port->hba->krb_realm))
	{
		if (pg_strcasecmp(port->hba->krb_realm, domainname) != 0)
		{
			elog(DEBUG2,
				 "SSPI domain (%s) and configured domain (%s) don't match",
				 domainname, port->hba->krb_realm);

			return STATUS_ERROR;
		}
	}

	/*
	 * 我们在 accountname 中有用户名（不含域/领域），与
	 * 所提供的值比较。在 SSPI 中，比较始终不区分大小写。
	 *
	 * 若设置为包含领域，就按 <username>@<realm> 格式追加它。
	 */
	if (port->hba->include_realm)
	{
		char	   *namebuf;
		int			retval;

		namebuf = psprintf("%s@%s", accountname, domainname);
		retval = check_usermap(port->hba->usermap, port->user_name, namebuf, true);
		pfree(namebuf);
		return retval;
	}
	else
		return check_usermap(port->hba->usermap, port->user_name, accountname, true);
}

/*
 * 用 Kerberos 领域名替换 domainname，
 * 并可选地用 Kerberos 用户名替换 accountname。
 */
static int
pg_SSPI_make_upn(char *accountname,
				 size_t accountnamesize,
				 char *domainname,
				 size_t domainnamesize,
				 bool update_accountname)
{
	char	   *samname;
	char	   *upname = NULL;
	char	   *p = NULL;
	ULONG		upnamesize = 0;
	size_t		upnamerealmsize;
	BOOLEAN		res;

	/*
	 * 构造 SAM 名（DOMAIN\user），然后转换成 UPN
	 * （user@kerberos.realm）。领域名以小写返回，但
	 * 这没问题，因为在 SSPI 认证中，字符串比较始终
	 * 不区分大小写。
	 */

	samname = psprintf("%s\\%s", domainname, accountname);
	res = TranslateName(samname, NameSamCompatible, NameUserPrincipal,
						NULL, &upnamesize);

	if ((!res && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		|| upnamesize == 0)
	{
		pfree(samname);
		ereport(LOG,
				(errcode(ERRCODE_INVALID_ROLE_SPECIFICATION),
				 errmsg("could not translate name")));
		return STATUS_ERROR;
	}

	/* upnamesize 包含结尾的 NUL。 */
	upname = palloc(upnamesize);

	res = TranslateName(samname, NameSamCompatible, NameUserPrincipal,
						upname, &upnamesize);

	pfree(samname);
	if (res)
		p = strchr(upname, '@');

	if (!res || p == NULL)
	{
		pfree(upname);
		ereport(LOG,
				(errcode(ERRCODE_INVALID_ROLE_SPECIFICATION),
				 errmsg("could not translate name")));
		return STATUS_ERROR;
	}

	/* '@' 之后领域名的长度，包括 NUL。 */
	upnamerealmsize = upnamesize - (p - upname + 1);

	/* 用领域名替换 domainname。 */
	if (upnamerealmsize > domainnamesize)
	{
		pfree(upname);
		ereport(LOG,
				(errcode(ERRCODE_INVALID_ROLE_SPECIFICATION),
				 errmsg("realm name too long")));
		return STATUS_ERROR;
	}

	/* 长度现在是安全的。 */
	strcpy(domainname, p + 1);

	/* 也替换账户名（以防 UPN != SAM）？ */
	if (update_accountname)
	{
		if ((p - upname + 1) > accountnamesize)
		{
			pfree(upname);
			ereport(LOG,
					(errcode(ERRCODE_INVALID_ROLE_SPECIFICATION),
					 errmsg("translated account name too long")));
			return STATUS_ERROR;
		}

		*p = 0;
		strcpy(accountname, upname);
	}

	pfree(upname);
	return STATUS_OK;
}
#endif							/* ENABLE_SSPI */



/*----------------------------------------------------------------
 * Ident authentication system
 *----------------------------------------------------------------
 */

/*
 *	把字符串 "*ident_response" 解析为对 Ident
 *	服务器查询的响应。若它是表明用户名的正常响应，就返回 true
 *	并把用户名存到 *ident_user。若是其他任何东西，
 *	就返回 false。
 */
static bool
interpret_ident_response(const char *ident_response,
						 char *ident_user)
{
	const char *cursor = ident_response;	/* 指向 *ident_response 的游标 */

	/*
	 * Ident 的响应按 telnet 惯例应以 crlf（\r\n）结束。
	 */
	if (strlen(ident_response) < 2)
		return false;
	else if (ident_response[strlen(ident_response) - 2] != '\r')
		return false;
	else
	{
		while (*cursor != ':' && *cursor != '\r')
			cursor++;			/* 跳过端口字段 */

		if (*cursor != ':')
			return false;
		else
		{
			/* 我们正位于响应类型字段之前的冒号处 */
			char		response_type[80];
			int			i;		/* 指向 *response_type 的下标 */

			cursor++;			/* 越过冒号 */
			while (pg_isblank(*cursor))
				cursor++;		/* 跳过空白 */
			i = 0;
			while (*cursor != ':' && *cursor != '\r' && !pg_isblank(*cursor) &&
				   i < (int) (sizeof(response_type) - 1))
				response_type[i++] = *cursor++;
			response_type[i] = '\0';
			while (pg_isblank(*cursor))
				cursor++;		/* 跳过空白 */
			if (strcmp(response_type, "USERID") != 0)
				return false;
			else
			{
				/*
				 * 这是 USERID 响应。很好。"cursor" 应当指向
				 * 操作系统类型之前的那个冒号。
				 */
				if (*cursor != ':')
					return false;
				else
				{
					cursor++;	/* 越过冒号 */
					/* 跳过操作系统字段。 */
					while (*cursor != ':' && *cursor != '\r')
						cursor++;
					if (*cursor != ':')
						return false;
					else
					{
						cursor++;	/* 越过冒号 */
						while (pg_isblank(*cursor))
							cursor++;	/* 跳过空白 */
						/* 该行剩余部分是用户名。把它复制过来。 */
						i = 0;
						while (*cursor != '\r' && i < IDENT_USERNAME_MAX)
							ident_user[i++] = *cursor++;
						ident_user[i] = '\0';
						return true;
					}
				}
			}
		}
	}
}


/*
 *	与 "remote_addr" 上的 ident 服务器交谈，查明谁
 *	拥有到 "local_addr" 的 TCP 连接
 *	若成功取到用户名，就检查用户映射。
 *
 *	XXX：若锁存器已置位，使用 WaitLatchOrSocket() 并执行 CHECK_FOR_INTERRUPTS()，
 *	会改善对超时和取消的响应能力。
 */
static int
ident_inet(hbaPort *port)
{
	const SockAddr remote_addr = port->raddr;
	const SockAddr local_addr = port->laddr;
	char		ident_user[IDENT_USERNAME_MAX + 1];
	pgsocket	sock_fd = PGINVALID_SOCKET; /* 用于与 Ident 服务器交谈 */
	int			rc;				/* 本地调用函数的返回码 */
	bool		ident_return;
	char		remote_addr_s[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	char		local_addr_s[NI_MAXHOST];
	char		local_port[NI_MAXSERV];
	char		ident_port[NI_MAXSERV];
	char		ident_query[80];
	char		ident_response[80 + IDENT_USERNAME_MAX];
	struct addrinfo *ident_serv = NULL,
			   *la = NULL,
				hints;

	/*
	 * 先转成文本再转回 sockaddr 可能看起来有点怪，
	 * 但这与协议无关。
	 */
	pg_getnameinfo_all(&remote_addr.addr, remote_addr.salen,
					   remote_addr_s, sizeof(remote_addr_s),
					   remote_port, sizeof(remote_port),
					   NI_NUMERICHOST | NI_NUMERICSERV);
	pg_getnameinfo_all(&local_addr.addr, local_addr.salen,
					   local_addr_s, sizeof(local_addr_s),
					   local_port, sizeof(local_port),
					   NI_NUMERICHOST | NI_NUMERICSERV);

	snprintf(ident_port, sizeof(ident_port), "%d", IDENT_PORT);
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = remote_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = pg_getaddrinfo_all(remote_addr_s, ident_port, &hints, &ident_serv);
	if (rc || !ident_serv)
	{
		/* 我们不期望发生这种情况 */
		ident_return = false;
		goto ident_inet_done;
	}

	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = local_addr.addr.ss_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	rc = pg_getaddrinfo_all(local_addr_s, NULL, &hints, &la);
	if (rc || !la)
	{
		/* 我们不期望发生这种情况 */
		ident_return = false;
		goto ident_inet_done;
	}

	sock_fd = socket(ident_serv->ai_family, ident_serv->ai_socktype,
					 ident_serv->ai_protocol);
	if (sock_fd == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not create socket for Ident connection: %m")));
		ident_return = false;
		goto ident_inet_done;
	}

	/*
	 * 绑定到客户端最初联系的地址，否则
	 * Ident 服务器将无法匹配到正确的连接。若
	 * PostgreSQL 服务器跑在 IP 别名上，这就有必要。
	 */
	rc = bind(sock_fd, la->ai_addr, la->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not bind to local address \"%s\": %m",
						local_addr_s)));
		ident_return = false;
		goto ident_inet_done;
	}

	rc = connect(sock_fd, ident_serv->ai_addr,
				 ident_serv->ai_addrlen);
	if (rc != 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not connect to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	/* 我们发给 Ident 服务器的查询 */
	snprintf(ident_query, sizeof(ident_query), "%s,%s\r\n",
			 remote_port, local_port);

	/* 循环，以防 send 被中断 */
	do
	{
		CHECK_FOR_INTERRUPTS();

		rc = send(sock_fd, ident_query, strlen(ident_query), 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not send query to Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	do
	{
		CHECK_FOR_INTERRUPTS();

		rc = recv(sock_fd, ident_response, sizeof(ident_response) - 1, 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
	{
		ereport(LOG,
				(errcode_for_socket_access(),
				 errmsg("could not receive response from Ident server at address \"%s\", port %s: %m",
						remote_addr_s, ident_port)));
		ident_return = false;
		goto ident_inet_done;
	}

	ident_response[rc] = '\0';
	ident_return = interpret_ident_response(ident_response, ident_user);
	if (!ident_return)
		ereport(LOG,
				(errmsg("invalidly formatted response from Ident server: \"%s\"",
						ident_response)));

ident_inet_done:
	if (sock_fd != PGINVALID_SOCKET)
		closesocket(sock_fd);
	if (ident_serv)
		pg_freeaddrinfo_all(remote_addr.addr.ss_family, ident_serv);
	if (la)
		pg_freeaddrinfo_all(local_addr.addr.ss_family, la);

	if (ident_return)
	{
		/*
		 * 成功！先保存身份，再检查用户映射。注意，
		 * 设置已认证身份是在检查用户映射之前做的，
		 * 因为此时认证已经成功。
		 */
		set_authn_id(port, ident_user);
		return check_usermap(port->hba->usermap, port->user_name, ident_user, false);
	}
	return STATUS_ERROR;
}


/*----------------------------------------------------------------
 * Peer authentication system
 *----------------------------------------------------------------
 */

/*
 *	向内核询问正在连接的进程的凭据，
 *	确定对应用户的符号名，并检查
 *	按用户映射是否有效。
 *
 *	当且仅当已授权时返回 STATUS_OK，否则返回 STATUS_ERROR。
 */
static int
auth_peer(hbaPort *port)
{
	uid_t		uid;
	gid_t		gid;
#ifndef WIN32
	struct passwd *pw;
	int			ret;
#endif

	if (getpeereid(port->sock, &uid, &gid) != 0)
	{
		/* 若 getpeereid 是桩函数，则给出专门的错误消息 */
		if (errno == ENOSYS)
			ereport(LOG,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("peer authentication is not supported on this platform")));
		else
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("could not get peer credentials: %m")));
		return STATUS_ERROR;
	}

#ifndef WIN32
	errno = 0;					/* 调用前清除 errno */
	pw = getpwuid(uid);
	if (!pw)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errmsg("could not look up local user ID %ld: %s",
						(long) uid,
						save_errno ? strerror(save_errno) : _("user does not exist"))));
		return STATUS_ERROR;
	}

	/*
	 * 复制静态 getpw*() 结果区的一份副本；这是我们的已认证
	 * 身份。在调用 check_usermap 之前设置它，因为认证
	 * 已经成功，我们希望日志反映出这一点。
	 */
	set_authn_id(port, pw->pw_name);

	ret = check_usermap(port->hba->usermap, port->user_name,
						MyClientConnectionInfo.authn_id, false);

	return ret;
#else
	/* 上面本应以 ENOSYS 失败 */
	Assert(false);
	return STATUS_ERROR;
#endif
}


/*----------------------------------------------------------------
 * PAM authentication system
 *----------------------------------------------------------------
 */
#ifdef USE_PAM

/*
 * PAM 对话函数
 */

static int
pam_passwd_conv_proc(int num_msg, PG_PAM_CONST struct pam_message **msg,
					 struct pam_response **resp, void *appdata_ptr)
{
	const char *passwd;
	struct pam_response *reply;
	int			i;

	if (appdata_ptr)
		passwd = (char *) appdata_ptr;
	else
	{
		/*
		 * 针对 Solaris 2.6 的变通办法，那里的 PAM 库有缺陷，不会
		 * 把 appdata_ptr 传给对话例程
		 */
		passwd = pam_passwd;
	}

	*resp = NULL;				/* 以便出错时退出 */

	if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;

	/*
	 * 这里明确不用 palloc：PAM 会在
	 * pam_end() 中释放这块内存
	 */
	if ((reply = calloc(num_msg, sizeof(struct pam_response))) == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return PAM_CONV_ERR;
	}

	for (i = 0; i < num_msg; i++)
	{
		switch (msg[i]->msg_style)
		{
			case PAM_PROMPT_ECHO_OFF:
				if (strlen(passwd) == 0)
				{
					/*
					 * 口令第一次没有传给 PAM。
					 * 我们就去请客户端发送口令，然后再把它
					 * 塞进 PAM。
					 */
					sendAuthRequest(pam_port_cludge, AUTH_REQ_PASSWORD, NULL, 0);
					passwd = recv_password_packet(pam_port_cludge);
					if (passwd == NULL)
					{
						/*
						 * 客户端不想发送口令。我们
						 * 有意不记录与此有关的任何内容，
						 * 无论在这里还是在更高层。
						 */
						pam_no_password = true;
						goto fail;
					}
				}
				if ((reply[i].resp = strdup(passwd)) == NULL)
					goto fail;
				reply[i].resp_retcode = PAM_SUCCESS;
				break;
			case PAM_ERROR_MSG:
				ereport(LOG,
						(errmsg("error from underlying PAM layer: %s",
								msg[i]->msg)));
				/* FALL THROUGH */
			case PAM_TEXT_INFO:
				/* 我们不费心记录 TEXT_INFO 消息 */
				if ((reply[i].resp = strdup("")) == NULL)
					goto fail;
				reply[i].resp_retcode = PAM_SUCCESS;
				break;
			default:
				ereport(LOG,
						(errmsg("unsupported PAM conversation %d/\"%s\"",
								msg[i]->msg_style,
								msg[i]->msg ? msg[i]->msg : "(none)")));
				goto fail;
		}
	}

	*resp = reply;
	return PAM_SUCCESS;

fail:
	/* 释放我们分配的任何东西 */
	for (i = 0; i < num_msg; i++)
		free(reply[i].resp);
	free(reply);

	return PAM_CONV_ERR;
}


/*
 * 对照 PAM 检查认证。
 */
static int
CheckPAMAuth(Port *port, const char *user, const char *password)
{
	int			retval;
	pam_handle_t *pamh = NULL;

	/*
	 * 我们无法完全依赖 PAM 传过 appdata：至少在
	 * Solaris 2.6 上它似乎不起作用。因此改用这些
	 * 难看的静态变量。
	 */
	pam_passwd = password;
	pam_port_cludge = port;
	pam_no_password = false;

	/*
	 * 设置对话结构体的应用数据部分。稍后
	 * 在 PAM 对话内部用它把口令传给
	 * 认证模块。
	 */
	pam_passw_conv.appdata_ptr = unconstify(char *, password);	/* 来自上面的口令，
																 * 并未单独分配 */

	/* 可选地，可以在 pg_hba.conf 中设置服务名 */
	if (port->hba->pamservice && port->hba->pamservice[0] != '\0')
		retval = pam_start(port->hba->pamservice, "pgsql@",
						   &pam_passw_conv, &pamh);
	else
		retval = pam_start(PGSQL_PAM_SERVICE, "pgsql@",
						   &pam_passw_conv, &pamh);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("could not create PAM authenticator: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* 清除 pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_set_item(pamh, PAM_USER, user);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_set_item(PAM_USER) failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* 清除 pam_passwd */
		return STATUS_ERROR;
	}

	if (port->hba->conntype != ctLocal)
	{
		char		hostinfo[NI_MAXHOST];
		int			flags;

		if (port->hba->pam_use_hostname)
			flags = 0;
		else
			flags = NI_NUMERICHOST | NI_NUMERICSERV;

		retval = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
									hostinfo, sizeof(hostinfo), NULL, 0,
									flags);
		if (retval != 0)
		{
			ereport(WARNING,
					(errmsg_internal("pg_getnameinfo_all() failed: %s",
									 gai_strerror(retval))));
			return STATUS_ERROR;
		}

		retval = pam_set_item(pamh, PAM_RHOST, hostinfo);

		if (retval != PAM_SUCCESS)
		{
			ereport(LOG,
					(errmsg("pam_set_item(PAM_RHOST) failed: %s",
							pam_strerror(pamh, retval))));
			pam_passwd = NULL;
			return STATUS_ERROR;
		}
	}

	retval = pam_set_item(pamh, PAM_CONV, &pam_passw_conv);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("pam_set_item(PAM_CONV) failed: %s",
						pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* 清除 pam_passwd */
		return STATUS_ERROR;
	}

	retval = pam_authenticate(pamh, 0);

	if (retval != PAM_SUCCESS)
	{
		/* 若 pam_passwd_conv_proc 看到 EOF，不要记任何日志 */
		if (!pam_no_password)
			ereport(LOG,
					(errmsg("pam_authenticate failed: %s",
							pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* 清除 pam_passwd */
		return pam_no_password ? STATUS_EOF : STATUS_ERROR;
	}

	retval = pam_acct_mgmt(pamh, 0);

	if (retval != PAM_SUCCESS)
	{
		/* 若 pam_passwd_conv_proc 看到 EOF，不要记任何日志 */
		if (!pam_no_password)
			ereport(LOG,
					(errmsg("pam_acct_mgmt failed: %s",
							pam_strerror(pamh, retval))));
		pam_passwd = NULL;		/* 清除 pam_passwd */
		return pam_no_password ? STATUS_EOF : STATUS_ERROR;
	}

	retval = pam_end(pamh, retval);

	if (retval != PAM_SUCCESS)
	{
		ereport(LOG,
				(errmsg("could not release PAM authenticator: %s",
						pam_strerror(pamh, retval))));
	}

	pam_passwd = NULL;			/* 清除 pam_passwd */

	if (retval == PAM_SUCCESS)
		set_authn_id(port, user);

	return (retval == PAM_SUCCESS ? STATUS_OK : STATUS_ERROR);
}
#endif							/* USE_PAM */


/*----------------------------------------------------------------
 * BSD authentication system
 *----------------------------------------------------------------
 */
#ifdef USE_BSD_AUTH
static int
CheckBSDAuth(Port *port, char *user)
{
	char	   *passwd;
	int			retval;

	/* 向客户端发送普通口令请求，并取得响应 */
	sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;

	/*
	 * 请 BSD 认证系统校验口令。注意 auth_userokay
	 * 会用零覆盖口令字符串，但它只是
	 * 临时字符串，所以我们不在意。
	 */
	retval = auth_userokay(user, NULL, "auth-postgresql", passwd);

	pfree(passwd);

	if (!retval)
		return STATUS_ERROR;

	set_authn_id(port, user);
	return STATUS_OK;
}
#endif							/* USE_BSD_AUTH */


/*----------------------------------------------------------------
 * LDAP authentication system
 *----------------------------------------------------------------
 */
#ifdef USE_LDAP

static int	errdetail_for_ldap(LDAP *ldap);

/*
 * 初始化到 LDAP 服务器的连接，包括在指定时
 * 设置 TLS。
 */
static int
InitializeLDAPConnection(Port *port, LDAP **ldap)
{
	const char *scheme;
	int			ldapversion = LDAP_VERSION3;
	int			r;

	scheme = port->hba->ldapscheme;
	if (scheme == NULL)
		scheme = "ldap";
#ifdef WIN32
	if (strcmp(scheme, "ldaps") == 0)
		*ldap = ldap_sslinit(port->hba->ldapserver, port->hba->ldapport, 1);
	else
		*ldap = ldap_init(port->hba->ldapserver, port->hba->ldapport);
	if (!*ldap)
	{
		ereport(LOG,
				(errmsg("could not initialize LDAP: error code %d",
						(int) LdapGetLastError())));

		return STATUS_ERROR;
	}
#else
#ifdef HAVE_LDAP_INITIALIZE

	/*
	 * OpenLDAP 提供非标准扩展 ldap_initialize()，它接受
	 * URIs 列表，使我们能请求 "ldaps" 而不是 "ldap"。它
	 * 还提供 ldap_domain2hostlist()，以便用 DNS SRV 自动
	 * 查找 LDAP 服务器。它们在同一版本引入，因此目前我们
	 * 没有针对后者的额外 configure 检查。
	 */
	{
		StringInfoData uris;
		char	   *hostlist = NULL;
		char	   *p;
		bool		append_port;

		/* 这里将构造一个以空格分隔的 scheme://hostname:port 列表 */
		initStringInfo(&uris);

		/*
		 * 若 pg_hba.conf 没有提供主机名，我们可以请 OpenLDAP 试着
		 * 从基准 DN 中提取域名，并查找
		 * _ldap._tcp.<domain> 的 DSN SRV 记录。
		 */
		if (!port->hba->ldapserver || port->hba->ldapserver[0] == '\0')
		{
			char	   *domain;

			/* 例如 ou=blah,dc=foo,dc=bar -> foo.bar */
			if (ldap_dn2domain(port->hba->ldapbasedn, &domain))
			{
				ereport(LOG,
						(errmsg("could not extract domain name from ldapbasedn")));
				return STATUS_ERROR;
			}

			/* 查找 LDAP 服务器主机和端口号列表 */
			if (ldap_domain2hostlist(domain, &hostlist))
			{
				ereport(LOG,
						(errmsg("LDAP authentication could not find DNS SRV records for \"%s\"",
								domain),
						 (errhint("Set an LDAP server name explicitly."))));
				ldap_memfree(domain);
				return STATUS_ERROR;
			}
			ldap_memfree(domain);

			/* 我们有一个以空格分隔的 host:port 项列表 */
			p = hostlist;
			append_port = false;
		}
		else
		{
			/* 我们有一个来自 pg_hba.conf 的、以空格分隔的主机列表 */
			p = port->hba->ldapserver;
			append_port = true;
		}

		/* 把 host[:port] 项列表转换成完整的 URIs */
		do
		{
			size_t		size;

			/* 找出下一项的跨度 */
			size = strcspn(p, " ");

			/* 若这不是第一个 URI，就追加一个空格分隔符 */
			if (uris.len > 0)
				appendStringInfoChar(&uris, ' ');

			/* 追加 scheme://host:port */
			appendStringInfoString(&uris, scheme);
			appendStringInfoString(&uris, "://");
			appendBinaryStringInfo(&uris, p, size);
			if (append_port)
				appendStringInfo(&uris, ":%d", port->hba->ldapport);

			/* 跳过这一项以及其后任意个空格 */
			p += size;
			while (*p == ' ')
				++p;
		} while (*p);

		/* 若查找了 SRV 记录，就释放 OpenLDAP 的内存 */
		if (hostlist)
			ldap_memfree(hostlist);

		/* 最后，尝试用 URI 列表连接 */
		r = ldap_initialize(ldap, uris.data);
		pfree(uris.data);
		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not initialize LDAP: %s",
							ldap_err2string(r))));

			return STATUS_ERROR;
		}
	}
#else
	if (strcmp(scheme, "ldaps") == 0)
	{
		ereport(LOG,
				(errmsg("ldaps not supported with this LDAP library")));

		return STATUS_ERROR;
	}
	*ldap = ldap_init(port->hba->ldapserver, port->hba->ldapport);
	if (!*ldap)
	{
		ereport(LOG,
				(errmsg("could not initialize LDAP: %m")));

		return STATUS_ERROR;
	}
#endif
#endif

	if ((r = ldap_set_option(*ldap, LDAP_OPT_PROTOCOL_VERSION, &ldapversion)) != LDAP_SUCCESS)
	{
		ereport(LOG,
				(errmsg("could not set LDAP protocol version: %s",
						ldap_err2string(r)),
				 errdetail_for_ldap(*ldap)));
		ldap_unbind(*ldap);
		return STATUS_ERROR;
	}

	if (port->hba->ldaptls)
	{
#ifndef WIN32
		if ((r = ldap_start_tls_s(*ldap, NULL, NULL)) != LDAP_SUCCESS)
#else
		if ((r = ldap_start_tls_s(*ldap, NULL, NULL, NULL, NULL)) != LDAP_SUCCESS)
#endif
		{
			ereport(LOG,
					(errmsg("could not start LDAP TLS session: %s",
							ldap_err2string(r)),
					 errdetail_for_ldap(*ldap)));
			ldap_unbind(*ldap);
			return STATUS_ERROR;
		}
	}

	return STATUS_OK;
}

/* FormatSearchFilter 识别的占位符。目前只有一个。 */
#define LPH_USERNAME "$username"
#define LPH_USERNAME_LEN (sizeof(LPH_USERNAME) - 1)

/* 并非所有 LDAP 实现都定义了它。 */
#ifndef LDAP_NO_ATTRS
#define LDAP_NO_ATTRS "1.1"
#endif

/* 并非所有 LDAP 实现都定义了它。 */
#ifndef LDAPS_PORT
#define LDAPS_PORT 636
#endif

static char *
dummy_ldap_password_mutator(char *input)
{
	return input;
}

/*
 * 返回从 "pattern" 新分配的 C 字符串，其中所有
 * 占位符 "$username" 都被替换为 "user_name"。
 */
static char *
FormatSearchFilter(const char *pattern, const char *user_name)
{
	StringInfoData output;

	initStringInfo(&output);
	while (*pattern != '\0')
	{
		if (strncmp(pattern, LPH_USERNAME, LPH_USERNAME_LEN) == 0)
		{
			appendStringInfoString(&output, user_name);
			pattern += LPH_USERNAME_LEN;
		}
		else
			appendStringInfoChar(&output, *pattern++);
	}

	return output.data;
}

/*
 * 执行 LDAP 认证
 */
static int
CheckLDAPAuth(Port *port)
{
	char	   *passwd;
	LDAP	   *ldap;
	int			r;
	char	   *fulluser;
	const char *server_name;

#ifdef HAVE_LDAP_INITIALIZE

	/*
	 * 对 OpenLDAP，若有 basedn，就允许空主机名。我们将通过
	 * OpenLDAP 库的功能用 DNS SRV 记录查找服务器。
	 */
	if ((!port->hba->ldapserver || port->hba->ldapserver[0] == '\0') &&
		(!port->hba->ldapbasedn || port->hba->ldapbasedn[0] == '\0'))
	{
		ereport(LOG,
				(errmsg("LDAP server not specified, and no ldapbasedn")));
		return STATUS_ERROR;
	}
#else
	if (!port->hba->ldapserver || port->hba->ldapserver[0] == '\0')
	{
		ereport(LOG,
				(errmsg("LDAP server not specified")));
		return STATUS_ERROR;
	}
#endif

	/*
	 * 若使用 SRV 记录，我们就没有服务器名，因此错误消息里
	 * 只显示空字符串。
	 */
	server_name = port->hba->ldapserver ? port->hba->ldapserver : "";

	if (port->hba->ldapport == 0)
	{
		if (port->hba->ldapscheme != NULL &&
			strcmp(port->hba->ldapscheme, "ldaps") == 0)
			port->hba->ldapport = LDAPS_PORT;
		else
			port->hba->ldapport = LDAP_PORT;
	}

	sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* 客户端不会发送口令 */

	if (InitializeLDAPConnection(port, &ldap) == STATUS_ERROR)
	{
		/* 错误消息已经发出 */
		pfree(passwd);
		return STATUS_ERROR;
	}

	if (port->hba->ldapbasedn)
	{
		/*
		 * 先做一次 LDAP 搜索，找出我们正要登录的用户的
		 * 可分辨名称 (DN)。
		 */
		char	   *filter;
		LDAPMessage *search_message;
		LDAPMessage *entry;
		char	   *attributes[] = {LDAP_NO_ATTRS, NULL};
		char	   *dn;
		char	   *c;
		int			count;

		/*
		 * 禁止任何否则就需要转义的字符，
		 * 因为它们在用户名里本来就不合理。允许
		 * 它们将使人能够向 LDAP 过滤器注入任何
		 * 自定义过滤器。
		 */
		for (c = port->user_name; *c; c++)
		{
			if (*c == '*' ||
				*c == '(' ||
				*c == ')' ||
				*c == '\\' ||
				*c == '/')
			{
				ereport(LOG,
						(errmsg("invalid character in user name for LDAP authentication")));
				ldap_unbind(ldap);
				pfree(passwd);
				return STATUS_ERROR;
			}
		}

		/*
		 * 用预定义的用户名/口令绑定（若有）来
		 * 搜索。若未指定，这就变成匿名绑定。
		 */
		r = ldap_simple_bind_s(ldap,
							   port->hba->ldapbinddn ? port->hba->ldapbinddn : "",
							   port->hba->ldapbindpasswd ? ldap_password_hook(port->hba->ldapbindpasswd) : "");
		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not perform initial LDAP bind for ldapbinddn \"%s\" on server \"%s\": %s",
							port->hba->ldapbinddn ? port->hba->ldapbinddn : "",
							server_name,
							ldap_err2string(r)),
					 errdetail_for_ldap(ldap)));
			ldap_unbind(ldap);
			pfree(passwd);
			return STATUS_ERROR;
		}

		/* 构造自定义过滤器，还是单一属性过滤器？ */
		if (port->hba->ldapsearchfilter)
			filter = FormatSearchFilter(port->hba->ldapsearchfilter, port->user_name);
		else if (port->hba->ldapsearchattribute)
			filter = psprintf("(%s=%s)", port->hba->ldapsearchattribute, port->user_name);
		else
			filter = psprintf("(uid=%s)", port->user_name);

		search_message = NULL;
		r = ldap_search_s(ldap,
						  port->hba->ldapbasedn,
						  port->hba->ldapscope,
						  filter,
						  attributes,
						  0,
						  &search_message);

		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not search LDAP for filter \"%s\" on server \"%s\": %s",
							filter, server_name, ldap_err2string(r)),
					 errdetail_for_ldap(ldap)));
			if (search_message != NULL)
				ldap_msgfree(search_message);
			ldap_unbind(ldap);
			pfree(passwd);
			pfree(filter);
			return STATUS_ERROR;
		}

		count = ldap_count_entries(ldap, search_message);
		if (count != 1)
		{
			if (count == 0)
				ereport(LOG,
						(errmsg("LDAP user \"%s\" does not exist", port->user_name),
						 errdetail("LDAP search for filter \"%s\" on server \"%s\" returned no entries.",
								   filter, server_name)));
			else
				ereport(LOG,
						(errmsg("LDAP user \"%s\" is not unique", port->user_name),
						 errdetail_plural("LDAP search for filter \"%s\" on server \"%s\" returned %d entry.",
										  "LDAP search for filter \"%s\" on server \"%s\" returned %d entries.",
										  count,
										  filter, server_name, count)));

			ldap_unbind(ldap);
			pfree(passwd);
			pfree(filter);
			ldap_msgfree(search_message);
			return STATUS_ERROR;
		}

		entry = ldap_first_entry(ldap, search_message);
		dn = ldap_get_dn(ldap, entry);
		if (dn == NULL)
		{
			int			error;

			(void) ldap_get_option(ldap, LDAP_OPT_ERROR_NUMBER, &error);
			ereport(LOG,
					(errmsg("could not get dn for the first entry matching \"%s\" on server \"%s\": %s",
							filter, server_name,
							ldap_err2string(error)),
					 errdetail_for_ldap(ldap)));
			ldap_unbind(ldap);
			pfree(passwd);
			pfree(filter);
			ldap_msgfree(search_message);
			return STATUS_ERROR;
		}
		fulluser = pstrdup(dn);

		pfree(filter);
		ldap_memfree(dn);
		ldap_msgfree(search_message);

		/* 解除绑定并断开与 LDAP 服务器的连接 */
		r = ldap_unbind_s(ldap);
		if (r != LDAP_SUCCESS)
		{
			ereport(LOG,
					(errmsg("could not unbind after searching for user \"%s\" on server \"%s\"",
							fulluser, server_name)));
			pfree(passwd);
			pfree(fulluser);
			return STATUS_ERROR;
		}

		/*
		 * 需要重新初始化 LDAP 连接，以便用另一个
		 * 用户名绑定到它。
		 */
		if (InitializeLDAPConnection(port, &ldap) == STATUS_ERROR)
		{
			pfree(passwd);
			pfree(fulluser);

			/* 错误消息已经发出 */
			return STATUS_ERROR;
		}
	}
	else
		fulluser = psprintf("%s%s%s",
							port->hba->ldapprefix ? port->hba->ldapprefix : "",
							port->user_name,
							port->hba->ldapsuffix ? port->hba->ldapsuffix : "");

	r = ldap_simple_bind_s(ldap, fulluser, passwd);

	if (r != LDAP_SUCCESS)
	{
		ereport(LOG,
				(errmsg("LDAP login failed for user \"%s\" on server \"%s\": %s",
						fulluser, server_name, ldap_err2string(r)),
				 errdetail_for_ldap(ldap)));
		ldap_unbind(ldap);
		pfree(passwd);
		pfree(fulluser);
		return STATUS_ERROR;
	}

	/* 把原来的绑定可分辨名称 (DN) 存为已认证身份。 */
	set_authn_id(port, fulluser);

	ldap_unbind(ldap);
	pfree(passwd);
	pfree(fulluser);

	return STATUS_OK;
}

/*
 * 若能从 LDAP 的诊断消息构造出细节错误文本，
 * 就把它加到当前错误上。
 */
static int
errdetail_for_ldap(LDAP *ldap)
{
	char	   *message;
	int			rc;

	rc = ldap_get_option(ldap, LDAP_OPT_DIAGNOSTIC_MESSAGE, &message);
	if (rc == LDAP_SUCCESS && message != NULL)
	{
		errdetail("LDAP diagnostics: %s", message);
		ldap_memfree(message);
	}

	return 0;
}

#endif							/* USE_LDAP */


/*----------------------------------------------------------------
 * SSL client certificate authentication
 *----------------------------------------------------------------
 */
#ifdef USE_SSL
static int
CheckCertAuth(Port *port)
{
	int			status_check_usermap = STATUS_ERROR;
	char	   *peer_username = NULL;

	Assert(port->ssl);

	/* 选择要比较的正确字段 */
	switch (port->hba->clientcertname)
	{
		case clientCertDN:
			peer_username = port->peer_dn;
			break;
		case clientCertCN:
			peer_username = port->peer_cn;
	}

	/* 确保我们已经从证书中收到用户名 */
	if (peer_username == NULL ||
		strlen(peer_username) <= 0)
	{
		ereport(LOG,
				(errmsg("certificate authentication failed for user \"%s\": client certificate contains no user name",
						port->user_name)));
		return STATUS_ERROR;
	}

	if (port->hba->auth_method == uaCert)
	{
		/*
		 * 对证书认证，客户端的主题可分辨名称 (DN) 始终是我们的已认证
		 * 身份，即使我们只用它的 CN 做授权。现在就
		 * 设置它，而不是等到下面的 check_usermap()，因为
		 * 认证已经成功，我们希望日志
		 * 反映出这一点。
		 */
		if (!port->peer_dn)
		{
			/*
			 * 这不应发生，因为在此上下文中 peer_dn 和 peer_cn 都应当
			 * 已经设置。
			 */
			ereport(LOG,
					(errmsg("certificate authentication failed for user \"%s\": unable to retrieve subject DN",
							port->user_name)));
			return STATUS_ERROR;
		}

		set_authn_id(port, port->peer_dn);
	}

	/* 只把证书的 cn/dn 传给用户映射检查 */
	status_check_usermap = check_usermap(port->hba->usermap, port->user_name, peer_username, false);
	if (status_check_usermap != STATUS_OK)
	{
		/*
		 * 若指定了 clientcert=verify-full，且认证方法
		 * 不是 uaCert，就记录拒绝该
		 * 认证的原因。
		 */
		if (port->hba->clientcert == clientCertFull && port->hba->auth_method != uaCert)
		{
			switch (port->hba->clientcertname)
			{
				case clientCertDN:
					ereport(LOG,
							(errmsg("certificate validation (clientcert=verify-full) failed for user \"%s\": DN mismatch",
									port->user_name)));
					break;
				case clientCertCN:
					ereport(LOG,
							(errmsg("certificate validation (clientcert=verify-full) failed for user \"%s\": CN mismatch",
									port->user_name)));
			}
		}
	}
	return status_check_usermap;
}
#endif


/*----------------------------------------------------------------
 * RADIUS authentication
 *----------------------------------------------------------------
 */

/*
 * RADIUS 认证在 RFC2865（以及其他几份文档）中描述。
 */

#define RADIUS_VECTOR_LENGTH 16
#define RADIUS_HEADER_LENGTH 20
#define RADIUS_MAX_PASSWORD_LENGTH 128

/* 我们将创建或接受的 RADIUS 报文的最大长度 */
#define RADIUS_BUFFER_SIZE 1024

typedef struct
{
	uint8		attribute;
	uint8		length;
	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} radius_attribute;

typedef struct
{
	uint8		code;
	uint8		id;
	uint16		length;
	uint8		vector[RADIUS_VECTOR_LENGTH];
	/* 这比严格所需略长： */
	char		pad[RADIUS_BUFFER_SIZE - RADIUS_VECTOR_LENGTH];
} radius_packet;

/* RADIUS 报文类型 */
#define RADIUS_ACCESS_REQUEST	1
#define RADIUS_ACCESS_ACCEPT	2
#define RADIUS_ACCESS_REJECT	3

/* RADIUS 属性 */
#define RADIUS_USER_NAME		1
#define RADIUS_PASSWORD			2
#define RADIUS_SERVICE_TYPE		6
#define RADIUS_NAS_IDENTIFIER	32

/* RADIUS 服务类型 */
#define RADIUS_AUTHENTICATE_ONLY	8

/* 等待的秒数。XXX：应当放进配置变量！ */
#define RADIUS_TIMEOUT 3

static void
radius_add_attribute(radius_packet *packet, uint8 type, const unsigned char *data, int len)
{
	radius_attribute *attr;

	if (packet->length + len > RADIUS_BUFFER_SIZE)
	{
		/*
		 * 以哪怕稍微真实一点的数据，这绝不会发生。但仍要抓住它，
		 * 以确保我们不会让缓冲区越界。我们将跳过添加
		 * 这个损坏的属性，这最终会导致认证
		 * 失败。
		 */
		elog(WARNING,
			 "adding attribute code %d with length %d to radius packet would create oversize packet, ignoring",
			 type, len);
		return;
	}

	attr = (radius_attribute *) ((unsigned char *) packet + packet->length);
	attr->attribute = type;
	attr->length = len + 2;		/* 总长度包括类型和长度 */
	memcpy(attr->data, data, len);
	packet->length += attr->length;
}

static int
CheckRADIUSAuth(Port *port)
{
	char	   *passwd;
	ListCell   *server,
			   *secrets,
			   *radiusports,
			   *identifiers;

	/* 确保结构体对齐正确 */
	Assert(offsetof(radius_packet, vector) == 4);

	/* 校验参数 */
	if (port->hba->radiusservers == NIL)
	{
		ereport(LOG,
				(errmsg("RADIUS server not specified")));
		return STATUS_ERROR;
	}

	if (port->hba->radiussecrets == NIL)
	{
		ereport(LOG,
				(errmsg("RADIUS secret not specified")));
		return STATUS_ERROR;
	}

	/* 向客户端发送普通口令请求，并取得响应 */
	sendAuthRequest(port, AUTH_REQ_PASSWORD, NULL, 0);

	passwd = recv_password_packet(port);
	if (passwd == NULL)
		return STATUS_EOF;		/* 客户端不会发送口令 */

	if (strlen(passwd) > RADIUS_MAX_PASSWORD_LENGTH)
	{
		ereport(LOG,
				(errmsg("RADIUS authentication does not support passwords longer than %d characters", RADIUS_MAX_PASSWORD_LENGTH)));
		pfree(passwd);
		return STATUS_ERROR;
	}

	/*
	 * 按顺序循环尝试每一台服务器。
	 */
	secrets = list_head(port->hba->radiussecrets);
	radiusports = list_head(port->hba->radiusports);
	identifiers = list_head(port->hba->radiusidentifiers);
	foreach(server, port->hba->radiusservers)
	{
		int			ret = PerformRadiusTransaction(lfirst(server),
												   lfirst(secrets),
												   radiusports ? lfirst(radiusports) : NULL,
												   identifiers ? lfirst(identifiers) : NULL,
												   port->user_name,
												   passwd);

		/*------
		 * STATUS_OK = Login OK
		 * STATUS_ERROR = Login not OK, but try next server
		 * STATUS_EOF = Login not OK, and don't try next server
		 *------
		 */
		if (ret == STATUS_OK)
		{
			set_authn_id(port, port->user_name);

			pfree(passwd);
			return STATUS_OK;
		}
		else if (ret == STATUS_EOF)
		{
			pfree(passwd);
			return STATUS_ERROR;
		}

		/*
		 * 密钥、端口和标识符的长度要么是 0（用默认值），
		 * 要么是 1（各处用同一个），要么与服务器数量相同。
		 * 因此若长度 > 1，我们就前进一步。其他情况下不前进，
		 * 随后会复用正确的值。
		 */
		if (list_length(port->hba->radiussecrets) > 1)
			secrets = lnext(port->hba->radiussecrets, secrets);
		if (list_length(port->hba->radiusports) > 1)
			radiusports = lnext(port->hba->radiusports, radiusports);
		if (list_length(port->hba->radiusidentifiers) > 1)
			identifiers = lnext(port->hba->radiusidentifiers, identifiers);
	}

	/* 没有剩下的服务器可试，因此放弃 */
	pfree(passwd);
	return STATUS_ERROR;
}

static int
PerformRadiusTransaction(const char *server, const char *secret, const char *portstr, const char *identifier, const char *user_name, const char *passwd)
{
	radius_packet radius_send_pack;
	radius_packet radius_recv_pack;
	radius_packet *packet = &radius_send_pack;
	radius_packet *receivepacket = &radius_recv_pack;
	char	   *radius_buffer = (char *) &radius_send_pack;
	char	   *receive_buffer = (char *) &radius_recv_pack;
	int32		service = pg_hton32(RADIUS_AUTHENTICATE_ONLY);
	uint8	   *cryptvector;
	int			encryptedpasswordlen;
	uint8		encryptedpassword[RADIUS_MAX_PASSWORD_LENGTH];
	uint8	   *md5trailer;
	int			packetlength;
	pgsocket	sock;

	struct sockaddr_in6 localaddr;
	struct sockaddr_in6 remoteaddr;
	struct addrinfo hint;
	struct addrinfo *serveraddrs;
	int			port;
	socklen_t	addrsize;
	fd_set		fdset;
	struct timeval endtime;
	int			i,
				j,
				r;

	/* 赋予默认值 */
	if (portstr == NULL)
		portstr = "1812";
	if (identifier == NULL)
		identifier = "postgresql";

	MemSet(&hint, 0, sizeof(hint));
	hint.ai_socktype = SOCK_DGRAM;
	hint.ai_family = AF_UNSPEC;
	port = atoi(portstr);

	r = pg_getaddrinfo_all(server, portstr, &hint, &serveraddrs);
	if (r || !serveraddrs)
	{
		ereport(LOG,
				(errmsg("could not translate RADIUS server name \"%s\" to address: %s",
						server, gai_strerror(r))));
		if (serveraddrs)
			pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}
	/* XXX：要支持多个返回地址吗？ */

	/* 构造 RADIUS 报文 */
	packet->code = RADIUS_ACCESS_REQUEST;
	packet->length = RADIUS_HEADER_LENGTH;
	if (!pg_strong_random(packet->vector, RADIUS_VECTOR_LENGTH))
	{
		ereport(LOG,
				(errmsg("could not generate random encryption vector")));
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}
	packet->id = packet->vector[0];
	radius_add_attribute(packet, RADIUS_SERVICE_TYPE, (const unsigned char *) &service, sizeof(service));
	radius_add_attribute(packet, RADIUS_USER_NAME, (const unsigned char *) user_name, strlen(user_name));
	radius_add_attribute(packet, RADIUS_NAS_IDENTIFIER, (const unsigned char *) identifier, strlen(identifier));

	/*
	 * RADIUS 口令属性这样计算：第一组 16 个八位组为 e[0] = p[0] XOR
	 * MD5(secret + Request Authenticator)，
	 * 随后的各组则为：e[i] = p[i] XOR MD5(secret + e[i-1])
	 * （若需要）
	 */
	encryptedpasswordlen = ((strlen(passwd) + RADIUS_VECTOR_LENGTH - 1) / RADIUS_VECTOR_LENGTH) * RADIUS_VECTOR_LENGTH;
	cryptvector = palloc(strlen(secret) + RADIUS_VECTOR_LENGTH);
	memcpy(cryptvector, secret, strlen(secret));

	/* 第一次迭代使用 Request Authenticator 向量 */
	md5trailer = packet->vector;
	for (i = 0; i < encryptedpasswordlen; i += RADIUS_VECTOR_LENGTH)
	{
		const char *errstr = NULL;

		memcpy(cryptvector + strlen(secret), md5trailer, RADIUS_VECTOR_LENGTH);

		/*
		 * ……随后的迭代则使用上一次 XOR 的结果
		 * （在下面计算）
		 */
		md5trailer = encryptedpassword + i;

		if (!pg_md5_binary(cryptvector, strlen(secret) + RADIUS_VECTOR_LENGTH,
						   encryptedpassword + i, &errstr))
		{
			ereport(LOG,
					(errmsg("could not perform MD5 encryption of password: %s",
							errstr)));
			pfree(cryptvector);
			pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
			return STATUS_ERROR;
		}

		for (j = i; j < i + RADIUS_VECTOR_LENGTH; j++)
		{
			if (j < strlen(passwd))
				encryptedpassword[j] = passwd[j] ^ encryptedpassword[j];
			else
				encryptedpassword[j] = '\0' ^ encryptedpassword[j];
		}
	}
	pfree(cryptvector);

	radius_add_attribute(packet, RADIUS_PASSWORD, encryptedpassword, encryptedpasswordlen);

	/* 在线路上，长度需要用网络字节序 */
	packetlength = packet->length;
	packet->length = pg_hton16(packet->length);

	sock = socket(serveraddrs[0].ai_family, SOCK_DGRAM, 0);
	if (sock == PGINVALID_SOCKET)
	{
		ereport(LOG,
				(errmsg("could not create RADIUS socket: %m")));
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	memset(&localaddr, 0, sizeof(localaddr));
	localaddr.sin6_family = serveraddrs[0].ai_family;
	localaddr.sin6_addr = in6addr_any;
	if (localaddr.sin6_family == AF_INET6)
		addrsize = sizeof(struct sockaddr_in6);
	else
		addrsize = sizeof(struct sockaddr_in);

	if (bind(sock, (struct sockaddr *) &localaddr, addrsize))
	{
		ereport(LOG,
				(errmsg("could not bind local RADIUS socket: %m")));
		closesocket(sock);
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	if (sendto(sock, radius_buffer, packetlength, 0,
			   serveraddrs[0].ai_addr, serveraddrs[0].ai_addrlen) < 0)
	{
		ereport(LOG,
				(errmsg("could not send RADIUS packet: %m")));
		closesocket(sock);
		pg_freeaddrinfo_all(hint.ai_family, serveraddrs);
		return STATUS_ERROR;
	}

	/* 不再需要服务器地址 */
	pg_freeaddrinfo_all(hint.ai_family, serveraddrs);

	/*
	 * 算出我们应当在什么时刻超时。我们无法只做一次
	 * 带超时的 select() 调用，因为可能有人向我们的端口
	 * 发送无效报文，从而使我们在循环里重试，永远
	 * 不会超时。
	 *
	 * XXX：若锁存器已置位，使用 WaitLatchOrSocket() 并执行 CHECK_FOR_INTERRUPTS()，
	 * 会改善对超时和取消的
	 * 响应能力。
	 */
	gettimeofday(&endtime, NULL);
	endtime.tv_sec += RADIUS_TIMEOUT;

	while (true)
	{
		struct timeval timeout;
		struct timeval now;
		int64		timeoutval;
		const char *errstr = NULL;

		gettimeofday(&now, NULL);
		timeoutval = (endtime.tv_sec * 1000000 + endtime.tv_usec) - (now.tv_sec * 1000000 + now.tv_usec);
		if (timeoutval <= 0)
		{
			ereport(LOG,
					(errmsg("timeout waiting for RADIUS response from %s",
							server)));
			closesocket(sock);
			return STATUS_ERROR;
		}
		timeout.tv_sec = timeoutval / 1000000;
		timeout.tv_usec = timeoutval % 1000000;

		FD_ZERO(&fdset);
		FD_SET(sock, &fdset);

		r = select(sock + 1, &fdset, NULL, NULL, &timeout);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;

			/* 其他任何情况都是真正的错误 */
			ereport(LOG,
					(errmsg("could not check status on RADIUS socket: %m")));
			closesocket(sock);
			return STATUS_ERROR;
		}
		if (r == 0)
		{
			ereport(LOG,
					(errmsg("timeout waiting for RADIUS response from %s",
							server)));
			closesocket(sock);
			return STATUS_ERROR;
		}

		/*
		 * 尝试读取响应报文，并校验其内容。
		 *
		 * 任何实际上不是 RADIUS 报文的包，或者并未被验证为
		 * 一条显式拒绝的包，都直接忽略，我们重试
		 * 下一个报文（直到超时）。这是为了避免
		 * 有人向我们期待 RADIUS 响应的端口洪泛无效报文，
		 * 从而对该登录造成拒绝服务
		 * 的可能。
		 */

		addrsize = sizeof(remoteaddr);
		packetlength = recvfrom(sock, receive_buffer, RADIUS_BUFFER_SIZE, 0,
								(struct sockaddr *) &remoteaddr, &addrsize);
		if (packetlength < 0)
		{
			ereport(LOG,
					(errmsg("could not read RADIUS response: %m")));
			closesocket(sock);
			return STATUS_ERROR;
		}

		if (remoteaddr.sin6_port != pg_hton16(port))
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s was sent from incorrect port: %d",
							server, pg_ntoh16(remoteaddr.sin6_port))));
			continue;
		}

		if (packetlength < RADIUS_HEADER_LENGTH)
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s too short: %d", server, packetlength)));
			continue;
		}

		if (packetlength != pg_ntoh16(receivepacket->length))
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s has corrupt length: %d (actual length %d)",
							server, pg_ntoh16(receivepacket->length), packetlength)));
			continue;
		}

		if (packet->id != receivepacket->id)
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s is to a different request: %d (should be %d)",
							server, receivepacket->id, packet->id)));
			continue;
		}

		/*
		 * 校验响应认证器，其计算方式为
		 * MD5(Code+ID+Length+RequestAuthenticator+Attributes+Secret)
		 */
		cryptvector = palloc(packetlength + strlen(secret));

		memcpy(cryptvector, receivepacket, 4);	/* code+id+length */
		memcpy(cryptvector + 4, packet->vector, RADIUS_VECTOR_LENGTH);	/* 请求
																		 * 认证器，来自
																		 * 原始报文 */
		if (packetlength > RADIUS_HEADER_LENGTH)	/* 可能根本
													 * 没有属性 */
			memcpy(cryptvector + RADIUS_HEADER_LENGTH, receive_buffer + RADIUS_HEADER_LENGTH, packetlength - RADIUS_HEADER_LENGTH);
		memcpy(cryptvector + packetlength, secret, strlen(secret));

		if (!pg_md5_binary(cryptvector,
						   packetlength + strlen(secret),
						   encryptedpassword, &errstr))
		{
			ereport(LOG,
					(errmsg("could not perform MD5 encryption of received packet: %s",
							errstr)));
			pfree(cryptvector);
			continue;
		}
		pfree(cryptvector);

		if (memcmp(receivepacket->vector, encryptedpassword, RADIUS_VECTOR_LENGTH) != 0)
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s has incorrect MD5 signature",
							server)));
			continue;
		}

		if (receivepacket->code == RADIUS_ACCESS_ACCEPT)
		{
			closesocket(sock);
			return STATUS_OK;
		}
		else if (receivepacket->code == RADIUS_ACCESS_REJECT)
		{
			closesocket(sock);
			return STATUS_EOF;
		}
		else
		{
			ereport(LOG,
					(errmsg("RADIUS response from %s has invalid code (%d) for user \"%s\"",
							server, receivepacket->code, user_name)));
			continue;
		}
	}							/* while (true) */
}
