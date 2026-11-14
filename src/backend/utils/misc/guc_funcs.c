/*--------------------------------------------------------------------
 *
 * guc_funcs.c
 *
 * 与 GUC 变量相关的 SQL 命令及可从 SQL 访问的函数。
 *
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_funcs.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_parameter_acl.h"
#include "funcapi.h"
#include "guc_internal.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/snapmgr.h"

static char *flatten_set_variable_args(const char *name, List *args);
static void ShowGUCConfigOption(const char *name, DestReceiver *dest);
static void ShowAllGUCConfig(DestReceiver *dest);


/*
 * SET 命令
 */
void
ExecSetVariableStmt(VariableSetStmt *stmt, bool isTopLevel)
{
	GucAction	action = stmt->is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET;

	/*
	 * worker 进程会在并行操作开始时同步这些参数；
	 * 随后在操作期间，我们阻止 SET。
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot set parameters during a parallel operation")));

	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
		case VAR_SET_CURRENT:
			if (stmt->is_local)
				WarnNoTransactionBlock(isTopLevel, "SET LOCAL");
			(void) set_config_option(stmt->name,
									 ExtractSetVariableArgs(stmt),
									 (superuser() ? PGC_SUSET : PGC_USERSET),
									 PGC_S_SESSION,
									 action, true, 0, false);
			break;
		case VAR_SET_MULTI:

			/*
			 * 特殊的 SQL 语法。TRANSACTION 与 SESSION
			 * CHARACTERISTICS 实际每条语句设置多个变量。
			 * TRANSACTION SNAPSHOT 只接受一个参数，
			 * 但我们仍把它放这里，因为它是特殊情形，
			 * 与 GUC 变量无关。
			 */
			if (strcmp(stmt->name, "TRANSACTION") == 0)
			{
				ListCell   *head;

				WarnNoTransactionBlock(isTopLevel, "SET TRANSACTION");

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_deferrable") == 0)
						SetPGVariable("transaction_deferrable",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET TRANSACTION element: %s",
							 item->defname);
				}
			}
			else if (strcmp(stmt->name, "SESSION CHARACTERISTICS") == 0)
			{
				ListCell   *head;

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("default_transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("default_transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_deferrable") == 0)
						SetPGVariable("default_transaction_deferrable",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET SESSION element: %s",
							 item->defname);
				}
			}
			else if (strcmp(stmt->name, "TRANSACTION SNAPSHOT") == 0)
			{
				A_Const    *con = linitial_node(A_Const, stmt->args);

				if (stmt->is_local)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("SET LOCAL TRANSACTION SNAPSHOT is not implemented")));

				WarnNoTransactionBlock(isTopLevel, "SET TRANSACTION");
				ImportSnapshot(strVal(&con->val));
			}
			else
				elog(ERROR, "unexpected SET MULTI element: %s",
					 stmt->name);
			break;
		case VAR_SET_DEFAULT:
			if (stmt->is_local)
				WarnNoTransactionBlock(isTopLevel, "SET LOCAL");
			/* fall through */
		case VAR_RESET:
			(void) set_config_option(stmt->name,
									 NULL,
									 (superuser() ? PGC_SUSET : PGC_USERSET),
									 PGC_S_SESSION,
									 action, true, 0, false);
			break;
		case VAR_RESET_ALL:
			ResetAllOptions();
			break;
	}

	/* 按名字为本次 GUC 变量设置调用 post-alter 钩子。 */
	InvokeObjectPostAlterHookArgStr(ParameterAclRelationId, stmt->name,
									ACL_SET, stmt->kind, false);
}

/*
 * 获取 VariableSetStmt 要赋的值，RESET 时为 NULL。
 * 结果是 palloc 的。
 *
 * 导出供 ALTER ROLE SET 之类的操作使用。
 */
char *
ExtractSetVariableArgs(VariableSetStmt *stmt)
{
	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
			return flatten_set_variable_args(stmt->name, stmt->args);
		case VAR_SET_CURRENT:
			return GetConfigOptionByName(stmt->name, NULL, false);
		default:
			return NULL;
	}
}

/*
 * flatten_set_variable_args
 *		给定文法为 SET 产生的 parsenode List，
 *		转换成 GUC 使用的扁平字符串表示。
 *
 * 我们需要被告知这些参数所属变量的名字，因为
 * 扁平化规则各异，唉。
 *
 * 若 args 为 NIL（即 SET ... TO DEFAULT）则结果为 NULL，否则
 * 为 palloc 串。
 */
static char *
flatten_set_variable_args(const char *name, List *args)
{
	struct config_generic *record;
	int			flags;
	StringInfoData buf;
	ListCell   *l;

	/* 仅 DEFAULT 快速路径 */
	if (args == NIL)
		return NULL;

	/*
	 * 取该变量的标志；若未知，则用默认标志。
	 * （调用者稍后可能报错，但这不归我们管。）
	 */
	record = find_option(name, false, true, WARNING);
	if (record)
		flags = record->flags;
	else
		flags = 0;

	/* 列表输入且变量非列表型时报错 */
	if ((flags & GUC_LIST_INPUT) == 0 &&
		list_length(args) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("SET %s takes only one argument", name)));

	initStringInfo(&buf);

	/*
	 * 每个列表成员可以是普通 A_Const 节点，
	 * 或是 TypeCast 内的 A_Const；后者仅对 ConstInterval 参数
	 * 即 SET TIME ZONE。
	 */
	foreach(l, args)
	{
		Node	   *arg = (Node *) lfirst(l);
		char	   *val;
		TypeName   *typeName = NULL;
		A_Const    *con;

		if (l != list_head(args))
			appendStringInfoString(&buf, ", ");

		if (IsA(arg, TypeCast))
		{
			TypeCast   *tc = (TypeCast *) arg;

			arg = tc->arg;
			typeName = tc->typeName;
		}

		if (!IsA(arg, A_Const))
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(arg));
		con = (A_Const *) arg;

		switch (nodeTag(&con->val))
		{
			case T_Integer:
				appendStringInfo(&buf, "%d", intVal(&con->val));
				break;
			case T_Float:
				/* 以字符串表示，因此直接复制 */
				appendStringInfoString(&buf, castNode(Float, &con->val)->fval);
				break;
			case T_String:
				val = strVal(&con->val);
				if (typeName != NULL)
				{
					/*
					 * TIME ZONE 情形必须是 ConstInterval 参数。先
					 * 转成 interval 再转回，以规范化值并
					 * 考虑 typmod
					 */
					Oid			typoid;
					int32		typmod;
					Datum		interval;
					char	   *intervalout;

					typenameTypeIdAndMod(NULL, typeName, &typoid, &typmod);
					Assert(typoid == INTERVALOID);

					interval =
						DirectFunctionCall3(interval_in,
											CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
											Int32GetDatum(typmod));

					intervalout =
						DatumGetCString(DirectFunctionCall1(interval_out,
															interval));
					appendStringInfo(&buf, "INTERVAL '%s'", intervalout);
				}
				else
				{
					/*
					 * 字符串字面量或标识符。引号模式下，
					 * 若非普通标识符则给它加引号。
					 */
					if (flags & GUC_LIST_QUOTE)
						appendStringInfoString(&buf, quote_identifier(val));
					else
						appendStringInfoString(&buf, val);
				}
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&con->val));
				break;
		}
	}

	return buf.data;
}

/*
 * SetPGVariable - 以易于 C 调用的函数导出的 SET 命令。
 *
 * 它提供 SET TO value 以及 SET TO DEFAULT（通过传递
 * args == NIL 来表达），但不提供 SET FROM CURRENT。
 */
void
SetPGVariable(const char *name, List *args, bool is_local)
{
	char	   *argstring = flatten_set_variable_args(name, args);

	/* 注意 SET DEFAULT（argstring == NULL）等价于 RESET */
	(void) set_config_option(name,
							 argstring,
							 (superuser() ? PGC_SUSET : PGC_USERSET),
							 PGC_S_SESSION,
							 is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
							 true, 0, false);
}

/*
 * 封装为 SQL 可调用函数的 SET 命令。
 */
Datum
set_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *value;
	char	   *new_value;
	bool		is_local;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("SET requires parameter name")));

	/* 获取 GUC 变量名 */
	name = TextDatumGetCString(PG_GETARG_DATUM(0));

	/* 获取期望的值；重置请求则设为 NULL */
	if (PG_ARGISNULL(1))
		value = NULL;
	else
		value = TextDatumGetCString(PG_GETARG_DATUM(1));

	/*
	 * 获取 is_local 的期望状态。若提供的值是 NULL 则默认
	 * 为假
	 */
	if (PG_ARGISNULL(2))
		is_local = false;
	else
		is_local = PG_GETARG_BOOL(2);

	/* 注意 SET DEFAULT（argstring == NULL）等价于 RESET */
	(void) set_config_option(name,
							 value,
							 (superuser() ? PGC_SUSET : PGC_USERSET),
							 PGC_S_SESSION,
							 is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
							 true, 0, false);

	/* 取新的当前值 */
	new_value = GetConfigOptionByName(name, NULL, false);

	/* 将返回字符串转为 text */
	PG_RETURN_TEXT_P(cstring_to_text(new_value));
}


/*
 * SHOW 命令
 */
void
GetPGVariable(const char *name, DestReceiver *dest)
{
	if (guc_name_compare(name, "all") == 0)
		ShowAllGUCConfig(dest);
	else
		ShowGUCConfigOption(name, dest);
}

/*
 * 为 SHOW 的结果获取元组描述符
 */
TupleDesc
GetPGVariableResultDesc(const char *name)
{
	TupleDesc	tupdesc;

	if (guc_name_compare(name, "all") == 0)
	{
		/* 需要代表三个 TEXT 列的元组描述符 */
		tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "description",
						   TEXTOID, -1, 0);
	}
	else
	{
		const char *varname;

		/* 获取名字的规范拼写 */
		(void) GetConfigOptionByName(name, &varname, false);

		/* 需要代表单个 TEXT 列的元组描述符 */
		tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
						   TEXTOID, -1, 0);
	}
	return tupdesc;
}

/*
 * SHOW 单个变量
 */
static void
ShowGUCConfigOption(const char *name, DestReceiver *dest)
{
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	const char *varname;
	char	   *value;

	/* 获取值以及名字的规范拼写 */
	value = GetConfigOptionByName(name, &varname, false);

	/* 需要代表单个 TEXT 列的元组描述符 */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, varname,
							  TEXTOID, -1, 0);

	/* 准备元组的投影输出 */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* 发送 */
	do_text_output_oneline(tstate, value);

	end_tup_output(tstate);
}

/*
 * SHOW ALL 命令
 */
static void
ShowAllGUCConfig(DestReceiver *dest)
{
	struct config_generic **guc_vars;
	int			num_vars;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		isnull[3] = {false, false, false};

	/* 按排序次序收集所有变量 */
	guc_vars = get_guc_variables(&num_vars);

	/* 需要代表三个 TEXT 列的元组描述符 */
	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "setting",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "description",
							  TEXTOID, -1, 0);

	/* 准备元组的投影输出 */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	for (int i = 0; i < num_vars; i++)
	{
		struct config_generic *conf = guc_vars[i];
		char	   *setting;

		/* NO_SHOW_ALL 则跳过 */
		if (conf->flags & GUC_NO_SHOW_ALL)
			continue;

		/* 只返回当前用户可见的选项 */
		if (!ConfigOptionIsVisible(conf))
			continue;

		/* 赋给 values 数组 */
		values[0] = PointerGetDatum(cstring_to_text(conf->name));

		setting = ShowGUCOption(conf, true);
		if (setting)
		{
			values[1] = PointerGetDatum(cstring_to_text(setting));
			isnull[1] = false;
		}
		else
		{
			values[1] = PointerGetDatum(NULL);
			isnull[1] = true;
		}

		if (conf->short_desc)
		{
			values[2] = PointerGetDatum(cstring_to_text(conf->short_desc));
			isnull[2] = false;
		}
		else
		{
			values[2] = PointerGetDatum(NULL);
			isnull[2] = true;
		}

		/* 送往 dest */
		do_tup_output(tstate, values, isnull);

		/* 清理 */
		pfree(DatumGetPointer(values[0]));
		if (setting)
		{
			pfree(setting);
			pfree(DatumGetPointer(values[1]));
		}
		if (conf->short_desc)
			pfree(DatumGetPointer(values[2]));
	}

	end_tup_output(tstate);
}

/*
 * 以文本数组的形式返回指定 GUC 所带的部分标志，
 * 若 GUC 不存在则返回 NULL。若 GUC 存在但
 * 无有意义的标志时，返回空数组。
 */
Datum
pg_settings_get_flags(PG_FUNCTION_ARGS)
{
#define MAX_GUC_FLAGS	6
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	struct config_generic *record;
	int			cnt = 0;
	Datum		flags[MAX_GUC_FLAGS];
	ArrayType  *a;

	record = find_option(varname, false, true, ERROR);

	/* 若无此变量则返回 NULL */
	if (record == NULL)
		PG_RETURN_NULL();

	if (record->flags & GUC_EXPLAIN)
		flags[cnt++] = CStringGetTextDatum("EXPLAIN");
	if (record->flags & GUC_NO_RESET)
		flags[cnt++] = CStringGetTextDatum("NO_RESET");
	if (record->flags & GUC_NO_RESET_ALL)
		flags[cnt++] = CStringGetTextDatum("NO_RESET_ALL");
	if (record->flags & GUC_NO_SHOW_ALL)
		flags[cnt++] = CStringGetTextDatum("NO_SHOW_ALL");
	if (record->flags & GUC_NOT_IN_SAMPLE)
		flags[cnt++] = CStringGetTextDatum("NOT_IN_SAMPLE");
	if (record->flags & GUC_RUNTIME_COMPUTED)
		flags[cnt++] = CStringGetTextDatum("RUNTIME_COMPUTED");

	Assert(cnt <= MAX_GUC_FLAGS);

	/* 记录以 Datum 返回 */
	a = construct_array_builtin(flags, cnt, TEXTOID);
	PG_RETURN_ARRAYTYPE_P(a);
}

/*
 * 返回 GUC 变量对当前用户是否可见。
 */
bool
ConfigOptionIsVisible(struct config_generic *conf)
{
	if ((conf->flags & GUC_SUPERUSER_ONLY) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_SETTINGS))
		return false;
	else
		return true;
}

/*
 * 抽取给定变量在 pg_settings 中显示的字段。
 */
static void
GetConfigOptionValues(struct config_generic *conf, const char **values)
{
	char		buffer[256];

	/* 先获取通用属性 */

	/* name */
	values[0] = conf->name;

	/* setting：使用 ShowGUCOption 以避免重复该逻辑 */
	values[1] = ShowGUCOption(conf, false);

	/* 单位（如有，NULL） */
	values[2] = get_config_unit_name(conf->flags);

	/* group */
	values[3] = _(config_group_names[conf->group]);

	/* short_desc */
	values[4] = conf->short_desc != NULL ? _(conf->short_desc) : NULL;

	/* extra_desc */
	values[5] = conf->long_desc != NULL ? _(conf->long_desc) : NULL;

	/* context */
	values[6] = GucContext_Names[conf->context];

	/* vartype */
	values[7] = config_type_names[conf->vartype];

	/* source */
	values[8] = GucSource_Names[conf->source];

	/* 现在获取特定类型的属性 */
	switch (conf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *lconf = (struct config_bool *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				values[12] = pstrdup(lconf->boot_val ? "on" : "off");

				/* reset_val */
				values[13] = pstrdup(lconf->reset_val ? "on" : "off");
			}
			break;

		case PGC_INT:
			{
				struct config_int *lconf = (struct config_int *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->boot_val);
				values[12] = pstrdup(buffer);

				/* reset_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->reset_val);
				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *lconf = (struct config_real *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->boot_val);
				values[12] = pstrdup(buffer);

				/* reset_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->reset_val);
				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *lconf = (struct config_string *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				if (lconf->boot_val == NULL)
					values[12] = NULL;
				else
					values[12] = pstrdup(lconf->boot_val);

				/* reset_val */
				if (lconf->reset_val == NULL)
					values[13] = NULL;
				else
					values[13] = pstrdup(lconf->reset_val);
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *lconf = (struct config_enum *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */

				/*
				 * 注意！enumvals 内含双引号时不
				 * 被支持
				 */
				values[11] = config_enum_get_options((struct config_enum *) conf,
													 "{\"", "\"}", "\",\"");

				/* boot_val */
				values[12] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->boot_val));

				/* reset_val */
				values[13] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->reset_val));
			}
			break;

		default:
			{
				/*
				 * 应到不了这里，但万一到了，设为 NULL
				 */

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				values[12] = NULL;

				/* reset_val */
				values[13] = NULL;
			}
			break;
	}

	/*
	 * 若设置来自配置文件，则设置其来源位置。出于
	 * 安全考虑，我们不为权限不足的用户
	 * 显示源文件/行号。
	 */
	if (conf->source == PGC_S_FILE &&
		has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_SETTINGS))
	{
		values[14] = conf->sourcefile;
		snprintf(buffer, sizeof(buffer), "%d", conf->sourceline);
		values[15] = pstrdup(buffer);
	}
	else
	{
		values[14] = NULL;
		values[15] = NULL;
	}

	values[16] = (conf->status & GUC_PENDING_RESTART) ? "t" : "f";
}

/*
 * show_config_by_name - 等价于 SHOW X 命令，但实现为
 * 函数。
 */
Datum
show_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *varval;

	/* 获取该值 */
	varval = GetConfigOptionByName(varname, NULL, false);

	/* 转成 text */
	PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_config_by_name_missing_ok - 等价于 SHOW X 命令，但实现为
 * 一个函数。若 X 不存在且 missing_ok 为真，则抑制错误并
 * 返回 NULL。
 */
Datum
show_config_by_name_missing_ok(PG_FUNCTION_ARGS)
{
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	char	   *varval;

	/* 获取该值 */
	varval = GetConfigOptionByName(varname, NULL, missing_ok);

	/* 若无此变量则返回 NULL */
	if (varval == NULL)
		PG_RETURN_NULL();

	/* 转成 text */
	PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_all_settings - 等价于 SHOW ALL 命令，但实现为
 * 表函数。
 */
#define NUM_PG_SETTINGS_ATTS	17

Datum
show_all_settings(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct config_generic **guc_vars;
	int			num_vars;
	TupleDesc	tupdesc;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* 只在函数首次调用时执行的操作 */
	if (SRF_IS_FIRSTCALL())
	{
		/* 创建用于跨调用持久的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * 需要一个元组描述符，代表 NUM_PG_SETTINGS_ATTS 个
		 * 适当类型的列
		 */
		tupdesc = CreateTemplateTupleDesc(NUM_PG_SETTINGS_ATTS);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unit",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "category",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "short_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "extra_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "context",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "vartype",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "source",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "min_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "max_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "enumvals",
						   TEXTARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "boot_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "reset_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 15, "sourcefile",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 16, "sourceline",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 17, "pending_restart",
						   BOOLOID, -1, 0);

		/*
		 * 生成从原始 C 字符串构造元组所需的属性元数据
		 *
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* 按排序次序收集所有变量 */
		guc_vars = get_guc_variables(&num_vars);

		/* 用 user_fctx 记住数组位置 */
		funcctx->user_fctx = guc_vars;

		/* 要返回的元组总数 */
		funcctx->max_calls = num_vars;

		MemoryContextSwitchTo(oldcontext);
	}

	/* 每次调用函数时执行的操作 */
	funcctx = SRF_PERCALL_SETUP();

	guc_vars = (struct config_generic **) funcctx->user_fctx;
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	while (call_cntr < max_calls)	/* 还有更多待发送时 */
	{
		struct config_generic *conf = guc_vars[call_cntr];
		char	   *values[NUM_PG_SETTINGS_ATTS];
		HeapTuple	tuple;
		Datum		result;

		/* 若标记 NO_SHOW_ALL 或当前用户不可见则跳过 */
		if ((conf->flags & GUC_NO_SHOW_ALL) ||
			!ConfigOptionIsVisible(conf))
		{
			call_cntr = ++funcctx->call_cntr;
			continue;
		}

		/* 抽取当前变量的值 */
		GetConfigOptionValues(conf, (const char **) values);

		/* 构建元组 */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* 把元组变成 datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}

	/* 没有更多剩余时执行 */
	SRF_RETURN_DONE(funcctx);
}

/*
 * show_all_file_settings
 *
 * 返回所有配置文件中全部参数设置的一张表，
 * 包括配置文件路径名、行号、表示设置被遇到次序的
 * 序号，以及参数名和值、
 * 一个表示该值能否被应用的布尔值，以及可能有的
 * 关联的错误消息。（对语法错误之类的问题，
 * 参数名/值可能为 NULL。）
 *
 * 注意：这里不做任何过滤，而是依靠 GRANT 系统
 * 来阻止无特权用户访问此函数或
 * 其上的视图。
 */
Datum
show_all_file_settings(PG_FUNCTION_ARGS)
{
#define NUM_PG_FILE_SETTINGS_ATTS 7
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ConfigVariable *conf;
	int			seqno;

	/* 以当前上下文为工作区扫描配置文件 */
	conf = ProcessConfigFileInternal(PGC_SIGHUP, false, DEBUG3);

	/* 构建用于返回结果的元组存储 */
	InitMaterializedSRF(fcinfo, 0);

	/* 处理结果并创建元组存储 */
	for (seqno = 1; conf != NULL; conf = conf->next, seqno++)
	{
		Datum		values[NUM_PG_FILE_SETTINGS_ATTS];
		bool		nulls[NUM_PG_FILE_SETTINGS_ATTS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* sourcefile */
		if (conf->filename)
			values[0] = PointerGetDatum(cstring_to_text(conf->filename));
		else
			nulls[0] = true;

		/* sourceline（无源文件时无意义） */
		if (conf->filename)
			values[1] = Int32GetDatum(conf->sourceline);
		else
			nulls[1] = true;

		/* seqno */
		values[2] = Int32GetDatum(seqno);

		/* name */
		if (conf->name)
			values[3] = PointerGetDatum(cstring_to_text(conf->name));
		else
			nulls[3] = true;

		/* setting */
		if (conf->value)
			values[4] = PointerGetDatum(cstring_to_text(conf->value));
		else
			nulls[4] = true;

		/* applied */
		values[5] = BoolGetDatum(conf->applied);

		/* error */
		if (conf->errmsg)
			values[6] = PointerGetDatum(cstring_to_text(conf->errmsg));
		else
			nulls[6] = true;

		/* 把行推入元组存储 */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
