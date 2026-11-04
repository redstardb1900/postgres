/*-------------------------------------------------------------------------
 *
 * fe-secure-common.c
 *
 * common implementation-independent SSL support code
 *
 * While fe-secure.c contains the interfaces that the rest of libpq call, this
 * file contains support routines that are used by the library-specific
 * implementations such as fe-secure-openssl.c.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <arpa/inet.h>

#include "fe-secure-common.h"

#include "libpq-int.h"
#include "pqexpbuffer.h"

/*
 * 检查通配符证书是否与服务器主机名匹配。   
 *
 * 匹配规则如下：
 *	1. 仅将 '*' 字符视为通配符           
 *	2. 仅匹配字符串开头的通配符              
 *	3. '*' 字符*不*匹配 '.'，即仅匹配                          
 *	   单个路径名组件。    
 *	4. 单个模式不支持多个 '*'。                     
 *
 * 这大致符合 RFC2818，但与大多数浏览器的实现不同       
 * （差异在于第 3 点）                             
 *
 * 由于 DNS 不区分大小写，匹配始终不区分大小写。  
 */
static bool
wildcard_certificate_match(const char *pattern, const char *string)
{
	int			lenpat = strlen(pattern);
	int			lenstr = strlen(string);

	/* 如果不以通配符开头，则不匹配（规则 1 和 2）   */
	if (lenpat < 3 ||
		pattern[0] != '*' ||
		pattern[1] != '.')
		return false;

	/* 如果模式比字符串长，则永远无法匹配      */
	if (lenpat > lenstr)
		return false;

	/*
	 * 如果字符串结尾并非模式（除去通配符），则不匹配 
	 */
	if (pg_strcasecmp(pattern + 1, string + lenstr - lenpat + 1) != 0)
		return false;

	/*
	 * 如果在模式匹配起始位置左侧有点，则不匹配          
	 * （规则 3）
	 */
	if (strchr(string, '.') < string + lenstr - lenpat)
		return false;

	/* 字符串以模式结尾且之前没有点，因此匹配            */
	return true;
}

/*
 * 检查服务器证书中的名称是否与对等方的主机名匹配。
 *
 * 如果名称匹配则返回 1，否则返回 0。出错时返回      
 * -1，并设置 libpq 错误消息。 
 *
 * 从证书中提取的名称通过 *store_name 返回。                
 * 调用者负责释放它。          
 */
int
pq_verify_peer_name_matches_certificate_name(PGconn *conn,
											 const char *namedata, size_t namelen,
											 char **store_name)
{
	char	   *name;
	int			result;
	char	   *host = conn->connhost[conn->whichhost].host;

	*store_name = NULL;

	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return -1;
	}

	/*
	 * 无法保证证书返回的字符串以 NULL 结尾，           
	 * 因此创建以 NULL 结尾的副本。 
	 */
	name = malloc(namelen + 1);
	if (name == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return -1;
	}
	memcpy(name, namedata, namelen);
	name[namelen] = '\0';

	/*
	 * 拒绝证书通用名或备用名中的嵌入式 NULLs，         
	 * 以防 CVE-2009-4034 类攻击。  
	 */
	if (namelen != strlen(name))
	{
		free(name);
		libpq_append_conn_error(conn, "SSL certificate's name contains embedded null");
		return -1;
	}

	if (pg_strcasecmp(name, host) == 0)
	{
		/* 精确匹配     */
		result = 1;
	}
	else if (wildcard_certificate_match(name, host))
	{
		/* 通配符名称匹配 */
		result = 1;
	}
	else
	{
		result = 0;
	}

	*store_name = name;
	return result;
}

/*
 * 检查服务器证书的 IP 地址是否与对等方主机名匹配  
 * （主机名本身须为 IPv4/6 地址）。      
 *
 * 如果地址匹配则返回 1，否则返回 0。出错时返回         
 * -1，并设置 libpq 错误消息。 
 *
 * 证书 IP 地址的字符串表示通过 *store_name 返回。        
 * 调用者负责释放它。                           
 */
int
pq_verify_peer_name_matches_certificate_ip(PGconn *conn,
										   const unsigned char *ipdata,
										   size_t iplen,
										   char **store_name)
{
	char	   *addrstr;
	int			match = 0;
	char	   *host = conn->connhost[conn->whichhost].host;
	int			family;
	char		tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];
	char		sebuf[PG_STRERROR_R_BUFLEN];

	*store_name = NULL;

	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return -1;
	}

	/*
	 * 证书中的数据为网络字节序。为进行比较，          
	 * 也将我们的主机字符串转换为网络序字节。              
	 * （主机字符串并不保证确实是 IP 地址，所以若此
	 * 转换失败，则应视为不匹配而非错误。）           
	 *        
	 */
	if (iplen == 4)
	{
		/* IPv4 */
		struct in_addr addr;

		family = AF_INET;

		/*
		 * 故意使用 inet_aton()；我们接受 inet_aton() 接受的   
		 * 但 inet_pton() 不接受的替代 IPv4 地址表示法   
		 * 作为服务器地址。        
		 */
		if (inet_aton(host, &addr))
		{
			if (memcmp(ipdata, &addr.s_addr, iplen) == 0)
				match = 1;
		}
	}

	/*
	 * 如果没有 inet_pton()，跳过此步骤。此时，证书中的    
	 * IPv6 地址将导致错误。   
	 */
#ifdef HAVE_INET_PTON
	else if (iplen == 16)
	{
		/* IPv6 */
		struct in6_addr addr;

		family = AF_INET6;

		if (inet_pton(AF_INET6, host, &addr) == 1)
		{
			if (memcmp(ipdata, &addr.s6_addr, iplen) == 0)
				match = 1;
		}
	}
#endif
	else
	{
		/*
		 * 非 IPv4 或 IPv6。此字段本可忽略，但鉴于主题， 
		 * 宽松处理似有不妥。    
		 */
		libpq_append_conn_error(conn, "certificate contains IP address with invalid length %zu",
								iplen);
		return -1;
	}

	/* 生成证书 IP 的可读表示。                                */
	addrstr = pg_inet_net_ntop(family, ipdata, 8 * iplen, tmp, sizeof(tmp));
	if (!addrstr)
	{
		libpq_append_conn_error(conn, "could not convert certificate's IP address to string: %s",
								strerror_r(errno, sebuf, sizeof(sebuf)));
		return -1;
	}

	*store_name = strdup(addrstr);
	return match;
}

/*
 * 验证服务器证书是否与我们连接的主机名匹配。         
 *
 * 考虑证书的通用名称和主题备用名称。                        
 */
bool
pq_verify_peer_name_matches_certificate(PGconn *conn)
{
	char	   *host = conn->connhost[conn->whichhost].host;
	int			rc;
	int			names_examined = 0;
	char	   *first_name = NULL;

	/*
	 * 如果被告知不验证对等方名称，则不执行。    
	 * 返回 true 表示验证成功。               
	 */
	if (strcmp(conn->sslmode, "verify-full") != 0)
		return true;

	/* 检查是否有主机名可供比较。        */
	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified for a verified SSL connection");
		return false;
	}

	rc = pgtls_verify_peer_name_matches_certificate_guts(conn, &names_examined, &first_name);

	if (rc == 0)
	{
		/*
		 * 不匹配。在错误消息中包含服务器证书中的名称， 
		 * 以辅助调试损坏的配置。如果有多个名称，    
		 * 仅打印第一个以免错误消息                            
		 * 过长。     
		 */
		if (names_examined > 1)
		{
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_ngettext("server certificate for \"%s\" (and %d other name) does not match host name \"%s\"",
											 "server certificate for \"%s\" (and %d other names) does not match host name \"%s\"",
											 names_examined - 1),
							  first_name, names_examined - 1, host);
			appendPQExpBufferChar(&conn->errorMessage, '\n');
		}
		else if (names_examined == 1)
		{
			libpq_append_conn_error(conn, "server certificate for \"%s\" does not match host name \"%s\"",
									first_name, host);
		}
		else
		{
			libpq_append_conn_error(conn, "could not get server's host name from server certificate");
		}
	}

	/* 清理   */
	free(first_name);

	return (rc == 1);
}
