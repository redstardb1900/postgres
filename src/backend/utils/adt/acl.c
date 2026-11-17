/*-------------------------------------------------------------------------
 *
 * acl.c
 *	  基础访问控制列表（ACL）数据结构操作例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/acl.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

typedef struct
{
	const char *name;
	AclMode		value;
} priv_map;

/*
 * 我们经常需要测试某给定角色是否是另一角色的
 * 成员。在多数这类测试中，"给定角色"都是同一个，
 * 即当前活跃用户。为此可缓存列表来优化，列表内容为
 * "给定角色"直接或间接所属的全部角色。
 *
 * 也许这个机制应当被泛化，以允许为多个角色缓存
 * 成员信息？
 *
 * cached_roles 的每个元素是对应 cached_role 元素的
 * 组成角色 OID 列表（始终包含 cached_role 本身）。
 * 每种 RoleRecurseType 都有独立的缓存，语义与
 * 其对应。
 */
enum RoleRecurseType
{
	ROLERECURSE_MEMBERS = 0,	/* 无条件递归 */
	ROLERECURSE_PRIVS = 1,		/* 沿可继承授权递归 */
	ROLERECURSE_SETROLE = 2		/* 沿带 set_option 的授权递归 */
};
static Oid	cached_role[] = {InvalidOid, InvalidOid, InvalidOid};
static List *cached_roles[] = {NIL, NIL, NIL};
static uint32 cached_db_hash;


static const char *getid(const char *s, char *n, Node *escontext);
static void putid(char *p, const char *s);
static Acl *allocacl(int n);
static void check_acl(const Acl *acl);
static const char *aclparse(const char *s, AclItem *aip, Node *escontext);
static bool aclitem_match(const AclItem *a1, const AclItem *a2);
static int	aclitemComparator(const void *arg1, const void *arg2);
static void check_circularity(const Acl *old_acl, const AclItem *mod_aip,
							  Oid ownerId);
static Acl *recursive_revoke(Acl *acl, Oid grantee, AclMode revoke_privs,
							 Oid ownerId, DropBehavior behavior);

static AclMode convert_any_priv_string(text *priv_type_text,
									   const priv_map *privileges);

static Oid	convert_table_name(text *tablename);
static AclMode convert_table_priv_string(text *priv_type_text);
static AclMode convert_sequence_priv_string(text *priv_type_text);
static AttrNumber convert_column_name(Oid tableoid, text *column);
static AclMode convert_column_priv_string(text *priv_type_text);
static Oid	convert_database_name(text *databasename);
static AclMode convert_database_priv_string(text *priv_type_text);
static Oid	convert_foreign_data_wrapper_name(text *fdwname);
static AclMode convert_foreign_data_wrapper_priv_string(text *priv_type_text);
static Oid	convert_function_name(text *functionname);
static AclMode convert_function_priv_string(text *priv_type_text);
static Oid	convert_language_name(text *languagename);
static AclMode convert_language_priv_string(text *priv_type_text);
static Oid	convert_schema_name(text *schemaname);
static AclMode convert_schema_priv_string(text *priv_type_text);
static Oid	convert_server_name(text *servername);
static AclMode convert_server_priv_string(text *priv_type_text);
static Oid	convert_tablespace_name(text *tablespacename);
static AclMode convert_tablespace_priv_string(text *priv_type_text);
static Oid	convert_type_name(text *typename);
static AclMode convert_type_priv_string(text *priv_type_text);
static AclMode convert_parameter_priv_string(text *priv_text);
static AclMode convert_role_priv_string(text *priv_type_text);
static AclResult pg_role_aclcheck(Oid role_oid, Oid roleid, AclMode mode);

static void RoleMembershipCacheCallback(Datum arg, int cacheid, uint32 hashvalue);


/*
 * 测试标识符字符在 ACL 中是否可不加引号。
 *
 * 过去我们即使在非 ASCII 字符上也使用 isalnum()，
 * 产生不可移植的行为。为确保与旧版转储兼容，
 * 现在在 putid() 中，高位字符一律视为需要加引号，
 * 但 getid() 总是无需引号即接受它们。
 */
static inline bool
is_safe_acl_char(unsigned char c, bool is_getid)
{
	if (IS_HIGHBIT_SET(c))
		return is_getid;
	return isalnum(c) || c == '_';
}

/*
 * getid
 *		消耗 's' 中出现的第一个字母数字串（标识符），
 *		忽略任何前导空白。若遇到双引号，
 *		则返回引号内的词。
 *
 * RETURNS:
 *		 's' 中（越过任何引号之后）指向下一个非空白
 *		字符处。此外还：
 *		- 将标识符载入 'n'。（若未找到标识符，则 'n'
 *		  内容为空串。）'n' 必须为 NAMEDATALEN 字节。
 *
 * 错误经 ereport 报告，除非 escontext 是 ErrorSaveData 节点，
 * 此时我们将错误记录于其中并返回 NULL。
 */
static const char *
getid(const char *s, char *n, Node *escontext)
{
	int			len = 0;
	bool		in_quotes = false;

	Assert(s && n);

	while (isspace((unsigned char) *s))
		s++;
	for (;
		 *s != '\0' &&
		 (in_quotes || *s == '"' || is_safe_acl_char(*s, true));
		 s++)
	{
		if (*s == '"')
		{
			if (!in_quotes)
			{
				in_quotes = true;
				continue;
			}
			/* 可安全读下一字符（或为 '\0'） */
			if (*(s + 1) != '"')
			{
				in_quotes = false;
				continue;
			}
			/* 这是被转义的双引号；跳过转义字符 */
			s++;
		}

		/* 把该字符加入字符串 */
		if (len >= NAMEDATALEN - 1)
			ereturn(escontext, NULL,
					(errcode(ERRCODE_NAME_TOO_LONG),
					 errmsg("identifier too long"),
					 errdetail("Identifier must be less than %d characters.",
							   NAMEDATALEN)));

		n[len++] = *s;
	}
	n[len] = '\0';
	while (isspace((unsigned char) *s))
		s++;
	return s;
}

/*
 * 在 *p 处写角色名，必要时加双引号。
 * 在 *p 处必须至少有 (2*NAMEDATALEN)+2 字节可用。
 * 此处需与 pg_dump/dumputils.c 中的 dequoteAclUserName 保持同步。
 */
static void
putid(char *p, const char *s)
{
	const char *src;
	bool		safe = true;

	/* 检测是否需要使用双引号 */
	for (src = s; *src; src++)
	{
		if (!is_safe_acl_char(*src, false))
		{
			safe = false;
			break;
		}
	}
	if (!safe)
		*p++ = '"';
	for (src = s; *src; src++)
	{
		/* 用户名中的双引号字符被编码为 ""。 */
		if (*src == '"')
			*p++ = '"';
		*p++ = *src;
	}
	if (!safe)
		*p++ = '"';
	*p = '\0';
}

/*
 * aclparse
 *		消耗并解析如下形式的 ACL 规格：
 *				[group|user] [A-Za-z0-9]*=[rwaR]*
 *		取自字符串 's'，忽略任何前导空白或介于可选
 *		id 类型关键字 (group|user) 与实际 ACL 规格
 *		之间的空白。
 *
 *		group|user 修饰在角色世界中已无必要，
 *		但为向后兼容我们仍接受它。
 *
 *		该例程既被解析器又被 aclitemin() 调用，因而
 *		有通用性。
 *
 * RETURNS:
 *		 's' 中紧随 ACL 规格之后的字符串位置。
 *		此外还：
 *		- 以适当的 UID/GID、id 类型标识符与模式类型值
 *		  填充 'aip' 所指向的结构。
 *
 * 错误经 ereport 报告，除非 escontext 是 ErrorSaveData 节点，
 * 此时我们将错误记录于其中并返回 NULL。
 */
static const char *
aclparse(const char *s, AclItem *aip, Node *escontext)
{
	AclMode		privs,
				goption,
				read;
	char		name[NAMEDATALEN];
	char		name2[NAMEDATALEN];

	Assert(s && aip);

	s = getid(s, name, escontext);
	if (s == NULL)
		return NULL;
	if (*s != '=')
	{
		/* 读到的是关键字而非名字 */
		if (strcmp(name, "group") != 0 && strcmp(name, "user") != 0)
			ereturn(escontext, NULL,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("unrecognized key word: \"%s\"", name),
					 errhint("ACL key word must be \"group\" or \"user\".")));
		/* 把 s 移到关键字后的名字处 */
		s = getid(s, name, escontext);
		if (s == NULL)
			return NULL;
		if (name[0] == '\0')
			ereturn(escontext, NULL,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing name"),
					 errhint("A name must follow the \"group\" or \"user\" key word.")));
	}

	if (*s != '=')
		ereturn(escontext, NULL,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("missing \"=\" sign")));

	privs = goption = ACL_NO_RIGHTS;

	for (++s, read = 0; isalpha((unsigned char) *s) || *s == '*'; s++)
	{
		switch (*s)
		{
			case '*':
				goption |= read;
				break;
			case ACL_INSERT_CHR:
				read = ACL_INSERT;
				break;
			case ACL_SELECT_CHR:
				read = ACL_SELECT;
				break;
			case ACL_UPDATE_CHR:
				read = ACL_UPDATE;
				break;
			case ACL_DELETE_CHR:
				read = ACL_DELETE;
				break;
			case ACL_TRUNCATE_CHR:
				read = ACL_TRUNCATE;
				break;
			case ACL_REFERENCES_CHR:
				read = ACL_REFERENCES;
				break;
			case ACL_TRIGGER_CHR:
				read = ACL_TRIGGER;
				break;
			case ACL_EXECUTE_CHR:
				read = ACL_EXECUTE;
				break;
			case ACL_USAGE_CHR:
				read = ACL_USAGE;
				break;
			case ACL_CREATE_CHR:
				read = ACL_CREATE;
				break;
			case ACL_CREATE_TEMP_CHR:
				read = ACL_CREATE_TEMP;
				break;
			case ACL_CONNECT_CHR:
				read = ACL_CONNECT;
				break;
			case ACL_SET_CHR:
				read = ACL_SET;
				break;
			case ACL_ALTER_SYSTEM_CHR:
				read = ACL_ALTER_SYSTEM;
				break;
			case 'R':			/* 忽略旧 RULE 权限 */
				read = 0;
				break;
			default:
				ereturn(escontext, NULL,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid mode character: must be one of \"%s\"",
								ACL_ALL_RIGHTS_STR)));
		}

		privs |= read;
	}

	if (name[0] == '\0')
		aip->ai_grantee = ACL_ID_PUBLIC;
	else
	{
		aip->ai_grantee = get_role_oid(name, true);
		if (!OidIsValid(aip->ai_grantee))
			ereturn(escontext, NULL,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", name)));
	}

	/*
	 * XXX 把授权者默认设为超级用户，借此容许一定
	 * 的兼容性。
	 */
	if (*s == '/')
	{
		s = getid(s + 1, name2, escontext);
		if (s == NULL)
			return NULL;
		if (name2[0] == '\0')
			ereturn(escontext, NULL,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("a name must follow the \"/\" sign")));
		aip->ai_grantor = get_role_oid(name2, true);
		if (!OidIsValid(aip->ai_grantor))
			ereturn(escontext, NULL,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", name2)));
	}
	else
	{
		aip->ai_grantor = BOOTSTRAP_SUPERUSERID;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_GRANTOR),
				 errmsg("defaulting grantor to user ID %u",
						BOOTSTRAP_SUPERUSERID)));
	}

	ACLITEM_SET_PRIVS_GOPTIONS(*aip, privs, goption);

	return s;
}

/*
 * allocacl
 *		为含 'n' 个条目的新 Acl 分配存储。
 *
 * RETURNS:
 *		新 Acl
 */
static Acl *
allocacl(int n)
{
	Acl		   *new_acl;
	Size		size;

	if (n < 0)
		elog(ERROR, "invalid size: %d", n);
	size = ACL_N_SIZE(n);
	new_acl = (Acl *) palloc0(size);
	SET_VARSIZE(new_acl, size);
	new_acl->ndim = 1;
	new_acl->dataoffset = 0;	/* 从不放入任何空值 */
	new_acl->elemtype = ACLITEMOID;
	ARR_LBOUND(new_acl)[0] = 1;
	ARR_DIMS(new_acl)[0] = n;
	return new_acl;
}

/*
 * 创建零条目 ACL
 */
Acl *
make_empty_acl(void)
{
	return allocacl(0);
}

/*
 * 复制 ACL
 */
Acl *
aclcopy(const Acl *orig_acl)
{
	Acl		   *result_acl;

	result_acl = allocacl(ACL_NUM(orig_acl));

	memcpy(ACL_DAT(result_acl),
		   ACL_DAT(orig_acl),
		   ACL_NUM(orig_acl) * sizeof(AclItem));

	return result_acl;
}

/*
 * 拼接两个 ACL
 *
 * 这有点投机取巧，因为可能产生含冗余条目的 ACL。
 * 注意结果会被用于何处！
 */
Acl *
aclconcat(const Acl *left_acl, const Acl *right_acl)
{
	Acl		   *result_acl;

	result_acl = allocacl(ACL_NUM(left_acl) + ACL_NUM(right_acl));

	memcpy(ACL_DAT(result_acl),
		   ACL_DAT(left_acl),
		   ACL_NUM(left_acl) * sizeof(AclItem));

	memcpy(ACL_DAT(result_acl) + ACL_NUM(left_acl),
		   ACL_DAT(right_acl),
		   ACL_NUM(right_acl) * sizeof(AclItem));

	return result_acl;
}

/*
 * 两 ACL 合并
 *
 * 这会产生一个正确合并、无冗余条目的 ACL。
 * NULL 输入返回 NULL。
 */
Acl *
aclmerge(const Acl *left_acl, const Acl *right_acl, Oid ownerId)
{
	Acl		   *result_acl;
	AclItem    *aip;
	int			i,
				num;

	/* 检查一方或双方为空/null 的情形 */
	if (left_acl == NULL || ACL_NUM(left_acl) == 0)
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return NULL;
		else
			return aclcopy(right_acl);
	}
	else
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return aclcopy(left_acl);
	}

	/* 以笨办法合并，一次一个条目 */
	result_acl = aclcopy(left_acl);

	aip = ACL_DAT(right_acl);
	num = ACL_NUM(right_acl);

	for (i = 0; i < num; i++, aip++)
	{
		Acl		   *tmp_acl;

		tmp_acl = aclupdate(result_acl, aip, ACL_MODECHG_ADD,
							ownerId, DROP_RESTRICT);
		pfree(result_acl);
		result_acl = tmp_acl;
	}

	return result_acl;
}

/*
 * 对 ACL 中的条目排序（按任意但一致的顺序）
 */
void
aclitemsort(Acl *acl)
{
	if (acl != NULL && ACL_NUM(acl) > 1)
		qsort(ACL_DAT(acl), ACL_NUM(acl), sizeof(AclItem), aclitemComparator);
}

/*
 * 检查两个 ACL 是否完全相等
 *
 * 若两数组以不同顺序包含相同条目，本函数不能
 * 检出相等。要处理这种情况，请先用
 * aclitemsort()。
 */
bool
aclequal(const Acl *left_acl, const Acl *right_acl)
{
	/* 检查一方或双方为空/null 的情形 */
	if (left_acl == NULL || ACL_NUM(left_acl) == 0)
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return true;
		else
			return false;
	}
	else
	{
		if (right_acl == NULL || ACL_NUM(right_acl) == 0)
			return false;
	}

	if (ACL_NUM(left_acl) != ACL_NUM(right_acl))
		return false;

	if (memcmp(ACL_DAT(left_acl),
			   ACL_DAT(right_acl),
			   ACL_NUM(left_acl) * sizeof(AclItem)) == 0)
		return true;

	return false;
}

/*
 * 校验 ACL 数组是可接受的（一维且不含空值）
 */
static void
check_acl(const Acl *acl)
{
	if (ARR_ELEMTYPE(acl) != ACLITEMOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL array contains wrong data type")));
	if (ARR_NDIM(acl) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ACL arrays must be one-dimensional")));
	if (ARR_HASNULL(acl))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("ACL arrays must not contain null values")));
}

/*
 * aclitemin
 *		为新 AclItem 分配存储并填充，输入为含 ACL
 *		的字符串 's'。细节见 aclparse。
 *
 * RETURNS:
 *		新 AclItem
 */
Datum
aclitemin(PG_FUNCTION_ARGS)
{
	const char *s = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	AclItem    *aip;

	aip = (AclItem *) palloc(sizeof(AclItem));

	s = aclparse(s, aip, escontext);
	if (s == NULL)
		PG_RETURN_NULL();

	while (isspace((unsigned char) *s))
		++s;
	if (*s)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("extra garbage at the end of the ACL specification")));

	PG_RETURN_ACLITEM_P(aip);
}

/*
 * aclitemout
 *		为以 null 分隔的新字符串分配存储并填入格式化
 *		的 ACL 规格。细节见 aclparse。
 *
 * RETURNS:
 *		新字符串
 */
Datum
aclitemout(PG_FUNCTION_ARGS)
{
	AclItem    *aip = PG_GETARG_ACLITEM_P(0);
	char	   *p;
	char	   *out;
	HeapTuple	htup;
	unsigned	i;

	out = palloc(strlen("=/") +
				 2 * N_ACL_RIGHTS +
				 2 * (2 * NAMEDATALEN + 2) +
				 1);

	p = out;
	*p = '\0';

	if (aip->ai_grantee != ACL_ID_PUBLIC)
	{
		htup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(aip->ai_grantee));
		if (HeapTupleIsValid(htup))
		{
			putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
			ReleaseSysCache(htup);
		}
		else
		{
			/* 若未找到条目则生成数字 OID */
			sprintf(p, "%u", aip->ai_grantee);
		}
	}
	while (*p)
		++p;

	*p++ = '=';

	for (i = 0; i < N_ACL_RIGHTS; ++i)
	{
		if (ACLITEM_GET_PRIVS(*aip) & (UINT64CONST(1) << i))
			*p++ = ACL_ALL_RIGHTS_STR[i];
		if (ACLITEM_GET_GOPTIONS(*aip) & (UINT64CONST(1) << i))
			*p++ = '*';
	}

	*p++ = '/';
	*p = '\0';

	htup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(aip->ai_grantor));
	if (HeapTupleIsValid(htup))
	{
		putid(p, NameStr(((Form_pg_authid) GETSTRUCT(htup))->rolname));
		ReleaseSysCache(htup);
	}
	else
	{
		/* 若未找到条目则生成数字 OID */
		sprintf(p, "%u", aip->ai_grantor);
	}

	PG_RETURN_CSTRING(out);
}

/*
 * aclitem_match
 *		两 AclItem 具有相同被授权者与授权者时，
 *		即视为匹配；权限被忽略。
 */
static bool
aclitem_match(const AclItem *a1, const AclItem *a2)
{
	return a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
}

/*
 * aclitemComparator
 *		用于 AclItem 的 qsort 比较函数
 */
static int
aclitemComparator(const void *arg1, const void *arg2)
{
	const AclItem *a1 = (const AclItem *) arg1;
	const AclItem *a2 = (const AclItem *) arg2;

	if (a1->ai_grantee > a2->ai_grantee)
		return 1;
	if (a1->ai_grantee < a2->ai_grantee)
		return -1;
	if (a1->ai_grantor > a2->ai_grantor)
		return 1;
	if (a1->ai_grantor < a2->ai_grantor)
		return -1;
	if (a1->ai_privs > a2->ai_privs)
		return 1;
	if (a1->ai_privs < a2->ai_privs)
		return -1;
	return 0;
}

/*
 * aclitem 相等操作符
 */
Datum
aclitem_eq(PG_FUNCTION_ARGS)
{
	AclItem    *a1 = PG_GETARG_ACLITEM_P(0);
	AclItem    *a2 = PG_GETARG_ACLITEM_P(1);
	bool		result;

	result = a1->ai_privs == a2->ai_privs &&
		a1->ai_grantee == a2->ai_grantee &&
		a1->ai_grantor == a2->ai_grantor;
	PG_RETURN_BOOL(result);
}

/*
 * aclitem 哈希函数
 *
 * 我们使 aclitem 可哈希，与其说因有人会哈希它们，
 * 不如说是因为想让数组相等能用于 aclitem 数组，而
 * 有了 typcache 机制就必须有哈希或 btree 操作符类。
 */
Datum
hash_aclitem(PG_FUNCTION_ARGS)
{
	AclItem    *a = PG_GETARG_ACLITEM_P(0);

	/* 不聪明，但可避免结构体填充的任何问题 */
	PG_RETURN_UINT32((uint32) (a->ai_privs + a->ai_grantee + a->ai_grantor));
}

/*
 * aclitem 的 64 位哈希函数。
 *
 * 与 hash_aclitem 类似，但接受一个种子并返回 uint64 值。
 */
Datum
hash_aclitem_extended(PG_FUNCTION_ARGS)
{
	AclItem    *a = PG_GETARG_ACLITEM_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	uint32		sum = (uint32) (a->ai_privs + a->ai_grantee + a->ai_grantor);

	return (seed == 0) ? UInt64GetDatum(sum) : hash_uint32_extended(sum, seed);
}

/*
 * acldefault()  --- 创建一个描述默认访问权限的 ACL
 *
 * 若你想更改新建对象默认访问策略（或任何 ACL 条目
 * 为 NULL 的对象），请修改此例程。改动后别忘了
 * 更新 GRANT 手册页，
 * 其中解释了全部默认权限。
 *
 * 注意这些是硬编码的"默认值"，在没有 pg_default_acl
 * 条目存在时使用。
 */
Acl *
acldefault(ObjectType objtype, Oid ownerId)
{
	AclMode		world_default;
	AclMode		owner_default;
	int			nacl;
	Acl		   *acl;
	AclItem    *aip;

	switch (objtype)
	{
		case OBJECT_COLUMN:
			/* 默认情况下，列没有额外权限 */
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_NO_RIGHTS;
			break;
		case OBJECT_TABLE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_RELATION;
			break;
		case OBJECT_SEQUENCE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_SEQUENCE;
			break;
		case OBJECT_DATABASE:
			/* 为向后兼容，默认授予某些权限 */
			world_default = ACL_CREATE_TEMP | ACL_CONNECT;
			owner_default = ACL_ALL_RIGHTS_DATABASE;
			break;
		case OBJECT_FUNCTION:
			/* 目前默认授予 EXECUTE */
			world_default = ACL_EXECUTE;
			owner_default = ACL_ALL_RIGHTS_FUNCTION;
			break;
		case OBJECT_LANGUAGE:
			/* 目前默认授予 USAGE */
			world_default = ACL_USAGE;
			owner_default = ACL_ALL_RIGHTS_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_LARGEOBJECT;
			break;
		case OBJECT_SCHEMA:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_SCHEMA;
			break;
		case OBJECT_TABLESPACE:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_TABLESPACE;
			break;
		case OBJECT_FDW:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_FDW;
			break;
		case OBJECT_FOREIGN_SERVER:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_FOREIGN_SERVER;
			break;
		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			world_default = ACL_USAGE;
			owner_default = ACL_ALL_RIGHTS_TYPE;
			break;
		case OBJECT_PARAMETER_ACL:
			world_default = ACL_NO_RIGHTS;
			owner_default = ACL_ALL_RIGHTS_PARAMETER_ACL;
			break;
		default:
			elog(ERROR, "unrecognized object type: %d", (int) objtype);
			world_default = ACL_NO_RIGHTS;	/* 让编译器安静 */
			owner_default = ACL_NO_RIGHTS;
			break;
	}

	nacl = 0;
	if (world_default != ACL_NO_RIGHTS)
		nacl++;
	if (owner_default != ACL_NO_RIGHTS)
		nacl++;

	acl = allocacl(nacl);
	aip = ACL_DAT(acl);

	if (world_default != ACL_NO_RIGHTS)
	{
		aip->ai_grantee = ACL_ID_PUBLIC;
		aip->ai_grantor = ownerId;
		ACLITEM_SET_PRIVS_GOPTIONS(*aip, world_default, ACL_NO_RIGHTS);
		aip++;
	}

	/*
	 * 注：属主条目显示所有普通权限而无授权选项。因
	 * 他的授权选项"来自系统"，而非凭他自身努力。
	 * （SQL 规范说属主的权限来自一个
	 *  "_SYSTEM" authid。）不过，我们确实认为属主
	 * 的普通权限是自授的；这让他能够收回它们。
	 * 我们实现属主授权选项时不需要任何显式的
	 *  "_SYSTEM" 式 ACL 条目，凡在测试授权选项之处，
	 * 我们都对属主做内部特判。
	 */
	if (owner_default != ACL_NO_RIGHTS)
	{
		aip->ai_grantee = ownerId;
		aip->ai_grantor = ownerId;
		ACLITEM_SET_PRIVS_GOPTIONS(*aip, owner_default, ACL_NO_RIGHTS);
	}

	return acl;
}


/*
 * acldefault() 的 SQL 版本。从 "char" 类型到 OBJECT_* 值的
 * 凑合映射。
 */
Datum
acldefault_sql(PG_FUNCTION_ARGS)
{
	char		objtypec = PG_GETARG_CHAR(0);
	Oid			owner = PG_GETARG_OID(1);
	ObjectType	objtype = 0;

	switch (objtypec)
	{
		case 'c':
			objtype = OBJECT_COLUMN;
			break;
		case 'r':
			objtype = OBJECT_TABLE;
			break;
		case 's':
			objtype = OBJECT_SEQUENCE;
			break;
		case 'd':
			objtype = OBJECT_DATABASE;
			break;
		case 'f':
			objtype = OBJECT_FUNCTION;
			break;
		case 'l':
			objtype = OBJECT_LANGUAGE;
			break;
		case 'L':
			objtype = OBJECT_LARGEOBJECT;
			break;
		case 'n':
			objtype = OBJECT_SCHEMA;
			break;
		case 'p':
			objtype = OBJECT_PARAMETER_ACL;
			break;
		case 't':
			objtype = OBJECT_TABLESPACE;
			break;
		case 'F':
			objtype = OBJECT_FDW;
			break;
		case 'S':
			objtype = OBJECT_FOREIGN_SERVER;
			break;
		case 'T':
			objtype = OBJECT_TYPE;
			break;
		default:
			elog(ERROR, "unrecognized object type abbreviation: %c", objtypec);
	}

	PG_RETURN_ACL_P(acldefault(objtype, owner));
}


/*
 * 更新 ACL 数组以增加或移除指定的权限。
 *
 *	old_acl: 输入 ACL 数组
 *	mod_aip: 定义要被增加、移除或替换的权限
 *	modechg: ACL_MODECHG_ADD、ACL_MODECHG_DEL 或ACL_MODECHG_EQL
 *	ownerId: 对象属主的 Oid
 *	behavior: 递归移除时采用 RESTRICT 或 CASCADE 行为
 *
 * ownerid 与 behavior 仅当更新操作指定删除授权选项时
 * 才相关。
 *
 * 结果为修改后的副本；输入对象不会被更改。
 *
 * 注意：调用者负责在需要时对输入 ACL 预先去 toast。
 */
Acl *
aclupdate(const Acl *old_acl, const AclItem *mod_aip,
		  int modechg, Oid ownerId, DropBehavior behavior)
{
	Acl		   *new_acl = NULL;
	AclItem    *old_aip,
			   *new_aip = NULL;
	AclMode		old_rights,
				old_goptions,
				new_rights,
				new_goptions;
	int			dst,
				num;

	/* 调用者多半已检查过 old_acl，但求稳妥 */
	check_acl(old_acl);

	/* 若授予授权选项，则检查循环性 */
	if (modechg != ACL_MODECHG_DEL &&
		ACLITEM_GET_GOPTIONS(*mod_aip) != ACL_NO_RIGHTS)
		check_circularity(old_acl, mod_aip, ownerId);

	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);

	/*
	 * 在 ACL 中搜索此被授权者和授权者的既有条目。若
	 * 存在，就原位修改该条目（确切说是同一位置，因为
	 * 实际返回的是副本）；否则把新条目插入到末尾。
	 *
	 */

	for (dst = 0; dst < num; ++dst)
	{
		if (aclitem_match(mod_aip, old_aip + dst))
		{
			/* 找到匹配，故修改既有条目 */
			new_acl = allocacl(num);
			new_aip = ACL_DAT(new_acl);
			memcpy(new_acl, old_acl, ACL_SIZE(old_acl));
			break;
		}
	}

	if (dst == num)
	{
		/* 需要追加新条目 */
		new_acl = allocacl(num + 1);
		new_aip = ACL_DAT(new_acl);
		memcpy(new_aip, old_aip, num * sizeof(AclItem));

		/* 以无权限初始化新条目 */
		new_aip[dst].ai_grantee = mod_aip->ai_grantee;
		new_aip[dst].ai_grantor = mod_aip->ai_grantor;
		ACLITEM_SET_PRIVS_GOPTIONS(new_aip[dst],
								   ACL_NO_RIGHTS, ACL_NO_RIGHTS);
		num++;					/* 设 num 为 new_acl 大小 */
	}

	old_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	old_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/* 应用指定的权限变更 */
	switch (modechg)
	{
		case ACL_MODECHG_ADD:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights | ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_DEL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   old_rights & ~ACLITEM_GET_RIGHTS(*mod_aip));
			break;
		case ACL_MODECHG_EQL:
			ACLITEM_SET_RIGHTS(new_aip[dst],
							   ACLITEM_GET_RIGHTS(*mod_aip));
			break;
	}

	new_rights = ACLITEM_GET_RIGHTS(new_aip[dst]);
	new_goptions = ACLITEM_GET_GOPTIONS(new_aip[dst]);

	/*
	 * 若调整后条目已无权限，则将其从列表中删除。
	 */
	if (new_rights == ACL_NO_RIGHTS)
	{
		memmove(new_aip + dst,
				new_aip + dst + 1,
				(num - dst - 1) * sizeof(AclItem));
		/* 调整数组大小为 'num - 1' 项 */
		ARR_DIMS(new_acl)[0] = num - 1;
		SET_VARSIZE(new_acl, ACL_N_SIZE(num - 1));
	}

	/*
	 * 移除被弃权限（级联收回）。目前仅当被授权者并非
	 * PUBLIC 时才能处理这种情况。
	 */
	if ((old_goptions & ~new_goptions) != 0)
	{
		Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);
		new_acl = recursive_revoke(new_acl, mod_aip->ai_grantee,
								   (old_goptions & ~new_goptions),
								   ownerId, behavior);
	}

	return new_acl;
}

/*
 * 更新 ACL 数组以反映父对象属主的变更
 *
 *	old_acl: 输入 ACL 数组（不得为NULL）
 *	oldOwnerId: 旧对象属主的 Oid
 *	newOwnerId: 新对象属主的 Oid
 *
 * 结果为修改后的副本；输入对象不会被更改。
 *
 * 注意：调用者负责在需要时对输入 ACL 预先去 toast。
 */
Acl *
aclnewowner(const Acl *old_acl, Oid oldOwnerId, Oid newOwnerId)
{
	Acl		   *new_acl;
	AclItem    *new_aip;
	AclItem    *old_aip;
	AclItem    *dst_aip;
	AclItem    *src_aip;
	AclItem    *targ_aip;
	bool		newpresent = false;
	int			dst,
				src,
				targ,
				num;

	check_acl(old_acl);

	/*
	 * 复制 ACL，旧属主 ID 以授权者或被授权者出现
	 * 之处，替换为新属主 ID；同时若新属主 ID
	 * 已存在则记录之。
	 */
	num = ACL_NUM(old_acl);
	old_aip = ACL_DAT(old_acl);
	new_acl = allocacl(num);
	new_aip = ACL_DAT(new_acl);
	memcpy(new_aip, old_aip, num * sizeof(AclItem));
	for (dst = 0, dst_aip = new_aip; dst < num; dst++, dst_aip++)
	{
		if (dst_aip->ai_grantor == oldOwnerId)
			dst_aip->ai_grantor = newOwnerId;
		else if (dst_aip->ai_grantor == newOwnerId)
			newpresent = true;
		if (dst_aip->ai_grantee == oldOwnerId)
			dst_aip->ai_grantee = newOwnerId;
		else if (dst_aip->ai_grantee == newOwnerId)
			newpresent = true;
	}

	/*
	 * 若旧 ACL 曾引用新属主，我们此时可能生成了一个
	 * 含重复条目的 ACL。把它们找出来合并，以消除重复。
	 * （代价高，因我们用 O(N^2) 笨算法，但情况不太
	 * 可能是常态。）
	 *
	 *
	 * 为简化重复条目的删除，我们暂时把它们留在数组里
	 * 但把其权限掩码设为零；遇到这种条目即跳过。
	 * （故本代码副作用之一是清除无权限条目，若输入中
	 * 有这类条目。）dst 是下一个输出槽位，targ 是当前
	 * 考察的输入槽位（总是 >= dst），src 向右扫描其右侧
	 * 条目以查找重复。一个条目一旦被输出到 dst，即可知
	 * 无重复，无需再被考察。
	 *
	 */
	if (newpresent)
	{
		dst = 0;
		for (targ = 0, targ_aip = new_aip; targ < num; targ++, targ_aip++)
		{
			/* 若前一轮已删除则忽略 */
			if (ACLITEM_GET_RIGHTS(*targ_aip) == ACL_NO_RIGHTS)
				continue;
			/* 查找并合并重复条目 */
			for (src = targ + 1, src_aip = targ_aip + 1; src < num;
				 src++, src_aip++)
			{
				if (ACLITEM_GET_RIGHTS(*src_aip) == ACL_NO_RIGHTS)
					continue;
				if (aclitem_match(targ_aip, src_aip))
				{
					ACLITEM_SET_RIGHTS(*targ_aip,
									   ACLITEM_GET_RIGHTS(*targ_aip) |
									   ACLITEM_GET_RIGHTS(*src_aip));
					/* 把重复标为已删除 */
					ACLITEM_SET_RIGHTS(*src_aip, ACL_NO_RIGHTS);
				}
			}
			/* 并输出到结果 */
			new_aip[dst] = *targ_aip;
			dst++;
		}
		/* 调整数组大小为 'dst' 项 */
		ARR_DIMS(new_acl)[0] = dst;
		SET_VARSIZE(new_acl, ACL_N_SIZE(dst));
	}

	return new_acl;
}


/*
 * 在授予授权选项时，我们必须禁止任何建立循环授权选
 * 项链的企图。设 A（对象属主）授予 B 一些权限且带
 * 授权选项，然后 B 又把它们转授给 C。若 C 也能把这
 * 些权限也授予 B，那么 A 将无法有效地从 B 收回这些权
 * 限，因为 recursive_revoke 会认为 B 仍从 C 处持有
 * 它们。
 *
 * 我们对此的检查方式是：递归删除属于目标被授权者的
 * 全部授权选项，然后看准授权者是否还
 * 持有它。
 */
static void
check_circularity(const Acl *old_acl, const AclItem *mod_aip,
				  Oid ownerId)
{
	Acl		   *acl;
	AclItem    *aip;
	int			i,
				num;
	AclMode		own_privs;

	check_acl(old_acl);

	/*
	 * 目前授权选项只能授予角色，不能授予 PUBLIC。
	 * 否则我们得在这里多花些功夫。
	 */
	Assert(mod_aip->ai_grantee != ACL_ID_PUBLIC);

	/* 属主总是持有授权选项，无需检查 */
	if (mod_aip->ai_grantor == ownerId)
		return;

	/* 建立工作副本 */
	acl = allocacl(ACL_NUM(old_acl));
	memcpy(acl, old_acl, ACL_SIZE(old_acl));

	/* 清除目标被授权者的全部授权选项及其所依赖项 */
cc_restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantee == mod_aip->ai_grantee &&
			ACLITEM_GET_GOPTIONS(aip[i]) != ACL_NO_RIGHTS)
		{
			Acl		   *new_acl;

			/* 实际也会清除普通权限，但无关紧要 */
			new_acl = aclupdate(acl, &aip[i], ACL_MODECHG_DEL,
								ownerId, DROP_CASCADE);

			pfree(acl);
			acl = new_acl;

			goto cc_restart;
		}
	}

	/* 现在我们可以计算授权者独立取得的权限了 */
	own_privs = aclmask(acl,
						mod_aip->ai_grantor,
						ownerId,
						ACL_GRANT_OPTION_FOR(ACLITEM_GET_GOPTIONS(*mod_aip)),
						ACLMASK_ALL);
	own_privs = ACL_OPTION_TO_PRIVS(own_privs);

	if ((ACLITEM_GET_GOPTIONS(*mod_aip) & ~own_privs) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_GRANT_OPERATION),
				 errmsg("grant options cannot be granted back to your own grantor")));

	pfree(acl);
}


/*
 * 确保没有任何权限被"遗弃"。所谓遗弃，
 * 是指授权该权限的用户失去了授权选项。（即其
 * 授权链断。）此时，
 * 要么把这些遗弃权限一并收回，要么打印错误
 * 消息，取决于丢弃行为选项。
 *
 *	acl: 输入 ACL 列表
 *	grantee: 被收回了某些授权选项的用户
 *	revoke_privs: 正被收回的授权选项
 *	ownerId: 对象属主的 Oid
 *	behavior: 递归移除时采用 RESTRICT 或 CASCADE 行为
 *
 * 若被替换，输入 Acl 会被 pfree。
 */
static Acl *
recursive_revoke(Acl *acl,
				 Oid grantee,
				 AclMode revoke_privs,
				 Oid ownerId,
				 DropBehavior behavior)
{
	AclMode		still_has;
	AclItem    *aip;
	int			i,
				num;

	check_acl(acl);

	/* 属主绝不会真正失去授权选项，故走捷径 */
	if (grantee == ownerId)
		return acl;

	/* 被授权者可能仍经由另一授权者持有某些授权选项 */
	still_has = aclmask(acl, grantee, ownerId,
						ACL_GRANT_OPTION_FOR(revoke_privs),
						ACLMASK_ALL);
	revoke_privs &= ~ACL_OPTION_TO_PRIVS(still_has);
	if (revoke_privs == ACL_NO_RIGHTS)
		return acl;

restart:
	num = ACL_NUM(acl);
	aip = ACL_DAT(acl);
	for (i = 0; i < num; i++)
	{
		if (aip[i].ai_grantor == grantee
			&& (ACLITEM_GET_PRIVS(aip[i]) & revoke_privs) != 0)
		{
			AclItem		mod_acl;
			Acl		   *new_acl;

			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			mod_acl.ai_grantor = grantee;
			mod_acl.ai_grantee = aip[i].ai_grantee;
			ACLITEM_SET_PRIVS_GOPTIONS(mod_acl,
									   revoke_privs,
									   revoke_privs);

			new_acl = aclupdate(acl, &mod_acl, ACL_MODECHG_DEL,
								ownerId, behavior);

			pfree(acl);
			acl = new_acl;

			goto restart;
		}
	}

	return acl;
}


/*
 * aclmask --- 计算 roleid 所持全部权限的位掩码。
 *
 * 当 'how' = ACLMASK_ALL 时，仅按给定 ACL 列表返回该
 *  roleid 持有的权限位与 'mask' 相与。（传 'mask'
 * 意在让例程找齐所需权限后即提前退出。）
 *
 *
 * 当 'how' = ACLMASK_ANY 时，掩码中任一位已知为真
 * 即返回。（调用者只想测零与否时，可最早退
 * 出。）
 *
 * 用法模式：
 *
 * 查一组权限中是否有任一被持有：
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ANY) != 0)
 *
 * 检查一组权限是否全部被持有：
 *		if (aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL) == privs)
 *
 * 确定一组权限中具体哪些被持有：
 *		heldprivs = aclmask(acl, roleid, ownerId, privs, ACLMASK_ALL);
 */
AclMode
aclmask(const Acl *acl, Oid roleid, Oid ownerId,
		AclMode mask, AclMaskHow how)
{
	AclMode		result;
	AclMode		remaining;
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * 空 ACL 不应出现，因为调用者应当已插入
	 * 相应的默认值
	 */
	if (acl == NULL)
		elog(ERROR, "null ACL");

	check_acl(acl);

	/* mask == 0 快速退出 */
	if (mask == 0)
		return 0;

	result = 0;

	/* 属主总是隐式持有全部授权选项 */
	if ((mask & ACLITEM_ALL_GOPTION_BITS) &&
		has_privs_of_role(roleid, ownerId))
	{
		result = mask & ACLITEM_ALL_GOPTION_BITS;
		if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
			return result;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * 检查直接授予 roleid 或 public 的权限
	 */
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == ACL_ID_PUBLIC ||
			aidata->ai_grantee == roleid)
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
		}
	}

	/*
	 * 检查经角色成员关系间接授予的权限。我们用单独一
	 * 轮来做，以尽量减少高代价的间接成员测试。特别
	 * 是，值得先测试某 ACL 条目是否仍授予我们所需的
	 * 权限，然后再执行 has_privs_of_role 测试。
	 *
	 */
	remaining = mask & ~result;
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == ACL_ID_PUBLIC ||
			aidata->ai_grantee == roleid)
			continue;			/* 已检查过 */

		if ((aidata->ai_privs & remaining) &&
			has_privs_of_role(roleid, aidata->ai_grantee))
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
			remaining = mask & ~result;
		}
	}

	return result;
}


/*
 * aclmask_direct --- 计算 roleid 所持全部权限的位掩码。
 *
 * 本函数与 aclmask() 完全相同，除了我们只考虑
 *  roleid *直接*持有的权限，不含经角色继承所得。
 */
static AclMode
aclmask_direct(const Acl *acl, Oid roleid, Oid ownerId,
			   AclMode mask, AclMaskHow how)
{
	AclMode		result;
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * 空 ACL 不应出现，因为调用者应当已插入
	 * 相应的默认值
	 */
	if (acl == NULL)
		elog(ERROR, "null ACL");

	check_acl(acl);

	/* mask == 0 快速退出 */
	if (mask == 0)
		return 0;

	result = 0;

	/* 属主总是隐式持有全部授权选项 */
	if ((mask & ACLITEM_ALL_GOPTION_BITS) &&
		roleid == ownerId)
	{
		result = mask & ACLITEM_ALL_GOPTION_BITS;
		if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
			return result;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * 检查直接授予 roleid（而非 public）的权限
	 */
	for (i = 0; i < num; i++)
	{
		AclItem    *aidata = &aidat[i];

		if (aidata->ai_grantee == roleid)
		{
			result |= aidata->ai_privs & mask;
			if ((how == ACLMASK_ALL) ? (result == mask) : (result != 0))
				return result;
		}
	}

	return result;
}


/*
 * aclmembers
 *		找出一个 Acl 中提及的全部 roleid。
 *		注意我们不区分授权者与被授权者。
 *
 * *roleids 被设为指向一个 palloc 出来的数组，其中按排
 * 序含去重 OID。数组长度即函数返回值。
 */
int
aclmembers(const Acl *acl, Oid **roleids)
{
	Oid		   *list;
	const AclItem *acldat;
	int			i,
				j;

	if (acl == NULL || ACL_NUM(acl) == 0)
	{
		*roleids = NULL;
		return 0;
	}

	check_acl(acl);

	/* 按最坏情形分配所需空间 */
	list = palloc(ACL_NUM(acl) * 2 * sizeof(Oid));
	acldat = ACL_DAT(acl);

	/*
	 * 遍历 ACL，收集提及的 RoleId。
	 */
	j = 0;
	for (i = 0; i < ACL_NUM(acl); i++)
	{
		const AclItem *ai = &acldat[i];

		if (ai->ai_grantee != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantee;
		/* 授权者目前从不会是 PUBLIC，但仍检查一下 */
		if (ai->ai_grantor != ACL_ID_PUBLIC)
			list[j++] = ai->ai_grantor;
	}

	/* 数组排序 */
	qsort(list, j, sizeof(Oid), oid_cmp);

	/*
	 * 本可以把数组 repalloc 缩到最小，但那几乎不值得，
	 * 因为它只是临时内存。
	 */
	*roleids = list;

	/* 从数组中移除重复项 */
	return qunique(list, j, sizeof(Oid), oid_cmp);
}


/*
 * aclinsert（导出函数）
 */
Datum
aclinsert(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclinsert is no longer supported")));

	PG_RETURN_NULL();			/* 让编译器安静 */
}

Datum
aclremove(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aclremove is no longer supported")));

	PG_RETURN_NULL();			/* 让编译器安静 */
}

Datum
aclcontains(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	AclItem    *aip = PG_GETARG_ACLITEM_P(1);
	AclItem    *aidat;
	int			i,
				num;

	check_acl(acl);
	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);
	for (i = 0; i < num; ++i)
	{
		if (aip->ai_grantee == aidat[i].ai_grantee &&
			aip->ai_grantor == aidat[i].ai_grantor &&
			(ACLITEM_GET_RIGHTS(*aip) & ACLITEM_GET_RIGHTS(aidat[i])) == ACLITEM_GET_RIGHTS(*aip))
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

Datum
makeaclitem(PG_FUNCTION_ARGS)
{
	Oid			grantee = PG_GETARG_OID(0);
	Oid			grantor = PG_GETARG_OID(1);
	text	   *privtext = PG_GETARG_TEXT_PP(2);
	bool		goption = PG_GETARG_BOOL(3);
	AclItem    *result;
	AclMode		priv;
	static const priv_map any_priv_map[] = {
		{"SELECT", ACL_SELECT},
		{"INSERT", ACL_INSERT},
		{"UPDATE", ACL_UPDATE},
		{"DELETE", ACL_DELETE},
		{"TRUNCATE", ACL_TRUNCATE},
		{"REFERENCES", ACL_REFERENCES},
		{"TRIGGER", ACL_TRIGGER},
		{"EXECUTE", ACL_EXECUTE},
		{"USAGE", ACL_USAGE},
		{"CREATE", ACL_CREATE},
		{"TEMP", ACL_CREATE_TEMP},
		{"TEMPORARY", ACL_CREATE_TEMP},
		{"CONNECT", ACL_CONNECT},
		{"SET", ACL_SET},
		{"ALTER SYSTEM", ACL_ALTER_SYSTEM},
		{"RULE", 0},			/* 忽略旧 RULE 权限 */
		{NULL, 0}
	};

	priv = convert_any_priv_string(privtext, any_priv_map);

	result = (AclItem *) palloc(sizeof(AclItem));

	result->ai_grantee = grantee;
	result->ai_grantor = grantor;

	ACLITEM_SET_PRIVS_GOPTIONS(*result, priv,
							   (goption ? priv : ACL_NO_RIGHTS));

	PG_RETURN_ACLITEM_P(result);
}


/*
 * convert_any_priv_string: 识别 has_foo_privilege 的权限字符串
 *
 * 我们接受以逗号分隔、大小写不敏感的权限名列表，
 * 产生各权限位按位或的位掩码。对条目之间的空白我
 * 们相当宽容，对条目内部的空白则不然。
 * 允许的权限名以 priv_map 结构体数组的形式给出，
 * 以名字指针为 NULL 的项收尾。
 */
static AclMode
convert_any_priv_string(text *priv_type_text,
						const priv_map *privileges)
{
	AclMode		result = 0;
	char	   *priv_type = text_to_cstring(priv_type_text);
	char	   *chunk;
	char	   *next_chunk;

	/* 此处依赖 priv_type 为私有可修改字符串 */
	for (chunk = priv_type; chunk; chunk = next_chunk)
	{
		int			chunk_len;
		const priv_map *this_priv;

		/* 在逗号处拆分 */
		next_chunk = strchr(chunk, ',');
		if (next_chunk)
			*next_chunk++ = '\0';

		/* 去掉本段的首尾空白 */
		while (*chunk && isspace((unsigned char) *chunk))
			chunk++;
		chunk_len = strlen(chunk);
		while (chunk_len > 0 && isspace((unsigned char) chunk[chunk_len - 1]))
			chunk_len--;
		chunk[chunk_len] = '\0';

		/* 对权限列表做匹配 */
		for (this_priv = privileges; this_priv->name; this_priv++)
		{
			if (pg_strcasecmp(this_priv->name, chunk) == 0)
			{
				result |= this_priv->value;
				break;
			}
		}
		if (!this_priv->name)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized privilege type: \"%s\"", chunk)));
	}

	pfree(priv_type);
	return result;
}


static const char *
convert_aclright_to_string(int aclright)
{
	switch (aclright)
	{
		case ACL_INSERT:
			return "INSERT";
		case ACL_SELECT:
			return "SELECT";
		case ACL_UPDATE:
			return "UPDATE";
		case ACL_DELETE:
			return "DELETE";
		case ACL_TRUNCATE:
			return "TRUNCATE";
		case ACL_REFERENCES:
			return "REFERENCES";
		case ACL_TRIGGER:
			return "TRIGGER";
		case ACL_EXECUTE:
			return "EXECUTE";
		case ACL_USAGE:
			return "USAGE";
		case ACL_CREATE:
			return "CREATE";
		case ACL_CREATE_TEMP:
			return "TEMPORARY";
		case ACL_CONNECT:
			return "CONNECT";
		case ACL_SET:
			return "SET";
		case ACL_ALTER_SYSTEM:
			return "ALTER SYSTEM";
		default:
			elog(ERROR, "unrecognized aclright: %d", aclright);
			return NULL;
	}
}


/*----------
 * 将 aclitem[] 转成表。
 *
 * 示例
 *
 * aclexplode('{=r/joe,foo=a*w/joe}'::aclitem[])
 *
 * 返回该表
 *
 * {{ OID(joe), 0::OID,   'SELECT', false },
 *	{ OID(joe), OID(foo), 'INSERT', true },
 *	{ OID(joe), OID(foo), 'UPDATE', false }}
 *----------
 */
Datum
aclexplode(PG_FUNCTION_ARGS)
{
	Acl		   *acl = PG_GETARG_ACL_P(0);
	FuncCallContext *funcctx;
	int		   *idx;
	AclItem    *aidat;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		check_acl(acl);

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 为结果元组构建 tupdesc（匹配 pg_proc 的输出参数）
		 *
		 */
		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "grantor",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "grantee",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "privilege_type",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "is_grantable",
						   BOOLOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* 为用户上下文分配内存 */
		idx = (int *) palloc(sizeof(int[2]));
		idx[0] = 0;				/* ACL 条目索引 */
		idx[1] = -1;			/* 权限类型计数器 */
		funcctx->user_fctx = (void *) idx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	idx = (int *) funcctx->user_fctx;
	aidat = ACL_DAT(acl);

	/* 此处需测试以防 acl 无条目 */
	while (idx[0] < ACL_NUM(acl))
	{
		AclItem    *aidata;
		AclMode		priv_bit;

		idx[1]++;
		if (idx[1] == N_ACL_RIGHTS)
		{
			idx[1] = 0;
			idx[0]++;
			if (idx[0] >= ACL_NUM(acl)) /* 完 */
				break;
		}
		aidata = &aidat[idx[0]];
		priv_bit = UINT64CONST(1) << idx[1];

		if (ACLITEM_GET_PRIVS(*aidata) & priv_bit)
		{
			Datum		result;
			Datum		values[4];
			bool		nulls[4] = {0};
			HeapTuple	tuple;

			values[0] = ObjectIdGetDatum(aidata->ai_grantor);
			values[1] = ObjectIdGetDatum(aidata->ai_grantee);
			values[2] = CStringGetTextDatum(convert_aclright_to_string(priv_bit));
			values[3] = BoolGetDatum((ACLITEM_GET_GOPTIONS(*aidata) & priv_bit) != 0);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			result = HeapTupleGetDatum(tuple);

			SRF_RETURN_NEXT(funcctx, result);
		}
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * has_table_privilege 变体
 *		它们在 SQL 层面都名为 "has_table_privilege"。
 *		它们接受关系名、关系 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。接受关系 OID 的变体在 OID 不存在
 *		时返回 NULL（而非报错；Postgres
 *		 8.4 之前会失败）。
 */

/*
 * has_table_privilege_name_name
 *		检查用户对表的权限，给定
 *		name username, text tablename, text priv name。
 */
Datum
has_table_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*rolename));
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_name
 *		检查用户对表的权限，给定
 *		text tablename, text priv name。
 *		默认为 current_user
 */
Datum
has_table_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_name_id
 *		检查用户对表的权限，给定
 *		name usename, table oid, text priv name。
 */
Datum
has_table_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_table_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id
 *		检查用户对表的权限，给定
 *		table oid, text priv name。
 *		默认为 current_user
 */
Datum
has_table_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_table_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_name
 *		检查用户对表的权限，给定
 *		roleid, text tablename, text priv name。
 */
Datum
has_table_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	tableoid = convert_table_name(tablename);
	mode = convert_table_priv_string(priv_type_text);

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_table_privilege_id_id
 *		检查用户对表的权限，给定
 *		roleid, table oid, text priv name。
 */
Datum
has_table_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_table_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	aclresult = pg_class_aclcheck(tableoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_table_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的表名，查找并返回其 Oid
 */
static Oid
convert_table_name(text *tablename)
{
	RangeVar   *relrv;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(tablename));

	/* 我们可能根本没有此关系上的权限；不要锁定它。 */
	return RangeVarGetRelid(relrv, NoLock, false);
}

/*
 * convert_table_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_table_priv_string(text *priv_type_text)
{
	static const priv_map table_priv_map[] = {
		{"SELECT", ACL_SELECT},
		{"SELECT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_SELECT)},
		{"INSERT", ACL_INSERT},
		{"INSERT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_INSERT)},
		{"UPDATE", ACL_UPDATE},
		{"UPDATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_UPDATE)},
		{"DELETE", ACL_DELETE},
		{"DELETE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_DELETE)},
		{"TRUNCATE", ACL_TRUNCATE},
		{"TRUNCATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_TRUNCATE)},
		{"REFERENCES", ACL_REFERENCES},
		{"REFERENCES WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_REFERENCES)},
		{"TRIGGER", ACL_TRIGGER},
		{"TRIGGER WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_TRIGGER)},
		{"RULE", 0},			/* 忽略旧 RULE 权限 */
		{"RULE WITH GRANT OPTION", 0},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, table_priv_map);
}

/*
 * has_sequence_privilege 变体
 *		它们在 SQL 层面都名为 "has_sequence_privilege"。
 *		它们接受关系名、关系 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。接受关系 OID 的变体在 OID 不存在
 *		时返回 NULL。
 */

/*
 * has_sequence_privilege_name_name
 *		检查用户对序列的权限，给定
 *		name username, text sequencename, text priv name。
 */
Datum
has_sequence_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *sequencename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			sequenceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*rolename));
	mode = convert_sequence_priv_string(priv_type_text);
	sequenceoid = convert_table_name(sequencename);
	if (get_rel_relkind(sequenceoid) != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						text_to_cstring(sequencename))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_sequence_privilege_name
 *		检查用户对序列的权限，给定
 *		text sequencename, text priv name。
 *		默认为 current_user
 */
Datum
has_sequence_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *sequencename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			sequenceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_sequence_priv_string(priv_type_text);
	sequenceoid = convert_table_name(sequencename);
	if (get_rel_relkind(sequenceoid) != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						text_to_cstring(sequencename))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_sequence_privilege_name_id
 *		检查用户对序列的权限，给定
 *		name usename, sequence oid, text priv name。
 */
Datum
has_sequence_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			sequenceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;
	char		relkind;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_sequence_priv_string(priv_type_text);
	relkind = get_rel_relkind(sequenceoid);
	if (relkind == '\0')
		PG_RETURN_NULL();
	else if (relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						get_rel_name(sequenceoid))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_sequence_privilege_id
 *		检查用户对序列的权限，给定
 *		sequence oid, text priv name。
 *		默认为 current_user
 */
Datum
has_sequence_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			sequenceoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;
	char		relkind;

	roleid = GetUserId();
	mode = convert_sequence_priv_string(priv_type_text);
	relkind = get_rel_relkind(sequenceoid);
	if (relkind == '\0')
		PG_RETURN_NULL();
	else if (relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						get_rel_name(sequenceoid))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_sequence_privilege_id_name
 *		检查用户对序列的权限，给定
 *		roleid, text sequencename, text priv name。
 */
Datum
has_sequence_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *sequencename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			sequenceoid;
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_sequence_priv_string(priv_type_text);
	sequenceoid = convert_table_name(sequencename);
	if (get_rel_relkind(sequenceoid) != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						text_to_cstring(sequencename))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_sequence_privilege_id_id
 *		检查用户对序列的权限，给定
 *		roleid, sequence oid, text priv name。
 */
Datum
has_sequence_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			sequenceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;
	char		relkind;

	mode = convert_sequence_priv_string(priv_type_text);
	relkind = get_rel_relkind(sequenceoid);
	if (relkind == '\0')
		PG_RETURN_NULL();
	else if (relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						get_rel_name(sequenceoid))));

	aclresult = pg_class_aclcheck(sequenceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * convert_sequence_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_sequence_priv_string(text *priv_type_text)
{
	static const priv_map sequence_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{"SELECT", ACL_SELECT},
		{"SELECT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_SELECT)},
		{"UPDATE", ACL_UPDATE},
		{"UPDATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_UPDATE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, sequence_priv_map);
}


/*
 * has_any_column_privilege 变体
 *		它们在 SQL 层面都名为 "has_any_column_privilege"。
 *		它们接受关系名、关系 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		所针对的是该表任一列，否则为 false。接受关系
 *		 OID 的变体在 OID 不存在时返回 NULL。
 */

/*
 * has_any_column_privilege_name_name
 *		检查用户对表任意列的权限，给定
 *		name username, text tablename, text priv name。
 */
Datum
has_any_column_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*rolename));
	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_any_column_privilege_name
 *		检查用户对表任意列的权限，给定
 *		text tablename, text priv name。
 *		默认为 current_user
 */
Datum
has_any_column_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_any_column_privilege_name_id
 *		检查用户对表任意列的权限，给定
 *		name usename, table oid, text priv name。
 */
Datum
has_any_column_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_column_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_any_column_privilege_id
 *		检查用户对表任意列的权限，给定
 *		table oid, text priv name。
 *		默认为 current_user
 */
Datum
has_any_column_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_column_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_any_column_privilege_id_name
 *		检查用户对表任意列的权限，给定
 *		roleid, text tablename, text priv name。
 */
Datum
has_any_column_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			tableoid;
	AclMode		mode;
	AclResult	aclresult;

	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_any_column_privilege_id_id
 *		检查用户对表任意列的权限，给定
 *		roleid, table oid, text priv name。
 */
Datum
has_any_column_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_column_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(tableoid)))
		PG_RETURN_NULL();

	/* 先在表级检查，然后若需要再逐列检查 */
	aclresult = pg_class_aclcheck(tableoid, roleid, mode);
	if (aclresult != ACLCHECK_OK)
		aclresult = pg_attribute_aclcheck_all(tableoid, roleid, mode,
											  ACLMASK_ANY);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}


/*
 * has_column_privilege 变体
 *		它们在 SQL 层面都名为 "has_column_privilege"。
 *		它们接受关系名、关系 OID、用户名、用户
 *		列名、列 attnum、用户名、用户 OID 或
 *		隐式用户=current_user。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。接受关系 OID 的变体在 OID 不存在
 *		时返回 NULL（不抛错），同样，接受整数 attnum
 *		的变体在没有这样的 pg_attribute 条目时也返回 NULL
 *		（不抛错误）。若选中 attisdropped 列，所有变体
 *		都返回 NULL。这些规则旨在避免那些扫描
 *		 pg_attribute 的查询出现不必要的失败。
 *
 */

/*
 * column_privilege_check: 检查列权限，但列或表已删除时
 *		不抛出错误
 *
 * 有权限返回 1，没有返回 0，列/表已删除返回 -1。
 */
static int
column_privilege_check(Oid tableoid, AttrNumber attnum,
					   Oid roleid, AclMode mode)
{
	AclResult	aclresult;
	bool		is_missing = false;

	/*
	 * 若 convert_column_name 失败，我们可以直接返回 -1。
	 */
	if (attnum == InvalidAttrNumber)
		return -1;

	/*
	 * 先检查列级权限。这部分也顺带检查该列是否真的存
	 * 在，所以我们需要在检查表级权限之前
	 * 先做这一步。
	 */
	aclresult = pg_attribute_aclcheck_ext(tableoid, attnum, roleid,
										  mode, &is_missing);
	if (aclresult == ACLCHECK_OK)
		return 1;
	else if (is_missing)
		return -1;

	/* 接着检查我们在表级是否有该权限 */
	aclresult = pg_class_aclcheck_ext(tableoid, roleid, mode, &is_missing);
	if (aclresult == ACLCHECK_OK)
		return 1;
	else if (is_missing)
		return -1;
	else
		return 0;
}

/*
 * has_column_privilege_name_name_name
 *		检查用户对列的权限，给定
 *		name username, text tablename, text colname, text priv name。
 */
Datum
has_column_privilege_name_name_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *column = PG_GETARG_TEXT_PP(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			roleid;
	Oid			tableoid;
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	roleid = get_role_oid_or_public(NameStr(*rolename));
	tableoid = convert_table_name(tablename);
	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_name_name_attnum
 *		检查用户对列的权限，给定
 *		name username, text tablename, int attnum, text priv name。
 */
Datum
has_column_privilege_name_name_attnum(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	AttrNumber	colattnum = PG_GETARG_INT16(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	int			privresult;

	roleid = get_role_oid_or_public(NameStr(*rolename));
	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_name_id_name
 *		检查用户对列的权限，给定
 *		name username, table oid, text colname, text priv name。
 */
Datum
has_column_privilege_name_id_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *column = PG_GETARG_TEXT_PP(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			roleid;
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_name_id_attnum
 *		检查用户对列的权限，给定
 *		name username, table oid, int attnum, text priv name。
 */
Datum
has_column_privilege_name_id_attnum(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tableoid = PG_GETARG_OID(1);
	AttrNumber	colattnum = PG_GETARG_INT16(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			roleid;
	AclMode		mode;
	int			privresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_name_name
 *		检查用户对列的权限，给定
 *		oid roleid, text tablename, text colname, text priv name。
 */
Datum
has_column_privilege_id_name_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	text	   *column = PG_GETARG_TEXT_PP(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			tableoid;
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	tableoid = convert_table_name(tablename);
	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_name_attnum
 *		检查用户对列的权限，给定
 *		oid roleid, text tablename, int attnum, text priv name。
 */
Datum
has_column_privilege_id_name_attnum(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablename = PG_GETARG_TEXT_PP(1);
	AttrNumber	colattnum = PG_GETARG_INT16(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	Oid			tableoid;
	AclMode		mode;
	int			privresult;

	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_id_name
 *		检查用户对列的权限，给定
 *		oid roleid, table oid, text colname, text priv name。
 */
Datum
has_column_privilege_id_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tableoid = PG_GETARG_OID(1);
	text	   *column = PG_GETARG_TEXT_PP(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_id_attnum
 *		检查用户对列的权限，给定
 *		oid roleid, table oid, int attnum, text priv name。
 */
Datum
has_column_privilege_id_id_attnum(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tableoid = PG_GETARG_OID(1);
	AttrNumber	colattnum = PG_GETARG_INT16(2);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(3);
	AclMode		mode;
	int			privresult;

	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_name_name
 *		检查用户对列的权限，给定
 *		text tablename, text colname, text priv name。
 *		默认为 current_user
 */
Datum
has_column_privilege_name_name(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	text	   *column = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			tableoid;
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	roleid = GetUserId();
	tableoid = convert_table_name(tablename);
	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_name_attnum
 *		检查用户对列的权限，给定
 *		text tablename, int attnum, text priv name。
 *		默认为 current_user
 */
Datum
has_column_privilege_name_attnum(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	AttrNumber	colattnum = PG_GETARG_INT16(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			tableoid;
	AclMode		mode;
	int			privresult;

	roleid = GetUserId();
	tableoid = convert_table_name(tablename);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_name
 *		检查用户对列的权限，给定
 *		table oid, text colname, text priv name。
 *		默认为 current_user
 */
Datum
has_column_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *column = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AttrNumber	colattnum;
	AclMode		mode;
	int			privresult;

	roleid = GetUserId();
	colattnum = convert_column_name(tableoid, column);
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 * has_column_privilege_id_attnum
 *		检查用户对列的权限，给定
 *		table oid, int attnum, text priv name。
 *		默认为 current_user
 */
Datum
has_column_privilege_id_attnum(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	AttrNumber	colattnum = PG_GETARG_INT16(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	int			privresult;

	roleid = GetUserId();
	mode = convert_column_priv_string(priv_type_text);

	privresult = column_privilege_check(tableoid, colattnum, roleid, mode);
	if (privresult < 0)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(privresult);
}

/*
 *		has_column_privilege 家族的支撑例程。
 */

/*
 * 给定表 OID 与字符串形式的列名，查找并
 * 返回列号。若调用者应返回 NULL 而非失败，则
 * 返回 InvalidAttrNumber。
 */
static AttrNumber
convert_column_name(Oid tableoid, text *column)
{
	char	   *colname;
	HeapTuple	attTuple;
	AttrNumber	attnum;

	colname = text_to_cstring(column);

	/*
	 * 我们不用 get_attnum()，因为它会把已删除列报
	 * 告为不存在。我们需要区别对待已删除列与
	 * 不存在的列。
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(tableoid),
							   CStringGetDatum(colname));
	if (HeapTupleIsValid(attTuple))
	{
		Form_pg_attribute attributeForm;

		attributeForm = (Form_pg_attribute) GETSTRUCT(attTuple);
		/* 对已删除列要返回 NULL */
		if (attributeForm->attisdropped)
			attnum = InvalidAttrNumber;
		else
			attnum = attributeForm->attnum;
		ReleaseSysCache(attTuple);
	}
	else
	{
		char	   *tablename = get_rel_name(tableoid);

		/*
		 * 若表 OID 无效或刚删除，我们将得到 NULL。这种
		 * 情况下我们希望 has_column_privilege 也返回 NULL，
		 * 所以只需返回 InvalidAttrNumber。
		 */
		if (tablename != NULL)
		{
			/* tableoid 存在但列名不存在，故报错 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname, tablename)));
		}
		/* tableoid 不存在，故按 attisdropped 情形处理 */
		attnum = InvalidAttrNumber;
	}

	pfree(colname);
	return attnum;
}

/*
 * convert_column_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_column_priv_string(text *priv_type_text)
{
	static const priv_map column_priv_map[] = {
		{"SELECT", ACL_SELECT},
		{"SELECT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_SELECT)},
		{"INSERT", ACL_INSERT},
		{"INSERT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_INSERT)},
		{"UPDATE", ACL_UPDATE},
		{"UPDATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_UPDATE)},
		{"REFERENCES", ACL_REFERENCES},
		{"REFERENCES WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_REFERENCES)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, column_priv_map);
}


/*
 * has_database_privilege 变体
 *		它们在 SQL 层面都名为 "has_database_privilege"。
 *		接受数据库名、数据库 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false，对象不存在则返回 NULL。
 */

/*
 * has_database_privilege_name_name
 *		检查用户对数据库权限，给定
 *		name username, text databasename, text priv name。
 */
Datum
has_database_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *databasename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_name
 *		检查用户对数据库权限，给定
 *		text databasename, text priv name。
 *		默认为 current_user
 */
Datum
has_database_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *databasename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_name_id
 *		检查用户对数据库权限，给定
 *		name usename, database oid, text priv name。
 */
Datum
has_database_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			databaseoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_database_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(databaseoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id
 *		检查用户对数据库权限，给定
 *		database oid, text priv name。
 *		默认为 current_user
 */
Datum
has_database_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			databaseoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_database_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(databaseoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_name
 *		检查用户对数据库权限，给定
 *		roleid, text databasename, text priv name。
 */
Datum
has_database_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *databasename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			databaseoid;
	AclMode		mode;
	AclResult	aclresult;

	databaseoid = convert_database_name(databasename);
	mode = convert_database_priv_string(priv_type_text);

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_database_privilege_id_id
 *		检查用户对数据库权限，给定
 *		roleid, database oid, text priv name。
 */
Datum
has_database_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			databaseoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_database_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(databaseoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(DatabaseRelationId, databaseoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_database_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的数据库名，查找并返回其 Oid
 */
static Oid
convert_database_name(text *databasename)
{
	char	   *dbname = text_to_cstring(databasename);

	return get_database_oid(dbname, false);
}

/*
 * convert_database_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_database_priv_string(text *priv_type_text)
{
	static const priv_map database_priv_map[] = {
		{"CREATE", ACL_CREATE},
		{"CREATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"TEMPORARY", ACL_CREATE_TEMP},
		{"TEMPORARY WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP)},
		{"TEMP", ACL_CREATE_TEMP},
		{"TEMP WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE_TEMP)},
		{"CONNECT", ACL_CONNECT},
		{"CONNECT WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CONNECT)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, database_priv_map);
}


/*
 * has_foreign_data_wrapper_privilege 变体
 *		它们在 SQL 层面都名为 "has_foreign_data_wrapper_privilege"。
 *		它们接受外部数据包装器名、
 *		fdw OID、用户名、用户OID 或 current_user。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。
 */

/*
 * has_foreign_data_wrapper_privilege_name_name
 *		检查用户对外部数据包装器权限，给定
 *		name username, text fdwname, text priv name。
 */
Datum
has_foreign_data_wrapper_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *fdwname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			fdwid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	fdwid = convert_foreign_data_wrapper_name(fdwname);
	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_foreign_data_wrapper_privilege_name
 *		检查用户对外部数据包装器权限，给定
 *		text fdwname, text priv name。
 *		默认为 current_user
 */
Datum
has_foreign_data_wrapper_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *fdwname = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			fdwid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	fdwid = convert_foreign_data_wrapper_name(fdwname);
	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_foreign_data_wrapper_privilege_name_id
 *		检查用户对外部数据包装器权限，给定
 *		name usename, foreign-data wrapper oid, text priv name。
 */
Datum
has_foreign_data_wrapper_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			fdwid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_foreign_data_wrapper_privilege_id
 *		检查用户对外部数据包装器权限，给定
 *		foreign-data wrapper oid, text priv name。
 *		默认为 current_user
 */
Datum
has_foreign_data_wrapper_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			fdwid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_foreign_data_wrapper_privilege_id_name
 *		检查用户对外部数据包装器权限，给定
 *		roleid, text fdwname, text priv name。
 */
Datum
has_foreign_data_wrapper_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *fdwname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			fdwid;
	AclMode		mode;
	AclResult	aclresult;

	fdwid = convert_foreign_data_wrapper_name(fdwname);
	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_foreign_data_wrapper_privilege_id_id
 *		检查用户对外部数据包装器权限，给定
 *		roleid, fdw oid, text priv name。
 */
Datum
has_foreign_data_wrapper_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			fdwid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_foreign_data_wrapper_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdwid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_foreign_data_wrapper_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的 FDW 名，查找并返回其 Oid
 */
static Oid
convert_foreign_data_wrapper_name(text *fdwname)
{
	char	   *fdwstr = text_to_cstring(fdwname);

	return get_foreign_data_wrapper_oid(fdwstr, false);
}

/*
 * convert_foreign_data_wrapper_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_foreign_data_wrapper_priv_string(text *priv_type_text)
{
	static const priv_map foreign_data_wrapper_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, foreign_data_wrapper_priv_map);
}


/*
 * has_function_privilege 变体
 *		它们在 SQL 层面都名为 "has_function_privilege"。
 *		它们接受函数名、函数 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false，对象不存在则返回 NULL。
 */

/*
 * has_function_privilege_name_name
 *		检查用户对函数的权限，给定
 *		name username, text functionname, text priv name。
 */
Datum
has_function_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *functionname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_name
 *		检查用户对函数的权限，给定
 *		text functionname, text priv name。
 *		默认为 current_user
 */
Datum
has_function_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *functionname = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_name_id
 *		检查用户对函数的权限，给定
 *		name usename, function oid, text priv name。
 */
Datum
has_function_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			functionoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_function_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(functionoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id
 *		检查用户对函数的权限，给定
 *		function oid, text priv name。
 *		默认为 current_user
 */
Datum
has_function_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			functionoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_function_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(functionoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_name
 *		检查用户对函数的权限，给定
 *		roleid, text functionname, text priv name。
 */
Datum
has_function_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *functionname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			functionoid;
	AclMode		mode;
	AclResult	aclresult;

	functionoid = convert_function_name(functionname);
	mode = convert_function_priv_string(priv_type_text);

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_function_privilege_id_id
 *		检查用户对函数的权限，给定
 *		roleid, function oid, text priv name。
 */
Datum
has_function_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			functionoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_function_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(functionoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ProcedureRelationId, functionoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_function_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的函数名，查找并返回其 Oid
 */
static Oid
convert_function_name(text *functionname)
{
	char	   *funcname = text_to_cstring(functionname);
	Oid			oid;

	oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
											   CStringGetDatum(funcname)));

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function \"%s\" does not exist", funcname)));

	return oid;
}

/*
 * convert_function_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_function_priv_string(text *priv_type_text)
{
	static const priv_map function_priv_map[] = {
		{"EXECUTE", ACL_EXECUTE},
		{"EXECUTE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_EXECUTE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, function_priv_map);
}


/*
 * has_language_privilege 变体
 *		它们在 SQL 层面都名为 "has_language_privilege"。
 *		它们接受语言名、语言 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false，对象不存在则返回 NULL。
 */

/*
 * has_language_privilege_name_name
 *		检查用户对语言的权限，给定
 *		name username, text languagename, text priv name。
 */
Datum
has_language_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *languagename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_name
 *		检查用户对语言的权限，给定
 *		text languagename, text priv name。
 *		默认为 current_user
 */
Datum
has_language_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *languagename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_name_id
 *		检查用户对语言的权限，给定
 *		name usename, language oid, text priv name。
 */
Datum
has_language_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			languageoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_language_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(LANGOID, ObjectIdGetDatum(languageoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id
 *		检查用户对语言的权限，给定
 *		language oid, text priv name。
 *		默认为 current_user
 */
Datum
has_language_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			languageoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_language_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(LANGOID, ObjectIdGetDatum(languageoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_name
 *		检查用户对语言的权限，给定
 *		roleid, text languagename, text priv name。
 */
Datum
has_language_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *languagename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			languageoid;
	AclMode		mode;
	AclResult	aclresult;

	languageoid = convert_language_name(languagename);
	mode = convert_language_priv_string(priv_type_text);

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_language_privilege_id_id
 *		检查用户对语言的权限，给定
 *		roleid, language oid, text priv name。
 */
Datum
has_language_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			languageoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_language_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(LANGOID, ObjectIdGetDatum(languageoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(LanguageRelationId, languageoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_language_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的语言名，查找并返回其 Oid
 */
static Oid
convert_language_name(text *languagename)
{
	char	   *langname = text_to_cstring(languagename);

	return get_language_oid(langname, false);
}

/*
 * convert_language_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_language_priv_string(text *priv_type_text)
{
	static const priv_map language_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, language_priv_map);
}


/*
 * has_schema_privilege 变体
 *		它们在 SQL 层面都名为 "has_schema_privilege"。
 *		它们接受模式名、模式 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false，对象不存在则返回 NULL。
 */

/*
 * has_schema_privilege_name_name
 *		检查用户对模式的权限，给定
 *		name username, text schemaname, text priv name。
 */
Datum
has_schema_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *schemaname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_name
 *		检查用户对模式的权限，给定
 *		text schemaname, text priv name。
 *		默认为 current_user
 */
Datum
has_schema_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *schemaname = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_name_id
 *		检查用户对模式的权限，给定
 *		name usename, schema oid, text priv name。
 */
Datum
has_schema_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			schemaoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_schema_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id
 *		检查用户对模式的权限，给定
 *		schema oid, text priv name。
 *		默认为 current_user
 */
Datum
has_schema_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			schemaoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_schema_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_name
 *		检查用户对模式的权限，给定
 *		roleid, text schemaname, text priv name。
 */
Datum
has_schema_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *schemaname = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			schemaoid;
	AclMode		mode;
	AclResult	aclresult;

	schemaoid = convert_schema_name(schemaname);
	mode = convert_schema_priv_string(priv_type_text);

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_schema_privilege_id_id
 *		检查用户对模式的权限，给定
 *		roleid, schema oid, text priv name。
 */
Datum
has_schema_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			schemaoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_schema_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(NamespaceRelationId, schemaoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_schema_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的模式名，查找并返回其 Oid
 */
static Oid
convert_schema_name(text *schemaname)
{
	char	   *nspname = text_to_cstring(schemaname);

	return get_namespace_oid(nspname, false);
}

/*
 * convert_schema_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_schema_priv_string(text *priv_type_text)
{
	static const priv_map schema_priv_map[] = {
		{"CREATE", ACL_CREATE},
		{"CREATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, schema_priv_map);
}


/*
 * has_server_privilege 变体
 *		它们在 SQL 层面都名为 "has_server_privilege"。
 *		它们接受外部服务器名、
 *		server OID、用户名、用户OID 或 current_user。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。
 */

/*
 * has_server_privilege_name_name
 *		检查用户对外部服务器权限，给定
 *		name username, text servername, text priv name。
 */
Datum
has_server_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *servername = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			serverid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	serverid = convert_server_name(servername);
	mode = convert_server_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_server_privilege_name
 *		检查用户对外部服务器权限，给定
 *		text servername, text priv name。
 *		默认为 current_user
 */
Datum
has_server_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *servername = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			serverid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	serverid = convert_server_name(servername);
	mode = convert_server_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_server_privilege_name_id
 *		检查用户对外部服务器权限，给定
 *		name usename, foreign server oid, text priv name。
 */
Datum
has_server_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			serverid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_server_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_server_privilege_id
 *		检查用户对外部服务器权限，给定
 *		server oid, text priv name。
 *		默认为 current_user
 */
Datum
has_server_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			serverid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_server_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_server_privilege_id_name
 *		检查用户对外部服务器权限，给定
 *		roleid, text servername, text priv name。
 */
Datum
has_server_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *servername = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			serverid;
	AclMode		mode;
	AclResult	aclresult;

	serverid = convert_server_name(servername);
	mode = convert_server_priv_string(priv_type_text);

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_server_privilege_id_id
 *		检查用户对外部服务器权限，给定
 *		roleid, server oid, text priv name。
 */
Datum
has_server_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			serverid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_server_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(ForeignServerRelationId, serverid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_server_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的服务器名，查找并返回其 Oid
 */
static Oid
convert_server_name(text *servername)
{
	char	   *serverstr = text_to_cstring(servername);

	return get_foreign_server_oid(serverstr, false);
}

/*
 * convert_server_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_server_priv_string(text *priv_type_text)
{
	static const priv_map server_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, server_priv_map);
}


/*
 * has_tablespace_privilege 变体
 *		它们在 SQL 层面都名为 "has_tablespace_privilege"。
 *		它们接受表空间名、表空间 OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。
 */

/*
 * has_tablespace_privilege_name_name
 *		检查用户对表空间的权限，给定
 *		name username, text tablespacename, text priv name。
 */
Datum
has_tablespace_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *tablespacename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_name
 *		检查用户对表空间的权限，给定
 *		text tablespacename, text priv name。
 *		默认为 current_user
 */
Datum
has_tablespace_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *tablespacename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_name_id
 *		检查用户对表空间的权限，给定
 *		name usename, tablespace oid, text priv name。
 */
Datum
has_tablespace_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			tablespaceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_tablespace_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TABLESPACEOID, ObjectIdGetDatum(tablespaceoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id
 *		检查用户对表空间的权限，给定
 *		tablespace oid, text priv name。
 *		默认为 current_user
 */
Datum
has_tablespace_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			tablespaceoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_tablespace_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TABLESPACEOID, ObjectIdGetDatum(tablespaceoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id_name
 *		检查用户对表空间的权限，给定
 *		roleid, text tablespacename, text priv name。
 */
Datum
has_tablespace_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *tablespacename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			tablespaceoid;
	AclMode		mode;
	AclResult	aclresult;

	tablespaceoid = convert_tablespace_name(tablespacename);
	mode = convert_tablespace_priv_string(priv_type_text);

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_tablespace_privilege_id_id
 *		检查用户对表空间的权限，给定
 *		roleid, tablespace oid, text priv name。
 */
Datum
has_tablespace_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			tablespaceoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_tablespace_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TABLESPACEOID, ObjectIdGetDatum(tablespaceoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TableSpaceRelationId, tablespaceoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_tablespace_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的表空间名，查找并返回其 Oid
 */
static Oid
convert_tablespace_name(text *tablespacename)
{
	char	   *spcname = text_to_cstring(tablespacename);

	return get_tablespace_oid(spcname, false);
}

/*
 * convert_tablespace_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_tablespace_priv_string(text *priv_type_text)
{
	static const priv_map tablespace_priv_map[] = {
		{"CREATE", ACL_CREATE},
		{"CREATE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, tablespace_priv_map);
}

/*
 * has_type_privilege 变体
 *		它们在 SQL 层面都名为 "has_type_privilege"。
 *		它们接受类型名、类型OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false，对象不存在则返回 NULL。
 */

/*
 * has_type_privilege_name_name
 *		检查用户对类型权限，给定
 *		name username, text typename, text priv name。
 */
Datum
has_type_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *typename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			typeoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	typeoid = convert_type_name(typename);
	mode = convert_type_priv_string(priv_type_text);

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_type_privilege_name
 *		检查用户对类型权限，给定
 *		text typename, text priv name。
 *		默认为 current_user
 */
Datum
has_type_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *typename = PG_GETARG_TEXT_PP(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			typeoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	typeoid = convert_type_name(typename);
	mode = convert_type_priv_string(priv_type_text);

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_type_privilege_name_id
 *		检查用户对类型权限，给定
 *		name usename, type oid, text priv name。
 */
Datum
has_type_privilege_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			typeoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid_or_public(NameStr(*username));
	mode = convert_type_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(typeoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_type_privilege_id
 *		检查用户对类型权限，给定
 *		type oid, text priv name。
 *		默认为 current_user
 */
Datum
has_type_privilege_id(PG_FUNCTION_ARGS)
{
	Oid			typeoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_type_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(typeoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_type_privilege_id_name
 *		检查用户对类型权限，给定
 *		roleid, text typename, text priv name。
 */
Datum
has_type_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *typename = PG_GETARG_TEXT_PP(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			typeoid;
	AclMode		mode;
	AclResult	aclresult;

	typeoid = convert_type_name(typename);
	mode = convert_type_priv_string(priv_type_text);

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * has_type_privilege_id_id
 *		检查用户对类型权限，给定
 *		roleid, type oid, text priv name。
 */
Datum
has_type_privilege_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			typeoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_type_priv_string(priv_type_text);

	if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(typeoid)))
		PG_RETURN_NULL();

	aclresult = object_aclcheck(TypeRelationId, typeoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		has_type_privilege 家族的支撑例程。
 */

/*
 * 给定以字符串表示的类型名，查找并返回其 Oid
 */
static Oid
convert_type_name(text *typename)
{
	char	   *typname = text_to_cstring(typename);
	Oid			oid;

	oid = DatumGetObjectId(DirectFunctionCall1(regtypein,
											   CStringGetDatum(typname)));

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist", typname)));

	return oid;
}

/*
 * convert_type_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_type_priv_string(text *priv_type_text)
{
	static const priv_map type_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_USAGE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, type_priv_map);
}

/*
 * has_parameter_privilege 变体
 *		它们在 SQL 层面都名为 "has_parameter_privilege"。
 *		它们接受参数名与用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户获授指明权限则 true，
 *		否则为 false。
 */

/*
 * has_param_priv_byname
 *
 *		辅助函数：在给定角色 Oid、text 参数名与 AclMode
 *		权限时，检查用户对参数的权限。
 */
static bool
has_param_priv_byname(Oid roleid, const text *parameter, AclMode priv)
{
	char	   *paramstr = text_to_cstring(parameter);

	return pg_parameter_aclcheck(paramstr, roleid, priv) == ACLCHECK_OK;
}

/*
 * has_parameter_privilege_name_name
 *		检查用户对参数的权限，给定 name username、text
 *		 parameter、text priv name。
 */
Datum
has_parameter_privilege_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	text	   *parameter = PG_GETARG_TEXT_PP(1);
	AclMode		priv = convert_parameter_priv_string(PG_GETARG_TEXT_PP(2));
	Oid			roleid = get_role_oid_or_public(NameStr(*username));

	PG_RETURN_BOOL(has_param_priv_byname(roleid, parameter, priv));
}

/*
 * has_parameter_privilege_name
 *		检查用户对参数的权限，给定 text parameter、text priv
 *		 name。默认为 current_user
 */
Datum
has_parameter_privilege_name(PG_FUNCTION_ARGS)
{
	text	   *parameter = PG_GETARG_TEXT_PP(0);
	AclMode		priv = convert_parameter_priv_string(PG_GETARG_TEXT_PP(1));

	PG_RETURN_BOOL(has_param_priv_byname(GetUserId(), parameter, priv));
}

/*
 * has_parameter_privilege_id_name
 *		检查用户对参数的权限，给定 roleid、text parameter、
 *		 priv name。
 */
Datum
has_parameter_privilege_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	text	   *parameter = PG_GETARG_TEXT_PP(1);
	AclMode		priv = convert_parameter_priv_string(PG_GETARG_TEXT_PP(2));

	PG_RETURN_BOOL(has_param_priv_byname(roleid, parameter, priv));
}

/*
 *		has_parameter_privilege 家族的支撑例程。
 */

/*
 * convert_parameter_priv_string
 *		把文本串转换为 AclMode 值。
 */
static AclMode
convert_parameter_priv_string(text *priv_text)
{
	static const priv_map parameter_priv_map[] = {
		{"SET", ACL_SET},
		{"SET WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_SET)},
		{"ALTER SYSTEM", ACL_ALTER_SYSTEM},
		{"ALTER SYSTEM WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_ALTER_SYSTEM)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_text, parameter_priv_map);
}

/*
 * pg_has_role 变体
 *		它们在 SQL 层面都名为 "pg_has_role"。
 *		它们接受角色名、角色OID、用户名、用户
 *		 OID 或隐式用户 = current_user 的组合。
 *
 *		结果是布尔值：用户有指明权限则 true，
 *		否则为 false。
 */

/*
 * pg_has_role_name_name
 *		检查用户对角色权限，给定
 *		name username, name rolename, text priv name。
 */
Datum
pg_has_role_name_name(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Name		rolename = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid(NameStr(*username), false);
	roleoid = get_role_oid(NameStr(*rolename), false);
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_name
 *		检查用户对角色权限，给定
 *		name rolename, text priv name。
 *		默认为 current_user
 */
Datum
pg_has_role_name(PG_FUNCTION_ARGS)
{
	Name		rolename = PG_GETARG_NAME(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	roleoid = get_role_oid(NameStr(*rolename), false);
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_name_id
 *		检查用户对角色权限，给定
 *		name usename, role oid, text priv name。
 */
Datum
pg_has_role_name_id(PG_FUNCTION_ARGS)
{
	Name		username = PG_GETARG_NAME(0);
	Oid			roleoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = get_role_oid(NameStr(*username), false);
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id
 *		检查用户对角色权限，给定
 *		role oid, text priv name。
 *		默认为 current_user
 */
Datum
pg_has_role_id(PG_FUNCTION_ARGS)
{
	Oid			roleoid = PG_GETARG_OID(0);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(1);
	Oid			roleid;
	AclMode		mode;
	AclResult	aclresult;

	roleid = GetUserId();
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id_name
 *		检查用户对角色权限，给定
 *		roleid, name rolename, text priv name。
 */
Datum
pg_has_role_id_name(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Name		rolename = PG_GETARG_NAME(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	Oid			roleoid;
	AclMode		mode;
	AclResult	aclresult;

	roleoid = get_role_oid(NameStr(*rolename), false);
	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 * pg_has_role_id_id
 *		检查用户对角色权限，给定
 *		roleid, role oid, text priv name。
 */
Datum
pg_has_role_id_id(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Oid			roleoid = PG_GETARG_OID(1);
	text	   *priv_type_text = PG_GETARG_TEXT_PP(2);
	AclMode		mode;
	AclResult	aclresult;

	mode = convert_role_priv_string(priv_type_text);

	aclresult = pg_role_aclcheck(roleoid, roleid, mode);

	PG_RETURN_BOOL(aclresult == ACLCHECK_OK);
}

/*
 *		pg_has_role 家族的支撑例程。
 */

/*
 * convert_role_priv_string
 *		把文本串转换为 AclMode 值。
 *
 * 我们用 USAGE 表示角色的权限是否可访问
 * （has_privs_of_role），用 MEMBER 表示 is_member，而
 * MEMBER WITH GRANT（或 ADMIN OPTION）表示 is_admin。没有
 * 与 MEMBER 对应的 ACL 位，故取巧以 ACL_CREATE 代替。
 * 此约定仅 pg_role_aclcheck 使用。
 */
static AclMode
convert_role_priv_string(text *priv_type_text)
{
	static const priv_map role_priv_map[] = {
		{"USAGE", ACL_USAGE},
		{"MEMBER", ACL_CREATE},
		{"SET", ACL_SET},
		{"USAGE WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"USAGE WITH ADMIN OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"MEMBER WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"MEMBER WITH ADMIN OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"SET WITH GRANT OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{"SET WITH ADMIN OPTION", ACL_GRANT_OPTION_FOR(ACL_CREATE)},
		{NULL, 0}
	};

	return convert_any_priv_string(priv_type_text, role_priv_map);
}

/*
 * pg_role_aclcheck
 *		pg_has_role 快速简陋的支持
 */
static AclResult
pg_role_aclcheck(Oid role_oid, Oid roleid, AclMode mode)
{
	if (mode & ACL_GRANT_OPTION_FOR(ACL_CREATE))
	{
		if (is_admin_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	if (mode & ACL_CREATE)
	{
		if (is_member_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	if (mode & ACL_USAGE)
	{
		if (has_privs_of_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	if (mode & ACL_SET)
	{
		if (member_can_set_role(roleid, role_oid))
			return ACLCHECK_OK;
	}
	return ACLCHECK_NO_PRIV;
}


/*
 * 初始化函数（InitPostgres 调用）
 */
void
initialize_acl(void)
{
	if (!IsBootstrapProcessingMode())
	{
		cached_db_hash =
			GetSysCacheHashValue1(DATABASEOID,
								  ObjectIdGetDatum(MyDatabaseId));

		/*
		 * 正常模式下，为 pg_auth_members 行的任何 syscache 失
		 * 效事件设置回调（供 roles_is_member_of() 使用；
		 * pg_database 亦然）
		 */
		CacheRegisterSyscacheCallback(AUTHMEMROLEMEM,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(AUTHOID,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(DATABASEOID,
									  RoleMembershipCacheCallback,
									  (Datum) 0);
	}
}

/*
 * RoleMembershipCacheCallback
 *		Syscache 失效回调函数
 */
static void
RoleMembershipCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	if (cacheid == DATABASEOID &&
		hashvalue != cached_db_hash &&
		hashvalue != 0)
	{
		return;					/* 忽略其他库的 pg_database 变更 */
	}

	/* 强制成员缓存在下次使用时重算 */
	cached_role[ROLERECURSE_MEMBERS] = InvalidOid;
	cached_role[ROLERECURSE_PRIVS] = InvalidOid;
	cached_role[ROLERECURSE_SETROLE] = InvalidOid;
}

/*
 * 取得指定 roleid 作为成员所属的角色列表
 *
 * ROLERECURSE_MEMBERS 类型经全部授权递归；ROLERECURSE_PRIVS
 * 仅经由可继承授权递归；ROLERECURSE_SETROLE 仅经由带
 *  set_option 的授权递归。
 *
 * 由于间接成员测试代价相对较高，我们缓存一份
 * 成员关系列表。因此，结果只保证在下一次调用
 * roles_is_member_of() 之前有效！
 *
 * 为方便 select_best_grantor，结果定义为按广度优先顺
 * 序排列，即更近的关系在前。
 *
 * 若 admin_of 不是 InvalidOid，本函数会设置 *admin_role，
 * 其值为结果列表中第一个在 admin_of 对应角色上直接
 * 持有 ADMIN OPTION 的角色 OID，若无这样的角色则为其
 * InvalidOid。
 */
static List *
roles_is_member_of(Oid roleid, enum RoleRecurseType type,
				   Oid admin_of, Oid *admin_role)
{
	Oid			dba;
	List	   *roles_list;
	ListCell   *l;
	List	   *new_cached_roles;
	MemoryContext oldctx;

	Assert(OidIsValid(admin_of) == PointerIsValid(admin_role));
	if (admin_role != NULL)
		*admin_role = InvalidOid;

	/* 若缓存有效且未要求 ADMIN OPTION，则直接返回列表 */
	if (cached_role[type] == roleid && !OidIsValid(admin_of) &&
		OidIsValid(cached_role[type]))
		return cached_roles[type];

	/*
	 * 角色展开发生在非数据库后端中，当 guc.c 为物理
	 * walsender 执行 SHOW 命令检查 ROLE_PG_READ_ALL_SETTINGS
	 * 时无角色获得 pg_database_owner。
	 */
	if (!OidIsValid(MyDatabaseId))
		dba = InvalidOid;
	else
	{
		HeapTuple	dbtup;

		dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(dbtup))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		dba = ((Form_pg_database) GETSTRUCT(dbtup))->datdba;
		ReleaseSysCache(dbtup);
	}

	/*
	 * 找出 roleid 作为成员所涉及的全部角色，包括多级
	 * 递归。角色本身总是结果的
	 * 首元素。
	 *
	 * 列表中每个元素都被扫描，看是否带来间接
	 * 成员关系。我们可以用单个列表同时充当
	 * 已发现成员关系的记录与待扫描角色的议程。
	 * 这有点巧妙但行得通，因为 foreach() 宏直到循环底
	 * 部才获取下一个列表元素。
	 */
	roles_list = list_make1_oid(roleid);

	foreach(l, roles_list)
	{
		Oid			memberid = lfirst_oid(l);
		CatCList   *memlist;
		int			i;

		/* 查找 memberid 直接作为成员的角色 */
		memlist = SearchSysCacheList1(AUTHMEMMEMROLE,
									  ObjectIdGetDatum(memberid));
		for (i = 0; i < memlist->n_members; i++)
		{
			HeapTuple	tup = &memlist->members[i]->tuple;
			Form_pg_auth_members form = (Form_pg_auth_members) GETSTRUCT(tup);
			Oid			otherid = form->roleid;

			/*
			 * 虽然 otherid==InvalidOid 不应出现在目录中，但
			 * OidIsValid() 可在发生时避免崩溃。
			 */
			if (otherid == admin_of && form->admin_option &&
				OidIsValid(admin_of) && !OidIsValid(*admin_role))
				*admin_role = memberid;

			/* 若应忽略不可继承的授权，就这么做。 */
			if (type == ROLERECURSE_PRIVS && !form->inherit_option)
				continue;

			/* 若应忽略非 SET 授权，就这么做。 */
			if (type == ROLERECURSE_SETROLE && !form->set_option)
				continue;

			/*
			 * 虽然成员关系图不应有环，仍须检查
			 * 已见过此角色。比如同时有 A->B 和 A->C->B 是
			 * 合法的。
			 */
			roles_list = list_append_unique_oid(roles_list, otherid);
		}
		ReleaseSysCacheList(memlist);

		/* 实现 pg_database_owner 的隐式成员关系 */
		if (memberid == dba && OidIsValid(dba))
			roles_list = list_append_unique_oid(roles_list,
												ROLE_PG_DATABASE_OWNER);
	}

	/*
	 * 把完成的列表复制入 TopMemoryContext 以持久保存。
	 */
	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	new_cached_roles = list_copy(roles_list);
	MemoryContextSwitchTo(oldctx);
	list_free(roles_list);

	/*
	 * 现在可安全赋给状态变量
	 */
	cached_role[type] = InvalidOid; /* 以防万一 */
	list_free(cached_roles[type]);
	cached_roles[type] = new_cached_roles;
	cached_role[type] = roleid;

	/* 现在可以返回答案了 */
	return cached_roles[type];
}


/*
 * member 是否拥有 role 的权限（直接或间接）？
 *
 * 本函数被定义为不经由不可继承的授权递归，且只有
 * 可继承的授权才自动赋予相关权限。
 *
 * 另见下方 member_can_set_role。
 */
bool
has_privs_of_role(Oid member, Oid role)
{
	/* 简单情形快速路径 */
	if (member == role)
		return true;

	/* 超级用户拥有全部权限，故属于每个角色 */
	if (superuser_arg(member))
		return true;

	/*
	 * 找出 member 拥有其权限的全部角色，包括多级
	 * 递归，然后看目标角色是否为其中之一。
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_PRIVS,
											  InvalidOid, NULL),
						   role);
}

/*
 * member 能 SET ROLE 到该角色？
 *
 * 必须存在一条从 'member' 到 'role' 的授权链，其中每一
 * 个授权都允许 SET ROLE，即 set_option 均为 true。
 *
 * 授权是否可继承并不重要。那是另一个问题；见
 * has_privs_of_role。
 *
 * 本函数应当用于判定会话用户能否用 SET ROLE 变身为
 * 目标用户。我们也用它判定会话用户可否把既有对象
 * 改为由目标用户拥有，或创建归其所有的新对象。
 *
 */
bool
member_can_set_role(Oid member, Oid role)
{
	/* 简单情形快速路径 */
	if (member == role)
		return true;

	/* 超级用户拥有全部权限，故总能 SET ROLE */
	if (superuser_arg(member))
		return true;

	/*
	 * 找出 member 经 SET ROLE 可访问的角色，包括多级
	 * 递归，然后看目标角色是否为其中之一。
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_SETROLE,
											  InvalidOid, NULL),
						   role);
}

/*
 * 除非能 SET ROLE 到目标角色，否则报权限错误。
 */
void
check_can_set_role(Oid member, Oid role)
{
	if (!member_can_set_role(member, role))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be able to SET ROLE \"%s\"",
						GetUserNameFromId(role, false))));
}

/*
 * member 是 role 的成员（直接或间接）吗？
 *
 * 本函数被定义为无论授权可继承与否都经由其递归。
 *
 * 不要用本函数做权限检查，请改用 has_privs_of_role()
 * 也不要用它判定能否对某角色 SET ROLE；那要用
 * member_can_set_role()。
 * 也不要用它判定创建归其他角色所有的对象是否可行：
 * 同样用 member_can_set_role()。
 *
 * 简而言之，在几乎所有场合调用本函数都是错误的做法。
 */
bool
is_member_of_role(Oid member, Oid role)
{
	/* 简单情形快速路径 */
	if (member == role)
		return true;

	/* 超级用户拥有全部权限，故属于每个角色 */
	if (superuser_arg(member))
		return true;

	/*
	 * 找出 member 所属的所有角色，包括多级
	 * 递归，然后看目标角色是否为其中之一。
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_MEMBERS,
											  InvalidOid, NULL),
						   role);
}

/*
 * member 是 role 成员吗（不计超级用户身份）？
 *
 * 本函数与 is_member_of_role 相同，唯忽略超级用户身
 * 份。
 *
 * 不要用本函数做权限检查，请改用 has_privs_of_role()
 */
bool
is_member_of_role_nosuper(Oid member, Oid role)
{
	/* 简单情形快速路径 */
	if (member == role)
		return true;

	/*
	 * 找出 member 所属的所有角色，包括多级
	 * 递归，然后看目标角色是否为其中之一。
	 */
	return list_member_oid(roles_is_member_of(member, ROLERECURSE_MEMBERS,
											  InvalidOid, NULL),
						   role);
}


/*
 * member 是否为 role 的管理员？即 member 是 role 本身（受下
 * 述限制）、（直接或间接）持 ADMIN OPTION 的成员，或是
 * 超级用户？
 */
bool
is_admin_of_role(Oid member, Oid role)
{
	Oid			admin_role;

	if (superuser_arg(member))
		return true;

	/* 按策略，角色无法对自身有 WITH ADMIN OPTION。 */
	if (member == role)
		return false;

	(void) roles_is_member_of(member, ROLERECURSE_MEMBERS, role, &admin_role);
	return OidIsValid(admin_role);
}

/*
 * 找出一个其权限被 member 继承、对 "role" 持有 ADMIN
 *  OPTION，不计超级用户。
 *
 * 这样的角色可能不止一个；优先选跳数更少的那
 * 的。也就是说，若 member 持有 ADMIN OPTION，优先选它
 * 而非其他选项；若没有，则优先选择 member 从中更
 * 直接继承权限的角色。
 */
Oid
select_best_admin(Oid member, Oid role)
{
	Oid			admin_role;

	/* 按策略，角色无法对自身有 WITH ADMIN OPTION。 */
	if (member == role)
		return InvalidOid;

	(void) roles_is_member_of(member, ROLERECURSE_PRIVS, role, &admin_role);
	return admin_role;
}


/* 如其所言…… */
static int
count_one_bits(AclMode mask)
{
	int			nbits = 0;

	/* 此代码依赖 AclMode 是无符号类型 */
	while (mask)
	{
		if (mask & 1)
			nbits++;
		mask >>= 1;
	}
	return nbits;
}


/*
 * 为 GRANT 或 REVOKE 操作选择有效授权者 ID。
 *
 * 授权者必须始终是对象属主，或是某个被显式授予了授
 * 权选项的角色。这确保被授予的权限看起来全都出
 * 自对象属主，权限绝无多个"原始来源"。故若
 * 准授权者是某个持有所需授权选项角色的成员，就
 * 必须以该角色执行授予。
 *
 *
 * 可能准授权者是好几个角色的成员，这些角色拥有所
 * 需授权选项的不同子集，但无单一角色全都有。
 * 此时我们挑选拥有最多所需选项的角色。
 * 平手时偏向更近的祖先。
 *
 * roleId: 尝试执行 GRANT/REVOKE 的角色
 * privileges: 要被授予/收回的权限
 * acl: 相关对象的 ACL
 * ownerId: 拥有相关对象的角色
 * *grantorId: 接收用来执行授予的角色 OID
 * *grantOptions: 接收 grantorId 实际持有的授权选项
 *
 * 若无授权选项，则 grantorId 设为 roleId，grantOptions 设 0。
 */
void
select_best_grantor(Oid roleId, AclMode privileges,
					const Acl *acl, Oid ownerId,
					Oid *grantorId, AclMode *grantOptions)
{
	AclMode		needed_goptions = ACL_GRANT_OPTION_FOR(privileges);
	List	   *roles_list;
	int			nrights;
	ListCell   *l;

	/*
	 * 对象属主总被当作持有全部授权选项，若 roleId
	 * 是属主就简单了。同样，若 roleId 是超级用户也很
	 * 简单：超级用户是每个角色的隐式成员，故以
	 * 属主行事。
	 */
	if (roleId == ownerId || superuser_arg(roleId))
	{
		*grantorId = ownerId;
		*grantOptions = needed_goptions;
		return;
	}

	/*
	 * 否则须小心搜索 roleId 是否拥有任何合适角色的
	 * 权限。注意：我们可以在整个循环中一直保留 
	 * roles_is_member_of() 的结果，因为 aclmask_direct() 不会查
	 * 询任何角色成员关系。
	 */
	roles_list = roles_is_member_of(roleId, ROLERECURSE_PRIVS,
									InvalidOid, NULL);

	/* 以默认值初始化候选结果 */
	*grantorId = roleId;
	*grantOptions = ACL_NO_RIGHTS;
	nrights = 0;

	foreach(l, roles_list)
	{
		Oid			otherrole = lfirst_oid(l);
		AclMode		otherprivs;

		otherprivs = aclmask_direct(acl, otherrole, ownerId,
									needed_goptions, ACLMASK_ALL);
		if (otherprivs == needed_goptions)
		{
			/* 找到合适的授权者 */
			*grantorId = otherrole;
			*grantOptions = otherprivs;
			return;
		}

		/*
		 * 若它只有部分所需权限，记住最佳候选
		 * 者。
		 */
		if (otherprivs != ACL_NO_RIGHTS)
		{
			int			nnewrights = count_one_bits(otherprivs);

			if (nnewrights > nrights)
			{
				*grantorId = otherrole;
				*grantOptions = otherprivs;
				nrights = nnewrights;
			}
		}
	}
}

/*
 * get_role_oid - 给定角色名查其 OID。
 *
 * 若 missing_ok 为 false，找不到角色名即报错。若为
 * true 返回 InvalidOid。
 */
Oid
get_role_oid(const char *rolname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(AUTHNAME, Anum_pg_authid_oid,
						  CStringGetDatum(rolname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", rolname)));
	return oid;
}

/*
 * get_role_oid_or_public - 同上，但角色名为 "public" 则
 *		返回ACL_ID_PUBLIC。
 */
Oid
get_role_oid_or_public(const char *rolname)
{
	if (strcmp(rolname, "public") == 0)
		return ACL_ID_PUBLIC;

	return get_role_oid(rolname, false);
}

/*
 * 给定 RoleSpec 节点，返回其对应的 OID。若 missing_ok 为
 * true，角色不存在时返回 InvalidOid。
 *
 * 此处总是不允许 PUBLIC。想要自己处理 PUBLIC 情形的
 * 例程必须单独检查之。
 */
Oid
get_rolespec_oid(const RoleSpec *role, bool missing_ok)
{
	Oid			oid;

	switch (role->roletype)
	{
		case ROLESPEC_CSTRING:
			Assert(role->rolename);
			oid = get_role_oid(role->rolename, missing_ok);
			break;

		case ROLESPEC_CURRENT_ROLE:
		case ROLESPEC_CURRENT_USER:
			oid = GetUserId();
			break;

		case ROLESPEC_SESSION_USER:
			oid = GetSessionUserId();
			break;

		case ROLESPEC_PUBLIC:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", "public")));
			oid = InvalidOid;	/* 让编译器高兴 */
			break;

		default:
			elog(ERROR, "unexpected role type %d", role->roletype);
	}

	return oid;
}

/*
 * 给定 RoleSpec 节点，返回其对应的 pg_authid 堆元组。
 * 用完结果元组后调用者必须 ReleaseSysCache。
 */
HeapTuple
get_rolespec_tuple(const RoleSpec *role)
{
	HeapTuple	tuple;

	switch (role->roletype)
	{
		case ROLESPEC_CSTRING:
			Assert(role->rolename);
			tuple = SearchSysCache1(AUTHNAME, CStringGetDatum(role->rolename));
			if (!HeapTupleIsValid(tuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", role->rolename)));
			break;

		case ROLESPEC_CURRENT_ROLE:
		case ROLESPEC_CURRENT_USER:
			tuple = SearchSysCache1(AUTHOID, GetUserId());
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for role %u", GetUserId());
			break;

		case ROLESPEC_SESSION_USER:
			tuple = SearchSysCache1(AUTHOID, GetSessionUserId());
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for role %u", GetSessionUserId());
			break;

		case ROLESPEC_PUBLIC:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", "public")));
			tuple = NULL;		/* 让编译器高兴 */
			break;

		default:
			elog(ERROR, "unexpected role type %d", role->roletype);
	}

	return tuple;
}

/*
 * 给定 RoleSpec，返回对应角色名的 palloc 出来的副本。
 */
char *
get_rolespec_name(const RoleSpec *role)
{
	HeapTuple	tp;
	Form_pg_authid authForm;
	char	   *rolename;

	tp = get_rolespec_tuple(role);
	authForm = (Form_pg_authid) GETSTRUCT(tp);
	rolename = pstrdup(NameStr(authForm->rolname));
	ReleaseSysCache(tp);

	return rolename;
}

/*
 * 给定 RoleSpec，若名字是保留名则报错，使用 detail_msg
 * （若提供；其必须已被翻译）。
 *
 * 若节点为 NULL，则不报错。若 detail_msg 为 NULL，则不
 * 提供详情。
 */
void
check_rolespec_name(const RoleSpec *role, const char *detail_msg)
{
	if (!role)
		return;

	if (role->roletype != ROLESPEC_CSTRING)
		return;

	if (IsReservedName(role->rolename))
	{
		if (detail_msg)
			ereport(ERROR,
					(errcode(ERRCODE_RESERVED_NAME),
					 errmsg("role name \"%s\" is reserved",
							role->rolename),
					 errdetail_internal("%s", detail_msg)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_RESERVED_NAME),
					 errmsg("role name \"%s\" is reserved",
							role->rolename)));
	}
}
