/*-------------------------------------------------------------------------
 *
 * outfuncs.c
 *	  Postgres 树节点的输出函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/outfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/attnum.h"
#include "common/shortest_dec.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/datum.h"

static void outChar(StringInfo str, char c);
static void outDouble(StringInfo str, double d);


/*
 * 简化各类字段输出的宏。尽可能使用它们，
 * 以减少低级笔误的机会。注意，这些宏把
 * Out 例程中局部变量名的约定
 * 写死了。
 */

/* 写出节点类型的标签 */
#define WRITE_NODE_TYPE(nodelabel) \
	appendStringInfoString(str, nodelabel)

/* 写出整数字段（任何写成 ":fldname %d" 的内容） */
#define WRITE_INT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* 写出无符号整数字段（任何写成 ":fldname %u" 的内容） */
#define WRITE_UINT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* 写出无符号整数字段（任何用 UINT64_FORMAT 写出的内容） */
#define WRITE_UINT64_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " " UINT64_FORMAT, \
					 node->fldname)

/* 写出 OID 字段（不要把 OID 与 uint 相同这一假设写死） */
#define WRITE_OID_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* 写出长整数字段 */
#define WRITE_LONG_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %ld", node->fldname)

/* 写出 char 字段（即一个 ascii 字符） */
#define WRITE_CHAR_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outChar(str, node->fldname))

/* 把枚举类型字段写成整数代码 */
#define WRITE_ENUM_FIELD(fldname, enumtype) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", \
					 (int) node->fldname)

/* 写出浮点字段（实际上是 double） */
#define WRITE_FLOAT_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outDouble(str, node->fldname))

/* 写出布尔字段 */
#define WRITE_BOOL_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %s", \
					 booltostr(node->fldname))

/* 写出字符串字段（可能为 NULL） */
#define WRITE_STRING_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outToken(str, node->fldname))

/* 写出解析位置字段（实际上与 INT 情形相同） */
#define WRITE_LOCATION_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* 写出 Node 字段 */
#define WRITE_NODE_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outNode(str, node->fldname))

/* 写出位图集字段 */
#define WRITE_BITMAPSET_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outBitmapset(str, node->fldname))

/* 写出 Node 指针的变长数组（不是 List） */
#define WRITE_NODE_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeNodeArray(str, (const Node * const *) node->fldname, len))

/* 写出 AttrNumber 的变长数组 */
#define WRITE_ATTRNUMBER_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeAttrNumberCols(str, node->fldname, len))

/* 写出 Oid 的变长数组 */
#define WRITE_OID_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeOidCols(str, node->fldname, len))

/* 写出 Index 的变长数组 */
#define WRITE_INDEX_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIndexCols(str, node->fldname, len))

/* 写出 int 的变长数组 */
#define WRITE_INT_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIntCols(str, node->fldname, len))

/* 写出 bool 的变长数组 */
#define WRITE_BOOL_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeBoolCols(str, node->fldname, len))

#define booltostr(x)  ((x) ? "true" : "false")


/*
 * outToken
 *	  把普通字符串（例如标识符）转换成一种形式，
 *	  使它会被 read.c 的函数解码回普通词元。
 *
 *	  若给定空指针，则编码为 '<>'。
 *	  空字符串编码为 '""'。为避免歧义，以
 *	  '<' 或 '"' 开头的输入字符串会加上前导反斜杠。
 */
void
outToken(StringInfo str, const char *s)
{
	if (s == NULL)
	{
		appendStringInfoString(str, "<>");
		return;
	}
	if (*s == '\0')
	{
		appendStringInfoString(str, "\"\"");
		return;
	}

	/*
	 * 查找被 read.c 特殊处理的字符或模式
	 * （在 pg_strtok() 或 nodeRead() 中），因此需要
	 * 保护性反斜杠。
	 */
	/* 这些字符只需要在字符串开头加引号 */
	if (*s == '<' ||
		*s == '"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* 这些字符在字符串中任何位置都必须加反斜杠 */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

/*
 * 转换一个字符。经由 outToken()，以便特殊字符
 * 被转义。
 */
static void
outChar(StringInfo str, char c)
{
	char		in[2];

	/* 历来我们把 \0 表示为 <>，因此继续这样做 */
	if (c == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}

	in[0] = c;
	in[1] = '\0';

	outToken(str, in);
}

/*
 * 转换一个 double 值，尽量保证该值被精确保留。
 */
static void
outDouble(StringInfo str, double d)
{
	char		buf[DOUBLE_SHORTEST_DECIMAL_LEN];

	double_to_shortest_decimal_buf(d, buf);
	appendStringInfoString(str, buf);
}

/*
 * 标量数组写出函数的公共实现
 *
 * 数据格式要么是 NULL 指针的 "<>"，要么是 "(item item item)"。
 * fmtstr 必须包含前导空格，其余部分必须产生
 * 会被 pg_strtok() 视为单个简单词元的内容。
 * convfunc 可以为空，或是转换宏或函数的名字。
 */
#define WRITE_SCALAR_ARRAY(fnname, datatype, fmtstr, convfunc) \
static void \
fnname(StringInfo str, const datatype *arr, int len) \
{ \
	if (arr != NULL) \
	{ \
		appendStringInfoChar(str, '('); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, fmtstr, convfunc(arr[i])); \
		appendStringInfoChar(str, ')'); \
	} \
	else \
		appendStringInfoString(str, "<>"); \
}

WRITE_SCALAR_ARRAY(writeAttrNumberCols, AttrNumber, " %d",)
WRITE_SCALAR_ARRAY(writeOidCols, Oid, " %u",)
WRITE_SCALAR_ARRAY(writeIndexCols, Index, " %u",)
WRITE_SCALAR_ARRAY(writeIntCols, int, " %d",)
WRITE_SCALAR_ARRAY(writeBoolCols, bool, " %s", booltostr)

/*
 * 打印 Node 指针的数组（不是 List）。
 *
 * 装饰与标量数组相同，但循环里无法
 * 直接使用 appendStringInfo()。
 */
static void
writeNodeArray(StringInfo str, const Node *const *arr, int len)
{
	if (arr != NULL)
	{
		appendStringInfoChar(str, '(');
		for (int i = 0; i < len; i++)
		{
			appendStringInfoChar(str, ' ');
			outNode(str, arr[i]);
		}
		appendStringInfoChar(str, ')');
	}
	else
		appendStringInfoString(str, "<>");
}

/*
 * 打印一个 List。
 */
static void
_outList(StringInfo str, const List *node)
{
	const ListCell *lc;

	appendStringInfoChar(str, '(');

	if (IsA(node, IntList))
		appendStringInfoChar(str, 'i');
	else if (IsA(node, OidList))
		appendStringInfoChar(str, 'o');
	else if (IsA(node, XidList))
		appendStringInfoChar(str, 'x');

	foreach(lc, node)
	{
		/*
		 * 为了向后兼容，我们对节点列表与其他类型的
		 * 列表输出略有不同的空白格式。
		 * XXX：这有必要吗？
		 */
		if (IsA(node, List))
		{
			outNode(str, lfirst(lc));
			if (lnext(node, lc))
				appendStringInfoChar(str, ' ');
		}
		else if (IsA(node, IntList))
			appendStringInfo(str, " %d", lfirst_int(lc));
		else if (IsA(node, OidList))
			appendStringInfo(str, " %u", lfirst_oid(lc));
		else if (IsA(node, XidList))
			appendStringInfo(str, " %u", lfirst_xid(lc));
		else
			elog(ERROR, "unrecognized list node type: %d",
				 (int) node->type);
	}

	appendStringInfoChar(str, ')');
}

/*
 * outBitmapset -
 *	   把整数位图集转换成字符串
 *
 * 注意：输出格式是 "(b int int ...)"，类似于整数 List。
 *
 * 我们导出此函数，供定义可扩展节点的扩展使用。
 * 不过这多少是历史原因，因为调用 outNode() 也能工作。
 */
void
outBitmapset(StringInfo str, const Bitmapset *bms)
{
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	x = -1;
	while ((x = bms_next_member(bms, x)) >= 0)
		appendStringInfo(str, " %d", x);
	appendStringInfoChar(str, ')');
}

/*
 * 打印给定类型的 Datum 值。
 */
void
outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, "%u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfoChar(str, ']');
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfoString(str, "0 [ ]");
		else
		{
			appendStringInfo(str, "%u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfoChar(str, ']');
		}
	}
}


#include "outfuncs.funcs.c"


/*
 * 带 custom_read_write 属性或
 * special_read_write 属性的节点的支持函数
 */

static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* 自行处理的枚举表示 */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* 为紧凑起见，只打印每列的匹配数： */
	appendStringInfoString(str, " :eclass");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * 为简化读取，我们只向上追到最顶层合并后的 EC，并
	 * 打印它，而不再单独显示被合并项。
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* 序列化私有字段 */
	methods->nodeOut(str, node);
}

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RANGETBLENTRY");

	/* 把 alias 和 eref 放前面，使转储更易读 */
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_NODE_FIELD(tablesample);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			/* 我们也复用这些 RELATION 字段： */
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			/* 我们也复用这些 RELATION 字段： */
			WRITE_OID_FIELD(relid);
			break;
		case RTE_RESULT:
			/* 没有额外字段 */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inh);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_NODE_FIELD(securityQuals);
}

static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("A_EXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoString(str, " ANY");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ALL:
			appendStringInfoString(str, " ALL");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		default:
			elog(ERROR, "unrecognized A_Expr_Kind: %d", (int) node->kind);
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outInteger(StringInfo str, const Integer *node)
{
	appendStringInfo(str, "%d", node->ival);
}

static void
_outFloat(StringInfo str, const Float *node)
{
	/*
	 * 我们假定该值是有效的数值字面量，因此不需要
	 * 加引号。
	 */
	appendStringInfoString(str, node->fval);
}

static void
_outBoolean(StringInfo str, const Boolean *node)
{
	appendStringInfoString(str, node->boolval ? "true" : "false");
}

static void
_outString(StringInfo str, const String *node)
{
	/*
	 * 我们用 outToken 转义字符串内容，但不希望
	 * 它把空字符串转换成 '""'，因为我们
	 * 已经在字符串外面加了双引号。
	 */
	appendStringInfoChar(str, '"');
	if (node->sval[0] != '\0')
		outToken(str, node->sval);
	appendStringInfoChar(str, '"');
}

static void
_outBitString(StringInfo str, const BitString *node)
{
	/*
	 * 词法分析器总是产生以 'b' 或 'x' 开头的字符串。其后
	 * 也许有需要转义的字符，但 outToken
	 * 不会转义 'b' 或 'x'。nodeTokenType 依赖这一点。
	 */
	Assert(node->bsval[0] == 'b' || node->bsval[0] == 'x');
	outToken(str, node->bsval);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	if (node->isnull)
		appendStringInfoString(str, " NULL");
	else
	{
		appendStringInfoString(str, " :val ");
		outNode(str, &node->val);
	}
	WRITE_LOCATION_FIELD(location);
}

static void
_outConstraint(StringInfo str, const Constraint *node)
{
	WRITE_NODE_TYPE("CONSTRAINT");

	WRITE_STRING_FIELD(conname);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :contype ");
	switch (node->contype)
	{
		case CONSTR_NULL:
			appendStringInfoString(str, "NULL");
			break;

		case CONSTR_NOTNULL:
			appendStringInfoString(str, "NOT_NULL");
			break;

		case CONSTR_DEFAULT:
			appendStringInfoString(str, "DEFAULT");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_IDENTITY:
			appendStringInfoString(str, "IDENTITY");
			WRITE_NODE_FIELD(options);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_GENERATED:
			appendStringInfoString(str, "GENERATED");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_CHECK:
			appendStringInfoString(str, "CHECK");
			WRITE_BOOL_FIELD(is_no_inherit);
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_PRIMARY:
			appendStringInfoString(str, "PRIMARY_KEY");
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method 和 where_clause 目前未使用 */
			break;

		case CONSTR_UNIQUE:
			appendStringInfoString(str, "UNIQUE");
			WRITE_BOOL_FIELD(nulls_not_distinct);
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method 和 where_clause 目前未使用 */
			break;

		case CONSTR_EXCLUSION:
			appendStringInfoString(str, "EXCLUSION");
			WRITE_NODE_FIELD(exclusions);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			WRITE_STRING_FIELD(access_method);
			WRITE_NODE_FIELD(where_clause);
			break;

		case CONSTR_FOREIGN:
			appendStringInfoString(str, "FOREIGN_KEY");
			WRITE_NODE_FIELD(pktable);
			WRITE_NODE_FIELD(fk_attrs);
			WRITE_NODE_FIELD(pk_attrs);
			WRITE_CHAR_FIELD(fk_matchtype);
			WRITE_CHAR_FIELD(fk_upd_action);
			WRITE_CHAR_FIELD(fk_del_action);
			WRITE_NODE_FIELD(fk_del_set_cols);
			WRITE_NODE_FIELD(old_conpfeqop);
			WRITE_OID_FIELD(old_pktable_oid);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_ATTR_DEFERRABLE:
			appendStringInfoString(str, "ATTR_DEFERRABLE");
			break;

		case CONSTR_ATTR_NOT_DEFERRABLE:
			appendStringInfoString(str, "ATTR_NOT_DEFERRABLE");
			break;

		case CONSTR_ATTR_DEFERRED:
			appendStringInfoString(str, "ATTR_DEFERRED");
			break;

		case CONSTR_ATTR_IMMEDIATE:
			appendStringInfoString(str, "ATTR_IMMEDIATE");
			break;

		default:
			elog(ERROR, "unrecognized ConstrType: %d", (int) node->contype);
			break;
	}
}


/*
 * outNode -
 *	  把 Node 转换成 ascii 字符串并追加到 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	if (obj == NULL)
		appendStringInfoString(str, "<>");
	else if (IsA(obj, List) || IsA(obj, IntList) || IsA(obj, OidList) ||
			 IsA(obj, XidList))
		_outList(str, obj);
	/* nodeRead 不希望看到这些外面的 { }！ */
	else if (IsA(obj, Integer))
		_outInteger(str, (Integer *) obj);
	else if (IsA(obj, Float))
		_outFloat(str, (Float *) obj);
	else if (IsA(obj, Boolean))
		_outBoolean(str, (Boolean *) obj);
	else if (IsA(obj, String))
		_outString(str, (String *) obj);
	else if (IsA(obj, BitString))
		_outBitString(str, (BitString *) obj);
	else if (IsA(obj, Bitmapset))
		outBitmapset(str, (Bitmapset *) obj);
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
#include "outfuncs.switch.c"

			default:

				/*
				 * 这本应是 ERROR，但能够转储
				 * outNode 只部分理解的结构太有用了。
				 */
				elog(WARNING, "could not dump unrecognized node type: %d",
					 (int) nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}

/*
 * nodeToString -
 *	   把 Node 的 ascii 表示作为 palloc 出来的字符串返回
 */
char *
nodeToString(const void *obj)
{
	StringInfoData str;

	/* 见 stringinfo.h 中对这一手法的说明 */
	initStringInfo(&str);
	outNode(&str, obj);
	return str.data;
}

/*
 * bmsToString -
 *	   把 Bitmapset 的 ascii 表示作为 palloc 出来的字符串返回
 */
char *
bmsToString(const Bitmapset *bms)
{
	StringInfoData str;

	/* 见 stringinfo.h 中对这一手法的说明 */
	initStringInfo(&str);
	outBitmapset(&str, bms);
	return str.data;
}
