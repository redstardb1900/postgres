/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  排序规则相关命令的辅助代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct
{
	char	   *localename;		/* locale 的名称，同 "locale -a" 的输出 */
	char	   *alias;			/* 它的缩短别名 */
	int			enc;			/* 编码 */
} CollAliasData;


/*
 * CREATE COLLATION
 */
ObjectAddress
DefineCollation(ParseState *pstate, List *names, List *parameters, bool if_not_exists)
{
	char	   *collName;
	Oid			collNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *fromEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *lccollateEl = NULL;
	DefElem    *lcctypeEl = NULL;
	DefElem    *providerEl = NULL;
	DefElem    *deterministicEl = NULL;
	DefElem    *rulesEl = NULL;
	DefElem    *versionEl = NULL;
	char	   *collcollate;
	char	   *collctype;
	char	   *colliculocale;
	char	   *collicurules;
	bool		collisdeterministic;
	int			collencoding;
	char		collprovider;
	char	   *collversion = NULL;
	Oid			newoid;
	ObjectAddress address;

	collNamespace = QualifiedNameGetCreationNamespace(names, &collName);

	aclresult = object_aclcheck(NamespaceRelationId, collNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(collNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "from") == 0)
			defelp = &fromEl;
		else if (strcmp(defel->defname, "locale") == 0)
			defelp = &localeEl;
		else if (strcmp(defel->defname, "lc_collate") == 0)
			defelp = &lccollateEl;
		else if (strcmp(defel->defname, "lc_ctype") == 0)
			defelp = &lcctypeEl;
		else if (strcmp(defel->defname, "provider") == 0)
			defelp = &providerEl;
		else if (strcmp(defel->defname, "deterministic") == 0)
			defelp = &deterministicEl;
		else if (strcmp(defel->defname, "rules") == 0)
			defelp = &rulesEl;
		else if (strcmp(defel->defname, "version") == 0)
			defelp = &versionEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("collation attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			break;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	if (localeEl && (lccollateEl || lcctypeEl))
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"),
				errdetail("LOCALE cannot be specified together with LC_COLLATE or LC_CTYPE."));

	if (fromEl && list_length(parameters) != 1)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("conflicting or redundant options"),
				errdetail("FROM cannot be specified together with any other options."));

	if (fromEl)
	{
		Oid			collid;
		HeapTuple	tp;
		Datum		datum;
		bool		isnull;

		collid = get_collation_oid(defGetQualifiedName(fromEl), false);
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		collprovider = ((Form_pg_collation) GETSTRUCT(tp))->collprovider;
		collisdeterministic = ((Form_pg_collation) GETSTRUCT(tp))->collisdeterministic;
		collencoding = ((Form_pg_collation) GETSTRUCT(tp))->collencoding;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collcollate, &isnull);
		if (!isnull)
			collcollate = TextDatumGetCString(datum);
		else
			collcollate = NULL;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collctype, &isnull);
		if (!isnull)
			collctype = TextDatumGetCString(datum);
		else
			collctype = NULL;

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_colliculocale, &isnull);
		if (!isnull)
			colliculocale = TextDatumGetCString(datum);
		else
			colliculocale = NULL;

		/*
		 * 当 ICU locale 来自现有排序规则时，
		 * 不要规范化为语言标签。
		 */

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collicurules, &isnull);
		if (!isnull)
			collicurules = TextDatumGetCString(datum);
		else
			collicurules = NULL;

		ReleaseSysCache(tp);

		/*
		 * 不允许复制 "default" 排序规则，因为大多数代码
		 * 检查 DEFAULT_COLLATION_OID 而不是 COLLPROVIDER_DEFAULT，
		 * 所以再有第二个 COLLPROVIDER_DEFAULT 的排序规则将
		 * 行不通，并可能迷惑或弄崩一些代码。
		 * 花些功夫可以修复这一点。
		 */
		if (collprovider == COLLPROVIDER_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("collation \"default\" cannot be copied")));
	}
	else
	{
		char	   *collproviderstr = NULL;

		collcollate = NULL;
		collctype = NULL;
		colliculocale = NULL;
		collicurules = NULL;

		if (providerEl)
			collproviderstr = defGetString(providerEl);

		if (deterministicEl)
			collisdeterministic = defGetBoolean(deterministicEl);
		else
			collisdeterministic = true;

		if (rulesEl)
			collicurules = defGetString(rulesEl);

		if (versionEl)
			collversion = defGetString(versionEl);

		if (collproviderstr)
		{
			if (pg_strcasecmp(collproviderstr, "icu") == 0)
				collprovider = COLLPROVIDER_ICU;
			else if (pg_strcasecmp(collproviderstr, "libc") == 0)
				collprovider = COLLPROVIDER_LIBC;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("unrecognized collation provider: %s",
								collproviderstr)));
		}
		else
			collprovider = COLLPROVIDER_LIBC;

		if (localeEl)
		{
			if (collprovider == COLLPROVIDER_LIBC)
			{
				collcollate = defGetString(localeEl);
				collctype = defGetString(localeEl);
			}
			else
				colliculocale = defGetString(localeEl);
		}

		if (lccollateEl)
			collcollate = defGetString(lccollateEl);

		if (lcctypeEl)
			collctype = defGetString(lcctypeEl);

		if (collprovider == COLLPROVIDER_LIBC)
		{
			if (!collcollate)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"lc_collate")));

			if (!collctype)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"lc_ctype")));
		}
		else if (collprovider == COLLPROVIDER_ICU)
		{
			if (!colliculocale)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("parameter \"%s\" must be specified",
								"locale")));

			/*
			 * 二进制升级期间保留 locale 字符串。
			 * 否则规范化为语言标签。
			 */
			if (!IsBinaryUpgrade)
			{
				char	   *langtag = icu_language_tag(colliculocale,
													   icu_validation_level);

				if (langtag && strcmp(colliculocale, langtag) != 0)
				{
					ereport(NOTICE,
							(errmsg("using standard form \"%s\" for ICU locale \"%s\"",
									langtag, colliculocale)));

					colliculocale = langtag;
				}
			}

			icu_validate_locale(colliculocale);
		}

		/*
		 * 非确定性排序规则目前仅受 ICU 支持，
		 * 因为只有在那里它才真正
		 * 有区别。这样我们就可以省去为其他
		 * 提供者编写代码。
		 */
		if (!collisdeterministic && collprovider != COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations not supported with this provider")));

		if (collicurules && collprovider != COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU rules cannot be specified unless locale provider is ICU")));

		if (collprovider == COLLPROVIDER_ICU)
		{
#ifdef USE_ICU
			/*
			 * 我们可以用 collencoding == 数据库编码创建 ICU
			 * 排序规则，但用 -1 更好，这样与 initdb 创建
			 * ICU 排序规则的方式一致。不过，只有当
			 * 当前数据库的编码受支持时才允许
			 * 创建。否则排序规则毫无用处，而且还会出现
			 * 无法删除该排序规则之类的意外行为。
			 *
			 * 当 !USE_ICU 时跳过此测试，因为我们想为此
			 * 抛出的错误要到后面才会抛出。
			 */
			if (!is_encoding_supported_by_icu(GetDatabaseEncoding()))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("current database's encoding is not supported with this provider")));
#endif
			collencoding = -1;
		}
		else
		{
			collencoding = GetDatabaseEncoding();
			check_encoding_locale_matches(collencoding, collcollate, collctype);
		}
	}

	if (!collversion)
		collversion = get_collation_actual_version(collprovider, collprovider == COLLPROVIDER_ICU ? colliculocale : collcollate);

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 collprovider,
							 collisdeterministic,
							 collencoding,
							 collcollate,
							 collctype,
							 colliculocale,
							 collicurules,
							 collversion,
							 if_not_exists,
							 false);	/* 非静默 */

	if (!OidIsValid(newoid))
		return InvalidObjectAddress;

	/*
	 * 检查这些 locale 能否装载。注意：pg_newlocale_from_collation
	 * 只应针对非 C 等价 locale 调用。
	 */
	CommandCounterIncrement();
	if (!lc_collate_is_c(newoid) || !lc_ctype_is_c(newoid))
		(void) pg_newlocale_from_collation(newoid);

	ObjectAddressSet(address, CollationRelationId, newoid);

	return address;
}

/*
 * ALTER COLLATION SET SCHEMA 与 RENAME 的子例程
 *
 * 给定命名空间中是否已有与给定排序规则同名的
 * 排序规则？若有，抛出合适的错误消息。
 */
void
IsThereCollationInNamespace(const char *collname, Oid nspOid)
{
	/* 确保新模式中还没有这个名称 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collname, GetDatabaseEncodingName(),
						get_namespace_name(nspOid))));

	/* 也不能匹配任何编码条目 */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collname, get_namespace_name(nspOid))));
}

/*
 * ALTER COLLATION
 */
ObjectAddress
AlterCollation(AlterCollationStmt *stmt)
{
	Relation	rel;
	Oid			collOid;
	HeapTuple	tup;
	Form_pg_collation collForm;
	Datum		datum;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;
	ObjectAddress address;

	rel = table_open(CollationRelationId, RowExclusiveLock);
	collOid = get_collation_oid(stmt->collname, false);

	if (collOid == DEFAULT_COLLATION_OID)
		ereport(ERROR,
				(errmsg("cannot refresh version of default collation"),
		/* translator: %s is an SQL command */
				 errhint("Use %s instead.",
						 "ALTER DATABASE ... REFRESH COLLATION VERSION")));

	if (!object_ownercheck(CollationRelationId, collOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_COLLATION,
					   NameListToString(stmt->collname));

	tup = SearchSysCacheCopy1(COLLOID, ObjectIdGetDatum(collOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for collation %u", collOid);

	collForm = (Form_pg_collation) GETSTRUCT(tup);
	datum = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collversion, &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(datum);

	datum = SysCacheGetAttrNotNull(COLLOID, tup, collForm->collprovider == COLLPROVIDER_ICU ? Anum_pg_collation_colliculocale : Anum_pg_collation_collcollate);
	newversion = get_collation_actual_version(collForm->collprovider, TextDatumGetCString(datum));

	/* 无法从 NULL 改为非 NULL，反之亦然 */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool		nulls[Natts_pg_collation];
		bool		replaces[Natts_pg_collation];
		Datum		values[Natts_pg_collation];

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		values[Anum_pg_collation_collversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_collation_collversion - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								values, nulls, replaces);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(CollationRelationId, collOid, 0);

	ObjectAddressSet(address, CollationRelationId, collOid);

	heap_freetuple(tup);
	table_close(rel, NoLock);

	return address;
}


Datum
pg_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			collid = PG_GETARG_OID(0);
	char		provider;
	char	   *locale;
	char	   *version;
	Datum		datum;

	if (collid == DEFAULT_COLLATION_OID)
	{
		/* 从 pg_database 检索 */

		HeapTuple	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));

		if (!HeapTupleIsValid(dbtup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("database with OID %u does not exist", MyDatabaseId)));

		provider = ((Form_pg_database) GETSTRUCT(dbtup))->datlocprovider;

		datum = SysCacheGetAttrNotNull(DATABASEOID, dbtup,
									   provider == COLLPROVIDER_ICU ?
									   Anum_pg_database_daticulocale : Anum_pg_database_datcollate);

		locale = TextDatumGetCString(datum);

		ReleaseSysCache(dbtup);
	}
	else
	{
		/* 从 pg_collation 检索 */

		HeapTuple	colltp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));

		if (!HeapTupleIsValid(colltp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("collation with OID %u does not exist", collid)));

		provider = ((Form_pg_collation) GETSTRUCT(colltp))->collprovider;
		Assert(provider != COLLPROVIDER_DEFAULT);
		datum = SysCacheGetAttrNotNull(COLLOID, colltp,
									   provider == COLLPROVIDER_ICU ?
									   Anum_pg_collation_colliculocale : Anum_pg_collation_collcollate);

		locale = TextDatumGetCString(datum);

		ReleaseSysCache(colltp);
	}

	version = get_collation_actual_version(provider, locale);
	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/* pg_import_system_collations 中是否使用 "locale -a"？ */
#if defined(HAVE_LOCALE_T) && !defined(WIN32)
#define READ_LOCALE_A_OUTPUT
#endif

/* pg_import_system_collations 中是否使用 EnumSystemLocalesEx？ */
#ifdef WIN32
#define ENUM_SYSTEM_LOCALE
#endif


#ifdef READ_LOCALE_A_OUTPUT
/*
 * "规范化" libc locale 名，剥掉 ".utf8" 之类的编码标签
 * （例如 "en_US.utf8" -> "en_US"，但 "br_FR.iso885915@euro"
 * -> "br_FR@euro"）。若生成了新的、不同的名称，
 * 返回 true。
 */
static bool
normalize_libc_locale_name(char *new, const char *old)
{
	char	   *n = new;
	const char *o = old;
	bool		changed = false;

	while (*o)
	{
		if (*o == '.')
		{
			/* 跳过 ".utf8" 或 ".UTF-8" 之类的编码标签 */
			o++;
			while ((*o >= 'A' && *o <= 'Z')
				   || (*o >= 'a' && *o <= 'z')
				   || (*o >= '0' && *o <= '9')
				   || (*o == '-'))
				o++;
			changed = true;
		}
		else
			*n++ = *o++;
	}
	*n = '\0';

	return changed;
}

/*
 * CollAliasData 项的 qsort 比较器
 */
static int
cmpaliases(const void *a, const void *b)
{
	const CollAliasData *ca = (const CollAliasData *) a;
	const CollAliasData *cb = (const CollAliasData *) b;

	/* 比较 localename 就够了，因为其他字段都由它派生 */
	return strcmp(ca->localename, cb->localename);
}
#endif							/* READ_LOCALE_A_OUTPUT */


#ifdef USE_ICU
/*
 * 获取 ICU locale 的注释（具体是显示名称）。
 * 结果是 palloc 分配的字符串；若拿不到注释，
 * 或发现它不全是 ASCII，则为 NULL。（我们*不能*接受
 * 非 ASCII 注释，因为 template0 的内容必须与编码无关。）
 */
static char *
get_icu_locale_comment(const char *localename)
{
	UErrorCode	status;
	UChar		displayname[128];
	int32		len_uchar;
	int32		i;
	char	   *result;

	status = U_ZERO_ERROR;
	len_uchar = uloc_getDisplayName(localename, "en",
									displayname, lengthof(displayname),
									&status);
	if (U_FAILURE(status))
		return NULL;			/* 没有充分的理由抛错误 */

	/* 检查非 ASCII 注释（这里不能用 pg_is_ascii） */
	for (i = 0; i < len_uchar; i++)
	{
		if (displayname[i] > 127)
			return NULL;
	}

	/* 好，转写 */
	result = palloc(len_uchar + 1);
	for (i = 0; i < len_uchar; i++)
		result[i] = displayname[i];
	result[len_uchar] = '\0';

	return result;
}
#endif							/* USE_ICU */


/*
 * 用输入 locale 'locale' 创建新的排序规则。（pg_import_system_collations()
 * 的子例程）
 *
 * 'nspid' 是将在其中创建排序规则的命名空间 id。
 *
 * 若 locale 的编码有效，'nvalidp' 递增。
 *
 * 若排序规则确实被创建，'ncreatedp' 递增。若排序规则
 * 已存在，则安静地什么也不做。
 *
 * 返回值是 locale 的编码，若该 locale 不适合创建
 * 排序规则则为 -1。
 *
 */
pg_attribute_unused()
static int
create_collation_from_locale(const char *locale, int nspid,
							 int *nvalidp, int *ncreatedp)
{
	int			enc;
	Oid			collid;

	/*
	 * 有些系统的 locale 名不全是 ASCII 字母（如
	 * "bokm&aring;l" 或 "fran&ccedil;ais"）。这相当愚蠢，
	 * 因为我们需要 locale 本身来解释那些非 ASCII
	 * 字符。我们对它们做不了什么，所以把它们过滤掉。
	 */
	if (!pg_is_ascii(locale))
	{
		elog(DEBUG1, "skipping locale with non-ASCII name: \"%s\"", locale);
		return -1;
	}

	enc = pg_get_encoding_from_locale(locale, false);
	if (enc < 0)
	{
		elog(DEBUG1, "skipping locale with unrecognized encoding: \"%s\"", locale);
		return -1;
	}
	if (!PG_VALID_BE_ENCODING(enc))
	{
		elog(DEBUG1, "skipping locale with client-only encoding: \"%s\"", locale);
		return -1;
	}
	if (enc == PG_SQL_ASCII)
		return -1;				/* C/POSIX 已经在目录里了 */

	/* 统计在操作系统中找到的有效 locale */
	(*nvalidp)++;

	/*
	 * 创建与 locale 同名的排序规则，但若它已存在
	 * 就安静地什么也不做。即使在 initdb 时我们也需要
	 * 这种行为，因为某些版本的 "locale -a" 会多次报告
	 * 同一个 locale 名。它对以后的导入运行也很方便，
	 * 因为你几乎总是想添加新 locale，而不想为已有
	 * 的那些产生大量唠叨。
	 */
	collid = CollationCreate(locale, nspid, GetUserId(),
							 COLLPROVIDER_LIBC, true, enc,
							 locale, locale, NULL, NULL,
							 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
							 true, true);
	if (OidIsValid(collid))
	{
		(*ncreatedp)++;

		/* 插入之间必须做 CCI，以正确处理重复 */
		CommandCounterIncrement();
	}

	return enc;
}


#ifdef ENUM_SYSTEM_LOCALE
/* 传给回调函数 win32_read_locale() 的参数 */
typedef struct
{
	Oid			nspid;
	int		   *ncreatedp;
	int		   *nvalidp;
} CollParam;

/*
 * pg_import_system_collations() 中 EnumSystemLocalesEx()
 * 的回调函数。为每个有效 locale 创建一个排序规则，
 * 以及一个 POSIX 别名排序规则。
 *
 * 回调约定：返回 TRUE 继续枚举，FALSE
 * 停止枚举。我们总是想继续。
 */
static BOOL CALLBACK
win32_read_locale(LPWSTR pStr, DWORD dwFlags, LPARAM lparam)
{
	CollParam  *param = (CollParam *) lparam;
	char		localebuf[NAMEDATALEN];
	int			result;
	int			enc;

	(void) dwFlags;

	result = WideCharToMultiByte(CP_ACP, 0, pStr, -1, localebuf, NAMEDATALEN,
								 NULL, NULL);

	if (result == 0)
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			elog(DEBUG1, "skipping locale with too-long name: \"%s\"", localebuf);
		return TRUE;
	}
	if (localebuf[0] == '\0')
		return TRUE;

	enc = create_collation_from_locale(localebuf, param->nspid,
									   param->nvalidp, param->ncreatedp);
	if (enc < 0)
		return TRUE;

	/*
	 * Windows 在语言与地域之间用连字符，而 POSIX
	 * 用下划线。直接创建一个 POSIX 别名。
	 */
	if (strchr(localebuf, '-'))
	{
		char		alias[NAMEDATALEN];
		Oid			collid;

		strcpy(alias, localebuf);
		for (char *p = alias; *p; p++)
			if (*p == '-')
				*p = '_';

		collid = CollationCreate(alias, param->nspid, GetUserId(),
								 COLLPROVIDER_LIBC, true, enc,
								 localebuf, localebuf, NULL, NULL,
								 get_collation_actual_version(COLLPROVIDER_LIBC, localebuf),
								 true, true);
		if (OidIsValid(collid))
		{
			(*param->ncreatedp)++;

			CommandCounterIncrement();
		}
	}

	return TRUE;
}
#endif							/* ENUM_SYSTEM_LOCALE */


/*
 * pg_import_system_collations: 把已知系统排序规则加入 pg_collation
 */
Datum
pg_import_system_collations(PG_FUNCTION_ARGS)
{
	Oid			nspid = PG_GETARG_OID(0);
	int			ncreated = 0;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to import system collations")));

	if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(nspid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nspid)));

	/* 装载 libc 已知的排序规则，用 "locale -a" 枚举它们 */
#ifdef READ_LOCALE_A_OUTPUT
	{
		FILE	   *locale_a_handle;
		char		localebuf[LOCALE_NAME_BUFLEN];
		int			nvalid = 0;
		Oid			collid;
		CollAliasData *aliases;
		int			naliases,
					maxaliases,
					i;

		/* 可扩展的别名数组 */
		maxaliases = 100;
		aliases = (CollAliasData *) palloc(maxaliases * sizeof(CollAliasData));
		naliases = 0;

		locale_a_handle = OpenPipeStream("locale -a", "r");
		if (locale_a_handle == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not execute command \"%s\": %m",
							"locale -a")));

		while (fgets(localebuf, sizeof(localebuf), locale_a_handle))
		{
			size_t		len;
			int			enc;
			char		alias[LOCALE_NAME_BUFLEN];

			len = strlen(localebuf);

			if (len == 0 || localebuf[len - 1] != '\n')
			{
				elog(DEBUG1, "skipping locale with too-long name: \"%s\"", localebuf);
				continue;
			}
			localebuf[len - 1] = '\0';

			enc = create_collation_from_locale(localebuf, nspid, &nvalid, &ncreated);
			if (enc < 0)
				continue;

			/*
			 * 除了 "en_US.utf8"，还生成 "en_US" 之类的别名，
			 * 以方便使用。注意排序规则名仅在每个编码内
			 * 唯一，所以它不会与 LATIN1 的 "en_US"
			 * 冲突。
			 *
			 * 不过，它可能与我们稍后在 "locale -a" 输出中
			 * 见到的名称冲突。所以先把别名攒起来，等
			 * 读完全部输出后再尝试添加。
			 */
			if (normalize_libc_locale_name(alias, localebuf))
			{
				if (naliases >= maxaliases)
				{
					maxaliases *= 2;
					aliases = (CollAliasData *)
						repalloc(aliases, maxaliases * sizeof(CollAliasData));
				}
				aliases[naliases].localename = pstrdup(localebuf);
				aliases[naliases].alias = pstrdup(alias);
				aliases[naliases].enc = enc;
				naliases++;
			}
		}

		/*
		 * 我们不检查它的返回值，因为我们想支持
		 * "locale" 命令不存在的情形。（这不寻常，但
		 * 例如精简 Linux 发行版上可能发生。）
		 * 若找不到任何 locale，我们会在下面警告。
		 */
		ClosePipeStream(locale_a_handle);

		/*
		 * 处理别名之前，按 locale 名排序。这里的要点是：
		 * 若 "locale -a" 给了我们多个编码与基本名相同的
		 * locale 名，比如 "en_US.utf8" 和 "en_US.utf-8"，
		 * 我们想从中挑一个确定性的。ASCII 排序
		 * 第一是足够好的规则。（PG 10 之前，initdb.c 中
		 * 与此逻辑对应的代码还有一条附加排序规则：
		 * 若有与别名完全相同的 locale 名就优先。
		 * 我们这里不需要考虑它，因为上面早已创建了
		 * 这样的 pg_collation 条目，它会胜出。）
		 */
		if (naliases > 1)
			qsort(aliases, naliases, sizeof(CollAliasData), cmpaliases);

		/* 现在添加别名，忽略与既有条目匹配的 */
		for (i = 0; i < naliases; i++)
		{
			char	   *locale = aliases[i].localename;
			char	   *alias = aliases[i].alias;
			int			enc = aliases[i].enc;

			collid = CollationCreate(alias, nspid, GetUserId(),
									 COLLPROVIDER_LIBC, true, enc,
									 locale, locale, NULL, NULL,
									 get_collation_actual_version(COLLPROVIDER_LIBC, locale),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				CommandCounterIncrement();
			}
		}

		/* 若 "locale -a" 似乎出故障就发警告 */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* READ_LOCALE_A_OUTPUT */

	/*
	 * 装载 ICU 已知的排序规则
	 *
	 * 我们用 uloc_countAvailable()/uloc_getAvailable()
	 * 而不是 ucol_countAvailable()/ucol_getAvailable()。
	 * 前者返回完整的语言+地域组合集，而后者只在
	 * 组合不同于该语言的基本排序规则时才返回。
	 * 所以可能没有 de-DE 或 en-GB，
	 * 这会让人困惑。
	 */
#ifdef USE_ICU
	{
		int			i;

		/*
		 * 循环从 -1 开始，把根 locale 塞进来，
		 * 避免太多代码重复。
		 */
		for (i = -1; i < uloc_countAvailable(); i++)
		{
			const char *name;
			char	   *langtag;
			char	   *icucomment;
			Oid			collid;

			if (i == -1)
				name = "";		/* ICU 根 locale */
			else
				name = uloc_getAvailable(i);

			langtag = icu_language_tag(name, ERROR);

			/*
			 * 多疑一点：绝不允许任何非 ASCII 字符串
			 * 进入 pg_collation
			 */
			if (!pg_is_ascii(langtag))
				continue;

			collid = CollationCreate(psprintf("%s-x-icu", langtag),
									 nspid, GetUserId(),
									 COLLPROVIDER_ICU, true, -1,
									 NULL, NULL, langtag, NULL,
									 get_collation_actual_version(COLLPROVIDER_ICU, langtag),
									 true, true);
			if (OidIsValid(collid))
			{
				ncreated++;

				CommandCounterIncrement();

				icucomment = get_icu_locale_comment(name);
				if (icucomment)
					CreateComments(collid, CollationRelationId, 0,
								   icucomment);
			}
		}
	}
#endif							/* USE_ICU */

	/* 装载 WIN32 已知的排序规则 */
#ifdef ENUM_SYSTEM_LOCALE
	{
		int			nvalid = 0;
		CollParam	param;

		param.nspid = nspid;
		param.ncreatedp = &ncreated;
		param.nvalidp = &nvalid;

		/*
		 * 枚举操作系统上已安装或受支持的
		 * locale。
		 */
		if (!EnumSystemLocalesEx(win32_read_locale, LOCALE_ALL,
								 (LPARAM) &param, NULL))
			_dosmaperr(GetLastError());

		/* 若 EnumSystemLocalesEx 似乎出故障就发警告 */
		if (nvalid == 0)
			ereport(WARNING,
					(errmsg("no usable system locales were found")));
	}
#endif							/* ENUM_SYSTEM_LOCALE */

	PG_RETURN_INT32(ncreated);
}
