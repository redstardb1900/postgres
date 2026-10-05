/*-------------------------------------------------------------------------
 *
 * equalfuncs.c
 *	  用来比较节点树是否相等的函数。
 *
 * 注意：有意不比较解析位置字段（对含有该字段的
 * 节点而言）。这是因为我们希望，例如变量
 * "x" 与查询中另一处对 "x" 的引用被 equal() 视为相等。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/equalfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/datum.h"


/*
 * 简化各类字段比较的宏。尽可能使用它们，
 * 以减少低级笔误的机会。注意，这些宏把
 * Equal 例程中局部变量名为 'a' 和 'b'
 * 这一约定写死了。
 */

/* 比较简单标量字段（int、float、bool、enum 等） */
#define COMPARE_SCALAR_FIELD(fldname) \
	do { \
		if (a->fldname != b->fldname) \
			return false; \
	} while (0)

/* 比较指向某种 Node 或 Node 树的字段 */
#define COMPARE_NODE_FIELD(fldname) \
	do { \
		if (!equal(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* 比较指向 Bitmapset 的字段 */
#define COMPARE_BITMAPSET_FIELD(fldname) \
	do { \
		if (!bms_equal(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* 比较指向 C 字符串的字段，或者也许是 NULL */
#define COMPARE_STRING_FIELD(fldname) \
	do { \
		if (!equalstr(a->fldname, b->fldname)) \
			return false; \
	} while (0)

/* 比较可能为 NULL 的字符串字段的宏 */
#define equalstr(a, b)	\
	(((a) != NULL && (b) != NULL) ? (strcmp(a, b) == 0) : (a) == (b))

/* 比较内联数组字段 */
#define COMPARE_ARRAY_FIELD(fldname) \
	do { \
		if (memcmp(a->fldname, b->fldname, sizeof(a->fldname)) != 0) \
			return false; \
	} while (0)

/* 比较指向大小为 sz 的、简单 palloc 对象的字段 */
#define COMPARE_POINTER_FIELD(fldname, sz) \
	do { \
		if (memcmp(a->fldname, b->fldname, (sz)) != 0) \
			return false; \
	} while (0)

/* 比较解析位置字段（这是空操作，见上面的说明） */
#define COMPARE_LOCATION_FIELD(fldname) \
	((void) 0)

/* 比较 CoercionForm 字段（同样是空操作，见 primnodes.h 中的注释） */
#define COMPARE_COERCIONFORM_FIELD(fldname) \
	((void) 0)


#include "equalfuncs.funcs.c"


/*
 * 带 custom_copy_equal 属性的节点的支持函数
 */

static bool
_equalConst(const Const *a, const Const *b)
{
	COMPARE_SCALAR_FIELD(consttype);
	COMPARE_SCALAR_FIELD(consttypmod);
	COMPARE_SCALAR_FIELD(constcollid);
	COMPARE_SCALAR_FIELD(constlen);
	COMPARE_SCALAR_FIELD(constisnull);
	COMPARE_SCALAR_FIELD(constbyval);
	COMPARE_LOCATION_FIELD(location);

	/*
	 * 我们把同一类型的所有 NULL 常量视为相等。将来也许
	 * 需要改变？但 datumIsEqual 对 null 不起作用，所以……
	 */
	if (a->constisnull)
		return true;
	return datumIsEqual(a->constvalue, b->constvalue,
						a->constbyval, a->constlen);
}

static bool
_equalExtensibleNode(const ExtensibleNode *a, const ExtensibleNode *b)
{
	const ExtensibleNodeMethods *methods;

	COMPARE_STRING_FIELD(extnodename);

	/* 至此，我们知道两个节点的 extnodename 相同。 */
	methods = GetExtensibleNodeMethods(a->extnodename, false);

	/* 比较私有字段 */
	if (!methods->nodeEqual(a, b))
		return false;

	return true;
}

static bool
_equalA_Const(const A_Const *a, const A_Const *b)
{
	COMPARE_SCALAR_FIELD(isnull);
	/* 针对内联 val 字段的权宜做法。若 isnull 为 true，val 也无效 */
	if (!a->isnull &&
		!equal(&a->val, &b->val))
		return false;
	COMPARE_LOCATION_FIELD(location);

	return true;
}

static bool
_equalBitmapset(const Bitmapset *a, const Bitmapset *b)
{
	return bms_equal(a, b);
}

/*
 * 列表被特殊处理
 */
static bool
_equalList(const List *a, const List *b)
{
	const ListCell *item_a;
	const ListCell *item_b;

	/*
	 * 在翻遍所有列表元素之前，先用简单的标量检查尝试排除
	 * ……
	 */
	COMPARE_SCALAR_FIELD(type);
	COMPARE_SCALAR_FIELD(length);

	/*
	 * 为了效率我们把 switch 放在循环外面；这
	 * 也许并不值得……
	 */
	switch (a->type)
	{
		case T_List:
			forboth(item_a, a, item_b, b)
			{
				if (!equal(lfirst(item_a), lfirst(item_b)))
					return false;
			}
			break;
		case T_IntList:
			forboth(item_a, a, item_b, b)
			{
				if (lfirst_int(item_a) != lfirst_int(item_b))
					return false;
			}
			break;
		case T_OidList:
			forboth(item_a, a, item_b, b)
			{
				if (lfirst_oid(item_a) != lfirst_oid(item_b))
					return false;
			}
			break;
		case T_XidList:
			forboth(item_a, a, item_b, b)
			{
				if (lfirst_xid(item_a) != lfirst_xid(item_b))
					return false;
			}
			break;
		default:
			elog(ERROR, "unrecognized list node type: %d",
				 (int) a->type);
			return false;		/* 消除编译器告警 */
	}

	/*
	 * 若到达这里，两个列表的元素都应当已经用完
	 */
	Assert(item_a == NULL);
	Assert(item_b == NULL);

	return true;
}


/*
 * equal
 *	  返回两个节点是否相等
 */
bool
equal(const void *a, const void *b)
{
	bool		retval;

	if (a == b)
		return true;

	/*
	 * 注意 a!=b，因此只有其中一个可以为 NULL
	 */
	if (a == NULL || b == NULL)
		return false;

	/*
	 * 它们是同一类型的节点吗？
	 */
	if (nodeTag(a) != nodeTag(b))
		return false;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	switch (nodeTag(a))
	{
#include "equalfuncs.switch.c"

		case T_List:
		case T_IntList:
		case T_OidList:
		case T_XidList:
			retval = _equalList(a, b);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(a));
			retval = false;		/* 消除编译器告警 */
			break;
	}

	return retval;
}
