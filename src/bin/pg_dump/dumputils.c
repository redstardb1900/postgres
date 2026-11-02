/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/dumputils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "dumputils.h"
#include "fe_utils/string_utils.h"

static const char restrict_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static bool parseAclItem(const char *item, const char *type,
						 const char *name, const char *subname, int remoteVersion,
						 PQExpBuffer grantee, PQExpBuffer grantor,
						 PQExpBuffer privs, PQExpBuffer privswgo);
static char *dequoteAclUserName(PQExpBuffer output, char *input);
static void AddAcl(PQExpBuffer aclbuf, const char *keyword,
				   const char *subname);


/*
 * 净化字符串，使其可以安全地包含在 SQL 注释或 TOC 列表中，通过将所有
 * 换行符替换为空格来实现。这确保每个逻辑输出行实际上就是一个物理输出行，
 * 从而防止导出数据损坏（最坏情况下，如果某些人贸然加载一个包含
 * 恶意构造名称的对象的导出文件，可能会出现 SQL 注入漏洞）。
 *
 * 返回值是 malloc 分配的新字符串。如果输入字符串为 NULL，
 * 则返回 malloc 分配的空字符串，除非 want_hyphen 为真，在此情况下返回
 * malloc 分配的连字符。
 *
 * 注意，当前我们并未费心去引用名称，这意味着名称字段不会自动可解析。
 * "pg_restore -L" 不关心这一点，因为它只检查 dumpId 字段，
 * 但将来我们可能想要在这方面做得更好。
 *
 *
 */
char *
sanitize_line(const char *str, bool want_hyphen)
{
	char	   *result;
	char	   *s;

	if (!str)
		return pg_strdup(want_hyphen ? "-" : "");

	result = pg_strdup(str);

	for (s = result; *s != '\0'; s++)
	{
		if (*s == '\n' || *s == '\r')
			*s = ' ';
	}

	return result;
}


/*
 * 为对象构建 GRANT/REVOKE 命令。
 *
 *	name: 对象名称，格式适用于命令中（已引用）
 *	subname: 子对象名称（如有，已引用）；无则为 NULL
 *	nspname: 对象所在的命名空间（无则为 NULL）；未预先引用
 *	type: 对象类型（见 GRANT 命令中的格式：必须为以下之一：
 *		TABLE、SEQUENCE、FUNCTION、PROCEDURE、LANGUAGE、SCHEMA、DATABASE、TABLESPACE、
 *		FOREIGN DATA WRAPPER、SERVER、PARAMETER 或 LARGE OBJECT）
 *	acls: 从数据库获取的 ACL 字符串
 *	baseacls: 此对象的初始 ACL 字符串
 *	owner: 对象所有者的用户名（将通过 fmtId 处理）；可以为 NULL
 *		或空字符串表示"未知所有者"
 *	prefix: 每个生成命令前添加的字符串前缀；通常为空
 *	remoteVersion: 数据库版本
 *
 * 成功返回 true，无法解析 acl 字符串则返回 false。
 * 生成的命令（如有）将追加到 'sql' 的内容之后。
 *
 * baseacls 通常是该对象类型的 acldefault() 的结果。
 * 然而，如果对象有 pg_init_privs 条目，则应改用该 initprivs ACL。
 * 当 acls 本身是 pg_init_privs 条目时，baseacls 是相对于它导出的基础；
 * 此时它可以是 acldefault() 值或空 ACL "{}"。
 *
 * 注意：处理默认 ACL 时，prefix 是 "ALTER DEFAULT PRIVILEGES " 或类似内容，
 * name 为空字符串。
 *
 * 注意：谨防将 fmtId() 的结果直接作为 'name' 或 'subname' 传入，
 * 因为此例程内部也使用了 fmtId()。
 *
 */
bool
buildACLCommands(const char *name, const char *subname, const char *nspname,
				 const char *type, const char *acls, const char *baseacls,
				 const char *owner, const char *prefix, int remoteVersion,
				 PQExpBuffer sql)
{
	bool		ok = true;
	char	  **aclitems = NULL;
	char	  **baseitems = NULL;
	char	  **grantitems = NULL;
	char	  **revokeitems = NULL;
	int			naclitems = 0;
	int			nbaseitems = 0;
	int			ngrantitems = 0;
	int			nrevokeitems = 0;
	int			i;
	PQExpBuffer grantee,
				grantor,
				privs,
				privswgo;
	PQExpBuffer firstsql,
				secondsql;

	/*
	 * 如果 acl 为 NULL（初始默认状态），我们无需做任何事。注意，
	 * 这与所有权限被撤销的情况不同，后者看起来像空数组（"{}"）。
	 *
	 */
	if (acls == NULL || *acls == '\0')
		return true;			/* 对象具有默认权限 */

	/* 将空字符串所有者视为 NULL */
	if (owner && *owner == '\0')
		owner = NULL;

	/* 解析 acls 数组 */
	if (!parsePGArray(acls, &aclitems, &naclitems))
	{
		free(aclitems);
		return false;
	}

	/* 同时解析 baseacls */
	if (!parsePGArray(baseacls, &baseitems, &nbaseitems))
	{
		free(aclitems);
		free(baseitems);
		return false;
	}

	/*
	 * 将实际 ACL 与基础 ACL 进行比较，提取需要授予的权限
	 * （即在实际 ACL 中但不在基础 ACL 中）和需要撤销的权限
	 * （与之相反）。我们使用普通的字符串比较来检查匹配。
	 * 原则上，这可能会被空格等无关问题愚弄，但由于所有这些字符串
	 * 都是 aclitemout() 的工作成果，在实践中应该没问题。
	 * 此外，错误的失配只是会让输出比实际需要的稍微冗长一些。
	 *
	 *
	 */
	grantitems = (char **) pg_malloc(naclitems * sizeof(char *));
	for (i = 0; i < naclitems; i++)
	{
		bool		found = false;

		for (int j = 0; j < nbaseitems; j++)
		{
			if (strcmp(aclitems[i], baseitems[j]) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			grantitems[ngrantitems++] = aclitems[i];
	}
	revokeitems = (char **) pg_malloc(nbaseitems * sizeof(char *));
	for (i = 0; i < nbaseitems; i++)
	{
		bool		found = false;

		for (int j = 0; j < naclitems; j++)
		{
			if (strcmp(baseitems[i], aclitems[j]) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			revokeitems[nrevokeitems++] = baseitems[i];
	}

	/* 准备工作缓冲区 */
	grantee = createPQExpBuffer();
	grantor = createPQExpBuffer();
	privs = createPQExpBuffer();
	privswgo = createPQExpBuffer();

	/*
	 * 最后，以下两个缓冲区会被拼接在一起形成结果。
	 */
	firstsql = createPQExpBuffer();
	secondsql = createPQExpBuffer();

	/*
	 * 为 revokeitems[] 中列出的 ACL 构建 REVOKE 语句。
	 */
	for (i = 0; i < nrevokeitems; i++)
	{
		if (!parseAclItem(revokeitems[i],
						  type, name, subname, remoteVersion,
						  grantee, grantor, privs, NULL))
		{
			ok = false;
			break;
		}

		if (privs->len > 0)
		{
			appendPQExpBuffer(firstsql, "%sREVOKE %s ON %s ",
							  prefix, privs->data, type);
			if (nspname && *nspname)
				appendPQExpBuffer(firstsql, "%s.", fmtId(nspname));
			if (name && *name)
				appendPQExpBuffer(firstsql, "%s ", name);
			appendPQExpBufferStr(firstsql, "FROM ");
			if (grantee->len == 0)
				appendPQExpBufferStr(firstsql, "PUBLIC;\n");
			else
				appendPQExpBuffer(firstsql, "%s;\n",
								  fmtId(grantee->data));
		}
	}

	/*
	 * 此时我们已为所有不再在对象上存在的初始和默认权限
	 * 发出了 REVOKE 语句，因此我们几乎准备好 GRANT
	 * grantitems[] 中列出的权限。
	 *
	 * 不过我们仍需做一些处理来应对新版本中新增默认公共权限的情况：
	 * REVOKE ALL 会撤销它们，导致行为与旧版本不同，
	 * 而这通常不是我们想要的。因此，如果源数据库版本太旧，
	 * 我们没有特定权限，则需要重新加回默认权限。
	 * （截至目前，支持的版本中不存在此类情况。）
	 *
	 */

	/*
	 * 扫描待授予的单个 ACL 条目。
	 *
	 * 权限在 ACL 字符串中出现的顺序（即它们被 GRANT 的顺序，
	 * 由后端维护）必须保留，以确保 WITH GRANT OPTION 的 GRANT
	 * 和基于这些的后续 GRANT 以正确的顺序导出。
	 * 然而，某些旧服务器版本会在所有者的授予之前显示对 PUBLIC 的授予；
	 * 为了一致性，强制先输出所有者的授予。
	 *
	 */
	for (i = 0; i < ngrantitems; i++)
	{
		if (parseAclItem(grantitems[i], type, name, subname, remoteVersion,
						 grantee, grantor, privs, privswgo))
		{
			/*
			 * 如果授予者不是所有者，我们需要使用 SET SESSION
			 * AUTHORIZATION 来成为授予者。仅在有实际内容
			 * 需要执行时才发出 SET/RESET。
			 */
			if (privs->len > 0 || privswgo->len > 0)
			{
				PQExpBuffer thissql;

				/* 如果 ACL 中没有显式指定授予者，则将所有者设为授予者 */
				if (grantor->len == 0 && owner)
					printfPQExpBuffer(grantor, "%s", owner);

				/* 确保所有者自身的授予在他人之前输出 */
				if (owner &&
					strcmp(grantee->data, owner) == 0 &&
					strcmp(grantor->data, owner) == 0)
					thissql = firstsql;
				else
					thissql = secondsql;

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBuffer(thissql, "SET SESSION AUTHORIZATION %s;\n",
									  fmtId(grantor->data));

				if (privs->len > 0)
				{
					appendPQExpBuffer(thissql, "%sGRANT %s ON %s ",
									  prefix, privs->data, type);
					if (nspname && *nspname)
						appendPQExpBuffer(thissql, "%s.", fmtId(nspname));
					if (name && *name)
						appendPQExpBuffer(thissql, "%s ", name);
					appendPQExpBufferStr(thissql, "TO ");
					if (grantee->len == 0)
						appendPQExpBufferStr(thissql, "PUBLIC;\n");
					else
						appendPQExpBuffer(thissql, "%s;\n", fmtId(grantee->data));
				}
				if (privswgo->len > 0)
				{
					appendPQExpBuffer(thissql, "%sGRANT %s ON %s ",
									  prefix, privswgo->data, type);
					if (nspname && *nspname)
						appendPQExpBuffer(thissql, "%s.", fmtId(nspname));
					if (name && *name)
						appendPQExpBuffer(thissql, "%s ", name);
					appendPQExpBufferStr(thissql, "TO ");
					if (grantee->len == 0)
						appendPQExpBufferStr(thissql, "PUBLIC");
					else
						appendPQExpBufferStr(thissql, fmtId(grantee->data));
					appendPQExpBufferStr(thissql, " WITH GRANT OPTION;\n");
				}

				if (grantor->len > 0
					&& (!owner || strcmp(owner, grantor->data) != 0))
					appendPQExpBufferStr(thissql, "RESET SESSION AUTHORIZATION;\n");
			}
		}
		else
		{
			/* parseAclItem 失败，放弃 */
			ok = false;
			break;
		}
	}

	destroyPQExpBuffer(grantee);
	destroyPQExpBuffer(grantor);
	destroyPQExpBuffer(privs);
	destroyPQExpBuffer(privswgo);

	appendPQExpBuffer(sql, "%s%s", firstsql->data, secondsql->data);
	destroyPQExpBuffer(firstsql);
	destroyPQExpBuffer(secondsql);

	free(aclitems);
	free(baseitems);
	free(grantitems);
	free(revokeitems);

	return ok;
}

/*
 * 为单个 pg_default_acl 条目构建 ALTER DEFAULT PRIVILEGES 命令。
 *
 *	type: 对象类型（TABLES、FUNCTIONS 等）
 *	nspname: 模式名称，或 NULL 表示全局默认权限
 *	acls: 从数据库获取的 ACL 字符串
 *	acldefault: 适用于该对象类型和所有者的默认 ACL
 *	owner: 权限所有者的用户名（将通过 fmtId 处理）
 *	remoteVersion: 数据库版本
 *
 * 成功返回 true，无法解析 acl 字符串则返回 false。
 * 生成的命令（如有）将追加到 'sql' 的内容之后。
 */
bool
buildDefaultACLCommands(const char *type, const char *nspname,
						const char *acls, const char *acldefault,
						const char *owner,
						int remoteVersion,
						PQExpBuffer sql)
{
	PQExpBuffer prefix;

	prefix = createPQExpBuffer();

	/*
	 * 我们将目标角色直接嵌入到命令中，
	 * 而不是通过 SET ROLE 或类似方式来处理。
	 * 这样权限错误只导致不做任何操作，而不是
	 * 为错误的用户更改默认权限。
	 */
	appendPQExpBuffer(prefix, "ALTER DEFAULT PRIVILEGES FOR ROLE %s ",
					  fmtId(owner));
	if (nspname)
		appendPQExpBuffer(prefix, "IN SCHEMA %s ", fmtId(nspname));

	/*
	 * 默认 ACL 不存在 initprivs，因此基础 ACL
	 * 始终只是特定对象类型的默认值。
	 */
	if (!buildACLCommands("", NULL, NULL, type,
						  acls, acldefault, owner,
						  prefix->data, remoteVersion, sql))
	{
		destroyPQExpBuffer(prefix);
		return false;
	}

	destroyPQExpBuffer(prefix);

	return true;
}

/*
 * 解析 aclitem 字符串，其一般形式为
 *		username=privilegecodes/grantor
 *
 * 成功返回 true，解析错误返回 false。成功时，字符串的组成部分
 * 将通过 PQExpBuffer 参数返回。
 *
 * 返回的 grantee 字符串将是去引用后的用户名，或在授予 PUBLIC
 * 的情况下为空字符串。返回的 grantor 是去引用后的授予者名称。
 * 权限字符被转换为 GRANT/REVOKE 逗号分隔的权限列表。
 * 如果 "privswgo" 非 NULL，结果分别为带授予选项的权限列表
 * （"privswgo"）和不带授予选项的权限列表（"privs"）。
 * 否则，"privs" 承载所有相关权限，忽略授予选项的区分。
 *
 * 注意：为了跨版本兼容性，尽可能使用 ALL 来表示权限集很重要。
 *
 *
 */
static bool
parseAclItem(const char *item, const char *type,
			 const char *name, const char *subname, int remoteVersion,
			 PQExpBuffer grantee, PQExpBuffer grantor,
			 PQExpBuffer privs, PQExpBuffer privswgo)
{
	char	   *buf;
	bool		all_with_go = true;
	bool		all_without_go = true;
	char	   *eqpos;
	char	   *slpos;
	char	   *pos;

	buf = pg_strdup(item);

	/* 用户名或组名是直到 = 的字符串 */
	eqpos = dequoteAclUserName(grantee, buf);
	if (*eqpos != '=')
	{
		pg_free(buf);
		return false;
	}

	/* 授予者应出现在 / 之后 */
	slpos = strchr(eqpos + 1, '/');
	if (slpos)
	{
		*slpos++ = '\0';
		slpos = dequoteAclUserName(grantor, slpos);
		if (*slpos != '\0')
		{
			pg_free(buf);
			return false;
		}
	}
	else
	{
		pg_free(buf);
		return false;
	}

	/* 权限码 */
#define CONVERT_PRIV(code, keywd) \
do { \
	if ((pos = strchr(eqpos + 1, code))) \
	{ \
		if (*(pos + 1) == '*' && privswgo != NULL) \
		{ \
			AddAcl(privswgo, keywd, subname); \
			all_without_go = false; \
		} \
		else \
		{ \
			AddAcl(privs, keywd, subname); \
			all_with_go = false; \
		} \
	} \
	else \
		all_with_go = all_without_go = false; \
} while (0)

	resetPQExpBuffer(privs);
	resetPQExpBuffer(privswgo);

	if (strcmp(type, "TABLE") == 0 || strcmp(type, "SEQUENCE") == 0 ||
		strcmp(type, "TABLES") == 0 || strcmp(type, "SEQUENCES") == 0)
	{
		CONVERT_PRIV('r', "SELECT");

		if (strcmp(type, "SEQUENCE") == 0 ||
			strcmp(type, "SEQUENCES") == 0)
			/* 仅适用于序列 */
			CONVERT_PRIV('U', "USAGE");
		else
		{
			/* 仅适用于表 */
			CONVERT_PRIV('a', "INSERT");
			CONVERT_PRIV('x', "REFERENCES");
			/* 其余权限不适用于列 */
			if (subname == NULL)
			{
				CONVERT_PRIV('d', "DELETE");
				CONVERT_PRIV('t', "TRIGGER");
				CONVERT_PRIV('D', "TRUNCATE");
			}
		}

		/* UPDATE */
		CONVERT_PRIV('w', "UPDATE");
	}
	else if (strcmp(type, "FUNCTION") == 0 ||
			 strcmp(type, "FUNCTIONS") == 0)
		CONVERT_PRIV('X', "EXECUTE");
	else if (strcmp(type, "PROCEDURE") == 0 ||
			 strcmp(type, "PROCEDURES") == 0)
		CONVERT_PRIV('X', "EXECUTE");
	else if (strcmp(type, "LANGUAGE") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "SCHEMA") == 0 ||
			 strcmp(type, "SCHEMAS") == 0)
	{
		CONVERT_PRIV('C', "CREATE");
		CONVERT_PRIV('U', "USAGE");
	}
	else if (strcmp(type, "DATABASE") == 0)
	{
		CONVERT_PRIV('C', "CREATE");
		CONVERT_PRIV('c', "CONNECT");
		CONVERT_PRIV('T', "TEMPORARY");
	}
	else if (strcmp(type, "TABLESPACE") == 0)
		CONVERT_PRIV('C', "CREATE");
	else if (strcmp(type, "TYPE") == 0 ||
			 strcmp(type, "TYPES") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN DATA WRAPPER") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN SERVER") == 0)
		CONVERT_PRIV('U', "USAGE");
	else if (strcmp(type, "FOREIGN TABLE") == 0)
		CONVERT_PRIV('r', "SELECT");
	else if (strcmp(type, "PARAMETER") == 0)
	{
		CONVERT_PRIV('s', "SET");
		CONVERT_PRIV('A', "ALTER SYSTEM");
	}
	else if (strcmp(type, "LARGE OBJECT") == 0)
	{
		CONVERT_PRIV('r', "SELECT");
		CONVERT_PRIV('w', "UPDATE");
	}
	else
		abort();

#undef CONVERT_PRIV

	if (all_with_go)
	{
		resetPQExpBuffer(privs);
		printfPQExpBuffer(privswgo, "ALL");
		if (subname)
			appendPQExpBuffer(privswgo, "(%s)", subname);
	}
	else if (all_without_go)
	{
		resetPQExpBuffer(privswgo);
		printfPQExpBuffer(privs, "ALL");
		if (subname)
			appendPQExpBuffer(privs, "(%s)", subname);
	}

	pg_free(buf);

	return true;
}

/*
 * 将 *input 处的角色名称传输到输出缓冲区，
 * 并按照后端 acl.c 中 putid() 相同的规则添加引用。
 */
void
quoteAclUserName(PQExpBuffer output, const char *input)
{
	const char *src;
	bool		safe = true;

	for (src = input; *src; src++)
	{
		/* 此测试必须与 putid() 的操作匹配 */
		if (!isalnum((unsigned char) *src) && *src != '_')
		{
			safe = false;
			break;
		}
	}
	if (!safe)
		appendPQExpBufferChar(output, '"');
	for (src = input; *src; src++)
	{
		/* 用户名中的双引号字符被编码为 "" */
		if (*src == '"')
			appendPQExpBufferChar(output, '"');
		appendPQExpBufferChar(output, *src);
	}
	if (!safe)
		appendPQExpBufferChar(output, '"');
}

/*
 * 将以 *input 开始的用户名或组名传输到输出缓冲区，
 * 必要时进行去引用。返回一个指向输入名后的指针。
 * 名称以未引用的 '=' 或字符串结束为结束标记。
 * 注意：与 quoteAclUserName() 不同，此函数首先清空输出缓冲区。
 */
static char *
dequoteAclUserName(PQExpBuffer output, char *input)
{
	resetPQExpBuffer(output);

	while (*input && *input != '=')
	{
		/*
		 * 如果用户名未被引用，直接添加到输出缓冲区
		 */
		if (*input != '"')
			appendPQExpBufferChar(output, *input++);
		else
		{
			/* 否则，这是一个被引用的用户名 */
			input++;
			/* 循环直到遇到未转义的引号 */
			while (!(*input == '"' && *(input + 1) != '"'))
			{
				if (*input == '\0')
					return input;	/* 实际上是语法错误... */

				/*
				 * 引用约定是将 " 转义为 ""。保持此代码
				 * 与后端 acl.c 中的 putid() 同步。
				 */
				if (*input == '"' && *(input + 1) == '"')
					input++;
				appendPQExpBufferChar(output, *input++);
			}
			input++;
		}
	}
	return input;
}

/*
 * 将权限关键字追加到关键字列表，必要时插入逗号。
 */
static void
AddAcl(PQExpBuffer aclbuf, const char *keyword, const char *subname)
{
	if (aclbuf->len > 0)
		appendPQExpBufferChar(aclbuf, ',');
	appendPQExpBufferStr(aclbuf, keyword);
	if (subname)
		appendPQExpBuffer(aclbuf, "(%s)", subname);
}


/*
 * buildShSecLabelQuery
 *
 * 构建用于检索共享对象安全标签的查询。
 * 对象通过其 OID 加上所在目录的名称来标识
 * （例如，"pg_database" 用于数据库名称）。
 * 查询被追加到 "sql"。（我们不在此执行查询，以便
 * 保持此文件不依赖任何处理 SQL 错误的假设。）
 */
void
buildShSecLabelQuery(const char *catalog_name, Oid objectId,
					 PQExpBuffer sql)
{
	appendPQExpBuffer(sql,
					  "SELECT provider, label FROM pg_catalog.pg_shseclabel "
					  "WHERE classoid = 'pg_catalog.%s'::pg_catalog.regclass "
					  "AND objoid = '%u'", catalog_name, objectId);
}

/*
 * emitShSecLabels
 *
 * 使用 buildShSecLabelQuery 生成的查询所获取的数据构建
 * SECURITY LABEL 命令，并将其追加到 "buffer"。
 * 此处，目标对象由其类型名称（例如 "DATABASE"）及其名称
 * （未预先引用）来标识。
 */
void
emitShSecLabels(PGconn *conn, PGresult *res, PQExpBuffer buffer,
				const char *objtype, const char *objname)
{
	int			i;

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *provider = PQgetvalue(res, i, 0);
		char	   *label = PQgetvalue(res, i, 1);

		/* 必须先使用 fmtId 结果，然后再调用它 */
		appendPQExpBuffer(buffer,
						  "SECURITY LABEL FOR %s ON %s",
						  fmtId(provider), objtype);
		appendPQExpBuffer(buffer,
						  " %s IS ",
						  fmtId(objname));
		appendStringLiteralConn(buffer, label, conn);
		appendPQExpBufferStr(buffer, ";\n");
	}
}


/*
 * 检测给定的 GUC 变量是否为 GUC_LIST_QUOTE 类型。
 *
 * 如果能直接从后端查询会更好；但即使有相应的函数，
 * 它也只能告诉我们 guc.c 当前已知的变量，因此对于扩展
 * 声明 GUC_LIST_QUOTE 变量仍然是不安全的。在缺乏解决方案的情况下，
 * 除了维护此列表外，似乎不值得做更多工作，此列表必须
 * 与 guc_tables.c 中实际标记为 GUC_LIST_QUOTE 的变量保持同步。
 *
 */
bool
variable_is_guc_list_quote(const char *name)
{
	if (pg_strcasecmp(name, "local_preload_libraries") == 0 ||
		pg_strcasecmp(name, "search_path") == 0 ||
		pg_strcasecmp(name, "session_preload_libraries") == 0 ||
		pg_strcasecmp(name, "shared_preload_libraries") == 0 ||
		pg_strcasecmp(name, "temp_tablespaces") == 0 ||
		pg_strcasecmp(name, "unix_socket_directories") == 0)
		return true;
	else
		return false;
}

/*
 * SplitGUCList --- 解析包含标识符或文件名的字符串
 *
 * 用于分割 GUC_LIST_QUOTE GUC 变量的值，
 * 不预设元素会被视为标识符还是文件名。
 * 参见 src/backend/utils/adt/varlena.c 中的类似代码。
 *
 * 输入：
 *	rawstring: 输入字符串；必须可覆盖！返回时，
 *		它已被修改为包含分隔后的标识符。
 *	separator: 期望在标识符之间出现的分隔标点
 *		（通常是 '.' 或 ','）。标识符周围也可能出现空白。
 * 输出：
 *	namelist: 接收一个 malloc 分配的、以 NULL 结尾的指针数组，
 *		指向 rawstring 内的标识符。调用者应释放此数组，
 *		即使是在错误返回时。
 *
 * 成功返回 true，字符串存在语法错误则返回 false。
 *
 */
bool
SplitGUCList(char *rawstring, char separator,
			 char ***namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;
	char	  **nextptr;

	/*
	 * 由于我们不允许空标识符，这是对可能需要的指针数量的
	 * 保守高估。保留一个用于列表终止符。
	 *
	 */
	*namelist = nextptr = (char **)
		pg_malloc((strlen(rawstring) / 2 + 2) * sizeof(char *));
	*nextptr = NULL;

	while (isspace((unsigned char) *nextp))
		nextp++;				/* 跳过前导空白 */

	if (*nextp == '\0')
		return true;			/* 允许空字符串 */

	/* 循环顶部，我们处于新标识符的开始位置。 */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '"')
		{
			/* 引用的名称 --- 折叠引号-引号对 */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '"');
				if (endp == NULL)
					return false;	/* 引号不匹配 */
				if (endp[1] != '"')
					break;		/* 找到了引用名称的结束 */
				/* 将相邻引号折叠为一个引号，然后再次查找 */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp 现在指向终止引号 */
			nextp = endp + 1;
		}
		else
		{
			/* 未引用的名称 --- 延伸到分隔符或空白 */
			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !isspace((unsigned char) *nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* 不允许空的未引用名称 */
		}

		while (isspace((unsigned char) *nextp))
			nextp++;			/* 跳过尾部空白 */

		if (*nextp == separator)
		{
			nextp++;
			while (isspace((unsigned char) *nextp))
				nextp++;		/* 跳过下一个的前导空白 */
			/* 我们期望另一个名称，因此 done 保持 false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* 无效语法 */

		/* 现在可以安全地用空字符覆盖分隔符 */
		*endp = '\0';

		/*
		 * 完成当前名称的隔离 --- 将其添加到输出数组
		 */
		*nextptr++ = curname;

		/* 如果未到达字符串末尾则循环回去 */
	} while (!done);

	*nextptr = NULL;
	return true;
}

/*
 * 用于导出 "ALTER DATABASE/ROLE SET ..." 命令的辅助函数。
 *
 * 解析 configitem 的内容（"名称=值" 字符串），将其包装在
 * 完整的 ALTER 命令中，并追加到 buf。
 *
 * type 是 DATABASE 或 ROLE，name 是数据库或角色的名称。
 * 如果需要 "IN" 子句，type2 和 name2 类似地定义其中的内容；
 * 否则它们应为 NULL。
 * conn 仅用于确定字符串字面量的引用约定。
 */
void
makeAlterConfigCommand(PGconn *conn, const char *configitem,
					   const char *type, const char *name,
					   const char *type2, const char *name2,
					   PQExpBuffer buf)
{
	char	   *mine;
	char	   *pos;

	/* 解析 configitem。如果找不到 "="，则静默不做任何操作。 */
	mine = pg_strdup(configitem);
	pos = strchr(mine, '=');
	if (pos == NULL)
	{
		pg_free(mine);
		return;
	}
	*pos++ = '\0';

	/* 构建命令，对所有内容进行适当引用。 */
	appendPQExpBuffer(buf, "ALTER %s %s ", type, fmtId(name));
	if (type2 != NULL && name2 != NULL)
		appendPQExpBuffer(buf, "IN %s %s ", type2, fmtId(name2));
	appendPQExpBuffer(buf, "SET %s TO ", fmtId(mine));

	/*
	 * 标记为 GUC_LIST_QUOTE 的变量在放入 setconfig 数组之前
	 * 已被 flatten_set_variable_args() 完全引用。然而，
	 * 由于那里使用的引用规则与 SQL 不完全相同，
	 * 我们必须拆分列表值，然后将元素作为字符串字面量引用。
	 * （元素可以保持原有的双引号形式，但不能直接提供给 SQL 解析器；
	 * 这对零长度或超 NAMEDATALEN 的元素会做错误处理。）
	 *
	 * 未标记为此类别的变量应直接作为简单的字符串字面量输出。
	 * 如果变量不在 variable_is_guc_list_quote() 的已知范围内，
	 * 我们将这样做；这使得扩展变量使用 GUC_LIST_QUOTE 不安全。
	 *
	 *
	 */
	if (variable_is_guc_list_quote(mine))
	{
		char	  **namelist;
		char	  **nameptr;

		/* 将字符串解析为标识符列表 */
		/* 这实际上不应该失败 */
		if (SplitGUCList(pos, ',', &namelist))
		{
			for (nameptr = namelist; *nameptr; nameptr++)
			{
				if (nameptr != namelist)
					appendPQExpBufferStr(buf, ", ");
				appendStringLiteralConn(buf, *nameptr, conn);
			}
		}
		pg_free(namelist);
	}
	else
		appendStringLiteralConn(buf, pos, conn);

	appendPQExpBufferStr(buf, ";\n");

	pg_free(mine);
}

/*
 * 生成一个有效的限制键（即字母数字字符串），用于
 * psql 的 \restrict 和 \unrestrict 元命令。为安全起见，
 * 值是随机选择的。
 */
char *
generate_restrict_key(void)
{
	uint8		buf[64];
	char	   *ret = palloc(sizeof(buf));

	if (!pg_strong_random(buf, sizeof(buf)))
		return NULL;

	for (int i = 0; i < sizeof(buf) - 1; i++)
	{
		uint8		idx = buf[i] % strlen(restrict_chars);

		ret[i] = restrict_chars[idx];
	}
	ret[sizeof(buf) - 1] = '\0';

	return ret;
}

/*
 * 检查给定的限制键（用于 psql 的 \restrict 和
 * \unrestrict 元命令）是否仅包含字母数字字符。
 */
bool
valid_restrict_key(const char *restrict_key)
{
	return restrict_key != NULL &&
		restrict_key[0] != '\0' &&
		strspn(restrict_key, restrict_chars) == strlen(restrict_key);
}
