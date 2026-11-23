/*-------------------------------------------------------------------------
 *
 * hbafuncs.c
 *	  认证文件 SQL 视图的支持函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/hbafuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaddress.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/hba.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"


static ArrayType *get_hba_options(HbaLine *hba);
static void fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
						  int rule_number, char *filename, int lineno,
						  HbaLine *hba, const char *err_msg);
static void fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);
static void fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
							int map_number, char *filename, int lineno,
							IdentLine *ident, const char *err_msg);
static void fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc);


/*
 * 本宏规定任一受支持认证方法可能出现的
 * 认证选项最大个数。当前 LDAP 支持 12 个,另有
 * 3 个与认证方法无关。未必能同时全部设置,但
 * 把宏值设得足够高以保守并避免静态分析
 * 工具警告。
 *
 */
#define MAX_HBA_OPTIONS 15

/*
 * 创建列出 HBA 行中所指定选项的 text 数组。
 * 若未指定选项则返回 NULL。
 */
static ArrayType *
get_hba_options(HbaLine *hba)
{
	int			noptions;
	Datum		options[MAX_HBA_OPTIONS];

	noptions = 0;

	if (hba->auth_method == uaGSS || hba->auth_method == uaSSPI)
	{
		if (hba->include_realm)
			options[noptions++] =
				CStringGetTextDatum("include_realm=true");

		if (hba->krb_realm)
			options[noptions++] =
				CStringGetTextDatum(psprintf("krb_realm=%s", hba->krb_realm));
	}

	if (hba->usermap)
		options[noptions++] =
			CStringGetTextDatum(psprintf("map=%s", hba->usermap));

	if (hba->clientcert != clientCertOff)
		options[noptions++] =
			CStringGetTextDatum(psprintf("clientcert=%s", (hba->clientcert == clientCertCA) ? "verify-ca" : "verify-full"));

	if (hba->pamservice)
		options[noptions++] =
			CStringGetTextDatum(psprintf("pamservice=%s", hba->pamservice));

	if (hba->auth_method == uaLDAP)
	{
		if (hba->ldapserver)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapserver=%s", hba->ldapserver));

		if (hba->ldapport)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapport=%d", hba->ldapport));

		if (hba->ldapscheme)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscheme=%s", hba->ldapscheme));

		if (hba->ldaptls)
			options[noptions++] =
				CStringGetTextDatum("ldaptls=true");

		if (hba->ldapprefix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapprefix=%s", hba->ldapprefix));

		if (hba->ldapsuffix)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsuffix=%s", hba->ldapsuffix));

		if (hba->ldapbasedn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbasedn=%s", hba->ldapbasedn));

		if (hba->ldapbinddn)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbinddn=%s", hba->ldapbinddn));

		if (hba->ldapbindpasswd)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapbindpasswd=%s",
											 hba->ldapbindpasswd));

		if (hba->ldapsearchattribute)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchattribute=%s",
											 hba->ldapsearchattribute));

		if (hba->ldapsearchfilter)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapsearchfilter=%s",
											 hba->ldapsearchfilter));

		if (hba->ldapscope)
			options[noptions++] =
				CStringGetTextDatum(psprintf("ldapscope=%d", hba->ldapscope));
	}

	if (hba->auth_method == uaRADIUS)
	{
		if (hba->radiusservers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusservers=%s", hba->radiusservers_s));

		if (hba->radiussecrets_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiussecrets=%s", hba->radiussecrets_s));

		if (hba->radiusidentifiers_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusidentifiers=%s", hba->radiusidentifiers_s));

		if (hba->radiusports_s)
			options[noptions++] =
				CStringGetTextDatum(psprintf("radiusports=%s", hba->radiusports_s));
	}

	/* 若再加选项,考虑增大 MAX_HBA_OPTIONS。 */
	Assert(noptions <= MAX_HBA_OPTIONS);

	if (noptions > 0)
		return construct_array_builtin(options, noptions, TEXTOID);
	else
		return NULL;
}

/* pg_hba_file_rules 视图的列数 */
#define NUM_PG_HBA_FILE_RULES_ATTS	 11

/*
 * fill_hba_line
 *		构建 pg_hba_file_rules 视图的一行,加入 tuplestore。
 *
 * tuple_store: 存放数据之处
 * tupdesc: 视图的元组描述符
 * rule_number: 全部有效规则中的唯一标识
 * filename: 配置文件名(必须始终有效)
 * lineno: 配置文件行号(必须始终有效)
 * hba: 已解析行数据(可为 NULL,此时应设置 err_msg)
 * err_msg: 错误消息(无则 NULL)
 *
 * 注意:会泄漏内存,但无妨,因运行于短命内存上下文。
 *
 */
static void
fill_hba_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
			  int rule_number, char *filename, int lineno, HbaLine *hba,
			  const char *err_msg)
{
	Datum		values[NUM_PG_HBA_FILE_RULES_ATTS];
	bool		nulls[NUM_PG_HBA_FILE_RULES_ATTS];
	char		buffer[NI_MAXHOST];
	HeapTuple	tuple;
	int			index;
	ListCell   *lc;
	const char *typestr;
	const char *addrstr;
	const char *maskstr;
	ArrayType  *options;

	Assert(tupdesc->natts == NUM_PG_HBA_FILE_RULES_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* rule_number,出错时无 */
	if (err_msg)
		nulls[index++] = true;
	else
		values[index++] = Int32GetDatum(rule_number);

	/* file_name */
	values[index++] = CStringGetTextDatum(filename);

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (hba != NULL)
	{
		/* type */
		/* 不用 default: 以便编译器警告缺案 */
		typestr = NULL;
		switch (hba->conntype)
		{
			case ctLocal:
				typestr = "local";
				break;
			case ctHost:
				typestr = "host";
				break;
			case ctHostSSL:
				typestr = "hostssl";
				break;
			case ctHostNoSSL:
				typestr = "hostnossl";
				break;
			case ctHostGSS:
				typestr = "hostgssenc";
				break;
			case ctHostNoGSS:
				typestr = "hostnogssenc";
				break;
		}
		if (typestr)
			values[index++] = CStringGetTextDatum(typestr);
		else
			nulls[index++] = true;

		/* database */
		if (hba->databases)
		{
			/*
			 * 把 AuthToken 列表展平为字符串列表。或许应
			 * 对被引号的词元重新加引号,但已被否决,
			 * 理由是会更难把数组元素与其他系统
			 * 目录比较。这使得 "all"/"samerole" 等项
			 * 形式上有歧义……但把库/角色起成那样的
			 * 用户是自找麻烦。
			 */
			List	   *names = NIL;

			foreach(lc, hba->databases)
			{
				AuthToken  *tok = lfirst(lc);

				names = lappend(names, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(names));
		}
		else
			nulls[index++] = true;

		/* user */
		if (hba->roles)
		{
			/* 把 AuthToken 列表展平为字符串列表;见上文 */
			List	   *roles = NIL;

			foreach(lc, hba->roles)
			{
				AuthToken  *tok = lfirst(lc);

				roles = lappend(roles, tok->string);
			}
			values[index++] = PointerGetDatum(strlist_to_textarray(roles));
		}
		else
			nulls[index++] = true;

		/* address 与 netmask */
		/* 不用 default: 以便编译器警告缺案 */
		addrstr = maskstr = NULL;
		switch (hba->ip_cmp_method)
		{
			case ipCmpMask:
				if (hba->hostname)
				{
					addrstr = hba->hostname;
				}
				else
				{
					/*
					 * 注意:pg_getnameinfo_all 失败则 buffer 为
					 * "???",正是要返回的。
					 */
					if (hba->addrlen > 0)
					{
						if (pg_getnameinfo_all(&hba->addr, hba->addrlen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->addr.ss_family, buffer);
						addrstr = pstrdup(buffer);
					}
					if (hba->masklen > 0)
					{
						if (pg_getnameinfo_all(&hba->mask, hba->masklen,
											   buffer, sizeof(buffer),
											   NULL, 0,
											   NI_NUMERICHOST) == 0)
							clean_ipv6_addr(hba->mask.ss_family, buffer);
						maskstr = pstrdup(buffer);
					}
				}
				break;
			case ipCmpAll:
				addrstr = "all";
				break;
			case ipCmpSameHost:
				addrstr = "samehost";
				break;
			case ipCmpSameNet:
				addrstr = "samenet";
				break;
		}
		if (addrstr)
			values[index++] = CStringGetTextDatum(addrstr);
		else
			nulls[index++] = true;
		if (maskstr)
			values[index++] = CStringGetTextDatum(maskstr);
		else
			nulls[index++] = true;

		/* auth_method */
		values[index++] = CStringGetTextDatum(hba_authname(hba->auth_method));

		/* options */
		options = get_hba_options(hba);
		if (options)
			values[index++] = PointerGetDatum(options);
		else
			nulls[index++] = true;
	}
	else
	{
		/* 无解析结果,相关字段置空 */
		memset(&nulls[3], true, (NUM_PG_HBA_FILE_RULES_ATTS - 4) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_HBA_FILE_RULES_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_HBA_FILE_RULES_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * fill_hba_view
 *		读 pg_hba.conf 并用视图记录填 tuplestore。
 */
static void
fill_hba_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	ListCell   *line;
	int			rule_number = 0;
	MemoryContext hbacxt;
	MemoryContext oldcxt;

	/*
	 * 万一打不开 pg_hba.conf,我们抛错,而不是试图
	 * 用某种视图条目报告。(多数其他错误条件应
	 * 在视图条目中留消息。)
	 *
	 */
	file = open_auth_file(HbaFileName, ERROR, 0, NULL);

	tokenize_auth_file(HbaFileName, file, &hba_lines, DEBUG3, 0);

	/* 现在解析全部行 */
	hbacxt = AllocSetContextCreate(CurrentMemoryContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *hbaline = NULL;

		/* 已有错误的行不再解析 */
		if (tok_line->err_msg == NULL)
			hbaline = parse_hba_line(tok_line, DEBUG3);

		/* 无错,设置新规则号 */
		if (tok_line->err_msg == NULL)
			rule_number++;

		fill_hba_line(tuple_store, tupdesc, rule_number,
					  tok_line->file_name, tok_line->line_num, hbaline,
					  tok_line->err_msg);
	}

	/* 释放分词器内存 */
	free_auth_file(file, 0);
	/* 清 parse_hba_line 内存 */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(hbacxt);
}

/*
 * pg_hba_file_rules
 *
 * SQL 可访问 SRF,返回 pg_hba.conf 全部
 * 条目。
 */
Datum
pg_hba_file_rules(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * 构建存放结果行的 tuplestore。必须用 Materialize
	 * 模式以防游标打开期间 HBA 文件变更。也比每次
	 * 在已解析列表中查找当前位置更
	 * 高效。
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* 填充 tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_hba_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}

/* pg_ident_file_mappings 视图的列数 */
#define NUM_PG_IDENT_FILE_MAPPINGS_ATTS	 7

/*
 * fill_ident_line: 构建 pg_ident_file_mappings 视图的一行,加入
 * tuplestore
 *
 * tuple_store: 存放数据之处
 * tupdesc: 视图的元组描述符
 * map_number: 全部有效映射中的唯一标识
 * filename: 配置文件名(必须始终有效)
 * lineno: 配置文件行号(必须始终有效)
 * ident: 已解析行数据(可为 NULL,此时应设置 err_msg)
 * err_msg: 错误消息(无则 NULL)
 *
 * 注意:会泄漏内存,但无妨,因运行于短命内存上下文。
 *
 */
static void
fill_ident_line(Tuplestorestate *tuple_store, TupleDesc tupdesc,
				int map_number, char *filename, int lineno, IdentLine *ident,
				const char *err_msg)
{
	Datum		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	bool		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS];
	HeapTuple	tuple;
	int			index;

	Assert(tupdesc->natts == NUM_PG_IDENT_FILE_MAPPINGS_ATTS);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	index = 0;

	/* map_number,出错时无 */
	if (err_msg)
		nulls[index++] = true;
	else
		values[index++] = Int32GetDatum(map_number);

	/* file_name */
	values[index++] = CStringGetTextDatum(filename);

	/* line_number */
	values[index++] = Int32GetDatum(lineno);

	if (ident != NULL)
	{
		values[index++] = CStringGetTextDatum(ident->usermap);
		values[index++] = CStringGetTextDatum(ident->system_user->string);
		values[index++] = CStringGetTextDatum(ident->pg_user->string);
	}
	else
	{
		/* 无解析结果,相关字段置空 */
		memset(&nulls[3], true, (NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 4) * sizeof(bool));
	}

	/* error */
	if (err_msg)
		values[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = CStringGetTextDatum(err_msg);
	else
		nulls[NUM_PG_IDENT_FILE_MAPPINGS_ATTS - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tuple_store, tuple);
}

/*
 * 读 pg_ident.conf 并用视图记录填 tuplestore。
 */
static void
fill_ident_view(Tuplestorestate *tuple_store, TupleDesc tupdesc)
{
	FILE	   *file;
	List	   *ident_lines = NIL;
	ListCell   *line;
	int			map_number = 0;
	MemoryContext identcxt;
	MemoryContext oldcxt;

	/*
	 * 万一打不开 pg_ident.conf,我们抛错,而不是试图
	 * 用某种视图条目报告。(多数其他错误条件应
	 * 在视图条目中留消息。)
	 *
	 */
	file = open_auth_file(IdentFileName, ERROR, 0, NULL);

	tokenize_auth_file(IdentFileName, file, &ident_lines, DEBUG3, 0);

	/* 现在解析全部行 */
	identcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "ident parser context",
									 ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(identcxt);
	foreach(line, ident_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		IdentLine  *identline = NULL;

		/* 已有错误的行不再解析 */
		if (tok_line->err_msg == NULL)
			identline = parse_ident_line(tok_line, DEBUG3);

		/* 无错,设置新映射号 */
		if (tok_line->err_msg == NULL)
			map_number++;

		fill_ident_line(tuple_store, tupdesc, map_number,
						tok_line->file_name, tok_line->line_num,
						identline, tok_line->err_msg);
	}

	/* 释放分词器内存 */
	free_auth_file(file, 0);
	/* 清 parse_ident_line 内存 */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(identcxt);
}

/*
 * SQL 可访问的 SRF,返回 pg_ident.conf 全部条目。
 */
Datum
pg_ident_file_mappings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;

	/*
	 * 构建存放结果行的 tuplestore。必须用 Materialize
	 * 模式以防游标打开期间 HBA 文件变更。也比每次
	 * 在已解析列表中查找当前位置更
	 * 高效。
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* 填充 tuplestore */
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	fill_ident_view(rsi->setResult, rsi->setDesc);

	PG_RETURN_NULL();
}
