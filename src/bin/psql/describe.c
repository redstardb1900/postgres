/*
 * psql - the PostgreSQL interactive terminal
 *
 * Support for the various \d ("describe") commands.  Note that the current
 * expectation is that all functions in this file will succeed when working
 * with servers of versions 9.2 and up.  It's okay to omit irrelevant
 * information for an old server, but not to fail outright.  (But failing
 * against a pre-9.2 server is allowed.)
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/describe.c
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "catalog/pg_am.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_default_acl_d.h"
#include "common.h"
#include "common/logging.h"
#include "describe.h"
#include "fe_utils/mbprint.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "settings.h"
#include "variables.h"

static const char *map_typename_pattern(const char *pattern);
static bool describeOneTableDetails(const char *schemaname,
									const char *relationname,
									const char *oid,
									bool verbose);
static void add_tablespace_footer(printTableContent *const cont, char relkind,
								  Oid tablespace, const bool newline);
static void add_role_attribute(PQExpBuffer buf, const char *const str);
static bool listTSParsersVerbose(const char *pattern);
static bool describeOneTSParser(const char *oid, const char *nspname,
								const char *prsname);
static bool listTSConfigsVerbose(const char *pattern);
static bool describeOneTSConfig(const char *oid, const char *nspname,
								const char *cfgname,
								const char *pnspname, const char *prsname);
static void printACLColumn(PQExpBuffer buf, const char *colname);
static bool listOneExtensionContents(const char *extname, const char *oid);
static bool validateSQLNamePattern(PQExpBuffer buf, const char *pattern,
								   bool have_where, bool force_escape,
								   const char *schemavar, const char *namevar,
								   const char *altnamevar,
								   const char *visibilityrule,
								   bool *added_clause, int maxparts);


/*----------------
 * Handlers for various slash commands displaying some sort of list
 * of things in the database.
 *
 * Note: try to format the queries to look nice in -E output.
 *----------------
 */


/*
 * \da
 * 接受一个可选的正则表达式来选择特定的聚合
 */
bool
describeAggregates(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname AS \"%s\",\n"
					  "  pg_catalog.format_type(p.prorettype, NULL) AS \"%s\",\n"
					  "  CASE WHEN p.pronargs = 0\n"
					  "    THEN CAST('*' AS pg_catalog.text)\n"
					  "    ELSE pg_catalog.pg_get_function_arguments(p.oid)\n"
					  "  END AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Result data type"),
					  gettext_noop("Argument data types"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.prokind = 'a'\n",
						  gettext_noop("Description"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"\n"
						  "FROM pg_catalog.pg_proc p\n"
						  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n"
						  "WHERE p.proisagg\n",
						  gettext_noop("Description"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "p.proname", NULL,
								"pg_catalog.pg_function_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of aggregate functions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dA
 * 接受一个可选的正则表达式来选择特定的访问方法
 */
bool
describeAccessMethods(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, true, false, false};

	if (pset.sversion < 90600)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support access methods.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT amname AS \"%s\",\n"
					  "  CASE amtype"
					  " WHEN 'i' THEN '%s'"
					  " WHEN 't' THEN '%s'"
					  " END AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Index"),
					  gettext_noop("Table"),
					  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n  amhandler AS \"%s\",\n"
						  "  pg_catalog.obj_description(oid, 'pg_am') AS \"%s\"",
						  gettext_noop("Handler"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_am\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "amname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of access methods");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \db
 * 接受一个可选的正则表达式来选择特定的表空间
 */
bool
describeTablespaces(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT spcname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(spcowner) AS \"%s\",\n"
					  "  pg_catalog.pg_tablespace_location(oid) AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Location"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "spcacl");
		appendPQExpBuffer(&buf,
						  ",\n  spcoptions AS \"%s\""
						  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_tablespace_size(oid)) AS \"%s\""
						  ",\n  pg_catalog.shobj_description(oid, 'pg_tablespace') AS \"%s\"",
						  gettext_noop("Options"),
						  gettext_noop("Size"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_tablespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "spcname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of tablespaces");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \df
 * 接受一个可选的正则表达式来选择特定的函数。
 *
 * 与 \d 类似，你可以指定所需的函数类型：
 *
 * a 表示聚合
 * n 表示普通
 * p 表示过程
 * t 表示触发器
 * w 表示窗口
 *
 * 并且你可以按任意顺序混合搭配这些选项。
 */
bool
describeFunctions(const char *functypes, const char *func_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	bool		showAggregate = strchr(functypes, 'a') != NULL;
	bool		showNormal = strchr(functypes, 'n') != NULL;
	bool		showProcedure = strchr(functypes, 'p') != NULL;
	bool		showTrigger = strchr(functypes, 't') != NULL;
	bool		showWindow = strchr(functypes, 'w') != NULL;
	bool		have_where;
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, true, true, true, false, true, false, false, false, false};

	/* 在 9.6 之前没有"并行"列 */
	static const bool translate_columns_pre_96[] = {false, false, false, false, true, true, false, true, false, false, false, false};

	if (strlen(functypes) != strspn(functypes, "anptwS+"))
	{
		pg_log_error("\\df only takes [anptwS+] as options");
		return true;
	}

	if (showProcedure && pset.sversion < 110000)
	{
		char		sverbuf[32];

		pg_log_error("\\df does not take a \"%c\" option with server version %s",
					 'p',
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	if (!showAggregate && !showNormal && !showProcedure && !showTrigger && !showWindow)
	{
		showAggregate = showNormal = showTrigger = showWindow = true;
		if (pset.sversion >= 110000)
			showProcedure = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  p.proname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE p.prokind\n"
						  "  WHEN 'a' THEN '%s'\n"
						  "  WHEN 'w' THEN '%s'\n"
						  "  WHEN 'p' THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("proc"),
						  gettext_noop("func"),
						  gettext_noop("Type"));
	else
		appendPQExpBuffer(&buf,
						  "  pg_catalog.pg_get_function_result(p.oid) as \"%s\",\n"
						  "  pg_catalog.pg_get_function_arguments(p.oid) as \"%s\",\n"
						  " CASE\n"
						  "  WHEN p.proisagg THEN '%s'\n"
						  "  WHEN p.proiswindow THEN '%s'\n"
						  "  WHEN p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype THEN '%s'\n"
						  "  ELSE '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("Result data type"),
						  gettext_noop("Argument data types"),
		/* translator: "agg" is short for "aggregate" */
						  gettext_noop("agg"),
						  gettext_noop("window"),
						  gettext_noop("trigger"),
						  gettext_noop("func"),
						  gettext_noop("Type"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n CASE\n"
						  "  WHEN p.provolatile = 'i' THEN '%s'\n"
						  "  WHEN p.provolatile = 's' THEN '%s'\n"
						  "  WHEN p.provolatile = 'v' THEN '%s'\n"
						  " END as \"%s\"",
						  gettext_noop("immutable"),
						  gettext_noop("stable"),
						  gettext_noop("volatile"),
						  gettext_noop("Volatility"));
		if (pset.sversion >= 90600)
			appendPQExpBuffer(&buf,
							  ",\n CASE\n"
							  "  WHEN p.proparallel = 'r' THEN '%s'\n"
							  "  WHEN p.proparallel = 's' THEN '%s'\n"
							  "  WHEN p.proparallel = 'u' THEN '%s'\n"
							  " END as \"%s\"",
							  gettext_noop("restricted"),
							  gettext_noop("safe"),
							  gettext_noop("unsafe"),
							  gettext_noop("Parallel"));
		appendPQExpBuffer(&buf,
						  ",\n pg_catalog.pg_get_userbyid(p.proowner) as \"%s\""
						  ",\n CASE WHEN prosecdef THEN '%s' ELSE '%s' END AS \"%s\"",
						  gettext_noop("Owner"),
						  gettext_noop("definer"),
						  gettext_noop("invoker"),
						  gettext_noop("Security"));
		appendPQExpBufferStr(&buf, ",\n ");
		printACLColumn(&buf, "p.proacl");
		appendPQExpBuffer(&buf,
						  ",\n l.lanname as \"%s\"",
						  gettext_noop("Language"));
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN l.lanname IN ('internal', 'c') THEN p.prosrc END as \"%s\"",
						  gettext_noop("Internal name"));
		appendPQExpBuffer(&buf,
						  ",\n pg_catalog.obj_description(p.oid, 'pg_proc') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_proc p"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		appendPQExpBuffer(&buf,
						  "     LEFT JOIN pg_catalog.pg_type t%d ON t%d.oid = p.proargtypes[%d]\n"
						  "     LEFT JOIN pg_catalog.pg_namespace nt%d ON nt%d.oid = t%d.typnamespace\n",
						  i, i, i, i, i, i);
	}

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_language l ON l.oid = p.prolang\n");

	have_where = false;

	/* 根据请求按函数类型过滤 */
	if (showNormal && showAggregate && showProcedure && showTrigger && showWindow)
		 /* 不做任何事 */ ;
	else if (showNormal)
	{
		if (!showAggregate)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> 'a'\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proisagg\n");
		}
		if (!showProcedure && pset.sversion >= 110000)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prokind <> 'p'\n");
		}
		if (!showTrigger)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			appendPQExpBufferStr(&buf, "p.prorettype <> 'pg_catalog.trigger'::pg_catalog.regtype\n");
		}
		if (!showWindow)
		{
			if (have_where)
				appendPQExpBufferStr(&buf, "      AND ");
			else
			{
				appendPQExpBufferStr(&buf, "WHERE ");
				have_where = true;
			}
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind <> 'w'\n");
			else
				appendPQExpBufferStr(&buf, "NOT p.proiswindow\n");
		}
	}
	else
	{
		bool		needs_or = false;

		appendPQExpBufferStr(&buf, "WHERE (\n       ");
		have_where = true;
		/* 注意：其中至少有一个必须为真 ... */
		if (showAggregate)
		{
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = 'a'\n");
			else
				appendPQExpBufferStr(&buf, "p.proisagg\n");
			needs_or = true;
		}
		if (showTrigger)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf,
								 "p.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype\n");
			needs_or = true;
		}
		if (showProcedure)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			appendPQExpBufferStr(&buf, "p.prokind = 'p'\n");
			needs_or = true;
		}
		if (showWindow)
		{
			if (needs_or)
				appendPQExpBufferStr(&buf, "       OR ");
			if (pset.sversion >= 110000)
				appendPQExpBufferStr(&buf, "p.prokind = 'w'\n");
			else
				appendPQExpBufferStr(&buf, "p.proiswindow\n");
		}
		appendPQExpBufferStr(&buf, "      )\n");
	}

	if (!validateSQLNamePattern(&buf, func_pattern, have_where, false,
								"n.nspname", "p.proname", NULL,
								"pg_catalog.pg_function_is_visible(p.oid)",
								NULL, 3))
		goto error_return;

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * 将类型名称模式匹配到内部名称或外部名称，
			 * 类似于 \dT。与 \dT 不同，似乎没有理由
			 * 排除数组类型或复合类型。
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				goto error_return;
		}
		else
		{
			/* "-" 模式表示没有这样的参数 */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	if (!showSystem && !func_pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of functions");
	myopt.translate_header = true;
	if (pset.sversion >= 90600)
	{
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);
	}
	else
	{
		myopt.translate_columns = translate_columns_pre_96;
		myopt.n_translate_columns = lengthof(translate_columns_pre_96);
	}

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}



/*
 * \dT
 * 描述类型
 */
bool
describeTypes(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  pg_catalog.format_type(t.oid, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));
	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  t.typname AS \"%s\",\n"
						  "  CASE WHEN t.typrelid != 0\n"
						  "      THEN CAST('tuple' AS pg_catalog.text)\n"
						  "    WHEN t.typlen < 0\n"
						  "      THEN CAST('var' AS pg_catalog.text)\n"
						  "    ELSE CAST(t.typlen AS pg_catalog.text)\n"
						  "  END AS \"%s\",\n"
						  "  pg_catalog.array_to_string(\n"
						  "      ARRAY(\n"
						  "          SELECT e.enumlabel\n"
						  "          FROM pg_catalog.pg_enum e\n"
						  "          WHERE e.enumtypid = t.oid\n"
						  "          ORDER BY e.enumsortorder\n"
						  "      ),\n"
						  "      E'\\n'\n"
						  "  ) AS \"%s\",\n"
						  "  pg_catalog.pg_get_userbyid(t.typowner) AS \"%s\",\n",
						  gettext_noop("Internal name"),
						  gettext_noop("Size"),
						  gettext_noop("Elements"),
						  gettext_noop("Owner"));
		printACLColumn(&buf, "t.typacl");
		appendPQExpBufferStr(&buf, ",\n  ");
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(t.oid, 'pg_type') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_type t\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	/*
	 * 不要包含复杂类型（typrelid!=0），除非它们是独立的
	 * 复合类型
	 */
	appendPQExpBufferStr(&buf, "WHERE (t.typrelid = 0 ");
	appendPQExpBufferStr(&buf, "OR (SELECT c.relkind = " CppAsString2(RELKIND_COMPOSITE_TYPE)
						 " FROM pg_catalog.pg_class c "
						 "WHERE c.oid = t.typrelid))\n");

	/*
	 * 不要包含数组类型，除非模式包含 []
	 */
	if (pattern == NULL || strstr(pattern, "[]") == NULL)
		appendPQExpBufferStr(&buf, "  AND NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type el WHERE el.oid = t.typelem AND el.typarray = t.oid)\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/* 将名称模式匹配到内部名称或外部名称 */
	if (!validateSQLNamePattern(&buf, map_typename_pattern(pattern),
								true, false,
								"n.nspname", "t.typname",
								"pg_catalog.format_type(t.oid, NULL)",
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of data types");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * 将后端语法接受的一些变体类型名称映射为
 * 规范类型名称。
 *
 * \dT 和其他接受类型名称模式的函数的辅助函数。
 * 这并不能完全掩盖这些名称是特殊的这一事实；
 * 例如，模式 "dec*" 不会神奇地匹配 "numeric"。
 * 但它大大减少了意外性。
 */
static const char *
map_typename_pattern(const char *pattern)
{
	static const char *const typename_map[] = {
		/*
		 * 这些名称被 gram.y 接受，尽管它们既不是
		 * pg_type 中看到的"真实"名称，也不是 format_type()
		 * 打印的规范名称。
		 */
		"decimal", "numeric",
		"float", "double precision",
		"int", "integer",

		/*
		 * 当规范名称与 pg_type 所说的不同时，
		 * 我们还必须映射数组名称。
		 */
		"bool[]", "boolean[]",
		"decimal[]", "numeric[]",
		"float[]", "double precision[]",
		"float4[]", "real[]",
		"float8[]", "double precision[]",
		"int[]", "integer[]",
		"int2[]", "smallint[]",
		"int4[]", "integer[]",
		"int8[]", "bigint[]",
		"time[]", "time without time zone[]",
		"timetz[]", "time with time zone[]",
		"timestamp[]", "timestamp without time zone[]",
		"timestamptz[]", "timestamp with time zone[]",
		"varbit[]", "bit varying[]",
		"varchar[]", "character varying[]",
		NULL
	};

	if (pattern == NULL)
		return NULL;
	for (int i = 0; typename_map[i] != NULL; i += 2)
	{
		if (pg_strcasecmp(pattern, typename_map[i]) == 0)
			return typename_map[i + 1];
	}
	return pattern;
}


/*
 * \do
 * 描述操作符
 */
bool
describeOperators(const char *oper_pattern,
				  char **arg_patterns, int num_arg_patterns,
				  bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	/*
	 * 注意：在 Postgres 9.1 之前，我们没有为任何内置       
	 * 操作符分配注释，更倾向于让底层函数上的注释    
	 * 来满足需求。下面 obj_description() 调用上的 coalesce()    
	 * 通过为操作符函数的注释提供回退查找来支持      
	 * 这一约定。从 9.1 起有一项政策：每个内置操作符    
	 * 均应带注释；故就内置操作符而言，coalesce()    
	 * 已不再必要。但我们仍然暂时保留它，              
	 * 因为第三方模块可能仍在遵循                                
	 * 旧的约定。
	 *
	 * 此查询中对后缀操作符的支持自 Postgres 14 起        
	 * 已是死代码，但只要我们还需与 pre-v14 服务器         
	 * 通信得保留。 
	 */

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  o.oprname AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='l' THEN NULL ELSE pg_catalog.format_type(o.oprleft, NULL) END AS \"%s\",\n"
					  "  CASE WHEN o.oprkind='r' THEN NULL ELSE pg_catalog.format_type(o.oprright, NULL) END AS \"%s\",\n"
					  "  pg_catalog.format_type(o.oprresult, NULL) AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Left arg type"),
					  gettext_noop("Right arg type"),
					  gettext_noop("Result type"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  "  o.oprcode AS \"%s\",\n",
						  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
					  "  coalesce(pg_catalog.obj_description(o.oid, 'pg_operator'),\n"
					  "           pg_catalog.obj_description(o.oprcode, 'pg_proc')) AS \"%s\"\n"
					  "FROM pg_catalog.pg_operator o\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = o.oprnamespace\n",
					  gettext_noop("Description"));

	if (num_arg_patterns >= 2)
	{
		num_arg_patterns = 2;	/* 忽略任何额外参数 */
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprleft\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n"
							 "     LEFT JOIN pg_catalog.pg_type t1 ON t1.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt1 ON nt1.oid = t1.typnamespace\n");
	}
	else if (num_arg_patterns == 1)
	{
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_type t0 ON t0.oid = o.oprright\n"
							 "     LEFT JOIN pg_catalog.pg_namespace nt0 ON nt0.oid = t0.typnamespace\n");
	}

	if (!showSystem && !oper_pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, oper_pattern,
								!showSystem && !oper_pattern, true,
								"n.nspname", "o.oprname", NULL,
								"pg_catalog.pg_operator_is_visible(o.oid)",
								NULL, 3))
		goto error_return;

	if (num_arg_patterns == 1)
		appendPQExpBufferStr(&buf, "  AND o.oprleft = 0\n");

	for (int i = 0; i < num_arg_patterns; i++)
	{
		if (strcmp(arg_patterns[i], "-") != 0)
		{
			/*
			 * 将类型名称模式匹配到内部名称或外部名称，
			 * 类似于 \dT。与 \dT 不同，似乎没有理由
			 * 排除数组类型或复合类型。
			 */
			char		nspname[64];
			char		typname[64];
			char		ft[64];
			char		tiv[64];

			snprintf(nspname, sizeof(nspname), "nt%d.nspname", i);
			snprintf(typname, sizeof(typname), "t%d.typname", i);
			snprintf(ft, sizeof(ft),
					 "pg_catalog.format_type(t%d.oid, NULL)", i);
			snprintf(tiv, sizeof(tiv),
					 "pg_catalog.pg_type_is_visible(t%d.oid)", i);
			if (!validateSQLNamePattern(&buf,
										map_typename_pattern(arg_patterns[i]),
										true, false,
										nspname, typname, ft, tiv,
										NULL, 3))
				goto error_return;
		}
		else
		{
			/* "-" 模式表示没有这样的参数 */
			appendPQExpBuffer(&buf, "  AND t%d.typname IS NULL\n", i);
		}
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3, 4;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operators");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * listAllDbs
 *
 * 用于 \l、\list 和 -l 开关
 */
bool
listAllDbs(const char *pattern, bool verbose)
{
	PGresult   *res;
	PQExpBufferData buf;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  d.datname as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(d.datdba) as \"%s\",\n"
					  "  pg_catalog.pg_encoding_to_char(d.encoding) as \"%s\",\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Encoding"));
	if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  CASE d.datlocprovider WHEN 'c' THEN 'libc' WHEN 'i' THEN 'icu' END AS \"%s\",\n",
						  gettext_noop("Locale Provider"));
	else
		appendPQExpBuffer(&buf,
						  "  'libc' AS \"%s\",\n",
						  gettext_noop("Locale Provider"));
	appendPQExpBuffer(&buf,
					  "  d.datcollate as \"%s\",\n"
					  "  d.datctype as \"%s\",\n",
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));
	if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  d.daticulocale as \"%s\",\n",
						  gettext_noop("ICU Locale"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL as \"%s\",\n",
						  gettext_noop("ICU Locale"));
	if (pset.sversion >= 160000)
		appendPQExpBuffer(&buf,
						  "  d.daticurules as \"%s\",\n",
						  gettext_noop("ICU Rules"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL as \"%s\",\n",
						  gettext_noop("ICU Rules"));
	appendPQExpBufferStr(&buf, "  ");
	printACLColumn(&buf, "d.datacl");
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  CASE WHEN pg_catalog.has_database_privilege(d.datname, 'CONNECT')\n"
						  "       THEN pg_catalog.pg_size_pretty(pg_catalog.pg_database_size(d.datname))\n"
						  "       ELSE 'No Access'\n"
						  "  END as \"%s\""
						  ",\n  t.spcname as \"%s\""
						  ",\n  pg_catalog.shobj_description(d.oid, 'pg_database') as \"%s\"",
						  gettext_noop("Size"),
						  gettext_noop("Tablespace"),
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_database d\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  JOIN pg_catalog.pg_tablespace t on d.dattablespace = t.oid\n");

	if (pattern)
	{
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "d.datname", NULL, NULL,
									NULL, 1))
		{
			termPQExpBuffer(&buf);
			return false;
		}
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of databases");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * 列出表的授权/回收权限
 * \z（现在也是 \dp -- 也许更容易记忆）
 */
bool
permissionsList(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false, false, false};

	initPQExpBuffer(&buf);

	/*
	 * 我们忽略索引和 toast 表，因为它们没有有意义的权限
	 */
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN " CppAsString2(RELKIND_RELATION) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_VIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_MATVIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_SEQUENCE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
					  " END as \"%s\",\n"
					  "  ",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("sequence"),
					  gettext_noop("foreign table"),
					  gettext_noop("partitioned table"),
					  gettext_noop("Type"));

	printACLColumn(&buf, "c.relacl");

	appendPQExpBuffer(&buf,
					  ",\n  pg_catalog.array_to_string(ARRAY(\n"
					  "    SELECT attname || E':\\n  ' || pg_catalog.array_to_string(attacl, E'\\n  ')\n"
					  "    FROM pg_catalog.pg_attribute a\n"
					  "    WHERE attrelid = c.oid AND NOT attisdropped AND attacl IS NOT NULL\n"
					  "  ), E'\\n') AS \"%s\"",
					  gettext_noop("Column privileges"));

	if (pset.sversion >= 90500 && pset.sversion < 100000)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT polname\n"
						  "    || CASE WHEN polcmd != '*' THEN\n"
						  "           E' (' || polcmd::pg_catalog.text || E'):'\n"
						  "       ELSE E':'\n"
						  "       END\n"
						  "    || CASE WHEN polqual IS NOT NULL THEN\n"
						  "           E'\\n  (u): ' || pg_catalog.pg_get_expr(polqual, polrelid)\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    || CASE WHEN polwithcheck IS NOT NULL THEN\n"
						  "           E'\\n  (c): ' || pg_catalog.pg_get_expr(polwithcheck, polrelid)\n"
						  "       ELSE E''\n"
						  "       END"
						  "    || CASE WHEN polroles <> '{0}' THEN\n"
						  "           E'\\n  to: ' || pg_catalog.array_to_string(\n"
						  "               ARRAY(\n"
						  "                   SELECT rolname\n"
						  "                   FROM pg_catalog.pg_roles\n"
						  "                   WHERE oid = ANY (polroles)\n"
						  "                   ORDER BY 1\n"
						  "               ), E', ')\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    FROM pg_catalog.pg_policy pol\n"
						  "    WHERE polrelid = c.oid), E'\\n')\n"
						  "    AS \"%s\"",
						  gettext_noop("Policies"));

	if (pset.sversion >= 100000)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.array_to_string(ARRAY(\n"
						  "    SELECT polname\n"
						  "    || CASE WHEN NOT polpermissive THEN\n"
						  "       E' (RESTRICTIVE)'\n"
						  "       ELSE '' END\n"
						  "    || CASE WHEN polcmd != '*' THEN\n"
						  "           E' (' || polcmd::pg_catalog.text || E'):'\n"
						  "       ELSE E':'\n"
						  "       END\n"
						  "    || CASE WHEN polqual IS NOT NULL THEN\n"
						  "           E'\\n  (u): ' || pg_catalog.pg_get_expr(polqual, polrelid)\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    || CASE WHEN polwithcheck IS NOT NULL THEN\n"
						  "           E'\\n  (c): ' || pg_catalog.pg_get_expr(polwithcheck, polrelid)\n"
						  "       ELSE E''\n"
						  "       END"
						  "    || CASE WHEN polroles <> '{0}' THEN\n"
						  "           E'\\n  to: ' || pg_catalog.array_to_string(\n"
						  "               ARRAY(\n"
						  "                   SELECT rolname\n"
						  "                   FROM pg_catalog.pg_roles\n"
						  "                   WHERE oid = ANY (polroles)\n"
						  "                   ORDER BY 1\n"
						  "               ), E', ')\n"
						  "       ELSE E''\n"
						  "       END\n"
						  "    FROM pg_catalog.pg_policy pol\n"
						  "    WHERE polrelid = c.oid), E'\\n')\n"
						  "    AS \"%s\"",
						  gettext_noop("Policies"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_class c\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
						 "WHERE c.relkind IN ("
						 CppAsString2(RELKIND_RELATION) ","
						 CppAsString2(RELKIND_VIEW) ","
						 CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_SEQUENCE) ","
						 CppAsString2(RELKIND_FOREIGN_TABLE) ","
						 CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	myopt.nullPrint = NULL;
	printfPQExpBuffer(&buf, _("Access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * \ddp
 *
 * 列出默认 ACL。模式可以匹配模式名称或角色名称。
 */
bool
listDefaultACLs(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.pg_get_userbyid(d.defaclrole) AS \"%s\",\n"
					  "  n.nspname AS \"%s\",\n"
					  "  CASE d.defaclobjtype WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' WHEN '%c' THEN '%s' END AS \"%s\",\n"
					  "  ",
					  gettext_noop("Owner"),
					  gettext_noop("Schema"),
					  DEFACLOBJ_RELATION,
					  gettext_noop("table"),
					  DEFACLOBJ_SEQUENCE,
					  gettext_noop("sequence"),
					  DEFACLOBJ_FUNCTION,
					  gettext_noop("function"),
					  DEFACLOBJ_TYPE,
					  gettext_noop("type"),
					  DEFACLOBJ_NAMESPACE,
					  gettext_noop("schema"),
					  gettext_noop("Type"));

	printACLColumn(&buf, "d.defaclacl");

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_default_acl d\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.defaclnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL,
								"n.nspname",
								"pg_catalog.pg_get_userbyid(d.defaclrole)",
								NULL,
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	myopt.nullPrint = NULL;
	printfPQExpBuffer(&buf, _("Default access privileges"));
	myopt.title = buf.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * 获取对象注释
 *
 * \dd [foo]
 *
 * 注意：此命令仅列出那些没有通过其自身反斜杠命令
 * 显示注释的对象类型的注释。以下类型的对象将被显示：
 * 约束、操作符类、操作符族、规则和触发器。
 *
 *
 */
bool
objectDescription(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, true, false};

	initPQExpBuffer(&buf);

	appendPQExpBuffer(&buf,
					  "SELECT DISTINCT tt.nspname AS \"%s\", tt.name AS \"%s\", tt.object AS \"%s\", d.description AS \"%s\"\n"
					  "FROM (\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Object"),
					  gettext_noop("Description"));

	/* 表约束描述 */
	appendPQExpBuffer(&buf,
					  "  SELECT pgc.oid as oid, pgc.tableoid AS tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(pgc.conname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_constraint pgc\n"
					  "    JOIN pg_catalog.pg_class c "
					  "ON c.oid = pgc.conrelid\n"
					  "    LEFT JOIN pg_catalog.pg_namespace n "
					  "    ON n.oid = c.relnamespace\n",
					  gettext_noop("table constraint"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern,
								false, "n.nspname", "pgc.conname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	/* 域约束描述 */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT pgc.oid as oid, pgc.tableoid AS tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(pgc.conname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_constraint pgc\n"
					  "    JOIN pg_catalog.pg_type t "
					  "ON t.oid = pgc.contypid\n"
					  "    LEFT JOIN pg_catalog.pg_namespace n "
					  "    ON n.oid = t.typnamespace\n",
					  gettext_noop("domain constraint"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern,
								false, "n.nspname", "pgc.conname", NULL,
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
		goto error_return;

	/* 操作符类描述 */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT o.oid as oid, o.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(o.opcname AS pg_catalog.text) as name,\n"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_opclass o\n"
					  "    JOIN pg_catalog.pg_am am ON "
					  "o.opcmethod = am.oid\n"
					  "    JOIN pg_catalog.pg_namespace n ON "
					  "n.oid = o.opcnamespace\n",
					  gettext_noop("operator class"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "o.opcname", NULL,
								"pg_catalog.pg_opclass_is_visible(o.oid)",
								NULL, 3))
		goto error_return;

	/* 操作符族描述 */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT opf.oid as oid, opf.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(opf.opfname AS pg_catalog.text) AS name,\n"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_opfamily opf\n"
					  "    JOIN pg_catalog.pg_am am "
					  "ON opf.opfmethod = am.oid\n"
					  "    JOIN pg_catalog.pg_namespace n "
					  "ON opf.opfnamespace = n.oid\n",
					  gettext_noop("operator family"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "opf.opfname", NULL,
								"pg_catalog.pg_opfamily_is_visible(opf.oid)",
								NULL, 3))
		goto error_return;

	/* 规则描述（忽略视图的规则） */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT r.oid as oid, r.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(r.rulename AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_rewrite r\n"
					  "       JOIN pg_catalog.pg_class c ON c.oid = r.ev_class\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
					  "  WHERE r.rulename != '_RETURN'\n",
					  gettext_noop("rule"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "r.rulename", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	/* 触发器描述 */
	appendPQExpBuffer(&buf,
					  "UNION ALL\n"
					  "  SELECT t.oid as oid, t.tableoid as tableoid,\n"
					  "  n.nspname as nspname,\n"
					  "  CAST(t.tgname AS pg_catalog.text) as name,"
					  "  CAST('%s' AS pg_catalog.text) as object\n"
					  "  FROM pg_catalog.pg_trigger t\n"
					  "       JOIN pg_catalog.pg_class c ON c.oid = t.tgrelid\n"
					  "       LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n",
					  gettext_noop("trigger"));

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern, false,
								"n.nspname", "t.tgname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf,
						 ") AS tt\n"
						 "  JOIN pg_catalog.pg_description d ON (tt.oid = d.objoid AND tt.tableoid = d.classoid AND d.objsubid = 0)\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("Object descriptions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * describeTableDetails（用于 \d）
 *
 * 此例程查找要显示的表，并为每个表
 * 调用 describeOneTableDetails。
 *
 * verbose：如果为 true，表示 \d+
 */
bool
describeTableDetails(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid,\n"
					  "  n.nspname,\n"
					  "  c.relname\n"
					  "FROM pg_catalog.pg_class c\n"
					  "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, !showSystem && !pattern, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 2, 3;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any relation named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any relations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname;
		const char *relname;

		oid = PQgetvalue(res, i, 0);
		nspname = PQgetvalue(res, i, 1);
		relname = PQgetvalue(res, i, 2);

		if (!describeOneTableDetails(nspname, relname, oid, verbose))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

/*
 * describeOneTableDetails（用于 \d）
 *
 * 不幸的是，这里呈现的信息非常复杂，无法在单个查询
 * 中完成。因此我们必须手工组装打印表，并将其
 * 传递给底层的 printTable() 函数。
 */
static bool
describeOneTableDetails(const char *schemaname,
						const char *relationname,
						const char *oid,
						bool verbose)
{
	bool		retval = false;
	PQExpBufferData buf;
	PGresult   *res = NULL;
	printTableOpt myopt = pset.popt.topt;
	printTableContent cont;
	bool		printTableInitialized = false;
	int			i;
	char	   *view_def = NULL;
	char	   *headers[12];
	PQExpBufferData title;
	PQExpBufferData tmpbuf;
	int			cols;
	int			attname_col = -1,	/* "res" 中的列索引 */
				atttype_col = -1,
				attrdef_col = -1,
				attnotnull_col = -1,
				attcoll_col = -1,
				attidentity_col = -1,
				attgenerated_col = -1,
				isindexkey_col = -1,
				indexdef_col = -1,
				fdwopts_col = -1,
				attstorage_col = -1,
				attcompression_col = -1,
				attstattarget_col = -1,
				attdescr_col = -1;
	int			numrows;
	struct
	{
		int16		checks;
		char		relkind;
		bool		hasindex;
		bool		hasrules;
		bool		hastriggers;
		bool		rowsecurity;
		bool		forcerowsecurity;
		bool		hasoids;
		bool		ispartition;
		Oid			tablespace;
		char	   *reloptions;
		char	   *reloftype;
		char		relpersistence;
		char		relreplident;
		char	   *relam;
	}			tableinfo;
	bool		show_column_details = false;

	myopt.default_footer = false;
	/* 此输出在扩展模式下看起来会混乱。 */
	myopt.expanded = false;

	initPQExpBuffer(&buf);
	initPQExpBuffer(&title);
	initPQExpBuffer(&tmpbuf);

	/* 获取通用表信息 */
	if (pset.sversion >= 120000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "false AS relhasoids, c.relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident, am.amname\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "LEFT JOIN pg_catalog.pg_am am ON (c.relam = am.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 100000)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relhasoids, c.relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90500)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, c.relrowsecurity, c.relforcerowsecurity, "
						  "c.relhasoids, false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else if (pset.sversion >= 90400)
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, false, c.relhasoids, "
						  "false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence, c.relreplident\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}
	else
	{
		printfPQExpBuffer(&buf,
						  "SELECT c.relchecks, c.relkind, c.relhasindex, c.relhasrules, "
						  "c.relhastriggers, false, false, c.relhasoids, "
						  "false as relispartition, %s, c.reltablespace, "
						  "CASE WHEN c.reloftype = 0 THEN '' ELSE c.reloftype::pg_catalog.regtype::pg_catalog.text END, "
						  "c.relpersistence\n"
						  "FROM pg_catalog.pg_class c\n "
						  "LEFT JOIN pg_catalog.pg_class tc ON (c.reltoastrelid = tc.oid)\n"
						  "WHERE c.oid = '%s';",
						  (verbose ?
						   "pg_catalog.array_to_string(c.reloptions || "
						   "array(select 'toast.' || x from pg_catalog.unnest(tc.reloptions) x), ', ')\n"
						   : "''"),
						  oid);
	}

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	/* 我们获取到了什么吗？ */
	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
			pg_log_error("Did not find any relation with OID %s.", oid);
		goto error_return;
	}

	tableinfo.checks = atoi(PQgetvalue(res, 0, 0));
	tableinfo.relkind = *(PQgetvalue(res, 0, 1));
	tableinfo.hasindex = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
	tableinfo.hasrules = strcmp(PQgetvalue(res, 0, 3), "t") == 0;
	tableinfo.hastriggers = strcmp(PQgetvalue(res, 0, 4), "t") == 0;
	tableinfo.rowsecurity = strcmp(PQgetvalue(res, 0, 5), "t") == 0;
	tableinfo.forcerowsecurity = strcmp(PQgetvalue(res, 0, 6), "t") == 0;
	tableinfo.hasoids = strcmp(PQgetvalue(res, 0, 7), "t") == 0;
	tableinfo.ispartition = strcmp(PQgetvalue(res, 0, 8), "t") == 0;
	tableinfo.reloptions = pg_strdup(PQgetvalue(res, 0, 9));
	tableinfo.tablespace = atooid(PQgetvalue(res, 0, 10));
	tableinfo.reloftype = (strcmp(PQgetvalue(res, 0, 11), "") != 0) ?
		pg_strdup(PQgetvalue(res, 0, 11)) : NULL;
	tableinfo.relpersistence = *(PQgetvalue(res, 0, 12));
	tableinfo.relreplident = (pset.sversion >= 90400) ?
		*(PQgetvalue(res, 0, 13)) : 'd';
	if (pset.sversion >= 120000)
		tableinfo.relam = PQgetisnull(res, 0, 14) ?
			(char *) NULL : pg_strdup(PQgetvalue(res, 0, 14));
	else
		tableinfo.relam = NULL;
	PQclear(res);
	res = NULL;

	/*
	 * 如果是序列，在这里单独处理。
	 */
	if (tableinfo.relkind == RELKIND_SEQUENCE)
	{
		PGresult   *result = NULL;
		printQueryOpt myopt = pset.popt;
		char	   *footers[2] = {NULL, NULL};

		if (pset.sversion >= 100000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT pg_catalog.format_type(seqtypid, NULL) AS \"%s\",\n"
							  "       seqstart AS \"%s\",\n"
							  "       seqmin AS \"%s\",\n"
							  "       seqmax AS \"%s\",\n"
							  "       seqincrement AS \"%s\",\n"
							  "       CASE WHEN seqcycle THEN '%s' ELSE '%s' END AS \"%s\",\n"
							  "       seqcache AS \"%s\"\n",
							  gettext_noop("Type"),
							  gettext_noop("Start"),
							  gettext_noop("Minimum"),
							  gettext_noop("Maximum"),
							  gettext_noop("Increment"),
							  gettext_noop("yes"),
							  gettext_noop("no"),
							  gettext_noop("Cycles?"),
							  gettext_noop("Cache"));
			appendPQExpBuffer(&buf,
							  "FROM pg_catalog.pg_sequence\n"
							  "WHERE seqrelid = '%s';",
							  oid);
		}
		else
		{
			printfPQExpBuffer(&buf,
							  "SELECT 'bigint' AS \"%s\",\n"
							  "       start_value AS \"%s\",\n"
							  "       min_value AS \"%s\",\n"
							  "       max_value AS \"%s\",\n"
							  "       increment_by AS \"%s\",\n"
							  "       CASE WHEN is_cycled THEN '%s' ELSE '%s' END AS \"%s\",\n"
							  "       cache_value AS \"%s\"\n",
							  gettext_noop("Type"),
							  gettext_noop("Start"),
							  gettext_noop("Minimum"),
							  gettext_noop("Maximum"),
							  gettext_noop("Increment"),
							  gettext_noop("yes"),
							  gettext_noop("no"),
							  gettext_noop("Cycles?"),
							  gettext_noop("Cache"));
			appendPQExpBuffer(&buf, "FROM %s", fmtId(schemaname));
			/* 必须分开，因为 fmtId 不可重入 */
			appendPQExpBuffer(&buf, ".%s;", fmtId(relationname));
		}

		res = PSQLexec(buf.data);
		if (!res)
			goto error_return;

		/* 获取拥有此序列的列 */
		printfPQExpBuffer(&buf, "SELECT pg_catalog.quote_ident(nspname) || '.' ||"
						  "\n   pg_catalog.quote_ident(relname) || '.' ||"
						  "\n   pg_catalog.quote_ident(attname),"
						  "\n   d.deptype"
						  "\nFROM pg_catalog.pg_class c"
						  "\nINNER JOIN pg_catalog.pg_depend d ON c.oid=d.refobjid"
						  "\nINNER JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace"
						  "\nINNER JOIN pg_catalog.pg_attribute a ON ("
						  "\n a.attrelid=c.oid AND"
						  "\n a.attnum=d.refobjsubid)"
						  "\nWHERE d.classid='pg_catalog.pg_class'::pg_catalog.regclass"
						  "\n AND d.refclassid='pg_catalog.pg_class'::pg_catalog.regclass"
						  "\n AND d.objid='%s'"
						  "\n AND d.deptype IN ('a', 'i')",
						  oid);

		result = PSQLexec(buf.data);

		/*
		 * 如果没有返回行，显然不显示任何内容。我们
		 * 永远不应该返回多于一行，但如果真的发生了，
		 * 就忽略它，不打印任何内容。
		 */
		if (!result)
			goto error_return;
		else if (PQntuples(result) == 1)
		{
			switch (PQgetvalue(result, 0, 1)[0])
			{
				case 'a':
					footers[0] = psprintf(_("Owned by: %s"),
										  PQgetvalue(result, 0, 0));
					break;
				case 'i':
					footers[0] = psprintf(_("Sequence for identity column: %s"),
										  PQgetvalue(result, 0, 0));
					break;
			}
		}
		PQclear(result);

		if (tableinfo.relpersistence == 'u')
			printfPQExpBuffer(&title, _("Unlogged sequence \"%s.%s\""),
							  schemaname, relationname);
		else
			printfPQExpBuffer(&title, _("Sequence \"%s.%s\""),
							  schemaname, relationname);

		myopt.footers = footers;
		myopt.topt.default_footer = false;
		myopt.title = title.data;
		myopt.translate_header = true;

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

		free(footers[0]);

		retval = true;
		goto error_return;		/* 不是错误，只是提前返回 */
	}

	/* 确定是否应打印排序规则、可为空性、默认值 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_VIEW ||
		tableinfo.relkind == RELKIND_MATVIEW ||
		tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
		tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
		tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		show_column_details = true;

	/*
	 * 获取逐列信息
	 *
	 * 由于我们需要的查询列集合因 relkind 和服务器版本
	 * 而异，我们动态计算所有列号。未获取的列的
	 * 列号变量保持为 -1；这避免了下面重复的测试逻辑。
	 *
	 */
	cols = 0;
	printfPQExpBuffer(&buf, "SELECT a.attname");
	attname_col = cols++;
	appendPQExpBufferStr(&buf, ",\n  pg_catalog.format_type(a.atttypid, a.atttypmod)");
	atttype_col = cols++;

	if (show_column_details)
	{
		/* 对表达式使用"精美"模式以避免过多的括号 */
		appendPQExpBufferStr(&buf,
							 ",\n  (SELECT pg_catalog.pg_get_expr(d.adbin, d.adrelid, true)"
							 "\n   FROM pg_catalog.pg_attrdef d"
							 "\n   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef)"
							 ",\n  a.attnotnull");
		attrdef_col = cols++;
		attnotnull_col = cols++;
		appendPQExpBufferStr(&buf, ",\n  (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type t\n"
							 "   WHERE c.oid = a.attcollation AND t.oid = a.atttypid AND a.attcollation <> t.typcollation) AS attcollation");
		attcoll_col = cols++;
		if (pset.sversion >= 100000)
			appendPQExpBufferStr(&buf, ",\n  a.attidentity");
		else
			appendPQExpBufferStr(&buf, ",\n  ''::pg_catalog.char AS attidentity");
		attidentity_col = cols++;
		if (pset.sversion >= 120000)
			appendPQExpBufferStr(&buf, ",\n  a.attgenerated");
		else
			appendPQExpBufferStr(&buf, ",\n  ''::pg_catalog.char AS attgenerated");
		attgenerated_col = cols++;
	}
	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX)
	{
		if (pset.sversion >= 110000)
		{
			appendPQExpBuffer(&buf, ",\n  CASE WHEN a.attnum <= (SELECT i.indnkeyatts FROM pg_catalog.pg_index i WHERE i.indexrelid = '%s') THEN '%s' ELSE '%s' END AS is_key",
							  oid,
							  gettext_noop("yes"),
							  gettext_noop("no"));
			isindexkey_col = cols++;
		}
		appendPQExpBufferStr(&buf, ",\n  pg_catalog.pg_get_indexdef(a.attrelid, a.attnum, TRUE) AS indexdef");
		indexdef_col = cols++;
	}
	/* 外部表列的 FDW 选项 */
	if (tableinfo.relkind == RELKIND_FOREIGN_TABLE)
	{
		appendPQExpBufferStr(&buf, ",\n  CASE WHEN attfdwoptions IS NULL THEN '' ELSE "
							 "  '(' || pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(option_name) || ' ' || pg_catalog.quote_literal(option_value)  FROM "
							 "  pg_catalog.pg_options_to_table(attfdwoptions)), ', ') || ')' END AS attfdwoptions");
		fdwopts_col = cols++;
	}
	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  a.attstorage");
		attstorage_col = cols++;

		/* 压缩信息（如果与 relkind 相关） */
		if (pset.sversion >= 140000 &&
			!pset.hide_compression &&
			(tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
			 tableinfo.relkind == RELKIND_MATVIEW))
		{
			appendPQExpBufferStr(&buf, ",\n  a.attcompression AS attcompression");
			attcompression_col = cols++;
		}

		/* 统计目标（如果与 relkind 相关） */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_INDEX ||
			tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
			tableinfo.relkind == RELKIND_MATVIEW ||
			tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBufferStr(&buf, ",\n  CASE WHEN a.attstattarget=-1 THEN NULL ELSE a.attstattarget END AS attstattarget");
			attstattarget_col = cols++;
		}

		/*
		 * 在 9.0+ 中，我们为以下对象提供列注释：关系、视图、
		 * 复合类型和外部表（参见 comment.c 中的 CommentObject()）。
		 */
		if (tableinfo.relkind == RELKIND_RELATION ||
			tableinfo.relkind == RELKIND_VIEW ||
			tableinfo.relkind == RELKIND_MATVIEW ||
			tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			tableinfo.relkind == RELKIND_COMPOSITE_TYPE ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBufferStr(&buf, ",\n  pg_catalog.col_description(a.attrelid, a.attnum)");
			attdescr_col = cols++;
		}
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_attribute a");
	appendPQExpBuffer(&buf, "\nWHERE a.attrelid = '%s' AND a.attnum > 0 AND NOT a.attisdropped", oid);
	appendPQExpBufferStr(&buf, "\nORDER BY a.attnum;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;
	numrows = PQntuples(res);

	/* 制作标题 */
	switch (tableinfo.relkind)
	{
		case RELKIND_RELATION:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Table \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_VIEW:
			printfPQExpBuffer(&title, _("View \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_MATVIEW:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged materialized view \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Materialized view \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_INDEX:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_PARTITIONED_INDEX:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged partitioned index \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned index \"%s.%s\""),
								  schemaname, relationname);
			break;
		case RELKIND_TOASTVALUE:
			printfPQExpBuffer(&title, _("TOAST table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			printfPQExpBuffer(&title, _("Composite type \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_FOREIGN_TABLE:
			printfPQExpBuffer(&title, _("Foreign table \"%s.%s\""),
							  schemaname, relationname);
			break;
		case RELKIND_PARTITIONED_TABLE:
			if (tableinfo.relpersistence == 'u')
				printfPQExpBuffer(&title, _("Unlogged partitioned table \"%s.%s\""),
								  schemaname, relationname);
			else
				printfPQExpBuffer(&title, _("Partitioned table \"%s.%s\""),
								  schemaname, relationname);
			break;
		default:
			/* 未翻译的未知 relkind */
			printfPQExpBuffer(&title, "?%c? \"%s.%s\"",
							  tableinfo.relkind, schemaname, relationname);
			break;
	}

	/* 用我们将输出的列名填充 headers[] */
	cols = 0;
	headers[cols++] = gettext_noop("Column");
	headers[cols++] = gettext_noop("Type");
	if (show_column_details)
	{
		headers[cols++] = gettext_noop("Collation");
		headers[cols++] = gettext_noop("Nullable");
		headers[cols++] = gettext_noop("Default");
	}
	if (isindexkey_col >= 0)
		headers[cols++] = gettext_noop("Key?");
	if (indexdef_col >= 0)
		headers[cols++] = gettext_noop("Definition");
	if (fdwopts_col >= 0)
		headers[cols++] = gettext_noop("FDW options");
	if (attstorage_col >= 0)
		headers[cols++] = gettext_noop("Storage");
	if (attcompression_col >= 0)
		headers[cols++] = gettext_noop("Compression");
	if (attstattarget_col >= 0)
		headers[cols++] = gettext_noop("Stats target");
	if (attdescr_col >= 0)
		headers[cols++] = gettext_noop("Description");

	Assert(cols <= lengthof(headers));

	printTableInit(&cont, &myopt, title.data, cols, numrows);
	printTableInitialized = true;

	for (i = 0; i < cols; i++)
		printTableAddHeader(&cont, headers[i], true, 'l');

	/* 生成要打印的表格单元格 */
	for (i = 0; i < numrows; i++)
	{
		/* 列 */
		printTableAddCell(&cont, PQgetvalue(res, i, attname_col), false, false);

		/* 类型 */
		printTableAddCell(&cont, PQgetvalue(res, i, atttype_col), false, false);

		/* 排序规则、可为空、默认值 */
		if (show_column_details)
		{
			char	   *identity;
			char	   *generated;
			char	   *default_str;
			bool		mustfree = false;

			printTableAddCell(&cont, PQgetvalue(res, i, attcoll_col), false, false);

			printTableAddCell(&cont,
							  strcmp(PQgetvalue(res, i, attnotnull_col), "t") == 0 ? "not null" : "",
							  false, false);

			identity = PQgetvalue(res, i, attidentity_col);
			generated = PQgetvalue(res, i, attgenerated_col);

			if (identity[0] == ATTRIBUTE_IDENTITY_ALWAYS)
				default_str = "generated always as identity";
			else if (identity[0] == ATTRIBUTE_IDENTITY_BY_DEFAULT)
				default_str = "generated by default as identity";
			else if (generated[0] == ATTRIBUTE_GENERATED_STORED)
			{
				default_str = psprintf("generated always as (%s) stored",
									   PQgetvalue(res, i, attrdef_col));
				mustfree = true;
			}
			else
				default_str = PQgetvalue(res, i, attrdef_col);

			printTableAddCell(&cont, default_str, false, mustfree);
		}

		/* 索引列的信息 */
		if (isindexkey_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, isindexkey_col), true, false);
		if (indexdef_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, indexdef_col), false, false);

		/* 外部表列的 FDW 选项 */
		if (fdwopts_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, fdwopts_col), false, false);

		/* 存储模式（如果相关） */
		if (attstorage_col >= 0)
		{
			char	   *storage = PQgetvalue(res, i, attstorage_col);

			/* 这些字符串在我们的语法中是字面量，因此不翻译。 */
			printTableAddCell(&cont, (storage[0] == 'p' ? "plain" :
									  (storage[0] == 'm' ? "main" :
									   (storage[0] == 'x' ? "extended" :
										(storage[0] == 'e' ? "external" :
										 "???")))),
							  false, false);
		}

		/* 列压缩（如果相关） */
		if (attcompression_col >= 0)
		{
			char	   *compression = PQgetvalue(res, i, attcompression_col);

			/* 这些字符串在我们的语法中是字面量，因此不翻译。 */
			printTableAddCell(&cont, (compression[0] == 'p' ? "pglz" :
									  (compression[0] == 'l' ? "lz4" :
									   (compression[0] == '\0' ? "" :
										"???"))),
							  false, false);
		}

		/* 统计目标（如果 relkind 支持此功能） */
		if (attstattarget_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attstattarget_col),
							  false, false);

		/* 列注释（如果 relkind 支持此功能） */
		if (attdescr_col >= 0)
			printTableAddCell(&cont, PQgetvalue(res, i, attdescr_col),
							  false, false);
	}

	/* 制作页脚 */

	if (tableinfo.ispartition)
	{
		/* 分区子表的页脚信息 */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT inhparent::pg_catalog.regclass,\n"
						  "  pg_catalog.pg_get_expr(c.relpartbound, c.oid),\n  ");

		appendPQExpBufferStr(&buf,
							 pset.sversion >= 140000 ? "inhdetachpending" :
							 "false as inhdetachpending");

		/* 如果 verbose，还要请求分区约束定义 */
		if (verbose)
			appendPQExpBufferStr(&buf,
								 ",\n  pg_catalog.pg_get_partition_constraintdef(c.oid)");
		appendPQExpBuffer(&buf,
						  "\nFROM pg_catalog.pg_class c"
						  " JOIN pg_catalog.pg_inherits i"
						  " ON c.oid = inhrelid"
						  "\nWHERE c.oid = '%s';", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
		{
			char	   *parent_name = PQgetvalue(result, 0, 0);
			char	   *partdef = PQgetvalue(result, 0, 1);
			char	   *detached = PQgetvalue(result, 0, 2);

			printfPQExpBuffer(&tmpbuf, _("Partition of: %s %s%s"), parent_name,
							  partdef,
							  strcmp(detached, "t") == 0 ? " DETACH PENDING" : "");
			printTableAddFooter(&cont, tmpbuf.data);

			if (verbose)
			{
				char	   *partconstraintdef = NULL;

				if (!PQgetisnull(result, 0, 3))
					partconstraintdef = PQgetvalue(result, 0, 3);
				/* 如果没有任何约束，明确显示出来 */
				if (partconstraintdef == NULL || partconstraintdef[0] == '\0')
					printfPQExpBuffer(&tmpbuf, _("No partition constraint"));
				else
					printfPQExpBuffer(&tmpbuf, _("Partition constraint: %s"),
									  partconstraintdef);
				printTableAddFooter(&cont, tmpbuf.data);
			}
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* 分区表（分区父表）的页脚信息 */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_partkeydef('%s'::pg_catalog.oid);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *partkeydef = PQgetvalue(result, 0, 0);

			printfPQExpBuffer(&tmpbuf, _("Partition key: %s"), partkeydef);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* 对于 TOAST 表，打印所属表的名称 */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT n.nspname, c.relname\n"
						  "FROM pg_catalog.pg_class c"
						  " JOIN pg_catalog.pg_namespace n"
						  " ON n.oid = c.relnamespace\n"
						  "WHERE reltoastrelid = '%s';", oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) == 1)
		{
			char	   *schemaname = PQgetvalue(result, 0, 0);
			char	   *relname = PQgetvalue(result, 0, 1);

			printfPQExpBuffer(&tmpbuf, _("Owning table: \"%s.%s\""),
							  schemaname, relname);
			printTableAddFooter(&cont, tmpbuf.data);
		}
		PQclear(result);
	}

	if (tableinfo.relkind == RELKIND_INDEX ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX)
	{
		/* 索引的页脚信息 */
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT i.indisunique, i.indisprimary, i.indisclustered, "
						  "i.indisvalid,\n"
						  "  (NOT i.indimmediate) AND "
						  "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = i.indrelid AND "
						  "conindid = i.indexrelid AND "
						  "contype IN ('p','u','x') AND "
						  "condeferrable) AS condeferrable,\n"
						  "  (NOT i.indimmediate) AND "
						  "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = i.indrelid AND "
						  "conindid = i.indexrelid AND "
						  "contype IN ('p','u','x') AND "
						  "condeferred) AS condeferred,\n");

		if (pset.sversion >= 90400)
			appendPQExpBufferStr(&buf, "i.indisreplident,\n");
		else
			appendPQExpBufferStr(&buf, "false AS indisreplident,\n");

		if (pset.sversion >= 150000)
			appendPQExpBufferStr(&buf, "i.indnullsnotdistinct,\n");
		else
			appendPQExpBufferStr(&buf, "false AS indnullsnotdistinct,\n");

		appendPQExpBuffer(&buf, "  a.amname, c2.relname, "
						  "pg_catalog.pg_get_expr(i.indpred, i.indrelid, true)\n"
						  "FROM pg_catalog.pg_index i, pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_am a\n"
						  "WHERE i.indexrelid = c.oid AND c.oid = '%s' AND c.relam = a.oid\n"
						  "AND i.indrelid = c2.oid;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else if (PQntuples(result) != 1)
		{
			PQclear(result);
			goto error_return;
		}
		else
		{
			char	   *indisunique = PQgetvalue(result, 0, 0);
			char	   *indisprimary = PQgetvalue(result, 0, 1);
			char	   *indisclustered = PQgetvalue(result, 0, 2);
			char	   *indisvalid = PQgetvalue(result, 0, 3);
			char	   *deferrable = PQgetvalue(result, 0, 4);
			char	   *deferred = PQgetvalue(result, 0, 5);
			char	   *indisreplident = PQgetvalue(result, 0, 6);
			char	   *indnullsnotdistinct = PQgetvalue(result, 0, 7);
			char	   *indamname = PQgetvalue(result, 0, 8);
			char	   *indtable = PQgetvalue(result, 0, 9);
			char	   *indpred = PQgetvalue(result, 0, 10);

			if (strcmp(indisprimary, "t") == 0)
				printfPQExpBuffer(&tmpbuf, _("primary key, "));
			else if (strcmp(indisunique, "t") == 0)
			{
				printfPQExpBuffer(&tmpbuf, _("unique"));
				if (strcmp(indnullsnotdistinct, "t") == 0)
					appendPQExpBufferStr(&tmpbuf, _(" nulls not distinct"));
				appendPQExpBufferStr(&tmpbuf, _(", "));
			}
			else
				resetPQExpBuffer(&tmpbuf);
			appendPQExpBuffer(&tmpbuf, "%s, ", indamname);

			/* 这里我们假设索引和表在同一模式中 */
			appendPQExpBuffer(&tmpbuf, _("for table \"%s.%s\""),
							  schemaname, indtable);

			if (strlen(indpred))
				appendPQExpBuffer(&tmpbuf, _(", predicate (%s)"), indpred);

			if (strcmp(indisclustered, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", clustered"));

			if (strcmp(indisvalid, "t") != 0)
				appendPQExpBufferStr(&tmpbuf, _(", invalid"));

			if (strcmp(deferrable, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", deferrable"));

			if (strcmp(deferred, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", initially deferred"));

			if (strcmp(indisreplident, "t") == 0)
				appendPQExpBufferStr(&tmpbuf, _(", replica identity"));

			printTableAddFooter(&cont, tmpbuf.data);

			/*
			 * 如果是分区索引，我们将在下面打印表空间
			 */
			if (tableinfo.relkind == RELKIND_INDEX)
				add_tablespace_footer(&cont, tableinfo.relkind,
									  tableinfo.tablespace, true);
		}

		PQclear(result);
	}
	/* 如果在这里添加 relkind，另请参阅下面的"完成打印..."节 */
	else if (tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_MATVIEW ||
			 tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
			 tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
			 tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
			 tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		/* 表的页脚信息 */
		PGresult   *result = NULL;
		int			tuples = 0;

		/* 打印索引 */
		if (tableinfo.hasindex)
		{
			printfPQExpBuffer(&buf,
							  "SELECT c2.relname, i.indisprimary, i.indisunique, "
							  "i.indisclustered, i.indisvalid, "
							  "pg_catalog.pg_get_indexdef(i.indexrelid, 0, true),\n  "
							  "pg_catalog.pg_get_constraintdef(con.oid, true), "
							  "contype, condeferrable, condeferred");
			if (pset.sversion >= 90400)
				appendPQExpBufferStr(&buf, ", i.indisreplident");
			else
				appendPQExpBufferStr(&buf, ", false AS indisreplident");
			appendPQExpBufferStr(&buf, ", c2.reltablespace");
			appendPQExpBuffer(&buf,
							  "\nFROM pg_catalog.pg_class c, pg_catalog.pg_class c2, pg_catalog.pg_index i\n"
							  "  LEFT JOIN pg_catalog.pg_constraint con ON (conrelid = i.indrelid AND conindid = i.indexrelid AND contype IN ('p','u','x'))\n"
							  "WHERE c.oid = '%s' AND c.oid = i.indrelid AND i.indexrelid = c2.oid\n"
							  "ORDER BY i.indisprimary DESC, c2.relname;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Indexes:"));
				for (i = 0; i < tuples; i++)
				{
					/* 未翻译的索引名称 */
					printfPQExpBuffer(&buf, "    \"%s\"",
									  PQgetvalue(result, i, 0));

					/* 如果是排他约束，打印 constraintdef */
					if (strcmp(PQgetvalue(result, i, 7), "x") == 0)
					{
						appendPQExpBuffer(&buf, " %s",
										  PQgetvalue(result, i, 6));
					}
					else
					{
						const char *indexdef;
						const char *usingpos;

						/* 标记为主键或唯一（但不能两者都标） */
						if (strcmp(PQgetvalue(result, i, 1), "t") == 0)
							appendPQExpBufferStr(&buf, " PRIMARY KEY,");
						else if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
						{
							if (strcmp(PQgetvalue(result, i, 7), "u") == 0)
								appendPQExpBufferStr(&buf, " UNIQUE CONSTRAINT,");
							else
								appendPQExpBufferStr(&buf, " UNIQUE,");
						}

						/* "USING" 之后的所有内容原样回显 */
						indexdef = PQgetvalue(result, i, 5);
						usingpos = strstr(indexdef, " USING ");
						if (usingpos)
							indexdef = usingpos + 7;
						appendPQExpBuffer(&buf, " %s", indexdef);

						/* 可延迟的 PK/UNIQUE 索引需要这些 */
						if (strcmp(PQgetvalue(result, i, 8), "t") == 0)
							appendPQExpBufferStr(&buf, " DEFERRABLE");

						if (strcmp(PQgetvalue(result, i, 9), "t") == 0)
							appendPQExpBufferStr(&buf, " INITIALLY DEFERRED");
					}

					/* 为所有情况添加这些 */
					if (strcmp(PQgetvalue(result, i, 3), "t") == 0)
						appendPQExpBufferStr(&buf, " CLUSTER");

					if (strcmp(PQgetvalue(result, i, 4), "t") != 0)
						appendPQExpBufferStr(&buf, " INVALID");

					if (strcmp(PQgetvalue(result, i, 10), "t") == 0)
						appendPQExpBufferStr(&buf, " REPLICA IDENTITY");

					printTableAddFooter(&cont, buf.data);

					/* 在同一行上打印索引的表空间 */
					add_tablespace_footer(&cont, RELKIND_INDEX,
										  atooid(PQgetvalue(result, i, 11)),
										  false);
				}
			}
			PQclear(result);
		}

		/* 打印表（和列）检查约束 */
		if (tableinfo.checks)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.conname, "
							  "pg_catalog.pg_get_constraintdef(r.oid, true)\n"
							  "FROM pg_catalog.pg_constraint r\n"
							  "WHERE r.conrelid = '%s' AND r.contype = 'c'\n"
							  "ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Check constraints:"));
				for (i = 0; i < tuples; i++)
				{
					/* 未翻译的约束名称和定义 */
					printfPQExpBuffer(&buf, "    \"%s\" %s",
									  PQgetvalue(result, i, 0),
									  PQgetvalue(result, i, 1));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/*
		 * 打印外键约束（如果没有触发器则没有外键约束，
		 * 但如果表是分区表则例外，此时触发器
		 * 出现在分区中）
		 */
		if (tableinfo.hastriggers ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			if (pset.sversion >= 120000 &&
				(tableinfo.ispartition || tableinfo.relkind == RELKIND_PARTITIONED_TABLE))
			{
				/*
				 * 将在此表中定义的约束放在前面，
				 * 其后是在祖先分区表中定义的约束。
				 */
				printfPQExpBuffer(&buf,
								  "SELECT conrelid = '%s'::pg_catalog.regclass AS sametable,\n"
								  "       conname,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef,\n"
								  "       conrelid::pg_catalog.regclass AS ontable\n"
								  "  FROM pg_catalog.pg_constraint,\n"
								  "       pg_catalog.pg_partition_ancestors('%s')\n"
								  " WHERE conrelid = relid AND contype = 'f' AND conparentid = 0\n"
								  "ORDER BY sametable DESC, conname;",
								  oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT true as sametable, conname,\n"
								  "  pg_catalog.pg_get_constraintdef(r.oid, true) as condef,\n"
								  "  conrelid::pg_catalog.regclass AS ontable\n"
								  "FROM pg_catalog.pg_constraint r\n"
								  "WHERE r.conrelid = '%s' AND r.contype = 'f'\n",
								  oid);

				if (pset.sversion >= 120000)
					appendPQExpBufferStr(&buf, "     AND conparentid = 0\n");
				appendPQExpBufferStr(&buf, "ORDER BY conname");
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				int			i_sametable = PQfnumber(result, "sametable"),
							i_conname = PQfnumber(result, "conname"),
							i_condef = PQfnumber(result, "condef"),
							i_ontable = PQfnumber(result, "ontable");

				printTableAddFooter(&cont, _("Foreign-key constraints:"));
				for (i = 0; i < tuples; i++)
				{
					/*
					 * 打印未翻译的约束名称和定义。当约束定义为
					 * 父分区表中的约束时使用 "TABLE tab" 前缀。
					 *
					 */
					if (strcmp(PQgetvalue(result, i, i_sametable), "f") == 0)
						printfPQExpBuffer(&buf, "    TABLE \"%s\" CONSTRAINT \"%s\" %s",
										  PQgetvalue(result, i, i_ontable),
										  PQgetvalue(result, i, i_conname),
										  PQgetvalue(result, i, i_condef));
					else
						printfPQExpBuffer(&buf, "    \"%s\" %s",
										  PQgetvalue(result, i, i_conname),
										  PQgetvalue(result, i, i_condef));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* 打印传入的外键引用 */
		if (tableinfo.hastriggers ||
			tableinfo.relkind == RELKIND_PARTITIONED_TABLE)
		{
			if (pset.sversion >= 120000)
			{
				printfPQExpBuffer(&buf,
								  "SELECT conname, conrelid::pg_catalog.regclass AS ontable,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef\n"
								  "  FROM pg_catalog.pg_constraint c\n"
								  " WHERE confrelid IN (SELECT pg_catalog.pg_partition_ancestors('%s')\n"
								  "                     UNION ALL VALUES ('%s'::pg_catalog.regclass))\n"
								  "       AND contype = 'f' AND conparentid = 0\n"
								  "ORDER BY conname;",
								  oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT conname, conrelid::pg_catalog.regclass AS ontable,\n"
								  "       pg_catalog.pg_get_constraintdef(oid, true) AS condef\n"
								  "  FROM pg_catalog.pg_constraint\n"
								  " WHERE confrelid = %s AND contype = 'f'\n"
								  "ORDER BY conname;",
								  oid);
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				int			i_conname = PQfnumber(result, "conname"),
							i_ontable = PQfnumber(result, "ontable"),
							i_condef = PQfnumber(result, "condef");

				printTableAddFooter(&cont, _("Referenced by:"));
				for (i = 0; i < tuples; i++)
				{
					printfPQExpBuffer(&buf, "    TABLE \"%s\" CONSTRAINT \"%s\" %s",
									  PQgetvalue(result, i, i_ontable),
									  PQgetvalue(result, i, i_conname),
									  PQgetvalue(result, i, i_condef));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* 打印任何行级策略 */
		if (pset.sversion >= 90500)
		{
			printfPQExpBuffer(&buf, "SELECT pol.polname,");
			if (pset.sversion >= 100000)
				appendPQExpBufferStr(&buf,
									 " pol.polpermissive,\n");
			else
				appendPQExpBufferStr(&buf,
									 " 't' as polpermissive,\n");
			appendPQExpBuffer(&buf,
							  "  CASE WHEN pol.polroles = '{0}' THEN NULL ELSE pg_catalog.array_to_string(array(select rolname from pg_catalog.pg_roles where oid = any (pol.polroles) order by 1),',') END,\n"
							  "  pg_catalog.pg_get_expr(pol.polqual, pol.polrelid),\n"
							  "  pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid),\n"
							  "  CASE pol.polcmd\n"
							  "    WHEN 'r' THEN 'SELECT'\n"
							  "    WHEN 'a' THEN 'INSERT'\n"
							  "    WHEN 'w' THEN 'UPDATE'\n"
							  "    WHEN 'd' THEN 'DELETE'\n"
							  "    END AS cmd\n"
							  "FROM pg_catalog.pg_policy pol\n"
							  "WHERE pol.polrelid = '%s' ORDER BY 1;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			/*
			 * 处理 RLS 已启用且有策略的情况，或
			 * 没有策略的情况，或 RLS 未启用但
			 * 有策略的情况
			 */
			if (tableinfo.rowsecurity && !tableinfo.forcerowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies:"));

			if (tableinfo.rowsecurity && tableinfo.forcerowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies (forced row security enabled):"));

			if (tableinfo.rowsecurity && !tableinfo.forcerowsecurity && tuples == 0)
				printTableAddFooter(&cont, _("Policies (row security enabled): (none)"));

			if (tableinfo.rowsecurity && tableinfo.forcerowsecurity && tuples == 0)
				printTableAddFooter(&cont, _("Policies (forced row security enabled): (none)"));

			if (!tableinfo.rowsecurity && tuples > 0)
				printTableAddFooter(&cont, _("Policies (row security disabled):"));

			/* 可能是空集合 - 没问题 */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    POLICY \"%s\"",
								  PQgetvalue(result, i, 0));

				if (*(PQgetvalue(result, i, 1)) == 'f')
					appendPQExpBufferStr(&buf, " AS RESTRICTIVE");

				if (!PQgetisnull(result, i, 5))
					appendPQExpBuffer(&buf, " FOR %s",
									  PQgetvalue(result, i, 5));

				if (!PQgetisnull(result, i, 2))
				{
					appendPQExpBuffer(&buf, "\n      TO %s",
									  PQgetvalue(result, i, 2));
				}

				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, "\n      USING (%s)",
									  PQgetvalue(result, i, 3));

				if (!PQgetisnull(result, i, 4))
					appendPQExpBuffer(&buf, "\n      WITH CHECK (%s)",
									  PQgetvalue(result, i, 4));

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/* 打印任何扩展统计信息 */
		if (pset.sversion >= 140000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT oid, "
							  "stxrelid::pg_catalog.regclass, "
							  "stxnamespace::pg_catalog.regnamespace::pg_catalog.text AS nsp, "
							  "stxname,\n"
							  "pg_catalog.pg_get_statisticsobjdef_columns(oid) AS columns,\n"
							  "  'd' = any(stxkind) AS ndist_enabled,\n"
							  "  'f' = any(stxkind) AS deps_enabled,\n"
							  "  'm' = any(stxkind) AS mcv_enabled,\n"
							  "stxstattarget\n"
							  "FROM pg_catalog.pg_statistic_ext\n"
							  "WHERE stxrelid = '%s'\n"
							  "ORDER BY nsp, stxname;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Statistics objects:"));

				for (i = 0; i < tuples; i++)
				{
					bool		gotone = false;
					bool		has_ndistinct;
					bool		has_dependencies;
					bool		has_mcv;
					bool		has_all;
					bool		has_some;

					has_ndistinct = (strcmp(PQgetvalue(result, i, 5), "t") == 0);
					has_dependencies = (strcmp(PQgetvalue(result, i, 6), "t") == 0);
					has_mcv = (strcmp(PQgetvalue(result, i, 7), "t") == 0);

					printfPQExpBuffer(&buf, "    ");

					/* 统计对象名称（使用命名空间限定） */
					appendPQExpBuffer(&buf, "\"%s.%s\"",
									  PQgetvalue(result, i, 2),
									  PQgetvalue(result, i, 3));

					/*
					 * 打印种类时，我们忽略表达式统计信息，
					 * 因为它们仅供内部使用，用户无法指定。
					 * 当未指定种类时（此时它必须是单个
					 * 表达式的统计信息）或指定了所有种类时
					 * （此时我们假设它是通过 CREATE STATISTICS 展开的），
					 * 我们不打印种类。
					 */
					has_all = (has_ndistinct && has_dependencies && has_mcv);
					has_some = (has_ndistinct || has_dependencies || has_mcv);

					if (has_some && !has_all)
					{
						appendPQExpBufferStr(&buf, " (");

						/* 选项 */
						if (has_ndistinct)
						{
							appendPQExpBufferStr(&buf, "ndistinct");
							gotone = true;
						}

						if (has_dependencies)
						{
							appendPQExpBuffer(&buf, "%sdependencies", gotone ? ", " : "");
							gotone = true;
						}

						if (has_mcv)
						{
							appendPQExpBuffer(&buf, "%smcv", gotone ? ", " : "");
						}

						appendPQExpBufferChar(&buf, ')');
					}

					appendPQExpBuffer(&buf, " ON %s FROM %s",
									  PQgetvalue(result, i, 4),
									  PQgetvalue(result, i, 1));

					/* 如果统计目标不是默认值则显示 */
					if (strcmp(PQgetvalue(result, i, 8), "-1") != 0)
						appendPQExpBuffer(&buf, "; STATISTICS %s",
										  PQgetvalue(result, i, 8));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}
		else if (pset.sversion >= 100000)
		{
			printfPQExpBuffer(&buf,
							  "SELECT oid, "
							  "stxrelid::pg_catalog.regclass, "
							  "stxnamespace::pg_catalog.regnamespace AS nsp, "
							  "stxname,\n"
							  "  (SELECT pg_catalog.string_agg(pg_catalog.quote_ident(attname),', ')\n"
							  "   FROM pg_catalog.unnest(stxkeys) s(attnum)\n"
							  "   JOIN pg_catalog.pg_attribute a ON (stxrelid = a.attrelid AND\n"
							  "        a.attnum = s.attnum AND NOT attisdropped)) AS columns,\n"
							  "  'd' = any(stxkind) AS ndist_enabled,\n"
							  "  'f' = any(stxkind) AS deps_enabled,\n"
							  "  'm' = any(stxkind) AS mcv_enabled,\n");

			if (pset.sversion >= 130000)
				appendPQExpBufferStr(&buf, "  stxstattarget\n");
			else
				appendPQExpBufferStr(&buf, "  -1 AS stxstattarget\n");
			appendPQExpBuffer(&buf, "FROM pg_catalog.pg_statistic_ext\n"
							  "WHERE stxrelid = '%s'\n"
							  "ORDER BY 1;",
							  oid);

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				printTableAddFooter(&cont, _("Statistics objects:"));

				for (i = 0; i < tuples; i++)
				{
					bool		gotone = false;

					printfPQExpBuffer(&buf, "    ");

					/* 统计对象名称（使用命名空间限定） */
					appendPQExpBuffer(&buf, "\"%s.%s\" (",
									  PQgetvalue(result, i, 2),
									  PQgetvalue(result, i, 3));

					/* 选项 */
					if (strcmp(PQgetvalue(result, i, 5), "t") == 0)
					{
						appendPQExpBufferStr(&buf, "ndistinct");
						gotone = true;
					}

					if (strcmp(PQgetvalue(result, i, 6), "t") == 0)
					{
						appendPQExpBuffer(&buf, "%sdependencies", gotone ? ", " : "");
						gotone = true;
					}

					if (strcmp(PQgetvalue(result, i, 7), "t") == 0)
					{
						appendPQExpBuffer(&buf, "%smcv", gotone ? ", " : "");
					}

					appendPQExpBuffer(&buf, ") ON %s FROM %s",
									  PQgetvalue(result, i, 4),
									  PQgetvalue(result, i, 1));

					/* 如果统计目标不是默认值则显示 */
					if (strcmp(PQgetvalue(result, i, 8), "-1") != 0)
						appendPQExpBuffer(&buf, "; STATISTICS %s",
										  PQgetvalue(result, i, 8));

					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}

		/* 打印规则 */
		if (tableinfo.hasrules && tableinfo.relkind != RELKIND_MATVIEW)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true)), "
							  "ev_enabled\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
			{
				bool		have_heading;
				int			category;

				for (category = 0; category < 4; category++)
				{
					have_heading = false;

					for (i = 0; i < tuples; i++)
					{
						const char *ruledef;
						bool		list_rule = false;

						switch (category)
						{
							case 0:
								if (*PQgetvalue(result, i, 2) == 'O')
									list_rule = true;
								break;
							case 1:
								if (*PQgetvalue(result, i, 2) == 'D')
									list_rule = true;
								break;
							case 2:
								if (*PQgetvalue(result, i, 2) == 'A')
									list_rule = true;
								break;
							case 3:
								if (*PQgetvalue(result, i, 2) == 'R')
									list_rule = true;
								break;
						}
						if (!list_rule)
							continue;

						if (!have_heading)
						{
							switch (category)
							{
								case 0:
									printfPQExpBuffer(&buf, _("Rules:"));
									break;
								case 1:
									printfPQExpBuffer(&buf, _("Disabled rules:"));
									break;
								case 2:
									printfPQExpBuffer(&buf, _("Rules firing always:"));
									break;
								case 3:
									printfPQExpBuffer(&buf, _("Rules firing on replica only:"));
									break;
							}
							printTableAddFooter(&cont, buf.data);
							have_heading = true;
						}

						/* "CREATE RULE" 之后的所有内容原样回显 */
						ruledef = PQgetvalue(result, i, 1);
						ruledef += 12;
						printfPQExpBuffer(&buf, "    %s", ruledef);
						printTableAddFooter(&cont, buf.data);
					}
				}
			}
			PQclear(result);
		}

		/* 打印任何发布 */
		if (pset.sversion >= 100000)
		{
			if (pset.sversion >= 150000)
			{
				printfPQExpBuffer(&buf,
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "     JOIN pg_catalog.pg_publication_namespace pn ON p.oid = pn.pnpubid\n"
								  "     JOIN pg_catalog.pg_class pc ON pc.relnamespace = pn.pnnspid\n"
								  "WHERE pc.oid ='%s' and pg_catalog.pg_relation_is_publishable('%s')\n"
								  "UNION\n"
								  "SELECT pubname\n"
								  "     , pg_get_expr(pr.prqual, c.oid)\n"
								  "     , (CASE WHEN pr.prattrs IS NOT NULL THEN\n"
								  "         (SELECT string_agg(attname, ', ')\n"
								  "           FROM pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
								  "                pg_catalog.pg_attribute\n"
								  "          WHERE attrelid = pr.prrelid AND attnum = prattrs[s])\n"
								  "        ELSE NULL END) "
								  "FROM pg_catalog.pg_publication p\n"
								  "     JOIN pg_catalog.pg_publication_rel pr ON p.oid = pr.prpubid\n"
								  "     JOIN pg_catalog.pg_class c ON c.oid = pr.prrelid\n"
								  "WHERE pr.prrelid = '%s'\n"
								  "UNION\n"
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "WHERE p.puballtables AND pg_catalog.pg_relation_is_publishable('%s')\n"
								  "ORDER BY 1;",
								  oid, oid, oid, oid);
			}
			else
			{
				printfPQExpBuffer(&buf,
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "JOIN pg_catalog.pg_publication_rel pr ON p.oid = pr.prpubid\n"
								  "WHERE pr.prrelid = '%s'\n"
								  "UNION ALL\n"
								  "SELECT pubname\n"
								  "     , NULL\n"
								  "     , NULL\n"
								  "FROM pg_catalog.pg_publication p\n"
								  "WHERE p.puballtables AND pg_catalog.pg_relation_is_publishable('%s')\n"
								  "ORDER BY 1;",
								  oid, oid);
			}

			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else
				tuples = PQntuples(result);

			if (tuples > 0)
				printTableAddFooter(&cont, _("Publications:"));

			/* 可能是空集合 - 没问题 */
			for (i = 0; i < tuples; i++)
			{
				printfPQExpBuffer(&buf, "    \"%s\"",
								  PQgetvalue(result, i, 0));

				/* 列列表（如果有） */
				if (!PQgetisnull(result, i, 2))
					appendPQExpBuffer(&buf, " (%s)",
									  PQgetvalue(result, i, 2));

				/* 行过滤器（如果有） */
				if (!PQgetisnull(result, i, 1))
					appendPQExpBuffer(&buf, " WHERE %s",
									  PQgetvalue(result, i, 1));

				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}
	}

	/* 如果表是视图或物化视图则获取 view_def */
	if ((tableinfo.relkind == RELKIND_VIEW ||
		 tableinfo.relkind == RELKIND_MATVIEW) && verbose)
	{
		PGresult   *result;

		printfPQExpBuffer(&buf,
						  "SELECT pg_catalog.pg_get_viewdef('%s'::pg_catalog.oid, true);",
						  oid);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;

		if (PQntuples(result) > 0)
			view_def = pg_strdup(PQgetvalue(result, 0, 0));

		PQclear(result);
	}

	if (view_def)
	{
		PGresult   *result = NULL;

		/* 视图的页脚信息 */
		printTableAddFooter(&cont, _("View definition:"));
		printTableAddFooter(&cont, view_def);

		/* 打印规则 */
		if (tableinfo.hasrules)
		{
			printfPQExpBuffer(&buf,
							  "SELECT r.rulename, trim(trailing ';' from pg_catalog.pg_get_ruledef(r.oid, true))\n"
							  "FROM pg_catalog.pg_rewrite r\n"
							  "WHERE r.ev_class = '%s' AND r.rulename != '_RETURN' ORDER BY 1;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;

			if (PQntuples(result) > 0)
			{
				printTableAddFooter(&cont, _("Rules:"));
				for (i = 0; i < PQntuples(result); i++)
				{
					const char *ruledef;

					/* "CREATE RULE" 之后的所有内容原样回显 */
					ruledef = PQgetvalue(result, i, 1);
					ruledef += 12;

					printfPQExpBuffer(&buf, " %s", ruledef);
					printTableAddFooter(&cont, buf.data);
				}
			}
			PQclear(result);
		}
	}

	/*
	 * 接下来打印触发器（如果有，但仅限于用户定义的触发器）。
	 * 这可以适用于表或视图。
	 */
	if (tableinfo.hastriggers)
	{
		PGresult   *result;
		int			tuples;

		printfPQExpBuffer(&buf,
						  "SELECT t.tgname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, true), "
						  "t.tgenabled, t.tgisinternal,\n");

		/*
		 * 检测每个触发器是否被继承，如果是，获取其继承
		 * 来源的最顶层表名。在 v13 之前，由于缺少 tgparentid 列，
		 * 我们无法轻松做到这一点。即使有了 tgparentid，直接
		 * 搜索最顶层父表也需要递归 CTE，这似乎过于昂贵。
		 * 我们稍微取巧地假设父触发器按 tgname 匹配；然后，
		 * 与 pg_partition_ancestors() 进行连接，使得规划器可以
		 * 根据需要利用 pg_trigger_tgrelid_tgname_index。我们通过
		 * 在分区祖先顺序中停在第一个满足 tgparentid = 0 的
		 * 触发器来确保找到正确的最顶层父表。（栈上更上层
		 * 可能存在名称相同但不相关的非继承触发器，
		 * 所以这一点很重要。）
		 *
		 */
		if (pset.sversion >= 130000)
			appendPQExpBufferStr(&buf,
								 "  CASE WHEN t.tgparentid != 0 THEN\n"
								 "    (SELECT u.tgrelid::pg_catalog.regclass\n"
								 "     FROM pg_catalog.pg_trigger AS u,\n"
								 "          pg_catalog.pg_partition_ancestors(t.tgrelid) WITH ORDINALITY AS a(relid, depth)\n"
								 "     WHERE u.tgname = t.tgname AND u.tgrelid = a.relid\n"
								 "           AND u.tgparentid = 0\n"
								 "     ORDER BY a.depth LIMIT 1)\n"
								 "  END AS parent\n");
		else
			appendPQExpBufferStr(&buf, "  NULL AS parent\n");

		appendPQExpBuffer(&buf,
						  "FROM pg_catalog.pg_trigger t\n"
						  "WHERE t.tgrelid = '%s' AND ",
						  oid);

		/*
		 * 在 v11 到 v14 的服务器中，分区的继承触发器
		 * 将 tgisinternal 设置为 true，尽管这些仍必须向用户显示。
		 * 因此我们使用另一个对于此类继承触发器为真的
		 * 属性来避免它们被隐藏，即它们对另一个触发器
		 * 的依赖关系。
		 */
		if (pset.sversion >= 110000 && pset.sversion < 150000)
			appendPQExpBufferStr(&buf, "(NOT t.tgisinternal OR (t.tgisinternal AND t.tgenabled = 'D') \n"
								 "    OR EXISTS (SELECT 1 FROM pg_catalog.pg_depend WHERE objid = t.oid \n"
								 "        AND refclassid = 'pg_catalog.pg_trigger'::pg_catalog.regclass))");
		else
			/* 显示/警告禁用的内部触发器 */
			appendPQExpBufferStr(&buf, "(NOT t.tgisinternal OR (t.tgisinternal AND t.tgenabled = 'D'))");
		appendPQExpBufferStr(&buf, "\nORDER BY 1;");

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
			tuples = PQntuples(result);

		if (tuples > 0)
		{
			bool		have_heading;
			int			category;

			/*
			 * 将输出分为 4 个不同的类别。已启用的触发器、
			 * 禁用的触发器以及两个特殊的 ALWAYS 和 REPLICA
			 * 配置。
			 */
			for (category = 0; category <= 4; category++)
			{
				have_heading = false;
				for (i = 0; i < tuples; i++)
				{
					bool		list_trigger;
					const char *tgdef;
					const char *usingpos;
					const char *tgenabled;
					const char *tgisinternal;

					/*
					 * 检查此触发器是否属于当前类别
					 */
					tgenabled = PQgetvalue(result, i, 2);
					tgisinternal = PQgetvalue(result, i, 3);
					list_trigger = false;
					switch (category)
					{
						case 0:
							if (*tgenabled == 'O' || *tgenabled == 't')
								list_trigger = true;
							break;
						case 1:
							if ((*tgenabled == 'D' || *tgenabled == 'f') &&
								*tgisinternal == 'f')
								list_trigger = true;
							break;
						case 2:
							if ((*tgenabled == 'D' || *tgenabled == 'f') &&
								*tgisinternal == 't')
								list_trigger = true;
							break;
						case 3:
							if (*tgenabled == 'A')
								list_trigger = true;
							break;
						case 4:
							if (*tgenabled == 'R')
								list_trigger = true;
							break;
					}
					if (list_trigger == false)
						continue;

					/* 打印类别标题一次 */
					if (have_heading == false)
					{
						switch (category)
						{
							case 0:
								printfPQExpBuffer(&buf, _("Triggers:"));
								break;
							case 1:
								printfPQExpBuffer(&buf, _("Disabled user triggers:"));
								break;
							case 2:
								printfPQExpBuffer(&buf, _("Disabled internal triggers:"));
								break;
							case 3:
								printfPQExpBuffer(&buf, _("Triggers firing always:"));
								break;
							case 4:
								printfPQExpBuffer(&buf, _("Triggers firing on replica only:"));
								break;
						}
						printTableAddFooter(&cont, buf.data);
						have_heading = true;
					}

					/* "TRIGGER" 之后的所有内容原样回显 */
					tgdef = PQgetvalue(result, i, 1);
					usingpos = strstr(tgdef, " TRIGGER ");
					if (usingpos)
						tgdef = usingpos + 9;

					printfPQExpBuffer(&buf, "    %s", tgdef);

					/* 在视觉上区分继承的触发器 */
					if (!PQgetisnull(result, i, 4))
						appendPQExpBuffer(&buf, ", ON TABLE %s",
										  PQgetvalue(result, i, 4));

					printTableAddFooter(&cont, buf.data);
				}
			}
		}
		PQclear(result);
	}

	/*
	 * 完成打印表的页脚信息。
	 */
	if (tableinfo.relkind == RELKIND_RELATION ||
		tableinfo.relkind == RELKIND_MATVIEW ||
		tableinfo.relkind == RELKIND_FOREIGN_TABLE ||
		tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
		tableinfo.relkind == RELKIND_PARTITIONED_INDEX ||
		tableinfo.relkind == RELKIND_TOASTVALUE)
	{
		bool		is_partitioned;
		PGresult   *result;
		int			tuples;

		/* 简化下面的一些重复测试 */
		is_partitioned = (tableinfo.relkind == RELKIND_PARTITIONED_TABLE ||
						  tableinfo.relkind == RELKIND_PARTITIONED_INDEX);

		/* 打印外部服务器名称 */
		if (tableinfo.relkind == RELKIND_FOREIGN_TABLE)
		{
			char	   *ftoptions;

			/* 外部表的页脚信息 */
			printfPQExpBuffer(&buf,
							  "SELECT s.srvname,\n"
							  "  pg_catalog.array_to_string(ARRAY(\n"
							  "    SELECT pg_catalog.quote_ident(option_name)"
							  " || ' ' || pg_catalog.quote_literal(option_value)\n"
							  "    FROM pg_catalog.pg_options_to_table(ftoptions)),  ', ')\n"
							  "FROM pg_catalog.pg_foreign_table f,\n"
							  "     pg_catalog.pg_foreign_server s\n"
							  "WHERE f.ftrelid = '%s' AND s.oid = f.ftserver;",
							  oid);
			result = PSQLexec(buf.data);
			if (!result)
				goto error_return;
			else if (PQntuples(result) != 1)
			{
				PQclear(result);
				goto error_return;
			}

			/* 打印服务器名称 */
			printfPQExpBuffer(&buf, _("Server: %s"),
							  PQgetvalue(result, 0, 0));
			printTableAddFooter(&cont, buf.data);

			/* 打印每个表的 FDW 选项（如果有） */
			ftoptions = PQgetvalue(result, 0, 1);
			if (ftoptions && ftoptions[0] != '\0')
			{
				printfPQExpBuffer(&buf, _("FDW options: (%s)"), ftoptions);
				printTableAddFooter(&cont, buf.data);
			}
			PQclear(result);
		}

		/* 打印继承来源的表（排除分区父表） */
		printfPQExpBuffer(&buf,
						  "SELECT c.oid::pg_catalog.regclass\n"
						  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
						  "WHERE c.oid = i.inhparent AND i.inhrelid = '%s'\n"
						  "  AND c.relkind != " CppAsString2(RELKIND_PARTITIONED_TABLE)
						  " AND c.relkind != " CppAsString2(RELKIND_PARTITIONED_INDEX)
						  "\nORDER BY inhseqno;",
						  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
		{
			const char *s = _("Inherits");
			int			sw = pg_wcswidth(s, strlen(s), pset.encoding);

			tuples = PQntuples(result);

			for (i = 0; i < tuples; i++)
			{
				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  s, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  sw, "", PQgetvalue(result, i, 0));
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}

			PQclear(result);
		}

		/* 打印子表（如果是分区则附带额外信息） */
		if (pset.sversion >= 140000)
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " inhdetachpending,"
							  " pg_catalog.pg_get_expr(c.relpartbound, c.oid)\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT',"
							  " c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);
		else if (pset.sversion >= 100000)
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " false AS inhdetachpending,"
							  " pg_catalog.pg_get_expr(c.relpartbound, c.oid)\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY pg_catalog.pg_get_expr(c.relpartbound, c.oid) = 'DEFAULT',"
							  " c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);
		else
			printfPQExpBuffer(&buf,
							  "SELECT c.oid::pg_catalog.regclass, c.relkind,"
							  " false AS inhdetachpending, NULL\n"
							  "FROM pg_catalog.pg_class c, pg_catalog.pg_inherits i\n"
							  "WHERE c.oid = i.inhrelid AND i.inhparent = '%s'\n"
							  "ORDER BY c.oid::pg_catalog.regclass::pg_catalog.text;",
							  oid);

		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		tuples = PQntuples(result);

		/*
		 * 对于没有分区的分区表，始终将分区数打印为零，
		 * 即使预期 verbose 输出时也是如此。否则对于
		 * 没有任何分区的分区表，我们将不会打印
		 * "Partitions" 部分。
		 */
		if (is_partitioned && tuples == 0)
		{
			printfPQExpBuffer(&buf, _("Number of partitions: %d"), tuples);
			printTableAddFooter(&cont, buf.data);
		}
		else if (!verbose)
		{
			/* 打印子表数量（如果有） */
			if (tuples > 0)
			{
				if (is_partitioned)
					printfPQExpBuffer(&buf, _("Number of partitions: %d (Use \\d+ to list them.)"), tuples);
				else
					printfPQExpBuffer(&buf, _("Number of child tables: %d (Use \\d+ to list them.)"), tuples);
				printTableAddFooter(&cont, buf.data);
			}
		}
		else
		{
			/* 显示子表列表 */
			const char *ct = is_partitioned ? _("Partitions") : _("Child tables");
			int			ctw = pg_wcswidth(ct, strlen(ct), pset.encoding);

			for (i = 0; i < tuples; i++)
			{
				char		child_relkind = *PQgetvalue(result, i, 1);

				if (i == 0)
					printfPQExpBuffer(&buf, "%s: %s",
									  ct, PQgetvalue(result, i, 0));
				else
					printfPQExpBuffer(&buf, "%*s  %s",
									  ctw, "", PQgetvalue(result, i, 0));
				if (!PQgetisnull(result, i, 3))
					appendPQExpBuffer(&buf, " %s", PQgetvalue(result, i, 3));
				if (child_relkind == RELKIND_PARTITIONED_TABLE ||
					child_relkind == RELKIND_PARTITIONED_INDEX)
					appendPQExpBufferStr(&buf, ", PARTITIONED");
				else if (child_relkind == RELKIND_FOREIGN_TABLE)
					appendPQExpBufferStr(&buf, ", FOREIGN");
				if (strcmp(PQgetvalue(result, i, 2), "t") == 0)
					appendPQExpBufferStr(&buf, " (DETACH PENDING)");
				if (i < tuples - 1)
					appendPQExpBufferChar(&buf, ',');

				printTableAddFooter(&cont, buf.data);
			}
		}
		PQclear(result);

		/* 表类型 */
		if (tableinfo.reloftype)
		{
			printfPQExpBuffer(&buf, _("Typed table of type: %s"), tableinfo.reloftype);
			printTableAddFooter(&cont, buf.data);
		}

		if (verbose &&
			(tableinfo.relkind == RELKIND_RELATION ||
			 tableinfo.relkind == RELKIND_MATVIEW) &&

		/*
		 * 无需显示默认值；我们已经在索引上显示
		 * REPLICA IDENTITY 标记。
		 */
			tableinfo.relreplident != 'i' &&
			((strcmp(schemaname, "pg_catalog") != 0 && tableinfo.relreplident != 'd') ||
			 (strcmp(schemaname, "pg_catalog") == 0 && tableinfo.relreplident != 'n')))
		{
			const char *s = _("Replica Identity");

			printfPQExpBuffer(&buf, "%s: %s",
							  s,
							  tableinfo.relreplident == 'f' ? "FULL" :
							  tableinfo.relreplident == 'n' ? "NOTHING" :
							  "???");

			printTableAddFooter(&cont, buf.data);
		}

		/* OID（如果 verbose 且不是物化视图） */
		if (verbose && tableinfo.relkind != RELKIND_MATVIEW && tableinfo.hasoids)
			printTableAddFooter(&cont, _("Has OIDs: yes"));

		/* 表空间信息 */
		add_tablespace_footer(&cont, tableinfo.relkind, tableinfo.tablespace,
							  true);

		/* 访问方法信息 */
		if (verbose && tableinfo.relam != NULL && !pset.hide_tableam)
		{
			printfPQExpBuffer(&buf, _("Access method: %s"), tableinfo.relam);
			printTableAddFooter(&cont, buf.data);
		}
	}

	/* reloptions（如果 verbose） */
	if (verbose &&
		tableinfo.reloptions && tableinfo.reloptions[0] != '\0')
	{
		const char *t = _("Options");

		printfPQExpBuffer(&buf, "%s: %s", t, tableinfo.reloptions);
		printTableAddFooter(&cont, buf.data);
	}

	printTable(&cont, pset.queryFout, false, pset.logfile);

	retval = true;

error_return:

	/* 清理 */
	if (printTableInitialized)
		printTableCleanup(&cont);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	termPQExpBuffer(&tmpbuf);

	free(view_def);

	PQclear(res);

	return retval;
}

/*
 * 向页脚添加表空间描述。如果 'newline' 为 true，则将其添加
 * 到新行中；否则将其附加到最后一个页脚的当前值。
 *
 */
static void
add_tablespace_footer(printTableContent *const cont, char relkind,
					  Oid tablespace, const bool newline)
{
	/* 我们支持表空间的 relkind */
	if (relkind == RELKIND_RELATION ||
		relkind == RELKIND_MATVIEW ||
		relkind == RELKIND_INDEX ||
		relkind == RELKIND_PARTITIONED_TABLE ||
		relkind == RELKIND_PARTITIONED_INDEX ||
		relkind == RELKIND_TOASTVALUE)
	{
		/*
		 * 我们忽略数据库默认表空间，以便不使用表空间
		 * 的用户无需了解它们。
		 */
		if (tablespace != 0)
		{
			PGresult   *result = NULL;
			PQExpBufferData buf;

			initPQExpBuffer(&buf);
			printfPQExpBuffer(&buf,
							  "SELECT spcname FROM pg_catalog.pg_tablespace\n"
							  "WHERE oid = '%u';", tablespace);
			result = PSQLexec(buf.data);
			if (!result)
			{
				termPQExpBuffer(&buf);
				return;
			}
			/* 应该始终是这种情况，但... */
			if (PQntuples(result) > 0)
			{
				if (newline)
				{
					/* 将表空间作为新页脚添加 */
					printfPQExpBuffer(&buf, _("Tablespace: \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableAddFooter(cont, buf.data);
				}
				else
				{
					/* 将表空间附加到最新的页脚 */
					printfPQExpBuffer(&buf, "%s", cont->footer->data);

					/*-------
					   translator: before this string there's an index description like
					   '"foo_pkey" PRIMARY KEY, btree (a)' */
					appendPQExpBuffer(&buf, _(", tablespace \"%s\""),
									  PQgetvalue(result, 0, 0));
					printTableSetFooter(cont, buf.data);
				}
			}
			PQclear(result);
			termPQExpBuffer(&buf);
		}
	}
}

/*
 * \du 或 \dg
 *
 * 描述角色。模式中的任何模式部分都将被忽略。
 */
bool
describeRoles(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printTableContent cont;
	printTableOpt myopt = pset.popt.topt;
	int			ncols = 2;
	int			nrows = 0;
	int			i;
	int			conns;
	const char	align = 'l';
	char	  **attr;

	myopt.default_footer = false;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT r.rolname, r.rolsuper, r.rolinherit,\n"
					  "  r.rolcreaterole, r.rolcreatedb, r.rolcanlogin,\n"
					  "  r.rolconnlimit, r.rolvaliduntil");

	if (verbose)
	{
		appendPQExpBufferStr(&buf, "\n, pg_catalog.shobj_description(r.oid, 'pg_authid') AS description");
		ncols++;
	}
	appendPQExpBufferStr(&buf, "\n, r.rolreplication");

	if (pset.sversion >= 90500)
	{
		appendPQExpBufferStr(&buf, "\n, r.rolbypassrls");
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_roles r\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE r.rolname !~ '^pg_'\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "r.rolname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	if (!res)
		return false;

	nrows = PQntuples(res);
	attr = pg_malloc0((nrows + 1) * sizeof(*attr));

	printTableInit(&cont, &myopt, _("List of roles"), ncols, nrows);

	printTableAddHeader(&cont, gettext_noop("Role name"), true, align);
	printTableAddHeader(&cont, gettext_noop("Attributes"), true, align);

	if (verbose)
		printTableAddHeader(&cont, gettext_noop("Description"), true, align);

	for (i = 0; i < nrows; i++)
	{
		printTableAddCell(&cont, PQgetvalue(res, i, 0), false, false);

		resetPQExpBuffer(&buf);
		if (strcmp(PQgetvalue(res, i, 1), "t") == 0)
			add_role_attribute(&buf, _("Superuser"));

		if (strcmp(PQgetvalue(res, i, 2), "t") != 0)
			add_role_attribute(&buf, _("No inheritance"));

		if (strcmp(PQgetvalue(res, i, 3), "t") == 0)
			add_role_attribute(&buf, _("Create role"));

		if (strcmp(PQgetvalue(res, i, 4), "t") == 0)
			add_role_attribute(&buf, _("Create DB"));

		if (strcmp(PQgetvalue(res, i, 5), "t") != 0)
			add_role_attribute(&buf, _("Cannot login"));

		if (strcmp(PQgetvalue(res, i, (verbose ? 9 : 8)), "t") == 0)
			add_role_attribute(&buf, _("Replication"));

		if (pset.sversion >= 90500)
			if (strcmp(PQgetvalue(res, i, (verbose ? 10 : 9)), "t") == 0)
				add_role_attribute(&buf, _("Bypass RLS"));

		conns = atoi(PQgetvalue(res, i, 6));
		if (conns >= 0)
		{
			if (buf.len > 0)
				appendPQExpBufferChar(&buf, '\n');

			if (conns == 0)
				appendPQExpBufferStr(&buf, _("No connections"));
			else
				appendPQExpBuffer(&buf, ngettext("%d connection",
												 "%d connections",
												 conns),
								  conns);
		}

		if (strcmp(PQgetvalue(res, i, 7), "") != 0)
		{
			if (buf.len > 0)
				appendPQExpBufferChar(&buf, '\n');
			appendPQExpBufferStr(&buf, _("Password valid until "));
			appendPQExpBufferStr(&buf, PQgetvalue(res, i, 7));
		}

		attr[i] = pg_strdup(buf.data);

		printTableAddCell(&cont, attr[i], false, false);

		if (verbose)
			printTableAddCell(&cont, PQgetvalue(res, i, 8), false, false);
	}
	termPQExpBuffer(&buf);

	printTable(&cont, pset.queryFout, false, pset.logfile);
	printTableCleanup(&cont);

	for (i = 0; i < nrows; i++)
		free(attr[i]);
	free(attr);

	PQclear(res);
	return true;
}

static void
add_role_attribute(PQExpBuffer buf, const char *const str)
{
	if (buf->len > 0)
		appendPQExpBufferStr(buf, ", ");

	appendPQExpBufferStr(buf, str);
}

/*
 * \drds
 */
bool
listDbRoleSettings(const char *pattern, const char *pattern2)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		havewhere;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "SELECT rolname AS \"%s\", datname AS \"%s\",\n"
					  "pg_catalog.array_to_string(setconfig, E'\\n') AS \"%s\"\n"
					  "FROM pg_catalog.pg_db_role_setting s\n"
					  "LEFT JOIN pg_catalog.pg_database d ON d.oid = setdatabase\n"
					  "LEFT JOIN pg_catalog.pg_roles r ON r.oid = setrole\n",
					  gettext_noop("Role"),
					  gettext_noop("Database"),
					  gettext_noop("Settings"));
	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "r.rolname", NULL, NULL, &havewhere, 1))
		goto error_return;
	if (!validateSQLNamePattern(&buf, pattern2, havewhere, false,
								NULL, "d.datname", NULL, NULL,
								NULL, 1))
		goto error_return;
	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	/*
	 * 本文件中的大多数函数在没有匹配对象时满足于
	 * 打印空表。我们有意在这里偏离这一做法，
	 * 但仅在非静默模式下，因为用户可能对两个模式
	 * 参数的含义感到困惑。
	 */
	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern && pattern2)
			pg_log_error("Did not find any settings for role \"%s\" and database \"%s\".",
						 pattern, pattern2);
		else if (pattern)
			pg_log_error("Did not find any settings for role \"%s\".",
						 pattern);
		else
			pg_log_error("Did not find any settings.");
	}
	else
	{
		myopt.nullPrint = NULL;
		myopt.title = _("List of settings");
		myopt.translate_header = true;

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);
	}

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \drg
 * 描述角色授权。
 */
bool
describeRoleGrants(const char *pattern, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT m.rolname AS \"%s\", r.rolname AS \"%s\",\n"
					  "  pg_catalog.concat_ws(', ',\n",
					  gettext_noop("Role name"),
					  gettext_noop("Member of"));

	if (pset.sversion >= 160000)
		appendPQExpBufferStr(&buf,
							 "    CASE WHEN pam.admin_option THEN 'ADMIN' END,\n"
							 "    CASE WHEN pam.inherit_option THEN 'INHERIT' END,\n"
							 "    CASE WHEN pam.set_option THEN 'SET' END\n");
	else
		appendPQExpBufferStr(&buf,
							 "    CASE WHEN pam.admin_option THEN 'ADMIN' END,\n"
							 "    CASE WHEN m.rolinherit THEN 'INHERIT' END,\n"
							 "    'SET'\n");

	appendPQExpBuffer(&buf,
					  "  ) AS \"%s\",\n"
					  "  g.rolname AS \"%s\"\n",
					  gettext_noop("Options"),
					  gettext_noop("Grantor"));

	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_roles m\n"
						 "     JOIN pg_catalog.pg_auth_members pam ON (pam.member = m.oid)\n"
						 "     LEFT JOIN pg_catalog.pg_roles r ON (pam.roleid = r.oid)\n"
						 "     LEFT JOIN pg_catalog.pg_roles g ON (pam.grantor = g.oid)\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE m.rolname !~ '^pg_'\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "m.rolname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;\n");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of role grants");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * listTables()
 *
 * \dt、\di 等的处理程序。
 *
 * tabtypes 是一个字符数组，指定所需的信息：
 * t - 表
 * i - 索引
 * v - 视图
 * m - 物化视图
 * s - 序列
 * E - 外部表（注意：与 'f' 不同，后者是 relkind 值）
 * （以上任意顺序均可）  
 */
bool
listTables(const char *tabtypes, const char *pattern, bool verbose, bool showSystem)
{
	bool		showTables = strchr(tabtypes, 't') != NULL;
	bool		showIndexes = strchr(tabtypes, 'i') != NULL;
	bool		showViews = strchr(tabtypes, 'v') != NULL;
	bool		showMatViews = strchr(tabtypes, 'm') != NULL;
	bool		showSeq = strchr(tabtypes, 's') != NULL;
	bool		showForeign = strchr(tabtypes, 'E') != NULL;

	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	int			cols_so_far;
	bool		translate_columns[] = {false, false, true, false, false, false, false, false, false};

	/* 如果 tabtypes 为空，默认为 \dtvmsE（另请参阅 command.c） */
	if (!(showTables || showIndexes || showViews || showMatViews || showSeq || showForeign))
		showTables = showViews = showMatViews = showSeq = showForeign = true;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  CASE c.relkind"
					  " WHEN " CppAsString2(RELKIND_RELATION) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_VIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_MATVIEW) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_INDEX) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_SEQUENCE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_TOASTVALUE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
					  " WHEN " CppAsString2(RELKIND_PARTITIONED_INDEX) " THEN '%s'"
					  " END as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(c.relowner) as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("table"),
					  gettext_noop("view"),
					  gettext_noop("materialized view"),
					  gettext_noop("index"),
					  gettext_noop("sequence"),
					  gettext_noop("TOAST table"),
					  gettext_noop("foreign table"),
					  gettext_noop("partitioned table"),
					  gettext_noop("partitioned index"),
					  gettext_noop("Type"),
					  gettext_noop("Owner"));
	cols_so_far = 4;

	if (showIndexes)
	{
		appendPQExpBuffer(&buf,
						  ",\n  c2.relname as \"%s\"",
						  gettext_noop("Table"));
		cols_so_far++;
	}

	if (verbose)
	{
		/*
		 * 显示关系是永久的、临时的还是未日志的。
		 */
		appendPQExpBuffer(&buf,
						  ",\n  CASE c.relpersistence WHEN 'p' THEN '%s' WHEN 't' THEN '%s' WHEN 'u' THEN '%s' END as \"%s\"",
						  gettext_noop("permanent"),
						  gettext_noop("temporary"),
						  gettext_noop("unlogged"),
						  gettext_noop("Persistence"));
		translate_columns[cols_so_far] = true;

		/*
		 * 我们在这里下面不费心计数 cols_so_far，因为没有
		 * 这个必要；随着未来输出列的添加，这一点可能会改变。
		 */

		/*
		 * 表、物化视图和索引可以使用访问方法。
		 * 这在 PostgreSQL 12 中为表引入。
		 */
		if (pset.sversion >= 120000 && !pset.hide_tableam &&
			(showTables || showMatViews || showIndexes))
			appendPQExpBuffer(&buf,
							  ",\n  am.amname as \"%s\"",
							  gettext_noop("Access method"));

		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.pg_size_pretty(pg_catalog.pg_table_size(c.oid)) as \"%s\""
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  gettext_noop("Size"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_class c"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace");

	if (pset.sversion >= 120000 && !pset.hide_tableam &&
		(showTables || showMatViews || showIndexes))
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_am am ON am.oid = c.relam");

	if (showIndexes)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
							 "\n     LEFT JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid");

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
	{
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_RELATION) ","
							 CppAsString2(RELKIND_PARTITIONED_TABLE) ",");
		/* 使用 'S' 或模式时，允许 't' 也匹配 TOAST 表 */
		if (showSystem || pattern)
			appendPQExpBufferStr(&buf, CppAsString2(RELKIND_TOASTVALUE) ",");
	}
	if (showViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_VIEW) ",");
	if (showMatViews)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_MATVIEW) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_INDEX) ","
							 CppAsString2(RELKIND_PARTITIONED_INDEX) ",");
	if (showSeq)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_SEQUENCE) ",");
	if (showSystem || pattern)
		appendPQExpBufferStr(&buf, "'s',"); /* 曾是 RELKIND_SPECIAL */
	if (showForeign)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_FOREIGN_TABLE) ",");

	appendPQExpBufferStr(&buf, "''");	/* 占位 */
	appendPQExpBufferStr(&buf, ")\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname !~ '^pg_toast'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1,2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	/*
	 * 本文件中的大多数函数在没有匹配对象时满足于
	 * 打印空表。出于历史原因，我们有意在这里
	 * 偏离这一做法，但仅在非静默模式下。
	 */
	if (PQntuples(res) == 0 && !pset.quiet)
	{
		if (pattern)
			pg_log_error("Did not find any relation named \"%s\".",
						 pattern);
		else
			pg_log_error("Did not find any relations.");
	}
	else
	{
		myopt.nullPrint = NULL;
		myopt.title = _("List of relations");
		myopt.translate_header = true;
		myopt.translate_columns = translate_columns;
		myopt.n_translate_columns = lengthof(translate_columns);

		printQuery(res, &myopt, pset.queryFout, false, pset.logfile);
	}

	PQclear(res);
	return true;
}

/*
 * \dP
 * 接受一个可选的正则表达式来选择特定的关系
 *
 * 与 \d 类似，你可以指定所需的关系类型：
 *
 * t 表示表
 * i 表示索引
 *
 * 还有额外的标志：
 *
 * n 表示列出非叶子分区表
 *
 * 并且你可以按任意顺序混合搭配这些选项。
 */
bool
listPartitionedTables(const char *reltypes, const char *pattern, bool verbose)
{
	bool		showTables = strchr(reltypes, 't') != NULL;
	bool		showIndexes = strchr(reltypes, 'i') != NULL;
	bool		showNested = strchr(reltypes, 'n') != NULL;
	PQExpBufferData buf;
	PQExpBufferData title;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		translate_columns[] = {false, false, false, false, false, false, false, false, false};
	const char *tabletitle;
	bool		mixed_output = false;

	/*
	 * 注意：声明式表分区仅在 Pg 10.0 及以上版本支持。
	 */
	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support declarative table partitioning.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	/* 如果未选择关系类型，则全部显示 */
	if (!showTables && !showIndexes)
		showTables = showIndexes = true;

	if (showIndexes && !showTables)
		tabletitle = _("List of partitioned indexes");	/* \dPi */
	else if (showTables && !showIndexes)
		tabletitle = _("List of partitioned tables");	/* \dPt */
	else
	{
		/* 显示所有种类 */
		tabletitle = _("List of partitioned relations");
		mixed_output = true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "  c.relname as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(c.relowner) as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Owner"));

	if (mixed_output)
	{
		appendPQExpBuffer(&buf,
						  ",\n  CASE c.relkind"
						  " WHEN " CppAsString2(RELKIND_PARTITIONED_TABLE) " THEN '%s'"
						  " WHEN " CppAsString2(RELKIND_PARTITIONED_INDEX) " THEN '%s'"
						  " END as \"%s\"",
						  gettext_noop("partitioned table"),
						  gettext_noop("partitioned index"),
						  gettext_noop("Type"));

		translate_columns[3] = true;
	}

	if (showNested || pattern)
		appendPQExpBuffer(&buf,
						  ",\n  inh.inhparent::pg_catalog.regclass as \"%s\"",
						  gettext_noop("Parent name"));

	if (showIndexes)
		appendPQExpBuffer(&buf,
						  ",\n c2.oid::pg_catalog.regclass as \"%s\"",
						  gettext_noop("Table"));

	if (verbose)
	{
		if (showNested)
		{
			appendPQExpBuffer(&buf,
							  ",\n  s.dps as \"%s\"",
							  gettext_noop("Leaf partition size"));
			appendPQExpBuffer(&buf,
							  ",\n  s.tps as \"%s\"",
							  gettext_noop("Total size"));
		}
		else
			/* 在这种情况下考虑所有分区的大小。 */
			appendPQExpBuffer(&buf,
							  ",\n  s.tps as \"%s\"",
							  gettext_noop("Total size"));

		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_class') as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_class c"
						 "\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace");

	if (showIndexes)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid"
							 "\n     LEFT JOIN pg_catalog.pg_class c2 ON i.indrelid = c2.oid");

	if (showNested || pattern)
		appendPQExpBufferStr(&buf,
							 "\n     LEFT JOIN pg_catalog.pg_inherits inh ON c.oid = inh.inhrelid");

	if (verbose)
	{
		if (pset.sversion < 120000)
		{
			appendPQExpBufferStr(&buf,
								 ",\n     LATERAL (WITH RECURSIVE d\n"
								 "                AS (SELECT inhrelid AS oid, 1 AS level\n"
								 "                      FROM pg_catalog.pg_inherits\n"
								 "                     WHERE inhparent = c.oid\n"
								 "                    UNION ALL\n"
								 "                    SELECT inhrelid, level + 1\n"
								 "                      FROM pg_catalog.pg_inherits i\n"
								 "                           JOIN d ON i.inhparent = d.oid)\n"
								 "                SELECT pg_catalog.pg_size_pretty(sum(pg_catalog.pg_table_size("
								 "d.oid))) AS tps,\n"
								 "                       pg_catalog.pg_size_pretty(sum("
								 "\n             CASE WHEN d.level = 1"
								 " THEN pg_catalog.pg_table_size(d.oid) ELSE 0 END)) AS dps\n"
								 "               FROM d) s");
		}
		else
		{
			/* PostgreSQL 12 有 pg_partition_tree 函数 */
			appendPQExpBufferStr(&buf,
								 ",\n     LATERAL (SELECT pg_catalog.pg_size_pretty(sum("
								 "\n                 CASE WHEN ppt.isleaf AND ppt.level = 1"
								 "\n                      THEN pg_catalog.pg_table_size(ppt.relid)"
								 " ELSE 0 END)) AS dps"
								 ",\n                     pg_catalog.pg_size_pretty(sum("
								 "pg_catalog.pg_table_size(ppt.relid))) AS tps"
								 "\n              FROM pg_catalog.pg_partition_tree(c.oid) ppt) s");
		}
	}

	appendPQExpBufferStr(&buf, "\nWHERE c.relkind IN (");
	if (showTables)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_PARTITIONED_TABLE) ",");
	if (showIndexes)
		appendPQExpBufferStr(&buf, CppAsString2(RELKIND_PARTITIONED_INDEX) ",");
	appendPQExpBufferStr(&buf, "''");	/* 占位 */
	appendPQExpBufferStr(&buf, ")\n");

	appendPQExpBufferStr(&buf, !showNested && !pattern ?
						 " AND NOT c.relispartition\n" : "");

	if (!pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname !~ '^pg_toast'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBuffer(&buf, "ORDER BY \"Schema\", %s%s\"Name\";",
					  mixed_output ? "\"Type\" DESC, " : "",
					  showNested || pattern ? "\"Parent name\" NULLS FIRST, " : "");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);
	appendPQExpBufferStr(&title, tabletitle);

	myopt.nullPrint = NULL;
	myopt.title = title.data;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);

	PQclear(res);
	return true;
}

/*
 * \dL
 *
 * 描述语言。
 */
bool
listLanguages(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT l.lanname AS \"%s\",\n"
					  "       pg_catalog.pg_get_userbyid(l.lanowner) as \"%s\",\n"
					  "       l.lanpltrusted AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Trusted"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ",\n       NOT l.lanispl AS \"%s\",\n"
						  "       l.lanplcallfoid::pg_catalog.regprocedure AS \"%s\",\n"
						  "       l.lanvalidator::pg_catalog.regprocedure AS \"%s\",\n       "
						  "l.laninline::pg_catalog.regprocedure AS \"%s\",\n       ",
						  gettext_noop("Internal language"),
						  gettext_noop("Call handler"),
						  gettext_noop("Validator"),
						  gettext_noop("Inline handler"));
		printACLColumn(&buf, "l.lanacl");
	}

	appendPQExpBuffer(&buf,
					  ",\n       d.description AS \"%s\""
					  "\nFROM pg_catalog.pg_language l\n"
					  "LEFT JOIN pg_catalog.pg_description d\n"
					  "  ON d.classoid = l.tableoid AND d.objoid = l.oid\n"
					  "  AND d.objsubid = 0\n",
					  gettext_noop("Description"));

	if (pattern)
	{
		if (!validateSQLNamePattern(&buf, pattern, false, false,
									NULL, "l.lanname", NULL, NULL,
									NULL, 2))
		{
			termPQExpBuffer(&buf);
			return false;
		}
	}

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "WHERE l.lanplcallfoid != 0\n");


	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of languages");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dD
 *
 * 描述域。
 */
bool
listDomains(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname as \"%s\",\n"
					  "       t.typname as \"%s\",\n"
					  "       pg_catalog.format_type(t.typbasetype, t.typtypmod) as \"%s\",\n"
					  "       (SELECT c.collname FROM pg_catalog.pg_collation c, pg_catalog.pg_type bt\n"
					  "        WHERE c.oid = t.typcollation AND bt.oid = t.typbasetype AND t.typcollation <> bt.typcollation) as \"%s\",\n"
					  "       CASE WHEN t.typnotnull THEN 'not null' END as \"%s\",\n"
					  "       t.typdefault as \"%s\",\n"
					  "       pg_catalog.array_to_string(ARRAY(\n"
					  "         SELECT pg_catalog.pg_get_constraintdef(r.oid, true) FROM pg_catalog.pg_constraint r WHERE t.oid = r.contypid\n"
					  "       ), ' ') as \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Type"),
					  gettext_noop("Collation"),
					  gettext_noop("Nullable"),
					  gettext_noop("Default"),
					  gettext_noop("Check"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "t.typacl");
		appendPQExpBuffer(&buf,
						  ",\n       d.description as \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_type t\n"
						 "     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.typnamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_description d "
							 "ON d.classoid = t.tableoid AND d.objoid = t.oid "
							 "AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE t.typtype = 'd'\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "t.typname", NULL,
								"pg_catalog.pg_type_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of domains");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dc
 *
 * 描述转换。
 */
bool
listConversions(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] =
	{false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "       c.conname AS \"%s\",\n"
					  "       pg_catalog.pg_encoding_to_char(c.conforencoding) AS \"%s\",\n"
					  "       pg_catalog.pg_encoding_to_char(c.contoencoding) AS \"%s\",\n"
					  "       CASE WHEN c.condefault THEN '%s'\n"
					  "       ELSE '%s' END AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Source"),
					  gettext_noop("Destination"),
					  gettext_noop("yes"), gettext_noop("no"),
					  gettext_noop("Default?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       d.description AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_conversion c\n"
						 "     JOIN pg_catalog.pg_namespace n "
						 "ON n.oid = c.connamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d "
							 "ON d.classoid = c.tableoid\n"
							 "          AND d.objoid = c.oid "
							 "AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE true\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "  AND n.nspname <> 'pg_catalog'\n"
							 "  AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.conname", NULL,
								"pg_catalog.pg_conversion_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of conversions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dconfig
 *
 * 描述配置参数。
 */
bool
describeConfigurationParameters(const char *pattern, bool verbose,
								bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT s.name AS \"%s\", "
					  "pg_catalog.current_setting(s.name) AS \"%s\"",
					  gettext_noop("Parameter"),
					  gettext_noop("Value"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  ", s.vartype AS \"%s\", s.context AS \"%s\", ",
						  gettext_noop("Type"),
						  gettext_noop("Context"));
		if (pset.sversion >= 150000)
			printACLColumn(&buf, "p.paracl");
		else
			appendPQExpBuffer(&buf, "NULL AS \"%s\"",
							  gettext_noop("Access privileges"));
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_settings s\n");

	if (verbose && pset.sversion >= 150000)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_parameter_acl p\n"
							 "  ON pg_catalog.lower(s.name) = p.parname\n");

	if (pattern)
		processSQLNamePattern(pset.db, &buf, pattern,
							  false, false,
							  NULL, "pg_catalog.lower(s.name)", NULL,
							  NULL, NULL, NULL);
	else
		appendPQExpBufferStr(&buf, "WHERE s.source <> 'default' AND\n"
							 "      s.setting IS DISTINCT FROM s.boot_val\n");

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	if (pattern)
		myopt.title = _("List of configuration parameters");
	else
		myopt.title = _("List of non-default configuration parameters");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dy
 *
 * 描述事件触发器。
 */
bool
listEventTriggers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] =
	{false, false, false, true, false, false, false};

	if (pset.sversion < 90300)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support event triggers.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT evtname as \"%s\", "
					  "evtevent as \"%s\", "
					  "pg_catalog.pg_get_userbyid(e.evtowner) as \"%s\",\n"
					  " case evtenabled when 'O' then '%s'"
					  "  when 'R' then '%s'"
					  "  when 'A' then '%s'"
					  "  when 'D' then '%s' end as \"%s\",\n"
					  " e.evtfoid::pg_catalog.regproc as \"%s\", "
					  "pg_catalog.array_to_string(array(select x"
					  " from pg_catalog.unnest(evttags) as t(x)), ', ') as \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Event"),
					  gettext_noop("Owner"),
					  gettext_noop("enabled"),
					  gettext_noop("replica"),
					  gettext_noop("always"),
					  gettext_noop("disabled"),
					  gettext_noop("Enabled"),
					  gettext_noop("Function"),
					  gettext_noop("Tags"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\npg_catalog.obj_description(e.oid, 'pg_event_trigger') as \"%s\"",
						  gettext_noop("Description"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_event_trigger e ");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "evtname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of event triggers");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dX
 *
 * 描述扩展统计信息。
 */
bool
listExtendedStats(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support extended statistics.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT \n"
					  "es.stxnamespace::pg_catalog.regnamespace::pg_catalog.text AS \"%s\", \n"
					  "es.stxname AS \"%s\", \n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 140000)
		appendPQExpBuffer(&buf,
						  "pg_catalog.format('%%s FROM %%s', \n"
						  "  pg_catalog.pg_get_statisticsobjdef_columns(es.oid), \n"
						  "  es.stxrelid::pg_catalog.regclass) AS \"%s\"",
						  gettext_noop("Definition"));
	else
		appendPQExpBuffer(&buf,
						  "pg_catalog.format('%%s FROM %%s', \n"
						  "  (SELECT pg_catalog.string_agg(pg_catalog.quote_ident(a.attname),', ') \n"
						  "   FROM pg_catalog.unnest(es.stxkeys) s(attnum) \n"
						  "   JOIN pg_catalog.pg_attribute a \n"
						  "   ON (es.stxrelid = a.attrelid \n"
						  "   AND a.attnum = s.attnum \n"
						  "   AND NOT a.attisdropped)), \n"
						  "es.stxrelid::pg_catalog.regclass) AS \"%s\"",
						  gettext_noop("Definition"));

	appendPQExpBuffer(&buf,
					  ",\nCASE WHEN 'd' = any(es.stxkind) THEN 'defined' \n"
					  "END AS \"%s\", \n"
					  "CASE WHEN 'f' = any(es.stxkind) THEN 'defined' \n"
					  "END AS \"%s\"",
					  gettext_noop("Ndistinct"),
					  gettext_noop("Dependencies"));

	/*
	 * 包含 MCV 统计信息种类。
	 */
	if (pset.sversion >= 120000)
	{
		appendPQExpBuffer(&buf,
						  ",\nCASE WHEN 'm' = any(es.stxkind) THEN 'defined' \n"
						  "END AS \"%s\" ",
						  gettext_noop("MCV"));
	}

	appendPQExpBufferStr(&buf,
						 " \nFROM pg_catalog.pg_statistic_ext es \n");

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								"es.stxnamespace::pg_catalog.regnamespace::pg_catalog.text", "es.stxname",
								NULL, "pg_catalog.pg_statistics_obj_is_visible(es.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of extended statistics");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dC
 *
 * 描述类型转换。
 */
bool
listCasts(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.format_type(castsource, NULL) AS \"%s\",\n"
					  "       pg_catalog.format_type(casttarget, NULL) AS \"%s\",\n",
					  gettext_noop("Source type"),
					  gettext_noop("Target type"));

	/*
	 * 我们不尝试本地化 '(binary coercible)' 或 '(with inout)'，
	 * 因为 gettext 可能恰好将碰巧匹配 PO 数据库中某些字符串的
	 * 函数名翻译出来，风险太大。
	 */
	appendPQExpBuffer(&buf,
					  "       CASE WHEN c.castmethod = '%c' THEN '(binary coercible)'\n"
					  "            WHEN c.castmethod = '%c' THEN '(with inout)'\n"
					  "            ELSE p.proname\n"
					  "       END AS \"%s\",\n",
					  COERCION_METHOD_BINARY,
					  COERCION_METHOD_INOUT,
					  gettext_noop("Function"));

	appendPQExpBuffer(&buf,
					  "       CASE WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            WHEN c.castcontext = '%c' THEN '%s'\n"
					  "            ELSE '%s'\n"
					  "       END AS \"%s\"",
					  COERCION_CODE_EXPLICIT,
					  gettext_noop("no"),
					  COERCION_CODE_ASSIGNMENT,
					  gettext_noop("in assignment"),
					  gettext_noop("yes"),
					  gettext_noop("Implicit?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n       d.description AS \"%s\"",
						  gettext_noop("Description"));

	/*
	 * 对于二进制类型转换，我们需要左连接 pg_proc；
	 * 其他的只是为了多疑一点。
	 */
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_cast c LEFT JOIN pg_catalog.pg_proc p\n"
						 "     ON c.castfunc = p.oid\n"
						 "     LEFT JOIN pg_catalog.pg_type ts\n"
						 "     ON c.castsource = ts.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace ns\n"
						 "     ON ns.oid = ts.typnamespace\n"
						 "     LEFT JOIN pg_catalog.pg_type tt\n"
						 "     ON c.casttarget = tt.oid\n"
						 "     LEFT JOIN pg_catalog.pg_namespace nt\n"
						 "     ON nt.oid = tt.typnamespace\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "     LEFT JOIN pg_catalog.pg_description d\n"
							 "     ON d.classoid = c.tableoid AND d.objoid = "
							 "c.oid AND d.objsubid = 0\n");

	appendPQExpBufferStr(&buf, "WHERE ( (true");

	/*
	 * 将名称模式匹配到 castsource 或 casttarget
	 * 的内部或外部名称
	 */
	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"ns.nspname", "ts.typname",
								"pg_catalog.format_type(ts.oid, NULL)",
								"pg_catalog.pg_type_is_visible(ts.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, ") OR (true");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"nt.nspname", "tt.typname",
								"pg_catalog.format_type(tt.oid, NULL)",
								"pg_catalog.pg_type_is_visible(tt.oid)",
								NULL, 3))
		goto error_return;

	appendPQExpBufferStr(&buf, ") )\nORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of casts");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dO
 *
 * 描述排序规则。
 */
bool
listCollations(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, false, false, true, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname AS \"%s\",\n"
					  "  c.collname AS \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (pset.sversion >= 100000)
		appendPQExpBuffer(&buf,
						  "  CASE c.collprovider WHEN 'd' THEN 'default' WHEN 'c' THEN 'libc' WHEN 'i' THEN 'icu' END AS \"%s\",\n",
						  gettext_noop("Provider"));
	else
		appendPQExpBuffer(&buf,
						  "  'libc' AS \"%s\",\n",
						  gettext_noop("Provider"));

	appendPQExpBuffer(&buf,
					  "  c.collcollate AS \"%s\",\n"
					  "  c.collctype AS \"%s\",\n",
					  gettext_noop("Collate"),
					  gettext_noop("Ctype"));

	if (pset.sversion >= 150000)
		appendPQExpBuffer(&buf,
						  "  c.colliculocale AS \"%s\",\n",
						  gettext_noop("ICU Locale"));
	else
		appendPQExpBuffer(&buf,
						  "  c.collcollate AS \"%s\",\n",
						  gettext_noop("ICU Locale"));

	if (pset.sversion >= 160000)
		appendPQExpBuffer(&buf,
						  "  c.collicurules AS \"%s\",\n",
						  gettext_noop("ICU Rules"));
	else
		appendPQExpBuffer(&buf,
						  "  NULL AS \"%s\",\n",
						  gettext_noop("ICU Rules"));

	if (pset.sversion >= 120000)
		appendPQExpBuffer(&buf,
						  "  CASE WHEN c.collisdeterministic THEN '%s' ELSE '%s' END AS \"%s\"",
						  gettext_noop("yes"), gettext_noop("no"),
						  gettext_noop("Deterministic?"));
	else
		appendPQExpBuffer(&buf,
						  "  '%s' AS \"%s\"",
						  gettext_noop("yes"),
						  gettext_noop("Deterministic?"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(c.oid, 'pg_collation') AS \"%s\"",
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_collation c, pg_catalog.pg_namespace n\n"
						 "WHERE n.oid = c.collnamespace\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf, "      AND n.nspname <> 'pg_catalog'\n"
							 "      AND n.nspname <> 'information_schema'\n");

	/*
	 * 隐藏当前数据库编码中不可用的排序规则。
	 * 如果你想更改此行为，请注意 pg_collation_is_visible 会拒绝
	 * 不可用的排序规则，因此你需要某种方式修改名称模式处理
	 * 以避免不一致的行为。
	 */
	appendPQExpBufferStr(&buf, "      AND c.collencoding IN (-1, pg_catalog.pg_char_to_encoding(pg_catalog.getdatabaseencoding()))\n");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.collname", NULL,
								"pg_catalog.pg_collation_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of collations");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dn
 *
 * 描述模式（命名空间）
 */
bool
listSchemas(const char *pattern, bool verbose, bool showSystem)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	int			pub_schema_tuples = 0;
	char	  **footers = NULL;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(n.nspowner) AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "n.nspacl");
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.obj_description(n.oid, 'pg_namespace') AS \"%s\"",
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_namespace n\n");

	if (!showSystem && !pattern)
		appendPQExpBufferStr(&buf,
							 "WHERE n.nspname !~ '^pg_' AND n.nspname <> 'information_schema'\n");

	if (!validateSQLNamePattern(&buf, pattern,
								!showSystem && !pattern, false,
								NULL, "n.nspname", NULL,
								NULL,
								NULL, 2))
		goto error_return;

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	if (!res)
		goto error_return;

	myopt.nullPrint = NULL;
	myopt.title = _("List of schemas");
	myopt.translate_header = true;

	if (pattern && pset.sversion >= 150000)
	{
		PGresult   *result;
		int			i;

		printfPQExpBuffer(&buf,
						  "SELECT pubname \n"
						  "FROM pg_catalog.pg_publication p\n"
						  "     JOIN pg_catalog.pg_publication_namespace pn ON p.oid = pn.pnpubid\n"
						  "     JOIN pg_catalog.pg_namespace n ON n.oid = pn.pnnspid \n"
						  "WHERE n.nspname = '%s'\n"
						  "ORDER BY 1",
						  pattern);
		result = PSQLexec(buf.data);
		if (!result)
			goto error_return;
		else
			pub_schema_tuples = PQntuples(result);

		if (pub_schema_tuples > 0)
		{
			/*
			 * 为页脚分配内存。页脚大小为 1（用于存储
			 * "Publications:" 字符串）+ 发布与模式的映射
			 * 数量 + 1（用于存储 NULL）。
			 */
			footers = (char **) pg_malloc((1 + pub_schema_tuples + 1) * sizeof(char *));
			footers[0] = pg_strdup(_("Publications:"));

			/* 可能是空集合 - 没问题 */
			for (i = 0; i < pub_schema_tuples; i++)
			{
				printfPQExpBuffer(&buf, "    \"%s\"",
								  PQgetvalue(result, i, 0));

				footers[i + 1] = pg_strdup(buf.data);
			}

			footers[i + 1] = NULL;
			myopt.footers = footers;
		}

		PQclear(result);
	}

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&buf);
	PQclear(res);

	/* 释放为页脚分配的内存 */
	if (footers)
	{
		char	  **footer = NULL;

		for (footer = footers; *footer; footer++)
			pg_free(*footer);

		pg_free(footers);
	}

	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}


/*
 * \dFp
 * 列出文本搜索解析器
 */
bool
listTSParsers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSParsersVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  p.prsname as \"%s\",\n"
					  "  pg_catalog.obj_description(p.oid, 'pg_ts_parser') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search parsers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * 解析器的完整描述
 */
static bool
listTSParsersVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT p.oid,\n"
					  "  n.nspname,\n"
					  "  p.prsname\n"
					  "FROM pg_catalog.pg_ts_parser p\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.prsnamespace\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "p.prsname", NULL,
								"pg_catalog.pg_ts_parser_is_visible(p.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search parser named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search parsers.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *nspname = NULL;
		const char *prsname;

		oid = PQgetvalue(res, i, 0);
		if (!PQgetisnull(res, i, 1))
			nspname = PQgetvalue(res, i, 1);
		prsname = PQgetvalue(res, i, 2);

		if (!describeOneTSParser(oid, nspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSParser(const char *oid, const char *nspname, const char *prsname)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {true, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT '%s' AS \"%s\",\n"
					  "   p.prsstart::pg_catalog.regproc AS \"%s\",\n"
					  "   pg_catalog.obj_description(p.prsstart, 'pg_proc') as \"%s\"\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prstoken::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prstoken, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsend::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsend, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prsheadline::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prsheadline, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s'\n"
					  "UNION ALL\n"
					  "SELECT '%s',\n"
					  "   p.prslextype::pg_catalog.regproc,\n"
					  "   pg_catalog.obj_description(p.prslextype, 'pg_proc')\n"
					  " FROM pg_catalog.pg_ts_parser p\n"
					  " WHERE p.oid = '%s';",
					  gettext_noop("Start parse"),
					  gettext_noop("Method"),
					  gettext_noop("Function"),
					  gettext_noop("Description"),
					  oid,
					  gettext_noop("Get next token"),
					  oid,
					  gettext_noop("End parse"),
					  oid,
					  gettext_noop("Get headline"),
					  oid,
					  gettext_noop("Get token types"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	initPQExpBuffer(&title);
	if (nspname)
		printfPQExpBuffer(&title, _("Text search parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Text search parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT t.alias as \"%s\",\n"
					  "  t.description as \"%s\"\n"
					  "FROM pg_catalog.ts_token_type( '%s'::pg_catalog.oid ) as t\n"
					  "ORDER BY 1;",
					  gettext_noop("Token name"),
					  gettext_noop("Description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
	{
		termPQExpBuffer(&title);
		return false;
	}

	myopt.nullPrint = NULL;
	if (nspname)
		printfPQExpBuffer(&title, _("Token types for parser \"%s.%s\""),
						  nspname, prsname);
	else
		printfPQExpBuffer(&title, _("Token types for parser \"%s\""), prsname);
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = true;
	myopt.translate_header = true;
	myopt.translate_columns = NULL;
	myopt.n_translate_columns = 0;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}


/*
 * \dFd
 * 列出文本搜索字典
 */
bool
listTSDictionaries(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  n.nspname as \"%s\",\n"
					  "  d.dictname as \"%s\",\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"));

	if (verbose)
	{
		appendPQExpBuffer(&buf,
						  "  ( SELECT COALESCE(nt.nspname, '(null)')::pg_catalog.text || '.' || t.tmplname FROM\n"
						  "    pg_catalog.pg_ts_template t\n"
						  "    LEFT JOIN pg_catalog.pg_namespace nt ON nt.oid = t.tmplnamespace\n"
						  "    WHERE d.dicttemplate = t.oid ) AS  \"%s\",\n"
						  "  d.dictinitoption as \"%s\",\n",
						  gettext_noop("Template"),
						  gettext_noop("Init options"));
	}

	appendPQExpBuffer(&buf,
					  "  pg_catalog.obj_description(d.oid, 'pg_ts_dict') as \"%s\"\n",
					  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_dict d\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = d.dictnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "d.dictname", NULL,
								"pg_catalog.pg_ts_dict_is_visible(d.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search dictionaries");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dFt
 * 列出文本搜索模板
 */
bool
listTSTemplates(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	if (verbose)
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  t.tmplinit::pg_catalog.regproc AS \"%s\",\n"
						  "  t.tmpllexize::pg_catalog.regproc AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Init"),
						  gettext_noop("Lexize"),
						  gettext_noop("Description"));
	else
		printfPQExpBuffer(&buf,
						  "SELECT\n"
						  "  n.nspname AS \"%s\",\n"
						  "  t.tmplname AS \"%s\",\n"
						  "  pg_catalog.obj_description(t.oid, 'pg_ts_template') AS \"%s\"\n",
						  gettext_noop("Schema"),
						  gettext_noop("Name"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf, "FROM pg_catalog.pg_ts_template t\n"
						 "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = t.tmplnamespace\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "t.tmplname", NULL,
								"pg_catalog.pg_ts_template_is_visible(t.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search templates");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}


/*
 * \dF
 * 列出文本搜索配置
 */
bool
listTSConfigs(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	if (verbose)
		return listTSConfigsVerbose(pattern);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "   n.nspname as \"%s\",\n"
					  "   c.cfgname as \"%s\",\n"
					  "   pg_catalog.obj_description(c.oid, 'pg_ts_config') as \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace\n",
					  gettext_noop("Schema"),
					  gettext_noop("Name"),
					  gettext_noop("Description")
		);

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of text search configurations");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

static bool
listTSConfigsVerbose(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT c.oid, c.cfgname,\n"
					  "   n.nspname,\n"
					  "   p.prsname,\n"
					  "   np.nspname as pnspname\n"
					  "FROM pg_catalog.pg_ts_config c\n"
					  "   LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.cfgnamespace,\n"
					  " pg_catalog.pg_ts_parser p\n"
					  "   LEFT JOIN pg_catalog.pg_namespace np ON np.oid = p.prsnamespace\n"
					  "WHERE  p.oid = c.cfgparser\n"
		);

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								"n.nspname", "c.cfgname", NULL,
								"pg_catalog.pg_ts_config_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 3, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any text search configuration named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any text search configurations.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *oid;
		const char *cfgname;
		const char *nspname = NULL;
		const char *prsname;
		const char *pnspname = NULL;

		oid = PQgetvalue(res, i, 0);
		cfgname = PQgetvalue(res, i, 1);
		if (!PQgetisnull(res, i, 2))
			nspname = PQgetvalue(res, i, 2);
		prsname = PQgetvalue(res, i, 3);
		if (!PQgetisnull(res, i, 4))
			pnspname = PQgetvalue(res, i, 4);

		if (!describeOneTSConfig(oid, nspname, cfgname, pnspname, prsname))
		{
			PQclear(res);
			return false;
		}

		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
describeOneTSConfig(const char *oid, const char *nspname, const char *cfgname,
					const char *pnspname, const char *prsname)
{
	PQExpBufferData buf,
				title;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  ( SELECT t.alias FROM\n"
					  "    pg_catalog.ts_token_type(c.cfgparser) AS t\n"
					  "    WHERE t.tokid = m.maptokentype ) AS \"%s\",\n"
					  "  pg_catalog.btrim(\n"
					  "    ARRAY( SELECT mm.mapdict::pg_catalog.regdictionary\n"
					  "           FROM pg_catalog.pg_ts_config_map AS mm\n"
					  "           WHERE mm.mapcfg = m.mapcfg AND mm.maptokentype = m.maptokentype\n"
					  "           ORDER BY mapcfg, maptokentype, mapseqno\n"
					  "    ) :: pg_catalog.text,\n"
					  "  '{}') AS \"%s\"\n"
					  "FROM pg_catalog.pg_ts_config AS c, pg_catalog.pg_ts_config_map AS m\n"
					  "WHERE c.oid = '%s' AND m.mapcfg = c.oid\n"
					  "GROUP BY m.mapcfg, m.maptokentype, c.cfgparser\n"
					  "ORDER BY 1;",
					  gettext_noop("Token"),
					  gettext_noop("Dictionaries"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	initPQExpBuffer(&title);

	if (nspname)
		appendPQExpBuffer(&title, _("Text search configuration \"%s.%s\""),
						  nspname, cfgname);
	else
		appendPQExpBuffer(&title, _("Text search configuration \"%s\""),
						  cfgname);

	if (pnspname)
		appendPQExpBuffer(&title, _("\nParser: \"%s.%s\""),
						  pnspname, prsname);
	else
		appendPQExpBuffer(&title, _("\nParser: \"%s\""),
						  prsname);

	myopt.nullPrint = NULL;
	myopt.title = title.data;
	myopt.footers = NULL;
	myopt.topt.default_footer = false;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);

	PQclear(res);
	return true;
}


/*
 * \dew
 *
 * 描述外部数据包装器
 */
bool
listForeignDataWrappers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT fdw.fdwname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(fdw.fdwowner) AS \"%s\",\n"
					  "  fdw.fdwhandler::pg_catalog.regproc AS \"%s\",\n"
					  "  fdw.fdwvalidator::pg_catalog.regproc AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Handler"),
					  gettext_noop("Validator"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "fdwacl");
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN fdwoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(fdwoptions)),  ', ') || ')' "
						  "  END AS \"%s\""
						  ",\n  d.description AS \"%s\" ",
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_foreign_data_wrapper fdw\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d\n"
							 "       ON d.classoid = fdw.tableoid "
							 "AND d.objoid = fdw.oid AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "fdwname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign-data wrappers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \des
 *
 * 描述外部服务器。
 */
bool
listForeignServers(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT s.srvname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(s.srvowner) AS \"%s\",\n"
					  "  f.fdwname AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Foreign-data wrapper"));

	if (verbose)
	{
		appendPQExpBufferStr(&buf, ",\n  ");
		printACLColumn(&buf, "s.srvacl");
		appendPQExpBuffer(&buf,
						  ",\n"
						  "  s.srvtype AS \"%s\",\n"
						  "  s.srvversion AS \"%s\",\n"
						  "  CASE WHEN srvoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(srvoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("Type"),
						  gettext_noop("Version"),
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));
	}

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_foreign_server s\n"
						 "     JOIN pg_catalog.pg_foreign_data_wrapper f ON f.oid=s.srvfdw\n");

	if (verbose)
		appendPQExpBufferStr(&buf,
							 "LEFT JOIN pg_catalog.pg_description d\n       "
							 "ON d.classoid = s.tableoid AND d.objoid = s.oid "
							 "AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "s.srvname", NULL, NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign servers");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \deu
 *
 * 描述用户映射。
 */
bool
listUserMappings(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT um.srvname AS \"%s\",\n"
					  "  um.usename AS \"%s\"",
					  gettext_noop("Server"),
					  gettext_noop("User name"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN umoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(umoptions)),  ', ') || ')' "
						  "  END AS \"%s\"",
						  gettext_noop("FDW options"));

	appendPQExpBufferStr(&buf, "\nFROM pg_catalog.pg_user_mappings um\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "um.srvname", "um.usename", NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of user mappings");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \det
 *
 * 描述外部表。
 */
bool
listForeignTables(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT n.nspname AS \"%s\",\n"
					  "  c.relname AS \"%s\",\n"
					  "  s.srvname AS \"%s\"",
					  gettext_noop("Schema"),
					  gettext_noop("Table"),
					  gettext_noop("Server"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n CASE WHEN ftoptions IS NULL THEN '' ELSE "
						  "  '(' || pg_catalog.array_to_string(ARRAY(SELECT "
						  "  pg_catalog.quote_ident(option_name) ||  ' ' || "
						  "  pg_catalog.quote_literal(option_value)  FROM "
						  "  pg_catalog.pg_options_to_table(ftoptions)),  ', ') || ')' "
						  "  END AS \"%s\",\n"
						  "  d.description AS \"%s\"",
						  gettext_noop("FDW options"),
						  gettext_noop("Description"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_foreign_table ft\n"
						 "  INNER JOIN pg_catalog.pg_class c"
						 " ON c.oid = ft.ftrelid\n"
						 "  INNER JOIN pg_catalog.pg_namespace n"
						 " ON n.oid = c.relnamespace\n"
						 "  INNER JOIN pg_catalog.pg_foreign_server s"
						 " ON s.oid = ft.ftserver\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "   LEFT JOIN pg_catalog.pg_description d\n"
							 "          ON d.classoid = c.tableoid AND "
							 "d.objoid = c.oid AND d.objsubid = 0\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								"n.nspname", "c.relname", NULL,
								"pg_catalog.pg_table_is_visible(c.oid)",
								NULL, 3))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of foreign tables");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dx
 *
 * 简要描述已安装的扩展。
 */
bool
listExtensions(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname AS \"%s\", "
					  "e.extversion AS \"%s\", n.nspname AS \"%s\", c.description AS \"%s\"\n"
					  "FROM pg_catalog.pg_extension e "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace "
					  "LEFT JOIN pg_catalog.pg_description c ON c.objoid = e.oid "
					  "AND c.classoid = 'pg_catalog.pg_extension'::pg_catalog.regclass\n",
					  gettext_noop("Name"),
					  gettext_noop("Version"),
					  gettext_noop("Schema"),
					  gettext_noop("Description"));

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of installed extensions");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * \dx+
 *
 * 列出已安装扩展的内容。
 */
bool
listExtensionContents(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	int			i;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT e.extname, e.oid\n"
					  "FROM pg_catalog.pg_extension e\n");

	if (!validateSQLNamePattern(&buf, pattern,
								false, false,
								NULL, "e.extname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any extension named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any extensions.");
		}
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *extname;
		const char *oid;

		extname = PQgetvalue(res, i, 0);
		oid = PQgetvalue(res, i, 1);

		if (!listOneExtensionContents(extname, oid))
		{
			PQclear(res);
			return false;
		}
		if (cancel_pressed)
		{
			PQclear(res);
			return false;
		}
	}

	PQclear(res);
	return true;
}

static bool
listOneExtensionContents(const char *extname, const char *oid)
{
	PQExpBufferData buf;
	PGresult   *res;
	PQExpBufferData title;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);
	printfPQExpBuffer(&buf,
					  "SELECT pg_catalog.pg_describe_object(classid, objid, 0) AS \"%s\"\n"
					  "FROM pg_catalog.pg_depend\n"
					  "WHERE refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass AND refobjid = '%s' AND deptype = 'e'\n"
					  "ORDER BY 1;",
					  gettext_noop("Object description"),
					  oid);

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	initPQExpBuffer(&title);
	printfPQExpBuffer(&title, _("Objects in extension \"%s\""), extname);
	myopt.title = title.data;
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	termPQExpBuffer(&title);
	PQclear(res);
	return true;
}

/*
 * validateSQLNamePattern
 *
 * string_utils 的 processSQLNamePattern 的包装器，还检查
 * 模式的有效性。除了该函数的参数外，还接受一个
 * 'maxparts' 参数，指定模式允许的最大点分名称数量，
 * 以及一个 'added_clause' 参数，通过引用返回是否
 * 向 'buf' 添加了子句。返回模式是否通过验证，在记录
 * 任何错误之后。
 */
static bool
validateSQLNamePattern(PQExpBuffer buf, const char *pattern, bool have_where,
					   bool force_escape, const char *schemavar,
					   const char *namevar, const char *altnamevar,
					   const char *visibilityrule, bool *added_clause,
					   int maxparts)
{
	PQExpBufferData dbbuf;
	int			dotcnt;
	bool		added;

	initPQExpBuffer(&dbbuf);
	added = processSQLNamePattern(pset.db, buf, pattern, have_where, force_escape,
								  schemavar, namevar, altnamevar,
								  visibilityrule, &dbbuf, &dotcnt);
	if (added_clause != NULL)
		*added_clause = added;

	if (dotcnt >= maxparts)
	{
		pg_log_error("improper qualified name (too many dotted names): %s",
					 pattern);
		goto error_return;
	}

	if (maxparts > 1 && dotcnt == maxparts - 1)
	{
		if (PQdb(pset.db) == NULL)
		{
			pg_log_error("You are currently not connected to a database.");
			goto error_return;
		}
		if (strcmp(PQdb(pset.db), dbbuf.data) != 0)
		{
			pg_log_error("cross-database references are not implemented: %s",
						 pattern);
			goto error_return;
		}
	}
	termPQExpBuffer(&dbbuf);
	return true;

error_return:
	termPQExpBuffer(&dbbuf);
	return false;
}

/*
 * \dRp
 * 列出发布。
 *
 * 接受一个可选的正则表达式来选择特定的发布
 */
bool
listPublications(const char *pattern)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false, false, false, false, false};

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support publications.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT pubname AS \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(pubowner) AS \"%s\",\n"
					  "  puballtables AS \"%s\",\n"
					  "  pubinsert AS \"%s\",\n"
					  "  pubupdate AS \"%s\",\n"
					  "  pubdelete AS \"%s\"",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("All tables"),
					  gettext_noop("Inserts"),
					  gettext_noop("Updates"),
					  gettext_noop("Deletes"));
	if (pset.sversion >= 110000)
		appendPQExpBuffer(&buf,
						  ",\n  pubtruncate AS \"%s\"",
						  gettext_noop("Truncates"));
	if (pset.sversion >= 130000)
		appendPQExpBuffer(&buf,
						  ",\n  pubviaroot AS \"%s\"",
						  gettext_noop("Via root"));

	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_publication\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "pubname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of publications");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);

	return true;
}

/*
 * 向发布描述添加页脚。
 */
static bool
addFooterToPublicationDesc(PQExpBuffer buf, const char *footermsg,
						   bool as_schema, printTableContent *const cont)
{
	PGresult   *res;
	int			count = 0;
	int			i = 0;

	res = PSQLexec(buf->data);
	if (!res)
		return false;
	else
		count = PQntuples(res);

	if (count > 0)
		printTableAddFooter(cont, footermsg);

	for (i = 0; i < count; i++)
	{
		if (as_schema)
			printfPQExpBuffer(buf, "    \"%s\"", PQgetvalue(res, i, 0));
		else
		{
			printfPQExpBuffer(buf, "    \"%s.%s\"", PQgetvalue(res, i, 0),
							  PQgetvalue(res, i, 1));

			if (!PQgetisnull(res, i, 3))
				appendPQExpBuffer(buf, " (%s)", PQgetvalue(res, i, 3));

			if (!PQgetisnull(res, i, 2))
				appendPQExpBuffer(buf, " WHERE %s", PQgetvalue(res, i, 2));
		}

		printTableAddFooter(cont, buf->data);
	}

	PQclear(res);
	return true;
}

/*
 * \dRp+
 * 描述发布，包括内容。
 *
 * 接受一个可选的正则表达式来选择特定的发布
 */
bool
describePublications(const char *pattern)
{
	PQExpBufferData buf;
	int			i;
	PGresult   *res;
	bool		has_pubtruncate;
	bool		has_pubviaroot;

	PQExpBufferData title;
	printTableContent cont;

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support publications.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	has_pubtruncate = (pset.sversion >= 110000);
	has_pubviaroot = (pset.sversion >= 130000);

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT oid, pubname,\n"
					  "  pg_catalog.pg_get_userbyid(pubowner) AS owner,\n"
					  "  puballtables, pubinsert, pubupdate, pubdelete");
	if (has_pubtruncate)
		appendPQExpBufferStr(&buf,
							 ", pubtruncate");
	if (has_pubviaroot)
		appendPQExpBufferStr(&buf,
							 ", pubviaroot");
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_publication\n");

	if (!validateSQLNamePattern(&buf, pattern, false, false,
								NULL, "pubname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 2;");

	res = PSQLexec(buf.data);
	if (!res)
	{
		termPQExpBuffer(&buf);
		return false;
	}

	if (PQntuples(res) == 0)
	{
		if (!pset.quiet)
		{
			if (pattern)
				pg_log_error("Did not find any publication named \"%s\".",
							 pattern);
			else
				pg_log_error("Did not find any publications.");
		}

		termPQExpBuffer(&buf);
		PQclear(res);
		return false;
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		const char	align = 'l';
		int			ncols = 5;
		int			nrows = 1;
		char	   *pubid = PQgetvalue(res, i, 0);
		char	   *pubname = PQgetvalue(res, i, 1);
		bool		puballtables = strcmp(PQgetvalue(res, i, 3), "t") == 0;
		printTableOpt myopt = pset.popt.topt;

		if (has_pubtruncate)
			ncols++;
		if (has_pubviaroot)
			ncols++;

		initPQExpBuffer(&title);
		printfPQExpBuffer(&title, _("Publication %s"), pubname);
		printTableInit(&cont, &myopt, title.data, ncols, nrows);

		printTableAddHeader(&cont, gettext_noop("Owner"), true, align);
		printTableAddHeader(&cont, gettext_noop("All tables"), true, align);
		printTableAddHeader(&cont, gettext_noop("Inserts"), true, align);
		printTableAddHeader(&cont, gettext_noop("Updates"), true, align);
		printTableAddHeader(&cont, gettext_noop("Deletes"), true, align);
		if (has_pubtruncate)
			printTableAddHeader(&cont, gettext_noop("Truncates"), true, align);
		if (has_pubviaroot)
			printTableAddHeader(&cont, gettext_noop("Via root"), true, align);

		printTableAddCell(&cont, PQgetvalue(res, i, 2), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 3), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 4), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 5), false, false);
		printTableAddCell(&cont, PQgetvalue(res, i, 6), false, false);
		if (has_pubtruncate)
			printTableAddCell(&cont, PQgetvalue(res, i, 7), false, false);
		if (has_pubviaroot)
			printTableAddCell(&cont, PQgetvalue(res, i, 8), false, false);

		if (!puballtables)
		{
			/* 获取指定发布的表 */
			printfPQExpBuffer(&buf,
							  "SELECT n.nspname, c.relname");
			if (pset.sversion >= 150000)
			{
				appendPQExpBufferStr(&buf,
									 ", pg_get_expr(pr.prqual, c.oid)");
				appendPQExpBufferStr(&buf,
									 ", (CASE WHEN pr.prattrs IS NOT NULL THEN\n"
									 "     pg_catalog.array_to_string("
									 "      ARRAY(SELECT attname\n"
									 "              FROM\n"
									 "                pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
									 "                pg_catalog.pg_attribute\n"
									 "        WHERE attrelid = c.oid AND attnum = prattrs[s]), ', ')\n"
									 "       ELSE NULL END)");
			}
			else
				appendPQExpBufferStr(&buf,
									 ", NULL, NULL");
			appendPQExpBuffer(&buf,
							  "\nFROM pg_catalog.pg_class c,\n"
							  "     pg_catalog.pg_namespace n,\n"
							  "     pg_catalog.pg_publication_rel pr\n"
							  "WHERE c.relnamespace = n.oid\n"
							  "  AND c.oid = pr.prrelid\n"
							  "  AND pr.prpubid = '%s'\n"
							  "ORDER BY 1,2", pubid);
			if (!addFooterToPublicationDesc(&buf, _("Tables:"), false, &cont))
				goto error_return;

			if (pset.sversion >= 150000)
			{
				/* 获取指定发布的模式 */
				printfPQExpBuffer(&buf,
								  "SELECT n.nspname\n"
								  "FROM pg_catalog.pg_namespace n\n"
								  "     JOIN pg_catalog.pg_publication_namespace pn ON n.oid = pn.pnnspid\n"
								  "WHERE pn.pnpubid = '%s'\n"
								  "ORDER BY 1", pubid);
				if (!addFooterToPublicationDesc(&buf, _("Tables from schemas:"),
												true, &cont))
					goto error_return;
			}
		}

		printTable(&cont, pset.queryFout, false, pset.logfile);
		printTableCleanup(&cont);

		termPQExpBuffer(&title);
	}

	termPQExpBuffer(&buf);
	PQclear(res);

	return true;

error_return:
	printTableCleanup(&cont);
	PQclear(res);
	termPQExpBuffer(&buf);
	termPQExpBuffer(&title);
	return false;
}

/*
 * \dRs
 * 描述订阅。
 *
 * 接受一个可选的正则表达式来选择特定的订阅
 */
bool
describeSubscriptions(const char *pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	static const bool translate_columns[] = {false, false, false, false,
	false, false, false, false, false, false, false, false, false, false};

	if (pset.sversion < 100000)
	{
		char		sverbuf[32];

		pg_log_error("The server (version %s) does not support subscriptions.",
					 formatPGVersionNumber(pset.sversion, false,
										   sverbuf, sizeof(sverbuf)));
		return true;
	}

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT subname AS \"%s\"\n"
					  ",  pg_catalog.pg_get_userbyid(subowner) AS \"%s\"\n"
					  ",  subenabled AS \"%s\"\n"
					  ",  subpublications AS \"%s\"\n",
					  gettext_noop("Name"),
					  gettext_noop("Owner"),
					  gettext_noop("Enabled"),
					  gettext_noop("Publication"));

	if (verbose)
	{
		/* 二进制模式和流式复制仅在 v14 及以上版本支持 */
		if (pset.sversion >= 140000)
		{
			appendPQExpBuffer(&buf,
							  ", subbinary AS \"%s\"\n",
							  gettext_noop("Binary"));

			if (pset.sversion >= 160000)
				appendPQExpBuffer(&buf,
								  ", (CASE substream\n"
								  "    WHEN 'f' THEN 'off'\n"
								  "    WHEN 't' THEN 'on'\n"
								  "    WHEN 'p' THEN 'parallel'\n"
								  "   END) AS \"%s\"\n",
								  gettext_noop("Streaming"));
			else
				appendPQExpBuffer(&buf,
								  ", substream AS \"%s\"\n",
								  gettext_noop("Streaming"));
		}

		/* Two_phase 和 disable_on_error 仅在 v15 及以上版本支持 */
		if (pset.sversion >= 150000)
			appendPQExpBuffer(&buf,
							  ", subtwophasestate AS \"%s\"\n"
							  ", subdisableonerr AS \"%s\"\n",
							  gettext_noop("Two-phase commit"),
							  gettext_noop("Disable on error"));

		if (pset.sversion >= 160000)
			appendPQExpBuffer(&buf,
							  ", suborigin AS \"%s\"\n"
							  ", subpasswordrequired AS \"%s\"\n"
							  ", subrunasowner AS \"%s\"\n",
							  gettext_noop("Origin"),
							  gettext_noop("Password required"),
							  gettext_noop("Run as owner?"));

		appendPQExpBuffer(&buf,
						  ",  subsynccommit AS \"%s\"\n"
						  ",  subconninfo AS \"%s\"\n",
						  gettext_noop("Synchronous commit"),
						  gettext_noop("Conninfo"));

		/* Skip LSN 仅在 v15 及以上版本支持 */
		if (pset.sversion >= 150000)
			appendPQExpBuffer(&buf,
							  ", subskiplsn AS \"%s\"\n",
							  gettext_noop("Skip LSN"));
	}

	/* 仅显示当前数据库中的订阅。 */
	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_subscription\n"
						 "WHERE subdbid = (SELECT oid\n"
						 "                 FROM pg_catalog.pg_database\n"
						 "                 WHERE datname = pg_catalog.current_database())");

	if (!validateSQLNamePattern(&buf, pattern, true, false,
								NULL, "subname", NULL,
								NULL,
								NULL, 1))
	{
		termPQExpBuffer(&buf);
		return false;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of subscriptions");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}

/*
 * printACLColumn
 *
 * 用于一致格式化 ACL（权限）列的辅助函数。
 * 将适当的目标列表项追加到 buf。注意没有
 * 任何空白或逗号修饰。
 */
static void
printACLColumn(PQExpBuffer buf, const char *colname)
{
	appendPQExpBuffer(buf,
					  "pg_catalog.array_to_string(%s, E'\\n') AS \"%s\"",
					  colname, gettext_noop("Access privileges"));
}

/*
 * \dAc
 * 列出操作符类
 *
 * 接受可选的正则表达式来按索引访问方法和输入数据类型过滤。
 */
bool
listOperatorClasses(const char *access_method_pattern,
					const char *type_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  pg_catalog.format_type(c.opcintype, NULL) AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN c.opckeytype <> 0 AND c.opckeytype <> c.opcintype\n"
					  "    THEN pg_catalog.format_type(c.opckeytype, NULL)\n"
					  "    ELSE NULL\n"
					  "  END AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opclass_is_visible(c.oid)\n"
					  "    THEN pg_catalog.format('%%I', c.opcname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', n.nspname, c.opcname)\n"
					  "  END AS \"%s\",\n"
					  "  (CASE WHEN c.opcdefault\n"
					  "    THEN '%s'\n"
					  "    ELSE '%s'\n"
					  "  END) AS \"%s\"",
					  gettext_noop("AM"),
					  gettext_noop("Input type"),
					  gettext_noop("Storage type"),
					  gettext_noop("Operator class"),
					  gettext_noop("yes"),
					  gettext_noop("no"),
					  gettext_noop("Default?"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  CASE\n"
						  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
						  "    THEN pg_catalog.format('%%I', of.opfname)\n"
						  "    ELSE pg_catalog.format('%%I.%%I', ofn.nspname, of.opfname)\n"
						  "  END AS \"%s\",\n"
						  " pg_catalog.pg_get_userbyid(c.opcowner) AS \"%s\"\n",
						  gettext_noop("Operator family"),
						  gettext_noop("Owner"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_opclass c\n"
						 "  LEFT JOIN pg_catalog.pg_am am on am.oid = c.opcmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.opcnamespace\n"
						 "  LEFT JOIN pg_catalog.pg_type t ON t.oid = c.opcintype\n"
						 "  LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid = t.typnamespace\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = c.opcfamily\n"
							 "  LEFT JOIN pg_catalog.pg_namespace ofn ON ofn.oid = of.opfnamespace\n");

	if (access_method_pattern)
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname", NULL, NULL,
									&have_where, 1))
			goto error_return;
	if (type_pattern)
	{
		/* 将类型名称模式匹配到内部名称或外部名称 */
		if (!validateSQLNamePattern(&buf, type_pattern, have_where, false,
									"tn.nspname", "t.typname",
									"pg_catalog.format_type(t.oid, NULL)",
									"pg_catalog.pg_type_is_visible(t.oid)",
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2, 4;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operator classes");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAf
 * 列出操作符族
 *
 * 接受可选的正则表达式来按索引访问方法和输入数据类型过滤。
 */
bool
listOperatorFamilies(const char *access_method_pattern,
					 const char *type_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(f.oid)\n"
					  "    THEN pg_catalog.format('%%I', f.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', n.nspname, f.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  (SELECT\n"
					  "     pg_catalog.string_agg(pg_catalog.format_type(oc.opcintype, NULL), ', ')\n"
					  "   FROM pg_catalog.pg_opclass oc\n"
					  "   WHERE oc.opcfamily = f.oid) \"%s\"",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Applicable types"));
	if (verbose)
		appendPQExpBuffer(&buf,
						  ",\n  pg_catalog.pg_get_userbyid(f.opfowner) AS \"%s\"\n",
						  gettext_noop("Owner"));
	appendPQExpBufferStr(&buf,
						 "\nFROM pg_catalog.pg_opfamily f\n"
						 "  LEFT JOIN pg_catalog.pg_am am on am.oid = f.opfmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace n ON n.oid = f.opfnamespace\n");

	if (access_method_pattern)
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname", NULL, NULL,
									&have_where, 1))
			goto error_return;
	if (type_pattern)
	{
		appendPQExpBuffer(&buf,
						  "  %s EXISTS (\n"
						  "    SELECT 1\n"
						  "    FROM pg_catalog.pg_type t\n"
						  "    JOIN pg_catalog.pg_opclass oc ON oc.opcintype = t.oid\n"
						  "    LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid = t.typnamespace\n"
						  "    WHERE oc.opcfamily = f.oid\n",
						  have_where ? "AND" : "WHERE");
		/* 将类型名称模式匹配到内部名称或外部名称 */
		if (!validateSQLNamePattern(&buf, type_pattern, true, false,
									"tn.nspname", "t.typname",
									"pg_catalog.format_type(t.oid, NULL)",
									"pg_catalog.pg_type_is_visible(t.oid)",
									NULL, 3))
			goto error_return;
		appendPQExpBufferStr(&buf, "  )\n");
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2;");
	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAo
 * 列出操作符族的操作符
 *
 * 接受可选的正则表达式来按索引访问方法和
 * 操作符族过滤。
 */
bool
listOpFamilyOperators(const char *access_method_pattern,
					  const char *family_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;

	static const bool translate_columns[] = {false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
					  "    THEN pg_catalog.format('%%I', of.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', nsf.nspname, of.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  o.amopopr::pg_catalog.regoperator AS \"%s\"\n,"
					  "  o.amopstrategy AS \"%s\",\n"
					  "  CASE o.amoppurpose\n"
					  "    WHEN 'o' THEN '%s'\n"
					  "    WHEN 's' THEN '%s'\n"
					  "  END AS \"%s\"\n",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Operator"),
					  gettext_noop("Strategy"),
					  gettext_noop("ordering"),
					  gettext_noop("search"),
					  gettext_noop("Purpose"));

	if (verbose)
		appendPQExpBuffer(&buf,
						  ", ofs.opfname AS \"%s\"\n",
						  gettext_noop("Sort opfamily"));
	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_amop o\n"
						 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = o.amopfamily\n"
						 "  LEFT JOIN pg_catalog.pg_am am ON am.oid = of.opfmethod AND am.oid = o.amopmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace nsf ON of.opfnamespace = nsf.oid\n");
	if (verbose)
		appendPQExpBufferStr(&buf,
							 "  LEFT JOIN pg_catalog.pg_opfamily ofs ON ofs.oid = o.amopsortfamily\n");

	if (access_method_pattern)
	{
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname",
									NULL, NULL,
									&have_where, 1))
			goto error_return;
	}

	if (family_pattern)
	{
		if (!validateSQLNamePattern(&buf, family_pattern, have_where, false,
									"nsf.nspname", "of.opfname", NULL, NULL,
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2,\n"
						 "  o.amoplefttype = o.amoprighttype DESC,\n"
						 "  pg_catalog.format_type(o.amoplefttype, NULL),\n"
						 "  pg_catalog.format_type(o.amoprighttype, NULL),\n"
						 "  o.amopstrategy;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of operators of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dAp
 * 列出操作符族的支持函数
 *
 * 接受可选的正则表达式来按索引访问方法和
 * 操作符族过滤。
 */
bool
listOpFamilyFunctions(const char *access_method_pattern,
					  const char *family_pattern, bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;
	bool		have_where = false;
	static const bool translate_columns[] = {false, false, false, false, false, false};

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT\n"
					  "  am.amname AS \"%s\",\n"
					  "  CASE\n"
					  "    WHEN pg_catalog.pg_opfamily_is_visible(of.oid)\n"
					  "    THEN pg_catalog.format('%%I', of.opfname)\n"
					  "    ELSE pg_catalog.format('%%I.%%I', ns.nspname, of.opfname)\n"
					  "  END AS \"%s\",\n"
					  "  pg_catalog.format_type(ap.amproclefttype, NULL) AS \"%s\",\n"
					  "  pg_catalog.format_type(ap.amprocrighttype, NULL) AS \"%s\",\n"
					  "  ap.amprocnum AS \"%s\"\n",
					  gettext_noop("AM"),
					  gettext_noop("Operator family"),
					  gettext_noop("Registered left type"),
					  gettext_noop("Registered right type"),
					  gettext_noop("Number"));

	if (!verbose)
		appendPQExpBuffer(&buf,
						  ", p.proname AS \"%s\"\n",
						  gettext_noop("Function"));
	else
		appendPQExpBuffer(&buf,
						  ", ap.amproc::pg_catalog.regprocedure AS \"%s\"\n",
						  gettext_noop("Function"));

	appendPQExpBufferStr(&buf,
						 "FROM pg_catalog.pg_amproc ap\n"
						 "  LEFT JOIN pg_catalog.pg_opfamily of ON of.oid = ap.amprocfamily\n"
						 "  LEFT JOIN pg_catalog.pg_am am ON am.oid = of.opfmethod\n"
						 "  LEFT JOIN pg_catalog.pg_namespace ns ON of.opfnamespace = ns.oid\n"
						 "  LEFT JOIN pg_catalog.pg_proc p ON ap.amproc = p.oid\n");

	if (access_method_pattern)
	{
		if (!validateSQLNamePattern(&buf, access_method_pattern,
									false, false, NULL, "am.amname",
									NULL, NULL,
									&have_where, 1))
			goto error_return;
	}
	if (family_pattern)
	{
		if (!validateSQLNamePattern(&buf, family_pattern, have_where, false,
									"ns.nspname", "of.opfname", NULL, NULL,
									NULL, 3))
			goto error_return;
	}

	appendPQExpBufferStr(&buf, "ORDER BY 1, 2,\n"
						 "  ap.amproclefttype = ap.amprocrighttype DESC,\n"
						 "  3, 4, 5;");

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("List of support functions of operator families");
	myopt.translate_header = true;
	myopt.translate_columns = translate_columns;
	myopt.n_translate_columns = lengthof(translate_columns);

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;

error_return:
	termPQExpBuffer(&buf);
	return false;
}

/*
 * \dl 或 \lo_list
 * 列出大对象
 */
bool
listLargeObjects(bool verbose)
{
	PQExpBufferData buf;
	PGresult   *res;
	printQueryOpt myopt = pset.popt;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf,
					  "SELECT oid as \"%s\",\n"
					  "  pg_catalog.pg_get_userbyid(lomowner) as \"%s\",\n  ",
					  gettext_noop("ID"),
					  gettext_noop("Owner"));

	if (verbose)
	{
		printACLColumn(&buf, "lomacl");
		appendPQExpBufferStr(&buf, ",\n  ");
	}

	appendPQExpBuffer(&buf,
					  "pg_catalog.obj_description(oid, 'pg_largeobject') as \"%s\"\n"
					  "FROM pg_catalog.pg_largeobject_metadata\n"
					  "ORDER BY oid",
					  gettext_noop("Description"));

	res = PSQLexec(buf.data);
	termPQExpBuffer(&buf);
	if (!res)
		return false;

	myopt.nullPrint = NULL;
	myopt.title = _("Large objects");
	myopt.translate_header = true;

	printQuery(res, &myopt, pset.queryFout, false, pset.logfile);

	PQclear(res);
	return true;
}
