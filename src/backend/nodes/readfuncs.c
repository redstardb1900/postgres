/*-------------------------------------------------------------------------
 *
 * readfuncs.c
 *	  Reader functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/readfuncs.c
 *
 * NOTES
 *	  Parse location fields are written out by outfuncs.c, but only for
 *	  debugging use.  When reading a location field, we normally discard
 *	  the stored value and set the location field to -1 (ie, "unknown").
 *	  This is because nodes coming from a stored rule should not be thought
 *	  to have a known location in the current query's text.
 *
 *	  However, if restore_location_fields is true, we do restore location
 *	  fields from the string.  This is currently intended only for use by the
 *	  WRITE_READ_PARSE_PLAN_TREES test code, which doesn't want to cause
 *	  any change in the node contents.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/readfuncs.h"


/*
 * 简化各类字段读取的宏。尽可能使用它们，
 * 以减少低级笔误的机会。注意，这些宏把
 * Read 例程中局部变量名的约定
 * 写死了。
 */

/* 声明适当局部变量的宏 */

/* 有少数只需要 local_node */
#define READ_LOCALS_NO_FIELDS(nodeTypeName) \
	nodeTypeName *local_node = makeNode(nodeTypeName)

/* 还有少数只需要 pg_strtok 的支持字段 */
#define READ_TEMP_LOCALS()	\
	const char *token;		\
	int			length

/* ……但大多数两者都需要 */
#define READ_LOCALS(nodeTypeName)			\
	READ_LOCALS_NO_FIELDS(nodeTypeName);	\
	READ_TEMP_LOCALS()

/* 读取整数字段（任何写成 ":fldname %d" 的内容） */
#define READ_INT_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = atoi(token)

/* 读取无符号整数字段（任何写成 ":fldname %u" 的内容） */
#define READ_UINT_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = atoui(token)

/* 读取无符号整数字段（任何用 UINT64_FORMAT 写出的内容） */
#define READ_UINT64_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = strtou64(token, NULL, 10)

/* 读取长整数字段（任何写成 ":fldname %ld" 的内容） */
#define READ_LONG_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = atol(token)

/* 读取 OID 字段（不要把 OID 与 uint 相同这一假设写死） */
#define READ_OID_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = atooid(token)

/* 读取 char 字段（即一个 ascii 字符） */
#define READ_CHAR_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	/* 避免为单个字符调用 debackslash() 的开销 */ \
	local_node->fldname = (length == 0) ? '\0' : (token[0] == '\\' ? token[1] : token[0])

/* 读取写成整数代码的枚举类型字段 */
#define READ_ENUM_FIELD(fldname, enumtype) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = (enumtype) atoi(token)

/* 读取浮点字段 */
#define READ_FLOAT_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = atof(token)

/* 读取布尔字段 */
#define READ_BOOL_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = strtobool(token)

/* 读取字符串字段 */
#define READ_STRING_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = nullable_string(token, length)

/* 读取解析位置字段（并可能丢弃该值） */
#ifdef WRITE_READ_PARSE_PLAN_TREES
#define READ_LOCATION_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	local_node->fldname = restore_location_fields ? atoi(token) : -1
#else
#define READ_LOCATION_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	token = pg_strtok(&length);		/* 取字段值 */ \
	(void) token;				/* 以防别处未使用 */ \
	local_node->fldname = -1	/* 把字段设为 "未知" */
#endif

/* 读取 Node 字段 */
#define READ_NODE_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	(void) token;				/* 以防别处未使用 */ \
	local_node->fldname = nodeRead(NULL, 0)

/* 读取位图集字段 */
#define READ_BITMAPSET_FIELD(fldname) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	(void) token;				/* 以防别处未使用 */ \
	local_node->fldname = _readBitmapset()

/* 读取属性号数组 */
#define READ_ATTRNUMBER_ARRAY(fldname, len) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	local_node->fldname = readAttrNumberCols(len)

/* 读取 oid 数组 */
#define READ_OID_ARRAY(fldname, len) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	local_node->fldname = readOidCols(len)

/* 读取 int 数组 */
#define READ_INT_ARRAY(fldname, len) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	local_node->fldname = readIntCols(len)

/* 读取 bool 数组 */
#define READ_BOOL_ARRAY(fldname, len) \
	token = pg_strtok(&length);		/* 跳过 :fldname */ \
	local_node->fldname = readBoolCols(len)

/* 例程出口 */
#define READ_DONE() \
	return local_node


/*
 * 注意：用 atoi() 读取以 %d 写出的值，或用 atoui() 读取
 * outfuncs.c 中以 %u 写出的值。例外是 OID 值，
 * 对它们使用 atooid()。（自 7.1 起，outfuncs.c 把 OIDs 写成 %u，
 * 但这将来大概会变。）
 */
#define atoui(x)  ((unsigned int) strtoul((x), NULL, 10))

#define strtobool(x)  ((*(x) == 't') ? true : false)

static char *
nullable_string(const char *token, int length)
{
	/* outToken 对 NULL 输出 <>，pg_strtok 把它变成空字符串 */
	if (length == 0)
		return NULL;
	/* outToken 对空字符串输出 "" */
	if (length == 2 && token[0] == '"' && token[1] == '"')
		return pstrdup("");
	/* 否则，我们必须去掉 outToken 加上的保护性反斜杠 */
	return debackslash(token, length);
}


/*
 * _readBitmapset
 *
 * 注意：这段代码用于已知预期是 Bitmapset 的上下文。
 * 在其他上下文里遇到 Bitmapset 时，nodeRead() 中有等价代码
 * 可以读取。
 */
static Bitmapset *
_readBitmapset(void)
{
	Bitmapset  *result = NULL;

	READ_TEMP_LOCALS();

	token = pg_strtok(&length);
	if (token == NULL)
		elog(ERROR, "incomplete Bitmapset structure");
	if (length != 1 || token[0] != '(')
		elog(ERROR, "unrecognized token: \"%.*s\"", length, token);

	token = pg_strtok(&length);
	if (token == NULL)
		elog(ERROR, "incomplete Bitmapset structure");
	if (length != 1 || token[0] != 'b')
		elog(ERROR, "unrecognized token: \"%.*s\"", length, token);

	for (;;)
	{
		int			val;
		char	   *endptr;

		token = pg_strtok(&length);
		if (token == NULL)
			elog(ERROR, "unterminated Bitmapset structure");
		if (length == 1 && token[0] == ')')
			break;
		val = (int) strtol(token, &endptr, 10);
		if (endptr != token + length)
			elog(ERROR, "unrecognized integer: \"%.*s\"", length, token);
		result = bms_add_member(result, val);
	}

	return result;
}

/*
 * 我们导出此函数，供定义可扩展节点的扩展使用。
 * 不过这多少是历史原因，因为调用 nodeRead() 也能工作。
 */
Bitmapset *
readBitmapset(void)
{
	return _readBitmapset();
}

#include "readfuncs.funcs.c"


/*
 * 带 custom_read_write 属性或
 * special_read_write 属性的节点的支持函数
 */

static Const *
_readConst(void)
{
	READ_LOCALS(Const);

	READ_OID_FIELD(consttype);
	READ_INT_FIELD(consttypmod);
	READ_OID_FIELD(constcollid);
	READ_INT_FIELD(constlen);
	READ_BOOL_FIELD(constbyval);
	READ_BOOL_FIELD(constisnull);
	READ_LOCATION_FIELD(location);

	token = pg_strtok(&length); /* 跳过 :constvalue */
	if (local_node->constisnull)
		token = pg_strtok(&length); /* 跳过 "<>" */
	else
		local_node->constvalue = readDatum(local_node->constbyval);

	READ_DONE();
}

static BoolExpr *
_readBoolExpr(void)
{
	READ_LOCALS(BoolExpr);

	/* 自行处理的枚举表示 */
	token = pg_strtok(&length); /* 跳过 :boolop */
	token = pg_strtok(&length); /* 取字段值 */
	if (length == 3 && strncmp(token, "and", 3) == 0)
		local_node->boolop = AND_EXPR;
	else if (length == 2 && strncmp(token, "or", 2) == 0)
		local_node->boolop = OR_EXPR;
	else if (length == 3 && strncmp(token, "not", 3) == 0)
		local_node->boolop = NOT_EXPR;
	else
		elog(ERROR, "unrecognized boolop \"%.*s\"", length, token);

	READ_NODE_FIELD(args);
	READ_LOCATION_FIELD(location);

	READ_DONE();
}

static A_Const *
_readA_Const(void)
{
	READ_LOCALS(A_Const);

	/* 这里我们预期要么是 NULL，要么是 :val */
	token = pg_strtok(&length);
	if (length == 4 && strncmp(token, "NULL", 4) == 0)
		local_node->isnull = true;
	else
	{
		union ValUnion *tmp = nodeRead(NULL, 0);

		/* 为避免 valgrind 抱怨，只复制有效数据 */
		switch (nodeTag(tmp))
		{
			case T_Integer:
				memcpy(&local_node->val, tmp, sizeof(Integer));
				break;
			case T_Float:
				memcpy(&local_node->val, tmp, sizeof(Float));
				break;
			case T_Boolean:
				memcpy(&local_node->val, tmp, sizeof(Boolean));
				break;
			case T_String:
				memcpy(&local_node->val, tmp, sizeof(String));
				break;
			case T_BitString:
				memcpy(&local_node->val, tmp, sizeof(BitString));
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(tmp));
				break;
		}
	}

	READ_LOCATION_FIELD(location);

	READ_DONE();
}

/*
 * _readConstraint
 */
static Constraint *
_readConstraint(void)
{
	READ_LOCALS(Constraint);

	READ_STRING_FIELD(conname);
	READ_BOOL_FIELD(deferrable);
	READ_BOOL_FIELD(initdeferred);
	READ_LOCATION_FIELD(location);

	token = pg_strtok(&length); /* 跳过 :contype */
	token = pg_strtok(&length); /* 取字段值 */
	if (length == 4 && strncmp(token, "NULL", 4) == 0)
		local_node->contype = CONSTR_NULL;
	else if (length == 8 && strncmp(token, "NOT_NULL", 8) == 0)
		local_node->contype = CONSTR_NOTNULL;
	else if (length == 7 && strncmp(token, "DEFAULT", 7) == 0)
		local_node->contype = CONSTR_DEFAULT;
	else if (length == 8 && strncmp(token, "IDENTITY", 8) == 0)
		local_node->contype = CONSTR_IDENTITY;
	else if (length == 9 && strncmp(token, "GENERATED", 9) == 0)
		local_node->contype = CONSTR_GENERATED;
	else if (length == 5 && strncmp(token, "CHECK", 5) == 0)
		local_node->contype = CONSTR_CHECK;
	else if (length == 11 && strncmp(token, "PRIMARY_KEY", 11) == 0)
		local_node->contype = CONSTR_PRIMARY;
	else if (length == 6 && strncmp(token, "UNIQUE", 6) == 0)
		local_node->contype = CONSTR_UNIQUE;
	else if (length == 9 && strncmp(token, "EXCLUSION", 9) == 0)
		local_node->contype = CONSTR_EXCLUSION;
	else if (length == 11 && strncmp(token, "FOREIGN_KEY", 11) == 0)
		local_node->contype = CONSTR_FOREIGN;
	else if (length == 15 && strncmp(token, "ATTR_DEFERRABLE", 15) == 0)
		local_node->contype = CONSTR_ATTR_DEFERRABLE;
	else if (length == 19 && strncmp(token, "ATTR_NOT_DEFERRABLE", 19) == 0)
		local_node->contype = CONSTR_ATTR_NOT_DEFERRABLE;
	else if (length == 13 && strncmp(token, "ATTR_DEFERRED", 13) == 0)
		local_node->contype = CONSTR_ATTR_DEFERRED;
	else if (length == 14 && strncmp(token, "ATTR_IMMEDIATE", 14) == 0)
		local_node->contype = CONSTR_ATTR_IMMEDIATE;

	switch (local_node->contype)
	{
		case CONSTR_NULL:
		case CONSTR_NOTNULL:
			/* 没有额外字段 */
			break;

		case CONSTR_DEFAULT:
			READ_NODE_FIELD(raw_expr);
			READ_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_IDENTITY:
			READ_NODE_FIELD(options);
			READ_CHAR_FIELD(generated_when);
			break;

		case CONSTR_GENERATED:
			READ_NODE_FIELD(raw_expr);
			READ_STRING_FIELD(cooked_expr);
			READ_CHAR_FIELD(generated_when);
			break;

		case CONSTR_CHECK:
			READ_BOOL_FIELD(is_no_inherit);
			READ_NODE_FIELD(raw_expr);
			READ_STRING_FIELD(cooked_expr);
			READ_BOOL_FIELD(skip_validation);
			READ_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_PRIMARY:
			READ_NODE_FIELD(keys);
			READ_NODE_FIELD(including);
			READ_NODE_FIELD(options);
			READ_STRING_FIELD(indexname);
			READ_STRING_FIELD(indexspace);
			READ_BOOL_FIELD(reset_default_tblspc);
			/* access_method 和 where_clause 目前未使用 */
			break;

		case CONSTR_UNIQUE:
			READ_BOOL_FIELD(nulls_not_distinct);
			READ_NODE_FIELD(keys);
			READ_NODE_FIELD(including);
			READ_NODE_FIELD(options);
			READ_STRING_FIELD(indexname);
			READ_STRING_FIELD(indexspace);
			READ_BOOL_FIELD(reset_default_tblspc);
			/* access_method 和 where_clause 目前未使用 */
			break;

		case CONSTR_EXCLUSION:
			READ_NODE_FIELD(exclusions);
			READ_NODE_FIELD(including);
			READ_NODE_FIELD(options);
			READ_STRING_FIELD(indexname);
			READ_STRING_FIELD(indexspace);
			READ_BOOL_FIELD(reset_default_tblspc);
			READ_STRING_FIELD(access_method);
			READ_NODE_FIELD(where_clause);
			break;

		case CONSTR_FOREIGN:
			READ_NODE_FIELD(pktable);
			READ_NODE_FIELD(fk_attrs);
			READ_NODE_FIELD(pk_attrs);
			READ_CHAR_FIELD(fk_matchtype);
			READ_CHAR_FIELD(fk_upd_action);
			READ_CHAR_FIELD(fk_del_action);
			READ_NODE_FIELD(fk_del_set_cols);
			READ_NODE_FIELD(old_conpfeqop);
			READ_OID_FIELD(old_pktable_oid);
			READ_BOOL_FIELD(skip_validation);
			READ_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			/* 没有额外字段 */
			break;

		default:
			elog(ERROR, "unrecognized ConstrType: %d", (int) local_node->contype);
			break;
	}

	READ_DONE();
}

static RangeTblEntry *
_readRangeTblEntry(void)
{
	READ_LOCALS(RangeTblEntry);

	/* 把 alias 和 eref 放前面，使转储更易读 */
	READ_NODE_FIELD(alias);
	READ_NODE_FIELD(eref);
	READ_ENUM_FIELD(rtekind, RTEKind);

	switch (local_node->rtekind)
	{
		case RTE_RELATION:
			READ_OID_FIELD(relid);
			READ_CHAR_FIELD(relkind);
			READ_INT_FIELD(rellockmode);
			READ_NODE_FIELD(tablesample);
			READ_UINT_FIELD(perminfoindex);
			break;
		case RTE_SUBQUERY:
			READ_NODE_FIELD(subquery);
			READ_BOOL_FIELD(security_barrier);
			/* 我们也复用这些 RELATION 字段： */
			READ_OID_FIELD(relid);
			READ_CHAR_FIELD(relkind);
			READ_INT_FIELD(rellockmode);
			READ_UINT_FIELD(perminfoindex);
			break;
		case RTE_JOIN:
			READ_ENUM_FIELD(jointype, JoinType);
			READ_INT_FIELD(joinmergedcols);
			READ_NODE_FIELD(joinaliasvars);
			READ_NODE_FIELD(joinleftcols);
			READ_NODE_FIELD(joinrightcols);
			READ_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			READ_NODE_FIELD(functions);
			READ_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			READ_NODE_FIELD(tablefunc);
			/* RTE 若有列类型信息，必须有一份副本 */
			if (local_node->tablefunc)
			{
				TableFunc  *tf = local_node->tablefunc;

				local_node->coltypes = tf->coltypes;
				local_node->coltypmods = tf->coltypmods;
				local_node->colcollations = tf->colcollations;
			}
			break;
		case RTE_VALUES:
			READ_NODE_FIELD(values_lists);
			READ_NODE_FIELD(coltypes);
			READ_NODE_FIELD(coltypmods);
			READ_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			READ_STRING_FIELD(ctename);
			READ_UINT_FIELD(ctelevelsup);
			READ_BOOL_FIELD(self_reference);
			READ_NODE_FIELD(coltypes);
			READ_NODE_FIELD(coltypmods);
			READ_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			READ_STRING_FIELD(enrname);
			READ_FLOAT_FIELD(enrtuples);
			READ_NODE_FIELD(coltypes);
			READ_NODE_FIELD(coltypmods);
			READ_NODE_FIELD(colcollations);
			/* 我们也复用这些 RELATION 字段： */
			READ_OID_FIELD(relid);
			break;
		case RTE_RESULT:
			/* 没有额外字段 */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) local_node->rtekind);
			break;
	}

	READ_BOOL_FIELD(lateral);
	READ_BOOL_FIELD(inh);
	READ_BOOL_FIELD(inFromCl);
	READ_NODE_FIELD(securityQuals);

	READ_DONE();
}

static A_Expr *
_readA_Expr(void)
{
	READ_LOCALS(A_Expr);

	token = pg_strtok(&length);

	if (length == 3 && strncmp(token, "ANY", 3) == 0)
	{
		local_node->kind = AEXPR_OP_ANY;
		READ_NODE_FIELD(name);
	}
	else if (length == 3 && strncmp(token, "ALL", 3) == 0)
	{
		local_node->kind = AEXPR_OP_ALL;
		READ_NODE_FIELD(name);
	}
	else if (length == 8 && strncmp(token, "DISTINCT", 8) == 0)
	{
		local_node->kind = AEXPR_DISTINCT;
		READ_NODE_FIELD(name);
	}
	else if (length == 12 && strncmp(token, "NOT_DISTINCT", 12) == 0)
	{
		local_node->kind = AEXPR_NOT_DISTINCT;
		READ_NODE_FIELD(name);
	}
	else if (length == 6 && strncmp(token, "NULLIF", 6) == 0)
	{
		local_node->kind = AEXPR_NULLIF;
		READ_NODE_FIELD(name);
	}
	else if (length == 2 && strncmp(token, "IN", 2) == 0)
	{
		local_node->kind = AEXPR_IN;
		READ_NODE_FIELD(name);
	}
	else if (length == 4 && strncmp(token, "LIKE", 4) == 0)
	{
		local_node->kind = AEXPR_LIKE;
		READ_NODE_FIELD(name);
	}
	else if (length == 5 && strncmp(token, "ILIKE", 5) == 0)
	{
		local_node->kind = AEXPR_ILIKE;
		READ_NODE_FIELD(name);
	}
	else if (length == 7 && strncmp(token, "SIMILAR", 7) == 0)
	{
		local_node->kind = AEXPR_SIMILAR;
		READ_NODE_FIELD(name);
	}
	else if (length == 7 && strncmp(token, "BETWEEN", 7) == 0)
	{
		local_node->kind = AEXPR_BETWEEN;
		READ_NODE_FIELD(name);
	}
	else if (length == 11 && strncmp(token, "NOT_BETWEEN", 11) == 0)
	{
		local_node->kind = AEXPR_NOT_BETWEEN;
		READ_NODE_FIELD(name);
	}
	else if (length == 11 && strncmp(token, "BETWEEN_SYM", 11) == 0)
	{
		local_node->kind = AEXPR_BETWEEN_SYM;
		READ_NODE_FIELD(name);
	}
	else if (length == 15 && strncmp(token, "NOT_BETWEEN_SYM", 15) == 0)
	{
		local_node->kind = AEXPR_NOT_BETWEEN_SYM;
		READ_NODE_FIELD(name);
	}
	else if (length == 5 && strncmp(token, ":name", 5) == 0)
	{
		local_node->kind = AEXPR_OP;
		local_node->name = nodeRead(NULL, 0);
	}
	else
		elog(ERROR, "unrecognized A_Expr kind: \"%.*s\"", length, token);

	READ_NODE_FIELD(lexpr);
	READ_NODE_FIELD(rexpr);
	READ_LOCATION_FIELD(location);

	READ_DONE();
}

static ExtensibleNode *
_readExtensibleNode(void)
{
	const ExtensibleNodeMethods *methods;
	ExtensibleNode *local_node;
	const char *extnodename;

	READ_TEMP_LOCALS();

	token = pg_strtok(&length); /* 跳过 :extnodename */
	token = pg_strtok(&length); /* 取 extnodename */

	extnodename = nullable_string(token, length);
	if (!extnodename)
		elog(ERROR, "extnodename has to be supplied");
	methods = GetExtensibleNodeMethods(extnodename, false);

	local_node = (ExtensibleNode *) newNode(methods->node_size,
											T_ExtensibleNode);
	local_node->extnodename = extnodename;

	/* 反序列化私有字段 */
	methods->nodeRead(local_node);

	READ_DONE();
}


/*
 * parseNodeString
 *
 * 给定表示节点树的字符串，parseNodeString 创建
 * 内部节点结构。
 *
 * 要读的字符串必须已经装入 pg_strtok()。
 */
Node *
parseNodeString(void)
{
	READ_TEMP_LOCALS();

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	token = pg_strtok(&length);

#define MATCH(tokname, namelen) \
	(length == namelen && memcmp(token, tokname, namelen) == 0)

#include "readfuncs.switch.c"

	elog(ERROR, "badly formatted node string \"%.32s\"...", token);
	return NULL;				/* 消除编译器告警 */
}


/*
 * readDatum
 *
 * 给定常量的字符串表示，重建相应的
 * Datum。字符串表示嵌入了长度信息，但不含 byValue，
 * 因此必须由调用方告知。
 */
Datum
readDatum(bool typbyval)
{
	Size		length,
				i;
	int			tokenLength;
	const char *token;
	Datum		res;
	char	   *s;

	/*
	 * 读取该值的实际长度
	 */
	token = pg_strtok(&tokenLength);
	length = atoui(token);

	token = pg_strtok(&tokenLength);	/* 读取 '[' */
	if (token == NULL || token[0] != '[')
		elog(ERROR, "expected \"[\" to start datum, but got \"%s\"; length = %zu",
			 token ? token : "[NULL]", length);

	if (typbyval)
	{
		if (length > (Size) sizeof(Datum))
			elog(ERROR, "byval datum but length = %zu", length);
		res = (Datum) 0;
		s = (char *) (&res);
		for (i = 0; i < (Size) sizeof(Datum); i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
	}
	else if (length <= 0)
		res = (Datum) NULL;
	else
	{
		s = (char *) palloc(length);
		for (i = 0; i < length; i++)
		{
			token = pg_strtok(&tokenLength);
			s[i] = (char) atoi(token);
		}
		res = PointerGetDatum(s);
	}

	token = pg_strtok(&tokenLength);	/* 读取 ']' */
	if (token == NULL || token[0] != ']')
		elog(ERROR, "expected \"]\" to end datum, but got \"%s\"; length = %zu",
			 token ? token : "[NULL]", length);

	return res;
}

/*
 * 标量数组读取函数的公共实现
 *
 * 数据格式要么是 NULL 指针的 "<>"（此时忽略 numCols），
 * 要么是 "(item item item)"，其中项数必须等于
 * numCols。convfunc 必须能在空白或
 * 右括号处停下，因为 pg_strtok 不会给词元加上结尾的空字符。
 */
#define READ_SCALAR_ARRAY(fnname, datatype, convfunc) \
datatype * \
fnname(int numCols) \
{ \
	datatype   *vals; \
	READ_TEMP_LOCALS(); \
	token = pg_strtok(&length); \
	if (token == NULL) \
		elog(ERROR, "incomplete scalar array"); \
	if (length == 0) \
		return NULL;			/* 它是 "<>"，因此返回 NULL 指针 */ \
	if (length != 1 || token[0] != '(') \
		elog(ERROR, "unrecognized token: \"%.*s\"", length, token); \
	vals = (datatype *) palloc(numCols * sizeof(datatype)); \
	for (int i = 0; i < numCols; i++) \
	{ \
		token = pg_strtok(&length); \
		if (token == NULL || token[0] == ')') \
			elog(ERROR, "incomplete scalar array"); \
		vals[i] = convfunc(token); \
	} \
	token = pg_strtok(&length); \
	if (token == NULL || length != 1 || token[0] != ')') \
		elog(ERROR, "incomplete scalar array"); \
	return vals; \
}

/*
 * 注意：这些函数在 nodes.h 中导出，扩展可能会使用，
 * 因此不要过度改动它们的名字或 API。
 */
READ_SCALAR_ARRAY(readAttrNumberCols, int16, atoi)
READ_SCALAR_ARRAY(readOidCols, Oid, atooid)
/* outfuncs.c 有 writeIndexCols，但这里还不需要 */
/* READ_SCALAR_ARRAY(readIndexCols, Index, atoui) */
READ_SCALAR_ARRAY(readIntCols, int, atoi)
READ_SCALAR_ARRAY(readBoolCols, bool, strtobool)
