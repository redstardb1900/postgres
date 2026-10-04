/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * src/include/libpq/hba.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "libpq/pqcomm.h"	/* pgrminclude ignore */	/* NetBSD 需要 */
#include "nodes/pg_list.h"
#include "regex/regex.h"


/*
 * 下面的枚举表示 PostgreSQL 支持的认证方法。
 *
 * 注意：与 hba.c 中的 UserAuthName 数组保持同步。
 *
 */
typedef enum UserAuth
{
	uaReject,
	uaImplicitReject,			/* 不是用户可见的选项 */
	uaTrust,
	uaIdent,
	uaPassword,
	uaMD5,
	uaSCRAM,
	uaGSS,
	uaSSPI,
	uaPAM,
	uaBSD,
	uaLDAP,
	uaCert,
	uaRADIUS,
	uaPeer
#define USER_AUTH_LAST uaPeer	/* 必须是该枚举的最后一个值 */
} UserAuth;

/*
 * 表示 pg_hba.conf 条目的数据结构
 */

typedef enum IPCompareMethod
{
	ipCmpMask,
	ipCmpSameHost,
	ipCmpSameNet,
	ipCmpAll
} IPCompareMethod;

typedef enum ConnType
{
	ctLocal,
	ctHost,
	ctHostSSL,
	ctHostNoSSL,
	ctHostGSS,
	ctHostNoGSS,
} ConnType;

typedef enum ClientCertMode
{
	clientCertOff,
	clientCertCA,
	clientCertFull
} ClientCertMode;

typedef enum ClientCertName
{
	clientCertCN,
	clientCertDN
} ClientCertName;

/*
 * 从认证配置文件（pg_ident.conf 或 pg_hba.conf）词法分析
 * 得到的单个字符串词元，以及该词元是否被加了引号。
 * 若 "string" 以斜杠开头，它可以选择包含一个正则表达式
 * （目前在构造 IdentLines 时用于 pg_ident.conf，在构造
 * HbaLines 时用于 pg_hba.conf）。
 */
typedef struct AuthToken
{
	char	   *string;
	bool		quoted;
	regex_t    *regex;
} AuthToken;

typedef struct HbaLine
{
	char	   *sourcefile;
	int			linenumber;
	char	   *rawline;
	ConnType	conntype;
	List	   *databases;
	List	   *roles;
	struct sockaddr_storage addr;
	int			addrlen;		/* 若没有有效地址则为零 */
	struct sockaddr_storage mask;
	int			masklen;		/* 若没有有效掩码则为零 */
	IPCompareMethod ip_cmp_method;
	char	   *hostname;
	UserAuth	auth_method;
	char	   *usermap;
	char	   *pamservice;
	bool		pam_use_hostname;
	bool		ldaptls;
	char	   *ldapscheme;
	char	   *ldapserver;
	int			ldapport;
	char	   *ldapbinddn;
	char	   *ldapbindpasswd;
	char	   *ldapsearchattribute;
	char	   *ldapsearchfilter;
	char	   *ldapbasedn;
	int			ldapscope;
	char	   *ldapprefix;
	char	   *ldapsuffix;
	ClientCertMode clientcert;
	ClientCertName clientcertname;
	char	   *krb_realm;
	bool		include_realm;
	bool		compat_realm;
	bool		upn_username;
	List	   *radiusservers;
	char	   *radiusservers_s;
	List	   *radiussecrets;
	char	   *radiussecrets_s;
	List	   *radiusidentifiers;
	char	   *radiusidentifiers_s;
	List	   *radiusports;
	char	   *radiusports_s;
} HbaLine;

typedef struct IdentLine
{
	int			linenumber;

	char	   *usermap;
	AuthToken  *system_user;
	AuthToken  *pg_user;
} IdentLine;

/*
 * TokenizedAuthLine 表示从认证配置文件词法分析出的一行。
 * "fields" 列表中的每一项都是 AuthTokens 的子列表。空行或
 * 整行都是注释的行不会发出 TokenizedAuthLine，因此 "fields"
 * 从不为 NIL（其子列表也都不是）。
 *
 * 例外：若词法分析期间出错，fields 可能为 NIL，此时
 * err_msg 不为 NULL。
 */
typedef struct TokenizedAuthLine
{
	List	   *fields;			/* AuthTokens 列表的列表 */
	char	   *file_name;		/* 来源文件名 */
	int			line_num;		/* 行号 */
	char	   *raw_line;		/* 原始行文本 */
	char	   *err_msg;		/* 若有错误则是错误消息 */
} TokenizedAuthLine;

/* 避免在此包含 libpq/libpq-be.h 的权宜之计 */
typedef struct Port hbaPort;

extern bool load_hba(void);
extern bool load_ident(void);
extern const char *hba_authname(UserAuth auth_method);
extern void hba_getauthmethod(hbaPort *port);
extern int	check_usermap(const char *usermap_name,
						  const char *pg_user, const char *system_user,
						  bool case_insensitive);
extern HbaLine *parse_hba_line(TokenizedAuthLine *tok_line, int elevel);
extern IdentLine *parse_ident_line(TokenizedAuthLine *tok_line, int elevel);
extern bool pg_isblank(const char c);
extern FILE *open_auth_file(const char *filename, int elevel, int depth,
							char **err_msg);
extern void free_auth_file(FILE *file, int depth);
extern void tokenize_auth_file(const char *filename, FILE *file,
							   List **tok_lines, int elevel, int depth);

#endif							/* HBA_H */
